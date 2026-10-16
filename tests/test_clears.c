#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 19's step 4: vkCmdClearDepthStencilImage, whose clear a later pass loading the image finds, and
// vkCmdClearAttachments's rectangles of colour, depth and stencil inside a pass.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	FENCE_TIMEOUT_NS = 1000000000,
	// A sampled depth image, cleared outside a pass and read back through its memory.
	SAMPLED_SIDE = 64,
};

typedef enum {
	PIPELINE_COLOR,
	PIPELINE_DEPTH,
	// Draws where stencil equals 1, then 2.
	PIPELINE_STENCIL_1,
	PIPELINE_STENCIL_2,
	PIPELINES,
} PipelineKind;

// The image's clear: depth 0.2, stencil 1.
static const VkClearDepthStencilValue image_clear = { 0.2f, 1 };
static const VkClearDepthStencilValue sampled_clear = { 0.25f, 7 };
// D24's 0.25, 0x400000, above stencil 7.
static const uint32_t sampled_word = 0x40000007;

static const VkClearValue background = { .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } };

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

enum {
	GROUP_IMAGE,
	GROUP_COLOR,
	GROUP_DEPTH,
	GROUP_STENCIL,
	GROUPS,
};

static const HarnessProbe image_probes[] = {
	{ "the image's depth: a quad behind it hidden", 90, 180, { 20, 20, 28 }, 0 },
	{ "the image's depth: a quad in front of it shows", 190, 180, { 64, 224, 64 }, 0 },
	{ "the image's stencil: a quad testing equal to it shows", 380, 180, { 224, 224, 64 }, 0 },
};

static const HarnessProbe color_probes[] = {
	{ "colour: the cleared rectangle", 620, 180, { 224, 64, 224 }, 0 },
	{ "colour: the quad around it", 530, 90, { 224, 64, 64 }, 0 },
	{ "colour: a quad drawn after with the pipeline before it", 620, 262, { 64, 64, 224 }, 0 },
};

static const HarnessProbe depth_probes[] = {
	{ "depth: a farther quad shows in the rectangle cleared farther still", 860, 180, { 255, 255, 255 }, 0 },
	{ "depth: and stays behind the first quad around it", 770, 90, { 240, 128, 0 }, 0 },
};

static const HarnessProbe stencil_probes[] = {
	{ "stencil: a quad testing equal to the cleared value shows in the rectangle", 1100, 180, { 64, 224, 224 }, 0 },
	{ "stencil: and not around it", 1010, 90, { 20, 20, 28 }, 0 },
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

// Colour cleared each frame; depth and stencil loaded, as the image's clear sets them.
static VkResult make_render_pass(const Harness *harness, Scene *scene)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkResult result = vkCreateRenderPass(harness->device, &info, NULL, &scene->render_pass);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkImageView views[2] = { harness->views[i], scene->depth_view };
		const VkFramebufferCreateInfo framebuffer = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->render_pass,
			.attachmentCount = 2,
			.pAttachments = views,
			.width = HARNESS_WIDTH,
			.height = HARNESS_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &framebuffer, NULL, &scene->framebuffers[i]);
	}
	return result;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const bool stencil = kind == PIPELINE_STENCIL_1 || kind == PIPELINE_STENCIL_2;
	const VkStencilOpState equal = {
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_KEEP,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_EQUAL,
		.compareMask = 0xFF,
		.writeMask = 0,
		.reference = kind == PIPELINE_STENCIL_2 ? 2 : 1,
	};
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = kind == PIPELINE_DEPTH,
		.depthWriteEnable = kind == PIPELINE_DEPTH,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = stencil,
		.front = equal,
		.back = equal,
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
		.depth_stencil = &depth_stencil,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok &&
		harness_ok("depth image",
			harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, HARNESS_WIDTH, HARNESS_HEIGHT, usage, &scene->depth,
				&scene->depth_memory, &scene->depth_view));
	ok = ok && harness_ok("render pass", make_render_pass(harness, scene));
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
	const HarnessQuad quad = { x, y, width, height, depth, { color[0], color[1], color[2], color[3] }, false };
	harness_push_quad(commands, scene->layout, &quad);
}

static void clear_rect(
	VkCommandBuffer commands, VkImageAspectFlags aspect, VkClearValue value, int32_t x, int32_t y, uint32_t side)
{
	const VkClearAttachment attachment = { aspect, 0, value };
	const VkClearRect rect = { { { x, y }, { side, side } }, 0, 1 };
	vkCmdClearAttachments(commands, 1, &attachment, 1, &rect);
}

// The cells, left to right: the image's depth, its stencil, a colour clear, a depth clear and a stencil clear.
static void record_pass(VkCommandBuffer commands, const Scene *scene)
{
	const float red[4] = RGBA(224, 64, 64);
	const float green[4] = RGBA(64, 224, 64);
	const float blue[4] = RGBA(64, 64, 224);
	const float yellow[4] = RGBA(224, 224, 64);
	const float orange[4] = RGBA(240, 128, 0);
	const float white[4] = RGBA(255, 255, 255);
	const float cyan[4] = RGBA(64, 224, 224);
	const VkClearValue magenta = { .color = { .float32 = RGBA(224, 64, 224) } };
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
	bind(commands, scene, PIPELINE_DEPTH);
	draw_quad(commands, scene, 40, 80, 100, 200, 0.3f, orange);
	draw_quad(commands, scene, 140, 80, 100, 200, 0.1f, green);
	bind(commands, scene, PIPELINE_STENCIL_1);
	draw_quad(commands, scene, 280, 80, 200, 200, 0.0f, yellow);
	bind(commands, scene, PIPELINE_COLOR);
	draw_quad(commands, scene, 520, 80, 200, 200, 0.0f, red);
	clear_rect(commands, VK_IMAGE_ASPECT_COLOR_BIT, magenta, 560, 120, 120);
	draw_quad(commands, scene, 520, 250, 200, 25, 0.0f, blue);
	bind(commands, scene, PIPELINE_DEPTH);
	draw_quad(commands, scene, 760, 80, 200, 200, 0.1f, orange);
	clear_rect(commands, VK_IMAGE_ASPECT_DEPTH_BIT, (VkClearValue){ .depthStencil = { 0.9f, 0 } }, 800, 120, 120);
	draw_quad(commands, scene, 760, 80, 200, 200, 0.5f, white);
	clear_rect(commands, VK_IMAGE_ASPECT_STENCIL_BIT, (VkClearValue){ .depthStencil = { 0.0f, 2 } }, 1040, 120, 120);
	bind(commands, scene, PIPELINE_STENCIL_2);
	draw_quad(commands, scene, 1000, 80, 200, 200, 0.0f, cyan);
	// The first two cells' depth and stencil undone, so the next frame's pass finds them by the image's clear alone.
	clear_rect(commands, VK_IMAGE_ASPECT_DEPTH_BIT, (VkClearValue){ .depthStencil = { 0.9f, 0 } }, 40, 80, 200);
	clear_rect(commands, VK_IMAGE_ASPECT_STENCIL_BIT, (VkClearValue){ .depthStencil = { 0.0f, 0 } }, 280, 80, 200);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdClearDepthStencilImage(
			commands, scene->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &image_clear, 1, &range);
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &background,
		};
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		record_pass(commands, scene);
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

static bool sampled_holds(const Harness *harness, VkDeviceMemory memory, VkDeviceSize size)
{
	void *mapped = NULL;
	if (!harness_ok("vkMapMemory", vkMapMemory(harness->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
		return false;
	const volatile uint32_t *const words = mapped;
	bool holds = true;
	for (VkDeviceSize i = 0; i < size / 4 && holds; i++) {
		holds = words[i] == sampled_word;
		if (!holds)
			xsdk_log_write("clears: the sampled depth image's word %u is %08X, not %08X", (unsigned)i,
				(unsigned)words[i], (unsigned)sampled_word);
	}
	vkUnmapMemory(harness->device, memory);
	return holds;
}

// A sampled depth image's texture takes its clear at once, for a shader to sample.
static void check_sampled(const Harness *harness, VkCommandBuffer commands)
{
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	bool ok = harness_ok("sampled depth image",
		harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, SAMPLED_SIDE, SAMPLED_SIDE, usage, &image, &memory, &view));
	VkMemoryRequirements requirements = { 0 };
	if (ok)
		vkGetImageMemoryRequirements(harness->device, image, &requirements);
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	if (ok)
		vkCmdClearDepthStencilImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &sampled_clear, 1, &range);
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	check("clears: a sampled depth image cleared outside a pass holds the clear in its memory, for shaders",
		ok && sampled_holds(harness, memory, requirements.size));
	vkDestroyImageView(harness->device, view, NULL);
	vkDestroyImage(harness->device, image, NULL);
	vkFreeMemory(harness->device, memory, NULL);
}

// Each fails the command buffer it's recorded in.
static void check_refusals(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkCommandBuffer commands = scene->commands[0];
	const VkImageSubresourceRange depth_alone = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	bool refused = vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
	vkCmdClearDepthStencilImage(
		commands, scene->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &image_clear, 1, &depth_alone);
	refused = refused && vkEndCommandBuffer(commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	const VkClearAttachment attachment = { VK_IMAGE_ASPECT_COLOR_BIT, 0, background };
	const VkClearRect layers = { { { 0, 0 }, { 16, 16 } }, 0, 2 };
	refused = refused && vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
	vkCmdClearAttachments(commands, 1, &attachment, 1, &layers);
	refused = refused && vkEndCommandBuffer(commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	check("clears: a depth image's depth cleared alone and attachment clears of two layers are refused", refused);
	check("clears: step 4's 2 commands are found by name",
		vkGetDeviceProcAddr(harness->device, "vkCmdClearDepthStencilImage") &&
			vkGetDeviceProcAddr(harness->device, "vkCmdClearAttachments"));
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

void test_clears(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.checks[GROUP_IMAGE] =
		(HarnessProbes){ "image clear", image_probes, sizeof image_probes / sizeof image_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_COLOR] =
		(HarnessProbes){ "colour clear", color_probes, sizeof color_probes / sizeof color_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_DEPTH] =
		(HarnessProbes){ "depth clear", depth_probes, sizeof depth_probes / sizeof depth_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_STENCIL] = (HarnessProbes){ "stencil clear", stencil_probes,
		sizeof stencil_probes / sizeof stencil_probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("clears: a D24S8 image to clear, a pass loading it and four pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("clears: 30 frames drawn and presented", frames == FRAMES);
	check("clears: every frame's pass loads the depth and stencil vkCmdClearDepthStencilImage set before it",
		frames == FRAMES && scene.misses[GROUP_IMAGE] == 0);
	check("clears: every frame's colour cleared in a rectangle, and the pipeline's state after it as it was",
		frames == FRAMES && scene.misses[GROUP_COLOR] == 0);
	check("clears: every frame's depth cleared in a rectangle, alone",
		frames == FRAMES && scene.misses[GROUP_DEPTH] == 0);
	check("clears: every frame's stencil cleared in a rectangle, alone",
		frames == FRAMES && scene.misses[GROUP_STENCIL] == 0);
	if (recorded) {
		check_sampled(harness, scene.commands[0]);
		check_refusals(harness, &scene);
	}
	destroy(harness, &scene);
}
