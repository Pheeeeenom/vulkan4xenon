<div align="center">
  <img src="assets/vulkan4xenon.png" alt="Vulkan 4 xenon" width="640">
</div>

# vulkan4xenon

A Vulkan 1.0 subset for the Xbox 360, built as a static library on the xsdk homebrew SDK's GPU layer, which drives
Xenos directly under the retail kernel. It follows the layout of vulkan4vita, which does the same on the PS Vita's
GXM.

The goal is running Dusklight, a reimplementation of Twilight Princess built on encounter's Aurora, on an RGH console.

**Status:** being scoped. docs/ROADMAP.md is the plan, and docs/SUPPORT.md will list what exists as it's built.

## Shaders

Apps pass standard SPIR-V to `vkCreateShaderModule`. Shaders are compiled offline by xost into Xenos containers,
bundled with the app and looked up by their SPIR-V.

## Trademark

The Vulkan logo is a registered trademark of the Khronos Group, used here at the project owner's choice. vulkan4xenon
doesn't claim conformance.
