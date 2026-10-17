#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Each subpass draws into one to four colour attachments, and a depth attachment if it has one, and is replayed as a
// render pass of its own. They live in eDRAM during it, each colour a target of its own; the colours are resolved at
// its end, and depth stays (docs/ROADMAP.md, milestones 4, 19 and 25).

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

// Each colour attachment its own, apart from the others and from the depth.
static VkResult check_colors(const VkRenderPassCreateInfo *info, const VkSubpassDescription *subpass)
{
	const VkAttachmentReference *const depth = depth_of(subpass);
	for (uint32_t i = 0; i < subpass->colorAttachmentCount; i++) {
		const uint32_t color = subpass->pColorAttachments[i].attachment;
		if (color >= info->attachmentCount || (depth && depth->attachment == color))
			return v4x_host_unsupported("subpasses' colour attachments unused, or in the depth's attachment");
		for (uint32_t j = 0; j < i; j++) {
			if (subpass->pColorAttachments[j].attachment == color)
				return v4x_host_unsupported("subpasses naming one colour attachment twice");
		}
		const VkAttachmentDescription *const target = &info->pAttachments[color];
		if (!v4x_attachment_format(target->format) || target->samples != VK_SAMPLE_COUNT_1_BIT)
			return v4x_host_unsupported("colour attachments of formats xsdk's targets don't draw, or multisampled");
	}
	return VK_SUCCESS;
}

// Preserved attachments need nothing, as every subpass stores what a later one uses.
static VkResult check_subpass(const VkRenderPassCreateInfo *info, uint32_t index)
{
	const VkSubpassDescription *const subpass = &info->pSubpasses[index];
	if (subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass->inputAttachmentCount ||
		!subpass->colorAttachmentCount || subpass->colorAttachmentCount > V4X_MAX_COLOR_ATTACHMENTS ||
		subpass->pResolveAttachments)
		return v4x_host_unsupported(
			"subpasses other than one to four colour attachments and a depth attachment, and input attachments");
	const VkAttachmentReference *const depth = depth_of(subpass);
	if (depth && depth->attachment >= info->attachmentCount)
		return v4x_host_unsupported("subpasses' depth past the render pass's attachments");
	const VkResult result = check_colors(info, subpass);
	if (result != VK_SUCCESS || !depth)
		return result;
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
		for (uint32_t i = 0; i < info->pSubpasses[s].colorAttachmentCount; i++)
			mark(uses, info->pSubpasses[s].pColorAttachments[i].attachment, s);
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
	RenderPass pass = { .color_count = subpass->colorAttachmentCount };
	for (uint32_t i = 0; i < pass.color_count; i++) {
		const uint32_t color = subpass->pColorAttachments[i].attachment;
		const VkAttachmentDescription *const target = &info->pAttachments[color];
		pass.formats[i] = target->format;
		pass.targets[i] = v4x_target_format(target->format);
		pass.load_ops[i] = uses[color].first == index ? target->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD;
		pass.store_ops[i] = uses[color].last == index ? target->storeOp : VK_ATTACHMENT_STORE_OP_STORE;
		pass.color_indices[i] = color;
	}
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

// A pass clears and stores its render area by resolves, which copy and clear 8 by 8 pixels at a time, so the area is
// on that grid or reaches the framebuffer's edge; vkCmdBeginRenderPass refuses others.
VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(
	VkDevice device, VkRenderPass renderPass, VkExtent2D *pGranularity)
{
	(void)device;
	(void)renderPass;
	*pGranularity = (VkExtent2D){ XSDK_RESOLVE_ALIGNMENT, XSDK_RESOLVE_ALIGNMENT };
}

// A subpass's colour and depth images, each the framebuffer's size.
static VkResult subpass_images(const RenderPass *pass, const VkFramebufferCreateInfo *info, Framebuffer *images)
{
	*images = (Framebuffer){ .width = info->width, .height = info->height };
	for (uint32_t i = 0; i < pass->color_count; i++) {
		const ImageView *const view = v4x_object(info->pAttachments[pass->color_indices[i]]);
		if (!view->image->is_attachment)
			return v4x_host_unsupported("colour attachments without VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT");
		if (info->width != view->image->width || info->height != view->image->height)
			return v4x_host_unsupported("framebuffers smaller than their attachment");
		images->attachments[i] = view->image;
	}
	if (pass->has_depth) {
		images->depth = ((const ImageView *)v4x_object(info->pAttachments[pass->depth_index]))->image;
		if (!images->depth->is_depth || images->depth->width != info->width || images->depth->height != info->height)
			return v4x_host_unsupported("depth attachments of another size than the colour");
	}
	return VK_SUCCESS;
}

// The colour targets one after another from tile 0, and depth after them; past eDRAM's tiles they'd need predicated
// tiling, which is milestone 17's.
static VkResult place_targets(const RenderPass *pass, Framebuffer *images)
{
	uint32_t tile = 0;
	for (uint32_t i = 0; i < pass->color_count; i++) {
		images->targets[i] = (XsdkColorTarget){ .format = pass->targets[i], .tile = tile };
		tile += xsdk_gpu_color_target_tiles(images->width, images->height, pass->targets[i]);
	}
	images->color_tiles = tile;
	const uint32_t depth_tiles = pass->has_depth ? xsdk_gpu_target_tiles(images->width, images->height) : 0;
	if (tile + depth_tiles <= XSDK_EDRAM_TILES)
		return VK_SUCCESS;
	xsdk_log_write("vulkan4xenon: not supported: a subpass's colour and depth at %ux%u in %u tiles, past eDRAM's %u",
		(unsigned)images->width, (unsigned)images->height, (unsigned)(tile + depth_tiles), (unsigned)XSDK_EDRAM_TILES);
	return VK_ERROR_FEATURE_NOT_PRESENT;
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
		VkResult result = subpass_images(&render_pass->subpasses[s], pCreateInfo, &images.subpasses[s]);
		if (result == VK_SUCCESS)
			result = place_targets(&render_pass->subpasses[s], &images.subpasses[s]);
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
