#include "sync.h"

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

enum {
	NANOSECONDS_PER_MILLISECOND = 1000000,
	// How long one xsdk_gpu_wait lasts when a wait has no end.
	WAIT_SLICE_MS = 1000,
};

void v4x_sync_submitted(Fence *fence, uint32_t number)
{
	if (!fence)
		return;
	fence->pending = true;
	fence->number = number;
}

void v4x_sync_signal(Fence *fence)
{
	if (fence)
		fence->signaled = true;
}

static bool signaled(Fence *fence)
{
	if (!fence->signaled && fence->pending && xsdk_gpu_fence_done(fence->number)) {
		fence->signaled = true;
		fence->pending = false;
	}
	return fence->signaled;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
	VkDevice device, const VkFenceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
	Fence *const fence =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *fence, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!fence)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	fence->signaled = pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT;
	*pFence = v4x_handle(fence);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(fence));
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
	(void)device;
	for (uint32_t i = 0; i < fenceCount; i++)
		*(Fence *)v4x_object(pFences[i]) = (Fence){ .signaled = false };
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{
	(void)device;
	return signaled(v4x_object(fence)) ? VK_SUCCESS : VK_NOT_READY;
}

// Waits on the GPU for one fence, up to timeout_ns; UINT64_MAX never ends.
static VkResult wait_one(Fence *fence, uint64_t timeout_ns)
{
	if (signaled(fence))
		return VK_SUCCESS;
	if (!fence->pending)
		return VK_TIMEOUT;
	const bool forever = timeout_ns == UINT64_MAX;
	uint64_t left_ms = (timeout_ns + NANOSECONDS_PER_MILLISECOND - 1) / NANOSECONDS_PER_MILLISECOND;
	while (forever || left_ms) {
		const uint32_t slice = forever || left_ms > WAIT_SLICE_MS ? WAIT_SLICE_MS : (uint32_t)left_ms;
		if (NT_SUCCESS(xsdk_gpu_wait(fence->number, slice)))
			return signaled(fence) ? VK_SUCCESS : VK_TIMEOUT;
		left_ms -= forever ? 0 : slice;
	}
	return VK_TIMEOUT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
	VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout)
{
	(void)device;
	if (waitAll) {
		for (uint32_t i = 0; i < fenceCount; i++) {
			const VkResult result = wait_one(v4x_object(pFences[i]), timeout);
			if (result != VK_SUCCESS)
				return result;
		}
		return VK_SUCCESS;
	}
	// One queue signals fences in submission order, so the earliest pending fence signals first.
	Fence *earliest = NULL;
	for (uint32_t i = 0; i < fenceCount; i++) {
		Fence *const fence = v4x_object(pFences[i]);
		if (signaled(fence))
			return VK_SUCCESS;
		if (fence->pending && (!earliest || (int32_t)(fence->number - earliest->number) < 0))
			earliest = fence;
	}
	return earliest ? wait_one(earliest, timeout) : VK_TIMEOUT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore)
{
	(void)pCreateInfo;
	Semaphore *const semaphore = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *semaphore, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!semaphore)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*pSemaphore = v4x_handle(semaphore);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
	VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(semaphore));
}
