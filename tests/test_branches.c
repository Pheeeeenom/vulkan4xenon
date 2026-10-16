#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 21's if/else on data: in a vertex shader, in an unrolled loop's body, and around gl_FragDepth. Each arm
// puts its quad, band or depth somewhere of its own, so a branch that takes the wrong arm shows where it shouldn't.

enum {
	FRAMES = 30,
	QUADS = 4,
	QUAD_VERTICES = 6,
	SCREEN_VERTICES = 6,
	MAX_DRAWS = 4,
	MAX_PROBES = 12,
	MAX_PIPELINES = 3,
	// Every scene's push constants: the vertex shader's from byte 0, the fragment shader's from 32.
	PUSH_BYTES = 64,
	PUSH_FLOATS = PUSH_BYTES / 4,
	TOLERANCE = 2,
	// The scenes again from shaders the bundle doesn't hold: their SPIR-V's generator word, its third, changed.
	GENERATOR_WORD = 2,
	GENERATOR_CHANGE = 0x00010000,
};

typedef struct {
	float position[4];
	float color[4];
} Vertex;

typedef struct {
	uint32_t pipeline;
	uint32_t first_vertex;
	uint32_t vertex_count;
	float push[PUSH_FLOATS];
} Draw;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkPipelineLayout layout;
	VkPipeline pipelines[MAX_PIPELINES];
	uint32_t pipeline_count;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	Draw draws[MAX_DRAWS];
	uint32_t draw_count;
	HarnessProbe probes[MAX_PROBES];
	HarnessProbes check;
} Scene;

// The layer compiles it on the console, with its literals and blocks (docs/ROADMAP.md, milestone 13).
static uint32_t compiled_branch_data_vert[BRANCH_DATA_VERT_SPIRV_BYTES / 4];
static bool compiled;

// A dark slate no other test clears to.
static const VkClearValue clears[2] = { { .color = { .float32 = { 24.0f / 255, 40.0f / 255, 56.0f / 255, 1.0f } } },
	{ .depthStencil = { 1.0f, 0 } } };
static const Rgb clear_rgb = { 24, 40, 56 };

static const VkVertexInputBindingDescription vertex_binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription vertex_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color) },
};

static const float unit_quad[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

// Four quads' top left corners in clip space, each 0.3 a side; moved right or down by 0.5, no two land together.
static const float homes[QUADS][2] = { { -0.9f, -0.9f }, { 0.1f, -0.9f }, { -0.9f, 0.1f }, { 0.1f, 0.1f } };
static const float QUAD_SIZE = 0.3f;
static const float ARM_MOVE = 0.5f;

static uint32_t pixel_x(float x)
{
	return (uint32_t)((x + 1.0f) / 2.0f * HARNESS_WIDTH);
}

static uint32_t pixel_y(float y)
{
	return (uint32_t)((y + 1.0f) / 2.0f * HARNESS_HEIGHT);
}

static Rgb rgb_of(const float color[3])
{
	return (Rgb){ (uint32_t)(color[0] * 255.0f + 0.5f), (uint32_t)(color[1] * 255.0f + 0.5f),
		(uint32_t)(color[2] * 255.0f + 0.5f) };
}

static void add_probe(Scene *scene, const char *name, float x, float y, Rgb want)
{
	scene->probes[scene->check.count++] = (HarnessProbe){ name, pixel_x(x), pixel_y(y), want, 0 };
}

// Its colour where the arm it takes puts the quad, and the clear where the other would.
static void probe_quad(Scene *scene, uint32_t quad, bool right, Rgb want)
{
	const float x = homes[quad][0] + QUAD_SIZE / 2;
	const float y = homes[quad][1] + QUAD_SIZE / 2;
	add_probe(scene, "a quad where its arm puts it", right ? x + ARM_MOVE : x, right ? y : y + ARM_MOVE, want);
	add_probe(
		scene, "the clear where the other arm would", right ? x : x + ARM_MOVE, right ? y + ARM_MOVE : y, clear_rgb);
}

// One layout for every scene: its push constants, both stages, 64 bytes.
static bool build_layout(const Harness *harness, Scene *scene)
{
	const VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PUSH_BYTES };
	const VkPipelineLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	return harness_ok("vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &info, NULL, &scene->layout));
}

static bool build_pass(const Harness *harness, Scene *scene, bool depth)
{
	if (!depth)
		return harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	return harness_ok(
			   "depth image", harness_depth_image(harness, &scene->depth, &scene->depth_memory, &scene->depth_view)) &&
		harness_ok("render pass",
			harness_depth_render_pass(harness, scene->depth_view, &scene->render_pass, scene->framebuffers));
}

static bool add_pipeline(const Harness *harness, Scene *scene, const uint32_t *vertex_spirv, uint32_t vertex_bytes,
	const uint32_t *fragment_spirv, uint32_t fragment_bytes, const VkPipelineDepthStencilStateCreateInfo *depth)
{
	const HarnessPipeline spec = {
		.vertex_spirv = vertex_spirv,
		.vertex_bytes = vertex_bytes,
		.fragment_spirv = fragment_spirv,
		.fragment_bytes = fragment_bytes,
		.bindings = &vertex_binding,
		.binding_count = 1,
		.attributes = vertex_attributes,
		.attribute_count = 2,
		.layout = scene->layout,
		.depth_stencil = depth,
	};
	return harness_ok(
		"pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[scene->pipeline_count++]));
}

static bool build_vertices(const Harness *harness, Scene *scene, const Vertex *vertices, uint32_t count)
{
	return harness_ok("vertex buffer",
		harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, count * sizeof *vertices, &scene->vertices,
			&scene->vertex_memory));
}

// A quad over the clip-space rectangle, its colour (u, v, 0, 1) from 0 at the left and top to 1 at the right and
// bottom, so a fragment shader reads where in it a pixel is.
static void rectangle(Vertex *out, float left, float top, float right, float bottom, float z)
{
	for (uint32_t v = 0; v < SCREEN_VERTICES; v++) {
		const float u = unit_quad[v][0];
		const float w = unit_quad[v][1];
		out[v] = (Vertex){ { left + (right - left) * u, top + (bottom - top) * w, z, 1.0f }, { u, w, 0.0f, 1.0f } };
	}
}

// branch_data.vert: each quad's kind in its colour's w takes its arms, the bottom right's && greening the last.
static bool build_data(const Harness *harness, Scene *scene)
{
	static const float colors[QUADS][4] = {
		{ 0.25f, 0.75f, 0.5f, 0.25f },
		{ 0.875f, 0.375f, 0.125f, 0.75f },
		{ 0.5f, 0.25f, 0.75f, 1.0f },
		{ 0.5f, 0.5f, 0.25f, 0.0f },
	};
	static const float tint[4] = { 0.125f, 0.5f, 0.875f, 1.0f };
	Vertex vertices[QUADS * QUAD_VERTICES];
	scene->check = (HarnessProbes){ "branches: data", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t q = 0; q < QUADS; q++) {
		for (uint32_t v = 0; v < QUAD_VERTICES; v++)
			vertices[q * QUAD_VERTICES + v] = (Vertex){ { homes[q][0] + unit_quad[v][0] * QUAD_SIZE,
															homes[q][1] + unit_quad[v][1] * QUAD_SIZE, 0.5f, 1.0f },
				{ colors[q][0], colors[q][1], colors[q][2], colors[q][3] } };
		const bool right = colors[q][3] > 0.5f;
		float want[3] = { 1.0f - colors[q][0], 1.0f - colors[q][1], 1.0f - colors[q][2] };
		if (right)
			memcpy(want, colors[q][3] > 0.875f ? tint : colors[q], sizeof want);
		const float x = homes[q][0] + (right ? ARM_MOVE : 0.0f);
		const float y = homes[q][1] + (right ? 0.0f : ARM_MOVE);
		if (x > 0.0f && y > 0.0f)
			want[1] = 1.0f;
		probe_quad(scene, q, right, rgb_of(want));
	}
	scene->draws[0] = (Draw){ .vertex_count = QUADS * QUAD_VERTICES };
	memcpy(scene->draws[0].push, tint, sizeof tint);
	scene->draw_count = 1;
	return build_layout(harness, scene) && build_pass(harness, scene, false) &&
		add_pipeline(harness, scene, compiled ? compiled_branch_data_vert : branch_data_vert_spirv,
			BRANCH_DATA_VERT_SPIRV_BYTES, triangle_frag_spirv, TRIANGLE_FRAG_SPIRV_BYTES, NULL) &&
		build_vertices(harness, scene, vertices, QUADS * QUAD_VERTICES);
}

// loop_if.frag over the screen: the passes whose threshold a pixel's u passes, each a band, an edge's sides too.
static bool build_loop_data(const Harness *harness, Scene *scene)
{
	static const float step[4] = { 0.25f, 0.125f, 0.0625f, 0.0f };
	static const uint32_t xs[] = { 160, 318, 322, 480, 800, 1120 };
	Vertex vertices[SCREEN_VERTICES];
	rectangle(vertices, -1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
	scene->check = (HarnessProbes){ "branches: data in a loop", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
		const float u = (xs[i] + 0.5f) / HARNESS_WIDTH;
		uint32_t passes = 0;
		for (uint32_t p = 0; p < 4; p++)
			passes += u > p * 0.25f;
		const float want[3] = { passes * step[0], passes * step[1], passes * step[2] + 0.125f * (4 - passes) };
		scene->probes[scene->check.count++] =
			(HarnessProbe){ "a band by the passes its u passes", xs[i], 360, rgb_of(want), 0 };
	}
	scene->draws[0] = (Draw){ .vertex_count = SCREEN_VERTICES };
	memcpy(&scene->draws[0].push[8], step, sizeof step);
	scene->draw_count = 1;
	return build_layout(harness, scene) && build_pass(harness, scene, false) &&
		add_pipeline(harness, scene, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES, loop_if_frag_spirv,
			LOOP_IF_FRAG_SPIRV_BYTES, NULL) &&
		build_vertices(harness, scene, vertices, SCREEN_VERTICES);
}

// depth_if.frag on the left half by u, depth_before.frag on the right by v, then green at depth 0.5 testing less: it
// shows where they wrote 0.75 and hides where they wrote 0.25.
static bool build_depth(const Harness *harness, Scene *scene)
{
	static const VkPipelineDepthStencilStateCreateInfo writes = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_ALWAYS,
	};
	static const VkPipelineDepthStencilStateCreateInfo tests = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	Vertex vertices[3 * SCREEN_VERTICES];
	rectangle(vertices, -1.0f, -1.0f, 0.0f, 1.0f, 0.5f);
	rectangle(vertices + SCREEN_VERTICES, 0.0f, -1.0f, 1.0f, 1.0f, 0.5f);
	rectangle(vertices + 2 * SCREEN_VERTICES, -1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
	for (uint32_t v = 0; v < SCREEN_VERTICES; v++)
		memcpy(vertices[2 * SCREEN_VERTICES + v].color, (const float[4]){ 0.0f, 1.0f, 0.0f, 1.0f }, 4 * sizeof(float));
	scene->check = (HarnessProbes){ "branches: gl_FragDepth", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t d = 0; d < 3; d++)
		scene->draws[d] = (Draw){ .pipeline = d, .first_vertex = d * SCREEN_VERTICES, .vertex_count = SCREEN_VERTICES };
	scene->draw_count = 3;
	add_probe(scene, "depth 0.75 in the else arm, the green over it", -0.75f, 0.0f, (Rgb){ 0, 255, 0 });
	add_probe(scene, "depth 0.25 in the if arm, the red under the green", -0.25f, 0.0f, (Rgb){ 255, 0, 0 });
	add_probe(scene, "depth 0.75 before the if/else, the green over it", 0.5f, -0.5f, (Rgb){ 0, 255, 0 });
	add_probe(scene, "depth 0.25 in its arm, the blue under the green", 0.5f, 0.5f, (Rgb){ 0, 0, 255 });
	return build_layout(harness, scene) && build_pass(harness, scene, true) &&
		add_pipeline(harness, scene, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES, depth_if_frag_spirv,
			DEPTH_IF_FRAG_SPIRV_BYTES, &writes) &&
		add_pipeline(harness, scene, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES, depth_before_frag_spirv,
			DEPTH_BEFORE_FRAG_SPIRV_BYTES, &writes) &&
		add_pipeline(harness, scene, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES, triangle_frag_spirv,
			TRIANGLE_FRAG_SPIRV_BYTES, &tests) &&
		build_vertices(harness, scene, vertices, 3 * SCREEN_VERTICES);
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
			.clearValueCount = scene->depth ? 2 : 1,
			.pClearValues = clears,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t d = 0; d < scene->draw_count; d++) {
			const Draw *const draw = &scene->draws[d];
			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[draw->pipeline]);
			vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				PUSH_BYTES, draw->push);
			vkCmdDraw(commands, draw->vertex_count, 1, draw->first_vertex, 0);
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
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < scene->pipeline_count; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
}

static void run(const Harness *harness, const char *name, bool (*build)(const Harness *, Scene *), const char *holds)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool built = build(harness, &scene);
	char message[160];
	snprintf(message, sizeof message, "branches: %s: pipelines and buffers made", name);
	check(message, built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	snprintf(message, sizeof message, "branches: %s: 30 frames drawn and presented", name);
	check(message, frames == FRAMES);
	snprintf(message, sizeof message, "branches: %s: every frame %s", name, holds);
	check(message, frames == FRAMES && misses == 0);
	snprintf(message, sizeof message, "branches: %s: the image on screen does too", name);
	check(message, harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}

void test_branches(const Harness *harness)
{
	memcpy(compiled_branch_data_vert, branch_data_vert_spirv, sizeof compiled_branch_data_vert);
	compiled_branch_data_vert[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled = true;
	run(harness, "data in a vertex shader, the shader compiled here", build_data,
		"places and colours each quad by the arms its vertices take");
	compiled = false;

	run(harness, "data in a vertex shader", build_data, "places and colours each quad by the arms its vertices take");
	run(harness, "data in a loop's body", build_loop_data, "colours each band by the passes its u passes");
	run(harness, "gl_FragDepth stored in an if/else", build_depth, "shows the green where the arms wrote 0.75 alone");
}
