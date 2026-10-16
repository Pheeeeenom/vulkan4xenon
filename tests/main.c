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
	test_shader_cache_start();
	const bool made = harness_create_device(&harness) == VK_SUCCESS;
	check("setup: instance, device, command pool and fence made", made);
	if (made) {
		test_boot_logo(&harness, shown_before_device);
		test_objects(&harness);
		test_triangle(&harness);
		test_packed_colour(&harness);
		test_texture(&harness);
		test_push_constants(&harness);
		test_uniform_buffers(&harness);
		test_indexed(&harness);
		test_depth_stencil(&harness);
		test_raster(&harness);
		test_render_to_texture(&harness);
		test_run_time_index(&harness);
		test_uploads(&harness);
		test_thin_textures(&harness);
		test_sampled_depth(&harness);
		test_anisotropy(&harness);
		test_aurora_points(&harness);
		test_lod(&harness);
	}
	harness_destroy(&harness);
	if (made)
		test_shader_cache_finish();
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
