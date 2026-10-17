"""Breaks milestone 27's checks in a copy of the layer, a set at a time: breaks.py <copy> <set>, or breaks.py --list.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own. Sets whose breaks would fail the same checks are kept apart.

Step 1, render areas smaller than the framebuffer:
- a: the granularity 1x1; areas off the grid taken, and so blits' regions too; a smaller area of a half float
  attachment taken; a load finding its image anywhere in eDRAM, not over its area; the depth cleared for the next pass
  over an area that holds the next's, not the same; the depth image's own clear drawn over the pass's area alone.
- b: every pass's resolve of the whole framebuffer, which spoils a depth image's clear before a smaller pass too.
- c: the image drawn back with the scissor at the area's size from the corner.

Step 2, blits drawn on the GPU:
- d: linear blits sampled nearest; each blit's resolve of the whole destination; the drawn blit waiting for the GPU's
  work before it, as the CPU's does; regions off the grid taken.
- e: the source region's corner dropped from the texture coordinates.
- f: a region mirrored by its destination drawn unmirrored.
"""
import pathlib
import sys

GRANULARITY = "render areas: the granularity is 8 by 8"
# test_commands.c's check of the granularity, which milestone 27 made 8x8.
COMMANDS_GRANULARITY = "commands: a render pass's render area granularity is 8x8, the resolve's grid (milestone 27)"
REFUSED = "render areas: areas off the 8-pixel grid inside the framebuffer are refused"
REFUSED_FLOAT = "render areas: an area smaller than the framebuffer of a half float colour attachment is refused"
CLEARED = ("render areas: a pass clearing an area inside the framebuffer clears and draws there alone, and stores "
           "no more")
LOADED = ("render areas: a pass loading an area that reaches the framebuffer's edges, off the 8-pixel grid, loads and "
          "stores it alone, the area before it in place")
DEPTH = "render areas: a pass clearing an area's depth leaves the depth around it"
IMAGE_CLEAR = "render areas: a depth image's clear covers all of it, before a pass of a smaller area"
MIRRORED = "blits: a region mirrored across by its destination and down by its source, nearest"
LINEAR_UP = "blits: a region scaled up 2 times, linear"
LINEAR_DOWN = "blits: a region at an odd offset scaled down 2 times, linear"
EDGES = "blits: a region reaching the destination's edges, off the 8-pixel grid"
AROUND = "blits: the destination around the rectangles keeps its texels"
WAIT = "blits: a blit drawn on the GPU doesn't wait for it, a blit on the CPU does"
OFF_GRID = "blits: a region off the 8-pixel grid inside the destination is refused"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("src/render_pass.c", "(VkExtent2D){ XSDK_RESOLVE_ALIGNMENT, XSDK_RESOLVE_ALIGNMENT }", "(VkExtent2D){ 1, 1 }",
         [GRANULARITY, COMMANDS_GRANULARITY]),
        ("src/command_buffer.c",
         "first % XSDK_RESOLVE_ALIGNMENT == 0 &&\n\t\t(end % XSDK_RESOLVE_ALIGNMENT == 0 || end == edge);", "true;",
         [REFUSED, OFF_GRID]),
        ("src/command_buffer.c", "subpass->color_count != 1 || subpass->targets[0] != XSDK_TARGET_8_8_8_8",
         "subpass->color_count != 1", [REFUSED_FLOAT]),
        ("src/queue.c",
         "color->image == framebuffer->attachments[target] &&\n\t\t\tcontains(&color->area, &replay->area);",
         "color->image == framebuffer->attachments[target];", [LOADED]),
        ("src/queue.c", "contains(&replay->area, next_area) &&\n\t\tcontains(next_area, &replay->area);",
         "contains(&replay->area, next_area);", [DEPTH]),
        ("src/queue.c", "const ClearRect clear = { .rect = whole(framebuffer),",
         "const ClearRect clear = { .rect = replay->area,", [IMAGE_CLEAR]),
    ],
    "b": [
        ("src/queue.c", "\tconst VkRect2D *const area = &replay->area;\n\txsdk_gpu_set_resolve_clears",
         "\tconst VkRect2D all = { { 0, 0 }, { attachment->width, attachment->height } };\n"
         "\tconst VkRect2D *const area = &all;\n\txsdk_gpu_set_resolve_clears", [CLEARED, LOADED, DEPTH, IMAGE_CLEAR]),
    ],
    "c": [
        ("src/blit.c", "xsdk_gpu_set_scissor((uint32_t)area->offset.x, (uint32_t)area->offset.y,",
         "xsdk_gpu_set_scissor(0, 0,", [LOADED]),
    ],
    "d": [
        ("src/blit.c", "region->linear ? &blit->linear_sampler : &blit->sampler;", "&blit->sampler;",
         [LINEAR_UP, LINEAR_DOWN]),
        ("src/queue.c",
         "xsdk_gpu_resolve_texture_rect(&destination->texture, (uint32_t)rect->offset.x, (uint32_t)rect->offset.y,\n"
         "\t\trect->extent.width, rect->extent.height, 0);", "xsdk_gpu_resolve_texture(&destination->texture, 0);",
         [AROUND]),
        ("src/queue.c", "const DrawnBlit *blit)\n{\n",
         "const DrawnBlit *blit)\n{\n\t(void)xsdk_gpu_wait(xsdk_gpu_fence(), 10000);\n", [WAIT]),
        ("src/command_buffer.c",
         "XSDK_TARGET_8_8_8_8 ||\n\t\t!on_grid(rect.offset.x, rect.extent.width, destination->width) ||\n"
         "\t\t!on_grid(rect.offset.y, rect.extent.height, destination->height))", "XSDK_TARGET_8_8_8_8)", [OFF_GRID]),
    ],
    "e": [
        ("src/command_buffer.c", ".uv = { left, top, right - left, bottom - top }",
         ".uv = { 0.0f, 0.0f, right - left, bottom - top }", [MIRRORED, LINEAR_DOWN, EDGES]),
    ],
    "f": [
        ("src/command_buffer.c", "const bool across = to[0].x <= to[1].x;", "const bool across = true;", [MIRRORED]),
    ],
}


def apply(root, name):
    texts = {}
    for path, old, new, checks in SETS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
        print(f"{name}: {path}: should fail {'; '.join(checks)}")
    for file, text in texts.items():
        file.write_text(text)


if sys.argv[1] == "--list":
    print(" ".join(SETS))
else:
    apply(pathlib.Path(sys.argv[1]), sys.argv[2])
