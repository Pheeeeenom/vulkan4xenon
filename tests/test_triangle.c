#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/display.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	FRAMES = 120,
	ACQUIRE_TIMEOUT_NS = 1000000000,
	FENCE_TIMEOUT_NS = 1000000000,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	// The corners land at (640, 90), (160, 630) and (1120, 630): half of 960 by 540 pixels, give or take its edges.
	TRIANGLE_PIXELS = 960 * 540 / 2,
	TRIANGLE_TOLERANCE_PIXELS = 2000,
	CHANNEL_HIGH = 0xC0,
	CHANNEL_LOW = 0x40,
	MAX_MISSES_LOGGED = 4,
	// The boot logo test presented one frame before these.
	EARLIER_FRAMES = 1,
};

// Clip space with Vulkan's y down: red at the top, green bottom left, blue bottom right.
static const float triangle[3][VERTEX_FLOATS] = {
	{ 0.0f, -0.75f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
	{ -0.75f, 0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.75f, 0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
};

// A dark blue no other test clears to: 32, 32 and 64 after rounding to 8 bits.
static const VkClearValue clear = { .color = { .float32 = { 0.125f, 0.125f, 0.25f, 1.0f } } };
static const Rgb clear_rgb = { 32, 32, 64 };

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	Rgb high;
} Corner;

// A pixel just inside each corner, and which channels should be bright there.
static const Corner corners[] = {
	{ "red", 640, 120, { 1, 0, 0 } },
	{ "green", 200, 615, { 0, 1, 0 } },
	{ "blue", 1080, 615, { 0, 0, 1 } },
};

static const uint32_t outside_x = 20;
static const uint32_t outside_y = 20;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkShaderModule vertex_shader;
	VkShaderModule fragment_shader;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
} Scene;

static bool channel_holds(uint32_t value, uint32_t high)
{
	return high ? value >= CHANNEL_HIGH : value <= CHANNEL_LOW;
}

static bool same_rgb(Rgb a, Rgb b)
{
	return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// Each corner has its colour, and a pixel outside the triangle the clear colour.
static bool image_holds(uint32_t address, const char *what, uint32_t *misses)
{
	bool holds = same_rgb(harness_pixel(address, outside_x, outside_y), clear_rgb);
	for (uint32_t i = 0; i < sizeof corners / sizeof corners[0]; i++) {
		const Corner *const corner = &corners[i];
		const Rgb pixel = harness_pixel(address, corner->x, corner->y);
		if (channel_holds(pixel.red, corner->high.red) && channel_holds(pixel.green, corner->high.green) &&
			channel_holds(pixel.blue, corner->high.blue))
			continue;
		if (*misses < MAX_MISSES_LOGGED)
			xsdk_log_write("triangle: %s: the %s corner held %02X %02X %02X", what, corner->name, (unsigned)pixel.red,
				(unsigned)pixel.green, (unsigned)pixel.blue);
		holds = false;
	}
	*misses += !holds;
	return holds;
}

static uint32_t count_drawn(uint32_t address)
{
	uint32_t drawn = 0;
	for (uint32_t y = 0; y < HARNESS_HEIGHT; y++) {
		for (uint32_t x = 0; x < HARNESS_WIDTH; x++)
			drawn += !same_rgb(harness_pixel(address, x, y), clear_rgb);
	}
	return drawn;
}

static VkResult create_render_pass(const Harness *harness, Scene *scene)
{
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
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
	return vkCreateRenderPass(harness->device, &info, NULL, &scene->render_pass);
}

static VkResult create_framebuffers(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkFramebufferCreateInfo framebuffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->render_pass,
			.attachmentCount = 1,
			.pAttachments = &harness->views[i],
			.width = HARNESS_WIDTH,
			.height = HARNESS_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &scene->framebuffers[i]);
	}
	return result;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene)
{
	const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
	const VkVertexInputAttributeDescription attributes[] = {
		{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
		{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = COLOR_OFFSET_BYTES },
	};
	const VkPipelineVertexInputStateCreateInfo input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = attributes,
	};
	const VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	const VkViewport viewport = { 0.0f, 0.0f, HARNESS_WIDTH, HARNESS_HEIGHT, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } };
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	const VkPipelineColorBlendAttachmentState blend_attachment = {
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	const VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	const VkPipelineShaderStageCreateInfo stages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = scene->vertex_shader,
			.pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = scene->fragment_shader,
			.pName = "main" },
	};
	const VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &input,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.layout = scene->layout,
		.renderPass = scene->render_pass,
	};
	return vkCreateGraphicsPipelines(harness->device, VK_NULL_HANDLE, 1, &info, NULL, &scene->pipeline);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = harness->image_count,
	};
	VkResult result = vkAllocateCommandBuffers(harness->device, &allocate_info, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		const VkDeviceSize offset = 0;
		(void)vkBeginCommandBuffer(scene->commands[i], &begin);
		vkCmdBeginRenderPass(scene->commands[i], &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(scene->commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(scene->commands[i], 0, 1, &scene->vertices, &offset);
		vkCmdDraw(scene->commands[i], 3, 1, 0, 0);
		vkCmdEndRenderPass(scene->commands[i]);
		result = vkEndCommandBuffer(scene->commands[i]);
	}
	return result;
}

// What doesn't need the swapchain, so Xenia, which has no display, still makes it.
static bool build_pipeline(const Harness *harness, Scene *scene)
{
	const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	bool ok = harness_ok("vkCreateRenderPass", create_render_pass(harness, scene));
	ok = ok &&
		harness_ok("vertex shader",
			harness_shader(harness, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES, &scene->vertex_shader));
	ok = ok &&
		harness_ok("fragment shader",
			harness_shader(harness, triangle_frag_spirv, TRIANGLE_FRAG_SPIRV_BYTES, &scene->fragment_shader));
	ok = ok &&
		harness_ok(
			"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	ok = ok && harness_ok("vkCreateGraphicsPipelines", create_pipeline(harness, scene));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, triangle, sizeof triangle, &scene->vertices, &scene->vertex_memory));
}

static void destroy_scene(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyShaderModule(device, scene->vertex_shader, NULL);
	vkDestroyShaderModule(device, scene->fragment_shader, NULL);
	for (uint32_t i = 0; i < harness->image_count; i++)
		vkDestroyFramebuffer(device, scene->framebuffers[i], NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
}

// One frame: acquire, replay the image's commands, check what they drew, then present it.
static bool draw_frame(const Harness *harness, const Scene *scene, uint32_t frame, uint32_t *misses)
{
	uint32_t index = 0;
	if (!harness_ok("vkAcquireNextImageKHR",
			vkAcquireNextImageKHR(
				harness->device, harness->swapchain, ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)))
		return false;
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &scene->commands[index],
	};
	if (!harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) ||
		!harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS)))
		return false;
	(void)vkResetFences(harness->device, 1, &harness->fence);
	char what[16];
	(void)snprintf(what, sizeof what, "frame %u", (unsigned)frame);
	(void)image_holds(harness_image_address(index), what, misses);
	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &harness->swapchain,
		.pImageIndices = &index,
	};
	return harness_ok("vkQueuePresentKHR", vkQueuePresentKHR(harness->queue, &present));
}

// The image on screen once the frames have shown, and whether every one of them reached the screen.
static void check_screen(const Harness *harness)
{
	(void)xsdk_thread_sleep(HARNESS_SETTLE_MS);
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	xsdk_log_write("triangle: %u flips over %u vblanks, %u missed, %u late", (unsigned)stats.flips,
		(unsigned)stats.vblanks, (unsigned)stats.missed, (unsigned)stats.late);
	check("triangle: every presented frame reached the screen", stats.flips == EARLIER_FRAMES + FRAMES);
	const uint32_t shown = xsdk_display_shown_address();
	bool on_screen = false;
	for (uint32_t i = 0; i < harness->image_count; i++)
		on_screen = on_screen || shown == harness_image_address(i);
	uint32_t misses = 0;
	const bool holds = on_screen && image_holds(shown, "on screen", &misses);
	const uint32_t drawn = on_screen ? count_drawn(shown) : 0;
	xsdk_log_write("triangle: the image on screen, %08X, drew %u pixels", (unsigned)shown, (unsigned)drawn);
	check("triangle: the image on screen has red, green and blue corners on the clear colour", holds);
	check("triangle: the image on screen covers the triangle's area",
		drawn + TRIANGLE_TOLERANCE_PIXELS >= TRIANGLE_PIXELS && drawn <= TRIANGLE_PIXELS + TRIANGLE_TOLERANCE_PIXELS);
}

void test_triangle(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool pipeline = build_pipeline(harness, &scene);
	check("triangle: render pass, shader modules, pipeline and vertex buffer made", pipeline);
	const bool built = pipeline && harness->swapchain &&
		harness_ok("framebuffers", create_framebuffers(harness, &scene)) &&
		harness_ok("recording", record(harness, &scene));
	check("triangle: framebuffers made and command buffers recorded", built);
	uint32_t misses = 0;
	uint32_t frames = 0;
	while (built && frames < FRAMES && draw_frame(harness, &scene, frames, &misses))
		frames++;
	check("triangle: 120 frames acquired, drawn and presented", frames == FRAMES);
	check("triangle: every frame's image has red, green and blue corners on the clear colour",
		frames == FRAMES && misses == 0);
	(void)vkDeviceWaitIdle(harness->device);
	check_screen(harness);
	destroy_scene(harness, &scene);
}
