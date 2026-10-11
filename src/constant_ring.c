#include "constant_ring.h"

#include <assert.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	// vkbench's 4000 pushed cubes take 768 KB a submit.
	RING_BYTES = 4 * 1024 * 1024,
	// The GPU loads constants from 64-byte aligned memory (xsdk:include/xsdk/gpu.h).
	ALIGNMENT_BYTES = 64,
	WAIT_TIMEOUT_MS = 10000,
};

VkResult v4x_ring_create(ConstantRing *ring)
{
	*ring = (ConstantRing){ .memory = xsdk_gpu_alloc(RING_BYTES) };
	if (!ring->memory)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	ring->address = xsdk_gpu_address(ring->memory);
	return VK_SUCCESS;
}

void v4x_ring_destroy(ConstantRing *ring)
{
	xsdk_gpu_free(ring->memory);
	*ring = (ConstantRing){ .memory = NULL };
}

static void forget_oldest(ConstantRing *ring)
{
	ring->first = (ring->first + 1) % V4X_RING_SUBMITS;
	ring->count--;
}

static void wait_oldest(ConstantRing *ring)
{
	const NTSTATUS status = xsdk_gpu_wait(ring->fences[ring->first], WAIT_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: the GPU didn't pass a submit holding push constants: 0x%08X", (unsigned)status);
	forget_oldest(ring);
}

void v4x_ring_begin(ConstantRing *ring)
{
	while (ring->count && xsdk_gpu_fence_done(ring->fences[ring->first]))
		forget_oldest(ring);
	ring->submit_start = ring->taken;
}

volatile uint32_t *v4x_ring_take(ConstantRing *ring, uint32_t size_bytes, uint32_t *address)
{
	assert(size_bytes % ALIGNMENT_BYTES == 0 && size_bytes <= RING_BYTES);
	// A take never wraps: what's left at the end is skipped.
	const uint32_t offset = ring->taken % RING_BYTES;
	const uint32_t skip = offset + size_bytes > RING_BYTES ? RING_BYTES - offset : 0;
	for (;;) {
		const uint32_t oldest = ring->count ? ring->starts[ring->first] : ring->submit_start;
		if (ring->taken + skip + size_bytes - oldest <= RING_BYTES)
			break;
		if (!ring->count)
			return NULL;
		wait_oldest(ring);
	}
	ring->taken += skip;
	const uint32_t start = ring->taken % RING_BYTES;
	ring->taken += size_bytes;
	*address = ring->address + start;
	return ring->memory + start / 4;
}

void v4x_ring_end(ConstantRing *ring, uint32_t fence)
{
	if (ring->taken == ring->submit_start)
		return;
	if (ring->count == V4X_RING_SUBMITS)
		wait_oldest(ring);
	const uint32_t slot = (ring->first + ring->count) % V4X_RING_SUBMITS;
	ring->starts[slot] = ring->submit_start;
	ring->fences[slot] = fence;
	ring->count++;
}
