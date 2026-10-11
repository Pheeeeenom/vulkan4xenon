#ifndef V4X_PHYSICAL_DEVICE_H
#define V4X_PHYSICAL_DEVICE_H

#include <stdbool.h>

// Whether vkCreateDevice can enable this device extension.
bool v4x_physical_device_has_extension(const char *name);

#endif
