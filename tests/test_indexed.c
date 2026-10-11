#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	CELL = 150,
	CELL_Y = 120,
	SECOND_ROW_Y = 400,
	STRIDE_BYTES = 16,
	// The vertex buffer: shapes in the unit square, which push constants place in cells, then the long list's quad.
	CORNERS = 4,
	POINTS = 5,
	// Two vertices nothing draws, then the corners out of order: bottom right, top left, top right, bottom left.
	SCRAMBLED_AT = 0,
	// The corners in order, top left, top right, bottom right, bottom left, for the strip, fan and lines.
	ORDERED_AT = 6,
	POINTS_AT = 10,
	// Past 65535, so the long list's indices need all 32 bits; everything before it is a degenerate vertex.
	FAR_AT = 70000,
	VERTICES = FAR_AT + CORNERS,
	// Enough degenerate indices that the list passes a draw packet's 65535.
	LONG_LIST_INDICES = 65535 + 6,
	TOLERANCE = 2,
	LINE_RADIUS = 2,
	INSET = 12,
	OUTSIDE = 10,
};

typedef enum {
	SHAPE_TRIANGLE_LIST,
	SHAPE_TRIANGLE_STRIP,
	SHAPE_TRIANGLE_FAN,
	SHAPE_LINE_LIST,
	SHAPE_LINE_STRIP,
	SHAPE_POINT_LIST,
	SHAPE_LONG_LIST,
	SHAPES,
} Shape;

static const VkPrimitiveTopology topologies[SHAPES] = {
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
	VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
	VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
};

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	Rgb rgb;
	bool filled;
} Cell;

static const Cell cells[SHAPES] = {
	{ "triangle list", 60, CELL_Y, { 224, 64, 64 }, true },
	{ "triangle strip", 240, CELL_Y, { 64, 224, 64 }, true },
	{ "triangle fan", 420, CELL_Y, { 64, 96, 240 }, true },
	{ "line list", 600, CELL_Y, { 240, 224, 64 }, false },
	{ "line strip", 780, CELL_Y, { 64, 224, 224 }, false },
	{ "point list", 960, CELL_Y, { 255, 255, 255 }, false },
	{ "long list", 60, SECOND_ROW_Y, { 240, 128, 0 }, true },
};

// 16-bit: three junk indices the first index skips, then two triangles of the scrambled corners at 2: top left,
// top right, bottom left; top right, bottom right, bottom left.
static const uint16_t list_indices[] = { 9, 9, 9, 3, 4, 5, 4, 2, 5 };
// 32-bit, with a vertex offset of 6: a strip down the quad, then the line strip round it.
static const uint32_t strip_indices[] = { 0, 3, 1, 2 };
static const uint32_t line_strip_indices[] = { 0, 1, 2, 3, 0 };
// 16-bit, from the ordered corners at 6: a fan from the top left, then the outline as a list.
static const uint16_t fan_indices[] = { 6, 7, 8, 9 };
static const uint16_t line_list_indices[] = { 6, 7, 7, 8, 8, 9, 9, 6 };

static const VkClearValue clear = { .color = { .float32 = { 16.0f / 255, 24.0f / 255, 40.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 16, 24, 40 };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[SHAPES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer indices;
	VkDeviceMemory index_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[64];
	HarnessProbes check;
	// Where each index list sits in the index buffer.
	uint32_t list_at;
	uint32_t strip_at;
	uint32_t line_strip_at;
	uint32_t fan_at;
	uint32_t line_list_at;
	uint32_t long_at;
} Scene;

static void set_vertex(float *vertices, uint32_t index, float x, float y)
{
	float *const v = vertices + index * 4;
	v[0] = x, v[1] = y, v[2] = 0.0f, v[3] = 1.0f;
}

static float *make_vertices(void)
{
	float *const vertices = calloc(VERTICES * 4, sizeof(float));
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < VERTICES; i++)
		set_vertex(vertices, i, 0.5f, 0.5f);
	static const float scrambled[CORNERS][2] = { { 1, 1 }, { 0, 0 }, { 1, 0 }, { 0, 1 } };
	static const float ordered[CORNERS][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const float points[POINTS][2] = { { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.1f, 0.9f }, { 0.9f, 0.9f },
		{ 0.5f, 0.5f } };
	for (uint32_t i = 0; i < CORNERS; i++) {
		set_vertex(vertices, SCRAMBLED_AT + 2 + i, scrambled[i][0], scrambled[i][1]);
		set_vertex(vertices, ORDERED_AT + i, ordered[i][0], ordered[i][1]);
		set_vertex(vertices, FAR_AT + i, ordered[i][0], ordered[i][1]);
	}
	for (uint32_t i = 0; i < POINTS; i++)
		set_vertex(vertices, POINTS_AT + i, points[i][0], points[i][1]);
	return vertices;
}

static uint32_t append(uint8_t *bytes, uint32_t *used, const void *data, uint32_t size_bytes)
{
	const uint32_t at = *used;
	memcpy(bytes + at, data, size_bytes);
	*used = (at + size_bytes + 3) & ~3u;
	return at;
}

// Every list in one buffer; the long list's leading indices all name a degenerate vertex, then a quad past 65535.
static uint8_t *make_indices(Scene *scene, uint32_t *size_bytes)
{
	const uint32_t bytes = 256 + LONG_LIST_INDICES * 4;
	uint8_t *const indices = calloc(bytes, 1);
	if (!indices)
		return NULL;
	uint32_t used = 0;
	scene->list_at = append(indices, &used, list_indices, sizeof list_indices);
	scene->strip_at = append(indices, &used, strip_indices, sizeof strip_indices);
	scene->line_strip_at = append(indices, &used, line_strip_indices, sizeof line_strip_indices);
	scene->fan_at = append(indices, &used, fan_indices, sizeof fan_indices);
	scene->line_list_at = append(indices, &used, line_list_indices, sizeof line_list_indices);
	scene->long_at = used;
	uint32_t *const long_list = (uint32_t *)(indices + used);
	const uint32_t quad[6] = { FAR_AT, FAR_AT + 1, FAR_AT + 3, FAR_AT + 1, FAR_AT + 2, FAR_AT + 3 };
	memcpy(long_list + LONG_LIST_INDICES - 6, quad, sizeof quad);
	*size_bytes = used + LONG_LIST_INDICES * 4;
	return indices;
}

static uint32_t probe_count;

static void probe(Scene *scene, const char *name, uint32_t x, uint32_t y, Rgb want, uint32_t radius)
{
	scene->probes[probe_count++] = (HarnessProbe){ name, x, y, want, radius };
}

// Filled shapes: their inside to the edges, and nothing just outside; lines: each edge, with a clear middle; points:
// each point, and nothing between them.
static void make_probes(Scene *scene)
{
	probe_count = 0;
	for (uint32_t i = 0; i < SHAPES; i++) {
		const Cell *const c = &cells[i];
		if (c->filled) {
			probe(scene, c->name, c->x + CELL / 2, c->y + CELL / 2, c->rgb, 0);
			probe(scene, c->name, c->x + INSET, c->y + INSET, c->rgb, 0);
			probe(scene, c->name, c->x + CELL - INSET, c->y + CELL - INSET, c->rgb, 0);
			probe(scene, "outside a filled shape", c->x - OUTSIDE, c->y + CELL / 2, clear_rgb, 0);
			probe(scene, "outside a filled shape", c->x + CELL + OUTSIDE, c->y + CELL / 2, clear_rgb, 0);
		} else if (i == SHAPE_POINT_LIST) {
			probe(scene, c->name, c->x + CELL / 10, c->y + CELL / 10, c->rgb, LINE_RADIUS);
			probe(scene, c->name, c->x + CELL * 9 / 10, c->y + CELL * 9 / 10, c->rgb, LINE_RADIUS);
			probe(scene, c->name, c->x + CELL / 2, c->y + CELL / 2, c->rgb, LINE_RADIUS);
			probe(scene, "between points", c->x + CELL * 3 / 10, c->y + CELL / 10, clear_rgb, 0);
		} else {
			probe(scene, c->name, c->x + CELL / 2, c->y, c->rgb, LINE_RADIUS);
			probe(scene, c->name, c->x + CELL, c->y + CELL / 2, c->rgb, LINE_RADIUS);
			probe(scene, c->name, c->x + CELL / 2, c->y + CELL, c->rgb, LINE_RADIUS);
			probe(scene, c->name, c->x, c->y + CELL / 2, c->rgb, LINE_RADIUS);
			probe(scene, "inside an outline", c->x + CELL / 2, c->y + CELL / 2, clear_rgb, 0);
		}
	}
	scene->check = (HarnessProbes){ "indexed", scene->probes, probe_count, TOLERANCE, 0 };
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < SHAPES && ok; i++) {
		const VkPipelineInputAssemblyStateCreateInfo assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = topologies[i],
		};
		const HarnessPipeline spec = {
			.vertex_spirv = push_vert_spirv,
			.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
			.fragment_spirv = push_frag_spirv,
			.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
			.bindings = &harness_quad_binding,
			.binding_count = 1,
			.attributes = &harness_quad_attribute,
			.attribute_count = 1,
			.layout = scene->layout,
			.assembly = &assembly,
		};
		ok = harness_ok(cells[i].name, harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[i]));
	}
	float *const vertices = make_vertices();
	uint32_t index_bytes = 0;
	uint8_t *const indices = make_indices(scene, &index_bytes);
	ok = ok && vertices && indices;
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, VERTICES * STRIDE_BYTES,
				&scene->vertices, &scene->vertex_memory));
	ok = ok &&
		harness_ok("index buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, index_bytes, &scene->indices,
				&scene->index_memory));
	free(vertices);
	free(indices);
	return ok;
}

// Places the unit square in the cell and colours it.
static void place(VkCommandBuffer commands, VkPipelineLayout layout, const Cell *cell)
{
	const float half_width = HARNESS_WIDTH / 2.0f;
	const float half_height = HARNESS_HEIGHT / 2.0f;
	const float place_and_scale[8] = { CELL / half_width, CELL / half_height, 0.0f, 1.0f, cell->x / half_width - 1.0f,
		cell->y / half_height - 1.0f, 0.0f, 0.0f };
	const float color[4] = { cell->rgb.red / 255.0f, cell->rgb.green / 255.0f, cell->rgb.blue / 255.0f, 1.0f };
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place_and_scale, place_and_scale);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof color, color);
}

static void draw_shape(VkCommandBuffer commands, const Scene *scene, Shape shape)
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[shape]);
	place(commands, scene->layout, &cells[shape]);
	switch (shape) {
	case SHAPE_TRIANGLE_LIST:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->list_at, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commands, 6, 1, 3, 0, 0);
		break;
	case SHAPE_TRIANGLE_STRIP:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->strip_at, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(commands, 4, 1, 0, ORDERED_AT, 0);
		break;
	case SHAPE_TRIANGLE_FAN:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->fan_at, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commands, 4, 1, 0, 0, 0);
		break;
	case SHAPE_LINE_LIST:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->line_list_at, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commands, 8, 1, 0, 0, 0);
		break;
	case SHAPE_LINE_STRIP:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->line_strip_at, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(commands, 5, 1, 0, ORDERED_AT, 0);
		break;
	case SHAPE_POINT_LIST:
		vkCmdDraw(commands, POINTS, 1, POINTS_AT, 0);
		break;
	case SHAPE_LONG_LIST:
		vkCmdBindIndexBuffer(commands, scene->indices, scene->long_at, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(commands, LONG_LIST_INDICES, 1, 0, 0, 0);
		break;
	case SHAPES:
		break;
	}
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
		for (uint32_t shape = 0; shape < SHAPES; shape++)
			draw_shape(commands, scene, (Shape)shape);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->indices, NULL);
	vkFreeMemory(device, scene->index_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < SHAPES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_indexed(const Harness *harness)
{
	static Scene scene;
	scene = (Scene){ .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("indexed: a pipeline for each topology, a vertex buffer and an index buffer made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("indexed: 60 frames drawn and presented", frames == FRAMES);
	check(
		"indexed: every frame has each shape in its cell: triangle lists, strips and fans, lines, points, and a list "
		"past 65535",
		frames == FRAMES && misses == 0);
	check("indexed: the image on screen shows them too", harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
