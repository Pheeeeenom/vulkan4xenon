#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Each address mode past 0 and 1: a 4x4 image drawn over its own quad from -1.5 to 2.5 each way, eight pixels a texel,
// so each mode's texels show from -6 to 9. Clamp to border in opaque white, and in transparent black blended over the
// clear colour, which shows through where the border's alpha is 0 (milestone 23).

enum {
	FRAMES = 30,
	FENCE_TIMEOUT_NS = 1000000000,
	TEXELS = 4,
	TEXEL_BYTES = 4,
	STAGING_BYTES = TEXELS * TEXELS * TEXEL_BYTES,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	CELL_SIZE = 128,
	CELL_STEP = 200,
	CELL_LEFT = 40,
	CELL_TOP = 200,
	// A texel is 8 pixels, the quad 16 texels from -6.
	TEXEL_PIXELS = CELL_SIZE / 16,
	FIRST_TEXEL = -6,
	// The texels probed across and down each cell: two past each edge, two in the mirror image and two inside.
	PROBED = 6,
	TOLERANCE = 1,
};

// The black border's cell, whose border is blended over the clear colour.
enum {
	CELL_REPEAT,
	CELL_MIRRORED,
	CELL_EDGE,
	CELL_MIRROR_EDGE,
	CELL_WHITE,
	CELL_BLACK,
	CELLS,
};

typedef struct {
	const char *name;
	VkSamplerAddressMode mode;
	VkBorderColor border;
} Cell;

static const Cell cells[CELLS] = {
	[CELL_REPEAT] = { "repeat", VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
	[CELL_MIRRORED] = { "mirrored repeat", VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
	[CELL_EDGE] = { "clamp to edge", VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
	[CELL_MIRROR_EDGE] = { "mirror clamp to edge", VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
	[CELL_WHITE] = { "an opaque white border", VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE },
	[CELL_BLACK] = { "a transparent black border", VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
};

static const int32_t probed[PROBED] = { -5, -2, 0, 3, 5, 8 };

enum {
	// Every probe but the black border's, then the black border's, then the clear colour.
	MODE_PROBES = CELLS * PROBED * PROBED,
	PROBES = MODE_PROBES + 1,
};

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };
static const Rgb white = { 255, 255, 255 };
static const VkPipelineColorBlendAttachmentState alpha_blend = {
	.blendEnable = VK_TRUE,
	.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
	.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	.colorBlendOp = VK_BLEND_OP_ADD,
	.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
	.alphaBlendOp = VK_BLEND_OP_ADD,
	.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline opaque;
	VkPipeline blended;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkSampler samplers[CELLS];
	VkDescriptorPool pool;
	VkDescriptorSet sets[CELLS];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[PROBES];
	HarnessProbes modes;
	HarnessProbes black;
} Scene;

static Rgb texel_rgb(uint32_t x, uint32_t y)
{
	return (Rgb){ 40 + 60 * x, 40 + 60 * y, 220 - 25 * (x + y) };
}

// The texel each mode reads at an index past the image's edges, or -1 for the border.
static int32_t addressed(VkSamplerAddressMode mode, int32_t index)
{
	const int32_t once = index < 0 ? -1 - index : index;
	const int32_t folded = (index % (2 * TEXELS) + 2 * TEXELS) % (2 * TEXELS);
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		return (index % TEXELS + TEXELS) % TEXELS;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		return folded < TEXELS ? folded : 2 * TEXELS - 1 - folded;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		return index < 0 ? 0 : index >= TEXELS ? TEXELS - 1 : index;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
		return once >= TEXELS ? TEXELS - 1 : once;
	default:
		return index < 0 || index >= TEXELS ? -1 : index;
	}
}

static uint32_t cell_x(uint32_t cell)
{
	return CELL_LEFT + cell * CELL_STEP;
}

// What a cell's probed texel shows: the image's texel, the white border, or the clear colour through the black one.
static Rgb probe_want(uint32_t cell, int32_t i, int32_t j)
{
	const int32_t x = addressed(cells[cell].mode, i);
	const int32_t y = addressed(cells[cell].mode, j);
	if (x >= 0 && y >= 0)
		return texel_rgb((uint32_t)x, (uint32_t)y);
	return cell == CELL_WHITE ? white : clear_rgb;
}

// Each probe at its texel's centre; the black border's last, so they're a set of their own.
static void make_probes(Scene *scene)
{
	uint32_t modes = 0;
	uint32_t borders = 0;
	HarnessProbe border_probes[PROBED * PROBED];
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		for (uint32_t row = 0; row < PROBED; row++) {
			for (uint32_t column = 0; column < PROBED; column++) {
				const int32_t i = probed[column];
				const int32_t j = probed[row];
				const HarnessProbe probe = { cells[cell].name,
					cell_x(cell) + (uint32_t)(i - FIRST_TEXEL) * TEXEL_PIXELS + TEXEL_PIXELS / 2,
					CELL_TOP + (uint32_t)(j - FIRST_TEXEL) * TEXEL_PIXELS + TEXEL_PIXELS / 2, probe_want(cell, i, j),
					0 };
				const bool border = addressed(cells[cell].mode, i) < 0 || addressed(cells[cell].mode, j) < 0;
				if (cell == CELL_BLACK && border)
					border_probes[borders++] = probe;
				else
					scene->probes[modes++] = probe;
			}
		}
	}
	scene->probes[modes++] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	memcpy(&scene->probes[modes], border_probes, borders * sizeof border_probes[0]);
	scene->modes = (HarnessProbes){ "addressing", scene->probes, modes, TOLERANCE, 0 };
	scene->black = (HarnessProbes){ "black border", &scene->probes[modes], borders, TOLERANCE, 0 };
}

static void fill_staging(uint8_t *bytes)
{
	for (uint32_t y = 0; y < TEXELS; y++) {
		for (uint32_t x = 0; x < TEXELS; x++) {
			const Rgb rgb = texel_rgb(x, y);
			uint8_t *const texel = bytes + (y * TEXELS + x) * TEXEL_BYTES;
			texel[0] = (uint8_t)rgb.red, texel[1] = (uint8_t)rgb.green, texel[2] = (uint8_t)rgb.blue, texel[3] = 0xFF;
		}
	}
}

// Each cell's quad, from -1.5 to 2.5 each way.
static void fill_vertices(float vertices[CELLS * QUAD_VERTICES][VERTEX_FLOATS])
{
	static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
			float *const vertex = vertices[cell * QUAD_VERTICES + i];
			const float s = corners[i][0];
			const float t = corners[i][1];
			vertex[0] = ((float)cell_x(cell) + s * CELL_SIZE) / (HARNESS_WIDTH / 2) - 1.0f;
			vertex[1] = ((float)CELL_TOP + t * CELL_SIZE) / (HARNESS_HEIGHT / 2) - 1.0f;
			vertex[2] = 0.0f;
			vertex[3] = 1.0f;
			vertex[4] = -1.5f + 4.0f * s;
			vertex[5] = -1.5f + 4.0f * t;
		}
	}
}

static VkResult make_image(const Harness *harness, Scene *scene)
{
	return harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, TEXELS, TEXELS,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &scene->image, &scene->image_memory,
		&scene->view);
}

static VkResult make_descriptors(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t cell = 0; cell < CELLS && result == VK_SUCCESS; cell++) {
		const VkSamplerCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = cells[cell].mode,
			.addressModeV = cells[cell].mode,
			.addressModeW = cells[cell].mode,
			.borderColor = cells[cell].border,
		};
		result = vkCreateSampler(harness->device, &info, NULL, &scene->samplers[cell]);
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
	for (uint32_t cell = 0; cell < CELLS; cell++)
		layouts[cell] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = CELLS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t cell = 0; cell < CELLS && result == VK_SUCCESS; cell++) {
		const VkDescriptorImageInfo image = { scene->samplers[cell], scene->view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[cell],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

static VkResult make_pipelines(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding_info = {
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
	HarnessPipeline spec = {
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
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->opaque);
	spec.blend = &alpha_blend;
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->blended);
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	static float vertices[CELLS * QUAD_VERTICES][VERTEX_FLOATS];
	uint8_t staging[STAGING_BYTES];
	fill_vertices(vertices);
	fill_staging(staging);
	return harness_ok("pipelines", make_pipelines(harness, scene)) && harness_ok("image", make_image(harness, scene)) &&
		harness_ok("samplers", make_descriptors(harness, scene)) &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory)) &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
				&scene->staging_memory));
}

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
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { TEXELS, TEXELS, 1 },
	};
	vkCmdCopyBufferToImage(commands, scene->staging, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t cell = 0; cell < CELLS; cell++) {
			vkCmdBindPipeline(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, cell == CELL_BLACK ? scene->blended : scene->opaque);
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[cell], 0, NULL);
			vkCmdDraw(commands, QUAD_VERTICES, 1, cell * QUAD_VERTICES, 0);
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// The device lists VK_KHR_sampler_mirror_clamp_to_edge, which the harness enables.
static bool lists_mirror_clamp(const Harness *harness)
{
	VkExtensionProperties extensions[8];
	uint32_t count = sizeof extensions / sizeof extensions[0];
	if (vkEnumerateDeviceExtensionProperties(harness->physical_device, NULL, &count, extensions) != VK_SUCCESS)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		if (!strcmp(extensions[i].extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))
			return true;
	}
	return false;
}

// Opaque black and the integer borders, where a mode reads the border; with none that does, any border colour.
static bool refuses_borders(const Harness *harness)
{
	VkSamplerCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	VkSampler sampler = VK_NULL_HANDLE;
	bool refused = vkCreateSampler(harness->device, &info, NULL, &sampler) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
	refused = refused && vkCreateSampler(harness->device, &info, NULL, &sampler) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	const bool taken = vkCreateSampler(harness->device, &info, NULL, &sampler) == VK_SUCCESS;
	if (taken)
		vkDestroySampler(harness->device, sampler, NULL);
	return refused && taken;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	for (uint32_t cell = 0; cell < CELLS; cell++)
		vkDestroySampler(device, scene->samplers[cell], NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->opaque, NULL);
	vkDestroyPipeline(device, scene->blended, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_addressing(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	check("addressing: VK_KHR_sampler_mirror_clamp_to_edge is listed", lists_mirror_clamp(harness));
	const bool built = build(harness, &scene);
	check("addressing: an image, a sampler for each address mode and the pipelines made", built);
	check("addressing: an opaque black border and the integer borders refused, where a mode reads the border",
		refuses_borders(harness));
	const bool uploaded = built && upload(harness, &scene);
	const bool recorded = uploaded && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.modes, &misses) : 0;
	check("addressing: 30 frames drawn and presented", frames == FRAMES);
	check(
		"addressing: every frame shows repeat, mirrored repeat, clamp to edge, mirror clamp to edge and an opaque "
		"white border past 0 and 1",
		frames == FRAMES && misses == 0);
	check("addressing: the image on screen shows them too", harness_screen(harness, harness_probes_hold, &scene.modes));
	check("addressing: a transparent black border's alpha is 0, the clear colour showing through it blended",
		frames == FRAMES && harness_screen(harness, harness_probes_hold, &scene.black));
	destroy(harness, &scene);
}
