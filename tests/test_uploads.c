#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 12's uploads: a staging buffer in the cached memory type, R8 and R8G8 images through swizzled views, R8
// filtered linearly, and two copies into one image around a draw in one command buffer.

enum {
	FRAMES = 60,
	TEXELS = 4,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	FENCE_TIMEOUT_NS = 1000000000,
	// Where each image's texels sit in the staging buffer: the R8G8's 2 bytes a texel, the copied image's two contents
	// 4, each at a multiple of 256.
	R8_OFFSET = 0,
	R8G8_OFFSET = 256,
	FILTERED_OFFSET = 512,
	COPIED_FIRST_OFFSET = 768,
	COPIED_SECOND_OFFSET = 1024,
	STAGING_BYTES = 1280,
	// The quads, in pixels: the top row 320 by 320, 80 a texel, the filtered one 320 by 80, the bottom row 320 by 192.
	TOP_Y = 72,
	BOTTOM_Y = 456,
	QUAD_SIZE = 320,
	FILTERED_HEIGHT = 80,
	BOTTOM_HEIGHT = 192,
	COLUMN_0 = 64,
	COLUMN_1 = 448,
	COLUMN_2 = 832,
	FILTERED_TEXELS = 2,
	FILTERED_HIGH = 200,
	TOLERANCE = 3,
	MAX_PROBES = 2 * TEXELS * TEXELS,
};

// The images, as listed below.
enum {
	IMAGE_R8,
	IMAGE_R8G8,
	IMAGE_FILTERED,
	IMAGE_COPIED,
	IMAGES,
	// The copied image is drawn twice: in the first pass, then in the second.
	QUADS = IMAGES + 1,
};

// What the checks count apart.
enum {
	GROUP_SWIZZLED,
	GROUP_FILTERED,
	GROUP_COPIED,
	GROUPS,
};

typedef struct {
	VkFormat format;
	uint32_t width;
	uint32_t height;
	VkComponentMapping components;
} ImageSpec;

static const ImageSpec images[IMAGES] = {
	[IMAGE_R8] = { VK_FORMAT_R8_UNORM, TEXELS, TEXELS,
		{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R } },
	[IMAGE_R8G8] = { VK_FORMAT_R8G8_UNORM, TEXELS, TEXELS,
		{ VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO } },
	[IMAGE_FILTERED] = { VK_FORMAT_R8_UNORM, FILTERED_TEXELS, 1,
		{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE } },
	[IMAGE_COPIED] = { VK_FORMAT_R8G8B8A8_UNORM, TEXELS, TEXELS, { 0 } },
};

// Each quad's place in pixels and the image it samples.
typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t image;
} Quad;

static const Quad quads[QUADS] = {
	{ COLUMN_0, TOP_Y, QUAD_SIZE, QUAD_SIZE, IMAGE_R8 },
	{ COLUMN_1, TOP_Y, QUAD_SIZE, QUAD_SIZE, IMAGE_R8G8 },
	{ COLUMN_2, TOP_Y, QUAD_SIZE, FILTERED_HEIGHT, IMAGE_FILTERED },
	{ COLUMN_0, BOTTOM_Y, QUAD_SIZE, BOTTOM_HEIGHT, IMAGE_COPIED },
	{ COLUMN_1, BOTTOM_Y, QUAD_SIZE, BOTTOM_HEIGHT, IMAGE_COPIED },
};

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };

typedef struct {
	VkRenderPass clear_pass;
	VkRenderPass load_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint32_t staging_type;
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView plain_views[IMAGES];
	VkImageView views[IMAGES];
	VkSampler nearest;
	VkSampler linear;
	VkDescriptorPool pool;
	VkDescriptorSet sets[IMAGES];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[GROUPS][MAX_PROBES];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

// The R8 image's texels, 30 to 225; the R8G8's red climbs and green falls; the copied image's two contents differ in
// every channel.
static uint8_t r8_texel(uint32_t x, uint32_t y)
{
	return (uint8_t)(30 + 13 * (y * TEXELS + x));
}

static uint8_t rg_red(uint32_t x, uint32_t y)
{
	return (uint8_t)(40 + 12 * (y * TEXELS + x));
}

static uint8_t rg_green(uint32_t x, uint32_t y)
{
	return (uint8_t)(220 - 11 * (y * TEXELS + x));
}

static Rgb copied_texel(uint32_t content, uint32_t x, uint32_t y)
{
	if (content == 0)
		return (Rgb){ 200 - x * 40, 40 + y * 40, 60 + (x + y) * 10 };
	return (Rgb){ 30 + x * 50, 220 - y * 50, 180 - (x + y) * 20 };
}

static void fill_staging(uint8_t *bytes)
{
	memset(bytes, 0, STAGING_BYTES);
	for (uint32_t y = 0; y < TEXELS; y++) {
		for (uint32_t x = 0; x < TEXELS; x++) {
			const uint32_t i = y * TEXELS + x;
			bytes[R8_OFFSET + i] = r8_texel(x, y);
			bytes[R8G8_OFFSET + 2 * i] = rg_red(x, y);
			bytes[R8G8_OFFSET + 2 * i + 1] = rg_green(x, y);
			for (uint32_t content = 0; content < 2; content++) {
				const Rgb texel = copied_texel(content, x, y);
				uint8_t *const out = bytes + (content ? COPIED_SECOND_OFFSET : COPIED_FIRST_OFFSET) + 4 * i;
				out[0] = (uint8_t)texel.red, out[1] = (uint8_t)texel.green, out[2] = (uint8_t)texel.blue, out[3] = 0xFF;
			}
		}
	}
	bytes[FILTERED_OFFSET] = 0;
	bytes[FILTERED_OFFSET + 1] = FILTERED_HIGH;
}

// Linear filtering between the two texels, at a pixel's centre k pixels into the filtered quad, clamped at the edges.
static uint32_t filtered_value(uint32_t k)
{
	float t = ((float)k + 0.5f) / QUAD_SIZE * FILTERED_TEXELS - 0.5f;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return (uint32_t)(t * FILTERED_HIGH + 0.5f);
}

static void make_probes(Scene *scene)
{
	uint32_t counts[GROUPS] = { 0 };
	for (uint32_t y = 0; y < TEXELS; y++) {
		for (uint32_t x = 0; x < TEXELS; x++) {
			const uint32_t px = x * QUAD_SIZE / TEXELS + QUAD_SIZE / TEXELS / 2;
			const uint32_t py = y * QUAD_SIZE / TEXELS + QUAD_SIZE / TEXELS / 2;
			const uint8_t r8 = r8_texel(x, y);
			scene->probes[GROUP_SWIZZLED][counts[GROUP_SWIZZLED]++] =
				(HarnessProbe){ "R8 as RRRR", COLUMN_0 + px, TOP_Y + py, { r8, r8, r8 }, 0 };
			scene->probes[GROUP_SWIZZLED][counts[GROUP_SWIZZLED]++] = (HarnessProbe){ "R8G8 as G, R, 1, 0",
				COLUMN_1 + px, TOP_Y + py, { rg_green(x, y), rg_red(x, y), 255 }, 0 };
			const uint32_t by = y * BOTTOM_HEIGHT / TEXELS + BOTTOM_HEIGHT / TEXELS / 2;
			for (uint32_t content = 0; content < 2; content++)
				scene->probes[GROUP_COPIED][counts[GROUP_COPIED]++] =
					(HarnessProbe){ content ? "the second copy's texel" : "the first copy's texel",
						(content ? COLUMN_1 : COLUMN_0) + px, BOTTOM_Y + by, copied_texel(content, x, y), 0 };
		}
	}
	// Either side of halfway, where nearest filtering gives 0 or 200 and linear about 100; and a quarter in from each
	// end, the texels' own values.
	static const uint32_t columns[] = { 79, 159, 160, 240 };
	for (uint32_t i = 0; i < sizeof columns / sizeof columns[0]; i++) {
		const uint32_t value = filtered_value(columns[i]);
		scene->probes[GROUP_FILTERED][counts[GROUP_FILTERED]++] = (HarnessProbe){ "R8 filtered linearly",
			COLUMN_2 + columns[i], TOP_Y + FILTERED_HEIGHT / 2, { value, value, value }, 0 };
	}
	static const char *const names[GROUPS] = { "uploads swizzled", "uploads filtered", "uploads copied" };
	for (uint32_t g = 0; g < GROUPS; g++)
		scene->checks[g] = (HarnessProbes){ names[g], scene->probes[g], counts[g], TOLERANCE, 0 };
}

// Every group's probes, counting each group's misses apart.
static bool groups_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t g = 0; g < GROUPS; g++) {
		if (!harness_probes_hold(address, what, &scene->checks[g])) {
			scene->misses[g]++;
			all = false;
		}
	}
	return all;
}

static float clip_x(uint32_t x)
{
	return (float)x / (HARNESS_WIDTH / 2) - 1.0f;
}

static float clip_y(uint32_t y)
{
	return (float)y / (HARNESS_HEIGHT / 2) - 1.0f;
}

static void make_vertices(float out[QUADS * QUAD_VERTICES][VERTEX_FLOATS])
{
	for (uint32_t q = 0; q < QUADS; q++) {
		const float left = clip_x(quads[q].x);
		const float right = clip_x(quads[q].x + quads[q].width);
		const float top = clip_y(quads[q].y);
		const float bottom = clip_y(quads[q].y + quads[q].height);
		const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
			{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
			{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
			{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
			{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
			{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
			{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		};
		memcpy(out[q * QUAD_VERTICES], corners, sizeof corners);
	}
}

// The staging buffer, in the cached memory type its requirements offer.
static VkResult create_staging(const Harness *harness, Scene *scene)
{
	uint8_t bytes[STAGING_BYTES];
	fill_staging(bytes);
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = STAGING_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkResult result = vkCreateBuffer(harness->device, &info, NULL, &scene->staging);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, scene->staging, &requirements);
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(harness->physical_device, &memory);
	scene->staging_type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		if ((requirements.memoryTypeBits & 1u << i) &&
			(memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
			scene->staging_type = i;
	if (scene->staging_type == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = scene->staging_type,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, &scene->staging_memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(harness->device, scene->staging, scene->staging_memory, 0);
	void *mapped = NULL;
	if (result == VK_SUCCESS)
		result = vkMapMemory(harness->device, scene->staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (result == VK_SUCCESS) {
		memcpy(mapped, bytes, STAGING_BYTES);
		vkUnmapMemory(harness->device, scene->staging_memory);
	}
	return result;
}

// Each image, a plain view from harness_image and the swizzled one the draws sample.
static VkResult create_images(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++) {
		result = harness_image(harness, images[i].format, images[i].width, images[i].height,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &scene->images[i], &scene->image_memory[i],
			&scene->plain_views[i]);
		const VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = scene->images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = images[i].format,
			.components = images[i].components,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		if (result == VK_SUCCESS)
			result = vkCreateImageView(harness->device, &view_info, NULL, &scene->views[i]);
	}
	return result;
}

static VkResult create_sampler(const Harness *harness, VkFilter filter, VkSampler *sampler)
{
	const VkSamplerCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = filter,
		.minFilter = filter,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	return vkCreateSampler(harness->device, &info, NULL, sampler);
}

static VkResult create_descriptors(const Harness *harness, Scene *scene)
{
	VkResult result = create_sampler(harness, VK_FILTER_NEAREST, &scene->nearest);
	if (result == VK_SUCCESS)
		result = create_sampler(harness, VK_FILTER_LINEAR, &scene->linear);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = IMAGES,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[IMAGES];
	for (uint32_t i = 0; i < IMAGES; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = IMAGES,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	if (result != VK_SUCCESS)
		return result;
	for (uint32_t i = 0; i < IMAGES; i++) {
		const VkDescriptorImageInfo image = { i == IMAGE_FILTERED ? scene->linear : scene->nearest, scene->views[i],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return VK_SUCCESS;
}

// The second pass loads what the first stored, through the same framebuffers.
static VkResult create_load_pass(const Harness *harness, VkRenderPass *render_pass)
{
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult create_pipeline(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding_info = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding_info,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout);
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->clear_pass, scene->framebuffers);
	if (result == VK_SUCCESS)
		result = create_load_pass(harness, &scene->load_pass);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->clear_pass, &scene->pipeline);
	return result;
}

static void copy(VkCommandBuffer commands, const Scene *scene, uint32_t image, uint32_t offset)
{
	const VkBufferImageCopy region = {
		.bufferOffset = offset,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { images[image].width, images[image].height, 1 },
	};
	vkCmdCopyBufferToImage(
		commands, scene->staging, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// One submission copies the R8, R8G8 and filtered images' texels in.
static bool upload(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("upload command buffer", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin);
	copy(commands, scene, IMAGE_R8, R8_OFFSET);
	copy(commands, scene, IMAGE_R8G8, R8G8_OFFSET);
	copy(commands, scene, IMAGE_FILTERED, FILTERED_OFFSET);
	bool ok = harness_ok("upload recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("upload submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok &&
		harness_ok("upload wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static void draw_quad(VkCommandBuffer commands, const Scene *scene, uint32_t quad)
{
	vkCmdBindDescriptorSets(
		commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[quads[quad].image], 0, NULL);
	vkCmdDraw(commands, QUAD_VERTICES, 1, quad * QUAD_VERTICES, 0);
}

// Each frame: the first contents copied in, a pass drawing every quad but the last, the second contents copied into
// the same image, and a pass drawing the last quad over what the first stored.
static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->commands[i];
		const VkDeviceSize offset = 0;
		const VkRenderPassBeginInfo first = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->clear_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		VkRenderPassBeginInfo second = first;
		second.renderPass = scene->load_pass;
		second.clearValueCount = 0;
		second.pClearValues = NULL;
		copy(commands, scene, IMAGE_COPIED, COPIED_FIRST_OFFSET);
		vkCmdBeginRenderPass(commands, &first, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t quad = 0; quad < QUADS - 1; quad++)
			draw_quad(commands, scene, quad);
		vkCmdEndRenderPass(commands);
		copy(commands, scene, IMAGE_COPIED, COPIED_SECOND_OFFSET);
		vkCmdBeginRenderPass(commands, &second, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		draw_quad(commands, scene, QUADS - 1);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	float vertices[QUADS * QUAD_VERTICES][VERTEX_FLOATS];
	make_vertices(vertices);
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	ok = ok && harness_ok("images", create_images(harness, scene));
	ok = ok && harness_ok("descriptors", create_descriptors(harness, scene));
	ok = ok && harness_ok("staging buffer", create_staging(harness, scene));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->nearest, NULL);
	vkDestroySampler(device, scene->linear, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
		vkDestroyImageView(device, scene->plain_views[i], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
	}
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	vkDestroyRenderPass(device, scene->load_pass, NULL);
	harness_destroy_render_pass(harness, scene->clear_pass, scene->framebuffers);
}

void test_uploads(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("uploads: four images, swizzled views, two samplers, a staging buffer in cached memory and two passes made",
		built);
	const bool uploaded = built && upload(harness, &scene);
	check("uploads: the R8, R8G8 and filtered images copied in from cached memory", uploaded);
	const bool recorded = uploaded && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("uploads: 60 frames drawn and presented", frames == FRAMES);
	check("uploads: every frame shows R8 as intensity and R8G8 as green, red and 1, through swizzled views",
		frames == FRAMES && scene.misses[GROUP_SWIZZLED] == 0);
	check("uploads: every frame shows R8 filtered linearly between its two texels",
		frames == FRAMES && scene.misses[GROUP_FILTERED] == 0);
	check("uploads: every frame shows each draw the texels copied in just before it, two copies into one image",
		frames == FRAMES && scene.misses[GROUP_COPIED] == 0);
	check("uploads: the image on screen shows them too", harness_screen(harness, groups_hold, &scene));
	destroy(harness, &scene);
}
