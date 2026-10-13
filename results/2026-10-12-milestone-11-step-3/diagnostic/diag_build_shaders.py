"""diag_build_shaders.py: vulkan4xenon's tools/build_shaders.py for the milestone 11, step 3 diagnostic title, with two
one-off changes to what xost writes: a shader whose file name holds _cfa0 gets every exec's addressing mode a0, and a
pixel shader whose name holds _scxy gets SQ_CONTEXT_MISC.SC_OUTPUT_SCREEN_XY set. Takes build_shaders.py's
arguments; run from vulkan4xenon's root."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path("tools").resolve()))
import build_shaders  # noqa: E402

xost = Path(sys.argv[sys.argv.index("--xost") + 1]).resolve()
sys.path.insert(0, str(xost))
import xasm.program as program  # noqa: E402

mode = {"cfa0": False, "scxy": False}
original_exec = program.enc.build_cf_exec
original_container = program.ce.build_container
original_lower = build_shaders.lower


def build_cf_exec(**fields):
    if mode["cfa0"]:
        fields["addressing_mode"] = 1        # AddressingMode.kAbsolute: a0 (xenia:src/xenia/gpu/ucode.h:197-205)
    return original_exec(**fields)


def build_container(shader_type, ucode, **fields):
    if mode["scxy"] and shader_type == "pixel":
        fields["context_misc"] |= 0x2        # SC_OUTPUT_SCREEN_XY (xenia:src/xenia/gpu/registers.h:156)
    return original_container(shader_type, ucode, **fields)


def lower(xost_path, spirv_bytes, source):
    mode.update(cfa0="_cfa0" in source.name, scxy="_scxy" in source.name)
    try:
        return original_lower(xost_path, spirv_bytes, source)
    finally:
        mode.update(cfa0=False, scxy=False)


program.enc.build_cf_exec = build_cf_exec
program.ce.build_container = build_container
build_shaders.lower = lower
build_shaders.main()
