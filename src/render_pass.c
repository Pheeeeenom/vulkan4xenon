#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"

// One subpass drawing into one colour attachment, which lives in eDRAM during the pass and is resolved at its end.

static VkResult check_render_pass(const VkRenderPassCreateInfo *info)
{
	if (info->attachmentCount != 1 || info->subpassCount != 1)
		return v4x_host_unsupported("render passes other than one subpass with one attachment");
	const VkAttachmentDescription *const attachment = &info->pAttachments[0];
	if (attachment->format != VK_FORMAT_B8G8R8A8_UNORM || attachment->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported("attachments other than single-sampled B8G8R8A8_UNORM");
	if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
		return v4x_host_unsupported("VK_ATTACHMENT_LOAD_OP_LOAD");
	const VkSubpassDescription *const subpass = &info->pSubpasses[0];
	if (subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass->inputAttachmentCount ||
		subpass->colorAttachmentCount != 1 || subpass->pColorAttachments[0].attachment != 0 ||
		subpass->pResolveAttachments || subpass->pDepthStencilAttachment || subpass->preserveAttachmentCount)
		return v4x_host_unsupported("subpasses other than one colour attachment");
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	const VkResult result = check_render_pass(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	RenderPass *const render_pass = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *render_pass, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!render_pass)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	const VkAttachmentDescription *const attachment = &pCreateInfo->pAttachments[0];
	*render_pass =
		(RenderPass){ .format = attachment->format, .load_op = attachment->loadOp, .store_op = attachment->storeOp };
	*pRenderPass = v4x_handle(render_pass);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
	VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(renderPass));
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	const Image *const image = v4x_object(pCreateInfo->image);
	const VkImageSubresourceRange *const range = &pCreateInfo->subresourceRange;
	if (pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D || pCreateInfo->format != image->format || range->baseMipLevel ||
		range->levelCount != 1 || range->baseArrayLayer || range->layerCount != 1)
		return v4x_host_unsupported("image views other than the whole of a 2D image in its format");
	const VkComponentMapping *const mapping = &pCreateInfo->components;
	if ((mapping->r != VK_COMPONENT_SWIZZLE_IDENTITY && mapping->r != VK_COMPONENT_SWIZZLE_R) ||
		(mapping->g != VK_COMPONENT_SWIZZLE_IDENTITY && mapping->g != VK_COMPONENT_SWIZZLE_G) ||
		(mapping->b != VK_COMPONENT_SWIZZLE_IDENTITY && mapping->b != VK_COMPONENT_SWIZZLE_B) ||
		(mapping->a != VK_COMPONENT_SWIZZLE_IDENTITY && mapping->a != VK_COMPONENT_SWIZZLE_A))
		return v4x_host_unsupported("image view swizzles");
	ImageView *const view =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *view, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!view)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	view->image = image;
	*pView = v4x_handle(view);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
	VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(imageView));
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	if (pCreateInfo->attachmentCount != 1 || pCreateInfo->layers != 1)
		return v4x_host_unsupported("framebuffers other than one attachment and one layer");
	const ImageView *const view = v4x_object(pCreateInfo->pAttachments[0]);
	if (pCreateInfo->width != view->image->width || pCreateInfo->height != view->image->height)
		return v4x_host_unsupported("framebuffers smaller than their attachment");
	Framebuffer *const framebuffer = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *framebuffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!framebuffer)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*framebuffer =
		(Framebuffer){ .attachment = view->image, .width = pCreateInfo->width, .height = pCreateInfo->height };
	*pFramebuffer = v4x_handle(framebuffer);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
	VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(framebuffer));
}
