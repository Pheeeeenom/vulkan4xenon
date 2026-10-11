#ifndef V4X_CONSTANT_RING_H
#define V4X_CONSTANT_RING_H

#include <stdint.h>

#include <vulkan/vulkan.h>

enum {
	// Submits the ring keeps bytes for until the GPU passes them.
	V4X_RING_SUBMITS = 16,
};

// Write-combined memory the replay copies push constants into for the GPU to load (docs/ROADMAP.md, milestone 8).
typedef struct {
	volatile uint32_t *memory;
	uint32_t address;
	// Bytes taken since the ring was made, wrapping; where the next goes is this modulo the ring's size.
	uint32_t taken;
	uint32_t submit_start;
	// The submits the GPU hasn't passed, oldest first: where their bytes began, and their fences.
	uint32_t starts[V4X_RING_SUBMITS];
	uint32_t fences[V4X_RING_SUBMITS];
	uint32_t first;
	uint32_t count;
} ConstantRing;

VkResult v4x_ring_create(ConstantRing *ring);
void v4x_ring_destroy(ConstantRing *ring);
// Before a submit replays: forgets the submits the GPU has passed.
void v4x_ring_begin(ConstantRing *ring);
// Room for size_bytes, a multiple of 64, at a 64-byte aligned address the GPU loads from; NULL when this submit would
// lap the ring by itself.
volatile uint32_t *v4x_ring_take(ConstantRing *ring, uint32_t size_bytes, uint32_t *address);
// After the submit's fence: its bytes stay until the GPU passes it.
void v4x_ring_end(ConstantRing *ring, uint32_t fence);

#endif
