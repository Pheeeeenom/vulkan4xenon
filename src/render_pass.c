#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"

// One subpass drawing into one colour attachment, and a depth attachment if it has one. Both live in eDRAM during the
// pass; the colour is resolved at its end.

static bool clear_or_dont_care(VkAttachmentLoadOp op)
{
	return op == VK_ATTACHMENT_LOAD_OP_CLEAR || op == VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
	if (target->format != VK_FORMAT_B8G8R8A8_UNORM || target->samples != VK_SAMPLE_COUNT_1_BIT ||
		!clear_or_dont_care(target->loadOp))
		return v4x_host_unsupported("colour attachments other than single-sampled B8G8R8A8_UNORM, cleared or not");
	if (!has_depth)
		return VK_SUCCESS;
	const VkAttachmentDescription *const stencil = &info->pAttachments[depth->attachment];
	if (stencil->format != VK_FORMAT_D24_UNORM_S8_UINT || stencil->samples != VK_SAMPLE_COUNT_1_BIT ||
		!clear_or_dont_care(stencil->loadOp) || !clear_or_dont_care(stencil->stencilLoadOp))
		return v4x_host_unsupported("depth attachments other than single-sampled D24_UNORM_S8_UINT, cleared or not");
	return VK_SUCCESS;
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
	}
	*pRenderPass = v4x_handle(render_pass);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
	VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(renderPass));
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	const RenderPass *const render_pass = v4x_object(pCreateInfo->renderPass);
	if (pCreateInfo->attachmentCount != 1u + render_pass->has_depth || pCreateInfo->layers != 1)
		return v4x_host_unsupported("framebuffers other than the render pass's attachments and one layer");
	const ImageView *const view = v4x_object(pCreateInfo->pAttachments[render_pass->color_index]);
	if (view->image->is_texture || view->image->is_depth)
		return v4x_host_unsupported("rendering into images other than the swapchain's");
	const Image *depth = NULL;
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
