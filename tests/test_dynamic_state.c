#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 19's step 3: stencil masks and references, depth bias and blend constants set by commands, each pipeline's
// own state ignored where it takes them from commands and kept where it doesn't.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// A depth bias of 0.01, in units of D24's resolution, 2^-24 (Vulkan 1.0 spec, Depth Bias).
	BIAS_UNITS = 167772,
};

typedef enum {
	PIPELINE_PLAIN,
	PIPELINE_DEPTH,
	// As PIPELINE_DEPTH, biased by vkCmdSetDepthBias; its own bias is 0.
	PIPELINE_BIAS,
	// Stencil always passes and becomes the reference, through the write mask, without colour; its own reference is 9
	// and its write mask 1.
	PIPELINE_MASK,
	// Draws where stencil equals the reference through the compare mask; its own reference is 7.
	PIPELINE_TEST,
	// As PIPELINE_MASK, its reference 1 and masks its own.
	PIPELINE_STATIC_MASK,
	// The source and destination mixed by vkCmdSetBlendConstants's colour; its own is 0.
	PIPELINE_BLEND,
	// The same by its own constant alpha, 0.6.
	PIPELINE_STATIC_BLEND,
	PIPELINES,
} PipelineKind;

static const VkDynamicState stencil_states[] = {
	VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
	VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
	VK_DYNAMIC_STATE_STENCIL_REFERENCE,
};
static const VkDynamicState bias_state = VK_DYNAMIC_STATE_DEPTH_BIAS;
static const VkDynamicState blend_state = VK_DYNAMIC_STATE_BLEND_CONSTANTS;

static const float constant_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
static const float first_constants[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
static const float second_constants[4] = { 0.75f, 0.5f, 0.25f, 1.0f };

// Depth clears to 0.5, stencil to 0.
static const VkClearValue clears[2] = {
	{ .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } },
	{ .depthStencil = { 0.5f, 0 } },
};

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const float white[4] = RGBA(255, 255, 255);

// The stencil cells: a mask over the middle of each, then a quad over all of it that shows only over the mask.
typedef struct {
	uint32_t x;
	uint8_t mask_reference;
	uint8_t write_mask;
	uint8_t test_reference;
	uint8_t compare_mask;
	float color[4];
} StencilCell;

static const StencilCell stencil_cells[] = {
	// The write mask keeps 0x0F of 0xFF.
	{ 40, 0xFF, 0x0F, 0x0F, 0xFF, RGBA(64, 224, 64) },
	// Both references 5.
	{ 340, 5, 0xFF, 5, 0xFF, RGBA(64, 64, 224) },
	// 0x35 and 0x05 agree through the compare mask 0x0F.
	{ 640, 0x35, 0xFF, 0x05, 0x0F, RGBA(224, 224, 64) },
	// The static mask writes its own reference 1, though the commands set 0x77 before it.
	{ 940, 0x77, 0xFF, 1, 0xFF, RGBA(224, 64, 224) },
};

enum {
	STENCIL_CELLS = sizeof stencil_cells / sizeof stencil_cells[0],
	STENCIL_Y = 80,
	CELL_WIDTH = 240,
	CELL_HEIGHT = 180,
	MASK_INSET = 40,
	BIAS_Y = 340,
	BLEND_Y = 340,
	STATIC_BLEND_Y = 540,
};

enum {
	GROUP_STENCIL,
	GROUP_BIAS,
	GROUP_BLEND,
	GROUPS,
};

static const HarnessProbe stencil_probes[] = {
	{ "write mask: green over the mask", 160, 170, { 64, 224, 64 }, 0 },
	{ "write mask: nothing beside it", 50, 90, { 20, 20, 28 }, 0 },
	{ "reference: blue over the mask", 460, 170, { 64, 64, 224 }, 0 },
	{ "reference: nothing beside it", 350, 90, { 20, 20, 28 }, 0 },
	{ "compare mask: yellow over the mask", 760, 170, { 224, 224, 64 }, 0 },
	{ "compare mask: nothing beside it", 650, 90, { 20, 20, 28 }, 0 },
	{ "a static reference after a dynamic one: magenta over the mask", 1060, 170, { 224, 64, 224 }, 0 },
	{ "a static reference after a dynamic one: nothing beside it", 950, 90, { 20, 20, 28 }, 0 },
};

static const HarnessProbe bias_probes[] = {
	{ "depth bias pulled nearer: the orange shows", 110, 410, { 240, 128, 0 }, 0 },
	{ "depth bias pushed farther: the blue base stays", 250, 410, { 64, 64, 224 }, 0 },
};

// Overlay times the constant plus base times one less it, channel by channel.
static const HarnessProbe blend_probes[] = {
	{ "first constants: the base alone", 680, 410, { 32, 64, 224 }, 0 },
	{ "first constants: blended", 775, 410, { 80, 128, 80 }, 0 },
	{ "second constants: the base alone", 900, 410, { 32, 64, 224 }, 0 },
	{ "second constants: blended", 995, 410, { 176, 128, 176 }, 0 },
	{ "static constant alpha: the base alone", 80, 610, { 64, 224, 128 }, 0 },
	{ "static constant alpha: blended", 175, 610, { 141, 109, 109 }, 0 },
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

static VkStencilOpState stencil_of(PipelineKind kind)
{
	if (kind == PIPELINE_MASK)
		return stencil_op(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, 9, 0xFF, 0x01);
	if (kind == PIPELINE_STATIC_MASK)
		return stencil_op(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, 1, 0xFF, 0xFF);
	return stencil_op(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, 7, 0xFF, 0);
}

static VkPipelineColorBlendAttachmentState blend_of(PipelineKind kind)
{
	const VkColorComponentFlags all =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	const bool by_alpha = kind == PIPELINE_STATIC_BLEND;
	const bool blends = kind == PIPELINE_BLEND || by_alpha;
	const bool masks = kind == PIPELINE_MASK || kind == PIPELINE_STATIC_MASK;
	return (VkPipelineColorBlendAttachmentState){
		.blendEnable = blends,
		.srcColorBlendFactor = by_alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR,
		.dstColorBlendFactor =
			by_alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = masks ? 0 : all,
	};
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const bool depth = kind == PIPELINE_DEPTH || kind == PIPELINE_BIAS;
	const bool stencil = kind == PIPELINE_MASK || kind == PIPELINE_TEST || kind == PIPELINE_STATIC_MASK;
	const VkStencilOpState op = stencil_of(kind);
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = depth,
		.depthWriteEnable = depth,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = stencil,
		.front = op,
		.back = op,
	};
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.depthBiasEnable = kind == PIPELINE_BIAS,
		.lineWidth = 1.0f,
	};
	const VkPipelineColorBlendAttachmentState blend = blend_of(kind);
	const bool dynamic_stencil = kind == PIPELINE_MASK || kind == PIPELINE_TEST;
	const VkDynamicState *const dynamic = dynamic_stencil ? stencil_states
		: kind == PIPELINE_BIAS                           ? &bias_state
		: kind == PIPELINE_BLEND                          ? &blend_state
														  : NULL;
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
		.dynamic_state_count = dynamic_stencil ? sizeof stencil_states / sizeof stencil_states[0]
			: dynamic                          ? 1
											   : 0,
		.blend_constants = kind == PIPELINE_STATIC_BLEND ? constant_alpha : NULL,
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

static void draw_quad(VkCommandBuffer commands, const Scene *scene, float x, float y, float width, float height,
	float depth, const float color[4])
{
	HarnessQuad quad = { x, y, width, height, depth, { color[0], color[1], color[2], color[3] }, false };
	harness_push_quad(commands, scene->layout, &quad);
}

static void record_stencil(VkCommandBuffer commands, const Scene *scene)
{
	const VkStencilFaceFlags faces = VK_STENCIL_FACE_FRONT_AND_BACK;
	for (uint32_t i = 0; i < STENCIL_CELLS; i++) {
		const StencilCell *const cell = &stencil_cells[i];
		const bool last = i == STENCIL_CELLS - 1;
		vkCmdSetStencilReference(commands, faces, cell->mask_reference);
		vkCmdSetStencilWriteMask(commands, faces, cell->write_mask);
		bind(commands, scene, last ? PIPELINE_STATIC_MASK : PIPELINE_MASK);
		draw_quad(commands, scene, cell->x + MASK_INSET, STENCIL_Y + MASK_INSET, CELL_WIDTH - 2 * MASK_INSET,
			CELL_HEIGHT - 2 * MASK_INSET, 0.0f, white);
		bind(commands, scene, PIPELINE_TEST);
		vkCmdSetStencilReference(commands, faces, cell->test_reference);
		vkCmdSetStencilCompareMask(commands, faces, cell->compare_mask);
		draw_quad(commands, scene, cell->x, STENCIL_Y, CELL_WIDTH, CELL_HEIGHT, 0.0f, cell->color);
	}
}

// On a blue base at 0.3, an orange at the same depth pulled nearer shows, and one pushed farther stays hidden.
static void record_bias(VkCommandBuffer commands, const Scene *scene)
{
	const float base[4] = RGBA(64, 64, 224);
	const float orange[4] = RGBA(240, 128, 0);
	bind(commands, scene, PIPELINE_DEPTH);
	draw_quad(commands, scene, 40, BIAS_Y, 300, 140, 0.3f, base);
	bind(commands, scene, PIPELINE_BIAS);
	vkCmdSetDepthBias(commands, -BIAS_UNITS, 0.0f, 0.0f);
	draw_quad(commands, scene, 60, BIAS_Y + 20, 100, 100, 0.3f, orange);
	vkCmdSetDepthBias(commands, BIAS_UNITS, 0.0f, 0.0f);
	draw_quad(commands, scene, 200, BIAS_Y + 20, 100, 100, 0.3f, orange);
}

// Each overlay covers the right half of its base.
static void record_blend(VkCommandBuffer commands, const Scene *scene)
{
	const float base[4] = RGBA(32, 64, 224);
	const float overlay[4] = RGBA(224, 192, 32);
	const float alpha_base[4] = RGBA(64, 224, 128);
	const float alpha_overlay[4] = RGBA(192, 32, 96);
	bind(commands, scene, PIPELINE_PLAIN);
	draw_quad(commands, scene, 640, BLEND_Y, 180, 140, 0.0f, base);
	draw_quad(commands, scene, 860, BLEND_Y, 180, 140, 0.0f, base);
	draw_quad(commands, scene, 40, STATIC_BLEND_Y, 180, 140, 0.0f, alpha_base);
	bind(commands, scene, PIPELINE_BLEND);
	vkCmdSetBlendConstants(commands, first_constants);
	draw_quad(commands, scene, 730, BLEND_Y, 90, 140, 0.0f, overlay);
	vkCmdSetBlendConstants(commands, second_constants);
	draw_quad(commands, scene, 950, BLEND_Y, 90, 140, 0.0f, overlay);
	bind(commands, scene, PIPELINE_STATIC_BLEND);
	draw_quad(commands, scene, 130, STATIC_BLEND_Y, 90, 140, 0.0f, alpha_overlay);
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
		record_stencil(commands, scene);
		record_bias(commands, scene);
		record_blend(commands, scene);
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

// Each fails the command buffer it's recorded in, or the pipeline. A face's stencil alone is taken since milestone 26
// (test_two_sided_stencil.c).
static void check_refusals(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkCommandBuffer commands = scene->commands[0];
	bool refused = vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
	vkCmdSetDepthBias(commands, 1.0f, 0.5f, 0.0f);
	refused = refused && vkEndCommandBuffer(commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	const VkDynamicState bounds = VK_DYNAMIC_STATE_DEPTH_BOUNDS;
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
		.dynamic_states = &bounds,
		.dynamic_state_count = 1,
	};
	VkPipeline pipeline = VK_NULL_HANDLE;
	refused =
		refused && harness_pipeline(harness, &spec, scene->render_pass, &pipeline) == VK_ERROR_FEATURE_NOT_PRESENT;
	check("dynamic state: depth bias clamps and dynamic depth bounds are refused", refused);
	static const char *const names[] = { "vkCmdSetStencilCompareMask", "vkCmdSetStencilWriteMask",
		"vkCmdSetStencilReference", "vkCmdSetDepthBias", "vkCmdSetBlendConstants" };
	bool found = true;
	for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
		found = found && vkGetDeviceProcAddr(harness->device, names[i]);
	check("dynamic state: step 3's 5 commands are found by name", found);
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

void test_dynamic_state(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.checks[GROUP_STENCIL] = (HarnessProbes){ "dynamic stencil", stencil_probes,
		sizeof stencil_probes / sizeof stencil_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_BIAS] =
		(HarnessProbes){ "dynamic depth bias", bias_probes, sizeof bias_probes / sizeof bias_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_BLEND] =
		(HarnessProbes){ "blend constants", blend_probes, sizeof blend_probes / sizeof blend_probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("dynamic state: a D24S8 attachment, a render pass and eight pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("dynamic state: 30 frames drawn and presented", frames == FRAMES);
	check(
		"dynamic state: every frame's stencil takes the write masks, references and compare masks commands set, "
		"and a static pipeline's own",
		frames == FRAMES && scene.misses[GROUP_STENCIL] == 0);
	check("dynamic state: every frame's depth bias is what vkCmdSetDepthBias set, nearer then farther",
		frames == FRAMES && scene.misses[GROUP_BIAS] == 0);
	check("dynamic state: every frame blends by vkCmdSetBlendConstants's colours, and a static pipeline's own alpha",
		frames == FRAMES && scene.misses[GROUP_BLEND] == 0);
	if (recorded)
		check_refusals(harness, &scene);
	destroy(harness, &scene);
}
