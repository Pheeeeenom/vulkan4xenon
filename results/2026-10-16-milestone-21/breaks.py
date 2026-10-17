"""Breaks milestone 21's checks on purpose, in copies of vulkan4xenon and xsdk, never the trees themselves.

breaks.py list XSDK                            the breaks' names, xost's from xsdk's breaks.py, then the layer's
breaks.py kind NAME                            xost, for a break in xsdk's compiler, or layer
breaks.py apply NAME XSDK LAYER_COPY XSDK_COPY applies one to the copies
breaks.py fails NAME                           vulkan4xenon_test's checks it should fail

XSDK is the xsdk tree whose results/2026-10-16-vulkan4xenon-milestone-21/breaks.py holds xost's breaks. builds.sh builds
vulkan4xenon_test once for each: for one of xost's, xsdk's library and PC compiler with the break in, and the bundle
made again by that compiler, so the shaders compiled on the PC and those compiled on the console both have it. Each
break fails the "every frame" and "image on screen" checks of the scenes listed, and no other check; the scenes' "made"
and "drawn and presented" checks pass. xost's breaks change no shader older than milestone 21: on the PC, each changes
the containers of only the shaders its scenes draw (xsdk's breaks.py, FAILS, says the same of xsdk_test's).
"""
import importlib.util
import pathlib
import sys

# Each layer break: its edits, (path in vulkan4xenon, text, the text in its place), each text found exactly once.
LAYER_BREAKS = {
    # A loop count in a dynamic uniform buffer is read at the buffer's start, not at the dynamic offset.
    "dynamic_offset_ignored": [
        ("src/queue.c", "\t\t\toffset += replay->dynamic_offsets[resource->set][binding->dynamic_index];",
         "\t\t\toffset += 0 * replay->dynamic_offsets[resource->set][binding->dynamic_index];"),
    ],
    # A count isn't clamped, so the loop constant's 8 bits take 300 as 44 passes and -5 as 251.
    "unclamped": [
        ("src/queue.c", "return count < 0 ? 0 : count > V4X_MAX_LOOP_PASSES ? V4X_MAX_LOOP_PASSES : (uint32_t)count;",
         "return (uint32_t)count;"),
    ],
    # The loop constants are written once and never again, so every loop runs the first draw's 0 passes.
    "written_once": [
        ("src/queue.c", "if (queue->loops_written && !memcmp(values, queue->loop_constants, sizeof queue->loop_constants))",
         "if (queue->loops_written)"),
    ],
    # A push constant's count is read a word past where it is, from the zeros after it.
    "push_offset": [
        ("src/queue.c", "memcpy(&count, (const uint8_t *)replay->push + resource->first, sizeof count);",
         "memcpy(&count, (const uint8_t *)replay->push + resource->first + 4, sizeof count);"),
    ],
}

# The scenes of tests/test_branches.c and tests/test_loops.c: (test, scene, what every frame holds).
SCENES = {
    "data_compiled": ("branches", "data in a vertex shader, the shader compiled here",
                      "places and colours each quad by the arms its vertices take"),
    "uniform_compiled": ("branches", "a push constant in a vertex shader, the shader compiled here",
                         "places and colours each quad by the arm its push constant takes"),
    "data": ("branches", "data in a vertex shader", "places and colours each quad by the arms its vertices take"),
    "uniform": ("branches", "a push constant in a vertex shader",
                "places and colours each quad by the arm its push constant takes"),
    "loop_data": ("branches", "data in a loop's body", "colours each band by the passes its u passes"),
    "loop_uniform": ("branches", "a push constant in a loop's body", "colours each half by its push constant"),
    "depth": ("branches", "gl_FragDepth stored in an if/else", "shows the green where the arms wrote 0.75 alone"),
    "push_compiled": ("loops", "counted by push constants, the shader compiled here",
                      "colours each quad by its count's passes, 300 run as 255 and -5 as none"),
    "bands_compiled": ("loops", "counted by a uniform buffer, the shader compiled here",
                       "colours each band by the passes before the break"),
    "push": ("loops", "counted by push constants",
             "colours each quad by its count's passes, 300 run as 255 and -5 as none"),
    "vertex": ("loops", "counted by a dynamic uniform buffer in a vertex shader",
               "places and colours each row's quad by its count's passes"),
    "both": ("loops", "counted in both stages",
             "places each row's quad by the vertex shader's count and colours it by the pixel shader's"),
    "bands": ("loops", "counted by a uniform buffer, with an if/else and a break",
              "colours each band by the passes before the break"),
}

# The scenes each break fails.
FAILS = {
    # Every if/else on data: the vertex shader's, the unrolled loop's, gl_FragDepth's, and the hardware loop's pass.
    "select_swap": ["data_compiled", "data", "uniform_compiled", "uniform", "loop_data", "depth", "bands_compiled",
                    "bands"],
    # Vertex shaders keep selects (xsdk:docs/HARDWARE.md, "Branches, vulkan4xenon milestone 21").
    "predicate_inverted": ["loop_uniform"],
    # float(i) and the block's colour at i read 0 every pass.
    "counter_stuck": ["bands_compiled", "bands"],
    "carried_kept": ["push_compiled", "push", "vertex", "both", "bands_compiled", "bands"],
    # The vertex and pixel shaders' loops share loop constant 0, so one of them runs the other's count.
    "pixel_loops_at_0": ["both"],
    # The && works out as an ||, so the top right and bottom left quads turn green too.
    "phi_swapped": ["data_compiled", "data"],
    "dynamic_offset_ignored": ["vertex", "both"],
    "unclamped": ["push_compiled", "push"],
    "written_once": ["push_compiled", "bands_compiled", "push", "vertex", "both", "bands"],
    # Both the quads' counts and the both-stages scene's pixel shader count come from push constants.
    "push_offset": ["push_compiled", "push", "both"],
}


def xost(xsdk):
    path = pathlib.Path(xsdk) / "results/2026-10-16-vulkan4xenon-milestone-21/breaks.py"
    spec = importlib.util.spec_from_file_location("xsdk_breaks", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def apply_layer(name, root):
    texts = {}
    for path, old, new in LAYER_BREAKS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
    for file, text in texts.items():
        file.write_text(text)
    print(f"{name}: {len(LAYER_BREAKS[name])} edits in {len(texts)} files of {root}")


def main():
    command, args = sys.argv[1], sys.argv[2:]
    if command == "list":
        print("\n".join(list(xost(args[0]).BREAKS) + list(LAYER_BREAKS)))
    elif command == "kind":
        print("layer" if args[0] in LAYER_BREAKS else "xost")
    elif command == "apply":
        name, xsdk, layer_copy, xsdk_copy = args
        if name in LAYER_BREAKS:
            apply_layer(name, pathlib.Path(layer_copy))
        else:
            xost(xsdk).apply(name, pathlib.Path(xsdk_copy))
    elif command == "fails":
        for scene in FAILS[args[0]]:
            test, what, holds = SCENES[scene]
            print(f"{test}: {what}: every frame {holds}")
            print(f"{test}: {what}: the image on screen does too")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
