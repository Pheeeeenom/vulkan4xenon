#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"

// Each subpass draws into one colour attachment, and a depth attachment if it has one, and is replayed as a render pass
// of its own. Both live in eDRAM during it; the colour is resolved at its end, and depth stays (docs/ROADMAP.md,
// milestones 4 and 19).

// The subpasses an attachment is first and last used in, so the ones between keep it.
typedef struct {
	uint32_t first;
	uint32_t last;
} Uses;

// One register holds the depth and stencil a resolve clears to, so they clear together (xsdk:src/gpu_resolve.c).
static bool clear_and_load(VkAttachmentLoadOp depth, VkAttachmentLoadOp stencil)
{
	return (depth == VK_ATTACHMENT_LOAD_OP_CLEAR && stencil == VK_ATTACHMENT_LOAD_OP_LOAD) ||
		(depth == VK_ATTACHMENT_LOAD_OP_LOAD && stencil == VK_ATTACHMENT_LOAD_OP_CLEAR);
}

static const VkAttachmentReference *depth_of(const VkSubpassDescription *subpass)
{
	const VkAttachmentReference *const depth = subpass->pDepthStencilAttachment;
	return depth && depth->attachment != VK_ATTACHMENT_UNUSED ? depth : NULL;
}

// Preserved attachments need nothing, as every subpass stores what a later one uses.
static VkResult check_subpass(const VkRenderPassCreateInfo *info, uint32_t index)
{
	const VkSubpassDescription *const subpass = &info->pSubpasses[index];
	if (subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass->inputAttachmentCount ||
		subpass->colorAttachmentCount != 1 || subpass->pResolveAttachments)
		return v4x_host_unsupported(
			"subpasses other than one colour attachment and a depth attachment, and input attachments");
	const uint32_t color = subpass->pColorAttachments[0].attachment;
	const VkAttachmentReference *const depth = depth_of(subpass);
	if (color >= info->attachmentCount ||
		(depth && (depth->attachment >= info->attachmentCount || depth->attachment == color)))
		return v4x_host_unsupported("subpasses' colour and depth in one attachment");
	const VkAttachmentDescription *const target = &info->pAttachments[color];
	if (!v4x_attachment_format(target->format) || target->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported("colour attachments other than single-sampled R8G8B8A8_UNORM and B8G8R8A8_UNORM");
	if (!depth)
		return VK_SUCCESS;
	const VkAttachmentDescription *const stencil = &info->pAttachments[depth->attachment];
	if (stencil->format != VK_FORMAT_D24_UNORM_S8_UINT || stencil->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported("depth attachments other than single-sampled D24_UNORM_S8_UINT");
	if (clear_and_load(stencil->loadOp, stencil->stencilLoadOp))
		return v4x_host_unsupported("depth and stencil attachments that clear one and load the other");
	return VK_SUCCESS;
}

static void mark(Uses *uses, uint32_t attachment, uint32_t subpass)
{
	if (uses[attachment].first == UINT32_MAX)
		uses[attachment].first = subpass;
	uses[attachment].last = subpass;
}

static void find_uses(const VkRenderPassCreateInfo *info, Uses *uses)
{
	for (uint32_t a = 0; a < info->attachmentCount; a++)
		uses[a] = (Uses){ UINT32_MAX, 0 };
	for (uint32_t s = 0; s < info->subpassCount; s++) {
		mark(uses, info->pSubpasses[s].pColorAttachments[0].attachment, s);
		const VkAttachmentReference *const depth = depth_of(&info->pSubpasses[s]);
		if (depth)
			mark(uses, depth->attachment, s);
	}
}

bool v4x_read_layout(VkImageLayout layout)
{
	return layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
		layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

// An attachment takes its own load op in the first subpass that uses it and its store op in the last; the subpasses
// between load and store it.
static RenderPass subpass_pass(const VkRenderPassCreateInfo *info, uint32_t index, const Uses *uses)
{
	const VkSubpassDescription *const subpass = &info->pSubpasses[index];
	const uint32_t color = subpass->pColorAttachments[0].attachment;
	const VkAttachmentDescription *const target = &info->pAttachments[color];
	RenderPass pass = {
		.format = target->format,
		.load_op = uses[color].first == index ? target->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD,
		.store_op = uses[color].last == index ? target->storeOp : VK_ATTACHMENT_STORE_OP_STORE,
		.color_index = color,
	};
	const VkAttachmentReference *const depth = depth_of(subpass);
	if (!depth)
		return pass;
	const VkAttachmentDescription *const stencil = &info->pAttachments[depth->attachment];
	const bool first = uses[depth->attachment].first == index;
	const bool last = uses[depth->attachment].last == index;
	pass.has_depth = true;
	pass.depth_index = depth->attachment;
	pass.clears_depth = first &&
		(stencil->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || stencil->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	pass.loads_depth =
		!first || stencil->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || stencil->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
	const bool stores = !last || stencil->storeOp == VK_ATTACHMENT_STORE_OP_STORE ||
		stencil->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
	pass.leaves_depth_to_read = last && stores && v4x_read_layout(stencil->finalLayout);
	return pass;
}

// Replay is in order, so the dependencies between subpasses need nothing.
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	if (!pCreateInfo->subpassCount || pCreateInfo->subpassCount > V4X_MAX_SUBPASSES ||
		pCreateInfo->attachmentCount > V4X_MAX_ATTACHMENTS)
		return v4x_host_unsupported("render passes of more than 8 subpasses or 8 attachments");
	for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
		const VkResult result = check_subpass(pCreateInfo, s);
		if (result != VK_SUCCESS)
			return result;
	}
	RenderPassSubpasses *const render_pass = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *render_pass, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!render_pass)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	Uses uses[V4X_MAX_ATTACHMENTS];
	find_uses(pCreateInfo, uses);
	for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++)
		render_pass->subpasses[s] = subpass_pass(pCreateInfo, s, uses);
	render_pass->subpass_count = pCreateInfo->subpassCount;
	render_pass->attachment_count = pCreateInfo->attachmentCount;
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

// A subpass's colour and depth images, each the framebuffer's size.
static VkResult subpass_images(const RenderPass *pass, const VkFramebufferCreateInfo *info, Framebuffer *images)
{
	const ImageView *const view = v4x_object(info->pAttachments[pass->color_index]);
	if (!view->image->is_attachment)
		return v4x_host_unsupported("colour attachments without VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT");
	Image *depth = NULL;
	if (pass->has_depth) {
		depth = ((const ImageView *)v4x_object(info->pAttachments[pass->depth_index]))->image;
		if (!depth->is_depth || depth->width != view->image->width || depth->height != view->image->height)
			return v4x_host_unsupported("depth attachments of another size than the colour");
	}
	if (info->width != view->image->width || info->height != view->image->height)
		return v4x_host_unsupported("framebuffers smaller than their attachment");
	*images = (Framebuffer){ .attachment = view->image, .depth = depth, .width = info->width, .height = info->height };
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	const RenderPassSubpasses *const render_pass = v4x_object(pCreateInfo->renderPass);
	if (pCreateInfo->attachmentCount != render_pass->attachment_count || pCreateInfo->layers != 1)
		return v4x_host_unsupported("framebuffers other than the render pass's attachments and one layer");
	// Colour and depth at the display's size fill 1440 of eDRAM's tiles (xsdk:SCOPE.md 3.4).
	if (pCreateInfo->width > V4X_DISPLAY_WIDTH || pCreateInfo->height > V4X_DISPLAY_HEIGHT)
		return v4x_host_unsupported("framebuffers larger than 1280x720");
	FramebufferSubpasses images = { .width = pCreateInfo->width, .height = pCreateInfo->height };
	for (uint32_t s = 0; s < render_pass->subpass_count; s++) {
		const VkResult result = subpass_images(&render_pass->subpasses[s], pCreateInfo, &images.subpasses[s]);
		if (result != VK_SUCCESS)
			return result;
	}
	FramebufferSubpasses *const framebuffer = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *framebuffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!framebuffer)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*framebuffer = images;
	*pFramebuffer = v4x_handle(framebuffer);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
	VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(framebuffer));
}
