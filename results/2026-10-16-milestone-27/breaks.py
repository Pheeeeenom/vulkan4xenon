"""Breaks milestone 27's checks in a copy of the layer, a set at a time: breaks.py <copy> <set>, or breaks.py --list.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own. Sets whose breaks would fail the same checks are kept apart.

Step 1, render areas smaller than the framebuffer:
- a: the granularity 1x1; areas off the grid taken; a smaller area of a half float attachment taken; a load finding
  its image anywhere in eDRAM, not over its area; the depth cleared for the next pass over an area that holds the
  next's, not the same; the depth image's own clear drawn over the pass's area alone.
- b: every pass's resolve of the whole framebuffer.
- c: the image drawn back with the scissor at the area's size from the corner.
"""
import pathlib
import sys

GRANULARITY = "render areas: the granularity is 8 by 8"
REFUSED = "render areas: areas off the 8-pixel grid inside the framebuffer are refused"
REFUSED_FLOAT = "render areas: an area smaller than the framebuffer of a half float colour attachment is refused"
CLEARED = ("render areas: a pass clearing an area inside the framebuffer clears and draws there alone, and stores "
           "no more")
LOADED = ("render areas: a pass loading an area that reaches the framebuffer's edges, off the 8-pixel grid, loads and "
          "stores it alone, the area before it in place")
DEPTH = "render areas: a pass clearing an area's depth leaves the depth around it"
IMAGE_CLEAR = "render areas: a depth image's clear covers all of it, before a pass of a smaller area"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("src/render_pass.c", "(VkExtent2D){ XSDK_RESOLVE_ALIGNMENT, XSDK_RESOLVE_ALIGNMENT }", "(VkExtent2D){ 1, 1 }",
         [GRANULARITY]),
        ("src/command_buffer.c",
         "first % XSDK_RESOLVE_ALIGNMENT == 0 &&\n\t\t(end % XSDK_RESOLVE_ALIGNMENT == 0 || end == edge);", "true;",
         [REFUSED]),
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
         "\tconst VkRect2D *const area = &all;\n\txsdk_gpu_set_resolve_clears", [CLEARED, LOADED, DEPTH]),
    ],
    "c": [
        ("src/blit.c", "xsdk_gpu_set_scissor((uint32_t)area->offset.x, (uint32_t)area->offset.y,",
         "xsdk_gpu_set_scissor(0, 0,", [LOADED]),
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
