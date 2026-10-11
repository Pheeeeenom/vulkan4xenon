<div align="center">
  <img src="assets/vulkan4xenon.png" alt="Vulkan 4 xenon" width="640">
</div>

# vulkan4xenon

A Vulkan 1.0 subset for the Xbox 360, built as a static library on the xsdk homebrew SDK's GPU layer, which drives
Xenos directly under the retail kernel. It follows the layout of vulkan4vita, which does the same on the PS Vita's
GXM.

The goal is running Dusklight, a reimplementation of Twilight Princess built on encounter's Aurora, on an RGH console.

**Status:** milestones 1 (a triangle) and 2 (textures, descriptors and push constants) passed on the console on
2026-10-11; milestone 3, pipeline state, is built and waits for its run. docs/ROADMAP.md is the plan, and
docs/SUPPORT.md lists what exists.

## Shaders

Apps pass standard SPIR-V to `vkCreateShaderModule`. Shaders are compiled offline into Xenos containers, bundled with
the app and looked up by their SPIR-V. `tools/build_shaders.py` does it: GLSL → `glslang -V` → xost → a C source with
each shader's SPIR-V and the bundle.

```
python3 tools/build_shaders.py --xost ../Xbox-360-SDK/xenos-toolchain --output tests/shaders tests/shaders/*.vert tests/shaders/*.frag
```

## Building

The layer builds with xsdk's toolchain in WSL Debian, against an xsdk checkout next to this one (`XSDK=` points
elsewhere). Build xsdk first, then:

```
make
```

That makes `build/libvulkan4xenon.a` and the test title, `build/vulkan4xenon_test.xex`.

## Trademark

The Vulkan logo is a registered trademark of the Khronos Group, used here at the project owner's choice. vulkan4xenon
doesn't claim conformance.
