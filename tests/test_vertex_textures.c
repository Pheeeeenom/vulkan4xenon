#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Textures in a vertex shader beside one in the fragment shader (milestone 22): a row of columns each moved up by its
// own height, read from a height map in the vertex shader, and a row coloured by the level a vertex's textureLod picks.
// The fragment shader multiplies by a white texture of its own, so a stage reading the other's texture shows. Then the
// fetch constants' limit: 16 samplers in each stage are refused, 16 and 15 made.

enum {
	FRAMES = 3,
	COLUMNS = 8,
	COLUMN_LEFT = 160,
	COLUMN_STEP = 120,
	COLUMN_WIDTH = 80,
	COLUMN_HEIGHT = 40,
	// The moved row's columns stand on this row before they're moved up; the levels' row stays where it's drawn.
	MOVED_TOP = 400,
	LEVELS_TOP = 520,
	ROWS = 2,
	// Moved up by 48 pixels and 20 more each column, past the column's own height; a height of 1 is 255 pixels.
	FIRST_HEIGHT = 48,
	HEIGHT_STEP = 20,
	// The height map's texels: the moved row's heights, then the levels' row's zeros.
	HEIGHT_TEXELS = COLUMNS * ROWS,
	LEVEL_SIZE = 128,
	LEVELS = 8,
	TEXEL_BYTES = 4,
	LEVEL_BYTES = TEXEL_BYTES * (LEVEL_SIZE * LEVEL_SIZE * 4 - 1) / 3,
	HEIGHT_OFFSET = LEVEL_BYTES,
	TINT_OFFSET = HEIGHT_OFFSET + HEIGHT_TEXELS * TEXEL_BYTES,
	STAGING_BYTES = TINT_OFFSET + TEXEL_BYTES,
	IMAGES = 3,
	VERTEX_FLOATS = 12,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	UV_OFFSET_BYTES = 32,
	LEVEL_OFFSET_BYTES = 40,
	QUAD_VERTICES = 6,
	QUADS = COLUMNS * ROWS,
	// The fetch constants' check: 16 samplers in each stage, at bindings 0 to 15 and 16 to 31 of one set.
	MANY_BINDINGS = 32,
	TOLERANCE = 3,
	GROUPS = 2,
};

enum {
	IMAGE_LEVELS,
	IMAGE_HEIGHTS,
	IMAGE_TINT,
};

static const Rgb level_colours[LEVELS] = { { 230, 40, 40 }, { 40, 200, 40 }, { 40, 80, 230 }, { 230, 210, 40 },
	{ 200, 40, 200 }, { 40, 200, 210 }, { 240, 240, 240 }, { 120, 120, 120 } };
// The moved row's own colour.
static const Rgb moved_rgb = { 230, 140, 40 };
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };

static const char *const column_names[COLUMNS] = { "column 0", "column 1", "column 2", "column 3", "column 4",
	"column 5", "column 6", "column 7" };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = COLOR_OFFSET_BYTES },
	{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
	{ .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = LEVEL_OFFSET_BYTES },
};

typedef struct {
	const char *name;
	const char *passes;
} Group;

static const Group groups[GROUPS] = {
	{ "vertex textures, heights",
		"vertex textures: each column moved up by the height its vertex shader read, none left where it was drawn" },
	{ "vertex textures, levels",
		"vertex textures: each column the colour of the level its vertex shader's textureLod picked" },
};

typedef struct {
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipeline;
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView views[IMAGES];
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[GROUPS][2 * COLUMNS + 1];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

static uint32_t height(uint32_t column)
{
	return FIRST_HEIGHT + column * HEIGHT_STEP;
}

static uint32_t column_centre(uint32_t column)
{
	return COLUMN_LEFT + column * COLUMN_STEP + COLUMN_WIDTH / 2;
}

static uint32_t level_offset(uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; l++)
		offset += (LEVEL_SIZE >> l) * (LEVEL_SIZE >> l) * TEXEL_BYTES;
	return offset;
}

static void put_texel(uint8_t *texel, uint32_t red, uint32_t green, uint32_t blue)
{
	texel[0] = (uint8_t)red, texel[1] = (uint8_t)green, texel[2] = (uint8_t)blue, texel[3] = 0xFF;
}

// The levels a colour each, the heights in red, and the white tint.
static void fill_staging(uint8_t *bytes)
{
	for (uint32_t level = 0; level < LEVELS; level++) {
		const uint32_t size = LEVEL_SIZE >> level;
		for (uint32_t i = 0; i < size * size; i++)
			put_texel(bytes + level_offset(level) + i * TEXEL_BYTES, level_colours[level].red,
				level_colours[level].green, level_colours[level].blue);
	}
	for (uint32_t i = 0; i < HEIGHT_TEXELS; i++)
		put_texel(bytes + HEIGHT_OFFSET + i * TEXEL_BYTES, i < COLUMNS ? height(i) : 0, 0, 0);
	put_texel(bytes + TINT_OFFSET, 0xFF, 0xFF, 0xFF);
}

// The moved row's columns where they're moved to, and nothing where they're drawn; the levels' row's colours.
static void make_probes(Scene *scene)
{
	HarnessProbe *const moved = scene->probes[0];
	for (uint32_t c = 0; c < COLUMNS; c++) {
		const uint32_t y = MOVED_TOP + COLUMN_HEIGHT / 2;
		moved[2 * c] = (HarnessProbe){ column_names[c], column_centre(c), y - height(c), moved_rgb, 0 };
		moved[2 * c + 1] = (HarnessProbe){ "where it was drawn", column_centre(c), y, clear_rgb, 0 };
		scene->probes[1][c] =
			(HarnessProbe){ column_names[c], column_centre(c), LEVELS_TOP + COLUMN_HEIGHT / 2, level_colours[c], 0 };
	}
	moved[2 * COLUMNS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->checks[0] = (HarnessProbes){ groups[0].name, moved, 2 * COLUMNS + 1, TOLERANCE, 0 };
	scene->checks[1] = (HarnessProbes){ groups[1].name, scene->probes[1], COLUMNS, TOLERANCE, 0 };
}

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

// A column's two triangles: its height map texel, and for the levels' row the LOD a quarter past its level.
static void column_quad(uint32_t row, uint32_t column, float vertices[QUAD_VERTICES][VERTEX_FLOATS])
{
	const float x = (float)(COLUMN_LEFT + column * COLUMN_STEP);
	const float left = x / (HARNESS_WIDTH / 2) - 1.0f;
	const float right = (x + COLUMN_WIDTH) / (HARNESS_WIDTH / 2) - 1.0f;
	const float y = (float)(row ? LEVELS_TOP : MOVED_TOP);
	const float top = y / (HARNESS_HEIGHT / 2) - 1.0f;
	const float bottom = (y + COLUMN_HEIGHT) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float u = ((float)(row * COLUMNS + column) + 0.5f) / HEIGHT_TEXELS;
	const float lod = (float)column + 0.25f;
	const float levels = row ? 1.0f : 0.0f;
	const float r = moved_rgb.red / 255.0f, g = moved_rgb.green / 255.0f, b = moved_rgb.blue / 255.0f;
	const float corners[QUAD_VERTICES][2] = { { left, top }, { right, top }, { left, bottom }, { right, top },
		{ right, bottom }, { left, bottom } };
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float vertex[VERTEX_FLOATS] = { corners[v][0], corners[v][1], 0.0f, 1.0f, r, g, b, 1.0f, u, 0.5f, lod,
			levels };
		memcpy(vertices[v], vertex, sizeof vertex);
	}
}

static VkResult create_image(
	const Harness *harness, Scene *scene, uint32_t i, uint32_t width, uint32_t height_texels, uint32_t levels)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { width, height_texels, 1 },
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

// One point sampler for the three, mips point too, clamped, so each fetch is one texel of one level.
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
		.maxLod = VK_LOD_CLAMP_NONE,
	};
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES };
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
	// displace.vert's height map at binding 0 and levels at 1, displace.frag's tint at 2.
	static const uint32_t images_at[IMAGES] = { IMAGE_HEIGHTS, IMAGE_LEVELS, IMAGE_TINT };
	for (uint32_t b = 0; b < IMAGES; b++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[images_at[b]],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->set,
			.dstBinding = b,
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
	const VkDescriptorSetLayoutBinding bindings[IMAGES] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = IMAGES,
		.pBindings = bindings,
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
		.vertex_spirv = displace_vert_spirv,
		.vertex_bytes = DISPLACE_VERT_SPIRV_BYTES,
		.fragment_spirv = displace_frag_spirv,
		.fragment_bytes = DISPLACE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = sizeof attributes / sizeof attributes[0],
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
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

static void record_upload(VkCommandBuffer commands, const Scene *scene)
{
	static const uint32_t levels[IMAGES] = { LEVELS, 1, 1 };
	VkBufferImageCopy regions[LEVELS];
	for (uint32_t level = 0; level < LEVELS; level++)
		regions[level] = (VkBufferImageCopy){
			.bufferOffset = level_offset(level),
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
			.imageExtent = { LEVEL_SIZE >> level, LEVEL_SIZE >> level, 1 },
		};
	const VkBufferImageCopy heights = {
		.bufferOffset = HEIGHT_OFFSET,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { HEIGHT_TEXELS, 1, 1 },
	};
	const VkBufferImageCopy tint = {
		.bufferOffset = TINT_OFFSET,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 1, 1, 1 },
	};
	for (uint32_t i = 0; i < IMAGES; i++)
		transition(
			commands, scene->images[i], levels[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkImageLayout dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	vkCmdCopyBufferToImage(commands, scene->staging, scene->images[IMAGE_LEVELS], dst, LEVELS, regions);
	vkCmdCopyBufferToImage(commands, scene->staging, scene->images[IMAGE_HEIGHTS], dst, 1, &heights);
	vkCmdCopyBufferToImage(commands, scene->staging, scene->images[IMAGE_TINT], dst, 1, &tint);
	for (uint32_t i = 0; i < IMAGES; i++)
		transition(commands, scene->images[i], levels[i], dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// One submission copies the three images in.
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
		record_upload(commands, scene);
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
	static uint8_t staging[STAGING_BYTES];
	static float vertices[QUADS][QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	for (uint32_t row = 0; row < ROWS; row++) {
		for (uint32_t c = 0; c < COLUMNS; c++)
			column_quad(row, c, vertices[row * COLUMNS + c]);
	}
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	ok = ok && harness_ok("levels", create_image(harness, scene, IMAGE_LEVELS, LEVEL_SIZE, LEVEL_SIZE, LEVELS));
	ok = ok && harness_ok("height map", create_image(harness, scene, IMAGE_HEIGHTS, HEIGHT_TEXELS, 1, 1));
	ok = ok && harness_ok("tint", create_image(harness, scene, IMAGE_TINT, 1, 1, 1));
	ok = ok && harness_ok("sampler and descriptors", create_descriptors(harness, scene));
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
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->set, 0, NULL);
		vkCmdDraw(commands, QUADS * QUAD_VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// A pipeline of many_samplers.vert's samplers, 16 or 15, and many_samplers.frag's 16, with one vertex binding.
static VkResult many_samplers(const Harness *harness, bool sixteen, VkRenderPass render_pass)
{
	VkDescriptorSetLayoutBinding bindings[MANY_BINDINGS];
	for (uint32_t b = 0; b < MANY_BINDINGS; b++)
		bindings[b] = (VkDescriptorSetLayoutBinding){ b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			b < MANY_BINDINGS / 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = MANY_BINDINGS,
		.pBindings = bindings,
	};
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &layout);
	const HarnessPipeline spec = {
		.vertex_spirv = sixteen ? many_samplers_vert_spirv : many_samplers_15_vert_spirv,
		.vertex_bytes = sixteen ? MANY_SAMPLERS_VERT_SPIRV_BYTES : MANY_SAMPLERS_15_VERT_SPIRV_BYTES,
		.fragment_spirv = many_samplers_frag_spirv,
		.fragment_bytes = MANY_SAMPLERS_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = layout,
	};
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, render_pass, &pipeline);
	vkDestroyPipeline(harness->device, pipeline, NULL);
	vkDestroyPipelineLayout(harness->device, layout, NULL);
	vkDestroyDescriptorSetLayout(harness->device, set_layout, NULL);
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
	vkDestroySampler(device, scene->sampler, NULL);
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

void test_vertex_textures(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene) && upload(harness, &scene);
	check(
		"vertex textures: a pipeline sampling two textures in its vertex shader and one in its fragment shader", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	for (uint32_t g = 0; g < GROUPS; g++)
		check(groups[g].passes, frames == FRAMES && scene.misses[g] == 0);
	const bool refused = built && many_samplers(harness, true, scene.render_pass) == VK_ERROR_FEATURE_NOT_PRESENT;
	const bool made = built && many_samplers(harness, false, scene.render_pass) == VK_SUCCESS;
	check("vertex textures: 16 samplers in each stage are refused, past the 32 fetch constants, and 15 and 16 made",
		refused && made);
	destroy(harness, &scene);
}
