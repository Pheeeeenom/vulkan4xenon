#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// gl_FrontFacing: quads clockwise and counter-clockwise as they look, nothing culled, each pixel coloured by the face
// it's on under the pipeline's frontFace, and back faces discarded (milestone 22).

enum {
	FRAMES = 3,
	CELL_SIZE = 160,
	CELL_STEP = 200,
	CELL_LEFT = 80,
	CELL_TOP = 280,
	CELLS = 6,
	PIPELINES = 3,
	GROUPS = 3,
	CELLS_A_GROUP = 2,
	TOLERANCE = 2,
};

typedef struct {
	const char *name;
	const char *passes;
} Group;

// Two cells a pipeline: the counter-clockwise frontFace's, the clockwise one's, and the discard's.
static const Group groups[GROUPS] = {
	{ "facing, frontFace counter-clockwise",
		"facing: under a counter-clockwise frontFace, gl_FrontFacing holds on the counter-clockwise quad alone" },
	{ "facing, frontFace clockwise",
		"facing: under a clockwise frontFace, gl_FrontFacing holds on the clockwise quad alone" },
	{ "facing, back faces discarded",
		"facing: a discard on !gl_FrontFacing drops the back face and keeps the front, with no interpolator" },
};

static const VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
static const Rgb clear_rgb = { 0, 0, 0 };

typedef struct {
	// Its pipeline, whether it's mirrored, counter-clockwise as it looks, its colour on a front face, and the colour it
	// should show: that, facing.frag's blue for a back face, or the clear colour where it's discarded.
	uint32_t pipeline;
	bool mirrored;
	float color[4];
	Rgb want;
} Cell;

static const Cell cells[CELLS] = {
	{ 0, false, { 0.9f, 0.2f, 0.2f, 1.0f }, { 26, 77, 230 } },
	{ 0, true, { 0.9f, 0.2f, 0.2f, 1.0f }, { 230, 51, 51 } },
	{ 1, false, { 0.2f, 0.8f, 0.2f, 1.0f }, { 51, 204, 51 } },
	{ 1, true, { 0.2f, 0.8f, 0.2f, 1.0f }, { 26, 77, 230 } },
	{ 2, false, { 0.9f, 0.8f, 0.1f, 1.0f }, { 230, 204, 26 } },
	{ 2, true, { 0.9f, 0.8f, 0.1f, 1.0f }, { 0, 0, 0 } },
};

typedef struct {
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipelines[PIPELINES];
	VkBuffer quad;
	VkDeviceMemory quad_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[GROUPS][CELLS_A_GROUP + 1];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

static void make_probes(Scene *scene)
{
	for (uint32_t g = 0; g < GROUPS; g++) {
		for (uint32_t i = 0; i < CELLS_A_GROUP; i++) {
			const uint32_t c = g * CELLS_A_GROUP + i;
			scene->probes[g][i] =
				(HarnessProbe){ cells[c].mirrored ? "the counter-clockwise quad" : "the clockwise quad",
					CELL_LEFT + c * CELL_STEP + CELL_SIZE / 2, CELL_TOP + CELL_SIZE / 2, cells[c].want, 0 };
		}
		scene->checks[g] = (HarnessProbes){ groups[g].name, scene->probes[g], CELLS_A_GROUP, TOLERANCE, 0 };
	}
	scene->probes[0][CELLS_A_GROUP] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->checks[0].count = CELLS_A_GROUP + 1;
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

// The first two draw facing.frag, past facing.vert's interpolator, under each frontFace; the third discards back faces
// behind push.vert, which has none.
static VkResult create_pipeline(const Harness *harness, Scene *scene, uint32_t i)
{
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = i == 0 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const HarnessPipeline spec = {
		.vertex_spirv = i < 2 ? facing_vert_spirv : push_vert_spirv,
		.vertex_bytes = i < 2 ? FACING_VERT_SPIRV_BYTES : PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = i < 2 ? facing_frag_spirv : facing_discard_frag_spirv,
		.fragment_bytes = i < 2 ? FACING_FRAG_SPIRV_BYTES : FACING_DISCARD_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
		.raster = &raster,
	};
	const VkResult result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[i]);
	if (result != VK_SUCCESS)
		xsdk_log_write("facing: pipeline %u: %d", (unsigned)i, (int)result);
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("layout", harness_push_layout(harness, &scene->layout)) &&
		harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers)) &&
		harness_ok("quad", harness_quad_buffer(harness, &scene->quad, &scene->quad_memory));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, i));
	return ok;
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
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->quad, &offset);
		for (uint32_t c = 0; c < CELLS; c++) {
			const Cell *const cell = &cells[c];
			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[cell->pipeline]);
			const HarnessQuad quad = { .x = (float)(CELL_LEFT + c * CELL_STEP),
				.y = CELL_TOP,
				.width = CELL_SIZE,
				.height = CELL_SIZE,
				.color = { cell->color[0], cell->color[1], cell->color[2], cell->color[3] },
				.mirrored = cell->mirrored };
			harness_push_quad(commands, scene->layout, &quad);
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
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyBuffer(device, scene->quad, NULL);
	vkFreeMemory(device, scene->quad_memory, NULL);
}

void test_facing(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("facing: three pipelines reading gl_FrontFacing, nothing culled", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	for (uint32_t g = 0; g < GROUPS; g++)
		check(groups[g].passes, frames == FRAMES && scene.misses[g] == 0);
	destroy(harness, &scene);
}
