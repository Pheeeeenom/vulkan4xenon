#ifndef V4X_TESTS_H
#define V4X_TESTS_H

#include <stdint.h>

#include "harness.h"

// The logo shows from vkCreateDevice until the first present; called with the device made and no swapchain yet.
void test_boot_logo(Harness *harness, uint32_t shown_before_device);
// The proc-address table, memory types, buffer requirements and refusals.
void test_objects(const Harness *harness);
// A triangle through Vulkan, checked on the image the display shows.
void test_triangle(const Harness *harness);

#endif
