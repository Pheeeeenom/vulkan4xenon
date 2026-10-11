# Code guidelines

Follow xsdk's CODE_GUIDELINES.md (Z:\Xbox-360-SDK\CODE_GUIDELINES.md) for all code here, with these differences. If a
rule gets in the way of correct code, change the rule in its own commit first.

## Naming

- Vulkan entry points keep their spec names (`vkCreateDevice`).
- Every other non-`static` symbol starts with `v4x_`, then the module (`v4x_queue_replay` in `queue.c`).

## Errors

- Entry points return the `VkResult` the spec allows for the failure. Something not implemented returns
  `VK_ERROR_FEATURE_NOT_PRESENT` and logs one line saying what.
- Calls into xsdk that return `NTSTATUS` are checked; the failure is logged with its status as `0x%08X`.

## Dependencies

- xsdk's library and headers, and the Khronos Vulkan headers vendored in `third_party/vulkan-headers/` (Apache-2.0).
  Anything else needs a reason in the commit message.
- Host tools are Python 3.11 or later, standard library only, except the offline shader step, which runs glslang and
  xost.

## Vulkan behavior

As vulkan4vita's (vulkan4vita:CODE_GUIDELINES.md:131-138):
- Follow the Vulkan spec for everything implemented. Where Xenos or xsdk makes that impossible, document the deviation
  in docs/SUPPORT.md.
- Don't advertise anything that isn't implemented. Features, limits and formats reported to the app match reality.
- Respect external synchronization: the app synchronizes what the spec says it must, and the layer adds no locks for
  it. Internal shared state is protected, and the comment says how.
- docs/SUPPORT.md lists what's implemented: entry points, features, formats and limits. Update it in the same commit
  as the code.

## The hot path

Recording commands, replaying them at `vkQueueSubmit`, and presenting. It has no locks, no logging except when
returning an error, and no heap allocation; command buffers and pools allocate when created or reset.
