#ifndef V4X_QUERY_H
#define V4X_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#include "objects.h"

// Whether a query's time stamp has been written since its reset, and the GPU clock it holds.
bool v4x_query_value(const QueryPool *pool, uint32_t query, uint64_t *value);

#endif
