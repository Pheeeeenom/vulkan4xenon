"""Breaks milestone 20's checks in a copy of the layer: breaks.py SET DIR, DIR holding the copy builds.sh made.

Step 1:
- 1a: BC blocks copied in and out unswapped, which the four BC formats' sampled checks fail and their read-back
  doesn't; R4G4B4A4, B5G6R5 and A2R10G10B10 sampled in xsdk's own channel order, each its format's sampled check; half
  floats packed as 0, the blit and clear checks; a compressed image copied to an uncompressed one, the refusals check;
  textureCompressionBC reported, the features check. The filtered check fails too, through the formats sampled wrong.
- 1b: BC blocks copied out unswapped alone, the read-back check; BC1 without alpha reading DXT1's alpha, its sampled
  check; R16G16_SFLOAT's red and green swapped, its sampled check; an 8-byte texel's words taken the wrong way round,
  the clear check through R16G16B16A16_SFLOAT, whose blit unpacks and packs alike; linear BC images offered, the
  features check.
"""
import pathlib
import sys

FORMAT = "src/format.c"
TRANSFER = "src/transfer.c"

SETS = {
    "1a": [
        (TRANSFER, "\t\tto[i] = (word & 0x00FF00FF) << 8 | (word >> 8 & 0x00FF00FF);\n", "\t\tto[i] = word;\n"),
        (FORMAT, "XSDK_FORMAT_A4R4G4B4, { ALPHA, RED, GREEN, BLUE }", "XSDK_FORMAT_A4R4G4B4, { RED, GREEN, BLUE, ALPHA }"),
        (FORMAT, "XSDK_FORMAT_R5G6B5, { BLUE, GREEN, RED, ALPHA }", "XSDK_FORMAT_R5G6B5, { RED, GREEN, BLUE, ALPHA }"),
        (FORMAT, "XSDK_FORMAT_A2B10G10R10, { BLUE, GREEN, RED, ALPHA }",
         "XSDK_FORMAT_A2B10G10R10, { RED, GREEN, BLUE, ALPHA }"),
        (FORMAT, "return bits == 16 ? float_to_half(value) : bits_of(value);",
         "return bits == 16 ? 0 * float_to_half(value) : bits_of(value);"),
        ("src/command_buffer.c", "return (from && from->compressed) != (to && to->compressed);",
         "return (from && from->compressed) != (to && to->compressed) && false;"),
        ("src/physical_device.c", "(VkPhysicalDeviceFeatures){ .samplerAnisotropy = VK_TRUE }",
         "(VkPhysicalDeviceFeatures){ .samplerAnisotropy = VK_TRUE, .textureCompressionBC = VK_TRUE }"),
    ],
    "1b": [
        (TRANSFER,
         "\t\tswap_words((const uint32_t *)rows[ROW_OUT], command->copy.buffer + y * command->copy.row_bytes / 4, "
         "row_bytes);\n",
         "\t\tmemcpy((void *)(uintptr_t)(command->copy.buffer + y * command->copy.row_bytes / 4), rows[ROW_OUT], "
         "row_bytes);\n"),
        (FORMAT, "XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ONE }", "XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ALPHA }"),
        (FORMAT, "XSDK_FORMAT_R16G16F, { RED, GREEN, BLUE, ALPHA }", "XSDK_FORMAT_R16G16F, { GREEN, RED, BLUE, ALPHA }"),
        (FORMAT, "return format->texel_bytes > 4 && shift < 32;", "return format->texel_bytes > 4 && shift >= 32;"),
        ("src/physical_device.c", "flags || (linear && image->compressed) ||", "flags ||"),
    ],
}


def apply(name, root):
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = (root / path).read_text()
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (root / path).write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


if len(sys.argv) != 3 or sys.argv[1] not in SETS:
    sys.exit(f"breaks.py {'|'.join(SETS)} DIR")
apply(sys.argv[1], pathlib.Path(sys.argv[2]))
