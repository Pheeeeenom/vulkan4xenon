#include "shader.h"

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "vulkan4xenon.h"
#include "xsdk/log.h"

enum {
	FNV_OFFSET_BASIS_HIGH = 0xCBF29CE4,
	FNV_OFFSET_BASIS_LOW = 0x84222325,
	FNV_PRIME_HIGH = 0x00000100,
	FNV_PRIME_LOW = 0x000001B3,
};

// Weak, so an app that links no bundle still links: the address is then NULL. A weak definition instead gets folded.
extern const V4xShaderBundle v4x_shader_bundle __attribute__((weak));

// 64-bit FNV-1a over each word's bytes, low first, so the tool and the console agree whatever their byte order.
static uint64_t spirv_hash(const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t prime = (uint64_t)FNV_PRIME_HIGH << 32 | FNV_PRIME_LOW;
	uint64_t hash = (uint64_t)FNV_OFFSET_BASIS_HIGH << 32 | FNV_OFFSET_BASIS_LOW;
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			hash ^= code[i] >> shift & 0xFF;
			hash *= prime;
		}
	}
	return hash;
}

const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t hash = spirv_hash(code, size_bytes);
	for (uint32_t i = 0; bundle && i < bundle->count; i++) {
		const V4xShader *const shader = &bundle->shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == size_bytes)
			return shader;
	}
	xsdk_log_write("vulkan4xenon: no compiled shader for SPIR-V %08X%08X, %u bytes", (unsigned)(hash >> 32),
		(unsigned)hash, (unsigned)size_bytes);
	return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
	const V4xShader *const found =
		v4x_shader_find(&v4x_shader_bundle, pCreateInfo->pCode, (uint32_t)pCreateInfo->codeSize);
	if (!found)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	ShaderModule *const module = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *module, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!module)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	module->shader = found;
	*pShaderModule = v4x_handle(module);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
	VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(shaderModule));
}
