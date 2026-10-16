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

static bool address_mode(VkSamplerAddressMode mode, uint32_t *xsdk_mode)
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
	default:
		return false;
	}
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
	if (!address_mode(info->addressModeU, &setting.address_u) || !address_mode(info->addressModeV, &setting.address_v))
		return v4x_host_unsupported("address modes other than repeat, mirrored repeat and clamp to edge");
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
