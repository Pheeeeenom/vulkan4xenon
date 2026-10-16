"""Breaks milestone 19 step 2's checks in the layer, or restores it: step2_breaks.py apply a|b, or restore.

Set a breaks each check but the one that the buffers and images were made; set b breaks that one alone, as a buffer
only copied to and from loses the cached memory type, which test_objects checks too.
"""
import pathlib
import sys

ROOT = pathlib.Path("/mnt/z/vulkan4xenon")
BACKUP = pathlib.Path(__file__).with_name("step2_backup")

SETS = {
    "a": [
        # The GPU-drawn target read back before its resolve ran.
        ("src/transfer.c", "\twait_for_image(queue, image);\n\twait_for_draws(queue);",
         "\twait_for_draws(queue);"),
        # Clears write blue's bytes where red's go.
        ("src/command_buffer.c", "unorm8(rgba[bgra ? 2 : 0])", "unorm8(rgba[bgra ? 0 : 2])"),
        # The plain indirect draw takes its instance count as its vertex count.
        ("src/queue.c", "\t\tdraw(replay, count, parameters[2]);", "\t\tdraw(replay, parameters[1], parameters[2]);"),
        # The indexed one takes its first instance's.
        ("src/queue.c", "\tconst uint32_t count = parameters[0];\n\tif (drawable(replay, count, parameters[1], parameters[4]))",
         "\tconst uint32_t count = parameters[4];\n\tif (drawable(replay, count, parameters[1], parameters[4]))"),
        # A copy into a buffer doesn't wait for the draws before it.
        ("src/transfer.c", "\twait_for_draws(queue);\n\tmemcpy(", "\t(void)queue;\n\tmemcpy("),
        # Linear blits weigh nothing towards the second texel.
        ("src/transfer.c", "return (Taps){ first, second, centre - 0.5f - (float)below };",
         "return (Taps){ first, second, 0.0f };"),
        # Nearest blits round the centre.
        ("src/transfer.c", "(uint32_t)centre < from ? (uint32_t)centre : from - 1",
         "(uint32_t)(centre + 0.5f) < from ? (uint32_t)(centre + 0.5f) : from - 1"),
        # Clears start at level 0 whatever was asked.
        ("src/transfer.c", "for (uint32_t level = command->clear.first_level; level < end; level++)",
         "for (uint32_t level = 0; level < end; level++)"),
        # Image copies turn the rows upside down.
        ("src/transfer.c",
         "xsdk_texture_upload_rows(&destination->texture, command->image_copy.destination_level, y, 1, rows[ROW_OUT], 0);",
         "xsdk_texture_upload_rows(\n\t\t\t&destination->texture, command->image_copy.destination_level, height - 1 - y, 1, rows[ROW_OUT], 0);"),
        # A linear image's row pitch is its rows' bytes, without the 256-byte alignment.
        ("src/image.c", "(VkDeviceSize)object->texture.pitch * v4x_texel_bytes",
         "(VkDeviceSize)object->width * v4x_texel_bytes"),
        # Fills write the word byte-swapped.
        ("src/transfer.c", "\t\twords[i] = command->buffer_copy.data;", "\t\twords[i] = __builtin_bswap32(command->buffer_copy.data);"),
        # Indirect commands of two draws are recorded.
        ("src/command_buffer.c", "\tif (drawCount > 1) {", "\tif (drawCount > 2) {"),
        # The limit says indirect commands can't draw.
        ("src/physical_device.c", ".maxDrawIndirectCount = 1,", ".maxDrawIndirectCount = 0,"),
        # vkCmdFillBuffer isn't found by name.
        ("src/proc_addr.c", "\tENTRY(vkCmdFillBuffer, SCOPE_DEVICE),\n", ""),
    ],
    "b": [
        ("src/memory.c", "TRANSFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,",
         "TRANSFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,"),
    ],
}


# The read-back of the target before its resolve, and the copy into a buffer before the draws, fail several of the draw
# checks at once, so each also goes alone: a1 is the rest, a2 and a3 those two.
SETS["a1"] = [b for i, b in enumerate(SETS["a"]) if i not in (0, 4)]
SETS["a2"] = [SETS["a"][0]]
SETS["a3"] = [SETS["a"][4]]


def apply(name):
    BACKUP.mkdir(exist_ok=True)
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = (ROOT / path).read_text()
            (BACKUP / path.replace("/", "_")).write_text(texts[path])
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (ROOT / path).write_text(text)
    (BACKUP / "applied").write_text("\n".join(texts))
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


def restore():
    for path in (BACKUP / "applied").read_text().split("\n"):
        (ROOT / path).write_text((BACKUP / path.replace("/", "_")).read_text())
    (BACKUP / "applied").unlink()
    print("restored")


if sys.argv[1] == "apply":
    apply(sys.argv[2])
else:
    restore()
