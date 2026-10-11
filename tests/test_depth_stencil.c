#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	TOLERANCE = 2,
	PROBES = 14,
};

typedef enum {
	// Depth tested less-than and written.
	PIPELINE_DEPTH,
	PIPELINE_DEPTH_NO_WRITE,
	// No depth test; stencil always passes and becomes 1, and no colour is written.
	PIPELINE_MASK,
	// No depth test; draws only where stencil is 1.
	PIPELINE_STENCIL,
	// As PIPELINE_DEPTH, pulled nearer by a constant depth bias.
	PIPELINE_BIAS,
	PIPELINES,
} PipelineKind;

// A depth bias of -0.01, in units of D24's resolution, 2^-24 (Vulkan 1.0 spec, Depth Bias).
static const float bias_units = -0.01f * 16777216.0f;

// Depth clears to 0.5, so only what's nearer shows; stencil to 0.
static const VkClearValue clears[2] = {
	{ .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } },
	{ .depthStencil = { 0.5f, 0 } },
};

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

typedef struct {
	PipelineKind pipeline;
	HarnessQuad quad;
} Draw;

// In draw order. Each cell's comment says what shows where.
static const Draw draws[] = {
	// Back to front: the near green covers the far red where they overlap.
	{ PIPELINE_DEPTH, { 40, 60, 140, 110, 0.4f, RGBA(224, 64, 64), false } },
	{ PIPELINE_DEPTH, { 100, 110, 140, 100, 0.2f, RGBA(64, 224, 64), false } },
	// Front to back: the far red stays out of the near green's area.
	{ PIPELINE_DEPTH, { 360, 110, 140, 100, 0.2f, RGBA(64, 224, 64), false } },
	{ PIPELINE_DEPTH, { 300, 60, 140, 110, 0.4f, RGBA(224, 64, 64), false } },
	// The near blue writes no depth, so the farther yellow still covers it.
	{ PIPELINE_DEPTH_NO_WRITE, { 560, 60, 140, 110, 0.2f, RGBA(64, 64, 224), false } },
	{ PIPELINE_DEPTH, { 620, 110, 140, 100, 0.4f, RGBA(224, 224, 64), false } },
	// Behind the cleared depth of 0.5, red hides; in front of it, green shows.
	{ PIPELINE_DEPTH, { 820, 60, 120, 110, 0.75f, RGBA(224, 64, 64), false } },
	{ PIPELINE_DEPTH, { 960, 60, 120, 110, 0.25f, RGBA(64, 224, 64), false } },
	// Two masks set stencil without colour; magenta then draws only over the first.
	{ PIPELINE_MASK, { 100, 400, 80, 60, 0.0f, RGBA(255, 255, 255), false } },
	{ PIPELINE_MASK, { 300, 400, 60, 60, 0.0f, RGBA(255, 255, 255), false } },
	{ PIPELINE_STENCIL, { 40, 360, 200, 140, 0.0f, RGBA(224, 64, 224), false } },
	// On a blue base at 0.3, an orange at the same depth stays hidden, and a biased one shows.
	{ PIPELINE_DEPTH, { 560, 360, 240, 140, 0.3f, RGBA(64, 64, 224), false } },
	{ PIPELINE_DEPTH, { 580, 380, 90, 100, 0.3f, RGBA(240, 128, 0), false } },
	{ PIPELINE_BIAS, { 690, 380, 90, 100, 0.3f, RGBA(240, 128, 0), false } },
};

static const HarnessProbe probes[PROBES] = {
	{ "back to front: the overlap is the near green", 140, 140, { 64, 224, 64 }, 0 },
	{ "back to front: the far red beyond it", 60, 80, { 224, 64, 64 }, 0 },
	{ "front to back: the overlap is the near green", 400, 140, { 64, 224, 64 }, 0 },
	{ "front to back: the far red beyond it", 320, 80, { 224, 64, 64 }, 0 },
	{ "no depth write: the farther yellow covers the overlap", 660, 140, { 224, 224, 64 }, 0 },
	{ "no depth write: the near blue beyond it", 580, 80, { 64, 64, 224 }, 0 },
	{ "behind the cleared depth: nothing", 880, 115, { 20, 20, 28 }, 0 },
	{ "in front of the cleared depth: green", 1020, 115, { 64, 224, 64 }, 0 },
	{ "stencil: magenta over the first mask", 140, 430, { 224, 64, 224 }, 0 },
	{ "stencil: nothing beside the mask", 60, 380, { 20, 20, 28 }, 0 },
	{ "stencil: the second mask wrote no colour", 330, 430, { 20, 20, 28 }, 0 },
	{ "depth bias: the unbiased orange stays behind", 625, 430, { 64, 64, 224 }, 0 },
	{ "depth bias: the biased orange shows", 735, 430, { 240, 128, 0 }, 0 },
	{ "the clear colour", 20, 700, { 20, 20, 28 }, 0 },
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
	HarnessProbes check;
} Scene;

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkStencilOpState mask = {
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_REPLACE,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.compareMask = 0xFF,
		.writeMask = 0xFF,
		.reference = 1,
	};
	const VkStencilOpState equal = {
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_KEEP,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_EQUAL,
		.compareMask = 0xFF,
		.writeMask = 0,
		.reference = 1,
	};
	const bool depth = kind == PIPELINE_DEPTH || kind == PIPELINE_DEPTH_NO_WRITE || kind == PIPELINE_BIAS;
	const bool stencil = kind == PIPELINE_MASK || kind == PIPELINE_STENCIL;
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = depth,
		.depthWriteEnable = depth && kind != PIPELINE_DEPTH_NO_WRITE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = stencil,
		.front = kind == PIPELINE_MASK ? mask : equal,
		.back = kind == PIPELINE_MASK ? mask : equal,
	};
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.depthBiasEnable = kind == PIPELINE_BIAS,
		.depthBiasConstantFactor = kind == PIPELINE_BIAS ? bias_units : 0.0f,
		.lineWidth = 1.0f,
	};
	const VkPipelineColorBlendAttachmentState no_color = { .colorWriteMask = 0 };
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
		.blend = kind == PIPELINE_MASK ? &no_color : NULL,
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
		for (uint32_t d = 0; d < sizeof draws / sizeof draws[0]; d++) {
			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[draws[d].pipeline]);
			harness_push_quad(commands, scene->layout, &draws[d].quad);
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
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
}

void test_depth_stencil(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.check = (HarnessProbes){ "depth and stencil", probes, PROBES, TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("depth and stencil: a D24S8 attachment, a render pass clearing it and five pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("depth and stencil: 60 frames drawn and presented", frames == FRAMES);
	check(
		"depth and stencil: every frame tests depth both ways, without writing, against the clear, by stencil and "
		"with a bias",
		frames == FRAMES && misses == 0);
	check("depth and stencil: the image on screen shows them too",
		harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
