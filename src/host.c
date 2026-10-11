#include "host.h"

#include <stdlib.h>
#include <string.h>

#include "objects.h"
#include "xsdk/log.h"

enum {
	// What newlib's malloc guarantees on this target, and all any of the layer's objects need.
	HOST_ALIGNMENT = 8,
};

void *v4x_host_alloc(const VkAllocationCallbacks *allocator, size_t size_bytes, VkSystemAllocationScope scope)
{
	void *const memory = allocator ? allocator->pfnAllocation(allocator->pUserData, size_bytes, HOST_ALIGNMENT, scope)
								   : malloc(size_bytes);
	if (memory)
		memset(memory, 0, size_bytes);
	return memory;
}

void v4x_host_free(const VkAllocationCallbacks *allocator, void *memory)
{
	if (!memory)
		return;
	if (allocator)
		allocator->pfnFree(allocator->pUserData, memory);
	else
		free(memory);
}

const VkAllocationCallbacks *v4x_host_device_allocator(VkDevice device, const VkAllocationCallbacks *given)
{
	if (given)
		return given;
	return device->has_allocator ? &device->allocator : NULL;
}

VkResult v4x_host_unsupported(const char *what)
{
	xsdk_log_write("vulkan4xenon: not supported: %s", what);
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult v4x_host_enumerate(uint32_t *count, void *out, const void *items, uint32_t item_count, size_t item_bytes)
{
	if (!out) {
		*count = item_count;
		return VK_SUCCESS;
	}
	const uint32_t copied = *count < item_count ? *count : item_count;
	memcpy(out, items, copied * item_bytes);
	*count = copied;
	return copied < item_count ? VK_INCOMPLETE : VK_SUCCESS;
}
