"""Breaks vulkan4xenon_test's checks of milestone 25 in a copy of the layer: breaks.py <copy> <set>.

Set a, each break failing its own checks:
- blending off past the first colour attachment: colour attachment 1's check, and the screen's;
- a 64-bit target's tiles counted as a 32-bit one's: attachments 2 and 3 overlap in eDRAM and both fail, the screen's
  check with them, and the display-sized 64-bit target and depth fit, so the refusals' check fails too;
- an image drawn back without its target set alone: it lands in the second pass's first target, and the half-float
  image's check fails, with the screen's;
- independentBlend unreported: the limits and features check.
Set b: every channel written past the second attachment, so attachments 2 and 3 fail with the screen's check; and
blending into a float attachment taken, so the refusals' check fails. Set c: vkCmdClearAttachments clearing attachment
0 whichever it's asked for, so attachments 0 to 2 fail, with the screen's check.
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/pipeline_state.c", "xsdk_gpu_blend_state(&pipeline->blends[i], &setting);",
         "xsdk_gpu_blend_state(&pipeline->blends[i], i ? &(XsdkBlend){ XSDK_BLEND_ONE, XSDK_BLEND_ZERO, "
         "XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, setting.write_mask } : &setting);"),
        ("src/render_pass.c", "tile += xsdk_gpu_color_target_tiles(images->width, images->height, pass->targets[i]);",
         "tile += xsdk_gpu_color_target_tiles(images->width, images->height, XSDK_TARGET_8_8_8_8);"),
        ("src/queue.c", "\t\tif (render_pass->color_count > 1)\n\t\t\tset_one_target(replay, i);\n",
         "\t\tif (render_pass->color_count > 1 && false)\n\t\t\tset_one_target(replay, i);\n"),
        ("src/physical_device.c", ".independentBlend = VK_TRUE, .samplerAnisotropy",
         ".independentBlend = VK_FALSE, .samplerAnisotropy"),
    ],
    "b": [
        ("src/pipeline_state.c", "xsdk_gpu_blend_state(&pipeline->blends[i], &setting);",
         "setting.write_mask = i >= 2 ? XSDK_WRITE_ALL : setting.write_mask;\n"
         "\t\txsdk_gpu_blend_state(&pipeline->blends[i], &setting);"),
        ("src/pipeline_state.c", "if (attachment->blendEnable && !v4x_blend_format(format))",
         "if (attachment->blendEnable && !v4x_blend_format(format) && false)"),
    ],
    "c": [
        ("src/command_buffer.c", "const uint32_t color = attachment->colorAttachment;", "const uint32_t color = 0;"),
    ],
}


def main():
    root = pathlib.Path(sys.argv[1])
    texts = {}
    for name, old, new in SETS[sys.argv[2]]:
        path = root / name
        text = texts.get(path, path.read_text())
        assert text.count(old) == 1, (name, old)
        texts[path] = text.replace(old, new)
    for path, text in texts.items():
        path.write_text(text)
    print(f"set {sys.argv[2]}: {len(SETS[sys.argv[2]])} breaks in {len(texts)} files")


if __name__ == "__main__":
    main()
