# Third-party notices

vulkan4xenon is under the MIT licence (LICENSE). It builds on xsdk, whose `libxsdk.a` every title links: xsdk's
THIRD_PARTY_NOTICES.md says what a game built with it ships to meet the notices of the libxenon and Xenia code there.

## In the repository

- `third_party/vulkan-headers/`: the Khronos Group's Vulkan headers, which the layer and the apps built on it include,
  under Apache-2.0 or MIT (`third_party/vulkan-headers/LICENSE.md`).
- `tests/tiled_index.h`: `tiled_index` from libxenon's `drivers/console/console.c`, copyright 2009 Georg Lukas, under
  the 3-clause BSD licence; the file carries the notice. It's in the test title only, not the layer.
