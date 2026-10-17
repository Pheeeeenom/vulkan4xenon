#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 26's step 1: front and back faces each with stencil of their own, set by the pipeline and by commands for
// one face at a time. Each cell is marked by a front face on its left and a back face on its right, then a front face
// is drawn over all of it in one colour and a back face in another; each shows only over its own face's mark.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	CELL_Y = 100,
	CELL_WIDTH = 360,
	HALF_WIDTH = CELL_WIDTH / 2,
	CELL_HEIGHT = 180,
	PROBE_Y = CELL_Y + CELL_HEIGHT / 2,
	// Stencil clears to this, so a face that writes nothing, or tests against 0, shows.
	STENCIL_CLEAR = 0x80,
	// The pipelines' own marks: the front replaces with 0x11; the back inverts the clear through the write mask 0x2F,
	// 0xAF.
	STATIC_FRONT = 0x11,
	STATIC_BACK_WRITE_MASK = 0x2F,
	// The static test's back reference and compare mask: 0x0F of 0x3F matches 0x0F of 0xAF, and all of it doesn't.
	STATIC_BACK_REFERENCE = 0x3F,
	STATIC_BACK_COMPARE_MASK = 0x0F,
	// Values the dynamic pipelines' own state holds, which the commands' must replace.
	IGNORED_REFERENCE = 0x77,
	IGNORED_MASK = 0x01,
	// Commands mark the front 0x05, and the back 0xFA through the write mask 0x0F over the clear, 0x8A; then test each
	// face for its own.
	DYNAMIC_FRONT = 0x05,
	DYNAMIC_BACK_REFERENCE = 0xFA,
	DYNAMIC_BACK_WRITE_MASK = 0x0F,
	DYNAMIC_BACK = 0x8A,
	// Over the static marks, 0x10 of 0x1E through 0xF0 matches the front's 0x11, and 0x0F of 0xEF through 0x0F the
	// back's 0xAF; each face's mask swapped for the other's, neither matches.
	COMPARE_FRONT_MASK = 0xF0,
	COMPARE_FRONT_REFERENCE = 0x1E,
	COMPARE_BACK_MASK = 0x0F,
	COMPARE_BACK_REFERENCE = 0xEF,
};

typedef enum {
	// Marks with each face's own op, reference and write mask, without colour.
	PIPELINE_MARK,
	// Draws where each face's own reference matches through its own compare mask.
	PIPELINE_TEST,
	// Marks with the references and write masks commands set.
	PIPELINE_DYNAMIC_MARK,
	// Tests against the references commands set.
	PIPELINE_DYNAMIC_REFERENCE,
	// Tests through the compare masks and against the references commands set.
	PIPELINE_DYNAMIC_COMPARE,
	PIPELINES,
} PipelineKind;

static const VkDynamicState mark_states[] = { VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
static const VkDynamicState reference_states[] = { VK_DYNAMIC_STATE_STENCIL_REFERENCE };
static const VkDynamicState compare_states[] = { VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
	VK_DYNAMIC_STATE_STENCIL_REFERENCE };

static const VkClearValue clears[2] = {
	{ .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } },
	{ .depthStencil = { 1.0f, STENCIL_CLEAR } },
};

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const float white[4] = RGBA(255, 255, 255);

// A cell: where it is, the pipelines that mark and test it, and the colours its front and back faces draw over it.
typedef struct {
	uint32_t x;
	PipelineKind mark;
	PipelineKind test;
	float front_color[4];
	float back_color[4];
} Cell;

static const Cell cells[] = {
	{ 40, PIPELINE_MARK, PIPELINE_TEST, RGBA(64, 224, 64), RGBA(64, 64, 224) },
	{ 460, PIPELINE_DYNAMIC_MARK, PIPELINE_DYNAMIC_REFERENCE, RGBA(224, 224, 64), RGBA(224, 64, 224) },
	{ 880, PIPELINE_MARK, PIPELINE_DYNAMIC_COMPARE, RGBA(64, 224, 224), RGBA(240, 128, 0) },
};

enum {
	CELLS = sizeof cells / sizeof cells[0],
};

static const HarnessProbe static_probes[] = {
	{ "pipeline: the front face over its own mark", 130, PROBE_Y, { 64, 224, 64 }, 0 },
	{ "pipeline: the back face over its own mark", 310, PROBE_Y, { 64, 64, 224 }, 0 },
	{ "pipeline: nothing below the cell", 220, CELL_Y + CELL_HEIGHT + 20, { 20, 20, 28 }, 0 },
};

static const HarnessProbe reference_probes[] = {
	{ "references and write masks by face: the front face over its mark", 550, PROBE_Y, { 224, 224, 64 }, 0 },
	{ "references and write masks by face: the back face over its mark", 730, PROBE_Y, { 224, 64, 224 }, 0 },
};

static const HarnessProbe compare_probes[] = {
	{ "compare masks by face: the front face over its mark", 970, PROBE_Y, { 64, 224, 224 }, 0 },
	{ "compare masks by face: the back face over its mark", 1150, PROBE_Y, { 240, 128, 0 }, 0 },
};

enum {
	GROUP_STATIC,
	GROUP_REFERENCE,
	GROUP_COMPARE,
	GROUPS,
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

static VkStencilOpState stencil_op(
	VkCompareOp compare, VkStencilOp pass, uint32_t reference, uint32_t compare_mask, uint32_t write_mask)
{
	return (VkStencilOpState){
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = pass,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = compare,
		.compareMask = compare_mask,
		.writeMask = write_mask,
		.reference = reference,
	};
}

// Each kind's front face, then its back.
static void faces_of(PipelineKind kind, VkStencilOpState *front, VkStencilOpState *back)
{
	switch (kind) {
	case PIPELINE_MARK:
		*front = stencil_op(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, STATIC_FRONT, 0xFF, 0xFF);
		*back = stencil_op(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_INVERT, 0, 0xFF, STATIC_BACK_WRITE_MASK);
		return;
	case PIPELINE_TEST:
		*front = stencil_op(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, STATIC_FRONT, 0xFF, 0);
		*back = stencil_op(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, STATIC_BACK_REFERENCE, STATIC_BACK_COMPARE_MASK, 0);
		return;
	case PIPELINE_DYNAMIC_MARK:
		*front = stencil_op(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, IGNORED_REFERENCE, 0xFF, IGNORED_MASK);
		*back = *front;
		return;
	case PIPELINE_DYNAMIC_REFERENCE:
		*front = stencil_op(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, IGNORED_REFERENCE, 0xFF, 0);
		*back = *front;
		return;
	default:
		*front = stencil_op(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, IGNORED_REFERENCE, IGNORED_MASK, 0);
		*back = *front;
		return;
	}
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	VkStencilOpState front;
	VkStencilOpState back;
	faces_of(kind, &front, &back);
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.stencilTestEnable = VK_TRUE,
		.front = front,
		.back = back,
	};
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const bool marks = kind == PIPELINE_MARK || kind == PIPELINE_DYNAMIC_MARK;
	const VkPipelineColorBlendAttachmentState blend = {
		.colorWriteMask = marks
			? 0
			: VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	const VkDynamicState *dynamic = NULL;
	uint32_t dynamic_count = 0;
	if (kind == PIPELINE_DYNAMIC_MARK) {
		dynamic = mark_states;
		dynamic_count = sizeof mark_states / sizeof mark_states[0];
	} else if (kind == PIPELINE_DYNAMIC_REFERENCE) {
		dynamic = reference_states;
		dynamic_count = sizeof reference_states / sizeof reference_states[0];
	} else if (kind == PIPELINE_DYNAMIC_COMPARE) {
		dynamic = compare_states;
		dynamic_count = sizeof compare_states / sizeof compare_states[0];
	}
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
		.depth_stencil = &depth_stencil,
		.blend = &blend,
		.dynamic_states = dynamic,
		.dynamic_state_count = dynamic_count,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok &&
		harness_ok(
			"depth image", harness_depth_image(harness, &scene->depth, &scene->depth_memory, &scene->depth_view));
	ok = ok &&
		harness_ok("render pass",
			harness_depth_render_pass(harness, scene->depth_view, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	return ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
}

static void bind(VkCommandBuffer commands, const Scene *scene, PipelineKind kind)
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[kind]);
}

// A front face, clockwise as it looks, or mirrored into a back face.
static void draw_quad(
	VkCommandBuffer commands, const Scene *scene, float x, float width, bool back, const float color[4])
{
	HarnessQuad quad = { x, CELL_Y, width, CELL_HEIGHT, 0.0f, { color[0], color[1], color[2], color[3] }, back };
	harness_push_quad(commands, scene->layout, &quad);
}

// What commands set for each face alone, before the cell's mark and test draw.
static void set_mark_state(VkCommandBuffer commands, const Cell *cell)
{
	if (cell->mark != PIPELINE_DYNAMIC_MARK)
		return;
	vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_BIT, DYNAMIC_FRONT);
	vkCmdSetStencilReference(commands, VK_STENCIL_FACE_BACK_BIT, DYNAMIC_BACK_REFERENCE);
	vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_BIT, 0xFF);
	vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_BACK_BIT, DYNAMIC_BACK_WRITE_MASK);
}

static void set_test_state(VkCommandBuffer commands, const Cell *cell)
{
	if (cell->test == PIPELINE_DYNAMIC_REFERENCE) {
		vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_BIT, DYNAMIC_FRONT);
		vkCmdSetStencilReference(commands, VK_STENCIL_FACE_BACK_BIT, DYNAMIC_BACK);
	} else if (cell->test == PIPELINE_DYNAMIC_COMPARE) {
		vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_BIT, COMPARE_FRONT_MASK);
		vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_BACK_BIT, COMPARE_BACK_MASK);
		vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_BIT, COMPARE_FRONT_REFERENCE);
		vkCmdSetStencilReference(commands, VK_STENCIL_FACE_BACK_BIT, COMPARE_BACK_REFERENCE);
	}
}

static void record_cell(VkCommandBuffer commands, const Scene *scene, const Cell *cell)
{
	set_mark_state(commands, cell);
	bind(commands, scene, cell->mark);
	draw_quad(commands, scene, cell->x, HALF_WIDTH, false, white);
	draw_quad(commands, scene, cell->x + HALF_WIDTH, HALF_WIDTH, true, white);
	bind(commands, scene, cell->test);
	set_test_state(commands, cell);
	draw_quad(commands, scene, cell->x, CELL_WIDTH, false, cell->front_color);
	draw_quad(commands, scene, cell->x, CELL_WIDTH, true, cell->back_color);
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
			.clearValueCount = 2,
			.pClearValues = clears,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t c = 0; c < CELLS; c++)
			record_cell(commands, scene, &cells[c]);
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
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
}

void test_two_sided_stencil(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.checks[GROUP_STATIC] = (HarnessProbes){ "two-sided stencil", static_probes,
		sizeof static_probes / sizeof static_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_REFERENCE] = (HarnessProbes){ "two-sided stencil", reference_probes,
		sizeof reference_probes / sizeof reference_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_COMPARE] = (HarnessProbes){ "two-sided stencil", compare_probes,
		sizeof compare_probes / sizeof compare_probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("two-sided stencil: a D24S8 attachment, a render pass and five pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("two-sided stencil: 30 frames drawn and presented", frames == FRAMES);
	check(
		"two-sided stencil: every frame's front and back faces pass the pipeline's own test for each, over what each "
		"wrote with its own op and write mask",
		frames == FRAMES && scene.misses[GROUP_STATIC] == 0);
	check("two-sided stencil: every frame's faces take the references and write masks commands set for each alone",
		frames == FRAMES && scene.misses[GROUP_REFERENCE] == 0);
	check("two-sided stencil: every frame's faces take the compare masks commands set for each alone",
		frames == FRAMES && scene.misses[GROUP_COMPARE] == 0);
	destroy(harness, &scene);
}
