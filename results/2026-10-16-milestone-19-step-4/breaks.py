"""Breaks milestone 19 step 4's checks, or restores them: step4_breaks.py apply a|b|c|d, or restore.

Set a breaks the colour, depth and stencil clears, the sampled image's fill, the refusal and the names, a check each;
b ignores the image's pending clear, which the depth cells after it lean on too; c leaves the pipeline's state unset
after a clear; d refuses depth images made to be cleared, and with them everything after.
"""
import pathlib
import sys

BACKUP = pathlib.Path(__file__).with_name("step4_backup")
LAYER = pathlib.Path("/mnt/z/vulkan4xenon")

SETS = {
    "a": [
        (LAYER / "src/blit.c", ".write_mask = clear->clears_color ? XSDK_WRITE_ALL : 0 };", ".write_mask = 0 };"),
        (LAYER / "src/blit.c", ".depth_write = clear->clears_depth,", ".depth_write = false,"),
        (LAYER / "src/blit.c", ".stencil_test = clear->clears_stencil,", ".stencil_test = false,"),
        (LAYER / "src/queue.c", "\tif (image->is_sampled_depth)\n", "\tif (!image)\n"),
        (LAYER / "src/command_buffer.c", "pRanges[i].aspectMask != both", "!(pRanges[i].aspectMask & both)"),
        (LAYER / "src/proc_addr.c", "\tENTRY(vkCmdClearAttachments, SCOPE_DEVICE),\n", ""),
    ],
    "b": [
        (LAYER / "src/queue.c", "depth->clear_pending && !replay->clears_depth", "depth->clear_pending && !depth"),
    ],
    "c": [
        (LAYER / "src/queue.c",
         "replay->framebuffer->height);\n\treplay->pipeline_dirty = true;\n",
         "replay->framebuffer->height);\n"),
    ],
    "d": [
        (LAYER / "src/image.h", "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,",
         "VK_IMAGE_USAGE_SAMPLED_BIT,"),
    ],
}


def backup_name(path):
    return str(path).replace("/", "_")


def apply(name):
    BACKUP.mkdir(exist_ok=True)
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = path.read_text()
            (BACKUP / backup_name(path)).write_text(texts[path])
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        path.write_text(text)
    (BACKUP / "applied").write_text("\n".join(str(p) for p in texts))
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


def restore():
    for path in (BACKUP / "applied").read_text().split("\n"):
        pathlib.Path(path).write_text((BACKUP / backup_name(path)).read_text())
    (BACKUP / "applied").unlink()
    print("restored")


if sys.argv[1] == "apply":
    apply(sys.argv[2])
else:
    restore()
