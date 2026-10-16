#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// LODs and gradients a fragment shader works out, on an image whose levels are each a colour of their own: four strips
// of eight bands, the image drawn at LOD 2 in each band, every band showing the level its shader picks there
// (milestone 22).

enum {
	FRAMES = 3,
	TEXTURE_SIZE = 256,
	LEVELS = 9,
	TEXEL_BYTES = 4,
	MIPPED_BYTES = TEXEL_BYTES * (TEXTURE_SIZE * TEXTURE_SIZE * 4 - 1) / 3,
	// Each band shows the image once at 64 pixels a side, so at LOD 2.
	BANDS = 8,
	BAND_SIZE = 64,
	STRIP_LEFT = (HARNESS_WIDTH - BANDS * BAND_SIZE) / 2,
	STRIP_TOP = 140,
	STRIP_STEP = 120,
	STRIPS = 4,
	// The constant strip's left half picks level 1, its right half level 6.
	CONSTANT_STRIP = 3,
	CONSTANT_LEFT_LEVEL = 1,
	CONSTANT_RIGHT_LEVEL = 6,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	BAND_OFFSET_BYTES = 24,
	QUAD_VERTICES = 6,
	// A strip's bands, and the clear colour with the first.
	MAX_STRIP_PROBES = BANDS + 1,
	// Each colour far from the others, every sample a texel of one level.
	TOLERANCE = 3,
};

typedef struct {
	const char *name;
	const uint32_t *spirv;
	uint32_t spirv_bytes;
	// The check's log line.
	const char *passes;
} Strip;

// Top to bottom: a LOD, gradients and a bias each band works out, then two constant LODs.
static const Strip strips[STRIPS] = {
	{ "shader_lod, a LOD per pixel", lod_computed_frag_spirv, LOD_COMPUTED_FRAG_SPIRV_BYTES,
		"shader_lod: textureLod at a LOD each pixel works out shows its band's level in all eight bands" },
	{ "shader_lod, gradients", lod_gradients_frag_spirv, LOD_GRADIENTS_FRAG_SPIRV_BYTES,
		"shader_lod: textureGrad with gradients each pixel works out shows its band's level in all eight bands" },
	{ "shader_lod, a bias per pixel", lod_bias_frag_spirv, LOD_BIAS_FRAG_SPIRV_BYTES,
		"shader_lod: texture() biased from LOD 2 by an amount each pixel works out shows its band's level" },
	{ "shader_lod, constant LODs", lod_constant_frag_spirv, LOD_CONSTANT_FRAG_SPIRV_BYTES,
		"shader_lod: textureLod at 1.25 and 6.25 shows levels 1 and 6" },
};

static const char *const band_names[BANDS] = { "level 0", "level 1", "level 2", "level 3", "level 4", "level 5",
	"level 6", "level 7" };

static const Rgb level_colours[LEVELS] = { { 230, 40, 40 }, { 40, 200, 40 }, { 40, 80, 230 }, { 230, 210, 40 },
	{ 200, 40, 200 }, { 40, 200, 210 }, { 240, 240, 240 }, { 120, 120, 120 }, { 160, 90, 30 } };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
	{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = BAND_OFFSET_BYTES },
};
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };

typedef struct {
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipelines[STRIPS];
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[STRIPS][MAX_STRIP_PROBES];
	HarnessProbes checks[STRIPS];
	uint32_t misses[STRIPS];
} Scene;

static uint32_t level_offset(uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; l++)
		offset += (TEXTURE_SIZE >> l) * (TEXTURE_SIZE >> l) * TEXEL_BYTES;
	return offset;
}

static void fill_staging(uint8_t *bytes)
{
	for (uint32_t level = 0; level < LEVELS; level++) {
		const uint32_t size = TEXTURE_SIZE >> level;
		uint8_t *const texels = bytes + level_offset(level);
		for (uint32_t i = 0; i < size * size; i++) {
			const Rgb colour = level_colours[level];
			texels[i * TEXEL_BYTES] = (uint8_t)colour.red, texels[i * TEXEL_BYTES + 1] = (uint8_t)colour.green;
			texels[i * TEXEL_BYTES + 2] = (uint8_t)colour.blue, texels[i * TEXEL_BYTES + 3] = 0xFF;
		}
	}
}

static uint32_t band_centre_x(uint32_t band)
{
	return STRIP_LEFT + band * BAND_SIZE + BAND_SIZE / 2;
}

static uint32_t strip_centre_y(uint32_t strip)
{
	return STRIP_TOP + strip * STRIP_STEP + BAND_SIZE / 2;
}

// Each band of the first three strips shows its own level; the constant strip a band from each half.
static void make_probes(Scene *scene)
{
	for (uint32_t strip = 0; strip < CONSTANT_STRIP; strip++) {
		for (uint32_t band = 0; band < BANDS; band++)
			scene->probes[strip][band] =
				(HarnessProbe){ band_names[band], band_centre_x(band), strip_centre_y(strip), level_colours[band], 0 };
		scene->checks[strip] = (HarnessProbes){ strips[strip].name, scene->probes[strip], BANDS, TOLERANCE, 0 };
	}
	scene->probes[0][BANDS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->checks[0].count = BANDS + 1;
	HarnessProbe *const constant = scene->probes[CONSTANT_STRIP];
	constant[0] = (HarnessProbe){ "LOD 1.25", band_centre_x(1), strip_centre_y(CONSTANT_STRIP),
		level_colours[CONSTANT_LEFT_LEVEL], 0 };
	constant[1] = (HarnessProbe){ "LOD 6.25", band_centre_x(6), strip_centre_y(CONSTANT_STRIP),
		level_colours[CONSTANT_RIGHT_LEVEL], 0 };
	scene->checks[CONSTANT_STRIP] = (HarnessProbes){ strips[CONSTANT_STRIP].name, constant, 2, TOLERANCE, 0 };
}

// Every strip's probes, each strip's misses counted apart.
static bool strips_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t i = 0; i < STRIPS; i++) {
		if (!harness_probes_hold(address, what, &scene->checks[i])) {
			scene->misses[i]++;
			all = false;
		}
	}
	return all;
}

// A strip's two triangles in clip space: the image repeated once a band, and the band number from 0 to BANDS.
static void strip_quad(uint32_t strip, float vertices[QUAD_VERTICES][VERTEX_FLOATS])
{
	const float left = (float)STRIP_LEFT / (HARNESS_WIDTH / 2) - 1.0f;
	const float right = (float)(STRIP_LEFT + BANDS * BAND_SIZE) / (HARNESS_WIDTH / 2) - 1.0f;
	const uint32_t y = STRIP_TOP + strip * STRIP_STEP;
	const float top = (float)y / (HARNESS_HEIGHT / 2) - 1.0f;
	const float bottom = (float)(y + BAND_SIZE) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, BANDS, 0.0f, BANDS, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, BANDS, 0.0f, BANDS, 0.0f },
		{ right, bottom, 0.0f, 1.0f, BANDS, 1.0f, BANDS, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	};
	memcpy(vertices, corners, sizeof corners);
}

static VkResult create_image(const Harness *harness, Scene *scene)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { TEXTURE_SIZE, TEXTURE_SIZE, 1 },
		.mipLevels = LEVELS,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, &scene->image);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, scene->image, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, &scene->image_memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(harness->device, scene->image, scene->image_memory, 0);
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, &scene->view);
	return result;
}

// Point sampled, its mips point too, so each band shows one level's colour; repeated, as each band shows the image.
static VkResult create_descriptors(const Harness *harness, Scene *scene)
{
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.maxLod = VK_LOD_CLAMP_NONE,
	};
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, &scene->set);
	if (result != VK_SUCCESS)
		return result;
	const VkDescriptorImageInfo image = { scene->sampler, scene->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return VK_SUCCESS;
}

static VkResult create_pipelines(const Harness *harness, Scene *scene)
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
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->render_pass, scene->framebuffers);
	for (uint32_t i = 0; i < STRIPS && result == VK_SUCCESS; i++) {
		const HarnessPipeline spec = {
			.vertex_spirv = lod_bands_vert_spirv,
			.vertex_bytes = LOD_BANDS_VERT_SPIRV_BYTES,
			.fragment_spirv = strips[i].spirv,
			.fragment_bytes = strips[i].spirv_bytes,
			.bindings = &binding,
			.binding_count = 1,
			.attributes = attributes,
			.attribute_count = sizeof attributes / sizeof attributes[0],
			.layout = scene->layout,
		};
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[i]);
		if (result != VK_SUCCESS)
			xsdk_log_write("shader_lod: the pipeline for %s: %d", strips[i].name, (int)result);
	}
	return result;
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
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

// One submission copies every level in.
static bool upload(const Harness *harness, const Scene *scene)
{
	VkCommandBuffer commands;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!harness_ok("upload commands", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	if (ok) {
		VkBufferImageCopy regions[LEVELS];
		for (uint32_t level = 0; level < LEVELS; level++)
			regions[level] = (VkBufferImageCopy){
				.bufferOffset = level_offset(level),
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
				.imageExtent = { TEXTURE_SIZE >> level, TEXTURE_SIZE >> level, 1 },
			};
		transition(commands, scene->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LEVELS, regions);
		transition(
			commands, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ok = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	}
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	ok = ok && harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, UINT64_MAX));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static bool build(const Harness *harness, Scene *scene)
{
	static uint8_t staging[MIPPED_BYTES];
	static float vertices[STRIPS][QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	for (uint32_t i = 0; i < STRIPS; i++)
		strip_quad(i, vertices[i]);
	bool ok = harness_ok("pipelines", create_pipelines(harness, scene));
	ok = ok && harness_ok("mipped image", create_image(harness, scene));
	ok = ok && harness_ok("sampler and descriptor", create_descriptors(harness, scene));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
	return ok &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, MIPPED_BYTES, &scene->staging,
				&scene->staging_memory));
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t frame = 0; result == VK_SUCCESS && frame < harness->image_count; frame++) {
		const VkCommandBuffer commands = scene->commands[frame];
		const VkRenderPassBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[frame],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->set, 0, NULL);
		for (uint32_t i = 0; i < STRIPS; i++) {
			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[i]);
			vkCmdDraw(commands, QUAD_VERTICES, 1, i * QUAD_VERTICES, 0);
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
	for (uint32_t i = 0; i < STRIPS; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
}

void test_shader_lod(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene) && upload(harness, &scene);
	check("shader_lod: pipelines sampling at LODs, gradients and biases their shaders work out", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, strips_hold, &scene, &misses) : 0;
	for (uint32_t i = 0; i < STRIPS; i++)
		check(strips[i].passes, frames == FRAMES && scene.misses[i] == 0);
	destroy(harness, &scene);
}
