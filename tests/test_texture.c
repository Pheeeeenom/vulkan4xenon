#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

enum {
	FRAMES = 60,
	TEXELS = 4,
	TEXEL_BYTES = 4,
	// The second image's rows are copied from a buffer with room for 8 texels a row.
	PADDED_ROW_TEXELS = 8,
	SECOND_IMAGE_OFFSET = 256,
	STAGING_BYTES = SECOND_IMAGE_OFFSET + PADDED_ROW_TEXELS * TEXELS * TEXEL_BYTES,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	FENCE_TIMEOUT_NS = 1000000000,
	// The quads span x from 64 and 704, y from 72, each 512 by 576 pixels: 128 by 144 a texel.
	LEFT_X = 64,
	RIGHT_X = 704,
	TOP_Y = 72,
	TEXEL_WIDTH = 128,
	TEXEL_HEIGHT = 144,
	PROBES = 2 * TEXELS * TEXELS + 1,
	TOLERANCE = 1,
};

// Clip space, then texture coordinates: the first image on the left, the second on the right.
static const float quads[2 * QUAD_VERTICES][VERTEX_FLOATS] = {
	{ -0.9f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ -0.1f, -0.8f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -0.9f, 0.8f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ -0.1f, -0.8f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -0.1f, 0.8f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -0.9f, 0.8f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.1f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 0.9f, -0.8f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 0.1f, 0.8f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.9f, -0.8f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 0.9f, 0.8f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ 0.1f, 0.8f, 0.0f, 1.0f, 0.0f, 1.0f },
};

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

// A dark grey: 24 in each channel.
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };

static const VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkImage images[2];
	VkDeviceMemory image_memory[2];
	VkImageView views[2];
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[2];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[PROBES];
	HarnessProbes check;
} Scene;

// Each texel a colour of its own; the second image's red and blue differ, so a swap of them shows.
static Rgb texel(uint32_t image, uint32_t x, uint32_t y)
{
	if (image == 0)
		return (Rgb){ 40 + x * 60, 40 + y * 60, 200 - (x + y) * 20 };
	return (Rgb){ 200 - x * 50, 50 + y * 50, 20 + (x + y) * 10 };
}

// The first image packed tight, the second in rows 8 texels apart, each texel's bytes in its format's order.
static void fill_staging(uint8_t *bytes)
{
	memset(bytes, 0, STAGING_BYTES);
	for (uint32_t y = 0; y < TEXELS; y++) {
		for (uint32_t x = 0; x < TEXELS; x++) {
			const Rgb first = texel(0, x, y);
			uint8_t *const a = bytes + (y * TEXELS + x) * TEXEL_BYTES;
			a[0] = (uint8_t)first.red, a[1] = (uint8_t)first.green, a[2] = (uint8_t)first.blue, a[3] = 0xFF;
			const Rgb second = texel(1, x, y);
			uint8_t *const b = bytes + SECOND_IMAGE_OFFSET + (y * PADDED_ROW_TEXELS + x) * TEXEL_BYTES;
			b[0] = (uint8_t)second.blue, b[1] = (uint8_t)second.green, b[2] = (uint8_t)second.red, b[3] = 0xFF;
		}
	}
}

static void make_probes(Scene *scene)
{
	uint32_t count = 0;
	for (uint32_t image = 0; image < 2; image++) {
		for (uint32_t y = 0; y < TEXELS; y++) {
			for (uint32_t x = 0; x < TEXELS; x++)
				scene->probes[count++] = (HarnessProbe){ image ? "B8G8R8A8 texel" : "R8G8B8A8 texel",
					(image ? RIGHT_X : LEFT_X) + x * TEXEL_WIDTH + TEXEL_WIDTH / 2,
					TOP_Y + y * TEXEL_HEIGHT + TEXEL_HEIGHT / 2, texel(image, x, y), 0 };
		}
	}
	scene->probes[count++] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->check = (HarnessProbes){ "texture", scene->probes, count, TOLERANCE, 0 };
}

static VkResult create_image(const Harness *harness, Scene *scene, uint32_t i)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = formats[i],
		.extent = { TEXELS, TEXELS, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, &scene->images[i]);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, scene->images[i], &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, &scene->image_memory[i]);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(harness->device, scene->images[i], scene->image_memory[i], 0);
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->images[i],
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = formats[i],
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, &scene->views[i]);
	return result;
}

static VkResult create_descriptors(const Harness *harness, Scene *scene)
{
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetLayout layouts[2] = { scene->set_layout, scene->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 2,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	if (result != VK_SUCCESS)
		return result;
	for (uint32_t i = 0; i < 2; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[i],
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
		result = harness_render_pass(harness, &scene->render_pass, scene->framebuffers);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline);
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	uint8_t staging[STAGING_BYTES];
	fill_staging(staging);
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	ok = ok && harness_ok("first image", create_image(harness, scene, 0));
	ok = ok && harness_ok("second image", create_image(harness, scene, 1));
	ok = ok && harness_ok("descriptors", create_descriptors(harness, scene));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quads, sizeof quads, &scene->vertices,
				&scene->vertex_memory));
	return ok &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
				&scene->staging_memory));
}

static void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask =
			to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
		NULL, 1, &barrier);
}

// One submission copies the staging buffer into both images, the second from its padded rows.
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
	for (uint32_t i = 0; i < 2; i++) {
		const VkBufferImageCopy region = {
			.bufferOffset = i ? SECOND_IMAGE_OFFSET : 0,
			.bufferRowLength = i ? PADDED_ROW_TEXELS : 0,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { TEXELS, TEXELS, 1 },
		};
		transition(commands, scene->images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition(
			commands, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
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

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t image = 0; image < 2; image++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[image], 0, NULL);
			vkCmdDraw(commands, QUAD_VERTICES, 1, image * QUAD_VERTICES, 0);
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	for (uint32_t i = 0; i < 2; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
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
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_texture(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("texture: two images, a sampler, descriptor sets and a pipeline made", built);
	const bool uploaded = built && upload(harness, &scene);
	check("texture: a buffer copied into both images", uploaded);
	const bool recorded = uploaded && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("texture: 60 frames drawn and presented", frames == FRAMES);
	check("texture: every frame shows each texel, R8G8B8A8 on the left and B8G8R8A8 on the right",
		frames == FRAMES && misses == 0);
	check("texture: the image on screen shows them too", harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
