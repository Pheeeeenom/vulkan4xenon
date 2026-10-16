#ifndef V4X_SYNC_H
#define V4X_SYNC_H

#include <stdint.h>

#include "objects.h"

// Marks a fence, if any, as signalled once the GPU passes xsdk's fence number.
void v4x_sync_submitted(Fence *fence, uint32_t number);
// Marks a fence, if any, as signalled now.
void v4x_sync_signal(Fence *fence);
// The fence a submission ended with, by its serial; one that's being replayed has none yet, so the replay makes a
// fence of its own for it.
uint32_t v4x_sync_fence_of(VkQueue queue, uint32_t serial);

#endif
