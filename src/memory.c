#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

enum {
	// A vertex fetch constant needs a dword address (xsdk:src/gpu_draw.c), and the GPU loads uniform blocks from
	// 64-byte aligned memory (xsdk:include/xsdk/gpu.h).
	BUFFER_ALIGNMENT = 64,
	MEMORY_TYPE_BITS = 1,
};

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
	const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
	if (pAllocateInfo->memoryTypeIndex != 0)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (pAllocateInfo->allocationSize > UINT32_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, pAllocator);
	DeviceMemory *const memory = v4x_host_alloc(allocator, sizeof *memory, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!memory)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memory->size_bytes = (uint32_t)pAllocateInfo->allocationSize;
	memory->pointer = xsdk_gpu_alloc(memory->size_bytes);
	if (!memory->pointer) {
		xsdk_log_write("vulkan4xenon: xsdk_gpu_alloc(%u) failed", (unsigned)memory->size_bytes);
		v4x_host_free(allocator, memory);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	memory->address = xsdk_gpu_address(memory->pointer);
	*pMemory = v4x_handle(memory);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
	DeviceMemory *const object = v4x_object(memory);
	if (!object)
		return;
	xsdk_gpu_free(object->pointer);
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), object);
}

// Write-combined memory: memcpy into it is safe, and an 8-byte store needs an 8-byte-aligned address (docs/SUPPORT.md).
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
	VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
	(void)device;
	(void)size;
	(void)flags;
	const DeviceMemory *const object = v4x_object(memory);
	*ppData = (uint8_t *)(uintptr_t)object->pointer + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
	(void)device;
	(void)memory;
}

// Coherent: vkQueueSubmit drains the CPU's write-combining before the GPU reads.
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
	VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
	(void)device;
	(void)memoryRangeCount;
	(void)pMemoryRanges;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
	VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
	(void)device;
	(void)memoryRangeCount;
	(void)pMemoryRanges;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
	VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
	if (pCreateInfo->flags)
		return v4x_host_unsupported("sparse buffers");
	Buffer *const buffer = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *buffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!buffer)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	buffer->size = pCreateInfo->size;
	*pBuffer = v4x_handle(buffer);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(buffer));
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
	VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements)
{
	(void)device;
	const Buffer *const object = v4x_object(buffer);
	*pMemoryRequirements = (VkMemoryRequirements){
		.size = (object->size + BUFFER_ALIGNMENT - 1) & ~(VkDeviceSize)(BUFFER_ALIGNMENT - 1),
		.alignment = BUFFER_ALIGNMENT,
		.memoryTypeBits = MEMORY_TYPE_BITS,
	};
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
	VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
	(void)device;
	Buffer *const object = v4x_object(buffer);
	object->memory = v4x_object(memory);
	object->offset_bytes = (uint32_t)memoryOffset;
	return VK_SUCCESS;
}
