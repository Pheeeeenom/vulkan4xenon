#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 19's step 5: a render pass of three subpasses, an offscreen colour attachment, then the swapchain image,
// then the offscreen one again, one depth through them all; the second subpass drawn from two secondary command
// buffers.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	FENCE_TIMEOUT_NS = 1000000000,
	SUBPASSES = 3,
	ATTACHMENT_OFFSCREEN = 0,
	ATTACHMENT_SWAPCHAIN = 1,
	ATTACHMENT_DEPTH = 2,
	ATTACHMENTS = 3,
	SECONDARIES = 2,
	READBACK_BYTES = HARNESS_WIDTH * HARNESS_HEIGHT * 4,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
};

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

// The offscreen image clears to a dark blue, the swapchain image to the harness's grey, depth to 1.
static const VkClearValue clears[ATTACHMENTS] = {
	[ATTACHMENT_OFFSCREEN] = { .color = { .float32 = { 0.0f, 0.0f, 64.0f / 255, 1.0f } } },
	[ATTACHMENT_SWAPCHAIN] = { .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } },
	[ATTACHMENT_DEPTH] = { .depthStencil = { 1.0f, 0 } },
};

// The first subpass writes depth 0.2 at its red quad, so the second's green one at 0.5 hides over it.
static const HarnessProbe probes[] = {
	{ "the second subpass's green quad, past the first's depth", 500, 250, { 64, 224, 64 }, 0 },
	{ "the second subpass's green quad, below the first's depth", 300, 330, { 64, 224, 64 }, 0 },
	{ "the first subpass's depth hides the green quad over it", 300, 200, { 20, 20, 28 }, 0 },
	{ "the second secondary command buffer's yellow quad", 800, 200, { 224, 224, 64 }, 0 },
	{ "the first and third subpasses drew nothing here", 650, 500, { 20, 20, 28 }, 0 },
};

typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint8_t want[4];
} Texel;

// The offscreen image after the pass: the first subpass's red quad, the third's blue one, the clear between.
static const Texel offscreen_texels[] = {
	{ "the first subpass's red quad", 250, 200, { 224, 64, 64, 255 } },
	{ "the third subpass's blue quad, over what the first left", 650, 500, { 64, 64, 224, 255 } },
	{ "the clear the first subpass began with", 1000, 600, { 0, 0, 64, 255 } },
	{ "nothing of the second subpass's", 500, 250, { 0, 0, 64, 255 } },
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkImage offscreen;
	VkDeviceMemory offscreen_memory;
	VkImageView offscreen_view;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkPipelineLayout layout;
	VkPipeline pipelines[SUBPASSES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	VkCommandBuffer secondaries[SECONDARIES];
	HarnessProbes check;
} Scene;

static VkResult make_render_pass(const Harness *harness, Scene *scene)
{
	const VkAttachmentDescription attachments[ATTACHMENTS] = {
		[ATTACHMENT_OFFSCREEN] = { .format = VK_FORMAT_R8G8B8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
		[ATTACHMENT_SWAPCHAIN] = { .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
		[ATTACHMENT_DEPTH] = { .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	const VkAttachmentReference offscreen = { ATTACHMENT_OFFSCREEN, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference swapchain = { ATTACHMENT_SWAPCHAIN, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { ATTACHMENT_DEPTH, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpasses[SUBPASSES] = {
		{ .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &offscreen,
			.pDepthStencilAttachment = &depth },
		{ .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &swapchain,
			.pDepthStencilAttachment = &depth },
		{ .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &offscreen,
			.pDepthStencilAttachment = &depth },
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = ATTACHMENTS,
		.pAttachments = attachments,
		.subpassCount = SUBPASSES,
		.pSubpasses = subpasses,
	};
	VkResult result = vkCreateRenderPass(harness->device, &info, NULL, &scene->render_pass);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkImageView views[ATTACHMENTS] = { scene->offscreen_view, harness->views[i], scene->depth_view };
		const VkFramebufferCreateInfo framebuffer = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->render_pass,
			.attachmentCount = ATTACHMENTS,
			.pAttachments = views,
			.width = HARNESS_WIDTH,
			.height = HARNESS_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &framebuffer, NULL, &scene->framebuffers[i]);
	}
	return result;
}

// A pipeline for each subpass, testing depth less-than and writing it.
static VkResult create_pipeline(const Harness *harness, Scene *scene, uint32_t subpass)
{
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
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
		.subpass = subpass,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[subpass]);
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags offscreen_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout)) &&
		harness_ok("offscreen image",
			harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, HARNESS_WIDTH, HARNESS_HEIGHT, offscreen_usage,
				&scene->offscreen, &scene->offscreen_memory, &scene->offscreen_view)) &&
		harness_ok(
			"depth image", harness_depth_image(harness, &scene->depth, &scene->depth_memory, &scene->depth_view)) &&
		harness_ok("render pass", make_render_pass(harness, scene));
	for (uint32_t s = 0; s < SUBPASSES && ok; s++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, s));
	return ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
}

static void draw_quad(VkCommandBuffer commands, const Scene *scene, uint32_t subpass, float x, float y, float width,
	float height, float depth, const float color[4])
{
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[subpass]);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
	const HarnessQuad quad = { x, y, width, height, depth, { color[0], color[1], color[2], color[3] }, false };
	harness_push_quad(commands, scene->layout, &quad);
}

// Each draws one quad in the second subpass, binding all it uses, as a secondary command buffer starts with nothing.
static VkResult record_secondaries(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
		.commandBufferCount = SECONDARIES,
	};
	VkResult result = vkAllocateCommandBuffers(harness->device, &allocate, scene->secondaries);
	const VkCommandBufferInheritanceInfo inheritance = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
		.renderPass = scene->render_pass,
		.subpass = 1,
	};
	const VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
		.pInheritanceInfo = &inheritance,
	};
	const float green[4] = RGBA(64, 224, 64);
	const float yellow[4] = RGBA(224, 224, 64);
	for (uint32_t i = 0; i < SECONDARIES && result == VK_SUCCESS; i++) {
		result = vkBeginCommandBuffer(scene->secondaries[i], &begin);
		if (i == 0)
			draw_quad(scene->secondaries[i], scene, 1, 200, 150, 400, 200, 0.5f, green);
		else
			draw_quad(scene->secondaries[i], scene, 1, 700, 100, 200, 200, 0.5f, yellow);
		if (result == VK_SUCCESS)
			result = vkEndCommandBuffer(scene->secondaries[i]);
	}
	return result;
}

static VkResult record(const Harness *harness, Scene *scene)
{
	const float red[4] = RGBA(224, 64, 64);
	const float blue[4] = RGBA(64, 64, 224);
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = ATTACHMENTS,
			.pClearValues = clears,
		};
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		draw_quad(commands, scene, 0, 100, 100, 300, 200, 0.2f, red);
		vkCmdNextSubpass(commands, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
		vkCmdExecuteCommands(commands, SECONDARIES, scene->secondaries);
		vkCmdNextSubpass(commands, VK_SUBPASS_CONTENTS_INLINE);
		draw_quad(commands, scene, 2, 500, 400, 300, 200, 0.1f, blue);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// The offscreen image as the last frame left it, copied into cached memory.
static bool offscreen_holds(const Harness *harness, const Scene *scene)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = READBACK_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = NULL;
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = READBACK_BYTES,
		.memoryTypeIndex = CACHED_TYPE,
	};
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { HARNESS_WIDTH, HARNESS_HEIGHT, 1 },
	};
	const VkCommandBuffer commands = scene->commands[0];
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	bool ok = harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, &buffer)) &&
		harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, &memory)) &&
		harness_ok("vkBindBufferMemory", vkBindBufferMemory(harness->device, buffer, memory, 0)) &&
		harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	if (ok)
		vkCmdCopyImageToBuffer(commands, scene->offscreen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok(
			"vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	for (uint32_t i = 0; i < sizeof offscreen_texels / sizeof offscreen_texels[0] && ok; i++) {
		const Texel *const texel = &offscreen_texels[i];
		const uint8_t *const got = (const uint8_t *)mapped + (texel->y * HARNESS_WIDTH + texel->x) * 4;
		ok = !memcmp(got, texel->want, 4);
		if (!ok)
			xsdk_log_write("subpasses: the offscreen image's %s at (%u, %u) is %u %u %u %u", texel->what,
				(unsigned)texel->x, (unsigned)texel->y, got[0], got[1], got[2], got[3]);
	}
	vkDestroyBuffer(harness->device, buffer, NULL);
	vkFreeMemory(harness->device, memory, NULL);
	return ok;
}

static void check_refusals(const Harness *harness)
{
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.inputAttachmentCount = 1,
		.pInputAttachments = &reference,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkRenderPass render_pass;
	check("subpasses: input attachments are refused",
		vkCreateRenderPass(harness->device, &info, NULL, &render_pass) == VK_ERROR_FEATURE_NOT_PRESENT);
	check("subpasses: step 5's 2 commands are found by name",
		vkGetDeviceProcAddr(harness->device, "vkCmdNextSubpass") &&
			vkGetDeviceProcAddr(harness->device, "vkCmdExecuteCommands"));
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkFreeCommandBuffers(device, harness->pool, SECONDARIES, scene->secondaries);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < SUBPASSES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
	vkDestroyImageView(device, scene->offscreen_view, NULL);
	vkDestroyImage(device, scene->offscreen, NULL);
	vkFreeMemory(device, scene->offscreen_memory, NULL);
}

void test_subpasses(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.check = (HarnessProbes){ "subpasses", probes, sizeof probes / sizeof probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene) && harness_ok("secondaries", record_secondaries(harness, &scene));
	check(
		"subpasses: three subpasses over two colour attachments and a depth, their pipelines and two secondary "
		"command buffers made",
		built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("subpasses: 30 frames drawn and presented", frames == FRAMES);
	check("subpasses: every frame's second subpass, drawn from two secondary command buffers, tests the first's depth",
		frames == FRAMES && misses == 0);
	check("subpasses: the first and third subpasses draw into the offscreen attachment, the third over the first's",
		recorded && offscreen_holds(harness, &scene));
	check_refusals(harness);
	destroy(harness, &scene);
}
