#include "harness.h"
#include "tests.h"
#include "xsdk/display.h"
#include "xsdk/log.h"

static int passed_count;
static int failed_count;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

int main(void)
{
	xsdk_log_write("vulkan4xenon test title");
	Harness harness = { .instance = VK_NULL_HANDLE };
	// XAM's surface, which the boot logo should replace.
	const uint32_t shown_before_device = xsdk_display_shown_address();
	const bool made = harness_create_device(&harness) == VK_SUCCESS;
	check("setup: instance, device, command pool and fence made", made);
	if (made) {
		test_boot_logo(&harness, shown_before_device);
		test_objects(&harness);
		test_triangle(&harness);
		test_texture(&harness);
		test_push_constants(&harness);
		test_uniform_buffers(&harness);
		test_indexed(&harness);
		test_depth_stencil(&harness);
		test_raster(&harness);
	}
	harness_destroy(&harness);
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
