"""Milestone 14, step 5, local diagnostics only, never committed: vulkan4xenon logs, up to 120 lines, each pass whose
depth or colour takes the tiles of a depth image eDRAM held, and each load of a depth eDRAM lost, with the sizes, so a
run shows which pass costs the EFB its depth. Reverted by checking src/queue.c out again."""

from pathlib import Path

Z = next(p for p in (Path("/mnt/z"), Path("/z"), Path("Z:/")) if (p / "vulkan4xenon").exists())
p = Z / "vulkan4xenon/src/queue.c"
s = p.read_bytes().decode()
assert "\r\n" not in s


def rep(old, new):
    global s
    assert s.count(old) == 1, old[:80]
    s = s.replace(old, new)


rep("""// Colour reaching the tiles depth holds overwrites it.
static void cover_depth(VkQueue queue, const Framebuffer *framebuffer)
{
	const EdramPart *const depth = &queue->depth;
	if (depth->contents == EDRAM_UNKNOWN)
		return;
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(depth->width, depth->height);
	if (xsdk_gpu_target_tiles(framebuffer->width, framebuffer->height) > depth_tile)
		queue->depth = (EdramPart){ .contents = EDRAM_UNKNOWN };
}""", """static uint32_t diag_lines;

// Colour reaching the tiles depth holds overwrites it.
static void cover_depth(VkQueue queue, const Framebuffer *framebuffer)
{
	const EdramPart *const depth = &queue->depth;
	if (depth->contents == EDRAM_UNKNOWN)
		return;
	if (depth->contents == EDRAM_IMAGE && framebuffer->depth && framebuffer->depth != depth->image &&
		diag_lines < 120) {
		diag_lines++;
		xsdk_log_write("diag depth: a %ux%u pass's depth (image %p) takes eDRAM from %ux%u depth (image %p)",
			(unsigned)framebuffer->width, (unsigned)framebuffer->height, (void *)framebuffer->depth,
			(unsigned)depth->width, (unsigned)depth->height, (const void *)depth->image);
	}
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(depth->width, depth->height);
	if (xsdk_gpu_target_tiles(framebuffer->width, framebuffer->height) > depth_tile) {
		if (diag_lines < 120) {
			diag_lines++;
			xsdk_log_write("diag depth: a %ux%u pass's colour covers %ux%u depth (image %p)",
				(unsigned)framebuffer->width, (unsigned)framebuffer->height, (unsigned)depth->width,
				(unsigned)depth->height, (const void *)depth->image);
		}
		queue->depth = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
}"""),
rep("""	static bool logged;
	if (logged || (queue->depth.contents == EDRAM_IMAGE && queue->depth.image == framebuffer->depth))
		return;""", """	static bool logged;
	if (queue->depth.contents == EDRAM_IMAGE && queue->depth.image == framebuffer->depth)
		return;
	if (diag_lines < 120) {
		diag_lines++;
		xsdk_log_write("diag depth: a %ux%u pass loads depth (image %p) eDRAM lost; eDRAM has %s %ux%u (image %p)",
			(unsigned)framebuffer->width, (unsigned)framebuffer->height, (void *)framebuffer->depth,
			queue->depth.contents == EDRAM_IMAGE ? "image" : queue->depth.contents == EDRAM_CLEARED ? "cleared" : "unknown",
			(unsigned)queue->depth.width, (unsigned)queue->depth.height, (const void *)queue->depth.image);
	}
	if (logged)
		return;""")
p.write_bytes(s.encode())
print("layer depth log applied")
