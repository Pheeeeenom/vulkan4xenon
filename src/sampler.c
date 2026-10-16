#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/texture.h"

enum {
	// The fetch constant's LOD bias runs from -16 to under 16 (xsdk:include/xsdk/texture.h).
	MAX_LOD_BIAS = 15,
	// The levels it clamps to, whole ones from 0 to 15 (xsdk:include/xsdk/texture.h, XsdkSampler).
	MAX_LEVEL = 15,
};

// A LOD clamp's level: Xenos clamps to whole levels, so the nearest.
static uint32_t nearest_level(float lod)
{
	return (uint32_t)(lod + 0.5f);
}

// Mirror clamp to edge is VK_KHR_sampler_mirror_clamp_to_edge's, Xenos's mirror once (xsdk:include/xsdk/texture.h).
static bool address_mode(VkSamplerAddressMode mode, bool mirror_clamp, uint32_t *xsdk_mode)
{
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		*xsdk_mode = XSDK_ADDRESS_WRAP;
		return true;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		*xsdk_mode = XSDK_ADDRESS_MIRROR;
		return true;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		*xsdk_mode = XSDK_ADDRESS_CLAMP;
		return true;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		*xsdk_mode = XSDK_ADDRESS_BORDER;
		return true;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
		*xsdk_mode = XSDK_ADDRESS_MIRROR_ONCE;
		return mirror_clamp;
	default:
		return false;
	}
}

// Xenos's borders are black and white. Its black's alpha isn't known, and Xenia's backends take 0
// (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:817-824), so transparent black is it and opaque black waits for
// the console (xsdk:tests/test_sampling.c). The integer borders are for integer formats, which images don't take.
static bool border_colour(VkBorderColor colour, uint32_t *border)
{
	*border = colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? XSDK_BORDER_WHITE : XSDK_BORDER_BLACK;
	return colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || colour == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

static uint32_t filter(VkFilter filter)
{
	return filter == VK_FILTER_LINEAR ? XSDK_FILTER_LINEAR : XSDK_FILTER_POINT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
	const VkSamplerCreateInfo *const info = pCreateInfo;
	XsdkSampler setting = {
		.mag_filter = filter(info->magFilter),
		.min_filter = filter(info->minFilter),
		.mip_filter = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? XSDK_FILTER_LINEAR : XSDK_FILTER_POINT,
		.lod_bias = info->mipLodBias,
	};
	const bool mirror_clamp = device->mirror_clamp_to_edge;
	if (!address_mode(info->addressModeU, mirror_clamp, &setting.address_u) ||
		!address_mode(info->addressModeV, mirror_clamp, &setting.address_v) ||
		!address_mode(info->addressModeW, mirror_clamp, &setting.address_w))
		return v4x_host_unsupported(
			"address modes other than repeat, mirrored repeat, clamp to edge and to border, and "
			"mirror clamp to edge with its extension");
	// The border colour counts only where a mode reads the border.
	const bool reads_border = setting.address_u == XSDK_ADDRESS_BORDER || setting.address_v == XSDK_ADDRESS_BORDER ||
		setting.address_w == XSDK_ADDRESS_BORDER;
	if (!border_colour(info->borderColor, &setting.border) && reads_border)
		return v4x_host_unsupported("border colours other than transparent black and opaque white");
	if (info->compareEnable || info->unnormalizedCoordinates)
		return v4x_host_unsupported("comparison and unnormalized coordinates");
	// The most anisotropy goes to xsdk's sampler, which rounds it down to a power of two.
	if (info->anisotropyEnable) {
		if (!device->sampler_anisotropy || info->maxAnisotropy < 1.0f || info->maxAnisotropy > XSDK_MAX_ANISOTROPY)
			return v4x_host_unsupported("anisotropy without the device's samplerAnisotropy, or past 1 to 16");
		setting.max_anisotropy = (uint32_t)info->maxAnisotropy;
	}
	if (info->mipLodBias < -MAX_LOD_BIAS || info->mipLodBias > MAX_LOD_BIAS)
		return v4x_host_unsupported("LOD biases beyond 15");
	// minLod and maxLod to the nearest levels; a maxLod past the last level, VK_LOD_CLAMP_NONE's included, keeps
	// them all.
	if (info->minLod < 0.0f || info->minLod > (float)MAX_LEVEL || info->maxLod < info->minLod)
		return v4x_host_unsupported("a minimum LOD past 15, or a maximum below it");
	setting.min_level = nearest_level(info->minLod);
	if (info->maxLod < (float)MAX_LEVEL + 0.5f)
		setting.level_count = nearest_level(info->maxLod) + 1;
	Sampler *const sampler = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *sampler, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!sampler)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	xsdk_texture_sampler_state(&sampler->state, &setting);
	*pSampler = v4x_handle(sampler);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(sampler));
}
