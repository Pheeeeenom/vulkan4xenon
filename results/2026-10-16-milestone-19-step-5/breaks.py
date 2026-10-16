"""Breaks milestone 19 step 5's checks, or restores them: step5_breaks.py apply a|b|c, or restore.

Set a skips secondary command buffers, has a later subpass take its attachment's own load op, takes input attachments
and drops vkCmdNextSubpass's name, a check each; b clears depth again between subpasses, which the second subpass's
check catches too, so it goes alone; c refuses secondary command buffers, and with them the scene.
"""
import pathlib
import sys

BACKUP = pathlib.Path(__file__).with_name("step5_backup")
LAYER = pathlib.Path("/mnt/z/vulkan4xenon")

SETS = {
    "a": [
        (LAYER / "src/queue.c", "c < command->secondary->count; c++)", "c < command->secondary->count * 0; c++)"),
        (LAYER / "src/render_pass.c", "uses[color].first == index ? target->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD",
         "target->loadOp"),
        (LAYER / "src/render_pass.c", "subpass->inputAttachmentCount ||", ""),
        (LAYER / "src/proc_addr.c", "\tENTRY(vkCmdNextSubpass, SCOPE_DEVICE),\n", ""),
    ],
    "b": [
        (LAYER / "src/render_pass.c", "pass.clears_depth = first &&", "pass.clears_depth = (first || !first) &&"),
    ],
    "c": [
        (LAYER / "src/command_buffer.c", "\tCommandPool *const pool = v4x_object(pAllocateInfo->commandPool);\n",
         "\tif (pAllocateInfo->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)\n"
         "\t\treturn v4x_host_unsupported(\"secondary command buffers\");\n"
         "\tCommandPool *const pool = v4x_object(pAllocateInfo->commandPool);\n"),
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
