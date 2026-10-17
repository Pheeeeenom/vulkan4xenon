#include "query.h"

#include <string.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "sync.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Time stamp queries: each query's slot takes the GPU clock's three words from xsdk_gpu_write_clock, the last of them
// written last, so a slot whose last word isn't V4X_QUERY_UNWRITTEN holds a whole time stamp. Occlusion queries take
// D3D's visibility query, as Xenos's sample counts hang it (xsdk:docs/HARDWARE.md, "Occlusion queries, vulkan4xenon
// milestone 28"): a slot takes the status word, then the last visibility id the query took, ids 0 to it its own
// (queue.c). Pipeline statistics need a feature the device doesn't report.

enum {
	IDLE_TIMEOUT_MS = 10000,
};

bool v4x_query_value(const QueryPool *pool, uint32_t query, uint64_t *value)
{
	volatile const uint32_t *const slot = &pool->slots[query * V4X_QUERY_SLOT_WORDS];
	const uint32_t last = slot[V4X_QUERY_LAST_WORD];
	if (last == V4X_QUERY_UNWRITTEN)
		return false;
	// The words before it landed first; the loads are kept after it.
	__asm__ volatile("sync" ::: "memory");
	// 1 if any of the query's ids has its bit set, which can't count samples (docs/SUPPORT.md, Queries).
	*value = pool->occlusion ? (slot[0] & ((2u << last) - 1)) != 0 : xsdk_gpu_clock_value(slot);
	return true;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool)
{
	if (pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP && pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION)
		return v4x_host_unsupported("pipeline statistics queries");
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, pAllocator);
	QueryPool *const pool = v4x_host_alloc(allocator, sizeof *pool, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!pool)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	const uint32_t words = pCreateInfo->queryCount * V4X_QUERY_SLOT_WORDS;
	pool->slots = xsdk_gpu_alloc(words * sizeof(uint32_t));
	if (!pool->slots) {
		v4x_host_free(allocator, pool);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	for (uint32_t i = 0; i < words; i++)
		pool->slots[i] = V4X_QUERY_UNWRITTEN;
	__asm__ volatile("sync" ::: "memory");
	pool->address = xsdk_gpu_address(pool->slots);
	pool->count = pCreateInfo->queryCount;
	pool->occlusion = pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION;
	pool->used_serial = 0;
	*pQueryPool = v4x_handle(pool);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(
	VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks *pAllocator)
{
	QueryPool *const pool = v4x_object(queryPool);
	if (!pool)
		return;
	xsdk_gpu_free(pool->slots);
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), pool);
}

// Waits for the submission that last wrote the pool, by its fence alone: the ring is the queue's, so the host makes no
// fence of its own. A query still unwritten then never will be.
static VkResult wait_for_pool(VkDevice device, const QueryPool *pool)
{
	if (!pool->used_serial)
		return VK_SUCCESS;
	const NTSTATUS status = xsdk_gpu_wait(v4x_sync_fence_of(&device->queue, pool->used_serial), IDLE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return VK_SUCCESS;
	xsdk_log_write("vulkan4xenon: the GPU didn't finish writing queries: 0x%08X", (unsigned)status);
	return VK_ERROR_DEVICE_LOST;
}

static void store_result(uint8_t *out, uint32_t index, uint64_t value, bool wide)
{
	if (wide) {
		memcpy(out + index * sizeof value, &value, sizeof value);
	} else {
		const uint32_t narrow = (uint32_t)value;
		memcpy(out + index * sizeof narrow, &narrow, sizeof narrow);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
	uint32_t queryCount, size_t dataSize, void *pData, VkDeviceSize stride, VkQueryResultFlags flags)
{
	(void)dataSize;
	const QueryPool *const pool = v4x_object(queryPool);
	if (flags & VK_QUERY_RESULT_WAIT_BIT) {
		const VkResult waited = wait_for_pool(device, pool);
		if (waited != VK_SUCCESS)
			return waited;
	}
	const bool wide = flags & VK_QUERY_RESULT_64_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < queryCount; i++) {
		uint8_t *const out = (uint8_t *)pData + i * stride;
		uint64_t value = 0;
		const bool available = v4x_query_value(pool, firstQuery + i, &value);
		if (available || flags & VK_QUERY_RESULT_PARTIAL_BIT)
			store_result(out, 0, value, wide);
		if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
			store_result(out, 1, available, wide);
		if (!available)
			result = VK_NOT_READY;
	}
	return result;
}
