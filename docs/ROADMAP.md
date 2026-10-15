# vulkan4xenon roadmap (agreed 2026-10-11)

**Goal.** A Vulkan 1.0 subset for the Xbox 360: a static library on the xsdk GPU layer, for ports that run on the
retail kernel. The goal it's measured against is **Dusklight**, a reimplementation of Twilight Princess, running on
the console through it.

**Status.** Agreed on 2026-10-11. Milestones 1 to 7, 9 and 10 are done, and milestone 8 was tried and reverted.
Section 5 has the Dusklight track's order, agreed the same day; section 7 lists what's still open.

**Constraints.** xsdk's, unchanged:
- No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Every claim about Xenos cites Xenia, libxenon, xost or a hardware result.
- The console decides: a milestone is done only on a clean hardware run. Xenia is for smoke checks.

**Sources.**
- **xsdk** (Z:\Xbox-360-SDK), cited as `xsdk:<path>`. What the console has settled is in `xsdk:docs/HARDWARE.md`.
- **Dusklight** (TwilitRealm/dusklight at `2c4960d`), cited as `dusklight:<path>:<line>`. The port's fork is
  Z:\dusklight-xenon, from that commit.
- **encounter's Aurora** (encounter/aurora at `3227d76`, the commit Dusklight pins), cited as `aurora:<path>:<line>`.
  This is the GameCube and Wii compatibility layer Dusklight is built on, not the Aurora dashboard titles launch from.
- **Aurora's fork** (xenosworks/aurora-xenon, branch `xenon`, on `3227d76`), with the backend interface, the null
  backend and the vulkan4xenon backend, cited as `aurora-xenon:<path>`.
- **xost** (xsdk's `xenos-toolchain/`), cited as `xost:<path>:<line>`.

## 1. How the layer is built

- **A static library.**
  - Apps call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` work too.
  - It reports Vulkan 1.0, refuses 1.1 and up, and claims no conformance.
- **Khronos headers, vendored** in `third_party/vulkan-headers/`. They're Apache-2.0.
- **docs/SUPPORT.md** is the one list of what exists, every limit below the spec's minimum, and every deviation, each
  with a reason. It changes in the same commit as the code (CODE_GUIDELINES.md).
- **Milestones written one way:**
  - **Done when:** criteria a hardware run can check.
  - **Passed on hardware** on a date, at a commit, with the results' folder.
  - **Decisions**, **How it maps onto Xenos**, **New functions**, **New tests** and **Not in milestone N**.
- **Feature tests** in one title, each checking pixels of the presented image. They're broken on purpose before a
  hardware run, as xsdk's are.
- **A benchmark against the raw GPU layer,** xsdk's, scene for scene.
- **The boot logo:** the vulkan4xenon logo shows from `vkCreateDevice` until the app's first present, fading in from
  black. The image and its generator are xsdk's (`xsdk:assets/vulkan4xenon.png`,
  `xsdk:tools/gen_splash.py`). The Vulkan logo is a registered trademark of the Khronos Group, used at the user's
  choice.
- **Rules of thumb:**
  - Where the GPU's own ordering isn't proven, wait on the CPU.
  - Measure fast paths before keeping them.
  - Check the accounting before the ratio: a miscounted present skews every benchmark.
  - A passing test can hide an app that hangs, so the boot logo's present chain is checked through to the screen.

## 2. Decided (2026-10-11)

- **Its own repository,** this one. It links xsdk's `libxsdk.a` and can join the combined xsdk and xost later.
- **Shaders are compiled offline and looked up by their SPIR-V.**
  - Apps hand `vkCreateShaderModule` standard SPIR-V.
  - A build step runs xost over each module and writes a bundle of Xenos containers keyed by the SPIR-V; xost goes
    SPIR-V → Xenos (xost:docs/xsc.md:1-14).
  - `vkCreateShaderModule` hashes the code and finds its container. A module missing from the bundle fails with
    `VK_ERROR_FEATURE_NOT_PRESENT`.
  - **Python only in that offline step.** xost moving off Python is a later item (xsdk:TODO.md, "Toolchain").
- **Uniform buffers are copied into float constants** when a command buffer replays. Loading them from memory with
  `PM4_LOAD_ALU_CONSTANT` (xenia:src/xenia/gpu/command_processor.cc:1477-1500) is a later optimisation. It needs a
  probe first, since partial constant writes hang the GPU (xsdk:docs/HARDWARE.md, milestone 11).
- **The target is Dusklight,** through encounter's Aurora. It's demanding enough to show what's missing.
- **Vulkan 1.0 stays the version.** Nothing on the path needs 1.1: Aurora's backend asks for 1.0
  (aurora-xenon:lib/vulkan4xenon/device.cpp:303), most of what 1.1 promoted is optional, and its one new requirement,
  subgroup operations, is for compute shaders, which Xenos lacks. A single 1.1 piece a target needs comes as its
  extension on 1.0, such as `VK_KHR_maintenance1` for a negative viewport height.
- **Aurora's side is a fork,** aurora-xenon, with a backend interface and a design for a GPU without storage buffers
  (section 5).
- **The shader subset is xost's,** and its limits become the layer's, listed in SUPPORT.md:
  - no bitwise operations;
  - data-dependent branches inside loops raise an error;
  - 16 registers a pixel shader;
  - vertex shaders lower less than fragment shaders.

  See xost:docs/xsc.md:124-131, 176-195.

## 3. How Vulkan maps onto Xenos

| Vulkan | On xsdk's GPU layer |
|---|---|
| Instance, physical device, device, queue | One each. The queue is the ring. |
| Memory | One heap of physical memory, write-combined, from `xsdk_gpu_alloc`. Device-local, host-visible and coherent. Mapping returns the pointer. `vkQueueSubmit` drains the CPU's write-combining. |
| Buffers | Vertex buffers are streams (`xsdk_gpu_set_vertices`, 16 of them). Index buffers go to `xsdk_gpu_draw_indexed`. |
| Uniform buffers, dynamic uniform buffers | Copied into float constants at replay, at their dynamic offsets. |
| Push constants | Float constants, set at replay. |
| Images | `XsdkTexture`: optimal tiling is Xenos's tiled layout; linear tiling is linear with one level. Formats and mips as xsdk milestones 15 and 16 settled. |
| Image views, samplers | The texture's and the sampler state's halves of a fetch constant (`xsdk_gpu_set_texture`). |
| Descriptor sets | Combined image samplers fill fetch-constant slots 0 up; uniform buffers fill constant blocks. Bindings are numbered in order across the pipeline layout. |
| Graphics pipelines | Built once at creation from xsdk's depth and stencil, blend and raster state blocks. The vertex input state gives a patched copy of the vertex shader (xsdk milestone 17). |
| Render passes, framebuffers | One subpass. A render pass instance draws into eDRAM, and each attachment's store op is a resolve into its image. A clear load op is the resolve's clear; a load op draws the image back in. 1280x720 colour and depth fit eDRAM (xsdk:SCOPE.md 3.4). |
| Command buffers | Recorded into the layer's own list and replayed into the ring at `vkQueueSubmit`. xsdk's recorded command buffers (milestone 18) can take over where a buffer's state is all known when it's recorded. |
| Fences | xsdk's fences. |
| Semaphores, barriers | One queue, so semaphores do nothing. Barriers order copies against draws. |
| Swapchain | `VK_KHR_surface` and `VK_KHR_display` over `xsdk_present`: 2 or 3 images at 1280x720. |
| Copies | `vkCmdCopyBufferToImage` and the like run on the CPU at replay. |

## 4. The layer's milestones

1. **A triangle through Vulkan.** Done when a feature-test title draws a triangle on the console with only Vulkan
   calls, presented through the swapchain, with its pixels checked.
   - **Builds:** instance, device and queue; memory and buffers; one graphics pipeline; a render pass with one colour
     attachment; command buffers replayed at submit; a fence; the boot logo.
   - **The shader bundle:** glslang compiles the test's GLSL to Vulkan SPIR-V, and xost turns that into containers.
     xost lowers OpenGL-flavoured SPIR-V today (`glslang -G`, xost:docs/xsc.md:13); taking Vulkan's is xost work in
     this milestone.
2. **Textures, descriptors and push constants.** Done when the feature tests sample images through descriptor sets,
   read push constants and uniform buffers (dynamic offsets included), and copy a buffer into an image, each checked
   by pixel.
3. **Pipeline state.** Done when the feature tests cover, through Vulkan, what xsdk milestones 12 to 14 cover through
   the GPU layer: depth and stencil, blending, culling, dynamic viewport and scissor, indexed draws and topologies.
4. **Render to texture.** Done when an image rendered in one render pass is sampled in the next, with depth in eDRAM,
   checked by pixel. Encounter's Aurora copies its framebuffer this way.
5. **The benchmark.** Done when the same scenes run through the raw GPU layer and through Vulkan, with the gap
   measured on the console and its causes named.
6. **The replay's cost,** agreed on 2026-10-11 after milestone 5 and xsdk milestones 19 and 20 measured it. Done when
   the replay points streams and textures only when they change, and uniform buffers no longer cost the CPU an
   uncached read a word.
7. **Uniform blocks the GPU loads,** agreed on 2026-10-11 after milestone 6. Done when the GPU loads each uniform block
   straight from its buffer, and reading uniform buffers costs a draw next to nothing.
8. **Push constants the GPU loads,** agreed on 2026-10-11 after milestone 7. Done when each draw's push constants go
   through a ring the GPU loads them from, and push draws cost less for it. They cost the same, so it was reverted.
9. **Dusklight on Aurora's null backend,** agreed on 2026-10-11, the first of the Dusklight track (section 5). Done
   when Dusklight runs its title scene on the console for a minute without drawing.

### Milestone 1: a triangle through Vulkan (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console:
- shows the boot logo from `vkCreateDevice` until its first present;
- draws a triangle with only Vulkan calls, presented through the swapchain for 120 frames;
- checks the image the display shows: each corner's colour, and the clear colour outside the triangle;
- passes every check, and returns to the Aurora dashboard.

**Passed on hardware** on 2026-10-11, at `aba67e7` with xsdk at `7420d94`
(`results/2026-10-11-milestone-1/xsdk.log`). All 20 checks passed, and the console went back to the Aurora dashboard.
- **The boot logo:**
  - the display moved from XAM's surface at 0 to the logo's buffer;
  - it stayed there once the swapchain existed;
  - the first present replaced it with the app's dark green, read back exactly (00 60 00).
- **The triangle:** every one of the 120 frames had its corners and clear colour.
- **What was on screen at the end:** the corners, the clear colour, and 259,200 drawn pixels, the triangle's area
  (960 x 540 / 2).
- **Presenting:** all 121 frames were shown, 121 flips over 132 vblanks. The 10 missed vblanks came from the test,
  which waits for each frame and reads it back before presenting it.
- **Seen on screen,** the user reported: the logo faded in, held for a second or two, the dark green flashed, then the
  triangle showed. The triangle's capture is `results/2026-10-11-milestone-1/triangle.png`.

**Decisions:**
- **The library and its handles.**
  - Apps link `libvulkan4xenon.a` and call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` look
    names up in one table.
  - Dispatchable handles point at the layer's structs. Non-dispatchable ones are those pointers as 64-bit integers.
- **Replay.**
  - `vkQueueSubmit` replays on the caller's thread into xsdk's ring.
  - Each command buffer gets a fixed list of commands when it's allocated. Recording past its end makes
    `vkEndCommandBuffer` return `VK_ERROR_OUT_OF_HOST_MEMORY`.
- **Memory.**
  - One heap with one type: device-local, host-visible and coherent. Each allocation is `xsdk_gpu_alloc`'s
    write-combined memory, in 64 KB pages.
  - `memcpy` into it is safe: the toolchain's `memcpy` stores bytes or aligned words. Its disassembly has no 64-bit
    stores.
  - A plain 8-byte store needs an 8-byte-aligned address (xsdk:CLAUDE.md).
- **Shaders.**
  - An app's GLSL becomes Vulkan SPIR-V (`glslang -V`) and then a container through xost, in `tools/build_shaders.py`.
  - The app links the generated bundle: each container keyed by a 64-bit FNV-1a hash of its SPIR-V and the SPIR-V's
    size, with the literal constants xost pools (xost:docs/xsc.md, "Literal constant pool").
  - `vkCreateShaderModule` finds the entry, and a pipeline writes its literals when it's bound.
  - xost's instruction templates come from Xenia's definitions, not from XDK-compiled blobs (see the xost changes
    below).
- **Vertex input.**
  - Location n feeds the usage xost gives it: 0 position, 1 colour, then texture coordinates from 2
    (xost:xsc/resolve.py:158-162).
  - Each binding is one xsdk stream. `firstVertex` moves the stream's address forward by that many strides, since
    xsdk draws from index 0.
- **Clip space.**
  - Vulkan's y points down, and xsdk's viewport follows Direct3D's y-up (xsdk:src/gpu_draw.c:298). So the layer sets
    a viewport of negative height from the bottom edge.
  - Xenos centres pixels on whole coordinates under xsdk's `PIX_CENTER_D3D` (xenia:src/xenia/gpu/xenos.h:861-866,
    xsdk:src/gpu_setup.c:263), and Vulkan centres them on halves, so the viewport also moves up and left by half a
    pixel. Checked at the pixel level from milestone 3, which tests edges.
- **Render passes.**
  - Beginning a pass sets the eDRAM target, and ending it resolves into the attachment.
  - A resolve also clears eDRAM, to the colour it's given (xsdk:src/gpu_resolve.c:112-128). Each pass's resolve
    clears to that pass's clear colour, so a frame that clears to the same colour as the last needs nothing more.
  - When eDRAM holds anything else, such as at the start, beginning the pass resolves into the attachment once to
    clear it. That copies junk the pass then overwrites.
- **The swapchain** is `xsdk_present`'s front buffers: 2 or 3 images at 1280x720 in `VK_FORMAT_B8G8R8A8_UNORM`,
  presented in FIFO mode.
  - `vkAcquireNextImageKHR` waits on the CPU for the next buffer to leave the screen.
  - One image can be acquired at a time; SUPPORT.md lists that deviation.
- **The boot logo** is xsdk's splash.
  - `vkCreateDevice` fades it in over half a second and leaves it up.
  - The swapchain takes over the display without showing anything of its own, so the logo stays until the first
    present flips.
  - Its buffers are freed only after that flip and one more vblank, as freeing the logo's frame too early can hang.

**How it maps onto Xenos:** as section 3, less descriptors, uniform buffers, images other than the swapchain's, and
copies.

**New in xsdk:**
- `xsdk_splash_begin` and `xsdk_splash_end`, with `xsdk_present_open` taking over a splash's display;
- `xsdk_present_buffer_address`;
- `xsdk_display_shown_address`, so a test can read the image actually on screen.

**New in xost:**
- instruction templates built from Xenia's definitions, checked against the corpus as test vectors;
- a regression test lowering Vulkan SPIR-V.

**Functions:**

| Group | Functions |
|---|---|
| Instance | `vkCreateInstance`, `vkDestroyInstance`, `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`, `vkGetInstanceProcAddr`, `vkEnumeratePhysicalDevices` |
| Physical device | `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceFormatProperties`, `vkEnumerateDeviceExtensionProperties`, `vkEnumerateDeviceLayerProperties` |
| Device | `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceProcAddr`, `vkGetDeviceQueue`, `vkDeviceWaitIdle` |
| Memory and buffers | `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`, `vkUnmapMemory`, `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`, `vkCreateBuffer`, `vkDestroyBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory` |
| Pipelines | `vkCreateShaderModule`, `vkDestroyShaderModule`, `vkCreatePipelineLayout`, `vkDestroyPipelineLayout`, `vkCreateGraphicsPipelines`, `vkDestroyPipeline` |
| Render passes | `vkCreateImageView`, `vkDestroyImageView`, `vkCreateRenderPass`, `vkDestroyRenderPass`, `vkCreateFramebuffer`, `vkDestroyFramebuffer` |
| Commands | `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkResetCommandPool`, `vkAllocateCommandBuffers`, `vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkResetCommandBuffer`, `vkCmdBeginRenderPass`, `vkCmdEndRenderPass`, `vkCmdBindPipeline`, `vkCmdBindVertexBuffers`, `vkCmdDraw` |
| Sync and submit | `vkQueueSubmit`, `vkQueueWaitIdle`, `vkCreateFence`, `vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkWaitForFences`, `vkCreateSemaphore`, `vkDestroySemaphore` |
| Display | `vkGetPhysicalDeviceDisplayPropertiesKHR`, `vkGetPhysicalDeviceDisplayPlanePropertiesKHR`, `vkGetDisplayPlaneSupportedDisplaysKHR`, `vkGetDisplayModePropertiesKHR`, `vkCreateDisplayModeKHR`, `vkGetDisplayPlaneCapabilitiesKHR`, `vkCreateDisplayPlaneSurfaceKHR`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`, `vkGetPhysicalDeviceSurfacePresentModesKHR` |
| Swapchain | `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` |

Within them:
- 32-bit float vertex formats;
- triangle lists without culling;
- one viewport and scissor, set in the pipeline;
- no depth, stencil or blending;
- one subpass with one colour attachment, cleared or left on load and stored or dropped at the end;
- one instance a draw.

**New tests,** in one title (`tests/`):
- `boot_logo`: the logo is on screen after `vkCreateDevice`, and after the first present the screen shows the app's
  image;
- `objects`: the proc-address table, memory types and buffer requirements;
- `triangle`: 120 frames, each corner's colour and the clear colour checked on the image the display shows.

**Not in milestone 1:**
- images and samplers other than the swapchain's, descriptor sets, uniform buffers and push constants (milestone 2);
- depth, stencil, blending, culling, dynamic state, index buffers and other topologies (milestone 3);
- vertex formats other than 32-bit floats, which need xsdk to set a stream's byte swap;
- loading an attachment's old contents, and render areas smaller than the framebuffer (milestone 4);
- copies, queries, compute, secondary command buffers and multisampling.

### Milestone 2: textures, descriptors and push constants (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestone 1's checks and these, each checked by
pixel on the image the display shows:
- `texture`: images copied in from buffers with `vkCmdCopyBufferToImage`, in `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`,
  sampled through combined image sampler descriptors;
- `push_constants`: quads placed and coloured by push constants, a vertex range and a fragment range;
- `uniform_buffers`: quads placed by a matrix and coloured by vectors from uniform buffers, one of them dynamic and read
  at a different offset each draw, in two descriptor sets.

**Passed on hardware** on 2026-10-11, at `e24c820` with xsdk at `7ff6c4c` and xost at `b09a2d8`
(`results/2026-10-11-milestone-2/`). All 33 checks passed, milestone 1's 20 among them, and the console went back to
the Aurora dashboard.
- **Textures:** every texel of both 4x4 images showed in every frame, read through combined image sampler
  descriptors. Red and blue held their places in both byte orders, the second image copied from padded rows.
- **Push constants:** each quad took its place from the vertex range and its colour from the fragment range, both
  pushed before each draw.
- **Uniform buffers:**
  - each quad landed where its column-major matrix put it, read at three dynamic offsets;
  - its colour came through the same block;
  - set 1's tint halved green, from a second descriptor set and the fragment shader's own registers.
- **New paths in it that ran for the first time:** blocks copied into constants at replay; the two byte-order
  formats; xost's column-major products; a constant written straight to a varying.
- **Captures:** `texture.png`, `push_constants.png` and `uniform_buffers.png` in the results folder.

**Decisions:**
- **Where shader resources land is xost's choice, recorded in the bundle.**
  - Each shader gets its own registers. Push constants take the first ones, then each uniform block in
    (set, binding) order, each from its first byte. Combined image samplers take fetch constants from 0 in (set,
    binding) order. xost works this out from the SPIR-V alone, as `resolve_interface`.
  - `tools/build_shaders.py` writes the result into the bundle as each shader's resources: a kind, set, binding,
    first register or fetch constant, and register count.
  - `vkCreateGraphicsPipelines` matches them against the pipeline layout and refuses what the layout lacks.
- **Uniform blocks and push constants are copied in as raw bytes.**
  - A member at byte offset o of a block sits in register first + o / 16, from component (o % 16) / 4.
  - A matrix stays column-major, one column a register, as std140 lays it out. So xost lowers `M * v` as a
    multiply-add over columns and `v * M` as dot products, the reverse of what it does for OpenGL's row-uploaded
    uniforms.
  - At replay, each draw copies every block its shaders read into float constants
    (`xsdk_gpu_set_vertex_constants`, `xsdk_gpu_set_pixel_constants`), from the buffer at its offset plus any
    dynamic offset, as section 2 decided. Mapped memory is uncached, so each read is slow; milestone 5 measures it.
- **Images.**
  - 2D, optimal tiling only, which is Xenos's tiled layout with mips as xsdk milestones 15 and 16 settled.
  - Formats: `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`. Vulkan lays their bytes out in component order, so the texture
    fetch reads them unswapped. xsdk gains these two byte-order formats beside its 0xAARRGGBB one.
  - xsdk splits working out a texture from placing it, so an image's texels live in the `VkDeviceMemory` it's bound
    to.
- **Copies run on the CPU at replay.** Replay first
  waits for the GPU to finish what came before, since nothing yet orders the GPU's reads of the image against the
  CPU's writes. `vkCmdCopyBufferToImage` copies whole mip levels.
- **Samplers** become xsdk's sampler state: point or linear filtering, nearest or linear mips, repeat, mirrored repeat
  or clamp to edge, and a LOD bias. Border colours, anisotropy, comparison and LOD clamps return
  `VK_ERROR_FEATURE_NOT_PRESENT`; they're xsdk TODO items.
- **Descriptor sets** hold uniform buffers, dynamic uniform buffers and combined image samplers, one descriptor a
  binding. Fragment shaders sample; vertex shaders don't yet, since both stages share the fetch constants.
- **`vkCmdPipelineBarrier`** records nothing: replay is in order, and copies wait for the GPU.

**New in xost:** uniform blocks by set and binding, push constant blocks, members at any std140 offset, column-major
matrices, and samplers by set and binding, each with a regression test.

**New in xsdk:** `xsdk_texture_init` and `xsdk_texture_attach`, splitting `xsdk_texture_create`; the two byte-order
formats.

**New functions:** `vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`, `vkBindImageMemory`,
`vkCreateSampler`, `vkDestroySampler`, `vkCreateDescriptorSetLayout`, `vkDestroyDescriptorSetLayout`,
`vkCreateDescriptorPool`, `vkDestroyDescriptorPool`, `vkResetDescriptorPool`, `vkAllocateDescriptorSets`,
`vkFreeDescriptorSets`, `vkUpdateDescriptorSets`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`,
`vkCmdCopyBufferToImage`, `vkCmdPipelineBarrier`.

**Not in milestone 2:**
- linear images, other formats, partial copies, image arrays and views of part of an image;
- separate images and samplers, descriptor arrays, storage buffers and texel buffers;
- sampling in vertex shaders;
- dynamic indexing into uniform arrays;
- other copies and blits.

### Milestone 3: pipeline state (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestones 1 and 2's checks and these, each checked
by pixel on the image the display shows:
- `indexed`:
  - triangle lists, strips and fans, line lists and strips, and points, from 16- and 32-bit index buffers;
  - a first index and a vertex offset;
  - a list of more than 65535 indices whose values pass 65535.
- `depth_stencil`:
  - a D24S8 attachment cleared with the render pass;
  - depth tested and written in both draw orders, and tested but not written;
  - the clear depth hiding what's behind it;
  - a stencil mask with no colour written, and a draw tested against it;
  - a constant depth bias.
- `raster`:
  - culling of clockwise and counter-clockwise quads under both front faces;
  - blending: alpha, additive, subtract, reverse subtract, min and max;
  - a colour write mask;
  - dynamic viewport and scissor;
  - edges a quarter pixel off the pixel grid, which tell Vulkan's pixel centres from Direct3D 9's.

**Passed on hardware** on 2026-10-11, at `376e2da` with xsdk at `604bdfb` (`results/2026-10-11-milestone-3/`). All 45
checks passed, milestones 1 and 2's 33 among them, and the console went back to the Aurora dashboard.
- **Three things this settled that were assumptions before:**
  - **Pixel centres.** Edges a quarter pixel off the grid covered exactly the pixels Vulkan's centres on halves say,
    600 to 699 and 500 to 599. So the half-pixel viewport shift is right, and Xenos centres pixels on whole
    coordinates under `PIX_CENTER_D3D`, as Xenia says.
  - **Front faces.** Every culling case matched with `frontFace` mapped straight to `front_clockwise`, under the
    flipped viewport. Xenos decides facing from where vertices land on screen.
  - **Triangle fans,** first drawn on the console.
- **Indexed:**
  - triangle lists, strips and fans, line lists and strips, and points, from 16- and 32-bit indices;
  - a first index and vertex offsets;
  - a list of 65541 indices split in two, its quad at indices past 65535.
- **Depth and stencil:**
  - depth cleared to 0.5 with the render pass;
  - nearer won in both draw orders, and a draw that wrote no depth was covered by a farther one;
  - nothing showed behind the clear;
  - stencil drew only over a mask written without colour;
  - a constant bias of -0.01, given in 2^-24 units, pulled an equal-depth quad in front.
- **Raster:**
  - culling under both front faces;
  - alpha, additive, subtract, reverse subtract, min and max blends within 2 per channel;
  - a red-and-blue write mask;
  - a dynamic viewport filled and a scissor clipped, the scissor's right and bottom edges exclusive.
- **Captures:** `indexed.png`, `depth_stencil.png` and `raster.png` in the results folder.

**Decisions:**
- **State maps onto what xsdk milestones 12 to 14 proved**, worked out when a pipeline is created
  (`xsdk_gpu_depth_stencil_state`, `xsdk_gpu_blend_state`, `xsdk_gpu_raster_state`) and set when it's bound.
  - Vulkan's compare and stencil ops are xsdk's, value for value.
  - Blend factors and ops map one to one, except constant colours, which return `VK_ERROR_FEATURE_NOT_PRESENT` (an
    xsdk TODO). Min and max ignore their factors in Xenia (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324), as
    Vulkan says; the layer sets them to one anyway, so the result holds either way.
- **Front faces.** Vulkan's clockwise is clockwise as the triangle looks in its y-down framebuffer. xsdk's culling test
  measured it the same way, as it looks on screen (xsdk:tests/test_raster.c:207). The viewport flip keeps every
  vertex where Vulkan puts it, so `frontFace` maps straight to xsdk's `front_clockwise`.
- **Depth bias.**
  - The constant factor is in units of the depth format's resolution, 2^-24 for D24. The hardware's offset is in depth
    units: Xenia scales it by 2^24 - 1 (xenia:src/xenia/gpu/draw_util.h:165-172). So the layer divides by 2^24.
  - The slope factor passes through as xsdk's per-pixel slope. Unverified, as in xsdk (xsdk:docs/HARDWARE.md,
    milestone 14).
- **Depth attachments** are `VK_FORMAT_D24_UNORM_S8_UINT`, living in eDRAM beside the colour.
  - A clear load op is the resolve's depth and stencil clear (`xsdk_gpu_set_depth_clear`). eDRAM's known contents now
    cover depth and stencil, and a pass that clears to what it holds does nothing more.
  - Depth isn't resolved out (an xsdk TODO), so a store op keeps nothing. That can't be seen: nothing that could read
    the image afterwards exists, as loading it returns `VK_ERROR_FEATURE_NOT_PRESENT` and depth images can't be
    sampled or copied.
  - Depth images take a page of memory they never use.
- **Topologies:** point lists, line lists and strips, triangle lists, strips and fans. Fans are new to xsdk:
  `XSDK_PRIMITIVE_TRIANGLE_FAN` is Xenos's `kTriangleFan` (xenia:src/xenia/gpu/xenos.h:50), first proven here.
  Primitive restart returns `VK_ERROR_FEATURE_NOT_PRESENT` (an xsdk TODO).
- **Index buffers** go to `xsdk_gpu_draw_indexed`. A first index moves the address, and a vertex offset moves each
  stream as a first vertex does. Lists of more than 65535 indices are split; strips and fans that long return
  `VK_ERROR_FEATURE_NOT_PRESENT`. Index values reach 2^24 - 1, xsdk's `VGT_MAX_VTX_INDX`.
- **Dynamic state:** viewport and scissor, one of each. Other dynamic state returns `VK_ERROR_FEATURE_NOT_PRESENT`.

**New in xsdk:** `XSDK_PRIMITIVE_TRIANGLE_FAN`.

**New functions:** `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`.

**Not in milestone 3:**
- two-sided stencil, constant blend colours, primitive restart, fill modes, wide lines and points, and depth bounds;
- depth resolves, so loading or sampling depth;
- dynamic state beyond viewport and scissor;
- instancing.

### Milestone 4: render to texture (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestones 1 to 3's checks and these, each checked
by pixel on the image the display shows:
- `render_to_texture`: eight render passes a frame for 60 frames, in the order encounter's Aurora draws a frame:
  1. a scene drawn into a 200x120 `R8G8B8A8_UNORM` image with a D24S8 attachment, both cleared, depth tested;
  2. that image drawn into a 96x64 `B8G8R8A8_UNORM` image, as encounter's Aurora copies its framebuffer;
  3. the scene loaded again, colour and depth, with a quad that shows only where it's nearer than pass 1's depth;
  4. the scene loaded straight after, in a pass without depth, with one more quad;
  5. both images sampled onto the swapchain image;
  6. a 64x32 `B8G8R8A8_UNORM` image cleared and drawn;
  7. the swapchain image loaded after it, with that image drawn over it;
  8. the swapchain image loaded straight after, with one more quad.
- So the checks cover:
  - depth tested in an image of another size than the screen, its last column intact;
  - red and blue in place in both byte orders;
  - colour loaded after another pass (3 and 7), where the image is drawn back in, and straight after (4 and 8),
    where eDRAM still holds it;
  - depth loaded after a pass without depth (3).

**Passed on hardware** on 2026-10-11, at `91612ec` with xsdk at `c5d87ed` (`results/2026-10-11-milestone-4/`). All 58
checks passed, milestones 1 to 3's 45 among them, and the console went back to the Aurora dashboard. The images were
read back after pass 1 and again after pass 4, and every frame of 60 was checked on screen.
- **The scene:**
  - depth tested at 200x120, with depth at the top of eDRAM;
  - its last column held the red drawn there, past the width's last whole tile of 32, written by resolving the
    texture's 224-texel pitch;
  - red and blue in place in R8G8B8A8, resolved without the red-blue swap, and in B8G8R8A8, with it.
- **The copy** held the scene as pass 1 left it, without passes 3 and 4's quads.
- **Loads after another pass,** where eDRAM held something else, so the layer drew the image back in:
  - pass 3 drew the scene back, and its blue showed only where it was nearer than pass 1's depth. So depth stayed in
    eDRAM through the copy pass, which had none;
  - pass 7 drew the swapchain image back from its front buffer, sampled as a tiled B8G8R8A8 texture.
- **Loads straight after:** passes 4 and 8 found eDRAM holding their images, so the layer drew nothing back. The
  resolves before them, which cleared nothing, left eDRAM's colour as it was.
- **Xenia disagreed:** there, a draw sampling an image resolved into got its earlier contents (CLAUDE.md). The
  console decides.
- **Capture:** `render_to_texture.png` in the results folder.

**Decisions:**
- **Where attachments go in eDRAM.** Colour from tile 0, and depth from the top, ending at tile 2048. xsdk put depth
  at 0 and colour after it (libxenon:drivers/xenos/xe.c:998-1036), so a smaller colour target landed in a larger
  one's depth. Now a pass without depth leaves an earlier pass's depth alone.
- **What eDRAM holds.** The layer tracks eDRAM's colour and its depth apart: unknown, cleared to values at a
  framebuffer's size, or an image's contents.
  - A clear finds its values there, or a resolve into the attachment sets them, as milestone 1 decided.
  - A load finds its image there, or the image is drawn back in.
  - A pass's resolve clears what the next pass in the command buffer clears, and keeps what it loads. The last pass
    in a command buffer clears to its own clear values, as milestones 1 to 3 did, so the next frame finds them.
  - Copying into an image eDRAM holds makes eDRAM's copy unknown.
- **Drawing an image back in.** eDRAM's modes draw or copy out; none loads memory in
  (xenia:src/xenia/gpu/xenos.h:886-910). So a load draws the image over the framebuffer with the layer's own shaders,
  point-sampled texel for texel, with no blending, depth or stencil. Those shaders are built offline like an app's,
  into `src/blit_shaders.c`.
- **Depth stays in eDRAM.** Depth resolves are an xsdk TODO, so depth can be loaded only while eDRAM still holds it:
  no other depth attachment and no colour reaching its tiles since. Otherwise its contents are undefined, and the
  layer logs that once.
  - Depth and stencil clear together, as one register holds both (xsdk:src/gpu_resolve.c), so a pass that clears one
    and loads the other returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Resolves into images.** xsdk resolves into tiled textures of the byte-order formats now, swapping red and blue for
  `B8G8R8A8` and leaving the bytes in place (xenia:src/xenia/gpu/shaders/resolve.xesli:768-795). It resolves a
  texture's whole pitch, a multiple of 32, so widths needn't be one.
- **Colour attachment images:**
  - `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`, one level, both sides over 16 texels. A smaller base level sits in the
    packed mip tail, where a resolve can't write (xsdk:src/gpu_resolve.c:180-181).
  - A framebuffer's colour and depth fit eDRAM's 2048 tiles together.
- **Swapchain images load the same way.** A front buffer holds what a resolve into a tiled 1280x720 `B8G8R8A8`
  texture writes, at the same size (xsdk:src/gpu_resolve.c:171-174), so the layer samples it as one.

**New in xsdk:** `xsdk_gpu_set_target_at` and `xsdk_gpu_target_tiles`, to place targets in eDRAM;
`xsdk_gpu_set_resolve_clears`, as a resolve cleared both colour and depth before; byte-order formats in
`xsdk_gpu_resolve_texture`.

**New functions:** none. Colour attachment images, `VK_ATTACHMENT_LOAD_OP_LOAD` and `R8G8B8A8_UNORM` colour
attachments are new.

**Not in milestone 4:**
- render areas smaller than the framebuffer. Milestone 1 put them here, but they need a resolve of part of eDRAM or
  clears by drawing, and encounter's Aurora always renders whole targets. They wait for an app that needs them.
- depth resolves, so depth loaded after eDRAM lost it, sampled or copied;
- colour attachments 16 texels or less on a side, with mips, or more than one in a subpass.

### Milestone 5: the benchmark (done)

**Done when** `build/vkbench.xex`, on the console, runs each scene through xsdk's GPU layer and then through
vulkan4xenon, in two runs of the title, and this section records each scene's draw + submit and GPU time through the
layer against xsdk's, with the gap's causes named from measurements.

- **vkbench:** scenes, data and per-frame metrics, with a backend for xsdk's GPU layer and one for vulkan4xenon.
  - Both backends are in one title and run one after the other, so code they share is laid out the same for both.
  - Both draw with the same xost-compiled shaders: the xsdk backend loads the containers of the bundle vulkan4xenon
    looks SPIR-V up in, and sets their constants where the bundle says.
  - Each opens the GPU, the boot logo and the display as `vkCreateDevice` and the swapchain do, so the logo shows
    before each half.
- **Scenes:**
  - `draws_250`, `draws_1000` and `draws_4000`: that many cubes, one draw each, with depth, no state changes;
  - `states_250` and `states_1000`: the texture changes every draw, the shader every second and blending every fourth;
  - `uniforms_1000`: `draws_1000` with each draw's matrix and tint in a dynamic uniform buffer, as encounter's Aurora
    passes its uniforms, where the other scenes push them. Milestone 2 left the cost of
    reading uniform buffers at replay for this one to measure;
  - `stream_1000` and `stream_8000`: that many sprites, moved on the CPU and written out every frame, one draw;
  - `fill_4` and `fill_16`: alpha-blended full-screen layers, bound by the GPU, so both should tie.
  - vkbench's minify scenes are left out: they compare linear and swizzled images, and images here are always tiled.
- **Per frame, in microseconds,** as vkbench measures them: sim, draw, submit, present and frame, and also gpu: from
  the submit's kick to its fence landing, with the CPU spinning on it. 60 warm-up and 600 measured frames a scene.
  Medians, means and 99th percentiles go in the log and in `game:\vkbench_summary.csv`, every frame in
  `game:\vkbench_frames.csv`.
- **Accounting,** checked before any ratio:
  - xsdk's draw calls write the ring as they go and the layer writes it in `vkQueueSubmit`, so draw + submit is the
    number to compare, not either alone;
  - both kick the ring only at their fence, or when it fills (xsdk:src/gpu.c), so the GPU starts at submit in both;
  - both wait for the next image in present: `xsdk_present_begin` and `vkAcquireNextImageKHR` move there;
  - both present in FIFO at every vblank, the only mode the swapchain has, so frame times step in vblanks; gpu shows
    what the GPU took.
- **The same picture:** each scene's last frame is sampled on a grid in both backends, and the two must match.
- **Differences from vkbench:** 1280x720; sprite colours are four floats, as the layer takes only 32-bit float vertex
  formats; v-sync stays on.

**Passed on hardware** on 2026-10-11, in two whole runs. Each passed all 14 checks, both backends drew the same
picture in every scene, and the console went back to the Aurora dashboard.
- **Run 1**, at `ff0f2c7` with xsdk at `a441b04` (`results/2026-10-11-milestone-5/`: `vkbench_run1.log` and its
  CSVs), on xsdk's 32 KB ring.
- **Run 2**, at `077d2c4` with xsdk at `b5933a0` (xsdk:results/2026-10-11-milestone-19-stream-3/), after xsdk
  milestone 19 put commands into a stream the ring runs every 1024 dwords as they're written, and with the ring's
  counts. So the GPU starts during xsdk's draw calls and the layer's submit, and gpu counts only what's left after it.
- **The test title** passed all 58 checks after each (`results/2026-10-11-milestone-5/vulkan4xenon_test.log`,
  xsdk:results/2026-10-11-milestone-19-regression/).

Draw and submit, median, in microseconds:

| Scene | xsdk, run 1 | vulkan4xenon, run 1 | xsdk, run 2 | vulkan4xenon, run 2 |
|---|---|---|---|---|
| `draws_250` | 8,049 | 11,684 | 307 | 724 |
| `draws_1000` | 37,590 | 47,515 | 1,213 | 2,867 |
| `draws_4000` | 151,190 | 195,871 | 4,836 | 11,703 |
| `states_250` | 13,987 | 15,686 | 545 | 907 |
| `states_1000` | 55,496 | 65,512 | 2,167 | 3,607 |
| `uniforms_1000` | 37,590 | 48,514 | 1,214 | 6,852 |
| `stream_1000` | 700 | 704 | 698 | 702 |
| `stream_8000` | 5,553 | 5,565 | 5,554 | 5,564 |
| `fill_4` | 12 | 20 | 10 | 16 |
| `fill_16` | 22 | 49 | 17 | 39 |

GPU time tied: within 1% in the sprite and fill scenes in both runs, and in the cube scenes both finished 0.3 to
0.5 ms after the submit, about the frame's resolve.

**The gap's causes,** from the counts:
- **Run 1: both waited for xsdk's ring.** A cube wrote 156.4 dwords through xsdk and 200.3 through the layer (run 2's
  counts; what a draw writes didn't change between the runs). The 32 KB ring filled every 52 cubes through xsdk, and
  each fill slept about 2 ms, a 1 ms sleep having measured 1,993 us. So the layer's 1.28 times the dwords cost it
  1.26 and 1.30 times the time in `draws_1000` and `draws_4000`.
- **Run 2: nothing waited, so the CPU's work shows.** A cube cost xsdk 1.21 us and the layer 2.93: 0.23 recording and
  2.69 at submit, replaying it into xsdk's calls (`src/queue.c`). The layer's 44 extra dwords point the vertex stream
  and the texture again every draw, each behind a coherency request, though neither changed.
- **Uniform buffers:** `uniforms_1000` cost the layer 4.0 us a draw more than `draws_1000`, for reading two blocks, 20
  words in all, from mapped memory at submit. That memory is write-combined, and reads from it are slow. Run 1 showed
  1.0 us.
- **Sprites tie:** both write vertices with `memcpy` into write-combined memory, which runs at 185 MB/s on the console
  (xsdk:docs/HARDWARE.md), so `stream_8000`'s 1 MB takes 5.5 ms either way.
- **Fill ties on the GPU:** each blended full-screen layer took 462 us, about 2.0 Gpixels a second, in both.

**Decisions:**
- **Command buffers grow.** `draws_4000` records 12,000 commands a frame, and 320 KB of push constants, past the 4096
  commands and 16 KB of data each command buffer had. Each starts at that size now and doubles when it fills, keeping
  its size through resets, so recording stops allocating after the first frames.
- **xost fetches into r0 last** (xost `098f17a`). Vertex input n is fetched into rn, and every fetch reads the vertex
  index from r0, so fetching input 0 first overwrote the index the rest read. In Xenia, which reads the index as each
  fetch runs (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88), sprites took their uv and colour from the
  vertex their x position named, and the two backends' pictures differed until the fix. Milestone 2 passed on the
  console with the old order, so the hardware may not show it. Shaders with one input are unchanged; the test titles'
  and the blit shaders' containers are rebuilt.
- **The GPU's time is spun on.** `xsdk_gpu_wait` sleeps a millisecond between polls, which would round gpu up to
  whole milliseconds, so the xsdk backend spins on `xsdk_gpu_fence_done` and the Vulkan one on `vkGetFenceStatus`.
- **No display, no swapchain.** Where the display won't open, as in Xenia, each backend draws into an image of the
  screen's size and samples that, and logs that it did. So the bench runs and compares pictures in Xenia, whose
  timings mean nothing.
- **Comparing pictures:** at every 20th pixel across and down, from the 10th, 64 by 36 points, each channel within 2.
  The first 3 points that differ are logged with both colours.
- **Counting the ring, after run 1.** Run 1 left the causes inferred from code, so vkbench now also logs, for each
  scene, what xsdk's ring did a frame: dwords, kicks, waits for room and the time waited
  (`xsdk_gpu_stats`). It also times a 1 ms sleep, and 1 MB written into write-combined memory and into cached memory
  that is then flushed. Run 2 carries them; they are xsdk milestone 19's first step (xsdk:SCOPE.md).

**New functions:** none. Every call vkbench makes was there after milestone 4.

### Milestone 6: the replay's cost (done)

**Why.** After xsdk milestones 19 and 20, a cube drawn through the layer cost 1.50 us to xsdk's 0.63, and one taking
its matrix and tint from uniform buffers 3.8 us more (xsdk:docs/HARDWARE.md, "Fewer dwords a draw"):
- the replay points the vertex stream and the texture again for every draw, 22 dwords and the calls behind them,
  though neither changed (`src/queue.c`);
- it reads each uniform block from mapped memory a word at a time, and mapped memory is write-combined, so each read
  goes to memory: 20 words, about 190 ns each.

**In two steps,** each run on the console:
1. **Point what changed.** The replay remembers each stream's address and size and each slot's texture and sampler,
   and points them only when they differ, forgetting them where something else points them: a resolve, drawing an
   image back in, and the start of each submit.
2. **Uniform buffers the GPU loads.** `PM4_LOAD_ALU_CONSTANT` has the command processor copy constants from memory
   (xenia:src/xenia/gpu/command_processor.cc:1477-1515), so the CPU wouldn't read the buffer at all. A probe on the
   console first, at the sizes blocks take, as a lone float constant written as a register hung the GPU
   (xsdk:docs/HARDWARE.md). If it takes them, each block loads straight from its buffer; if not, the layer reads blocks
   16 bytes at a time with VMX, a quarter of the reads.

**Done when,** on the console:
- vkbench's `draws_1000` through the layer draws and submits faster than its 1,503 us, in fewer dwords a draw;
- `uniforms_1000` costs at most a third of its 3.8 us a draw more than `draws_1000`;
- vulkan4xenon_test passes in full, and vkbench's backends still draw the same pictures.

**Passed on hardware** on 2026-10-11, at `ef044d8` with xsdk at `051bffd` (`results/2026-10-11-milestone-6/`: vkbench's
log and CSVs, and the test title's tail).
- **vkbench passed all 14 checks,** both backends drawing the same picture in every scene, with no waits. Through the
  layer, drawing and submitting took:
  - `draws_1000` 1,221 us, from 1,503, in 65.3 dwords a cube, from 87.1; `draws_4000` 5,062 us, from 6,218;
    `states_1000` 2,006 us, from 2,111;
  - `uniforms_1000` 2,091 us, from 5,309, so its uniform buffers cost 0.87 us a draw more than `draws_1000`'s push
    constants, from 3.8: five 16-byte reads of write-combined memory, about 175 ns each.
  - A cube costs 1.97 times xsdk's, from 2.40.
- **The test title passed all 58 checks,** its render to texture among them: eight passes a frame, with resolves and
  images drawn back between them, where the replay forgets what it pointed.
- **VMX loads from write-combined memory work,** the first on the console.

**Decisions:**
- **`LOAD_ALU_CONSTANT` waits.** The console loads float constants from memory only in whole groups of 4 from a
  multiple of 4, and hung loading a group of 3 (xsdk:docs/HARDWARE.md). Blocks aren't placed on those groups, so a load
  would overwrite their neighbours. xost placing each block on its own groups would let the GPU load it with no CPU
  reads at all.
- **Pointing is forgotten at each render pass's start,** not tracked through the resolves and the image drawn back:
  draws happen only inside passes, and the first draw of each points what it needs.

### Milestone 7: uniform blocks the GPU loads (done)

**Why.** Milestone 6 left uniform buffers costing 0.87 us a draw: the CPU reads each block from write-combined memory,
16 bytes at a time. The console loads float constants from memory with `LOAD_ALU_CONSTANT`, but only in whole groups
of 4 from a multiple of 4 (xsdk:docs/HARDWARE.md), and xost placed blocks wherever the last one ended, so a load would
overwrite whatever shared a block's groups.

**In three parts,** run on the console together:
1. **xost gives each uniform block whole groups of 4 to itself** (xost `b9b38e9`): it starts on a multiple of 4, and
   whatever follows it, another block or the literal pool, starts on the next. Push constant blocks stay packed, as the
   CPU writes them.
2. **xsdk loads float constants from memory** (xsdk `5d4bda6`): `xsdk_gpu_load_vertex_constants` and
   `xsdk_gpu_load_pixel_constants`, whole groups of 4 from a multiple of 4, from 64-byte aligned memory, as the console
   loaded them.
3. **The replay has the GPU load each uniform block** from its buffer, its padding taking whatever follows the block,
   which nothing reads. A block whose buffer isn't 64-byte aligned, or whose descriptor's range doesn't cover it, still
   goes through the CPU. Buffers are 64-byte aligned now, and uniform offsets go in 256s.

**Done when,** on the console:
- vkbench's `uniforms_1000` through the layer costs at most 0.2 us a draw more than `draws_1000`, from 0.87;
- xsdk_test and vulkan4xenon_test pass in full, and vkbench's backends still draw the same pictures;
- xost's tests pass.

**Passed on hardware** on 2026-10-11, at `02f593b` with xsdk at `5d4bda6` and xost at `b9b38e9`
(`results/2026-10-11-milestone-7/`).
- **vkbench passed all 14 checks,** both backends drawing the same picture in every scene. Through the layer,
  `uniforms_1000` drew and submitted in 717 us, from 2,091, in 17.3 dwords a draw, from 49.3: 0.50 us a draw less than
  `draws_1000`'s push constants, where milestone 6 left it 0.87 more. It costs 1.16 times xsdk's own, from 3.4.
  - The GPU had 0.49 ms left at the submit, from 0.30: the CPU finishes sooner, and each load is the GPU's work now.
    The scene's draw, submit and GPU time still fell, 1.2 ms from 2.4.
- **The test title passed all 58 checks** on the second run (`vulkan4xenon_test-tail.log`), its uniform buffers loaded
  by the GPU at the registers xost gave them. The first (`vulkan4xenon_test-alignment-check.log`) failed only the
  objects check that still expected 16-byte buffer alignment; it drew everything right.
- **xsdk_test passed all 203 checks** (xsdk:results/2026-10-11-constant-loading/), test_constants's quads drawn right
  with their constants loaded from memory.
- **xost's tests pass,** and its block test fails with the groups of 4 turned off.

**Decisions:**
- **Push constant blocks stay packed.** The CPU writes them in groups of 4 from its copy, so they can share groups with
  literals; only a uniform block, which the GPU loads, needs groups to itself.
- **Buffers are 64-byte aligned,** as the console loaded constants from; with uniform offsets in 256s, every uniform
  block in a buffer bound at a 64-byte offset can be loaded.

### Milestone 8: push constants the GPU loads (reverted)

**Why.** After milestone 7 a draw taking its matrix and tint from uniform buffers cost 0.72 us through the layer, and
one pushing them 1.22 us: the CPU writes push blocks into constants itself, through xsdk's setters and their copy, 65
dwords a cube against a uniform draw's 17.

**In two parts,** run on the console together:
1. **xost gives push blocks whole groups of 4 to themselves,** as uniform blocks have, so loading one can't overwrite
   anything else.
2. **A ring the GPU loads push constants from.** A 4 MB ring of write-combined memory: each draw copies its push blocks
   in, padded to groups of 4, and has the GPU load them. A submit's bytes are reused once the GPU passes its fence, and
   the invalidate each submit starts with keeps the GPU's caches from holding an earlier lap. A submit that would lap
   the ring by itself, or a block that doesn't start a group, goes through the CPU as now.

The bytes the CPU stores barely change, about 48 into the ring against 56 written into constants; what goes is the
setters' bookkeeping.

**Done when,** on the console:
- vkbench's `draws_1000` through the layer draws and submits faster than its 1,215 us, measurably; otherwise the
  change is reverted (xsdk:CODE_GUIDELINES.md, "Measure, don't guess");
- vulkan4xenon_test passes in full, and vkbench's backends still draw the same pictures;
- xost's tests pass.

**Not faster on hardware,** so reverted on 2026-10-11. vkbench ran at `2e94c05` with xsdk at `a7151aa` and xost at
`af5bb8b` (`results/2026-10-11-milestone-8/`), passing all 14 checks with no waits. Through the layer, against
milestone 7's run:
- **`draws_1000` drew and submitted in 1,217 us, from 1,215,** its submit 974 us both times, though a pushed cube took
  17.3 dwords, from 65.3, and a frame 18 kicks, from 64.
- **`states_1000` took 2,021 us, from 1,975,** and `draws_4000` 5,078, from 5,043; `uniforms_1000` was unchanged at
  718.
- **The GPU took about 24 us longer after each cube scene's submit:** 325 us in `draws_1000`, from 301, and 512 in
  `uniforms_1000`, from 488, though the build changed neither that scene's path nor its shaders. Why is unmeasured.

**What it showed:** a pushed cube and a uniform one now wrote the same 17.3 dwords and 18 kicks, yet the pushed one's
submit cost 0.97 us a draw against 0.50. Writing three quarters fewer dwords saved nothing, so the push path's cost
isn't the command stream. What the push path does beyond the uniform path is copy each `vkCmdPushConstants` into the
replay's push array, then copy each block back out, into constants before, into the ring here. Where the time goes in
that is unmeasured.

The layer is back at milestone 7's code. xost packs push blocks again; its `vk_block_literals` test stays, its
uniform block still on c4 and its literals from c8.

### Milestone 9: Dusklight on Aurora's null backend

**Why.** Every later milestone needs the game running on the console, and running it settles the platform, the disc,
memory and byte order before anything draws. Read on 2026-10-11, what stands in the way:
- **Dusklight quits on the null backend:** with `BACKEND_NULL` it runs only its launcher's UI loop and returns
  (dusklight:src/m_Do/m_Do_main.cpp:737-751).
- **SDL3 is under everything:** Aurora's events, controller, file streams, paths, ticks and delays, threads and audio,
  and Dusklight's frame limiter (dusklight:src/dusk/time.h:31-72), with no port to the 360. Aurora pins release
  3.4.10 (aurora:cmake/AuroraDependencyVersions.cmake:20).
- **nod, Aurora's disc reader, is Rust** (aurora:cmake/AuroraNodProvider.cmake:41-80), with no target here.
- **RmlUi,** Dusklight's own menus, builds only with Dawn, as it draws with WebGPU.
- **Byte order:** the console is big-endian like the GameCube, so the PC build's swaps must turn off. Dusklight's data
  is mostly typed `BE()` fields behind one switch, `TARGET_LITTLE_ENDIAN`, but its big-endian branch has no `BSWAP64`
  and swaps constants regardless (dusklight:include/helpers/endian.h:9-29, 63-98), and Aurora's command stream writer
  swaps every word (aurora:lib/gx/fifo.hpp), which breaks even the null backend.
- **Types:** PowerPC's `char` is unsigned, and Dusklight adds `-fsigned-char` only on ARM
  (dusklight:CMakeLists.txt:256-260); xsdk's `uint32_t` is `unsigned long`, the PC build's `u32` `unsigned int`
  (dusklight:libs/dolphin/include/dolphin/types.h:15-17).
- **Memory:** the game asks for 256 MB of GameCube memory and 24 MB of ARAM (dusklight:src/m_Do/m_Do_main.cpp:705-706),
  and Aurora's null backend about 63 MB of pools; xsdk's heap reserves 256 MB (xsdk:src/sbrk.c). What the kernel
  leaves a title is unmeasured.

**Decided with the user:** SDL3 is ported, an xsdk platform added to it in a fork of its own, so Aurora, Dusklight and
borealis call it unchanged; the disc is a plain ISO on the console's drive; the forks are Z:\dusklight-xenon,
Z:\aurora-xenon and Z:\SDL-xenon (from release 3.4.10). Switched off: code mods, which load code
at run time; Dawn and nod; borealis's HTTP, network, crash, Discord, update and Sentry parts. Dusklight's menus, RmlUi,
run with a render interface that draws nothing: their header uses RmlUi's types in 23 of the game's files, so turning
them off would touch all of those, and the vulkan4xenon backend needs RmlUi's renderer later anyway (agreed with the
user after the scope above). Their settings come from `config.json` until that renderer brings them back.

**In three steps,** each run on the console:
1. **It builds and starts.** A CMake toolchain file in xsdk; the forks wired together; the libraries the build keeps
   built with xsdk's toolchain. The `.xex` shows the splash, logs its size and the memory the kernel left free, and
   exits.
2. **The platform.** SDL3's xsdk backends: the controller on `XamInputGetState`, audio on xsdk's audio client, the
   timer, threads on xsdk's pthreads and paths on `game:\`; in xsdk, what they need beyond it today, as xsdk
   milestone 21 (xsdk:SCOPE.md). Dusklight runs the game on the null backend up to the disc. The game reads the
   disc's ID before `main01` (dusklight:src/dusk/version.cpp:13-24, called at dusklight:src/m_Do/m_Do_main.cpp:903),
   so with no reader yet the `.xex` reports the disc missing and ends; reaching `main01` moved to step 3, agreed with
   the user on 2026-10-12.
3. **The disc, byte order and memory,** as scoped with the user on 2026-10-12:
   - **The disc:** a reader for plain GameCube ISOs behind nod's C API, in Aurora's fork. It reads the disc header,
     holds boot.bin, bi2.bin, the apploader, main.dol and the FST in memory, since Dusklight reads data out of
     main.dol (dusklight:src/dusk/dvd_asset.cpp:39), walks the FST as nod does and reads files by offset. Wii and
     compressed images are refused. A GameCube disc holds 1,459,978,240 bytes, so 32-bit offsets reach all of it.
   - **Byte order, as far as the null backend runs it:**
     - Aurora's command-stream writer swaps every value (aurora:lib/gx/fifo.hpp), while its reader swaps only data
       not in the host's order (aurora:lib/internal.hpp); the writer will swap only on a
       little-endian host.
     - Dusklight swaps the `BE<>` values the compiler sets at build time whatever the host
       (dusklight:include/helpers/endian.h:63-98). About 10 are hit, among them the message files' signature
       (dusklight:libs/JSystem/src/JMessage/data.cpp:5) and the audio envelope tables.
   - **Left for milestone 11,** when drawing shows whether they're right: Aurora's texture conversion swaps; the
     vertex and matrix arrays Dusklight flags little-endian in 58 places; and the memory card and controller binding
     files, which read back on the console but differ from a PC's.
   - **Memory:** the PC's sizes stay, 256 MB of main memory and 24 MB of ARAM
     (dusklight:src/m_Do/m_Do_main.cpp:705-706), inside the 442 MB the title has free.
   - **The run ends itself** a minute into the title scene, with status 0, after logging the frame rate and each
     heap's peak.

**Done when,** on the console:
- Dusklight's `.xex` starts with the splash, opens the ISO from the console's drive and runs the title scene (stage
  F_SP102, dusklight:src/d/d_com_inf_game.cpp:1221) for a minute on the null backend, with no call failing;
- its log names the scene, its frame rate and each heap's peak;
- xsdk_test passes in full.

**All three steps passed on hardware** on 2026-10-12, xsdk_test passing in full with each (xsdk:docs/HARDWARE.md, from
"What the kernel leaves a title"):
- **Step 1,** Dusklight `455b3f5`: the `.xex` showed the splash, logged its image, 32 MB on the console's calling
  convention, and the 442 MB of the title's 480 MB left free, and exited. xsdk's heap now reserves the title's free
  memory at its first growth, 443 MB here (xsdk:results/2026-10-12-memory/, 2026-10-12-heap-reservation/ and
  2026-10-12-dusklight-mcall-xsdk/).
- **Step 2,** Dusklight `13316c3` with Aurora `c7f5905`, SDL-xenon `c86389c89` and xsdk `f770193`: the game's own
  startup ran on the null backend, reading its config, starting Aurora with no window and loading RmlUi's fonts, and
  ended with status 1 at the missing `game:\dusklight.iso` (xsdk:results/2026-10-12-dusklight-step-2/). It took two
  xsdk changes: `main` gets argc 1, since Dusklight reads its options from `argv[1]`, and a path `std::filesystem`
  makes absolute, `game:\` in front of `game:\...`, starts over at its second drive.
- **Step 3,** Dusklight `0069f0d` with Aurora `94c1353`, SDL-xenon `c86389c89` and xsdk `c98ceb8`, the GZ2E01 disc
  beside it: the title scene ran its minute on the null backend at 30 fps, after 19 fps over the 10 seconds it
  loaded in, and the title exited with status 0 (xsdk:results/2026-10-12-dusklight-step-3/). At their peaks the
  game's root heap held 137 MB of its 256 and newlib's heap 377 MB of the 443 it reserves. Two console crashes
  Xenia never showed came first: a float load from Aurora's byte-packed command stream raised an alignment exception
  (GCC had merged an integer load and a cast into it), and Dusklight's reset freed message queues whose woken threads
  then relocked their mutexes, corrupting newlib's heap. The calls that still fail don't stop it: the memory card has
  no save yet, the pipeline cache has no database (sqlite has no file system here), and there's no window to title.

With that, milestone 9 is done: Dusklight runs on the console, from its disc, up to the title scene's game loop.

**The revert passed on hardware** on 2026-10-11, at `0579b93` with xsdk at `a7151aa` and xost at `77c7605`
(`results/2026-10-11-milestone-8-reverted/`): vulkan4xenon_test passed all 58 checks, and vkbench all 14. Through
the layer it ran milestone 7's figures to within 5 us: `draws_1000` 1,215 us in 65.3 dwords a cube, its submit 974;
`states_1000` 1,975; `uniforms_1000` 719; the GPU 301 us after `draws_1000`'s submit. The same code runs that close
from one run to the next, so the ring build's differences above were its own: it was a little slower, not as fast.
xsdk's TODO.md lists the ring, for if a run shows the cost it removes.

### Milestone 10: the boot screens drawn

**Why.** The game's own frames on the console, first what it shows while it loads. Read on 2026-10-12:
- **The PC build draws nothing before the title.** It skips the GameCube's warning, Nintendo and Dolby screens
  (dusklight:src/d/d_s_logo.cpp:1227-1229), so the archives load behind black and the title scene fades in.
- **Those screens are simple:** each is one textured 2D quad, under the fader's full-screen one. They take position,
  colour and one texture coordinate, one to three TEV stages, alpha blending and an alpha compare, with no depth and no
  framebuffer copies (dusklight:libs/JSystem/src/J2DGraph/J2DPicture.cpp:704-786,
  libs/JSystem/src/J2DGraph/J2DGrafContext.cpp:33-65). Their textures, in the disc's LogoUs.arc, are I4, IA4 and IA8.
  That is about five shader configurations. The title scene's are several dozen at least, with fog, matrices picked
  per vertex, palettes and framebuffer copies.
- **A Vulkan backend fits the fork's seam** (aurora-xenon:lib/gfx/backend.hpp). Beyond what the layer had, it would
  want 8-bit vertex colours (vulkan4xenon refused them, src/pipeline.c:40-58), a second memory type, an RGBA
  swapchain, linear textures and a shader compiler at run time.
- **xost compiles a restricted GLSL** (xost:docs/xsc.md, xsc/lower.py): one store per local, no function calls and no
  unary minus, and one comparison per discard. Indices into blocks must be constants. The boot screens' shaders fit
  that; the title scene's matrices picked per vertex, fragment position and depth export don't.
- **A PC's recorded configurations don't carry over:** Aurora's pipeline cache keeps each configuration as its raw
  struct, bitfields included, with 32-bit hashes on a 32-bit host (aurora:lib/gx/pipeline.hpp, lib/hash.hpp).

**Decided with the user (2026-10-12):** the first frames are the GameCube's boot screens, restored on the console
under `__XSDK__` with the PC's code kept; the title scene moves to milestone 11. Shader configurations are recorded by
the 360's own null backend, in Xenia or on the console, instead of on a PC.

**In two steps,** each run on the console:
1. **The boot screens and their shaders.** Dusklight shows the warning, Nintendo and Dolby screens on the null
   backend. A GLSL generator for the boot screens' configurations, modelled on Aurora's WGSL
   (aurora:lib/gx/shader.cpp) and written in the form xost compiles, runs in the null backend and writes each
   configuration's GLSL to a file on `game:\`. Each recorded program compiles with glslang and xost.
2. **The backend.** Aurora's vulkan4xenon backend:
   - with one memory type, a `B8G8R8A8` swapchain, every texture tiled and uniforms at 256-byte offsets;
   - with stores that are safe for write-combined memory;
   - with its shaders from a bundle an offline step builds out of the recorded GLSL, leaving out any program xost
     can't fit in the pixel shader's 16 registers.

   The layer takes 8-bit RGBA vertex colours, and Aurora's texture conversion reads IA8 on a big-endian host.

**Done when,** on the console:
- the warning, Nintendo and Dolby screens show with their fades while the game loads;
- the title scene then runs, its configurations missing from the bundle left undrawn;
- every recorded boot-screen configuration compiles with xost;
- xsdk_test and vulkan4xenon_test pass in full.

**Step 1 passed on hardware** on 2026-10-12, Dusklight `8089c2f` with Aurora `d263186`
(`results/2026-10-12-milestone-10-step-1/`):
- the logo scene went through the warning, Nintendo and Dolby screens on the null backend at 60 fps, the warning
  holding its minute with no button pressed;
- the title scene then ran its minute at 30 fps, and the title exited with status 0;
- the console recorded 36 programs, byte for byte the ones Xenia recorded from the same build;
- glslang and xost compiled all 8 of the boot screens' and 27 of the title scene's 28. The last needs 18 registers,
  over the pixel shader's 16 (`compile.txt`), and xost only warns that it would hang.

**Step 2 passed on hardware** on 2026-10-12, Dusklight `43b8f65` with Aurora `037ac39` and this repository at
`6be3389` (`results/2026-10-12-milestone-10-step-2/`; Dusklight's log names `54e3f14`, now `b03a1f6`, the commit at
its last configure):
- **The boot screens drawn:** Aurora's vulkan4xenon backend drew the warning, Nintendo and Dolby screens, which faded
  in and out on the user's screen, at 50 to 60 fps. The title scene then ran its minute, at 19 fps over the 10 seconds
  it loaded in and 30 after, its configurations missing from the pack left undrawn, and the title exited with status 0
  (`dusklight.log`). Its other errors are the title scene's, left for milestone 11: GLSL not yet written for fog,
  lighting, matrices picked per vertex and indirect texturing, depth copies, `GXPeekZ`, and a 20x11 copy target, under
  the layer's smallest colour attachment.
- **The pack** holds 68 of the 69 programs recorded: the 36 the console recorded in step 1, and the backend's 33 clears,
  copies and present, which Xenia recorded from the same build. The title scene's 18-register program is left out, so
  its draws are skipped. Aurora's `tools/build_glsl_pack.py` builds it, through this repository's
  `tools/build_shaders.py`.
- **The tests:** vulkan4xenon_test passed all 64 checks, 8-bit packed vertex colours among them
  (`vulkan4xenon_test-tail.log`). Broken on purpose, the colours packed the other way round, it failed only the two
  packed-colour pixel checks: the yellow corner read as magenta, cyan as white and magenta as yellow
  (`vulkan4xenon_test-broken.log`). xsdk_test passed all 230 (`xsdk_test-tail.log`).
- **Memory is tight.** The title scene started with 46 MB free and ended with 7: as it loaded, the GPU's physical pages
  went from 54 to 72 MB and newlib's heap from 330 to 347 MB. Aurora's frame pools and texture caches are sized for the
  console from runs in Xenia, at about 4 times a frame's peak for the pools and 16, 16 and 8 MiB for the texture
  caches. The game's root heap keeps the PC's 256 MB, 137 of it used, until gameplay can be measured (decided with
  the user). Dusklight's run log now prints the kernel's count of the title's memory every 10 seconds; Xenia's
  `MmQueryStatistics` doesn't count as the console's does.
- **Two runs before it ran out of memory** soon after the title scene loaded. On a first run Dusklight opens a preset
  window, which the backend doesn't draw, since Aurora records RmlUi's draws only on Dawn. The press that skipped the
  warning screen chose the Dusklight preset there unseen, turning on frame interpolation and 4x shadow maps, whose
  1536x1536 targets are over the layer's 1280x720 (`dusklight-preset.log`). The window isn't opened on the console now.
  Xenia, where nothing presses a button, never showed it. Saving the settings fails on the console ("Function not
  implemented"), so the preset didn't stay.

With that, milestone 10 is done: the console draws the game's boot screens while it loads.

Since then: pixel shaders get 64 registers under xsdk's split, so the pack holds all 69 programs, and the title scene's
18-register program draws (xsdk:docs/HARDWARE.md, milestone 24).

### Milestone 11: the title screen drawn

**Why.** The title scene, stage F_SP102 (dusklight:src/d/d_com_inf_game.cpp:1221), is the first scene the player
sees, and what it draws today is only the configurations the pack holds. Read on 2026-10-12, from the run with all 69
programs (xsdk:results/2026-10-12-milestone-24/dusklight.log):
- **The configurations the generator refuses,** each logged once with its first reason (Aurora
  lib/vulkan4xenon/glsl.cpp, `make_plan`): fog 42, matrices picked per vertex 15, lighting 4, indirect texturing 3.
  A configuration needing several is counted once, so how many need each isn't known.
- **What each needs:**
  - **Lighting:** the WGSL loops over a channel's lights, testing its mask's bits (aurora:lib/gx/shader.cpp). Unrolled
    over the lights the config's mask names, the lights are read at constant indices, with `normalize`, `dot`, `max`
    and division, which xost compiles (xost:docs/xsc.md). Its vertices also carry normals.
  - **Fog:** the WGSL reads the fragment's depth (aurora:lib/gx/shader.cpp); clip z and w can reach the fragment
    program in an interpolant instead, with `exp2` for the exponential kinds. Range fog adds a factor by the pixel's
    column: the fragment's position, xost's generated parameter (xenia:src/xenia/gpu/registers.h:140, 234),
    and a constant array indexed at run time. The game turns range fog on per environment
    (dusklight:src/d/d_kankyo.cpp:9459).
  - **Matrices picked per vertex:** the position, normal and texture matrices come from arrays indexed by a vertex
    attribute (aurora:lib/gx/shader.cpp). Xenos reads constants at `c[a0 + n]`, `a0` set by `MOVA`
    (xenia:src/xenia/gpu/ucode.h:196-202, 1140-1148); xost reads arrays only by a loop counter.
  - **Indirect texturing:** the WGSL masks texel values with an integer `&` (aurora:lib/gx/shader.cpp); the masks are a
    power of two less one, so `mod` of the floor does it without the bit operations Xenos lacks.
- **Memory:** each pipeline's two shaders take two 64 KiB pages at least, as xsdk rounds every GPU allocation up to a
  page (xsdk:src/gpu.c, `allocate`; xsdk:TODO.md, "Small GPU allocations"). The refused configurations' pipelines would
  take about 8 MB more, and the title scene ends with 9 to 11 MB free.
- **Copy targets:** the scene copies into a 20x11 target, which the layer refuses as a colour attachment 16 texels or
  less on a side (src/image.c:54-56), the size at which xsdk packs a texture.
- **Byte order,** left by milestone 9 until drawing shows it: Aurora's texture conversion swaps, the vertex and matrix
  arrays Dusklight flags little-endian in 58 places, and the memory card and controller binding files.
- **From encounter's upstream** (aurora:`3227d76..c8406f2`, read 2026-10-12): 23 commits, none of them a speed-up,
  and Dusklight's upstream still pins `3227d76`. Five apply onto the fork without conflict and fix something:
  `3094e04`, packed TEV alpha compares taking the colour operands from before the stage; `afa388d`, `SET_REG_FIELD`
  masking the value to its field; `c8406f2`, the DVD keeping its handle out of `userData`; `0857ad8`, texture
  coordinates as a texgen source, a fatal error before; and `77997d4`, `CARDProbeEx` with null outputs. `b922153`
  bounds the texture content, copy and offscreen caches with an LRU, and conflicts in 9 files with the fork's
  backend-neutral types. The rest are left: `d664382`'s input rework drops `aurora/lib/input.hpp`, which Dusklight
  includes (dusklight:src/dusk/action_bindings.cpp:6), and the others are churn, or already in the fork.
- **The fork's tuning,** read the same day:
  - a shader config's key holds its channels' light masks (aurora-xenon:lib/gx/gx.hpp:185), so each mask makes a
    pipeline of its own, and a draw is skipped until its pipeline is built (aurora-xenon:lib/vulkan4xenon/encoding.cpp,
    `usable_pipeline`);
  - pipelines compile on as many threads as cores less two, 4 on xsdk
    (aurora-xenon:lib/gfx/pipeline_cache.cpp:1149-1155), but the backend holds one lock across the pack lookup and
    the pipeline's creation (aurora-xenon:lib/vulkan4xenon/pipeline.cpp:503);
  - quads, strips and fans take their indices from prebuilt patterns only when `AURORA_INDEX_PATTERN_VERTICES` is
    set (aurora-xenon:CMakeLists.txt), and Dusklight doesn't set it.
- **Left for milestone 12,** depth out of eDRAM: the Z16 depth copy and `GXPeekZ`.

**Agreed with the user on 2026-10-12,** with Dusklight's PC build as the reference, and the audit's items added the same
day. **In six steps,** each run on the console:
1. **What the scene needs, measured.** A recording build logs, once each, every refused configuration with all it
   needs, not only the first, and each copy target's size and format; a run in Xenia counts them, says whether the
   scene turns range fog on, and counts the configurations that differ only in their light masks. The counts decide
   whether range fog and small copy targets are in this milestone.
2. **Aurora from upstream, and the 360's settings:** `3094e04`, `afa388d`, `c8406f2`, `0857ad8` and `77997d4`
   picked, with `3094e04`'s packed alpha compares mirrored in the fork's GLSL generator, which today compares the
   alpha operands (aurora-xenon:lib/vulkan4xenon/glsl.cpp, `alpha_op`): checked by a config made to hit it, its GLSL
   reading the colour operands as the WGSL does and compiling with glslang and xost. Dusklight takes Aurora's index
   patterns, `AURORA_INDEX_PATTERN_VERTICES` 4096, under the patterns' 16-bit indices, and 2 pipeline compile
   threads. The title scene runs as at milestone 24, its frame rate and free memory compared.
3. **xost: uniform arrays indexed at run time,** in vertex shaders (`m[int(i)]` lowered to `MOVA` and `c[a0 + n]`),
   and, if step 1 finds range fog, in pixel shaders with the fragment's position. Checked through xost's oracle, and
   on the console by vulkan4xenon_test: a vertex shader that picks each quad's matrix by a vertex attribute, broken on
   purpose with the index ignored.
4. **Memory:** shaders' microcode shares pages, xsdk's milestone 25 (xsdk:SCOPE.md): 256-byte blocks of shared
   64 KiB pages, with an xsdk_test check that 200 small shaders fit a few pages where they took 200 and draw right,
   broken on purpose by overlapping two. Aurora's texture content, copy and offscreen caches take upstream's LRU
   bounds (`b922153`), ported by hand to the fork's backend-neutral types, the fork's byte cap on converted textures
   their budget, checked by Aurora's host tests. The title scene runs on the console, its free memory compared.

**Step 4 scoped with the user on 2026-10-12,** from a count in Xenia (xsdk:results/2026-10-12-gpu-allocations/):
Dusklight's boot screens and a minute of its title scene made 87 GPU allocations, 73 under a page and 70 of those 24
to 1,440 bytes, shaders' microcode, about 85 KB in 73 pages, 4.6 MB. Only microcode shares pages, decided with the
user, as the other small allocations would save a few. Microcode had a page each, not shared pages as step 4 said
first. `b922153` conflicts in 9 files, 18 places; it frees nothing in the title scene, whose 8 copy sizes and 2
offscreen targets are each used every frame, but bounds the caches for gameplay and keeps a static copy texture that
upstream cleared on a resize.
5. **The generator and the pack:** fog, lighting, matrices picked per vertex and indirect texturing, and range fog if
   step 1 finds it, as gx/shader.cpp's WGSL writes them, in the GLSL xost compiles. Lighting's light masks are
   either kept in the key, each mask's lights unrolled, or left out of it by a backend limit, all eight lights
   unrolled and each one switched by a constant: decided with the user from step 1's counts. The scene is recorded
   again in Xenia, since every program's name is the hash of its text, and the pack rebuilt.
6. **What drawing shows:** the user captures the title screen on the console, and the same moment from Dusklight's
   PC build, the reference agreed with the user, at the settings the console runs: a 1280x720 window, Dusk bloom and
   depth of field, the internal resolution automatic, 1x shadows and no frame interpolation. The byte order above is
   fixed where the captures show it wrong, and the copy targets step 1 finds are drawn, if the layer must take small
   colour attachments.

**Changed with the user on 2026-10-12,** after step 1: the captures moved from step 1 to step 6, where they're first
compared. Step 1's counts settled all it decides, and a capture of what the console draws before step 5 would show
nothing step 6 can't.

**Done when,** on the console:
- the title scene runs its minute and exits with status 0, refusing no configuration for fog, lighting, matrices picked
  per vertex or indirect texturing, and missing no program from the pack; the depth copies and `GXPeekZ` are the only
  unsupported features it reports;
- the title screen, captured, matches the reference capture of the same moment, as the user judges it, both filed in
  `results/`;
- it ends with no less free memory than milestone 10's 7 MB;
- the fork carries upstream's `3094e04`, `afa388d`, `c8406f2`, `0857ad8` and `77997d4` and `b922153`'s bounded
  caches, and Dusklight runs with index patterns and 2 pipeline compile threads;
- vulkan4xenon_test, with its matrix-per-vertex check, vkbench and xsdk_test pass in full, vkbench within the noise of
  its last run.

**Step 1 passed on hardware** on 2026-10-12, Dusklight `8f99ef3` with Aurora `265dcda`
(`results/2026-10-12-milestone-11-step-1/`; Dusklight's log names `1961c31`, the commit at its last configure). The
generator logs each configuration it refuses once, under its hash, with all it needs; a recording build logs each
target it creates; `tools/count_refusals.py` counts both (`counts-xenia.txt`, from the recording build in Xenia):
- **62 configurations refused,** the same 62 needing the same in the recording build and the drawing build in Xenia
  (`dusklight-record-xenia.log`, `dusklight-xenia.log`) and the drawing build on the console (`dusklight.log`):
  - **fog 50, lighting 46, matrices picked per vertex 15, indirect texturing 5,** and nothing else;
  - **most need more than one:** fog and lighting 31, those and matrices picked per vertex 10, fog alone 7, matrices
    alone 5, lighting alone 4, indirect texturing alone 3, and indirect texturing with fog 2, one of them lit too;
  - **by the first each needs** they are 40, 15, 4 and 3. Milestone 24's 64 lines, 42 of them fog, were one for each
    pipeline asking, so two configurations were logged twice.
- **Range fog is on:** 43 of the 50 fog configurations adjust it by range. All are perspective: 47 linear and 3 reverse
  exponential, 2 of those squared. Range fog is in this milestone.
- **Lighting:** every lit channel is COLOR0, with clamped diffuse and spot attenuation, its ambient from the register
  in all but one. 30 sum all eight lights, 12 seven, 2 six and 2 one. No two configurations differ only in their light
  masks: the 46 stay 46 with every mask cleared, though each one's hash changes. Keeping the masks in the key costs no
  pipeline here, and leaving them out saves none.
- **Matrices picked per vertex:** by the position matrix's index alone in 7, and by it and texture matrix 1's in 8.
- **Indirect texturing:** one indirect stage in each of the 5.
- **Targets:** copies at 1280x720 and each half of it down to 20x11, and one at 192x192, all RGBA8 (GX format 6); and
  offscreen targets with depth at 384x384 and 960x360. Only the 20x11 copy is 16 texels or less on a side, so small
  copy targets are in this milestone too, at step 6.
- **The programs the generator writes didn't change:** the recording wrote the pack's 69, byte for byte.
- **The console run** went as at milestone 24: the title scene at 29.1 to 30.0 fps once loaded, 8 MB free at its end
  and status 0, with milestone 24's other errors.

**Step 2 passed on hardware** on 2026-10-12, Dusklight `3d1853a` with Aurora `da395ea`
(`results/2026-10-12-milestone-11-step-2/`; Dusklight's log names `8f99ef3`, the commit at its last configure):
- **Upstream's five fixes** applied to the fork without a conflict, each keeping its author and naming the commit it
  came from: `77997d4` as `d6b542b`, `3094e04` as `f8bb045`, `0857ad8` as `95613f0`, `afa388d` as `2676de6` and
  `c8406f2` as `d2617ff`. Aurora's DVD tests passed all 39 on the host (`dvd_tests.txt`), among them `c8406f2`'s new
  one and the 12 that read a disc, through the fork's ISO reader from the GameCube disc Dusklight reads.
- **The packed alpha compares in the GLSL** (`da395ea`): an alpha op's R8, GR16 and BGR24 compares take their keys from
  the colour operands a and b, as the WGSL now does, while A8 still compares the alpha operands. Aurora's new
  `vulkan4xenon_glsl_tests` checks each on a config made to hit it, and passed all 7 (`vulkan4xenon_glsl_tests.txt`).
  Broken on purpose, comparing the alpha operands again, it failed the 6 packed compares and passed A8. glslang and xost
  compiled all 42 programs its configs and the backend's own make. None of the title scene's programs compares in its
  alpha op: the recording build in Xenia wrote the pack's 69 as before, byte for byte, and refused step 1's 62
  (`dusklight-record-xenia.log`, `counts-xenia.txt`).
- **Dusklight's settings:** pipelines compile on 2 threads (`Pipeline compilation: 2 at a time`, from 4), and draws of
  up to 4096 vertices take their indices from Aurora's patterns. A frame's indices peaked at 222 KB, from 223:
  patterns serve only draws not merged into the one before, and a merged draw's indices still go into the frame's pool
  (aurora-xenon:lib/gx/command_processor.cpp, `draw_prim`).
- **The console run** (`dusklight.log`) went as at milestone 24: the title scene at 29.2 to 30.0 fps once loaded,
  against 29.4 to 30.0, and 9 MB free at its end, as then; status 0, with milestone 24's other errors and step 1's 62
  refused. The 10 seconds it loads in ran at 14.8 fps, against 19.2 at milestone 24 and 15.9 at step 1, both on 4
  threads: the count's 10-second windows take the boot screens' end and the loading in other parts each run.

**Step 3 passed on hardware** on 2026-10-12, this repository at `a39ad55` with xost at `7a5e42e`
(`results/2026-10-12-milestone-11-step-3/`; xsdk:docs/HARDWARE.md, "Reads at a0, and the pixel position"):
- **xost reads a block's array at an index known only at run time** (`4d7c297`): `MOVA` sets `a0` from the index, the
  element at `c[a0 + n]` is copied into a temp, and a constant added to the index goes in `n`, so a matrix's rows share
  one `MOVA`. A pixel shader's `gl_FragCoord.xy` comes from the pixel parameters, the register past the
  interpolators, plus 0.5; its `zw` are refused, and before, it lowered silently as the first interpolator. xost's
  `tests/test_dynamic_index.py` runs each shader through a simulator, `MOVA` and `a0` included, and the oracle. Every
  other test shader lowers as before, byte for byte, and so do Dusklight's 69 programs and this repository's other
  shaders.
- **vulkan4xenon_test passed 72 of 72** (`vulkan4xenon_test.log`), with two checks of its own (`test_run_time_index.c`):
  four quads each placed and coloured by the matrix their vertices name, three rows a matrix, and, beside the scope,
  the screen coloured in 128-pixel bands picked by `gl_FragCoord.x`, probed either side of band edges, as range fog
  needs both and only the console shows the pixel parameters. Broken on purpose, both shaders ignoring their index,
  the quads stacked in one corner and the bands took one colour, failing those 4 checks alone
  (`vulkan4xenon_test_broken.log`).
- **The first run failed the bands** (`vulkan4xenon_test-first.log`): every column read band 0. A diagnostic title
  (`vulkan4xenon_diag.log`, its sources in `diagnostic/`) showed a pixel shader's reads at `a0` work, but the pixel
  parameters' x reads 0 without `SQ_CONTEXT_MISC.SC_OUTPUT_SCREEN_XY`, which xost now sets with them (`7a5e42e`), and
  that the execs must keep addressing mode 0 for reads at `a0`.

**Step 4's first part, xsdk's milestone 25, passed on hardware** on 2026-10-12, xsdk at `4bcc93f`
(`results/2026-10-12-milestone-25/`; xsdk:docs/HARDWARE.md, "Shaders' microcode in shared pages"): shaders'
microcode takes 256-byte blocks of shared pages, where each shader took a page. vulkan4xenon_test passed 72 of 72 and
vkbench 14 of 14, with milestone 24's command stream and its times within noise. Dusklight's title scene ended with
13 MB free, from 9 at step 2, the title holding 69 MB of physical pages from 73. Aurora's LRU caches are next.

**Step 4 passed on hardware** on 2026-10-12, Dusklight `6b3d713` with Aurora `64066c4`
(`results/2026-10-12-milestone-11-step-4/`; Dusklight's log names `3d1853a`, the commit at its last configure):
- **Upstream's LRU caches** (`b922153`), ported by hand to the fork's backend-neutral types: the converted-texture,
  copy and offscreen caches share Aurora's `LruCache`. Converted textures keep the fork's byte cap, 16 MiB in
  Dusklight; copies and offscreen targets take upstream's 64 MiB, more than the console has free, so what bounds them
  there is upstream's other limit: a copy size or offscreen target unused for 32 frames goes. Offscreen targets are
  keyed by their formats too, as upstream's are, and the caches are no longer cleared on a window resize.
- **Aurora's host tests,** on Dawn, which the cache tests need: lru_cache_tests 7 of 7, gx_texture_cache_tests 38 of
  38 (upstream's 14 new), gfx_recording_tests 18 of 18 (upstream's 6 new, one varying the colour format alone, as the
  fork's formats name one depth format); on the null backend, dvd_tests 39 of 39 and lru_cache_tests 7 of 7. Broken on
  purpose three ways (`breaks.txt`), an LRU that doesn't reorder on use, a destination's superseded copy left outside
  the budget and an offscreen key without its formats, they failed 5, 12 and 1 tests.
- **gx_fifo_tests' two byte goldens** had failed since step 2's `2676de6`, upstream's `afa388d`, which masks a value to
  its register field; step 2 ran only the DVD and GLSL tests. Aurora `dbc0cf8` records them again from the per-value
  writes they came from, `bfaa5f3`, with the masking applied: built as it was, it gives the old goldens, all five
  (`goldens-e3908bb.txt`). gx_fifo_tests passes 236 of 236, and with the masking undone fails those two alone.
- **The console run** (`dusklight.log`): the title scene at 29.3 to 30.0 fps once loaded and 14 MB free at its end,
  against 13 at milestone 25, the title holding 69 MB of physical pages as then; status 0, with milestone 25's errors.
  It refused 61 configurations: the 62nd, `4b9dad0c2751e69f`, is drawn near the minute's end, and this run drew 45
  frames fewer after its slower load (14.5 fps against 19.6). The same `.xex` in Xenia refused all 62 and logged step
  1's Xenia errors (`dusklight-xenia.log`). The log came back whole.

**Step 5 passed on hardware** on 2026-10-13, Dusklight `962860e` with Aurora `470e2de` and xost `a22f915`
(`results/2026-10-13-milestone-11-step-5/`):
- **The generator** (Aurora `46e7a2b`, with the view's depth range from `f58b2da`) writes fog with its range
  adjustment, lighting with each mask's lights unrolled, the masks kept in the key as decided with the user, matrices
  picked per vertex through `a0`, and indirect texturing, as gx/shader.cpp's WGSL writes them.
  vulkan4xenon_glsl_tests passed 33 of 33. tools/check_glsl_math.py runs 14 named configurations' programs, compiled
  as the pack is, on xost's simulator against its own reference of the WGSL: all 14 agreed (`check_glsl_math.txt`).
  Broken on purpose seven ways, each break failed the cases it touches (`glsl_breaks.txt`).
- **xost** (`3d46e0c`, `a22f915`): temporaries run past r63 until the allocator packs them, vertex programs are
  allocated past 64 registers, dependent texture reads gather their coordinates first, and a vertex program's
  branches are refused, where they had been miscompiled without a word. Its 15 test files pass, 164 checks
  (`xost-tests.txt`). Of 536 lowerings, 470 came out as before, none that compiled changed, 24 more compile, and the
  18 now refused are the earlier generator's vertex programs with a `?:`, which it no longer writes
  (`xost-compare.txt`).
- **The scene recorded again** in Xenia refused no configuration: the pack's 69 programs came out byte for byte as
  before, and 71 more, 140 in all.
- **The first console run** (`dusklight-first.log`) drew every configuration and exited with status 0, but ran the
  title scene at 0.3 fps. Xenia shows none of the layer's frames, so Aurora `470e2de` has the backend write frame
  captures, the EFB untiled into a PPM, and Dusklight writes the title scene's 20 seconds in. In Xenia
  (`xenia-before-fixes.png`) the scene's models drew as flat triangles through the screen's centre, as they had on
  the console all along: the user had only ever seen the boot screens draw right. Two of the PC port's byte-order
  assumptions, step 6's to fix, were brought forward:
  - **Arrays the host writes,** which games mark `le`: J3D's matrices
    (dusklight:libs/JSystem/include/JSystem/J3DGraphBase/J3DSys.h:154) and its skinned vertices
    (dusklight:libs/JSystem/src/J3DGraphBase/J3DShape.cpp:158), read swapped on the big-endian console. The command
    processor reads them in the host's order now (Aurora `08900a8`; `xenia-geometry-fixed.png`).
  - **Textures' big-endian words,** RGB565, RGB5A3, C14X2 and CMPR, swapped unconditionally, which drew the models'
    textures as noise (Aurora `e34bef3`; `xenia-title.png`, `xenia-castle-gate.png`).

  With both fixed the console ran at 17 to 22 fps, so the 0.3 fps was the broken geometry: triangles across much of
  the screen, drawn with step 5's programs.
- **The console run** (`dusklight.log`): the title scene drew correctly, as the user saw on the screen; status 0,
  nothing refused, step 4's errors only. It ran at 16.9 to 22.1 fps once loaded, against step 4's 29.3 to 30.0 with
  61 configurations undrawn, and ended with 10 MB free, from 14, with more textures drawn. Xenia ran the same `.xex`
  at 30.0 fps (`dusklight-xenia.log`). Aurora's host tests passed on Dawn, gx_fifo 236, gx_texture_cache 38,
  gfx_recording 18 and lru_cache 7, and on the null backend dvd 39, vulkan4xenon_glsl 33 and lru_cache 7; the
  byte-order fixes change nothing on a little-endian host.
- **Where the frame goes,** from a diagnostics build (`dusklight_diag.log`; its sources in `diagnostic/`, against
  Aurora `470e2de` and Dusklight `32e17a7`). Every 10 seconds it logged each worker's time and the GPU's, the latter
  from submit to fence and so with the layer's replay of the command buffer, and drew the next period without one
  feature's draws. In the title's last, still shot, at 18.2 to 18.9 fps, a frame took the GPU 32 to 36 ms, the
  render worker 32 to 34 ms, 5 of them waiting for the display, and the FIFO worker 10 ms. Without the lit draws,
  nearly the whole 3D scene, the title scene ran at 28.5 fps. Without the indirect draws, 16 to 19 a frame with 1,000
  to 1,300 vertices in the periods before, it ran at 28.1, the GPU's time down to 30 ms from 48. With every feature's
  draws gone, 25 draws left, it ran at 28.0 fps, the GPU still at 20 ms a frame. The periods are different moments of
  a moving scene, so they compare neighbours, not the same frames. Reaching 30 fps is open.

**After step 5, xsdk's milestone 26** (2026-10-13; `results/2026-10-13-milestone-26/`; xsdk:docs/HARDWARE.md, "Threads
on all six hardware threads"): every thread had run on hardware thread 0, as the kernel keeps a thread on its
creator's, and now each of Aurora's and the game's threads runs on the one its list gives. vulkan4xenon_test passed 72
of 72 and vkbench 14 of 14, its frame times as at milestone 25. The title scene ran at 16.2 to 27.6 fps, from 16.9 to
22.1: most frames still take more than the 33 ms of two vblanks.

**Then the GPU's pixel shading, and xost's optimiser** (2026-10-13; `results/2026-10-13-shader-optimizer/`, the
diagnostics' sources in `diagnostic/`). A diagnostics build timed the replay's parts and the GPU in the title's still
logo shot, and drew phases of it differently:
- **The replay's 19 ms was a wait.** Per frame, the 246 draws' pipelines, uniforms, push constants, textures, streams
  and packets took 0.6 ms together; every uniform block loaded on the GPU. The rest was in `end_render_pass`, in one
  wait for the GPU: a frame resolves 9 sizes, and xsdk keeps a resolve's rectangle for 8 (xsdk:src/gpu.c,
  `rectangle_slot`), so reusing one waits for the GPU to finish the resolve that last used it
  (`dusklight_diag_rectangles.log`). With 32 slots the wait went, the render worker dropping from 22 to 4.5 ms a frame,
  and the frame rate didn't move (`dusklight_diag_32_rectangles.log`): the GPU is what holds it. xsdk keeps 8 for now.
- **The GPU's time is pixel shading.** The logo shot took 25.9 ms of it a frame. Scissored to one pixel, every draw's
  vertex work and the resolves still done, it took 7.9 ms; scissored to 3/4 of each side, 15.0 ms, and to 1/2, 11.0
  (`dusklight_diag_scissor.log`, `dusklight_diag_before.log`). Lowering the resolution was ruled out with the user:
  the work goes into the shaders.
- **xost's programs carried waste.** The heaviest pixel program's 156 ALU instructions held 36 moves, 19 reciprocals
  mostly of uniforms, scalar ops in instructions of their own, and multiplies and adds unfused (`pack_stats.txt`).
  xost's optimiser (xost `fed0752`, xasm/optimize.py) rewrites a program's instructions before allocation: 3,232 of the
  pack's pixel ALU instructions became 2,184 and 5,612 vertex ones 4,883; all 137 programs it can run matched before
  and after, as instructions and as microcode, and the 3 with relative addressing are left as they were
  (`check_optimize.txt`, `test_optimize.txt`). Some instructions now name two constants, 224 in the pack, which the
  console takes. tools/build_shaders.py (`0e7889e`) and Aurora's tools/build_glsl_pack.py (Aurora `89b5c5f`) take
  `--optimize`, and Dusklight's pack is built with it (Dusklight `c6ea25a`).
- **On the console** the optimised pack drew the logo shot as before, but for the logo's moving shine, which differs
  between two unoptimised runs too (`shot_before.png`, `shot_after.png`, `shot_diff.png`,
  `shot_diff_two_unoptimised_runs.png`), and took 22.0 ms of GPU time a frame, from 25.9 (`dusklight_diag_after.log`).
- **Five programs had been missing** since at least milestone 26, their draws skipped: the title's attract demo uses
  them, about 230 s into the title scene, which the recording never reached. A recording build run that long on the
  console recorded all five (`dusklight_record.log`, the log read as the title wrote it), and the pack holds 145.
  That run then **ran out of memory** as the demo's movie stage loaded, `sbrk` refused with 0xC0000017 and 51 MB free,
  and crashed; the drawing build has less free there, so the title screen left alone will crash when its demo starts.
  That's open, apart from the shaders.
- **The drawing build** (`dusklight.log`, `title.png`; xsdk, the layer and Dusklight cleaned and built in full): the
  pack's 145 programs, none missing, status 0, 8 MB free at the end. The title scene ran at 26.3, 28.7, 27.7, 30.0
  and 29.7 fps over its 10-second windows, from milestone 26's 16.2 to 27.6.

**Then range fog read from a table** (2026-10-13; `results/2026-10-13-range-fog-table/`), the next step of the shader
work agreed with the user. Range fog's factor depends only on the pixel's column, and the programs worked it out per
pixel, about 20 ALU instructions and a square root. Drawn without it, the logo shot took 19.5 ms of GPU time a frame,
from 21.9; the texture sizes' reciprocals, also worked out per pixel, cost nothing measurable, the optimiser pairing
them with vector instructions.
- **32-bit float images:** xsdk's milestone 28 added `XSDK_FORMAT_R32F` (xsdk:docs/HARDWARE.md, "32-bit float
  textures"), and the layer samples `VK_FORMAT_R32_SFLOAT` images as it (`e0462aa`; docs/SUPPORT.md, Formats): optimal
  tiling, nearest only, not colour attachments. vulkan4xenon_test's texture check draws one, its red from floats, one
  under 0 and one over 1, and passed 72 of 72 (`vulkan4xenon_test.log`); broken on purpose, the format taken as
  R8G8B8A8, only that check's two failed, its texels the floats' bytes (`vulkan4xenon_test_r32f_as_bytes.log`).
  vkbench passed 14 of 14.
- **The table** (Aurora `3a53fee`): the FIFO worker already builds the factor for each column into the storage pool,
  as the WGSL reads it. The backend copies it into a 64x32 R32_SFLOAT texture, column x at texel (x % 64, x / 64), 8
  KiB where a single row would take 32 rows of tiles, written by the CPU before any frame draws with it, so with no
  copy and no wait for the GPU; finds tables seen before by their factors, keeping 8; and binds it at set 2 for
  pipelines whose program has range fog. The generator samples it nearest, u repeating, at the middle of the column's
  texel. vulkan4xenon_glsl_tests passed 33 of 33, and tools/check_glsl_math.py 14 of 14, its range fog cases now also
  checking that the sample lands on the middle of its texel (`check_glsl_math.txt`); broken on purpose, the table's
  width taken as 32, or the sample off the middle of its row, each failed (`glsl_breaks.txt`).
- **The pack** (Dusklight `d44654aa97`): the title scene recorded again in Xenia gave the 25 range-fog fragment
  programs anew, each the old one with the generator's change and nothing else (`recorded_pairs.txt`); the other 120
  came out byte for byte as before. Optimised, the 25 take 896 ALU instructions, from 1,485, the heaviest 70 from 94
  (`alu_pairs.txt`); xost's optimiser check passed on all 25 (`check_optimize.txt`).
- **Drawn the same:** in Xenia the diagnostics build's logo shot came out pixel for pixel as step 1's, and with the
  table zeroed on purpose 14.7% of it changed, the castle and the far hills (`captures.txt`,
  `xenia_zero_table_diff.png`). On the console it differed from step 1's only in the logo's moving shine, as two of
  step 1's runs do (`shot_diff.png`).
- **On the console** (`dusklight_diag.log`, against `results/2026-10-13-shader-optimizer/dusklight_diag_after.log`):
  the logo shot took 20.6 ms of GPU time a frame, from 21.9, and 19.6 from 21.4 in the phase that skips the few draws
  picking matrices per vertex. The drawing build (`dusklight.log`, `title.png`; everything cleaned and built in full)
  drew the title scene with nothing missing at 29.8, 28.9, 29.9, 29.8 and 30.0 fps once loaded, and exited with status
  0, 8 MB free.

**Then the memory for the attract demo** (2026-10-13; `results/2026-10-13-attract-demo-memory/`), the out of memory
step 1 left open, three fixes agreed with the user:
- **Movie frames kept for nothing** (Aurora `b9ede7a`): Aurora's pre-conversion worker parked its result for content
  the FIFO thread had already converted inline and uploaded, at the draw it reached first. A movie's frames come once
  each, so nothing took them until the ready budget or their 120 s ran out, and the recording build's budget was
  Aurora's 128 MiB. The worker now checks the content wasn't uploaded before it parks. Aurora's Dawn tests passed, the
  texture cache's 39 with a new one that holds the worker in its conversion while the draw uploads the same content
  (`aurora-dawn-tests.txt`); with the check removed only that test failed, 64 bytes parked
  (`aurora-dawn-tests-not-parked-check-removed.txt`).
- **MEM1, the game's root heap, at 144 MiB** (Dusklight `24404604ed`, the Xbox 360 only), from the PC's 256: it parents
  the system, game, archive and smaller heaps at fixed sizes, 140,176 KB in all, and past start-up only a debug build's
  camera editor and previewer take from it, so 119 MiB were never handed out.
- **The recording build takes the drawing build's frame pools, index patterns and texture caps** (the same commit), so
  its memory is the drawing build's.
- **On the console,** with the title scene held 300 s rather than 60 for the run: the drawing build (`dusklight.log`) ran
  the title scene with 120 MB free, from 8, at 28.7 to 29.9 fps, then played the movie to the end with 35 to 44 MB
  free; the root heap peaked at 140,176 KB of 147,439. The recording build (`dusklight_record.log`), which had run out
  of memory as the movie began, played it at 30 fps with 197 MB free, flat. Both exited with status 0.
- **Open: the movie plays at 7.5 fps** in the drawing build, as the user saw on the screen and a second run logged
  (`dusklight_rerun.log`), and the title's physical pages rise from 69 to about 145 MB while it plays. On the null
  backend it plays at 30, so the cost is the backend's: each movie frame is three new textures, made and uploaded, and
  each upload waits for the GPU (docs/SUPPORT.md, Commands).

### Milestone 12: texture uploads at the CPU's speed (done)

**Why.** A diagnostics build timed the attract demo's movie on the console (2026-10-13): a frame takes 133 ms, 114 of
them in the replay's copies of its three textures, 1.6 MB, and 5 more waiting for the GPU before them; the GPU needs 3
after the replay, and the game thread waits 115 ms a frame for a frame slot. The copies read Aurora's upload buffer,
in write-combined memory the CPU reads uncached, and xsdk_texture_upload wrote a texel at a time. The movie's I8
planes are expanded to RGBA8 first, four times the bytes.

**Decided with the user (2026-10-13), all three, with xsdk's milestone 29:**
1. **Copies read cached memory.** A second memory type, `HOST_VISIBLE`, `HOST_COHERENT` and `HOST_CACHED`, the CPU's
   cached memory, which only buffers whose sole usage is `TRANSFER_SRC` take: copies run on the CPU, so the GPU never
   reads it. xsdk_texture_upload writes runs of whole words (xsdk's milestone 29).
2. **8-bit textures stay 8-bit.** `VK_FORMAT_R8_UNORM` and `VK_FORMAT_R8G8_UNORM` images, sampled, filtered linearly
   and copied into, xsdk's R8 and R8G8; image views take a component mapping, which the fetch constant's swizzle
   applies (xsdk_texture_swizzle). Aurora uploads I8 as R8, sampled as red in every channel.
3. **A copy waits only for what used its image.** Each image keeps the submission that last drew with it, sampled it,
   resolved into it or drew it back; each submission keeps the fence it ended with. A copy into an image no submission
   used doesn't wait, one used by an earlier submission waits for that fence, and one used earlier in its own
   submission waits as before.

**Done when,** on the console:
- vulkan4xenon_test draws R8 and R8G8 images through swizzled views, as intensity and as intensity and alpha, and an
  R8 image filtered linearly between two texels; broken on purpose, a view's mapping ignored, the check fails;
- vulkan4xenon_test copies into an image, draws with it, copies other texels into it and draws again in one command
  buffer, each draw showing its own texels; broken on purpose, a copy never waiting, the check fails;
- vulkan4xenon_test copies from a buffer in the cached memory type, and its objects check finds the type offered to
  transfer-source buffers alone;
- Dusklight's movie plays at its 30 fps, measured by the diagnostics build, and the title scene draws as before;
- xsdk_test, vulkan4xenon_test and vkbench pass.

**On the console** (2026-10-13; `results/2026-10-13-milestone-12/`; xsdk's milestone 29, xsdk:docs/HARDWARE.md,
"Uploads in runs, and channel swizzles"; Aurora `1844958`, Dusklight `9001ae09e6`):
- **vulkan4xenon_test passed 80 of 80** (`vulkan4xenon_test.log`). R8 drew as intensity, and R8G8 through a view of
  green, red, 1 and 0, so that both its channels show on screen, where alpha doesn't; the 2x1 R8 image filtered
  linearly between its two texels; two copies into one image around a draw in one command buffer, each draw showing its
  own texels; the copies came from a staging buffer in the cached memory type, which the objects check found offered to
  transfer-source buffers alone.
- **Broken on purpose,** a view's mapping ignored, the swizzled and filtered checks failed, the filtered image's view
  being swizzled too (`vulkan4xenon_test_mapping_ignored.log`); a copy never waiting, only the copies check failed
  (`vulkan4xenon_test_copies_never_wait.log`). Each break failed the check of the image on screen as well.
- **Found on the way:** the first run's filtered image read neither of its texels
  (`vulkan4xenon_test_2x1_as_wide.log`). xsdk had put a 2x1 texture's base where the console doesn't read it, fixed in
  xsdk (xsdk:docs/HARDWARE.md, "A 2x1 texture's tail").
- **vkbench** passed 14 of 14.
- **The movie** (the diagnostics build, `dusklight_movie.log`, against `dusklight_movie_before.log`): 29.9 to 30.0 fps
  over its four windows, from 7.5. A frame's uploads came to 224 to 266 KB, textures of 133 KB, from three of 532 KB,
  the movie's I8 planes now 8-bit; the copies took 2.4 to 3.1 ms a frame, from 114, with no wait for the GPU, from
  5.3 ms; the game thread no longer waited for a frame slot, from 115 ms. Drawn right (`movie.png`;
  `movie_before.png`, a different moment).
- **The title scene** (the drawing build, `dusklight.log`, `title.png`; xsdk, the layer and Dusklight cleaned and built
  in full): 29.1 to 30.0 fps once loaded, with 118 MB free, and status 0 at the exit.
- **The logo shot** (the diagnostics build, `dusklight_diag.log`, `shot.png`): drawn as in the range fog step's run but
  for the logo's moving shine, 0.05% of its pixels (`shot_diff.png`), in 20.6 ms of GPU time a frame, as then.

### Milestone 13: compiling shaders on the console (done)

**Agreed with the user (2026-10-14):** programs a pack doesn't hold are compiled on the console the first time the
game asks for them, and kept on the hard disk, so no one has to play the game drawn badly to fill a pack
(xsdk:docs/scope/shader-compiling.md). glslang on the console and xost's compiler in C are done; this milestone is
that scope's step 3, in five parts: xsdk's `rename` (done), the layer compiling SPIR-V its bundle doesn't hold
(done), the layer keeping what it compiles in a folder the app names (done), Aurora's backend compiling GLSL its pack
doesn't hold through glslang (done), and Dusklight built without its pack for the run that starts with nothing (done).

**On the console:**
- **Part 2** (`results/2026-10-14-milestone-13-part-2/`): vulkan4xenon_test draws the triangle, the per-vertex matrices
  and the bands again from SPIR-V the bundle can't match, its generator word changed. 89 passed, 0 failed; the four
  shaders compiled in 0.08 to 0.34 ms, and the triangle's image is the bundle's to the pixel.
- **Part 3** (`results/2026-10-14-milestone-13-part-3/`): the test names a folder for the layer's cache, emptied
  first. The four shaders compiled are kept in four files, and a second device, made after the first goes, reads the
  triangle's two back, compiles none and draws the bundle's image. 94 passed, 0 failed. With the renames left out on
  purpose, 92 passed and 2 failed: no file kept and the second device compiled both; with the reads left out, 93 passed
  and 1 failed.
- **Parts 4 and 5** (xsdk:docs/HARDWARE.md, "Shaders compiled as the game asks", its logs in
  xsdk:results/2026-10-14-shader-compiling-step3/; Aurora `953d207`, Dusklight `828f1f561d`): Aurora's backend reads a
  program the pack doesn't hold from the disk, else compiles its GLSL through glslang-xenon, and names the layer's
  folder beside its own. Without its pack (`DUSK_XSDK_GLSL_PACK=OFF`), played by the user from nothing kept through the
  title, the opening cutscenes and Ordon, Dusklight compiled 189 programs, each once: through glslang the first in
  919 ms with glslang starting and the rest in 23.9 ms on average, through xost 17.0 ms on average. The second time
  through compiled none and read 159 programs back. The 111 programs the title scene's runs kept are the pack's SPIR-V
  and containers, byte for byte. With the pack nothing was compiled, and the title scene and logo shot ran as before.
  vulkan4xenon_test passed 94 of 94 and vkbench 14 of 14.
- **Found on the way:** Aurora named an atomic write's temporary file in 44 characters for the memory card's, where
  FATX holds 42, so the card was never made on the console; Aurora `2ba57e8` names them in 21, and the second played
  run made it. Ordon's minimap drew as noise, and memory ran out there in the first played run: milestones 14 and 15.

### Milestone 14: the minimap, palette conversions of EFB copies

**Why.** The first play past the title on the console (2026-10-14, step 3's first run without the pack,
xsdk:docs/scope/shader-compiling.md) showed Ordon's minimap as noise, flickering in and out. Twilight Princess draws
its map into the EFB, copies it out as 8-bit indices, and draws the copy through a palette. Aurora turns such a copy
into an RGBA8 texture, `GX Dynamic Palette Texture`, by a pass through the palette (aurora-xenon:lib/gx/texture.cpp,
resolve_dynamic_palette_texture), and the vulkan4xenon backend skips that pass, logging "Palette conversions of EFB
copies aren't supported" once (aurora-xenon:lib/vulkan4xenon/encoding.cpp, encode_pass). The texture is never written,
and the GPU samples whatever its memory held. The game changes the map's palette often, and each change makes Aurora
drop the texture and make another (texture.cpp, get_tlut_texture), on other memory, so the noise changes; once the
console's memory ran out (milestone 15), 204 of them weren't made, and the map wasn't drawn on those frames.

**Agreed with the user (2026-10-14),** after milestone 15's measurement:
1. **The programs.** Aurora's GLSL generator writes the conversion's fragment programs, one for 8-bit indices and one
   for 4-bit, as the WebGPU backend's shaders do it (aurora-xenon:lib/gfx/tex_palette_conv.cpp): the copy's texel,
   sampled nearest, scaled to its index, then the palette's entry at that index. They join the fixed programs a
   recording writes (glsl::fixed_programs), so Dusklight's pack holds them, and the console compiles them otherwise
   (milestone 13). WebGPU's third variant, 16-bit integer indices, comes from no EFB copy (texture.cpp makes only the
   other two) and stays out.
2. **The backend.** A pipeline layout with one set of two samplers, the copy and the palette; a pipeline for each
   variant, drawing the full-screen triangle into the RGBA8 texture as the EFB copies' format conversions draw
   (encoding.cpp, draw_texture); and encode_pass running a pass's conversions before the pass, where the WebGPU
   backend runs them (aurora-xenon:lib/gfx/encoding.cpp, render).
3. **Getting there.** The diagnostics build's warp to Ordon (milestone 15) shows the map without anyone at the pad,
   and captures the frame.

**Found on the way (2026-10-14).** With the conversion in, the map still came out wrong, so the PC's Dawn build ran the
same warp beside the console's, logging each step. The copy's indices, the palette, the TEV registers and the uniforms
the GPU read all matched the PC's. What didn't: the map's areas, the draws with C0 at index 188, are GX lines, and the
GLSL generator refuses lines and points ("no GLSL yet for config ...: lines and points", three configs), so those
draws get no pipeline and are skipped. The map showed only what's drawn as triangles.

**Agreed with the user (2026-10-14), added:**

4. **Lines and points, as the WebGPU backend draws them** (aurora-xenon:lib/gx/shader.cpp:1042-1125): each segment
   and each point a quad in screen space, as wide as GX's line width or point size, in sixths of a pixel, scaled from
   the game's viewport to the one drawn into, its texture coordinates offset across it as `GXSetLineWidth`'s and
   `GXSetPointSize`'s texture offsets say (gx/shader_info.cpp:355-380). The layer draws lines and points 1 pixel wide
   (docs/SUPPORT.md, Pipelines) and xost has no instancing or storage buffers, so the backend's vertex decode writes
   each segment's or point's corners as two triangles, each corner with its end, the other end and which corner it is,
   and the GLSL vertex program moves the corners out from the ends' clip positions. The game draws them for the minimap
   and its paths (d_map.cpp, d_map_path.cpp, with `GX_TO_ONE` offsets), the effect lines (d_drawlist.cpp,
   `dDlst_effectLine_c`), 2D points (`dDlst_2DPoint_c`) and debug packets (m_Do_ext.cpp).

**Agreed with the user (2026-10-14), after lines and points:** the rest of what the game draws that the backend
refuses. Two more errors show in every console run past the title, and both need depth read out of eDRAM, which
neither xsdk nor the layer can do yet (xsdk:TODO.md, "Depth resolves"; docs/SUPPORT.md, "Depth stays in eDRAM"):
- "Depth copies (format 19) aren't supported": the depth of field copies the scene's depth as `GX_TF_Z16`
  (dusklight-xenon:src/m_Do/m_Do_graphic.cpp:1134), so the blur reads a texture never written;
- "GXPeekZ isn't supported": the sun's lens flare and the insects test their visibility against the depth
  (d_kankyo_rain.cpp:357, d_insect.cpp:96), and read nothing.

5. **Depth out of eDRAM.**
   - xsdk resolves depth into a texture, as a resolve with `RB_COPY_CONTROL`'s source 4 does
     (xenia:src/xenia/gpu/registers.h:856; xenos.h:925-933), checked by xsdk_test against depth it drew, and what a
     fetch of the resolved texture returns measured on the console and recorded in docs/HARDWARE.md.
   - The layer resolves a depth attachment into its image when the image is made to be sampled and a pass stores it
     and leaves it to be read, samples it, and leaves its words in the image's memory for the CPU, checked by
     vulkan4xenon_test against known depths.
   - Aurora's backend makes the `GX_TF_Z8` and `GX_TF_Z16` copies as the WebGPU backend's conversions do
     (aurora-xenon:lib/gfx/tex_copy_conv.cpp, FragZ8 and FragZ16), and answers `GXPeekZ` from a snapshot of the
     frame's depth, as gfx/depth_peek.cpp does.

**Agreed with the user (2026-10-14), after depth:**

6. **Anisotropic filtering.** Aurora asks for 16 times by default (aurora-xenon:lib/aurora.cpp:75-76) and the backend
   asks for none, as the layer has none (`maxSamplerAnisotropy` 1, src/sampler.c:46-47), though xsdk's samplers have
   had it since its milestone 33 (xsdk:SCOPE.md). The layer offers the `samplerAnisotropy` feature, up to 16, and
   passes a sampler's `maxAnisotropy` to xsdk's; the backend asks for Aurora's setting. vulkan4xenon_test checks it as
   xsdk_test does, stripes squeezed 8 to 1 keeping their contrast, and Dusklight's frame time in the title scene and
   Ordon is measured with it and without, for the user to settle the default.

**Changed on the way (2026-10-15),** steps 5 and 6 built together, as the user asked:
- **No copy into a buffer.** The layer's copies run on the CPU at replay and wait for the GPU (docs/SUPPORT.md,
  Commands), so one for each snapshot would stall the frame. The snapshot is drawn on the GPU instead, as
  gfx/depth_peek.cpp's compute pass makes it: GX's z of the depth under each texel of the GX framebuffer, packed into
  the red, green and blue of an RGBA8 image. Of two such slots, one is drawn only once the other's frame is done, and
  `GXPeekZ` reads its few words from the newest done, uncached memory a word at a time.
- **The resolve keys on the final layout, not the store op.** Aurora's passes store the EFB's depth for the next pass
  to load; resolving each would add a resolve the size of the colour's to every pass. So a pass resolves a sampled
  depth image only when it leaves it in a layout a shader samples in, and one that keeps it as an attachment leaves
  it in eDRAM alone (docs/SUPPORT.md, Render passes). Aurora's backend lays the EFB's depth out so: the pass before a
  depth copy, and the frame's last when a snapshot is due, leave it to be read, and the rest keep it.
- **A pass with depth of its own took the EFB's.** Milestone 4 placed every depth ending at the top of eDRAM, so
  Dusklight's bloom, a 960x360 pass with its own depth, took the 276 tiles holding the EFB depth's rows from 432 down,
  every frame from the title scene on; the passes after it loaded cleared depth there, and GXPeekZ read it, the lower
  half of the GX framebuffer far (`FFFFFF`) on the console and in Xenia alike. The layer now keeps up to four depths
  in eDRAM, a new one placed under one it holds when both fit above the pass's colour (docs/SUPPORT.md, Render
  passes); vulkan4xenon_test's sampled depth draws a smaller pass with depth of its own between the pass that draws
  the image's depth and the one that loads it.
- **xost lowers no component taken out of a vector into arithmetic,** only into a vector built of them
  (xsdk:src/xsc_lower.c, value_operand), so the depth programs work in vectors, `texture(...).rrrr`, and pick
  components only to build what they write.

**What else the backend refuses, and why it stays out.** Everything the backend refuses was checked against the game's
code and against every model, 2D screen and texture on its disc (a scan of its 2,095 archives: 3,626 J3D models with
13,371 materials, 132 2D screens, 19,493 textures). None of these is used:
- emboss texgens and binormal or tangent sources, and vertex formats with binormals: none on the disc, none in the code;
- more than xost's 10 interpolants: the most texgens a material has is 7, 10 with both colour channels and fog;
- a TEV stage sampling a texture with no coordinates, and logic ops: none in any material. The code's one logic op,
  the shadows' OR (d_drawlist.cpp:1574), is blending on the PC;
- destination alpha: only `JFWDisplay::clearEfb` sets it, and it returns at once on the PC
  (libs/JSystem/src/JFramework/JFWDisplay.cpp:486);
- polygon offset: only `GX2SetPolygonOffset` sets it, and the game never calls it;
- 16-bit palette indices (`GX_TF_C14X2` and the conversion's third variant): no texture on the disc, and no EFB copy;
- a sampler's minimum LOD above 0, its own anisotropy, or a maximum LOD short of the texture's last level: no texture
  on the disc sets one.

Two gaps outside GX were found on the way, each agreed (2026-10-14) as a milestone of its own after milestones 14 and
15: Dusklight's menus, milestone 16, and multisampling, milestone 17.

**Done when,** on the console:
- Ordon's minimap draws as on the PC's Dawn build, steady, and the log has no "Palette conversions" or "lines and
  points" line; broken on purpose, the index read one entry off, the map's colours come out wrong, and with the line
  width left out, its lines vanish, their quads having no area;
- with step 5: the log has no "Depth copies" or "GXPeekZ" line, Ordon's depth of field matches the PC's at the same
  warp, and the peeks the diagnostics build logs match the PC's; broken on purpose, the copy reading colour, the blur
  comes out wrong. vulkan4xenon_test finds each texel's depth above its stencil in the memory of a sampled depth image
  a pass left to be read, to its last texel, samples its depth in red, and finds the memory left alone by a pass that
  keeps it as an attachment; broken on purpose, resolving on the store op alone, that last check fails;
- with step 6: vulkan4xenon_test's stripes keep their contrast with anisotropy, and grey without it, and the frame
  times with and without are reported;
- vulkan4xenon_glsl_tests compile the new programs on the PC through glslang and xost, as the others;
- Dusklight's title scene and logo shot run as before, and vulkan4xenon_test, vkbench and xsdk_test pass.

**Left out:** palettes that don't come from EFB copies, which Aurora converts on the CPU; the 16-bit index variant;
the menus and multisampling, milestones 16 and 17.

**Steps 1 to 4 on the console** (2026-10-15; `results/2026-10-15-milestone-14/`; Aurora `a41820d`, Dusklight
`e5632c4f1a`, xsdk `2f8351d` and `0e8af8f`). Ordon's minimap draws as on the PC's Dawn build, the same warp from the
title scene (`minimap_pc_console.png`; `ordon_console.png`, `ordon_pc.png`), steady over five captures 20 s apart
(`minimap_5_captures.png`), and the log has no "Palette conversions" or "lines and points" line, only step 5's two
(`dusklight_ordon.log`). Broken on purpose (`breaks.png`): with the line width left out, the map's outlines are gone;
with each index read one entry off, Link's marker turns cyan, where the palette's entries, four alike in a row,
change. Three things were found on the way:
- **The game's map draws its areas' outlines as GX lines,** which the generator refused. Found by building Dusklight
  for Windows on Dawn and logging the map's draws there and on the console side by side: the copy's indices, the
  palette and the uniforms matched, and the draws at index 188 had no pipeline on the console. Hence step 4.
- **The corners' first decode froze the console:** they were written into the vertex cache, write-combined memory,
  through `memcpy`s of 8 and 12 bytes, which GCC makes 8-byte stores, and one at an odd word raised an alignment
  exception there. Xenia ran it. They're written in cached memory and copied over a word at a time now, as the decode
  writes (Aurora's lib/vulkan4xenon/decode.cpp).
- **The conversion came out as noise on the console and right in Xenia,** from the same containers: xost never set
  the serialize bit, and the ALU read the fetched index in its fetch's exec before the fetch had written it. xost now
  serializes what an exec reads across the ALU and its fetches (xsdk:docs/HARDWARE.md, "Serialized fetches"). It also
  lowered a vec2's `dot()` as a 4-wide one, which tools/check_glsl_math.py caught in the lines' corners.

On the PC, vulkan4xenon_glsl_tests passed 39 of 39 (`vulkan4xenon_glsl_tests.txt`), the corners' order broken on
purpose failing one (`vulkan4xenon_glsl_tests_corner_break.txt`), and their programs, 109, compiled through glslang and
xost; tools/check_glsl_math.py passed its 17 cases (`check_glsl_math.txt`), three new for lines and points, which fail
with the corners' sides swapped, the width unscaled or the mask ignored (`check_glsl_math_breaks.txt`). On the console
the title scene ran at 28 to 30 fps and the logo shot took 17.6 ms of GPU time a frame as before, its end frame the
same byte for byte; xsdk_test passed 300 of 300, vulkan4xenon_test 100 of 100 (with `test_thin_textures`, four strips
16 to 256 by 1, written while the noise was chased) and vkbench 14 of 14 (xsdk:results/2026-10-15-serialized-fetches/).
`disc_scan.txt` is the disc's scan behind "What else the backend refuses". Steps 5 and 6 are next.

**Steps 5 and 6 on the console** (2026-10-15; `results/2026-10-15-milestone-14-steps-5-6/`). The log has no "Depth
copies" or "GXPeekZ" line any more (`dusklight_aniso16.log`).
- **vulkan4xenon_test** passed 112 of 112 (`vulkan4xenon_test.log`). A sampled depth image's memory held each point's
  depth above its stencil word for word, to its last texel, with a smaller pass's depth drawn between the pass that
  drew it and the one that loaded it; its fetch returned the depth in red; and a pass keeping it as an attachment left
  its memory alone. The stripes kept a contrast of 1000 per mille with anisotropy 16 and greyed to 0 without. Broken on
  purpose in one build (`vulkan4xenon_test_breaks.log`), each break failed its own checks: every depth placed at the
  top again, the smaller pass's words (`0CCCCD5A`) in the image's last rows; the resolve keyed on the store op, the
  kept pass's memory overwritten; anisotropy left out of xsdk's sampler, the stripes greyed.
- **The depth of field** draws only while the player targets or an event focuses (dusklight-xenon:
  src/m_Do/m_Do_graphic.cpp, drawDepth2), so the diagnostics build forced it on at -200, within targeting's range, on
  the console and the PC alike (`diagnostic/`). The far houses blur and Link and the ground before him stay sharp on
  both (`depth_of_field_console_pc.png`): the top fifth's mean step in green fell from 5.17 to 3.21 on the console and
  6.52 to 3.55 on the PC, and the lower fifths didn't change (`diagnostic/sharpness.py`). Broken on purpose, the copy
  reading the pass's colour, the houses stayed sharp, 5.17 (`depth_of_field_break.png`).
- **GXPeekZ's** answers at a 5 by 4 grid over the GX framebuffer, 20, 40 and 60 s into Ordon, matched the PC's to
  within the frames' different framing, 1280x720 on the console and the PC's 1216x896 window: the grid's bottom row
  `FEEFB3 FEF642 FEFD14 FF036C FF09FB` on the console, `FEF338 FEF7DE FEFCAC FF0128 FF05CE` on the PC (`peeks.txt`).
  Before the placement fix (Changed on the way) the grid's lower half read `FFFFFF` on the console and in Xenia alike.
- **Anisotropy** changed nothing in Dusklight's frame times (`anisotropy_frame_times.txt`): the GPU took 18.18 to
  19.01 ms a frame in Ordon with Aurora's default of 16, and 18.08 to 18.78 with 1. Twilight Princess asks for none:
  no texture on its disc sets its own (step 4's `disc_scan.txt`), and Aurora only scales what a game asks for (aurora:
  lib/gfx/texture.cpp, anisotropy). Its default stays at 16, for games that ask.
- **The logo shot** took 17.9 ms of GPU time a frame, 0.3 ms more than before, for the depth copy the game makes every
  frame and the depth resolve before it: a copy more (11.0 to 12.0), a draw more and two resolves more. Its end frame
  was the same byte for byte (`dusklight_logo.log`).
- **On the PC,** vulkan4xenon_glsl_tests passed 40 of 40 (`vulkan4xenon_glsl_tests.txt`); tools/check_glsl_math.py
  passed its 20 cases (`check_glsl_math.txt`), three new for the depth copies and the snapshot, against WebGPU's
  gx_z24 in f32, which fail with the depth not reversed or a byte taken wrong (`check_glsl_math_breaks.txt`).
  Dusklight's pack holds the three new programs, 150 in all (`pack_dusklight.txt`); the 147 before are unchanged.
- **Elsewhere:** vkbench passed 14 of 14 (`vkbench.log`), its timings within about 1% of the run before. xsdk_test is
  recorded in xsdk:results/2026-10-15-depth-resolves/: it passed 305 of 305, its replay and pacing tests now reading
  their stats at the last flip, as a vblank after it had counted as missed in 2 of 3 runs (xsdk:docs/HARDWARE.md,
  "Depth resolves").

### Milestone 15: GPU memory in Ordon

**Why.** In the same run the console ran out of memory in Ordon (`dusklight_play.log`): free memory fell from 104 MB
in the title scene to 0, as the title's physical pages, its GPU memory, grew from 53 MB to 143 MB, the last 74 MB in
about 20 s of stages F_SP103 and F_SP104, while newlib's heap held at 280 MB. From then on GPU allocations failed.
What fills it isn't known, as the log counts pages, not what they hold. Candidates from the code, none measured: the
minimap's palette textures, 2.2 MB a pair, made anew at each palette change and freed only once the GPU is done with
the old ones (texture.cpp; aurora-xenon:lib/vulkan4xenon/device.cpp, retire); a pool's blocks, which go back to the
kernel only when empty (device.cpp, free_allocation), so a few live allocations can hold many; and Ordon's own
textures and EFB copies. Milestone 13's glslang keeps 13.5 MB of newlib's heap for the run, which counts against the
same memory.

**Agreed with the user (2026-10-14), in two steps,** the first before milestone 14:
1. **Measure.** The diagnostics build logs GPU memory by what holds it, every 10 s beside the memory line: each pool's
   blocks, bytes in use and largest free range; textures and buffers by label (EFB copies, converted and palette
   textures, render targets, the vertex cache, the frame pools); and what waits to be freed. It gets to Ordon by
   itself, warping to F_SP103 a minute into the title scene as Dusklight's `warp` command does
   (dusklight-xenon:src/dusk/commands.cpp), and stays two minutes, so a run needs no one at the pad; if a warp from
   the title doesn't hold, the user plays there instead. Only the diagnostics build warps. Done when a run in Ordon
   accounts for the physical pages the memory line counts, to within a few MB.
2. **Fix what the numbers show,** scoped with the user once they're in, without cutting resolution or texture quality.

**Left out:** memory past Ordon, until a run gets there.

**Step 1 on the console** (2026-10-14; `results/2026-10-14-milestone-15-step-1/`). The diagnostics build, Dusklight
with its pack and Aurora's diagnostics, counted every `xsdk_gpu_alloc` by caller through the linker's `--wrap`
(`callers.txt`), and Aurora's GPU memory by label and pool. It warped a minute into the title scene to F_SP103's room
1, outside Link's house, and stayed two minutes (`dusklight_mem.log`). It landed in a dialogue, so the minimap was
hidden, though the game still drew it offscreen (`ordon.png`). The physical pages went from 53 MB in the title scene
to 93 MB in Ordon, flat for the last 100 s, with 61 MB free. At the end they held:
- **78.1 MB through `vkAllocateMemory`, Aurora's pools:** eight 8 MB blocks, with 12 to 20 MB free inside them, and
  dedicated buffers: the vertex cache 8 MB, the frames' uniforms 4 MB and indices 2 MB. In the blocks: the minimap's
  palette textures, `GX Dynamic Palette Texture`, 35 to 49 live at once, 16 to 23 MB, none ever written (milestone
  14); the game's textures, 169 of them, 14.4 MB; 12 EFB copies, 6.3 MB; the EFB, 3.6 MB; offscreen targets, 2.4 MB.
- **11.7 MB of xsdk's:** the two display buffers, 7.25 MB (xsdk:src/present.c, xsdk_present_open); the ring, writeback
  and stream, 4.1 MB (xsdk:src/gpu.c); 0.3 MB of shader microcode pages.
- **3 to 4 MB outside xsdk's GPU allocations,** the same from boot on.

Aurora's texture uploads, 16 MB, sit in the cached memory type, which is newlib's heap, not physical pages. So the
physical pages are accounted for to within the 3 to 4 MB there from boot, and step 1 is done. What the run doesn't
show is the played run's 143 MB: here Link stood still in a dialogue. Aurora keeps a palette texture for each EFB
copy it converts until 600 frames go by unused (aurora-xenon:lib/gx/texture.cpp, sweep_object_caches), so 35 to 49 at
once means the copies behind the map keep changing, and a moving player may well change them faster.

### Milestone 16: Dusklight's menus

**Why.** Dusklight's own menus, its settings, controller setup, achievements, graphics tuner and command console, are
RmlUi documents (dusklight-xenon:src/dusk/ui/), and the vulkan4xenon backend skips every RmlUi draw
(aurora-xenon:lib/vulkan4xenon/encoding.cpp, encode_pass: "recorded only on Dawn"), so on the console they run unseen.
Aurora draws them on the PC through its WebGPU render interface (aurora-xenon:lib/rmlui/WebGPURenderInterface.cpp and
pipeline.cpp, about 2,900 lines): geometry with a transform, scissors, clip masks, layers composited together, and
blur, glass and drop-shadow filters.

**Agreed with the user (2026-10-14)** as a milestone after 14 and 15; its scope is settled when it starts.

### Milestone 17: multisampling

**Why.** Aurora draws with Dusklight's anti-aliasing setting on the PC, and the backend draws with one sample whatever
it's set to (aurora-xenon:lib/vulkan4xenon/backend.cpp:401-402). xsdk has had 2- and 4-sample targets since its
milestone 32 (xsdk:SCOPE.md), but at 1280x720 a 2-sample colour and depth target needs 14 MB, past eDRAM's 10 MB, so
the frame has to be drawn in tiles, as Xenos's predicated tiling does (xsdk:SCOPE.md, "predicated tiling").

**Agreed with the user (2026-10-14)** as a milestone after 14 and 15; its scope is settled when it starts.

### The Guide, through xsdk (2026-10-15)

xsdk's milestone 37 shows the Guide: `xsdk_present_end` hands each frame to the kernel's swap, and while the Guide is
open XAM's commands run between the title's frames and draw in eDRAM (xsdk:docs/HARDWARE.md, "The Guide shows through
VdSwap"). So `vkQueuePresentKHR` forgets what the queue keeps in eDRAM, its colour and depths, whenever xsdk's count of
those frames moves (src/swapchain.c). On the console vulkan4xenon_test passed 112 of 112, and vkbench 14 of 14,
this morning's build and the new one twice each in turn: the draw scenes record 0.7 to 2.7% slower and the uniform
scene 3.4% faster, the differences growing with the draws though the swap comes once a frame, so from where the
rebuilt code lies (`results/2026-10-15-guide/`; xsdk:docs/HARDWARE.md, "The Guide, milestone 37"). Dusklight shows the
Guide over the whole frame and leaves through its Xbox Home cleanly.

## 5. The Dusklight track

An outline, made precise one milestone at a time. Each starts only when agreed.

- **What encounter's Aurora needs from Vulkan,** through a backend on the fork's seam:
  - six dynamic uniform buffers, eight combined image samplers and 16 bytes of push constants;
  - dynamic viewport and scissor only;
  - triangle lists with 16-bit indices;
  - RGBA8 colour, D24S8 depth and BC1 textures;
  - render passes with load, clear and don't-care, and framebuffer copies drawn into sampled images.

  The layer's milestones 1 to 4 cover that.
- **What Twilight Princess adds,** as read on 2026-10-11:
  - **Depth copied into a texture every frame:** a half-size `GX_TF_Z16` copy for depth of field
    (dusklight:src/m_Do/m_Do_graphic.cpp:1130-1144), made before the PC build's "depth of field off" return (:1171),
    so it runs either way. The backend logs depth copies and skips them
    (aurora-xenon:lib/vulkan4xenon/encoding.cpp:599).
  - **Shadows in an offscreen target with its own depth** (dusklight:src/d/d_drawlist.cpp:1593-1626), after which
    the frame goes on with the EFB's depth (aurora:lib/gfx/recording.cpp). The layer keeps depth only in
    eDRAM, and loses it when another depth attachment comes between (SUPPORT.md, "Render passes").
  - **15 to 20 framebuffer copies a frame:** bloom's sub-rectangles, depth of field, the half-size frame and shadows
    (dusklight:src/m_Do/m_Do_graphic.cpp:943-1015, 1523-1678). Each breaks the render pass.
  - **`GXPeekZ`,** the game's only CPU read of the framebuffer (dusklight:src/d/d_drawlist.cpp:1848): up to 64 points a
    frame for occlusion tests, the sun's lens flare, the lantern's glow and golden bugs' sparkles
    (dusklight:src/d/d_kankyo_rain.cpp:337-362). The backend returns 0
    (aurora-xenon:lib/vulkan4xenon/backend.cpp:489-497), which only hides them.
  - **Fog range adjustment on every 3D draw** (dusklight:src/d/d_kankyo.cpp:1257, 9457-9460), a factor for each pixel
    column.

  Not needed after all: the Z texture with destination alpha (dusklight:libs/JSystem/src/JFramework/JFWDisplay.cpp:522-550)
  is in `clearEfb`, which returns at once on PC (:486); with it go the game's only destination alpha and any need for
  a constant blend colour, as the backend clears by drawing with write masks. Every GX texture format, palettes too,
  is decoded to RGBA8 on the CPU (aurora:lib/gfx/texture_convert.cpp), so more formats would save memory
  but block nothing.
- **The backend.** The fork adds a backend interface to encounter's Aurora, `lib/gfx/backend.hpp`, and the
  vulkan4xenon backend goes behind it.
  - **Why a fork and not upstream:** upstream draws only through Dawn, and its shaders read the game's vertex
    arrays from storage buffers and unpack big-endian bytes with integer shifts and masks
    (aurora:lib/gx/shader.cpp:1717-1764). Xenos has no storage buffers and only float ALU instructions
    (xenia:src/xenia/gpu/ucode.h:1006, 1359), and WebGPU requires compute shaders. So the backend decodes vertices
    on the CPU into one stream and keeps uniforms in uniform buffers (aurora-xenon:lib/vulkan4xenon/decode.hpp).
  - **Shaders:** the 360 has no shader compiler on the console, so:
    - a generator modelled on the WGSL's (aurora:lib/gx/shader.cpp) writes Vulkan GLSL, without its integer
      operations;
    - the 360's null backend runs it on every configuration the game asks for and writes the GLSL out, in Xenia or
      on the console (decided 2026-10-12, milestone 10). Aurora's pipeline cache holds configurations as raw structs
      (aurora:lib/gfx/pipeline_cache.cpp), bitfields included, so one recorded on a PC doesn't carry
      over;
    - glslang turns each into SPIR-V and xost into a container, and the pack ships both;
    - on the console the backend looks up the SPIR-V by configuration and hands it to `vkCreateShaderModule`, which
      finds the container by that SPIR-V. A configuration missing from the pack isn't drawn.
  - **Vertices:** encounter's Aurora fetches the game's raw big-endian vertex arrays in its shaders; the backend
    decodes them on the CPU, as the layer has no storage buffers. Xenos fetches big-endian data natively under
    an 8-in-16 swap. Fetching the raw arrays directly may avoid the decode; it needs xsdk to set a stream's endian
    swap, which is 8-in-32 for every stream today. Unverified until tried.
- **The rest of the port:**
  - **The game:** C11 and C++20 (dusklight:CMakeLists.txt:31-33), which xsdk's GCC 15 builds.
  - **The disc:** encounter's Aurora reads disc images through nod (aurora:CMakeLists.txt:11, 46), a Rust library;
    the port needs a reader in C.
  - **The platform:** encounter's Aurora takes threads, input, audio and windows from SDL3
    (aurora:CMakeLists.txt:39-44). Here they come from xsdk.
  - **Memory:** a budget within the console's 512 MB, which the CPU and GPU share.
  - **Byte order:** the console is big-endian like the GameCube, so the PC build's swaps at load must turn off
    (dusklight:include/helpers/endian.h).
- **What the shaders need from xost,** beyond the GLSL generator:
  - uniform arrays indexed at run time: the position, normal and texture matrices are picked by an index per vertex
    (aurora:lib/gx/shader.cpp). Xenos reads constants at `c[a0 + n]`, `a0` set by `MOVA`
    (xenia:src/xenia/gpu/ucode.h:196-202, 1140-1148); xost reads arrays only by a loop counter (xost:docs/xsc.md);
  - the fragment's position, for range fog: the pixel shader's generated parameter
    (xenia:src/xenia/gpu/registers.h:140, 234);
  - depth written by a pixel shader, to draw depth back into eDRAM (xenia:src/xenia/gpu/shader.h:972);
  - vertex inputs past position, colour and texture coordinates, for the matrix indices;
  - Twilight Princess's longest TEV chains within xost's 16 registers a pixel shader, unchecked.
- **What the layer needs at start-up:** 8-bit normalized vertex attributes, which xsdk has (`XSDK_VERTEX_UBYTE4N`,
  xsdk:include/xsdk/shader.h). The rest is the backend's to fit: one memory type, a `B8G8R8A8` swapchain,
  256-byte uniform offsets, and textures of any size tiled rather than linear.
- **The order, agreed on 2026-10-11:**
  1. **Milestone 9: Dusklight on Aurora's null backend,** running on the console without drawing up to the title
     scene's game loop: the build, the platform from xsdk, the disc, memory and byte order.
  2. **Milestone 10: the boot screens drawn** (section 4), changed on 2026-10-12 from "the shaders, from a pipeline
     cache recorded on a PC": the GLSL generator for the boot screens' configurations, recorded on the 360, and the
     backend that draws them.
  3. **Milestone 11: the title screen drawn** (stage F_SP102, dusklight:src/d/d_com_inf_game.cpp:1221), with the rest
     of the generator and what xost needs above, and its
     colour effects, through render to texture as it stands; depth copies skipped. It takes the
     byte order milestone 9 leaves: Aurora's texture conversion and the arrays flagged little-endian.
  4. **Depth out of eDRAM:** xsdk resolves depth to memory and samples it, after console probes of what a depth
     resolve writes (xenia:src/xenia/gpu/xenos.h:917-924); then the layer stores, samples and reloads depth
     attachments. That brings the Z16 copy, shadows over the EFB's depth, and `GXPeekZ` read from resolved depth.
  5. **Copies at the frame's pace,** if the frame needs it: sub-rectangle resolves, which libxenon says can't be done
     (libxenon:drivers/xenos/xe.c:1076) and Xenia finds games doing (xenia:src/xenia/gpu/xenos.h:944-976), and small
     passes kept out of the EFB's eDRAM.

  Later: logic ops, lines and points (the minimap), native texture formats, uploads that don't stall, the sampler's
  maximum LOD.
- **Where the code lives (decided 2026-10-11):** Dusklight's changes in a fork of its own, Z:\dusklight-xenon, from
  TwilitRealm/dusklight at `2c4960d` (remote `upstream`). Aurora's are a fork of encounter's Aurora,
  aurora-xenon, on `3227d76`, the commit Dusklight pins.

## 6. Left out

- Compute, queries, secondary command buffers, more than one queue, multisampling, multiple subpasses and dynamic
  rendering.
- Vulkan 1.1 and up (single pieces of 1.1 as extensions, section 2).
- Geometry and tessellation shaders.
- xsdk's TODO.md items, each until a milestone or Dusklight needs it.

## 7. Open

- **What milestone 5 found slow,** for when it's taken on:
  - reading uniform buffers at submit, 4.0 us a draw for two blocks, as mapped memory is write-combined. Milestone 6
    brought it to 0.87 us with 16-byte reads, and milestone 7 to none: the GPU loads each block. Encounter's Aurora
    passes its uniforms this way;
  - push constants are now the dearer path, 0.97 us a draw at submit against a uniform draw's 0.50. Milestone 8 had
    the GPU load them, 17 dwords a cube from 65, and the submit didn't move, so the cost lies elsewhere in the push
    path;
  - the replay pointing streams and textures again every draw though they didn't change. Milestone 6 points only what
    changed, and a cube costs 1.22 us through the layer against xsdk's 0.62, 1.97 times;
  - apps writing mapped memory with `memcpy` ran at 185 MB/s there. Since xsdk milestone 20, `memcpy` is xsdk's own
    and copies 5.6 GB/s from the cache and 1.5 GB/s from memory into write-combined memory (xsdk:docs/HARDWARE.md),
    so this one is done.
