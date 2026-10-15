# What vulkan4xenon supports

What the layer implements today. Anything not listed here isn't supported. Each limit below the Vulkan spec's minimum
and each deviation gives its reason (CODE_GUIDELINES.md, "Vulkan behavior").

## Version

Vulkan 1.0. `vkCreateInstance` refuses an `apiVersion` above 1.0 with `VK_ERROR_INCOMPATIBLE_DRIVER`, as a 1.0
implementation must. vulkan4xenon claims no conformance: it's a subset, and the deviations below break the spec's
required minimums.

## Extensions

- Instance: `VK_KHR_surface` and `VK_KHR_display`.
- Device: `VK_KHR_swapchain`.
- No layers.

## Functions

| Group | Functions |
|---|---|
| Instance | `vkCreateInstance`, `vkDestroyInstance`, `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`, `vkGetInstanceProcAddr`, `vkEnumeratePhysicalDevices` |
| Physical device | `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceFormatProperties`, `vkGetPhysicalDeviceImageFormatProperties`, `vkEnumerateDeviceExtensionProperties`, `vkEnumerateDeviceLayerProperties` |
| Device | `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceProcAddr`, `vkGetDeviceQueue`, `vkDeviceWaitIdle` |
| Memory and buffers | `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`, `vkUnmapMemory`, `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`, `vkCreateBuffer`, `vkDestroyBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory` |
| Pipelines | `vkCreateShaderModule`, `vkDestroyShaderModule`, `vkCreatePipelineLayout`, `vkDestroyPipelineLayout`, `vkCreateGraphicsPipelines`, `vkDestroyPipeline` |
| Images and samplers | `vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`, `vkBindImageMemory`, `vkCreateImageView`, `vkDestroyImageView`, `vkCreateSampler`, `vkDestroySampler` |
| Descriptor sets | `vkCreateDescriptorSetLayout`, `vkDestroyDescriptorSetLayout`, `vkCreateDescriptorPool`, `vkDestroyDescriptorPool`, `vkResetDescriptorPool`, `vkAllocateDescriptorSets`, `vkFreeDescriptorSets`, `vkUpdateDescriptorSets` |
| Render passes | `vkCreateRenderPass`, `vkDestroyRenderPass`, `vkCreateFramebuffer`, `vkDestroyFramebuffer` |
| Commands | `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkResetCommandPool`, `vkAllocateCommandBuffers`, `vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkResetCommandBuffer`, `vkCmdBeginRenderPass`, `vkCmdEndRenderPass`, `vkCmdBindPipeline`, `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdDraw`, `vkCmdDrawIndexed`, `vkCmdCopyBufferToImage`, `vkCmdPipelineBarrier` |
| Sync and submit | `vkQueueSubmit`, `vkQueueWaitIdle`, `vkCreateFence`, `vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkWaitForFences`, `vkCreateSemaphore`, `vkDestroySemaphore` |
| Display | `vkGetPhysicalDeviceDisplayPropertiesKHR`, `vkGetPhysicalDeviceDisplayPlanePropertiesKHR`, `vkGetDisplayPlaneSupportedDisplaysKHR`, `vkGetDisplayModePropertiesKHR`, `vkCreateDisplayModeKHR`, `vkGetDisplayPlaneCapabilitiesKHR`, `vkCreateDisplayPlaneSurfaceKHR`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`, `vkGetPhysicalDeviceSurfacePresentModesKHR` |
| Swapchain | `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` |

Apps call them directly. `vkGetInstanceProcAddr` finds global functions with a `NULL` instance and every function
with one; `vkGetDeviceProcAddr` finds device functions only.

## Device

- One physical device, and one device at a time: a second `vkCreateDevice` returns `VK_ERROR_TOO_MANY_OBJECTS`, since
  the GPU has one owner. `vkCreateDevice` takes the GPU from XAM (`xsdk_gpu_open`) and shows the boot logo.
- One feature, `samplerAnisotropy` (Samplers). Requesting any other returns `VK_ERROR_FEATURE_NOT_PRESENT`. Vulkan
  1.0 requires `robustBufferAccess`; that's a deviation.
- One queue family with one queue, `VK_QUEUE_GRAPHICS_BIT` only. Vulkan 1.0 requires a family with graphics and
  compute together; Xenos has no compute stage, so that's a deviation.

## Memory and buffers

- One heap of 256 MB, device-local. That's a budget, half the console's 512 MB that the CPU and GPU share, not a
  measurement.
- Two memory types, both in that heap:
  - 0: device-local, host-visible and coherent. Each allocation is `xsdk_gpu_alloc`'s write-combined physical memory,
    in 64 KB pages, so `minMemoryMapAlignment` is 64 KB.
  - 1: host-visible, coherent and cached: the CPU's own memory, from newlib's heap, 128-byte aligned. The GPU never
    reads it, so only buffers whose sole usage is `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` list it in their
    `memoryTypeBits`: copies run on the CPU, which reads it at the cache's speed (milestone 12). Another buffer, or an
    image, bound to it makes the bind return `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- Memory of type 0 is write-combined:
  - `memcpy` into it is safe, since the toolchain's `memcpy` stores bytes or aligned words;
  - a plain 8-byte store needs an 8-byte-aligned address, or it raises an alignment exception (xsdk:CLAUDE.md);
  - reading it back is slow, as it isn't cached.
- Coherence: `vkQueueSubmit` drains the CPU's write-combining (`sync`) before replaying, so flushing ranges does
  nothing and isn't needed.
- Buffers: 64-byte alignment, sizes rounded up to it, so the GPU can load uniform blocks straight from them. Sparse
  buffers return `VK_ERROR_FEATURE_NOT_PRESENT`.

## Shaders

- `vkCreateShaderModule` takes standard SPIR-V and looks it up in the app's `v4x_shader_bundle`, which
  `tools/build_shaders.py` writes: GLSL → `glslang -V` → xost → a Xenos container. The key is a 64-bit FNV-1a of the
  SPIR-V and its size (`include/vulkan4xenon.h`).
- SPIR-V the bundle doesn't hold is compiled on the console the first time a module is made from it, through xost's
  compiler in xsdk (`xsdk_xsc_compile`), at xsdk's register split with the optimiser on, and kept till the device goes
  (xsdk:docs/scope/shader-compiling.md, step 3). The layer logs each shader it compiles and the time it took.
- An app that names a folder with `v4x_set_shader_cache`, before it makes shader modules, has what the layer compiles
  kept there, in a folder named after the compiler's version (`xsdk_xsc_version()`), one file a shader named after its
  SPIR-V's hash and size. Each file is written beside its name and renamed into place, so a crash leaves none half
  written. A later device reads a shader back rather than compile it; a file that doesn't match its name, or whose size
  doesn't add up, is logged and the shader compiled again. `v4x_shader_counts` says how many the layer compiled and
  read back. With no folder named, nothing is kept.
- SPIR-V xost refuses returns `VK_ERROR_FEATURE_NOT_PRESENT`, logged with xost's reason, and so does a shader that
  wants a hardware loop's constants, which the layer doesn't set. `vkCreateShaderModule` doesn't list that result: a
  deviation.
- The shader subset is xost's (xost:docs/xsc.md): no bitwise operations, data-dependent branches inside loops raise an
  error, and 16 registers a pixel shader.
- Vertex input location n feeds the usage xost gives it: 0 position, 1 colour, 2 and up texture coordinates
  (xsdk:src/xsc_resolve.c:344-346).
- The literal constants xost pools are written when a pipeline is bound, at the registers after the shader's
  uniforms.
- Where a shader's resources land is xost's choice, recorded in the bundle as its `V4xShaderResource`s
  (xost:docs/xsc.md, "Interface conventions"):
  - push constants take the shader's first float constant registers, then each uniform block in (set, binding) order,
    each holding the block's std140 bytes from its first;
  - combined image samplers take fetch constants from 0 in (set, binding) order.

## Pipelines

- Pipeline layouts: up to 4 descriptor set layouts, and push constant ranges within 128 bytes.
- A pipeline's shaders read only what its layout has, for their stage. A resource the layout lacks makes
  `vkCreateGraphicsPipelines` log it and return `VK_ERROR_INITIALIZATION_FAILED`, which it doesn't list: that's invalid
  use, refused rather than drawn wrong.
- Fragment shaders sample; a sampler in a vertex shader returns `VK_ERROR_FEATURE_NOT_PRESENT`, since both stages share
  the fetch constants. Each stage's blocks must fit its 256 float constants.
- Graphics pipelines, otherwise `VK_ERROR_FEATURE_NOT_PRESENT`:
  - a vertex and a fragment stage, entry point `main`, no specialization constants;
  - per-vertex bindings with strides a multiple of 4, up to 1020 bytes;
  - `VK_FORMAT_R32_SFLOAT` to `R32G32B32A32_SFLOAT` and `A8B8G8R8_UNORM_PACK32` attributes at offsets a multiple of
    4;
  - point lists, line lists and strips, and triangle lists, strips and fans, without primitive restart (an xsdk
    TODO);
  - one viewport and scissor, set in the pipeline or dynamic; no other dynamic state;
  - filled polygons, line width 1, no depth clamp;
  - one sample;
  - one colour attachment.
- **Rasterization:**
  - culling front or back faces, or none; culling both returns `VK_ERROR_FEATURE_NOT_PRESENT`;
  - `frontFace` maps straight to xsdk's `front_clockwise`. Both mean clockwise as the triangle looks in the y-down
    framebuffer (xsdk:tests/test_raster.c:207), and the viewport flip keeps every vertex where Vulkan puts it;
  - depth bias: the constant factor is in units of 2^-24, D24's resolution, so the layer divides it by 2^24 into the
    hardware's depth units (xenia:src/xenia/gpu/draw_util.h:165-172). The slope factor passes through as xsdk's
    per-pixel slope, unverified as in xsdk. A bias clamp returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Depth and stencil,** with a depth attachment:
  - depth test, write and compare op;
  - stencil test, ops, compare op, masks and reference, the same for both faces. Faces that differ return
    `VK_ERROR_FEATURE_NOT_PRESENT` (two-sided stencil is an xsdk TODO).
  - Vulkan's compare and stencil ops are xsdk's, value for value. No depth bounds test.
- **Blending:**
  - every factor but the constant colours and dual-source ones, which return `VK_ERROR_FEATURE_NOT_PRESENT`;
  - every op, with min and max ignoring their factors as Vulkan says. Xenia ignores them too
    (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324), and the layer sets them to one, so the result holds either
    way;
  - the colour write mask. No logic ops.
- **Deviation:** points are 1 pixel, xsdk's `PA_SU_POINT_SIZE`; the shader's `gl_PointSize` is ignored.
- The pipeline cache passed in is ignored; `vkCreatePipelineCache` isn't implemented.
- Clip space is Vulkan's:
  - y points down, so the viewport is set from its bottom edge with a negative height, against xsdk's y-up
    (xsdk:src/gpu_draw.c:298);
  - pixel centres sit on halves. Xenos puts them on whole coordinates under xsdk's `PIX_CENTER_D3D`
    (xenia:src/xenia/gpu/xenos.h:861-866), so the viewport moves half a pixel up and left. On the console, edges a
    quarter pixel off the grid covered exactly what Vulkan's centres say (milestone 3).

## Render passes

- One subpass with one colour attachment, `VK_FORMAT_R8G8B8A8_UNORM` or `VK_FORMAT_B8G8R8A8_UNORM`, one sample, and
  optionally a depth attachment, `VK_FORMAT_D24_UNORM_S8_UINT`.
- Every load op: clear, load or don't-care. Store op store or don't-care.
- Depth and stencil clear together, as one register holds both (xsdk:src/gpu_resolve.c). A depth attachment that
  clears one and loads the other returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Depth stays in eDRAM:** a depth attachment is stored in eDRAM. Loading it finds what the last pass left there, as
  long as no colour and no other depth reached its tiles in between: a pass with another depth places it under the
  first where both fit above the pass's colour (How it maps). **Deviation:** otherwise its contents are undefined, and
  the layer logs that once.
- **Depth to sample:** a depth image made with `VK_IMAGE_USAGE_SAMPLED_BIT` is resolved out of eDRAM into its texture
  (`xsdk_gpu_resolve_depth_texture`) when a pass stores its depth or stencil and leaves it in a layout a shader samples
  in: `finalLayout` `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`, `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` or
  `VK_IMAGE_LAYOUT_GENERAL`. A pass that leaves it `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` keeps it in eDRAM
  alone, so the passes a later one loads cost no resolve (milestone 14). The resolve comes before the colour's and
  clears nothing, so the colour's clears as it would.
- A barrier moving a sampled depth image from `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` to one of those
  layouts fails the command buffer: `vkEndCommandBuffer` returns `VK_ERROR_FEATURE_NOT_PRESENT`, as only a pass's end
  resolves depth. Depth images can't be copied.
- Framebuffers: the render pass's attachments, their full size, up to 1280x720, one layer. Colour attachments are
  swapchain images or images made with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`. The render area must be the whole
  framebuffer.
- How it maps (docs/ROADMAP.md, milestone 4):
  - the pass draws into eDRAM, colour from tile 0 and depth where eDRAM holds its image's, or the clear it wants at
    its size; a depth eDRAM doesn't hold goes ending at tile 2048, or just under a depth image eDRAM holds there when
    that stays above the pass's colour, so a pass with a smaller depth of its own leaves the earlier one alone, as a
    pass without depth does (milestone 14). Up to four depths are tracked, each lost when a pass's colour or depth
    reaches its tiles;
  - its end resolves the colour into the attachment's image;
  - the layer tracks what eDRAM holds: values a resolve cleared it to, at a framebuffer's size, or an image's
    contents;
  - a clear finds its values there, or beginning the pass resolves into the attachment to set them, copying junk the
    pass then overwrites;
  - a load finds its image there, or the layer draws the image back in, texel for texel, with its own shaders;
  - a pass's resolve clears what the next pass in the command buffer clears, and keeps what it loads. The last pass in
    a command buffer clears to its own values, so the next frame's usually finds them.

## Images

- 2D, one layer, one sample, optimal tiling, used for sampling, copying in and as colour attachments; anything else
  returns `VK_ERROR_FEATURE_NOT_PRESENT`. Mip levels down to 1x1.
- Colour attachment images are `VK_FORMAT_R8G8B8A8_UNORM` or `VK_FORMAT_B8G8R8A8_UNORM`, with one level, and both
  sides over 16 texels: a smaller base level sits in the packed mip tail, where a resolve can't write
  (xsdk:src/gpu_resolve.c). Others return `VK_ERROR_FEATURE_NOT_PRESENT`.
- Optimal tiling is Xenos's tiled layout, with mips as xsdk milestones 15 and 16 settled.
- Memory requirements: the texture's size, 4 KB aligned, as a fetch constant holds its address in 4 KB units. Binding
  places the texels at the memory (`xsdk_texture_attach`).
- Depth images: `VK_FORMAT_D24_UNORM_S8_UINT`, one level, a depth attachment, and sampled if they're made to be. One
  that isn't has a memory requirement of one page, which nothing uses. A sampled one is a tiled D24S8 texture
  (`XSDK_FORMAT_D24S8`), both sides over 16 texels as a resolve needs, and needs its texture's memory:
  - after a pass leaves it to be read (Render passes), each texel's word in its memory holds depth's 24 bits above
    stencil's 8, as the CPU reads it, at `tiled_index` (tests/tiled_index.h) of the texel and the 32-texel pitch.
    **Deviation:** Vulkan leaves an optimal-tiled image's memory opaque; a mapped read of it is how an app reads depth
    back without a copy that waits for the GPU;
  - sampled, it returns the depth from 0 to 1 in red, 0 in green and blue and 1 in alpha, whatever the view's aspect
    mask.
- Image views: the whole of a 2D image in its format. A component mapping sets the fetch constant's swizzle
  (`xsdk_texture_swizzle`), so a view samples any of the format's channels, 0 or 1 as each of red, green, blue and
  alpha; a swizzled view of a depth image returns `VK_ERROR_FEATURE_NOT_PRESENT`.

## Samplers

- Nearest or linear filtering and mips; repeat, mirrored repeat or clamp to edge; a LOD bias within 15.
- Anisotropy, on a device made with `samplerAnisotropy`: `maxAnisotropy` from 1 to 16, which xsdk's sampler rounds
  down to a power of two (xsdk:include/xsdk/texture.h). Without the feature, or past 16, it returns
  `VK_ERROR_FEATURE_NOT_PRESENT`.
- Clamp to border, comparison, unnormalized coordinates and a minimum LOD above 0 return
  `VK_ERROR_FEATURE_NOT_PRESENT`; they're xsdk TODO items.
- **Deviation:** the maximum LOD is ignored, so a mipped image samples all its levels.

## Descriptor sets

- Host memory: a descriptor a binding, read when a draw replays. Bindings run from 0 to 31, one descriptor each.
- Types: uniform buffers, dynamic uniform buffers and combined image samplers. Anything else, arrays and immutable
  samplers return `VK_ERROR_FEATURE_NOT_PRESENT`.
- A write of several descriptors fills consecutive bindings.
- Pools hold any number of sets; their sizes and limits are ignored.
- **How draws read them:** each draw puts every uniform block its shaders read into float constants, from the
  buffer at the descriptor's offset plus any dynamic offset, as section 2 of the roadmap decided.
  - The GPU loads a block straight from the buffer when its address is 64-byte aligned and the descriptor's range
    covers it, which buffers and offsets of 256 make the usual case (milestone 7).
  - Otherwise the CPU reads it, 16 bytes at a time where it can, and bytes past the descriptor's range read as 0.
    Mapped memory isn't cached, so each read goes to memory.
- Push constants are copied in the same way, from what `vkCmdPushConstants` recorded.

## Commands

- Primary command buffers only. Each starts with room for 4096 commands, and 16 KB of data for push constants,
  dynamic offsets and copy regions; each doubles when it fills, and keeps its size through resets. If host memory runs
  out, `vkEndCommandBuffer` returns `VK_ERROR_OUT_OF_HOST_MEMORY`.
- `vkQueueSubmit` replays them on the caller's thread into xsdk's ring.
- `vkCmdCopyBufferToImage` copies whole mip levels, from tightly packed rows or rows `bufferRowLength` texels long, at
  4-byte-aligned offsets, in runs of whole words (`xsdk_texture_upload`). It runs on the CPU at replay, so it waits
  for the GPU to be done with what used the image: the submission that last sampled it, resolved into it or drew it
  back into eDRAM, for the fence that submission ended with; earlier in its own, for the GPU to finish what came
  before. An image no submission used isn't waited for (milestone 12).
- `vkCmdPipelineBarrier` records nothing: replay is in order, and copies wait for what used their image. One that
  would have a sampled depth image read from an attachment fails the command buffer (Render passes).
- `vkCmdDraw` and `vkCmdDrawIndexed`: one instance, `firstInstance` 0.
  - `firstVertex` and `vertexOffset` move each stream's address, since xsdk draws from vertex 0.
  - `firstIndex` moves the index buffer's address. Indices are 16 or 32 bits, with values up to 2^24 - 1.
  - Lists of more than 65535 vertices or indices are split on whole primitives, as the draw packets count in 16 bits
    (xsdk:src/gpu_draw.c:219, 309). Strips and fans that long return `VK_ERROR_FEATURE_NOT_PRESENT`.

## Sync

- Fences are xsdk's fences: `vkQueueSubmit` always ends with one, and a `VkFence` passed to it signals when the GPU
  passes it.
- Semaphores do nothing: there's one queue, and it replays in order.
- `vkWaitForFences` with `waitAll` false waits on the earliest pending fence, since fences signal in submission order.
  A fence never submitted never signals, so waiting on it returns `VK_TIMEOUT` at once.

## Display and swapchain

- One display with one plane and one mode: 1280x720, which the console scales to its output. The mode's refresh rate
  is the output's (`VdQueryVideoMode`). `vkCreateDisplayModeKHR` returns `VK_ERROR_INITIALIZATION_FAILED`.
- Surfaces must be full-screen, opaque and untransformed.
- Surface formats: `VK_FORMAT_B8G8R8A8_UNORM` in `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`; a resolve stores B, G, R, A in
  byte order, as the display reads them (xsdk:docs/HARDWARE.md). Present mode: FIFO.
- One swapchain at a time, of 2 or 3 images: `xsdk_present`'s front buffers, colour attachments only. A second, or
  `oldSwapchain`, returns `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`.
- `vkAcquireNextImageKHR` waits on the CPU for the next buffer to leave the screen, so its semaphore and fence are
  signalled when it returns.
- **Deviation:** one image can be acquired at a time. A second acquire before presenting returns `VK_NOT_READY` with
  a zero timeout and `VK_TIMEOUT` otherwise, though with 3 images Vulkan allows holding 2.
- `vkQueuePresentKHR` queues the acquired image; the vblank callback shows it once the GPU has finished it. Presenting
  any other image returns `VK_ERROR_OUT_OF_DATE_KHR`.
- Without a display controller, as in Xenia, `vkCreateSwapchainKHR` returns `VK_ERROR_INITIALIZATION_FAILED`.

## Boot logo

- `vkCreateDevice` fades the vulkan4xenon logo in over half a second (`xsdk_splash_begin`) and leaves it up.
- The swapchain takes the display over without showing anything of its own, so the logo stays until the first
  present flips. Its buffers are freed a vblank after that flip.
- Without a 1280x720 display, the device works without the logo.

## Formats

| Format | Features |
|---|---|
| `VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_B8G8R8A8_UNORM` | optimal tiling: sampled, linear filtering, colour attachment and blending |
| `VK_FORMAT_D24_UNORM_S8_UINT` | optimal tiling: depth and stencil attachment; sampled, its depth in red, nearest filtering only, as filtering depth hasn't been tried on the console |
| `VK_FORMAT_R32_SFLOAT` | optimal tiling: sampled, nearest filtering only, as filtering 32-bit floats hasn't been tried on the console; buffer: vertex buffer |
| `VK_FORMAT_R8_UNORM`, `VK_FORMAT_R8G8_UNORM` | optimal tiling: sampled, linear filtering |
| `VK_FORMAT_R32G32_SFLOAT`, `R32G32B32_SFLOAT`, `R32G32B32A32_SFLOAT`, `A8B8G8R8_UNORM_PACK32` | buffer: vertex buffer |

Vulkan 1.0 requires many more formats and features; each missing one is a deviation.

## Physical device

Only these limits are set. Every other limit is 0: what it measures isn't implemented yet, such as 1D, 3D and cube
images, storage and texel buffers, input attachments, indirect draws, clip and cull distances, and
compute. Where Vulkan 1.0 has a minimum for one of those, the 0 is below it. `mipmapPrecisionBits` and
`subTexelPrecisionBits` are 0 too, under the minimum of 4: filtering precision hasn't been measured (xsdk:TODO.md).

| Limit | Value |
|---|---|
| `maxImageDimension2D` | 8192 |
| `maxImageArrayLayers` | 1 |
| `maxUniformBufferRange` | 4096 |
| `maxPushConstantsSize` | 128 |
| `maxMemoryAllocationCount` | 4096 |
| `maxSamplerAllocationCount` | 4000 |
| `bufferImageGranularity` | 1 |
| `maxDrawIndexedIndexValue` | 2^24 - 1 |
| `maxBoundDescriptorSets` | 4 |
| `maxPerStageDescriptorSamplers`, `maxPerStageDescriptorSampledImages` | 16 |
| `maxPerStageDescriptorUniformBuffers` | 12 |
| `maxPerStageResources` | 28 |
| `maxDescriptorSetSamplers`, `maxDescriptorSetSampledImages` | 96 |
| `maxDescriptorSetUniformBuffers` | 72 |
| `maxDescriptorSetUniformBuffersDynamic` | 8 |
| `maxVertexInputAttributes`, `maxVertexInputBindings` | 16 |
| `maxVertexInputAttributeOffset` | 1016 |
| `maxVertexInputBindingStride` | 1020 |
| `maxVertexOutputComponents`, `maxFragmentInputComponents` | 64 |
| `maxFragmentOutputAttachments`, `maxColorAttachments` | 1 |
| `maxViewports` | 1 |
| `maxViewportDimensions` | 4096, 4096 |
| `viewportBoundsRange` | -8192, 8191 |
| `subPixelPrecisionBits` | 4 |
| `maxSamplerLodBias` | 15 |
| `maxSamplerAnisotropy` | 16 |
| `minMemoryMapAlignment` | 65536 |
| `minTexelBufferOffsetAlignment`, `minUniformBufferOffsetAlignment`, `minStorageBufferOffsetAlignment` | 256 |
| `maxFramebufferWidth`, `maxFramebufferHeight` | 1280, 720 |
| `maxFramebufferLayers` | 1 |
| `framebufferColorSampleCounts`, `framebufferDepthSampleCounts`, `framebufferStencilSampleCounts`, `sampledImageColorSampleCounts` | 1 |
| `discreteQueuePriorities` | 2 |
| `pointSizeRange`, `lineWidthRange` | 1, 1 |
| `standardSampleLocations` | true |
| `optimalBufferCopyOffsetAlignment`, `optimalBufferCopyRowPitchAlignment` | 1 |
| `nonCoherentAtomSize` | 128 |

Set limits below Vulkan 1.0's minimum:

| Limit | Why |
|---|---|
| `maxVertexInputAttributeOffset` 1016, `maxVertexInputBindingStride` 1020 (2047, 2048) | A fetch's stride is 8 bits of dwords (xsdk:src/shader.c). |
| `maxFragmentOutputAttachments`, `maxColorAttachments` 1 (4) | One colour target until multiple render targets are built (xsdk:TODO.md). |
| `maxFramebufferWidth`, `maxFramebufferHeight` 1280, 720 (4096) | A 1280x720 colour and depth target fills eDRAM (xsdk:SCOPE.md 3.4). |
| `maxFramebufferLayers` 1 (256) | No layered rendering. |
| `maxImageArrayLayers` 1 (256) | No image arrays yet. |
| `maxUniformBufferRange` 4096 (16384) | A block is copied into a stage's 256 float constants of 16 bytes. |
| `maxPerStageResources` 28 (128) | 12 uniform buffers and 16 samplers: a stage's constants and the fetch constants below the vertex streams'. |

`subPixelPrecisionBits` is 4 because vertices land on 1/16 pixel under xsdk's `PA_SU_VTX_CNTL`
(xenia:src/xenia/gpu/xenos.h:877-884). The device is `Xenos (vulkan4xenon)`, an integrated GPU, vendor and device ID
0.
