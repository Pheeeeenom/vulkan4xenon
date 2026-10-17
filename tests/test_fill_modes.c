#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 26's step 4: quads drawn with polygonMode LINE and POINT, their two triangles' edges and corners alone,
// and a back face culled before its edges are drawn.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// Lines and points land within a pixel of where they're put.
	SLACK = 1,
	CELL_Y = 100,
	CELL_HEIGHT = 160,
	MIDDLE_Y = CELL_Y + CELL_HEIGHT / 2,
	BOTTOM_Y = CELL_Y + CELL_HEIGHT,
	WIRE_X = 40,
	POINTS_X = 400,
	CULLED_X = 760,
	FRONT_X = 1040,
	WIDE = 300,
	NARROW = 200,
};

typedef enum {
	PIPELINE_LINE,
	PIPELINE_POINT,
	PIPELINE_LINE_CULL_BACK,
	PIPELINES,
} PipelineKind;

static const VkClearValue clear = { .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } };

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const float yellow[4] = RGBA(224, 224, 64);
static const float cyan[4] = RGBA(64, 224, 224);
static const float magenta[4] = RGBA(224, 64, 224);

// The quads' first triangles are their top left, top right and bottom left, so the two share the diagonal from the
// bottom left to the top right, through the centre.
static const HarnessProbe wire_probes[] = {
	{ "LINE: the top edge", WIRE_X + WIDE / 2, CELL_Y, { 224, 224, 64 }, SLACK },
	{ "LINE: the bottom edge", WIRE_X + WIDE / 2, BOTTOM_Y, { 224, 224, 64 }, SLACK },
	{ "LINE: the left edge", WIRE_X, MIDDLE_Y, { 224, 224, 64 }, SLACK },
	{ "LINE: the right edge", WIRE_X + WIDE, MIDDLE_Y, { 224, 224, 64 }, SLACK },
	{ "LINE: the diagonal", WIRE_X + WIDE / 2, MIDDLE_Y, { 224, 224, 64 }, SLACK },
	{ "LINE: inside, above the diagonal", WIRE_X + 40, CELL_Y + 30, { 20, 20, 28 }, 0 },
	{ "LINE: inside, below the diagonal", WIRE_X + WIDE - 40, BOTTOM_Y - 30, { 20, 20, 28 }, 0 },
};

static const HarnessProbe point_probes[] = {
	{ "POINT: the top left corner", POINTS_X, CELL_Y, { 64, 224, 224 }, SLACK },
	{ "POINT: the top right corner", POINTS_X + WIDE, CELL_Y, { 64, 224, 224 }, SLACK },
	{ "POINT: the bottom left corner", POINTS_X, BOTTOM_Y, { 64, 224, 224 }, SLACK },
	{ "POINT: the bottom right corner", POINTS_X + WIDE, BOTTOM_Y, { 64, 224, 224 }, SLACK },
	{ "POINT: nothing above the top edge's middle", POINTS_X + WIDE / 2, CELL_Y - 1, { 20, 20, 28 }, 0 },
	{ "POINT: nothing on the top edge's middle", POINTS_X + WIDE / 2, CELL_Y, { 20, 20, 28 }, 0 },
	{ "POINT: nothing below the top edge's middle", POINTS_X + WIDE / 2, CELL_Y + 1, { 20, 20, 28 }, 0 },
	{ "POINT: nothing on the diagonal", POINTS_X + WIDE / 2, MIDDLE_Y, { 20, 20, 28 }, 0 },
};

static const HarnessProbe cull_probes[] = {
	{ "a culled back face: nothing above its top edge", CULLED_X + NARROW / 2, CELL_Y - 1, { 20, 20, 28 }, 0 },
	{ "a culled back face: nothing on its top edge", CULLED_X + NARROW / 2, CELL_Y, { 20, 20, 28 }, 0 },
	{ "a culled back face: nothing below its top edge", CULLED_X + NARROW / 2, CELL_Y + 1, { 20, 20, 28 }, 0 },
	{ "a culled back face: nothing on its left edge", CULLED_X, MIDDLE_Y, { 20, 20, 28 }, 0 },
	{ "a culled back face: nothing on its diagonal", CULLED_X + NARROW / 2, MIDDLE_Y, { 20, 20, 28 }, 0 },
	{ "a front face beside it: its top edge", FRONT_X + NARROW / 2, CELL_Y, { 224, 64, 224 }, SLACK },
	{ "a front face beside it: its diagonal", FRONT_X + NARROW / 2, MIDDLE_Y, { 224, 64, 224 }, SLACK },
};

enum {
	GROUP_WIRE,
	GROUP_POINTS,
	GROUP_CULL,
	GROUPS,
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = kind == PIPELINE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_LINE,
		.cullMode = kind == PIPELINE_LINE_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
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
		.raster = &raster,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	return ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
}

// A quad whose corners sit on pixels' centres, so its edges and corners land on whole pixels; mirrored, a back face.
static void draw_quad(VkCommandBuffer commands, const Scene *scene, PipelineKind kind, uint32_t x, uint32_t width,
	bool mirrored, const float color[4])
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[kind]);
	HarnessQuad quad = { x + 0.5f, CELL_Y + 0.5f, width, CELL_HEIGHT, 0.0f, { color[0], color[1], color[2], color[3] },
		mirrored };
	harness_push_quad(commands, scene->layout, &quad);
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
		draw_quad(commands, scene, PIPELINE_LINE, WIRE_X, WIDE, false, yellow);
		draw_quad(commands, scene, PIPELINE_POINT, POINTS_X, WIDE, false, cyan);
		draw_quad(commands, scene, PIPELINE_LINE_CULL_BACK, CULLED_X, NARROW, true, magenta);
		draw_quad(commands, scene, PIPELINE_LINE_CULL_BACK, FRONT_X, NARROW, false, magenta);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// Every group's probes, each group's misses counted apart.
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

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_fill_modes(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	check("fill modes: fillModeNonSolid reported", features.fillModeNonSolid);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.checks[GROUP_WIRE] =
		(HarnessProbes){ "fill modes", wire_probes, sizeof wire_probes / sizeof wire_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_POINTS] =
		(HarnessProbes){ "fill modes", point_probes, sizeof point_probes / sizeof point_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_CULL] =
		(HarnessProbes){ "fill modes", cull_probes, sizeof cull_probes / sizeof cull_probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("fill modes: a render pass and three pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("fill modes: 30 frames drawn and presented", frames == FRAMES);
	check(
		"fill modes: every frame's LINE quad draws its triangles' edges, the diagonal between them too, and nothing "
		"inside",
		frames == FRAMES && scene.misses[GROUP_WIRE] == 0);
	check("fill modes: every frame's POINT quad draws its corners and nothing between them",
		frames == FRAMES && scene.misses[GROUP_POINTS] == 0);
	check(
		"fill modes: every frame culls a back-facing LINE quad before its edges are drawn, and draws a front-facing "
		"one's",
		frames == FRAMES && scene.misses[GROUP_CULL] == 0);
	destroy(harness, &scene);
}
