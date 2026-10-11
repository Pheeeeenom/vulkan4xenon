#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	TOLERANCE = 2,
};

typedef enum {
	PIPELINE_OPAQUE,
	// Culling back faces, front faces clockwise as they look.
	PIPELINE_CULL_BACK,
	PIPELINE_CULL_FRONT,
	// Culling back faces, front faces counter-clockwise.
	PIPELINE_CULL_BACK_CCW,
	PIPELINE_ALPHA,
	PIPELINE_ADD,
	PIPELINE_SUBTRACT,
	PIPELINE_REVERSE_SUBTRACT,
	PIPELINE_MIN,
	PIPELINE_MAX,
	// Writes red and blue only.
	PIPELINE_MASK,
	// The viewport and scissor come from the command buffer.
	PIPELINE_DYNAMIC,
	PIPELINES,
} PipelineKind;

typedef enum {
	SET_NOTHING,
	SET_VIEWPORT_A,
	SET_VIEWPORT_B,
} DynamicSetting;

#define RGBA(r, g, b, a) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a) }

typedef struct {
	PipelineKind pipeline;
	DynamicSetting setting;
	HarnessQuad quad;
} Draw;

static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 40.0f / 255, 1.0f } } };

// The base under each blend.
#define BASE RGBA(96, 160, 192, 1.0f)

static const Draw draws[] = {
	// Culling: unmirrored quads are clockwise as they look; mirrored ones counter-clockwise.
	{ PIPELINE_CULL_BACK, SET_NOTHING, { 40, 40, 120, 150, 0.0f, RGBA(224, 64, 64, 1.0f), false } },
	{ PIPELINE_CULL_BACK, SET_NOTHING, { 190, 40, 120, 150, 0.0f, RGBA(64, 224, 64, 1.0f), true } },
	{ PIPELINE_CULL_FRONT, SET_NOTHING, { 340, 40, 120, 150, 0.0f, RGBA(64, 64, 224, 1.0f), false } },
	{ PIPELINE_CULL_FRONT, SET_NOTHING, { 490, 40, 120, 150, 0.0f, RGBA(224, 224, 64, 1.0f), true } },
	{ PIPELINE_CULL_BACK_CCW, SET_NOTHING, { 640, 40, 120, 150, 0.0f, RGBA(64, 224, 224, 1.0f), false } },
	{ PIPELINE_CULL_BACK_CCW, SET_NOTHING, { 790, 40, 120, 150, 0.0f, RGBA(224, 64, 224, 1.0f), true } },
	// Blends: each a base, then an overlay inside it.
	{ PIPELINE_OPAQUE, SET_NOTHING, { 40, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_ALPHA, SET_NOTHING, { 70, 290, 90, 90, 0.0f, RGBA(255, 128, 0, 0.5f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 210, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_ADD, SET_NOTHING, { 240, 290, 90, 90, 0.0f, RGBA(64, 128, 96, 1.0f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 380, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_SUBTRACT, SET_NOTHING, { 410, 290, 90, 90, 0.0f, RGBA(255, 192, 224, 1.0f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 550, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_REVERSE_SUBTRACT, SET_NOTHING, { 580, 290, 90, 90, 0.0f, RGBA(32, 64, 96, 1.0f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 720, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_MIN, SET_NOTHING, { 750, 290, 90, 90, 0.0f, RGBA(128, 64, 255, 1.0f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 890, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_MAX, SET_NOTHING, { 920, 290, 90, 90, 0.0f, RGBA(128, 64, 255, 1.0f), false } },
	{ PIPELINE_OPAQUE, SET_NOTHING, { 1060, 260, 150, 150, 0.0f, BASE, false } },
	{ PIPELINE_MASK, SET_NOTHING, { 1090, 290, 90, 90, 0.0f, RGBA(255, 0, 255, 1.0f), false } },
	// Dynamic viewport and scissor: all of clip space fills the viewport, then only the scissor of a second one.
	{ PIPELINE_DYNAMIC, SET_VIEWPORT_A, { 0, 0, 1280, 720, 0.0f, RGBA(240, 128, 0, 1.0f), false } },
	{ PIPELINE_DYNAMIC, SET_VIEWPORT_B, { 0, 0, 1280, 720, 0.0f, RGBA(0, 192, 160, 1.0f), false } },
	// Edges a quarter pixel past whole pixels: Vulkan's centres on halves cover 600 to 699; Direct3D 9's wouldn't.
	{ PIPELINE_OPAQUE, SET_NOTHING, { 600.25f, 500.25f, 100, 100, 0.0f, RGBA(255, 255, 255, 1.0f), false } },
};

static const VkViewport viewport_a = { 40, 480, 160, 120, 0.0f, 1.0f };
static const VkRect2D scissor_a = { { 0, 0 }, { 1280, 720 } };
static const VkViewport viewport_b = { 240, 480, 200, 150, 0.0f, 1.0f };
static const VkRect2D scissor_b = { { 280, 510 }, { 60, 50 } };

static const HarnessProbe probes[] = {
	{ "cull back: the clockwise quad draws", 100, 115, { 224, 64, 64 }, 0 },
	{ "cull back: the counter-clockwise quad doesn't", 250, 115, { 24, 24, 40 }, 0 },
	{ "cull front: the clockwise quad doesn't draw", 400, 115, { 24, 24, 40 }, 0 },
	{ "cull front: the counter-clockwise quad does", 550, 115, { 224, 224, 64 }, 0 },
	{ "front counter-clockwise, cull back: the clockwise quad doesn't draw", 700, 115, { 24, 24, 40 }, 0 },
	{ "front counter-clockwise, cull back: the counter-clockwise quad does", 850, 115, { 224, 64, 224 }, 0 },
	{ "alpha: half orange over the base", 115, 335, { 176, 144, 96 }, 0 },
	{ "additive: green and blue clamp", 285, 335, { 160, 255, 255 }, 0 },
	{ "subtract: the overlay less the base", 455, 335, { 159, 32, 32 }, 0 },
	{ "reverse subtract: the base less the overlay", 625, 335, { 64, 96, 96 }, 0 },
	{ "min", 795, 335, { 96, 64, 192 }, 0 },
	{ "max", 965, 335, { 128, 160, 255 }, 0 },
	{ "write mask: red and blue only", 1135, 335, { 255, 160, 255 }, 0 },
	{ "the base beside an overlay", 50, 270, { 96, 160, 192 }, 0 },
	{ "viewport: its top left", 42, 482, { 240, 128, 0 }, 0 },
	{ "viewport: its bottom right", 197, 597, { 240, 128, 0 }, 0 },
	{ "viewport: nothing left of it", 37, 540, { 24, 24, 40 }, 0 },
	{ "viewport: nothing right of it", 203, 540, { 24, 24, 40 }, 0 },
	{ "viewport: nothing above it", 120, 477, { 24, 24, 40 }, 0 },
	{ "viewport: nothing below it", 120, 603, { 24, 24, 40 }, 0 },
	{ "scissor: its top-left pixel", 280, 510, { 0, 192, 160 }, 0 },
	{ "scissor: its bottom-right pixel", 339, 559, { 0, 192, 160 }, 0 },
	{ "scissor: nothing just left of it", 279, 535, { 24, 24, 40 }, 0 },
	{ "scissor: nothing just above it", 310, 509, { 24, 24, 40 }, 0 },
	{ "scissor: nothing just right of it", 340, 535, { 24, 24, 40 }, 0 },
	{ "scissor: nothing just below it", 310, 560, { 24, 24, 40 }, 0 },
	{ "scissor: nothing in the viewport outside it", 250, 490, { 24, 24, 40 }, 0 },
	{ "edges: column 600 covered", 600, 550, { 255, 255, 255 }, 0 },
	{ "edges: column 599 not", 599, 550, { 24, 24, 40 }, 0 },
	{ "edges: column 699 covered", 699, 550, { 255, 255, 255 }, 0 },
	{ "edges: column 700 not", 700, 550, { 24, 24, 40 }, 0 },
	{ "edges: row 500 covered", 650, 500, { 255, 255, 255 }, 0 },
	{ "edges: row 499 not", 650, 499, { 24, 24, 40 }, 0 },
	{ "edges: row 599 covered", 650, 599, { 255, 255, 255 }, 0 },
	{ "edges: row 600 not", 650, 600, { 24, 24, 40 }, 0 },
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbes check;
} Scene;

static VkPipelineColorBlendAttachmentState blend_for(PipelineKind kind)
{
	const VkColorComponentFlags all =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendAttachmentState blend = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.colorWriteMask = all,
	};
	switch (kind) {
	case PIPELINE_ALPHA:
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case PIPELINE_SUBTRACT:
		blend.colorBlendOp = VK_BLEND_OP_SUBTRACT;
		break;
	case PIPELINE_REVERSE_SUBTRACT:
		blend.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
		break;
	case PIPELINE_MIN:
		blend.colorBlendOp = VK_BLEND_OP_MIN;
		break;
	case PIPELINE_MAX:
		blend.colorBlendOp = VK_BLEND_OP_MAX;
		break;
	case PIPELINE_MASK:
		blend = (VkPipelineColorBlendAttachmentState){ .colorWriteMask =
														   VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT };
		break;
	case PIPELINE_ADD:
		break;
	default:
		blend = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = all };
		break;
	}
	return blend;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = kind == PIPELINE_CULL_FRONT                            ? VK_CULL_MODE_FRONT_BIT
			: kind == PIPELINE_CULL_BACK || kind == PIPELINE_CULL_BACK_CCW ? VK_CULL_MODE_BACK_BIT
																		   : VK_CULL_MODE_NONE,
		.frontFace = kind == PIPELINE_CULL_BACK_CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const VkPipelineColorBlendAttachmentState blend = blend_for(kind);
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
		.blend = &blend,
		.dynamic_viewport = kind == PIPELINE_DYNAMIC,
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
		for (uint32_t d = 0; d < sizeof draws / sizeof draws[0]; d++) {
			const Draw *const draw = &draws[d];
			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[draw->pipeline]);
			if (draw->setting != SET_NOTHING) {
				const bool a = draw->setting == SET_VIEWPORT_A;
				vkCmdSetViewport(commands, 0, 1, a ? &viewport_a : &viewport_b);
				vkCmdSetScissor(commands, 0, 1, a ? &scissor_a : &scissor_b);
			}
			harness_push_quad(commands, scene->layout, &draw->quad);
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
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_raster(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.check = (HarnessProbes){ "raster", probes, sizeof probes / sizeof probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("raster: twelve pipelines of culling, blending, write masks and dynamic viewports made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("raster: 60 frames drawn and presented", frames == FRAMES);
	check("raster: every frame culls, blends, masks, follows the dynamic viewport and scissor, and has Vulkan's edges",
		frames == FRAMES && misses == 0);
	check("raster: the image on screen shows them too", harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
