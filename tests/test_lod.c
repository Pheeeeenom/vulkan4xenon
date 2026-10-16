#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Samplers' LOD bias and clamps, and an image of one level, on images whose levels are each a colour of their own:
// each cell draws the 256 by 256 image at 64 by 64 pixels, so at LOD 2, and shows the level its sampler takes there
// (milestone 18, step 2).

enum {
	FRAMES = 3,
	TEXTURE_SIZE = 256,
	LEVELS = 9,
	TEXEL_BYTES = 4,
	MIPPED_BYTES = TEXEL_BYTES * (TEXTURE_SIZE * TEXTURE_SIZE * 4 - 1) / 3,
	SINGLE_OFFSET = MIPPED_BYTES,
	STAGING_BYTES = MIPPED_BYTES + TEXTURE_SIZE * TEXTURE_SIZE * TEXEL_BYTES,
	IMAGES = 2,
	CELL_SIZE = 64,
	CELL_STEP = 136,
	CELL_LEFT = 40,
	CELL_TOP = 328,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	// Each colour far from the others; linear mips blend two, which rounds once.
	TOLERANCE = 3,
};

typedef struct {
	const char *name;
	bool single;
	float bias;
	float min_lod;
	float max_lod;
	bool linear_mips;
	// The level shown, or LEVELS for the halfway blend of levels 2 and 3.
	uint32_t level;
} Cell;

// LOD 2 as drawn.
static const Cell cells[] = {
	{ "as drawn", false, 0.0f, 0.0f, VK_LOD_CLAMP_NONE, false, 2 },
	{ "biased up a level", false, 1.0f, 0.0f, VK_LOD_CLAMP_NONE, false, 3 },
	{ "biased down a level", false, -1.0f, 0.0f, VK_LOD_CLAMP_NONE, false, 1 },
	{ "biased half a level, linear mips", false, 0.5f, 0.0f, VK_LOD_CLAMP_NONE, true, LEVELS },
	{ "kept to level 4 and up", false, 0.0f, 4.0f, VK_LOD_CLAMP_NONE, false, 4 },
	{ "kept to levels 0 and 1", false, 0.0f, 0.0f, 1.0f, false, 1 },
	{ "kept to level 0", false, 0.0f, 0.0f, 0.0f, false, 0 },
	{ "kept to level 2, biased up 3", false, 3.0f, 2.0f, 2.0f, false, 2 },
	{ "one level, biased up 3", true, 3.0f, 0.0f, VK_LOD_CLAMP_NONE, false, 0 },
};

enum { CELLS = sizeof cells / sizeof cells[0] };

static const Rgb level_colours[LEVELS] = { { 230, 40, 40 }, { 40, 200, 40 }, { 40, 80, 230 }, { 230, 210, 40 },
	{ 200, 40, 200 }, { 40, 200, 210 }, { 240, 240, 240 }, { 120, 120, 120 }, { 160, 90, 30 } };
static const Rgb single_colour = { 250, 150, 180 };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };

typedef struct {
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipeline;
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView views[IMAGES];
	VkSampler samplers[CELLS];
	VkDescriptorPool pool;
	VkDescriptorSet sets[CELLS];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[CELLS + 1];
	HarnessProbes check;
} Scene;

static uint32_t level_offset(uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; l++)
		offset += (TEXTURE_SIZE >> l) * (TEXTURE_SIZE >> l) * TEXEL_BYTES;
	return offset;
}

static void fill(uint8_t *texels, uint32_t count, Rgb colour)
{
	for (uint32_t i = 0; i < count; i++) {
		uint8_t *const texel = texels + i * TEXEL_BYTES;
		texel[0] = (uint8_t)colour.red, texel[1] = (uint8_t)colour.green, texel[2] = (uint8_t)colour.blue;
		texel[3] = 0xFF;
	}
}

static void fill_staging(uint8_t *bytes)
{
	for (uint32_t level = 0; level < LEVELS; level++) {
		const uint32_t size = TEXTURE_SIZE >> level;
		fill(bytes + level_offset(level), size * size, level_colours[level]);
	}
	fill(bytes + SINGLE_OFFSET, TEXTURE_SIZE * TEXTURE_SIZE, single_colour);
}

static Rgb wanted(const Cell *cell)
{
	if (cell->single)
		return single_colour;
	if (cell->level < LEVELS)
		return level_colours[cell->level];
	const Rgb a = level_colours[2], b = level_colours[3];
	return (Rgb){ (a.red + b.red + 1) / 2, (a.green + b.green + 1) / 2, (a.blue + b.blue + 1) / 2 };
}

static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < CELLS; i++)
		scene->probes[i] = (HarnessProbe){ cells[i].name, CELL_LEFT + i * CELL_STEP + CELL_SIZE / 2,
			CELL_TOP + CELL_SIZE / 2, wanted(&cells[i]), 0 };
	scene->probes[CELLS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->check = (HarnessProbes){ "lod", scene->probes, CELLS + 1, TOLERANCE, 0 };
}

// A cell's two triangles in clip space, the whole image over it.
static void cell_quad(uint32_t cell, float vertices[QUAD_VERTICES][VERTEX_FLOATS])
{
	const float x = (float)(CELL_LEFT + cell * CELL_STEP);
	const float left = x / (HARNESS_WIDTH / 2) - 1.0f;
	const float right = (x + CELL_SIZE) / (HARNESS_WIDTH / 2) - 1.0f;
	const float top = (float)CELL_TOP / (HARNESS_HEIGHT / 2) - 1.0f;
	const float bottom = (float)(CELL_TOP + CELL_SIZE) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
	};
	memcpy(vertices, corners, sizeof corners);
}

static VkResult create_image(const Harness *harness, Scene *scene, uint32_t i, uint32_t levels)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { TEXTURE_SIZE, TEXTURE_SIZE, 1 },
		.mipLevels = levels,
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
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, &scene->views[i]);
	return result;
}

// A sampler for each cell, point filtered, its mips point or linear, with its bias and clamps.
static VkResult create_descriptors(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < CELLS && result == VK_SUCCESS; i++) {
		const VkSamplerCreateInfo sampler_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = cells[i].linear_mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.mipLodBias = cells[i].bias,
			.minLod = cells[i].min_lod,
			.maxLod = cells[i].max_lod,
		};
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->samplers[i]);
	}
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CELLS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = CELLS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[CELLS];
	for (uint32_t i = 0; i < CELLS; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = CELLS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < CELLS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->samplers[i], scene->views[cells[i].single ? 1 : 0],
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
	return result;
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

static void transition(VkCommandBuffer commands, VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to)
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
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

// One submission copies every level of both images in.
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
		const VkBufferImageCopy single = {
			.bufferOffset = SINGLE_OFFSET,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { TEXTURE_SIZE, TEXTURE_SIZE, 1 },
		};
		for (uint32_t i = 0; i < IMAGES; i++)
			transition(commands, scene->images[i], i ? 1 : LEVELS, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LEVELS, regions);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &single);
		for (uint32_t i = 0; i < IMAGES; i++)
			transition(commands, scene->images[i], i ? 1 : LEVELS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
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
	static uint8_t staging[STAGING_BYTES];
	static float vertices[CELLS][QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	for (uint32_t i = 0; i < CELLS; i++)
		cell_quad(i, vertices[i]);
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	ok = ok && harness_ok("mipped image", create_image(harness, scene, 0, LEVELS));
	ok = ok && harness_ok("single-level image", create_image(harness, scene, 1, 1));
	ok = ok && harness_ok("samplers and descriptors", create_descriptors(harness, scene));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
	return ok &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
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
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t i = 0; i < CELLS; i++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[i], 0, NULL);
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
	vkDestroyPipeline(device, scene->pipeline, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	for (uint32_t i = 0; i < CELLS; i++)
		vkDestroySampler(device, scene->samplers[i], NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
	}
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
}

void test_lod(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene) && upload(harness, &scene);
	check("lod: images with a colour a level, and samplers with LOD biases and clamps", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check(
		"lod: each cell shows the level its sampler's bias and clamps take at LOD 2, and one level for an image of one",
		frames == FRAMES && misses == 0);
	destroy(harness, &scene);
}
