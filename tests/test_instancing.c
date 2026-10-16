#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 24's instanced draws: a row of a grid each, every instance a quad in its own cell and colour.
// instanced.vert takes its column from gl_InstanceIndex and its row and colour from the instance's attributes;
// instanced_places.vert takes column and row from its attributes alone. Each row's draw binds the instance buffer at
// its own 8 entries, so an entry's colour shows where gl_InstanceIndex and the attributes agree on the instance.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// instanced.vert's grid: cells 160 by 120 pixels, each quad 96 by 72 of its cell from 32 by 24 in.
	COLUMNS = 8,
	ROWS = 5,
	CELL_WIDTH = 160,
	CELL_HEIGHT = 120,
	// Two vertices nothing draws, then the corners for indexed draws, then a list of six for the others.
	CORNERS_AT = 2,
	LIST_AT = 6,
	VERTICES = LIST_AT + 6,
	ENTRIES = COLUMNS,
	ENTRY_BYTES = 24,
	// A VkDrawIndirectCommand, then a VkDrawIndexedIndirectCommand.
	INDEXED_AT = 16,
	INDIRECT_BYTES = INDEXED_AT + 20,
};

typedef enum {
	ROW_DRAW,
	ROW_INDEXED,
	ROW_INDIRECT,
	ROW_INDEXED_INDIRECT,
	ROW_PLACES,
} Row;

typedef struct {
	const char *what;
	uint32_t instances;
	uint32_t first_instance;
} RowDraw;

static const RowDraw rows[ROWS] = {
	{ "vkCmdDraw", 4, 0 },
	{ "vkCmdDrawIndexed", 3, 2 },
	{ "vkCmdDrawIndirect", 4, 3 },
	{ "vkCmdDrawIndexedIndirect", 2, 5 },
	{ "vkCmdDraw without gl_InstanceIndex", 4, 1 },
};

// The quad's corners in the unit square: top left, top right, bottom left; top right, bottom right, bottom left.
static const float list[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
static const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
// Into the corners, at a vertex offset of 2.
static const uint16_t indices[6] = { 0, 1, 3, 1, 2, 3 };

static const VkVertexInputBindingDescription bindings[2] = {
	{ 0, 8, VK_VERTEX_INPUT_RATE_VERTEX },
	{ 1, ENTRY_BYTES, VK_VERTEX_INPUT_RATE_INSTANCE },
};
// The corner per vertex; the colour and the place per instance, the place first in each entry.
static const VkVertexInputAttributeDescription attributes[3] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 8 },
	{ .location = 2, .binding = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
};

static const VkClearValue clear = { .color = { .float32 = { 16.0f / 255, 24.0f / 255, 40.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 16, 24, 40 };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	// With gl_InstanceIndex, and with the attributes alone.
	VkPipeline pipelines[2];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer instances;
	VkDeviceMemory instance_memory;
	VkBuffer indices;
	VkDeviceMemory index_memory;
	VkBuffer indirect;
	VkDeviceMemory indirect_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[ROWS][COLUMNS];
	HarnessProbes checks[ROWS];
	uint32_t misses[ROWS];
} Scene;

// Entry e of a row's 8: its own colour, and its place, column 7 - e, which only instanced_places.vert reads.
static Rgb entry_rgb(uint32_t row, uint32_t entry)
{
	return (Rgb){ 40 + 26 * entry, 60 + 36 * row, 220 - 24 * entry };
}

static uint32_t places_column(uint32_t entry)
{
	return COLUMNS - 1 - entry;
}

// Each cell of the row holds its instance's entry colour or the clear colour.
static void make_probes(Scene *scene)
{
	for (uint32_t row = 0; row < ROWS; row++) {
		const RowDraw *const draw = &rows[row];
		for (uint32_t column = 0; column < COLUMNS; column++) {
			Rgb want = clear_rgb;
			for (uint32_t i = 0; i < draw->instances; i++) {
				const uint32_t entry = draw->first_instance + i;
				const uint32_t at = row == ROW_PLACES ? places_column(entry) : entry;
				if (at == column)
					want = entry_rgb(row, entry);
			}
			scene->probes[row][column] = (HarnessProbe){ draw->what, column * CELL_WIDTH + CELL_WIDTH / 2,
				row * CELL_HEIGHT + CELL_HEIGHT / 2, want, 0 };
		}
		scene->checks[row] = (HarnessProbes){ "instancing", scene->probes[row], COLUMNS, TOLERANCE, 0 };
	}
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, bool index)
{
	const HarnessPipeline spec = {
		.vertex_spirv = index ? instanced_vert_spirv : instanced_places_vert_spirv,
		.vertex_bytes = index ? INSTANCED_VERT_SPIRV_BYTES : INSTANCED_PLACES_VERT_SPIRV_BYTES,
		.fragment_spirv = triangle_frag_spirv,
		.fragment_bytes = TRIANGLE_FRAG_SPIRV_BYTES,
		.bindings = bindings,
		.binding_count = 2,
		.attributes = attributes,
		.attribute_count = 3,
		.layout = scene->layout,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[index ? 0 : 1]);
}

static bool make_buffers(const Harness *harness, Scene *scene)
{
	float vertices[VERTICES][2] = { { 0 } };
	for (uint32_t i = 0; i < 4; i++) {
		vertices[CORNERS_AT + i][0] = corners[i][0];
		vertices[CORNERS_AT + i][1] = corners[i][1];
	}
	for (uint32_t i = 0; i < 6; i++) {
		vertices[LIST_AT + i][0] = list[i][0];
		vertices[LIST_AT + i][1] = list[i][1];
	}
	float entries[ROWS * ENTRIES][ENTRY_BYTES / 4];
	for (uint32_t row = 0; row < ROWS; row++) {
		for (uint32_t entry = 0; entry < ENTRIES; entry++) {
			const Rgb rgb = entry_rgb(row, entry);
			float *const e = entries[row * ENTRIES + entry];
			e[0] = (float)places_column(entry), e[1] = (float)row;
			e[2] = rgb.red / 255.0f, e[3] = rgb.green / 255.0f, e[4] = rgb.blue / 255.0f, e[5] = 1.0f;
		}
	}
	// The indirect draws' own instances and first instances, as rows gives them.
	const uint32_t draw[4] = { 6, rows[ROW_INDIRECT].instances, LIST_AT, rows[ROW_INDIRECT].first_instance };
	const uint32_t indexed[5] = { 6, rows[ROW_INDEXED_INDIRECT].instances, 0, CORNERS_AT,
		rows[ROW_INDEXED_INDIRECT].first_instance };
	uint32_t indirect[INDIRECT_BYTES / 4];
	for (uint32_t i = 0; i < 4; i++)
		indirect[i] = draw[i];
	for (uint32_t i = 0; i < 5; i++)
		indirect[INDEXED_AT / 4 + i] = indexed[i];
	return harness_ok("vertices",
			   harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				   &scene->vertex_memory)) &&
		harness_ok("instances",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, entries, sizeof entries, &scene->instances,
				&scene->instance_memory)) &&
		harness_ok("indices",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, sizeof indices, &scene->indices,
				&scene->index_memory)) &&
		harness_ok("indirect",
			harness_buffer(harness, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirect, sizeof indirect, &scene->indirect,
				&scene->indirect_memory));
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	bool ok = harness_ok("layout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("instanced pipeline", create_pipeline(harness, scene, true));
	ok = ok && harness_ok("places pipeline", create_pipeline(harness, scene, false));
	return ok && make_buffers(harness, scene);
}

// Binds the row's 8 entries as the instance buffer, then draws its instances.
static void draw_row(VkCommandBuffer commands, const Scene *scene, Row row)
{
	const VkDeviceSize offset = (VkDeviceSize)row * ENTRIES * ENTRY_BYTES;
	vkCmdBindVertexBuffers(commands, 1, 1, &scene->instances, &offset);
	const RowDraw *const draw = &rows[row];
	switch (row) {
	case ROW_DRAW:
	case ROW_PLACES:
		vkCmdDraw(commands, 6, draw->instances, LIST_AT, draw->first_instance);
		break;
	case ROW_INDEXED:
		vkCmdDrawIndexed(commands, 6, draw->instances, 0, CORNERS_AT, draw->first_instance);
		break;
	case ROW_INDIRECT:
		vkCmdDrawIndirect(commands, scene->indirect, 0, 1, INDEXED_AT);
		break;
	case ROW_INDEXED_INDIRECT:
		vkCmdDrawIndexedIndirect(commands, scene->indirect, INDEXED_AT, 1, INDIRECT_BYTES - INDEXED_AT);
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
		vkCmdBindIndexBuffer(commands, scene->indices, 0, VK_INDEX_TYPE_UINT16);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[0]);
		for (uint32_t row = ROW_DRAW; row < ROW_PLACES; row++)
			draw_row(commands, scene, (Row)row);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[1]);
		draw_row(commands, scene, ROW_PLACES);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// Every row's probes, each row's misses counted apart.
static bool rows_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t row = 0; row < ROWS; row++) {
		if (!harness_probes_hold(address, what, &scene->checks[row])) {
			scene->misses[row]++;
			all = false;
		}
	}
	return all;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	const VkBuffer buffers[] = { scene->vertices, scene->instances, scene->indices, scene->indirect };
	const VkDeviceMemory memories[] = { scene->vertex_memory, scene->instance_memory, scene->index_memory,
		scene->indirect_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	vkDestroyPipeline(device, scene->pipelines[0], NULL);
	vkDestroyPipeline(device, scene->pipelines[1], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_instancing(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	check("instancing: the device has drawIndirectFirstInstance, and took it when made",
		features.drawIndirectFirstInstance);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("instancing: pipelines with a binding at the instance's rate made, with and without gl_InstanceIndex", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames = recorded ? harness_frames(harness, scene.commands, FRAMES, rows_hold, &scene, &misses) : 0;
	check("instancing: 30 frames drawn and presented", frames == FRAMES);
	check(
		"instancing: every frame, vkCmdDraw draws 4 instances, each in the column gl_InstanceIndex gives and the "
		"colour its attributes give",
		frames == FRAMES && scene.misses[ROW_DRAW] == 0);
	check(
		"instancing: every frame, vkCmdDrawIndexed draws 3 from instance 2, gl_InstanceIndex and the attributes "
		"both starting there",
		frames == FRAMES && scene.misses[ROW_INDEXED] == 0);
	check("instancing: every frame, vkCmdDrawIndirect draws the 4 instances from instance 3 it reads at replay",
		frames == FRAMES && scene.misses[ROW_INDIRECT] == 0);
	check("instancing: every frame, vkCmdDrawIndexedIndirect draws the 2 instances from instance 5 it reads at replay",
		frames == FRAMES && scene.misses[ROW_INDEXED_INDIRECT] == 0);
	check(
		"instancing: every frame, a shader without gl_InstanceIndex places 4 instances from instance 1 by their "
		"attributes alone",
		frames == FRAMES && scene.misses[ROW_PLACES] == 0);
	check("instancing: the image on screen shows them too", harness_screen(harness, rows_hold, &scene));
	destroy(harness, &scene);
}
