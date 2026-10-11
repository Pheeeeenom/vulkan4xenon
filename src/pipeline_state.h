#ifndef V4X_PIPELINE_STATE_H
#define V4X_PIPELINE_STATE_H

#include <vulkan/vulkan.h>

#include "objects.h"

// Fills the pipeline's topology, viewport, scissor, raster, depth and stencil, and blend state from its create info,
// or returns why it can't.
VkResult v4x_pipeline_state(
	const VkGraphicsPipelineCreateInfo *info, const RenderPass *render_pass, Pipeline *pipeline);

#endif
