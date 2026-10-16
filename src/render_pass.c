#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"

// One subpass drawing into one colour attachment, and a depth attachment if it has one. Both live in eDRAM during the
// pass; the colour is resolved at its end, and depth stays (docs/ROADMAP.md, milestone 4).

// One register holds the depth and stencil a resolve clears to, so they clear together (xsdk:src/gpu_resolve.c).
static bool clear_and_load(VkAttachmentLoadOp depth, VkAttachmentLoadOp stencil)
{
	return (depth == VK_ATTACHMENT_LOAD_OP_CLEAR && stencil == VK_ATTACHMENT_LOAD_OP_LOAD) ||
		(depth == VK_ATTACHMENT_LOAD_OP_LOAD && stencil == VK_ATTACHMENT_LOAD_OP_CLEAR);
}

static VkResult check_subpass(const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *const subpass = &info->pSubpasses[0];
	if (subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass->inputAttachmentCount ||
		subpass->colorAttachmentCount != 1 || subpass->pResolveAttachments || subpass->preserveAttachmentCount)
		return v4x_host_unsupported("subpasses other than one colour attachment and a depth attachment");
	const uint32_t color = subpass->pColorAttachments[0].attachment;
	const VkAttachmentReference *const depth = subpass->pDepthStencilAttachment;
	const bool has_depth = depth && depth->attachment != VK_ATTACHMENT_UNUSED;
	if (color >= info->attachmentCount || info->attachmentCount != 1u + has_depth ||
		(has_depth && (depth->attachment >= info->attachmentCount || depth->attachment == color)))
		return v4x_host_unsupported("attachments a subpass doesn't use");
	const VkAttachmentDescription *const target = &info->pAttachments[color];
	if (!v4x_attachment_format(target->format) || target->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported("colour attachments other than single-sampled R8G8B8A8_UNORM and B8G8R8A8_UNORM");
	if (!has_depth)
		return VK_SUCCESS;
	const VkAttachmentDescription *const stencil = &info->pAttachments[depth->attachment];
	if (stencil->format != VK_FORMAT_D24_UNORM_S8_UINT || stencil->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported("depth attachments other than single-sampled D24_UNORM_S8_UINT");
	if (clear_and_load(stencil->loadOp, stencil->stencilLoadOp))
		return v4x_host_unsupported("depth and stencil attachments that clear one and load the other");
	return VK_SUCCESS;
}

bool v4x_read_layout(VkImageLayout layout)
{
	return layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
		layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	if (pCreateInfo->subpassCount != 1)
		return v4x_host_unsupported("render passes of more than one subpass");
	const VkResult result = check_subpass(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	RenderPass *const render_pass = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *render_pass, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!render_pass)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	const VkSubpassDescription *const subpass = &pCreateInfo->pSubpasses[0];
	const uint32_t color = subpass->pColorAttachments[0].attachment;
	const VkAttachmentDescription *const target = &pCreateInfo->pAttachments[color];
	*render_pass = (RenderPass){
		.format = target->format, .load_op = target->loadOp, .store_op = target->storeOp, .color_index = color
	};
	const VkAttachmentReference *const depth = subpass->pDepthStencilAttachment;
	if (depth && depth->attachment != VK_ATTACHMENT_UNUSED) {
		const VkAttachmentDescription *const stencil = &pCreateInfo->pAttachments[depth->attachment];
		render_pass->has_depth = true;
		render_pass->depth_index = depth->attachment;
		render_pass->clears_depth =
			stencil->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || stencil->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
		render_pass->loads_depth =
			stencil->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || stencil->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
		const bool stores =
			stencil->storeOp == VK_ATTACHMENT_STORE_OP_STORE || stencil->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
		render_pass->leaves_depth_to_read = stores && v4x_read_layout(stencil->finalLayout);
	}
	*pRenderPass = v4x_handle(render_pass);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
	VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(renderPass));
}

// A render area is the whole framebuffer (vkCmdBeginRenderPass refuses less), so no granularity is better than any.
VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(
	VkDevice device, VkRenderPass renderPass, VkExtent2D *pGranularity)
{
	(void)device;
	(void)renderPass;
	*pGranularity = (VkExtent2D){ 1, 1 };
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	const RenderPass *const render_pass = v4x_object(pCreateInfo->renderPass);
	if (pCreateInfo->attachmentCount != 1u + render_pass->has_depth || pCreateInfo->layers != 1)
		return v4x_host_unsupported("framebuffers other than the render pass's attachments and one layer");
	const ImageView *const view = v4x_object(pCreateInfo->pAttachments[render_pass->color_index]);
	if (!view->image->is_attachment)
		return v4x_host_unsupported("colour attachments without VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT");
	// Colour and depth at the display's size fill 1440 of eDRAM's tiles (xsdk:SCOPE.md 3.4).
	if (pCreateInfo->width > V4X_DISPLAY_WIDTH || pCreateInfo->height > V4X_DISPLAY_HEIGHT)
		return v4x_host_unsupported("framebuffers larger than 1280x720");
	Image *depth = NULL;
	if (render_pass->has_depth) {
		depth = ((const ImageView *)v4x_object(pCreateInfo->pAttachments[render_pass->depth_index]))->image;
		if (!depth->is_depth || depth->width != view->image->width || depth->height != view->image->height)
			return v4x_host_unsupported("depth attachments of another size than the colour");
	}
	if (pCreateInfo->width != view->image->width || pCreateInfo->height != view->image->height)
		return v4x_host_unsupported("framebuffers smaller than their attachment");
	Framebuffer *const framebuffer = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *framebuffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!framebuffer)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*framebuffer = (Framebuffer){
		.attachment = view->image, .depth = depth, .width = pCreateInfo->width, .height = pCreateInfo->height
	};
	*pFramebuffer = v4x_handle(framebuffer);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
	VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(framebuffer));
}
