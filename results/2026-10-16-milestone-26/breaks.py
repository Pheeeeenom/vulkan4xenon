"""Milestone 26's breaks of vulkan4xenon_test, each a small source change that should fail the checks named with it.

breaks.py list prints the sets; breaks.py checks <set> the checks it should fail; breaks.py apply <set> <copy> applies
it to a copy holding vulkan4xenon, and Xbox-360-SDK beside it for a set that breaks xsdk; breaks.py touches <set>
<repository> says whether it breaks that one. builds.sh builds each set's copy into a .xex of its own. Every other
check should pass with each set.
"""
import pathlib
import sys

PIPELINE_STATE = "vulkan4xenon/src/pipeline_state.c"
QUEUE = "vulkan4xenon/src/queue.c"
PHYSICAL_DEVICE = "vulkan4xenon/src/physical_device.c"

TWO_SIDED_STATIC = ("two-sided stencil: every frame's front and back faces pass the pipeline's own test for each, over "
                    "what each wrote with its own op and write mask")
TWO_SIDED_REFERENCE = ("two-sided stencil: every frame's faces take the references and write masks commands set for "
                       "each alone")
TWO_SIDED_COMPARE = "two-sided stencil: every frame's faces take the compare masks commands set for each alone"
CLIP_LIMITS = ("clip distances: shaderClipDistance reported, maxClipDistances and maxCombinedClipAndCullDistances 6, "
               "no cull distances")
CLIP_PUSH = "clip distances: every frame's quads are cut at the lines of the planes pushed before each draw"
CLIP_UNIFORM = ("clip distances: every frame's quads are cut by distances 1 and 4, planes from a dynamic uniform "
                "buffer at each draw's offset")
CLIP_AFTER = ("clip distances: every frame cuts nothing of a pipeline without them nor of vkCmdClearAttachments's "
              "rectangle, and cuts the draw after the clear again")

SETS = {
    # Step 1: a back face's dynamic compare mask and write mask taken from the front's.
    "s1a": {
        "checks": [TWO_SIDED_REFERENCE, TWO_SIDED_COMPARE],
        "breaks": [
            (PIPELINE_STATE, "setting.back.read_mask = dynamic->stencil_compare_mask[1];",
             "setting.back.read_mask = dynamic->stencil_compare_mask[0];"),
            (PIPELINE_STATE, "setting.back.write_mask = dynamic->stencil_write_mask[1];",
             "setting.back.write_mask = dynamic->stencil_write_mask[0];"),
        ],
    },
    # Step 1: a pipeline's own two-sided stencil dropped, so its front face's serves both; the compare mask cell marks
    # with such a pipeline.
    "s1b": {
        "checks": [TWO_SIDED_STATIC, TWO_SIDED_COMPARE],
        "breaks": [
            (PIPELINE_STATE, "\t\tsetting.two_sided = faces_differ(&setting);\n", "\t\tsetting.two_sided = false;\n"),
        ],
    },
    # Step 1: a command for one face sets both.
    "s1c": {
        "checks": [TWO_SIDED_REFERENCE, TWO_SIDED_COMPARE],
        "breaks": [
            (QUEUE, "if (command->stencil.faces & 1u << face)", "if (command->stencil.faces)"),
        ],
    },
    # Step 2: a pushed plane read from the push constants' start, the quad's scale; the draw after the clear pushes one.
    "s2a": {
        "checks": [CLIP_PUSH, CLIP_AFTER],
        "breaks": [
            (QUEUE, "memcpy(plane, (const uint8_t *)replay->push + offset_bytes,",
             "memcpy(plane, (const uint8_t *)replay->push,"),
        ],
    },
    # Step 2: a uniform buffer's plane read without its dynamic offset; vkCmdClearAttachments leaving the planes on; and
    # shaderClipDistance unreported.
    "s2b": {
        "checks": [CLIP_UNIFORM, CLIP_AFTER, CLIP_LIMITS],
        "breaks": [
            (QUEUE, "\t\twords += replay->dynamic_offsets[resource->set][set->layout->bindings[resource->binding]"
             ".dynamic_index] / 4;\n", ""),
            (QUEUE, "\tif (replay->clip_planes_on)\n\t\txsdk_gpu_set_clip_planes(0, NULL);\n", ""),
            (PHYSICAL_DEVICE, ".shaderClipDistance = VK_TRUE", ".shaderClipDistance = VK_FALSE"),
        ],
    },
    # Step 2: a pipeline without clip distances leaving the last one's planes on.
    "s2c": {
        "checks": [CLIP_AFTER],
        "breaks": [
            (QUEUE, "if (!pipeline->clip_plane_count && !replay->clip_planes_on)", "if (!pipeline->clip_plane_count)"),
        ],
    },
}


def apply(name, copy):
    texts = {}
    for path, old, new in SETS[name]["breaks"]:
        file = copy / path
        text = texts.get(file, None)
        if text is None:
            text = file.read_text()
        if text.count(old) != 1:
            sys.exit(f"{name}: {path} holds {text.count(old)} of {old!r}")
        texts[file] = text.replace(old, new)
    for file, text in texts.items():
        file.write_text(text, newline="\n")
    print(f"{name}: {len(SETS[name]['breaks'])} breaks in {len(texts)} files")


def main():
    command = sys.argv[1]
    if command == "list":
        print(" ".join(SETS))
    elif command == "checks":
        print("\n".join(SETS[sys.argv[2]]["checks"]))
    elif command == "touches":
        sys.exit(0 if any(path.startswith(sys.argv[3] + "/") for path, _, _ in SETS[sys.argv[2]]["breaks"]) else 1)
    else:
        apply(sys.argv[2], pathlib.Path(sys.argv[3]))


if __name__ == "__main__":
    main()
