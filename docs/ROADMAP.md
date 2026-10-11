# vulkan4xenon roadmap (agreed 2026-10-11)

**Goal.** A Vulkan 1.0 subset for the Xbox 360: a static library on the xsdk GPU layer, for ports that run on the
retail kernel. The goal it's measured against is **Dusklight**, a reimplementation of Twilight Princess, running on
the console through it.

**The template.** vulkan4vita is the layout to follow (vulkan4vita:docs/ROADMAP.md:3-30). What it did on the Vita's
native GPU API, GXM, this does on Xenos through xsdk's GPU layer. Its Vita specifics don't carry over.

**Status.** Agreed on 2026-10-11. Milestone 1 is done; milestone 2 is under way. Section 7 lists what's still open.

**Constraints.** xsdk's, unchanged:
- No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Every claim about Xenos cites Xenia, libxenon, xost or a hardware result.
- The console decides: a milestone is done only on a clean hardware run. Xenia is for smoke checks.

**Sources.**
- **xsdk** (Z:\Xbox-360-SDK), cited as `xsdk:<path>`. What the console has settled is in `xsdk:docs/HARDWARE.md`.
- **vulkan4vita** (Z:\vulkan4vita, `37af886`), cited as `vulkan4vita:<path>:<line>`.
- **aurora-vita** (Z:\aurora-vita, `642b549`) is the user's port of encounter's Aurora onto vulkan4vita, cited as
  `aurora-vita:<path>:<line>`.
- **Dusklight** (TwilitRealm/dusklight at `2c4960d`), cited as `dusklight:<path>:<line>`.
- **encounter's Aurora** (encounter/aurora at `3227d76`, the commit Dusklight pins), cited as `aurora:<path>:<line>`.
  This is the GameCube and Wii compatibility layer Dusklight is built on, not the Aurora dashboard titles launch from.
- **xost** (xsdk's `xenos-toolchain/`), cited as `xost:<path>:<line>`.

## 1. What carries over from vulkan4vita

- **A static library.**
  - Apps call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` work too
    (vulkan4vita:docs/ROADMAP.md:10-13).
  - It reports Vulkan 1.0, refuses 1.1 and up, and claims no conformance (vulkan4vita:docs/SUPPORT.md:5-7).
- **Khronos headers, vendored** in `third_party/vulkan-headers/`, as vulkan4vita carries them. They're Apache-2.0.
- **docs/SUPPORT.md** is the one list of what exists, every limit below the spec's minimum, and every deviation, each
  with a reason. It changes in the same commit as the code (vulkan4vita:CODE_GUIDELINES.md:131-138).
- **Milestones written as vulkan4vita writes them** (vulkan4vita:docs/ROADMAP.md:3-64):
  - **Done when:** criteria a hardware run can check.
  - **Passed on hardware** on a date, at a commit, with the results' folder.
  - **Decisions**, **How it maps onto Xenos**, **New functions**, **New tests** and **Not in milestone N**.
- **Feature tests** in one title, each checking pixels of the presented image (vulkan4vita:README.md:157-162). They're
  broken on purpose before a hardware run, as xsdk's are.
- **A benchmark against the raw GPU layer,** as vulkan4vita measured itself against raw GXM
  (vulkan4vita:README.md:12-48).
- **The boot logo:** the vulkan4xenon logo shows from `vkCreateDevice` until the app's first present, fading in from
  black (vulkan4vita:docs/ROADMAP.md:706-758). The image and its generator are xsdk's (`xsdk:assets/vulkan4xenon.png`,
  `xsdk:tools/gen_splash.py`). The Vulkan logo is a registered trademark of the Khronos Group, used at the user's
  choice.
- **Lessons that transfer:**
  - Where the GPU's own ordering isn't proven, wait on the CPU. GXM's sync flags didn't order transfers against scenes
    on hardware (vulkan4vita:docs/ROADMAP.md:203-205).
  - Measure vendor fast paths before keeping them: GXM's precomputed draws didn't pay, and that milestone closed
    (vulkan4vita:docs/ROADMAP.md:322-395).
  - Check the accounting before the ratio: a miscounted present skewed every early benchmark
    (vulkan4vita:docs/ROADMAP.md:264-271).
  - A passing test can hide an app that hangs, as the boot logo's present chain did
    (vulkan4vita:docs/ROADMAP.md:729-733).

## 2. Decided (2026-10-11)

- **Its own repository,** this one. It links xsdk's `libxsdk.a` and can join the combined xsdk and xost later.
- **Shaders are compiled offline and looked up by their SPIR-V.**
  - Apps hand `vkCreateShaderModule` standard SPIR-V.
  - A build step runs xost over each module and writes a bundle of Xenos containers keyed by the SPIR-V; xost goes
    SPIR-V → Xenos (xost:docs/xsc.md:1-14).
  - `vkCreateShaderModule` hashes the code and finds its container. A module missing from the bundle fails with
    `VK_ERROR_FEATURE_NOT_PRESENT`, as vulkan4vita fails without its own format (vulkan4vita:docs/SUPPORT.md:11-16).
  - **Python only in that offline step.** xost moving off Python is a later item (xsdk:TODO.md, "Toolchain").
- **Uniform buffers are copied into float constants** when a command buffer replays. Loading them from memory with
  `PM4_LOAD_ALU_CONSTANT` (xenia:src/xenia/gpu/command_processor.cc:1477-1500) is a later optimisation. It needs a
  probe first, since partial constant writes hang the GPU (xsdk:docs/HARDWARE.md, milestone 11).
- **The target is Dusklight,** through encounter's Aurora. It's demanding enough to show what's missing.
- **Vulkan 1.0 stays the version.** Nothing on the path needs 1.1: aurora-vita's backend asks for 1.0
  (aurora-vita:lib/vulkan4vita/device.cpp:308), most of what 1.1 promoted is optional, and its one new requirement,
  subgroup operations, is for compute shaders, which Xenos lacks. A single 1.1 piece a target needs comes as its
  extension on 1.0, such as `VK_KHR_maintenance1` for a negative viewport height.
- **Aurora's side is a fork of aurora-vita,** for its backend interface and its design for a GPU without storage
  buffers (section 5), with its Cg replaced.
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
| Descriptor sets | Combined image samplers fill fetch-constant slots 0 up; uniform buffers fill constant blocks. Bindings are numbered in order across the pipeline layout, as vulkan4vita numbers them (vulkan4vita:docs/SUPPORT.md:41-127). |
| Graphics pipelines | Built once at creation from xsdk's depth and stencil, blend and raster state blocks. The vertex input state gives a patched copy of the vertex shader (xsdk milestone 17). |
| Render passes, framebuffers | One subpass. A render pass instance draws into eDRAM, and each attachment's store op is a resolve into its image. A clear load op is the resolve's clear; a load op draws the image back in. 1280x720 colour and depth fit eDRAM (xsdk:SCOPE.md 3.4). |
| Command buffers | Recorded into the layer's own list and replayed into the ring at `vkQueueSubmit`, as vulkan4vita replays (vulkan4vita:docs/ROADMAP.md:21-30). xsdk's recorded command buffers (milestone 18) can take over where a buffer's state is all known when it's recorded. |
| Fences | xsdk's fences. |
| Semaphores, barriers | One queue, so semaphores do nothing. Barriers order copies against draws. |
| Swapchain | `VK_KHR_surface` and `VK_KHR_display`, as vulkan4vita has, over `xsdk_present`: 2 or 3 images at 1280x720. |
| Copies | `vkCmdCopyBufferToImage` and the like run on the CPU at replay, as vulkan4vita settled on (vulkan4vita:docs/ROADMAP.md:585-634). |

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
   checked by pixel. Encounter's Aurora copies its framebuffer this way (aurora-vita:lib/vulkan4vita/encoding.cpp).
5. **The benchmark.** Done when the same scenes run through the raw GPU layer and through Vulkan, with the gap
   measured on the console and its causes named.

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
    This is vulkan4vita's scheme (vulkan4vita:src/objects.h:384-393).
- **Replay.**
  - `vkQueueSubmit` replays on the caller's thread into xsdk's ring, as vulkan4vita's first milestone did.
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
  - Its buffers are freed only after that flip and one more vblank: vulkan4vita's hang came from freeing the logo's
    frame too early (vulkan4vita:docs/ROADMAP.md:729-733).

**How it maps onto Xenos:** as section 3, less descriptors, uniform buffers, images other than the swapchain's, and
copies.

**New in xsdk:**
- `xsdk_splash_begin` and `xsdk_splash_end`, with `xsdk_present_open` taking over a splash's display;
- `xsdk_present_buffer_address`;
- `xsdk_display_shown_address`, so a test can read the image actually on screen, as vulkan4vita's tests read the
  Vita's (vulkan4vita:tests/harness.c:1066-1080).

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

### Milestone 2: textures, descriptors and push constants (in progress)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestone 1's checks and these, each checked by
pixel on the image the display shows:
- `texture`: images copied in from buffers with `vkCmdCopyBufferToImage`, in `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`,
  sampled through combined image sampler descriptors;
- `push_constants`: quads placed and coloured by push constants, a vertex range and a fragment range;
- `uniform_buffers`: quads placed by a matrix and coloured by vectors from uniform buffers, one of them dynamic and read
  at a different offset each draw, in two descriptor sets.

**Passed on hardware:** not yet.

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
- **Copies run on the CPU at replay,** as vulkan4vita settled (vulkan4vita:docs/ROADMAP.md:585-634). Replay first
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

## 5. The Dusklight track

An outline, made precise one milestone at a time. Each starts only when agreed.

- **What encounter's Aurora needs from Vulkan.** aurora-vita's backend uses (aurora-vita:lib/vulkan4vita/):
  - six dynamic uniform buffers, eight combined image samplers and 16 bytes of push constants;
  - dynamic viewport and scissor only;
  - triangle lists with 16-bit indices;
  - RGBA8 colour, D24S8 depth and BC1 textures;
  - render passes with load, clear and don't-care, and framebuffer copies drawn into sampled images.

  The layer's milestones 1 to 4 cover that.
- **What Twilight Princess adds beyond aurora-vita's game:**
  - depth read back on the CPU, for `GXPeekZ` (dusklight:src/d/d_drawlist.cpp:1848);
  - depth copied into a texture (`GX_TF_Z16`, dusklight:src/m_Do/m_Do_graphic.cpp:1132);
  - a Z texture with destination alpha (dusklight:libs/JSystem/src/JFramework/JFWDisplay.cpp:522-550).

  On xsdk's side these meet open items in its TODO.md: depth resolves, a constant blend colour, more texture
  formats.
- **The backend.** aurora-vita added a backend interface to encounter's Aurora, `lib/gfx/backend.hpp`, and a Vulkan
  backend behind it, about 6,200 lines (aurora-vita:lib/vulkan4vita/). A vulkan4xenon backend follows the same shape.
  - **Why aurora-vita and not upstream:** upstream draws only through Dawn, and its shaders read the game's vertex
    arrays from storage buffers and unpack big-endian bytes with integer shifts and masks
    (aurora:lib/gx/shader.cpp:1717-1764). Xenos has no storage buffers and only float ALU instructions
    (xenia:src/xenia/gpu/ucode.h:1006, 1359), and WebGPU requires compute shaders. aurora-vita already decodes
    vertices on the CPU into one stream and keeps uniforms in uniform buffers (aurora-vita:lib/vulkan4vita/cg.hpp).
  - **Shaders:** that backend writes Cg per GX configuration and compiles it on the Vita with the firmware's compiler,
    caching the result on the memory card (aurora-vita:lib/vulkan4vita/pipeline.cpp:1, 191-222, 539-542). The 360
    has no compiler on the console, so:
    - a generator modelled on `cg.cpp` writes Vulkan GLSL instead, without the integer operations `cg.cpp` uses
      (`(int)x & mask`, aurora-vita:lib/vulkan4vita/cg.cpp:214);
    - an offline step runs it over Aurora's pipeline cache, which records every configuration a playthrough used
      (aurora-vita:lib/gfx/pipeline_cache.cpp:357-370). The cache holds configurations as raw bytes, so one made on a
      PC is little-endian and the step byte-swaps it;
    - glslang turns each into SPIR-V and xost into a container, and the pack ships both;
    - on the console the backend looks up the SPIR-V by configuration and hands it to `vkCreateShaderModule`, which
      finds the container by that SPIR-V. A configuration missing from the pack isn't drawn, as on aurora-vita
      without the firmware's compiler (aurora-vita:lib/vulkan4vita/pipeline.cpp:542).
  - **Vertices:** encounter's Aurora fetches the game's raw big-endian vertex arrays in its shaders, and aurora-vita
    decoded them on the CPU because vulkan4vita has no storage buffers. Xenos fetches big-endian data natively under
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
- **A likely order:**
  1. the game running without drawing, on encounter's Aurora's null backend, to settle the platform, the disc and
     memory;
  2. the title screen through the vulkan4xenon backend;
  3. gameplay with Twilight Princess's extra features;
  4. performance.

## 6. Left out

- Compute, queries, secondary command buffers, more than one queue, multisampling, multiple subpasses and dynamic
  rendering, as vulkan4vita left them out (vulkan4vita:docs/ROADMAP.md:53, 119-120).
- Runtime SPIR-V compilation, and Vulkan 1.1 and up (single pieces of 1.1 as extensions, section 2).
- Geometry and tessellation shaders.
- xsdk's TODO.md items, each until a milestone or Dusklight needs it.

## 7. Open

- **Where Dusklight's own changes live:** probably a fork, for the byte order and the platform. Decide before the
  Dusklight track starts. Aurora's side is a fork of aurora-vita (section 2).
