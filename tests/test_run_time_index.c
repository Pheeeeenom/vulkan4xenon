#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	QUADS = 4,
	QUAD_VERTICES = 6,
	ROWS_PER_MATRIX = 3,
	MATRIX_ROWS = QUADS * ROWS_PER_MATRIX,
	BANDS = 10,
	BAND_PIXELS = 128,
	SCREEN_VERTICES = 6,
	TOLERANCE = 1,
};

typedef struct {
	float position[4];
	float matrix;
} MatrixVertex;

typedef struct {
	float position[4];
	float color[4];
} ColourVertex;

// matrix_index.vert's block: three rows a matrix, then a colour for each.
typedef struct {
	float rows[MATRIX_ROWS][4];
	float colors[QUADS][4];
} Matrices;

// band.frag's block.
typedef struct {
	float colors[BANDS][4];
} Bands;

static const float unit_quad[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

// Each matrix's quarter of the screen, in clip space, and its colour: a quad that took another's leaves its own clear.
typedef struct {
	float scale;
	float move_x;
	float move_y;
	Rgb color;
} Placement;

static const Placement placements[QUADS] = {
	{ 0.6f, -0.8f, -0.8f, { 220, 60, 40 } },
	{ 0.6f, 0.2f, -0.8f, { 40, 200, 80 } },
	{ 0.6f, -0.8f, 0.2f, { 60, 80, 220 } },
	{ 0.6f, 0.2f, 0.2f, { 220, 200, 40 } },
};

// A dark slate no other test clears to.
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 40.0f / 255, 56.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 40, 56 };

static const VkVertexInputBindingDescription matrix_binding = { 0, sizeof(MatrixVertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription matrix_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(MatrixVertex, matrix) },
};
static const VkVertexInputBindingDescription colour_binding = { 0, sizeof(ColourVertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription colour_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(ColourVertex, color) },
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer uniforms;
	VkDeviceMemory uniform_memory;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	uint32_t vertex_count;
	HarnessProbe probes[BANDS + 4];
	HarnessProbes check;
} Scene;

static float channel(uint32_t value)
{
	return value / 255.0f;
}

static uint32_t pixel_x(float x)
{
	return (uint32_t)((x + 1.0f) / 2.0f * HARNESS_WIDTH);
}

static uint32_t pixel_y(float y)
{
	return (uint32_t)((y + 1.0f) / 2.0f * HARNESS_HEIGHT);
}

static Rgb band_color(uint32_t band)
{
	return (Rgb){ 20 + band * 23, 230 - band * 21, band % 2 ? 200 : 40 };
}

// One uniform buffer, set 0 binding 0, for the stage.
static bool build_uniforms(
	const Harness *harness, Scene *scene, VkShaderStageFlags stage, const void *bytes, uint32_t size_bytes)
{
	const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	bool ok = harness_ok("vkCreateDescriptorSetLayout",
		vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout));
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	ok = ok &&
		harness_ok(
			"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	ok = ok &&
		harness_ok("vkCreateDescriptorPool", vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool));
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	ok = ok &&
		harness_ok("vkAllocateDescriptorSets", vkAllocateDescriptorSets(harness->device, &allocate_info, &scene->set));
	ok = ok &&
		harness_ok("uniform buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, bytes, size_bytes, &scene->uniforms,
				&scene->uniform_memory));
	if (!ok)
		return false;
	const VkDescriptorBufferInfo buffer = { scene->uniforms, 0, size_bytes };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &buffer,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return true;
}

static bool build_pipeline(
	const Harness *harness, Scene *scene, const HarnessPipeline *spec, const void *vertices, uint32_t vertex_bytes)
{
	bool ok = harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, spec, scene->render_pass, &scene->pipeline));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, vertex_bytes, &scene->vertices,
				&scene->vertex_memory));
}

// Each vertex names its quad's first matrix row.
static bool build_matrices(const Harness *harness, Scene *scene)
{
	Matrices block;
	memset(&block, 0, sizeof block);
	MatrixVertex vertices[QUADS * QUAD_VERTICES];
	for (uint32_t q = 0; q < QUADS; q++) {
		const Placement *const p = &placements[q];
		const uint32_t first = q * ROWS_PER_MATRIX;
		block.rows[first][0] = p->scale;
		block.rows[first][3] = p->move_x;
		block.rows[first + 1][1] = p->scale;
		block.rows[first + 1][3] = p->move_y;
		block.rows[first + 2][3] = 0.5f;
		block.colors[q][0] = channel(p->color.red);
		block.colors[q][1] = channel(p->color.green);
		block.colors[q][2] = channel(p->color.blue);
		block.colors[q][3] = 1.0f;
		for (uint32_t v = 0; v < QUAD_VERTICES; v++)
			vertices[q * QUAD_VERTICES + v] =
				(MatrixVertex){ { unit_quad[v][0], unit_quad[v][1], 0.0f, 1.0f }, (float)(q * ROWS_PER_MATRIX) };
		scene->probes[q] = (HarnessProbe){ "a quad's centre", pixel_x(p->move_x + p->scale / 2),
			pixel_y(p->move_y + p->scale / 2), p->color, 0 };
	}
	scene->probes[QUADS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->check = (HarnessProbes){ "run-time index: matrices", scene->probes, QUADS + 1, TOLERANCE, 0 };
	scene->vertex_count = QUADS * QUAD_VERTICES;
	bool ok = build_uniforms(harness, scene, VK_SHADER_STAGE_VERTEX_BIT, &block, sizeof block);
	const HarnessPipeline spec = {
		.vertex_spirv = matrix_index_vert_spirv,
		.vertex_bytes = MATRIX_INDEX_VERT_SPIRV_BYTES,
		.fragment_spirv = triangle_frag_spirv,
		.fragment_bytes = TRIANGLE_FRAG_SPIRV_BYTES,
		.bindings = &matrix_binding,
		.binding_count = 1,
		.attributes = matrix_attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	return ok && build_pipeline(harness, scene, &spec, vertices, sizeof vertices);
}

// The screen in bands; probes either side of band edges too, where a column a pixel off would show.
static bool build_bands(const Harness *harness, Scene *scene)
{
	Bands block;
	for (uint32_t b = 0; b < BANDS; b++) {
		const Rgb color = band_color(b);
		const float values[4] = { channel(color.red), channel(color.green), channel(color.blue), 1.0f };
		memcpy(block.colors[b], values, sizeof values);
		scene->probes[b] = (HarnessProbe){ "a band's middle", b * BAND_PIXELS + BAND_PIXELS / 2, 360, color, 0 };
	}
	scene->probes[BANDS] = (HarnessProbe){ "band 0's last column", BAND_PIXELS - 1, 100, band_color(0), 0 };
	scene->probes[BANDS + 1] = (HarnessProbe){ "band 1's first column", BAND_PIXELS, 100, band_color(1), 0 };
	scene->probes[BANDS + 2] = (HarnessProbe){ "band 8's last column", 9 * BAND_PIXELS - 1, 650, band_color(8), 0 };
	scene->probes[BANDS + 3] = (HarnessProbe){ "the last column", HARNESS_WIDTH - 1, 650, band_color(9), 0 };
	scene->check = (HarnessProbes){ "run-time index: pixel position", scene->probes, BANDS + 4, TOLERANCE, 0 };
	ColourVertex vertices[SCREEN_VERTICES];
	for (uint32_t v = 0; v < SCREEN_VERTICES; v++)
		vertices[v] = (ColourVertex){ { unit_quad[v][0] * 2.0f - 1.0f, unit_quad[v][1] * 2.0f - 1.0f, 0.5f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f } };
	scene->vertex_count = SCREEN_VERTICES;
	bool ok = build_uniforms(harness, scene, VK_SHADER_STAGE_FRAGMENT_BIT, &block, sizeof block);
	const HarnessPipeline spec = {
		.vertex_spirv = triangle_vert_spirv,
		.vertex_bytes = TRIANGLE_VERT_SPIRV_BYTES,
		.fragment_spirv = band_frag_spirv,
		.fragment_bytes = BAND_FRAG_SPIRV_BYTES,
		.bindings = &colour_binding,
		.binding_count = 1,
		.attributes = colour_attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	return ok && build_pipeline(harness, scene, &spec, vertices, sizeof vertices);
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
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->set, 0, NULL);
		vkCmdDraw(commands, scene->vertex_count, 1, 0, 0);
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
	vkDestroyBuffer(device, scene->uniforms, NULL);
	vkFreeMemory(device, scene->uniform_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

static void run(const Harness *harness, const char *name, bool (*build)(const Harness *, Scene *), const char *holds)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool built = build(harness, &scene);
	char message[160];
	snprintf(message, sizeof message, "run-time index: %s: a set, a pipeline and buffers made", name);
	check(message, built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	snprintf(message, sizeof message, "run-time index: %s: 60 frames drawn and presented", name);
	check(message, frames == FRAMES);
	snprintf(message, sizeof message, "run-time index: %s: every frame %s", name, holds);
	check(message, frames == FRAMES && misses == 0);
	snprintf(message, sizeof message, "run-time index: %s: the image on screen does too", name);
	check(message, harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}

void test_run_time_index(const Harness *harness)
{
	run(harness, "matrices picked per vertex", build_matrices,
		"places each quad by the matrix its vertices name, in that matrix's colour");
	run(harness, "pixel position", build_bands, "colours each 128-pixel band by the pixel's column");
}
