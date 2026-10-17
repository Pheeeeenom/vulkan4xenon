"""Breaks milestone 23's checks in a copy of the layer: breaks.py <copy> a|b|c. builds.sh makes the copies.

Set a breaks eight checks' worth, apart: copies out of several slices read the first for each, as one a slice late would
trip xsdk's assert past the last (the read-back of a face and two slices, not that of a face alone); the white border
taken as black (the address modes' frames and screen); comparisons' weights left 0 (the shadow frames and screen); D24S8
not reported filtered (shadow's report); comparison samplers that filter taken (shadow's refusal); opaque black taken
(addressing's refusals); 2D arrays of two layers taken (cube and 3D's refusals); and cubes reported with one layer (cube
and 3D's report).
Set b: copies in read a region's layers and slices in reverse (cube and 3D's frames, screen and both read-backs); mirror
clamp to edge taken as mirrored repeat (the address modes' frames and screen); and transparent black taken as white (the
black border's alpha check alone).
Set c: every linear filter taken as nearest: the D24S8 filter check, milestone 20's two filtered format checks, and
likely the earlier checks that sample linearly (anisotropy's, aurora points' and the uploads' linear sampler).
Copies of one layer or slice, as milestone 20's formats check makes, take the same path broken or not, so sets a and b
leave it alone.
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/transfer.c",
         "\t\t\txsdk_texture_download_slice_rows(\n\t\t\t\t&image->texture, copy->level, copy->first_slice + i, 0,",
         "\t\t\txsdk_texture_download_slice_rows(\n\t\t\t\t&image->texture, copy->level, copy->first_slice, 0,"),
        ("src/sampler.c", "*border = colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? XSDK_BORDER_WHITE : XSDK_BORDER_BLACK;",
         "*border = XSDK_BORDER_BLACK;"),
        ("src/sampler.c", "(float)((uint32_t)info->compareOp >> i & 1);", "(float)((uint32_t)info->compareOp >> (i + 3) & 1);"),
        ("src/physical_device.c", "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;",
         "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;"),
        ("src/sampler.c",
         "if (info->compareEnable && (info->magFilter != VK_FILTER_NEAREST || info->minFilter != VK_FILTER_NEAREST))",
         "if (info->compareEnable && !info->compareEnable)"),
        ("src/sampler.c",
         "return colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || colour == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;",
         "return colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || colour == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK ||\n"
         "\t\tcolour == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;"),
        ("src/image.c",
         "if (info->imageType != VK_IMAGE_TYPE_2D || info->extent.depth != 1 || info->arrayLayers != 1 || info->flags)",
         "if (info->imageType != VK_IMAGE_TYPE_2D || info->extent.depth != 1 || info->arrayLayers > 2 || info->flags)"),
        ("src/physical_device.c", ".maxArrayLayers = cube ? XSDK_CUBE_FACES : 1,", ".maxArrayLayers = 1,"),
    ],
    "b": [
        ("src/transfer.c",
         "\t\t\txsdk_texture_upload_slice_rows(&image->texture, copy->level, copy->first_slice + i, 0,\n"
         "\t\t\t\tlevel_rows(image, copy->level), slice_rows(copy, i),",
         "\t\t\txsdk_texture_upload_slice_rows(&image->texture, copy->level, copy->first_slice + i, 0,\n"
         "\t\t\t\tlevel_rows(image, copy->level), slice_rows(copy, copy->slice_count - 1 - i),"),
        ("src/sampler.c", "\t\t*xsdk_mode = XSDK_ADDRESS_MIRROR_ONCE;", "\t\t*xsdk_mode = XSDK_ADDRESS_MIRROR;"),
        ("src/sampler.c", "*border = colour == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? XSDK_BORDER_WHITE : XSDK_BORDER_BLACK;",
         "*border = XSDK_BORDER_WHITE;"),
    ],
    "c": [
        ("src/sampler.c", "return filter == VK_FILTER_LINEAR ? XSDK_FILTER_LINEAR : XSDK_FILTER_POINT;",
         "return filter == VK_FILTER_LINEAR ? XSDK_FILTER_POINT : XSDK_FILTER_POINT;"),
    ],
}


def apply(copy, name):
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = (copy / path).read_text()
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (copy / path).write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


apply(pathlib.Path(sys.argv[1]), sys.argv[2])
