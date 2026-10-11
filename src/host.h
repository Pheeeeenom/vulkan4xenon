#ifndef V4X_HOST_H
#define V4X_HOST_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

// Host memory for the layer's objects, and the one line it logs for what it doesn't do.

// Zeroed memory from the app's allocator if it gave one, else the C library's; NULL when there's none.
void *v4x_host_alloc(const VkAllocationCallbacks *allocator, size_t size_bytes, VkSystemAllocationScope scope);
void v4x_host_free(const VkAllocationCallbacks *allocator, void *memory);
// The allocator a device's object uses: the one passed with the call, else the device's.
const VkAllocationCallbacks *v4x_host_device_allocator(VkDevice device, const VkAllocationCallbacks *given);
// Logs what isn't implemented and returns VK_ERROR_FEATURE_NOT_PRESENT.
VkResult v4x_host_unsupported(const char *what);
// Vulkan's count-then-array query: the count when out is NULL, else as many items as fit, VK_INCOMPLETE if not all.
VkResult v4x_host_enumerate(uint32_t *count, void *out, const void *items, uint32_t item_count, size_t item_bytes);

#endif
