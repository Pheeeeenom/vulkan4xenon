#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "tests.h"
#include "xsdk/display.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	LOGO_MS = 1000,
	IMAGES = 2,
	ACQUIRE_TIMEOUT_NS = 1000000000,
	FENCE_TIMEOUT_NS = 1000000000,
	// A dark green no other test clears to, at a pixel the logo would have covered.
	CLEAR_GREEN = 0x60,
	PROBE_X = 640,
	PROBE_Y = 360,
	TOLERANCE = 1,
};

static bool is_image(const Harness *harness, uint32_t address)
{
	for (uint32_t i = 0; i < harness->image_count; i++) {
		if (harness_image_address(i) == address)
			return true;
	}
	return false;
}

static VkResult create_clear_pass(const Harness *harness, VkRenderPass *render_pass)
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
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

// Clears the acquired image to dark green and presents it: the app's first frame.
static bool present_first_frame(const Harness *harness, VkRenderPass render_pass, uint32_t *index)
{
	if (!harness_ok("vkAcquireNextImageKHR",
			vkAcquireNextImageKHR(
				harness->device, harness->swapchain, ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, VK_NULL_HANDLE, index)))
		return false;
	const VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = 1,
		.pAttachments = &harness->views[*index],
		.width = HARNESS_WIDTH,
		.height = HARNESS_HEIGHT,
		.layers = 1,
	};
	VkFramebuffer framebuffer;
	if (!harness_ok("vkCreateFramebuffer", vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &framebuffer)))
		return false;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	bool ok =
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands));
	if (ok) {
		const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		const VkClearValue clear = { .color = { .float32 = { 0.0f, CLEAR_GREEN / 255.0f, 0.0f, 1.0f } } };
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = render_pass,
			.framebuffer = framebuffer,
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		(void)vkBeginCommandBuffer(commands, &begin);
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(commands);
		ok = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	}
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &harness->swapchain,
		.pImageIndices = index,
	};
	ok = ok && harness_ok("vkQueuePresentKHR", vkQueuePresentKHR(harness->queue, &present));
	ok = ok && harness_ok("vkDeviceWaitIdle", vkDeviceWaitIdle(harness->device));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	vkDestroyFramebuffer(harness->device, framebuffer, NULL);
	return ok;
}

void test_boot_logo(Harness *harness, uint32_t shown_before_device)
{
	(void)xsdk_thread_sleep(LOGO_MS);
	const uint32_t logo = xsdk_display_shown_address();
	xsdk_log_write("boot logo: XAM's surface %08X, then %08X", (unsigned)shown_before_device, (unsigned)logo);
	check("boot logo: the display shows a buffer of its own after vkCreateDevice", logo != shown_before_device);
	const bool swapchain = harness_create_swapchain(harness, IMAGES) == VK_SUCCESS;
	check("boot logo: the swapchain made", swapchain);
	if (!swapchain)
		return;
	const uint32_t after_swapchain = xsdk_display_shown_address();
	check("boot logo: still on screen once the swapchain exists",
		after_swapchain == logo && !is_image(harness, after_swapchain));
	VkRenderPass render_pass;
	if (!harness_ok("vkCreateRenderPass", create_clear_pass(harness, &render_pass)))
		return;
	uint32_t index = 0;
	const bool presented = present_first_frame(harness, render_pass, &index);
	vkDestroyRenderPass(harness->device, render_pass, NULL);
	(void)xsdk_thread_sleep(HARNESS_SETTLE_MS);
	const uint32_t shown = xsdk_display_shown_address();
	const Rgb pixel = is_image(harness, shown) ? harness_pixel(shown, PROBE_X, PROBE_Y) : (Rgb){ 0, 0, 0 };
	xsdk_log_write("boot logo: after the first present the display shows %08X, image %u at %08X; pixel %02X %02X %02X",
		(unsigned)shown, (unsigned)index, (unsigned)harness_image_address(index), (unsigned)pixel.red,
		(unsigned)pixel.green, (unsigned)pixel.blue);
	check("boot logo: the first present replaces it with the app's image",
		presented && shown == harness_image_address(index) && pixel.red <= TOLERANCE &&
			pixel.green + TOLERANCE >= CLEAR_GREEN && pixel.green <= CLEAR_GREEN + TOLERANCE &&
			pixel.blue <= TOLERANCE);
}
