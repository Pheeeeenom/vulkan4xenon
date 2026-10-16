"""Breaks milestone 22's checks in a copy of xsdk and the layer: breaks.py <set> <root>, the copies being
<root>/Xbox-360-SDK and <root>/vulkan4xenon. builds.sh makes the copies and builds each set.

Set a, each break a check of its own: textureGrad's register gradients unread, the shader_lod gradients check; a bias
through the register LOD unread, the bias check; the bias field written 0, the constant LODs check; a discard on a
bool taken where it doesn't hold, the facing discard check; and the fetch constants' limit one past 32, the
vertex_textures refusal check.
Set b: a pixel shader's explicit LOD added to the computed one, the shader_lod LOD-per-pixel and constant LODs checks;
gl_FrontFacing inverted, all three facing checks; and the vertex shader's samplers bound at xost's slots though its
fetches moved, the vertex_textures heights and levels checks.
Set c: a vertex shader's register LOD unread, the vertex_textures levels check alone.
"""
import pathlib
import sys

SAMPLE = "Xbox-360-SDK/src/xsc_lower_sample.c"
FRAGMENT = "Xbox-360-SDK/src/xsc_lower_fragment.c"
PIPELINE = "vulkan4xenon/src/pipeline.c"

SETS = {
    "a": [
        (SAMPLE, "\tt->use_register_gradients = true;\n", "\tt->use_register_gradients = false;\n"),
        (SAMPLE, "\t\tt->use_register_lod = true;\n", "\t\tt->use_register_lod = !bias;\n"),
        (SAMPLE, "\t\tt->lod_bias = constant;\n", "\t\tt->lod_bias = 0.0;\n"),
        (FRAGMENT, "f->kill_on_true ? XSC_VECTOR_KILL_GT : XSC_VECTOR_KILL_EQ;",
         "f->kill_on_true ? XSC_VECTOR_KILL_EQ : XSC_VECTOR_KILL_GT;"),
        (PIPELINE, "fragment_slots + vertex_slots + stream_slots > V4X_TEXTURE_SLOTS)",
         "fragment_slots + vertex_slots + stream_slots > V4X_TEXTURE_SLOTS + 1)"),
    ],
    "b": [
        (SAMPLE, "\tt->use_computed_lod = !lod;\n", "\tt->use_computed_lod = !lod || !l->vertex;\n"),
        (FRAGMENT, "XSC_VECTOR_SGE, x, xsc_src(xsc_reg(sign), true, xsdk_xsc_bsw(0)), face,",
         "XSC_VECTOR_SGE, xsc_src(xsc_reg(sign), true, xsdk_xsc_bsw(0)), x, face,"),
        (PIPELINE, "r->resource.first = (uint16_t)(r->resource.first + fragment_slots);",
         "r->resource.first = (uint16_t)r->resource.first;"),
    ],
    "c": [
        (SAMPLE, "\t\tt->use_register_lod = true;\n", "\t\tt->use_register_lod = !l->vertex;\n"),
    ],
}


def apply(name, root):
    texts = {}
    for path, old, new in SETS[name]:
        file = root / path
        texts.setdefault(file, file.read_text())
        assert texts[file].count(old) == 1, (path, old)
        texts[file] = texts[file].replace(old, new)
    for file, text in texts.items():
        file.write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


apply(sys.argv[1], pathlib.Path(sys.argv[2]))
