#ifndef V4X_SYNC_H
#define V4X_SYNC_H

#include <stdint.h>

#include "objects.h"

// Marks a fence, if any, as signalled once the GPU passes xsdk's fence number.
void v4x_sync_submitted(Fence *fence, uint32_t number);
// Marks a fence, if any, as signalled now.
void v4x_sync_signal(Fence *fence);

#endif
