#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "harness.h"
#include "xsdk/display.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	ACQUIRE_TIMEOUT_NS = 1000000000,
	FENCE_TIMEOUT_NS = 1000000000,
	MAX_MISSES_LOGGED = 4,
};

bool harness_near(Rgb pixel, Rgb want, uint32_t tolerance)
{
	return pixel.red + tolerance >= want.red && pixel.red <= want.red + tolerance &&
		pixel.green + tolerance >= want.green && pixel.green <= want.green + tolerance &&
		pixel.blue + tolerance >= want.blue && pixel.blue <= want.blue + tolerance;
}

VkResult harness_render_pass(const Harness *harness, VkRenderPass *render_pass, VkFramebuffer *framebuffers)
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
	VkResult result = vkCreateRenderPass(harness->device, &info, NULL, render_pass);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS && framebuffers; i++) {
		const VkFramebufferCreateInfo framebuffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = *render_pass,
			.attachmentCount = 1,
			.pAttachments = &harness->views[i],
			.width = HARNESS_WIDTH,
			.height = HARNESS_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &framebuffers[i]);
	}
	return result;
}

void harness_destroy_render_pass(const Harness *harness, VkRenderPass render_pass, const VkFramebuffer *framebuffers)
{
	for (uint32_t i = 0; i < harness->image_count && framebuffers; i++)
		vkDestroyFramebuffer(harness->device, framebuffers[i], NULL);
	vkDestroyRenderPass(harness->device, render_pass, NULL);
}

static VkResult create_pipeline(const Harness *harness, const HarnessPipeline *spec, VkRenderPass render_pass,
	const VkShaderModule *modules, VkPipeline *pipeline)
{
	const VkPipelineVertexInputStateCreateInfo input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = spec->binding_count,
		.pVertexBindingDescriptions = spec->bindings,
		.vertexAttributeDescriptionCount = spec->attribute_count,
		.pVertexAttributeDescriptions = spec->attributes,
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
			.module = modules[0],
			.pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = modules[1],
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
		.layout = spec->layout,
		.renderPass = render_pass,
	};
	return vkCreateGraphicsPipelines(harness->device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
}

// The shader modules live only as long as pipeline creation needs them.
VkResult harness_pipeline(
	const Harness *harness, const HarnessPipeline *spec, VkRenderPass render_pass, VkPipeline *pipeline)
{
	VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkResult result = harness_shader(harness, spec->vertex_spirv, spec->vertex_bytes, &modules[0]);
	if (harness_ok("vertex shader module", result))
		result = harness_shader(harness, spec->fragment_spirv, spec->fragment_bytes, &modules[1]);
	if (harness_ok("fragment shader module", result))
		result = create_pipeline(harness, spec, render_pass, modules, pipeline);
	vkDestroyShaderModule(harness->device, modules[0], NULL);
	vkDestroyShaderModule(harness->device, modules[1], NULL);
	return result;
}

VkResult harness_command_buffers(const Harness *harness, VkCommandBuffer *commands)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = harness->image_count,
	};
	VkResult result = vkAllocateCommandBuffers(harness->device, &allocate_info, commands);
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++)
		result = vkBeginCommandBuffer(commands[i], &begin);
	return result;
}

static bool frame(const Harness *harness, const VkCommandBuffer *commands, uint32_t number, HarnessCheck check,
	void *context, uint32_t *misses)
{
	uint32_t index = 0;
	if (!harness_ok("vkAcquireNextImageKHR",
			vkAcquireNextImageKHR(
				harness->device, harness->swapchain, ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)))
		return false;
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands[index],
	};
	if (!harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) ||
		!harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS)))
		return false;
	(void)vkResetFences(harness->device, 1, &harness->fence);
	char what[24];
	(void)snprintf(what, sizeof what, "frame %u", (unsigned)number);
	*misses += !check(harness_image_address(index), what, context);
	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &harness->swapchain,
		.pImageIndices = &index,
	};
	return harness_ok("vkQueuePresentKHR", vkQueuePresentKHR(harness->queue, &present));
}

uint32_t harness_frames(const Harness *harness, const VkCommandBuffer *commands, uint32_t frames, HarnessCheck check,
	void *context, uint32_t *misses)
{
	uint32_t done = 0;
	while (done < frames && frame(harness, commands, done, check, context, misses))
		done++;
	(void)vkDeviceWaitIdle(harness->device);
	return done;
}

bool harness_screen(const Harness *harness, HarnessCheck check, void *context)
{
	(void)xsdk_thread_sleep(HARNESS_SETTLE_MS);
	const uint32_t shown = xsdk_display_shown_address();
	for (uint32_t i = 0; i < harness->image_count; i++) {
		if (shown == harness_image_address(i))
			return check(shown, "on screen", context);
	}
	return false;
}

uint32_t harness_flips(void)
{
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	return stats.flips;
}

bool harness_probes_hold(uint32_t address, const char *what, void *context)
{
	HarnessProbes *const probes = context;
	bool holds = true;
	for (uint32_t i = 0; i < probes->count; i++) {
		const HarnessProbe *const probe = &probes->probes[i];
		const Rgb pixel = harness_pixel(address, probe->x, probe->y);
		if (harness_near(pixel, probe->want, probes->tolerance))
			continue;
		if (probes->logged++ < MAX_MISSES_LOGGED)
			xsdk_log_write("%s: %s: %s at %u, %u held %02X %02X %02X, not %02X %02X %02X", probes->test, what,
				probe->name, (unsigned)probe->x, (unsigned)probe->y, (unsigned)pixel.red, (unsigned)pixel.green,
				(unsigned)pixel.blue, (unsigned)probe->want.red, (unsigned)probe->want.green,
				(unsigned)probe->want.blue);
		holds = false;
	}
	return holds;
}
