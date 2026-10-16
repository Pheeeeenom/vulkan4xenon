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

Step 2:
- 2a: R8G8B8A8_UNORM, R16G16_SNORM and R16G16B16A16_UNORM fetched without their picks, B8G8R8A8_UNORM with R8G8B8A8's,
  and A8B8G8R8_SSCALED_PACK32 as unsigned: each its format's colours check, and the first four the triangles check;
  R8G8B8A8_UINT offered as a vertex format, the vertex buffer features check.

Step 3:
- 3a: signed formats read unsigned, each signed format's sampled check, and the filtered check; R16_UNORM on xsdk's
  half floats, its sampled check; sRGB's encoding left out of the CPU's packing, the blit and clear checks.
- 3b: sRGB formats read without gamma, each sRGB format's sampled check and the sRGB filtered check; BC4 on DXN, its
  sampled check; BC5 offered for blits, the features check.
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
        (FORMAT, "A2R10G10B10_UNORM_PACK32, XSDK_FORMAT_A2B10G10R10, { BLUE, GREEN, RED, ALPHA }",
         "A2R10G10B10_UNORM_PACK32, XSDK_FORMAT_A2B10G10R10, { RED, GREEN, BLUE, ALPHA }"),
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
        (FORMAT, "BC1_RGB_UNORM_BLOCK, XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ONE }",
         "BC1_RGB_UNORM_BLOCK, XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ALPHA }"),
        (FORMAT, "XSDK_FORMAT_R16G16F, { RED, GREEN, BLUE, ALPHA }", "XSDK_FORMAT_R16G16F, { GREEN, RED, BLUE, ALPHA }"),
        (FORMAT, "return format->texel_bytes > 4 && shift < 32;", "return format->texel_bytes > 4 && shift >= 32;"),
        ("src/physical_device.c", "flags || (linear && image->compressed) ||", "flags ||"),
    ],
    "2a": [
        (FORMAT, "{ VK_FORMAT_R8G8B8A8_UNORM, XSDK_VERTEX_UBYTE4N, { W, Z, Y, X } },",
         "{ VK_FORMAT_R8G8B8A8_UNORM, XSDK_VERTEX_UBYTE4N, { 0 } },"),
        (FORMAT, "{ VK_FORMAT_R16G16_SNORM, XSDK_VERTEX_SHORT2N, { Y, X } },",
         "{ VK_FORMAT_R16G16_SNORM, XSDK_VERTEX_SHORT2N, { 0 } },"),
        (FORMAT, "{ VK_FORMAT_R16G16B16A16_UNORM, XSDK_VERTEX_USHORT4N, { Y, X, W, Z } },",
         "{ VK_FORMAT_R16G16B16A16_UNORM, XSDK_VERTEX_USHORT4N, { 0 } },"),
        (FORMAT, "{ VK_FORMAT_B8G8R8A8_UNORM, XSDK_VERTEX_UBYTE4N, { Y, Z, W, X } },",
         "{ VK_FORMAT_B8G8R8A8_UNORM, XSDK_VERTEX_UBYTE4N, { W, Z, Y, X } },"),
        (FORMAT, "{ VK_FORMAT_A8B8G8R8_SSCALED_PACK32, XSDK_VERTEX_BYTE4, { 0 } },",
         "{ VK_FORMAT_A8B8G8R8_SSCALED_PACK32, XSDK_VERTEX_UBYTE4, { 0 } },"),
        (FORMAT, "{ VK_FORMAT_R32_SFLOAT, XSDK_VERTEX_FLOAT1, { 0 } },",
         "{ VK_FORMAT_R32_SFLOAT, XSDK_VERTEX_FLOAT1, { 0 } },\n\t{ VK_FORMAT_R8G8B8A8_UINT, XSDK_VERTEX_UBYTE4, { 0 } },"),
    ],
    "3a": [
        ("src/image.c", "\t\t\tsigns[channel] = XSDK_SIGN_SIGNED;\n", "\t\t\tsigns[channel] = XSDK_SIGN_UNSIGNED;\n"),
        (FORMAT, "{ VK_FORMAT_R16_UNORM, XSDK_FORMAT_R16,", "{ VK_FORMAT_R16_UNORM, XSDK_FORMAT_R16F,"),
        (FORMAT, "return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;",
         "return value + 0.0f * powf(value, 1.0f / 2.4f);"),
    ],
    "3b": [
        ("src/image.c", "\t\t\tsigns[channel] = XSDK_SIGN_GAMMA;\n", "\t\t\tsigns[channel] = XSDK_SIGN_UNSIGNED;\n"),
        (FORMAT, "{ VK_FORMAT_BC4_UNORM_BLOCK, XSDK_FORMAT_DXT5A,", "{ VK_FORMAT_BC4_UNORM_BLOCK, XSDK_FORMAT_DXN,"),
        (FORMAT, "{ VK_FORMAT_BC5_UNORM_BLOCK, XSDK_FORMAT_DXN, { RED, GREEN, BLUE, ALPHA }, 16, true, { 0 }, { 0 }, UNORM,\n\t\tFILTERED },",
         "{ VK_FORMAT_BC5_UNORM_BLOCK, XSDK_FORMAT_DXN, { RED, GREEN, BLUE, ALPHA }, 16, true, { 0 }, { 0 }, UNORM,\n\t\tBLITTED },"),
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
