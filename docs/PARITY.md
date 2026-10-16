# Direct3D 9 parity

How far the layer is from what a port needs, and where each gap lies. Ports are expected from any system up to the
GameCube and Wii, and from PC games of the same years. All of them were written for GPUs of Direct3D 9's class or
less, so the yardstick is Shader Model 3 as a typical card of the time had it, not Vulkan 1.0's conformance. What
Vulkan 1.0 requires and Xenos can't do, compute and integer and bitwise maths in shaders, Direct3D 9 doesn't have
either.

docs/SUPPORT.md says exactly what the layer does; this page sums it up against Direct3D 9. Milestones 20 to 28 in
docs/ROADMAP.md close the gaps. Each row says whether Xenos has it, whether xsdk drives it (xost, for shaders), and
whether the layer exposes it. As of 2026-10-16.

Of the 47 rows, the layer has 23, part of 10 and none of 14. Xenos has all but three: occlusion queries hang it today,
its gamma only approximates sRGB, and whether it compares depth in a fetch hasn't been checked. Every other gap is
software.

## Shaders

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Vertex and pixel shaders from a high-level language | Yes | Yes: GLSL through xost, offline or on the console | Yes |
| 256 vertex and 224 pixel float constants | Yes | Yes | Yes, 256 a stage |
| Constant arrays at a run-time index, as skinning palettes are | Yes | Yes (xost:docs/xsc.md:71) | Yes |
| `if` and `else` | Yes: conditional execs and predicates (xenia:src/xenia/gpu/ucode.h:101-130) | Yes: selects on data, execs on the predicate where every vertex or pixel agrees; a `switch`, and a `break` or `return` in an arm, refused (xost:docs/xsc.md, "Control flow") | Yes |
| Loops | Yes (xenia:src/xenia/gpu/ucode.h:132) | Partial: fixed counts unrolled, an `if` on data inside them (xost:docs/xsc.md, "Loops (unrolling + mem2reg)") | Partial: a shader that needs a hardware loop's constants is refused (SUPPORT.md, Shaders) |
| `discard` | Yes | Yes, not yet confirmed on the console (xost:docs/xsc.md:64) | Yes |
| Derivatives | Yes | Yes, confirmed on the console (xost:docs/xsc.md:282) | Yes |
| A computed LOD and gradients (`texldl`, `texldd`) | Yes | Partial: a constant LOD only (xost:docs/xsc.md:82) | Partial |
| The pixel's position and facing (`vPos`, `vFace`) | Yes | Partial: `gl_FragCoord.xy` only (xost:docs/xsc.md:72) | Partial |
| Depth output | Yes | Yes: outside any `if`, or in both arms of one; written on one path alone, refused, as nothing else gives the pixel a depth (xost:docs/xsc.md, "Control flow") | Yes |
| Vertex texture fetch | Yes | Not checked | No: both stages share the fetch constants (SUPPORT.md, Pipelines) |
| 32 temporary registers | Yes | Partial: 16 a pixel shader | Partial |

## Vertex input and draws

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Vertex formats: half floats, bytes and shorts, normalized or not, 10-bit types | Yes (xenia:src/xenia/gpu/xenos.h:602-618) | Yes but the 10-bit types: 18 formats (xsdk:include/xsdk/shader.h) | Partial: half floats, and bytes and shorts normalized or scaled, two or four to an attribute (milestone 20); no 10-bit types |
| 16 vertex streams | Yes | Yes | Yes |
| Points, lines, line strips, triangle lists, strips and fans | Yes | Yes | Yes |
| 32-bit indices, draws past 65,535 | Yes, though a draw packet counts in 16 bits (xenia:src/xenia/gpu/registers.h:327) | Up to 65,535 a draw | Partial: lists are split; strips and fans that long are refused |
| Instancing | Yes: a vertex fetch takes its index from a register (xenia:src/xenia/gpu/ucode.h:695-715) | No | No |
| Dynamic vertex buffers | n/a | Yes | Yes |

## Textures and samplers

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| 2D textures, mips, any size | Yes, up to 8192 | Yes | Yes |
| Cube maps | Yes (xenia:src/xenia/gpu/xenos.h:97-102) | No (xsdk:TODO.md, "Cube and 3D textures"); xost samples them (xost:docs/xsc.md:40) | No |
| Volume textures | Yes | No | No |
| 16-bit colour, L8, A8, A8L8 | Yes | Yes | Yes: 565, 1555 and 4444 (milestone 20), and R8 and RG8, whose swizzles stand in for L8, A8 and A8L8 |
| DXT1 to 5 | Yes (xenia:src/xenia/gpu/xenos.h:494-536) | Yes | Yes: BC1 to BC3, and BC4 and BC5 from DXT5A and DXN, each reported a format at a time, as `textureCompressionBC` promises BC6H and BC7 too (SUPPORT.md, Formats) |
| Float textures | Yes, though the console filtered 16-bit floats wrong (SUPPORT.md, Formats) | Partial: 16-bit floats and R32F | Partial: 16-bit floats in one, two and four channels, and R32F, nearest filtering only |
| 10-10-10-2, 16-bit fixed point, signed formats for bump maps | Yes | Yes (xsdk_texture_signs for the signed) | Yes: and the signed 8-, 10- and 16-bit formats, not BC4 and BC5's (milestone 20); 10-10-10-2 nearest filtering only, as the console filtered it wrong |
| sRGB | Approximate: a piecewise-linear gamma (xenia:src/xenia/gpu/xenos.cc:27) | Yes, Xenos's gamma | Partial: sampled through Xenos's curve, a deviation; not colour attachments (milestone 20) |
| Point, linear and anisotropic filtering, LOD bias, mip clamps | Yes | Yes | Yes, clamped to whole levels |
| Wrap, mirror and clamp | Yes | Yes | Yes |
| Border and mirror-once addressing | Yes, a black or white border only (xenia:src/xenia/gpu/xenos.h:104-162) | No (xsdk:TODO.md, "Border and mirror-once addressing") | No |
| Depth textures and shadow comparison | Depth sampling yes; comparison not checked | Depth resolves into textures | Partial: D24S8 sampled raw, nearest; no comparison |

## Render targets and output

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Render to texture | Yes | Yes | Yes |
| Float, 10-bit and 64-bit targets | Yes (xenia:src/xenia/gpu/xenos.h:299-313) | No (xsdk:TODO.md, "Float render targets and multisampling") | No |
| Four render targets at once | Yes, each blended on its own (xenia:src/xenia/gpu/register_table.inc:481, 483-485, 537-547) | No; unproven in xost (xsdk:TODO.md, "Multiple render targets") | No |
| 2x and 4x multisampling, alpha to coverage | Yes (xenia:src/xenia/gpu/registers.h:687) | Multisampling only | No (milestone 17) |
| Targets past eDRAM: 720p multisampled, a big shadow map | Yes, by predicated tiling | No (xsdk:TODO.md, "Predicated tiling") | No: 1280x720 at most |
| D24S8, depth bias, one-sided stencil | Yes | Yes | Yes |
| Two-sided stencil | Yes (xenia:src/xenia/gpu/registers.h:808) | No (xsdk:TODO.md, "Two-sided stencil") | No |
| Alpha test | Yes (xenia:src/xenia/gpu/registers.h:685-686) | No | Yes, through `discard`, as Vulkan has it |
| Blending: separate alpha, a constant colour, min and max, write masks | Yes | Yes | Yes |
| User clip planes | Yes (xenia:src/xenia/gpu/registers.h:539-544) | No | No |
| Point sprites with a size | Yes (xenia:src/xenia/gpu/registers.h:184-212) | No | No: points are 1 pixel |
| Wireframe and point fill | Yes (xenia:src/xenia/gpu/registers.h:463-469) | No (xsdk:TODO.md, "Wireframe and point fill") | No |
| Scissor and viewport | Yes | Yes | Yes, one of each |

## Queries and copies

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Event queries and time stamps | Yes | Yes | Yes |
| Occlusion queries | Hangs today (xsdk:docs/HARDWARE.md, "Queries and events") | No | No |
| `StretchRect` and blits | Yes, through resolves | Resolves | Partial: on the CPU, after the GPU is done |
| Reading targets back | n/a | Yes | Yes, after the GPU is done |

## Where the gaps lie

- **The layer alone,** as xsdk has it: multisampling. DXT, which most PC ports ship, the 16-bit and half-float texture
  formats, and the vertex formats came with milestone 20.
- **xost:** loops a block counts, a computed LOD and gradients, facing, vertex texture fetch and the register budget.
  Almost every ported shader meets one.
- **xsdk, then the layer:** four targets and their formats, predicated tiling, cube and volume textures, border
  addressing, two-sided stencil, clip planes, point sprites, fill modes and instancing.

## Past Direct3D 9, for older consoles' ports

Open, each for a decision when a port needs it:
- **Paletted textures** (N64, PS1, PS2, GameCube): Direct3D 9's P8 was rarely supported, so ports expand palettes on
  the CPU or look them up in a shader, which xost's dependent reads allow (xost:docs/xsc.md:84).
- **Quads and rectangles:** Xenos draws both (xenia:src/xenia/gpu/xenos.h:53-56); Vulkan has neither, so a port makes
  triangles.
- **Copies of part of a target,** as GameCube framebuffer copies and N64 framebuffer effects make: xsdk resolves whole
  targets and the layer takes whole framebuffers as render areas (xsdk:TODO.md, "Resolving part of a target" and
  "Render areas smaller than the framebuffer").
- **PS2's `(A-B)*C+D` blending:** most of it maps to blend factors; the rest needs dual-source blending, which Xenos
  lacks (xsdk:TODO.md, "Destination alpha"), or work in the shader.
- **Logic ops** (GameCube): Xenia's register definitions show none.

## Not counted

Direct3D 9 doesn't have them: compute, integer and bitwise maths in shaders, atomics, geometry shaders. Bitwise
operations still turn up in GLSL written since; xost refuses them, and shifts by a constant could be multiplies but
aren't yet (xost:docs/xsc.md:273-275).
