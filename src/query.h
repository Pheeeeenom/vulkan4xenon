#ifndef V4X_QUERY_H
#define V4X_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#include "objects.h"

// Whether a query has been written since its reset, and its value: the GPU clock it holds, or an occlusion query's
// 0 or 1.
bool v4x_query_value(const QueryPool *pool, uint32_t query, uint64_t *value);

#endif
