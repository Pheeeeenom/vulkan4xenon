#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/texture.h"

enum {
	// The fetch constant's LOD bias runs from -16 to under 16 (xsdk:include/xsdk/texture.h).
	MAX_LOD_BIAS = 15,
};

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
	if (info->minLod != 0.0f || info->mipLodBias < -MAX_LOD_BIAS || info->mipLodBias > MAX_LOD_BIAS)
		return v4x_host_unsupported("a minimum LOD above 0, and LOD biases beyond 15");
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
