#ifndef V4X_PIPELINE_STATE_H
#define V4X_PIPELINE_STATE_H

#include <vulkan/vulkan.h>

#include "objects.h"

// Fills the pipeline's topology, viewport, scissor, raster, depth and stencil, and blend state from its create info,
// or returns why it can't.
VkResult v4x_pipeline_state(
	const VkGraphicsPipelineCreateInfo *info, const RenderPass *render_pass, Pipeline *pipeline);
// The raster, and the depth and stencil, a draw with the pipeline takes: its own blocks, or, where it takes state from
// commands, blocks worked out again into scratch with the dynamic state's values.
const XsdkRasterState *v4x_pipeline_raster(
	const Pipeline *pipeline, const DynamicState *dynamic, XsdkRasterState *scratch);
const XsdkDepthStencilState *v4x_pipeline_depth_stencil(
	const Pipeline *pipeline, const DynamicState *dynamic, XsdkDepthStencilState *scratch);
// The colour its constant blend factors take, its own or the dynamic state's; NULL if its blend takes none.
const float *v4x_pipeline_blend_constants(const Pipeline *pipeline, const DynamicState *dynamic);

#endif
