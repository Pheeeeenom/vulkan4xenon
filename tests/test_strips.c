#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 24's strips past the 65535 vertices a draw packet carries, drawn in pieces that share the vertices where
// they meet: a triangle strip's quads at its start, over the split and at its end, back faces culled, directly and from
// 32-bit indices; and a line strip's lines over its split and at its end.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	LINE_RADIUS = 2,
	VERTICES = 70000,
	// Where the strips' pieces meet: a triangle strip's second piece starts at vertex 65532, a line strip's at 65534.
	TRIANGLE_SPLIT = 65532,
	LINE_SPLIT = 65534,
	QUADS = 3,
};

static const float orange[4] = { 240.0f / 255, 128.0f / 255, 0.0f, 1.0f };
static const float cyan[4] = { 0.0f, 192.0f / 255, 224.0f / 255, 1.0f };
static const float yellow[4] = { 240.0f / 255, 224.0f / 255, 64.0f / 255, 1.0f };
static const Rgb orange_rgb = { 240, 128, 0 };
static const Rgb cyan_rgb = { 0, 192, 224 };
static const Rgb yellow_rgb = { 240, 224, 64 };

static const VkClearValue clear = { .color = { .float32 = { 16.0f / 255, 24.0f / 255, 40.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 16, 24, 40 };

// A quad's first vertex in the strip and its left and right in clip space; all three span y -0.8 to -0.5, pixels 72
// to 180, and the indexed strip is drawn 180 pixels lower.
typedef struct {
	uint32_t first;
	float left;
	float right;
} StripQuad;

static const StripQuad quads[QUADS] = {
	{ 0, -0.9f, -0.6f },
	// Its first triangle the first piece's last, its second the second piece's first.
	{ TRIANGLE_SPLIT - 1, -0.25f, 0.05f },
	{ VERTICES - 4, 0.5f, 0.8f },
};

enum {
	QUAD_TOP = 72,
	QUAD_HEIGHT = 108,
	INDEXED_DOWN = 180,
	// The line strip's rows: over the split, and at the end.
	SPLIT_ROW = 460,
	LAST_ROW = 560,
};

typedef enum {
	CHECK_TRIANGLES,
	CHECK_INDEXED,
	CHECK_LINES,
	CHECKS,
} CheckKind;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline triangles;
	VkPipeline lines;
	VkPipeline fan;
	VkBuffer strip;
	VkDeviceMemory strip_memory;
	VkBuffer line_strip;
	VkDeviceMemory line_memory;
	VkBuffer indices;
	VkDeviceMemory index_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[CHECKS][16];
	HarnessProbes checks[CHECKS];
	uint32_t misses[CHECKS];
} Scene;

static void set_vertex(float *vertices, uint32_t index, float x, float y)
{
	float *const v = vertices + index * 4;
	v[0] = x, v[1] = y, v[2] = 0.0f, v[3] = 1.0f;
}

static void copy_vertex(float *vertices, uint32_t to, uint32_t from)
{
	for (uint32_t i = 0; i < 4; i++)
		vertices[to * 4 + i] = vertices[from * 4 + i];
}

// A strip's odd triangles wind the other way from their vertices' order (Vulkan 1.0 spec, "Triangle Strips"), so a quad
// at an odd triangle swaps its corners in pairs; both kinds are counter-clockwise as they look.
static void place_quad(float *vertices, const StripQuad *quad)
{
	const float top = -0.8f;
	const float bottom = -0.5f;
	const bool odd = quad->first % 2;
	set_vertex(vertices, quad->first + (odd ? 1 : 0), quad->left, top);
	set_vertex(vertices, quad->first + (odd ? 0 : 1), quad->left, bottom);
	set_vertex(vertices, quad->first + (odd ? 3 : 2), quad->right, top);
	set_vertex(vertices, quad->first + (odd ? 2 : 3), quad->right, bottom);
}

// Between two quads the vertices are the first's last corner, then the next's first, so no triangle there has area.
static float *make_strip(void)
{
	float *const vertices = calloc(VERTICES * 4, sizeof(float));
	if (!vertices)
		return NULL;
	for (uint32_t q = 0; q < QUADS; q++)
		place_quad(vertices, &quads[q]);
	for (uint32_t q = 0; q + 1 < QUADS; q++) {
		const uint32_t last = quads[q].first + 3;
		copy_vertex(vertices, last + 1, last);
		for (uint32_t i = last + 2; i < quads[q + 1].first; i++)
			copy_vertex(vertices, i, quads[q + 1].first);
	}
	return vertices;
}

// A pixel row's middle in clip space.
static float row_y(uint32_t row)
{
	return ((float)row + 0.5f) / (HARNESS_HEIGHT / 2.0f) - 1.0f;
}

// Every vertex off the screen, beyond its top left or top right corner, but the seven that place the two rows' lines.
static float *make_line_strip(void)
{
	float *const vertices = calloc(VERTICES * 4, sizeof(float));
	if (!vertices)
		return NULL;
	const float off = 1.5f;
	for (uint32_t i = 0; i < VERTICES; i++)
		set_vertex(vertices, i, i < LINE_SPLIT ? -off : off, -off);
	// The first piece's last line from the left edge, the second's first, then on to the right edge.
	set_vertex(vertices, LINE_SPLIT - 1, -off, row_y(SPLIT_ROW));
	set_vertex(vertices, LINE_SPLIT, -0.2f, row_y(SPLIT_ROW));
	set_vertex(vertices, LINE_SPLIT + 1, 0.2f, row_y(SPLIT_ROW));
	set_vertex(vertices, LINE_SPLIT + 2, off, row_y(SPLIT_ROW));
	// The last two lines, from the right edge to the middle and on to the left edge.
	set_vertex(vertices, VERTICES - 3, off, row_y(LAST_ROW));
	set_vertex(vertices, VERTICES - 2, 0.0f, row_y(LAST_ROW));
	set_vertex(vertices, VERTICES - 1, -off, row_y(LAST_ROW));
	return vertices;
}

static uint32_t *make_indices(void)
{
	uint32_t *const indices = malloc(VERTICES * sizeof(uint32_t));
	for (uint32_t i = 0; indices && i < VERTICES; i++)
		indices[i] = i;
	return indices;
}

static uint32_t pixel_x(float x)
{
	return (uint32_t)((x + 1.0f) * (HARNESS_WIDTH / 2.0f));
}

static void add_probe(Scene *scene, CheckKind kind, uint32_t *count, HarnessProbe probe)
{
	scene->probes[kind][(*count)++] = probe;
}

// Each quad's two triangles, each a quarter of the way in from its corner, and nothing between the quads.
static uint32_t strip_probes(Scene *scene, CheckKind kind, uint32_t down, Rgb want)
{
	static const char *const names[QUADS][2] = {
		{ "the first quad's first triangle", "the first quad's second triangle" },
		{ "the first piece's last triangle", "the second piece's first triangle" },
		{ "the last quad's first triangle", "the last quad's second triangle" },
	};
	uint32_t count = 0;
	for (uint32_t q = 0; q < QUADS; q++) {
		const uint32_t left = pixel_x(quads[q].left);
		const uint32_t width = pixel_x(quads[q].right) - left;
		const uint32_t top = QUAD_TOP + down;
		// At an even triangle the first is the upper left half, at an odd one the lower left.
		const bool odd = quads[q].first % 2;
		const HarnessProbe first = { names[q][0], left + width / 4, top + (odd ? 3 : 1) * QUAD_HEIGHT / 4, want, 0 };
		const HarnessProbe second = { names[q][1], left + 3 * width / 4, top + (odd ? 1 : 3) * QUAD_HEIGHT / 4, want,
			0 };
		add_probe(scene, kind, &count, first);
		add_probe(scene, kind, &count, second);
	}
	add_probe(scene, kind, &count,
		(HarnessProbe){ "nothing between the first quads", 360, QUAD_TOP + down + QUAD_HEIGHT / 2, clear_rgb, 0 });
	add_probe(scene, kind, &count,
		(HarnessProbe){ "nothing between the last quads", 816, QUAD_TOP + down + QUAD_HEIGHT / 2, clear_rgb, 0 });
	return count;
}

static void make_probes(Scene *scene)
{
	uint32_t counts[CHECKS];
	counts[CHECK_TRIANGLES] = strip_probes(scene, CHECK_TRIANGLES, 0, orange_rgb);
	counts[CHECK_INDEXED] = strip_probes(scene, CHECK_INDEXED, INDEXED_DOWN, cyan_rgb);
	const HarnessProbe lines[] = {
		{ "the first piece's last line", 256, SPLIT_ROW, yellow_rgb, LINE_RADIUS },
		{ "the second piece's first line", 640, SPLIT_ROW, yellow_rgb, LINE_RADIUS },
		{ "the second piece's second line", 1024, SPLIT_ROW, yellow_rgb, LINE_RADIUS },
		{ "the strip's last line", 320, LAST_ROW, yellow_rgb, LINE_RADIUS },
		{ "the line before it", 960, LAST_ROW, yellow_rgb, LINE_RADIUS },
		{ "nothing between the rows", 640, (SPLIT_ROW + LAST_ROW) / 2, clear_rgb, 0 },
	};
	counts[CHECK_LINES] = 0;
	for (uint32_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
		add_probe(scene, CHECK_LINES, &counts[CHECK_LINES], lines[i]);
	static const char *const tests[CHECKS] = { "triangle strip", "indexed triangle strip", "line strip" };
	for (uint32_t kind = 0; kind < CHECKS; kind++)
		scene->checks[kind] = (HarnessProbes){ tests[kind], scene->probes[kind], counts[kind], TOLERANCE, 0 };
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, VkPrimitiveTopology topology, VkPipeline *out)
{
	const VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = topology,
	};
	// Back faces culled, so a piece whose triangles faced the other way wouldn't draw; lines have no faces.
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
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
		.raster = &raster,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, out);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok &&
		harness_ok("triangle strip pipeline",
			create_pipeline(harness, scene, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, &scene->triangles));
	ok = ok &&
		harness_ok(
			"line strip pipeline", create_pipeline(harness, scene, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, &scene->lines));
	ok = ok &&
		harness_ok(
			"triangle fan pipeline", create_pipeline(harness, scene, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, &scene->fan));
	float *const strip = make_strip();
	float *const line_strip = make_line_strip();
	uint32_t *const indices = make_indices();
	const uint32_t vertex_bytes = VERTICES * 4 * sizeof(float);
	ok = ok && strip && line_strip && indices;
	ok = ok &&
		harness_ok("triangle strip",
			harness_buffer(
				harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, strip, vertex_bytes, &scene->strip, &scene->strip_memory));
	ok = ok &&
		harness_ok("line strip",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, line_strip, vertex_bytes, &scene->line_strip,
				&scene->line_memory));
	ok = ok &&
		harness_ok("indices",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, VERTICES * sizeof(uint32_t),
				&scene->indices, &scene->index_memory));
	free(strip);
	free(line_strip);
	free(indices);
	return ok;
}

// push.vert's scale and offset, and push.frag's colour: the vertices as they are, moved down by down pixels.
static void push(VkCommandBuffer commands, VkPipelineLayout layout, uint32_t down, const float color[4])
{
	const float place[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, (float)down / (HARNESS_HEIGHT / 2.0f), 0.0f, 0.0f };
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16, color);
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
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->triangles);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->strip, &offset);
		push(commands, scene->layout, 0, orange);
		vkCmdDraw(commands, VERTICES, 1, 0, 0);
		vkCmdBindIndexBuffer(commands, scene->indices, 0, VK_INDEX_TYPE_UINT32);
		push(commands, scene->layout, INDEXED_DOWN, cyan);
		vkCmdDrawIndexed(commands, VERTICES, 1, 0, 0, 0);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->lines);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->line_strip, &offset);
		push(commands, scene->layout, 0, yellow);
		vkCmdDraw(commands, VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static bool checks_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t kind = 0; kind < CHECKS; kind++) {
		if (!harness_probes_hold(address, what, &scene->checks[kind])) {
			scene->misses[kind]++;
			all = false;
		}
	}
	return all;
}

// A fan that long fails its command buffer, recorded alone; one of 65535 doesn't.
static bool fan_refused(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bool refused = vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->fan);
	vkCmdDraw(commands, VERTICES, 1, 0, 0);
	refused = refused && vkEndCommandBuffer(commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	refused = refused && vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->fan);
	vkCmdDraw(commands, 65535, 1, 0, 0);
	refused = refused && vkEndCommandBuffer(commands) == VK_SUCCESS;
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return refused;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	const VkBuffer buffers[] = { scene->strip, scene->line_strip, scene->indices };
	const VkDeviceMemory memories[] = { scene->strip_memory, scene->line_memory, scene->index_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	vkDestroyPipeline(device, scene->triangles, NULL);
	vkDestroyPipeline(device, scene->lines, NULL);
	vkDestroyPipeline(device, scene->fan, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_strips(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("strips: triangle strip, line strip and fan pipelines made, and strips of 70000 vertices", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, checks_hold, &scene, &misses) : 0;
	check("strips: 30 frames drawn and presented", frames == FRAMES);
	check(
		"strips: every frame, a triangle strip of 70000 vertices, culling back faces, draws its first quad, the quad "
		"over its split at vertex 65532 and its last",
		frames == FRAMES && scene.misses[CHECK_TRIANGLES] == 0);
	check("strips: every frame, the same strip from 70000 32-bit indices draws them too",
		frames == FRAMES && scene.misses[CHECK_INDEXED] == 0);
	check(
		"strips: every frame, a line strip of 70000 vertices draws the lines over its split at vertex 65534 and its "
		"last two",
		frames == FRAMES && scene.misses[CHECK_LINES] == 0);
	check("strips: the image on screen shows them too", harness_screen(harness, checks_hold, &scene));
	check("strips: a triangle fan of 70000 vertices still fails its command buffer, and one of 65535 doesn't",
		built && fan_refused(harness, &scene));
	destroy(harness, &scene);
}
