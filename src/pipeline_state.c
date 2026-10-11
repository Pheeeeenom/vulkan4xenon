#include "pipeline_state.h"

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/gpu.h"

// A pipeline's fixed-function state as xsdk's state blocks, worked out once (docs/ROADMAP.md, milestone 3).

enum {
	// D24's resolution, the unit of Vulkan's constant depth bias; the hardware's offset is in depth units, which Xenia
	// scales by 2^24 - 1 (xenia:src/xenia/gpu/draw_util.h:165-172).
	DEPTH_BIAS_UNITS = 1 << 24,
	NO_BLEND_FACTOR = 0xFF,
};

static VkResult topology(const VkPipelineInputAssemblyStateCreateInfo *assembly, Pipeline *pipeline)
{
	if (assembly->primitiveRestartEnable)
		return v4x_host_unsupported("primitive restart");
	switch (assembly->topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_POINT_LIST;
		pipeline->list_vertices = 1;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_LINE_LIST;
		pipeline->list_vertices = 2;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		pipeline->primitive = XSDK_PRIMITIVE_LINE_STRIP;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_LIST;
		pipeline->list_vertices = 3;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_STRIP;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_FAN;
		return VK_SUCCESS;
	default:
		return v4x_host_unsupported("adjacency and patch topologies");
	}
}

static VkResult dynamic_state(const VkPipelineDynamicStateCreateInfo *dynamic, Pipeline *pipeline)
{
	for (uint32_t i = 0; dynamic && i < dynamic->dynamicStateCount; i++) {
		if (dynamic->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT)
			pipeline->dynamic_viewport = true;
		else if (dynamic->pDynamicStates[i] == VK_DYNAMIC_STATE_SCISSOR)
			pipeline->dynamic_scissor = true;
		else
			return v4x_host_unsupported("dynamic state other than the viewport and scissor");
	}
	return VK_SUCCESS;
}

static VkResult viewport_state(const VkPipelineViewportStateCreateInfo *state, Pipeline *pipeline)
{
	if (state->viewportCount != 1 || state->scissorCount != 1)
		return v4x_host_unsupported("more than one viewport");
	if (!pipeline->dynamic_viewport)
		pipeline->viewport = state->pViewports[0];
	if (!pipeline->dynamic_scissor)
		pipeline->scissor = state->pScissors[0];
	return VK_SUCCESS;
}

static VkResult raster_state(const VkPipelineRasterizationStateCreateInfo *raster, Pipeline *pipeline)
{
	if (raster->depthClampEnable || raster->rasterizerDiscardEnable || raster->polygonMode != VK_POLYGON_MODE_FILL ||
		raster->cullMode == VK_CULL_MODE_FRONT_AND_BACK || raster->lineWidth != 1.0f ||
		(raster->depthBiasEnable && raster->depthBiasClamp != 0.0f))
		return v4x_host_unsupported("rasterization other than filled, culling one face or none, unclamped bias");
	const XsdkRaster setting = {
		.cull = raster->cullMode == VK_CULL_MODE_FRONT_BIT ? XSDK_CULL_FRONT
			: raster->cullMode == VK_CULL_MODE_BACK_BIT    ? XSDK_CULL_BACK
														   : XSDK_CULL_NONE,
		// Clockwise as it looks in Vulkan's framebuffer, as xsdk measures it (xsdk:tests/test_raster.c:207).
		.front_clockwise = raster->frontFace == VK_FRONT_FACE_CLOCKWISE,
		.depth_bias = raster->depthBiasEnable ? raster->depthBiasConstantFactor / DEPTH_BIAS_UNITS : 0.0f,
		.depth_bias_slope = raster->depthBiasEnable ? raster->depthBiasSlopeFactor : 0.0f,
	};
	xsdk_gpu_raster_state(&pipeline->raster, &setting);
	return VK_SUCCESS;
}

static bool same_stencil(const VkStencilOpState *front, const VkStencilOpState *back)
{
	return front->failOp == back->failOp && front->passOp == back->passOp && front->depthFailOp == back->depthFailOp &&
		front->compareOp == back->compareOp && front->compareMask == back->compareMask &&
		front->writeMask == back->writeMask && front->reference == back->reference;
}

// Vulkan's compare and stencil ops are xsdk's value for value (xsdk:include/xsdk/gpu.h).
static VkResult depth_stencil_state(const VkPipelineDepthStencilStateCreateInfo *state, Pipeline *pipeline)
{
	XsdkDepthStencil setting = { .depth_compare = XSDK_COMPARE_ALWAYS, .stencil_compare = XSDK_COMPARE_ALWAYS };
	if (state) {
		if (state->depthBoundsTestEnable || (state->stencilTestEnable && !same_stencil(&state->front, &state->back)))
			return v4x_host_unsupported("depth bounds, and stencil that differs between faces");
		const VkStencilOpState *const stencil = &state->front;
		setting = (XsdkDepthStencil){
			.depth_test = state->depthTestEnable,
			.depth_write = state->depthTestEnable && state->depthWriteEnable,
			.depth_compare = (uint32_t)state->depthCompareOp,
			.stencil_test = state->stencilTestEnable,
			.stencil_compare = (uint32_t)stencil->compareOp,
			.stencil_fail = (uint32_t)stencil->failOp,
			.stencil_depth_fail = (uint32_t)stencil->depthFailOp,
			.stencil_pass = (uint32_t)stencil->passOp,
			.stencil_reference = (uint8_t)stencil->reference,
			.stencil_read_mask = (uint8_t)stencil->compareMask,
			.stencil_write_mask = (uint8_t)stencil->writeMask,
		};
	}
	xsdk_gpu_depth_stencil_state(&pipeline->depth_stencil, &setting);
	return VK_SUCCESS;
}

static uint32_t blend_factor(VkBlendFactor factor)
{
	switch (factor) {
	case VK_BLEND_FACTOR_ZERO:
		return XSDK_BLEND_ZERO;
	case VK_BLEND_FACTOR_ONE:
		return XSDK_BLEND_ONE;
	case VK_BLEND_FACTOR_SRC_COLOR:
		return XSDK_BLEND_SRC_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return XSDK_BLEND_ONE_MINUS_SRC_COLOR;
	case VK_BLEND_FACTOR_DST_COLOR:
		return XSDK_BLEND_DST_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return XSDK_BLEND_ONE_MINUS_DST_COLOR;
	case VK_BLEND_FACTOR_SRC_ALPHA:
		return XSDK_BLEND_SRC_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
	case VK_BLEND_FACTOR_DST_ALPHA:
		return XSDK_BLEND_DST_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return XSDK_BLEND_ONE_MINUS_DST_ALPHA;
	case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		return XSDK_BLEND_SRC_ALPHA_SATURATE;
	default:
		return NO_BLEND_FACTOR;
	}
}

static uint32_t blend_op(VkBlendOp op)
{
	switch (op) {
	case VK_BLEND_OP_SUBTRACT:
		return XSDK_BLEND_OP_SUBTRACT;
	case VK_BLEND_OP_REVERSE_SUBTRACT:
		return XSDK_BLEND_OP_REVERSE_SUBTRACT;
	case VK_BLEND_OP_MIN:
		return XSDK_BLEND_OP_MIN;
	case VK_BLEND_OP_MAX:
		return XSDK_BLEND_OP_MAX;
	default:
		return XSDK_BLEND_OP_ADD;
	}
}

// Min and max ignore their factors in Vulkan and in Xenia (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324);
// one each makes the result the same either way.
static bool factors_matter(VkBlendOp op)
{
	return op != VK_BLEND_OP_MIN && op != VK_BLEND_OP_MAX;
}

static VkResult blend_state(const VkPipelineColorBlendStateCreateInfo *state, Pipeline *pipeline)
{
	if (state->logicOpEnable || state->attachmentCount != 1)
		return v4x_host_unsupported("logic ops and more than one colour attachment");
	const VkPipelineColorBlendAttachmentState *const attachment = &state->pAttachments[0];
	XsdkBlend setting = {
		.color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.write_mask = attachment->colorWriteMask & XSDK_WRITE_ALL,
	};
	if (attachment->blendEnable) {
		const bool color = factors_matter(attachment->colorBlendOp);
		const bool alpha = factors_matter(attachment->alphaBlendOp);
		setting.color_source = color ? blend_factor(attachment->srcColorBlendFactor) : XSDK_BLEND_ONE;
		setting.color_dest = color ? blend_factor(attachment->dstColorBlendFactor) : XSDK_BLEND_ONE;
		setting.color_op = blend_op(attachment->colorBlendOp);
		setting.alpha_source = alpha ? blend_factor(attachment->srcAlphaBlendFactor) : XSDK_BLEND_ONE;
		setting.alpha_dest = alpha ? blend_factor(attachment->dstAlphaBlendFactor) : XSDK_BLEND_ONE;
		setting.alpha_op = blend_op(attachment->alphaBlendOp);
	}
	if (setting.color_source == NO_BLEND_FACTOR || setting.color_dest == NO_BLEND_FACTOR ||
		setting.alpha_source == NO_BLEND_FACTOR || setting.alpha_dest == NO_BLEND_FACTOR)
		return v4x_host_unsupported("constant colour and dual-source blend factors");
	xsdk_gpu_blend_state(&pipeline->blend, &setting);
	return VK_SUCCESS;
}

VkResult v4x_pipeline_state(const VkGraphicsPipelineCreateInfo *info, const RenderPass *render_pass, Pipeline *pipeline)
{
	const VkPipelineMultisampleStateCreateInfo *const multisample = info->pMultisampleState;
	if (multisample->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT || multisample->sampleShadingEnable ||
		multisample->alphaToCoverageEnable || multisample->alphaToOneEnable)
		return v4x_host_unsupported("multisampling");
	VkResult result = topology(info->pInputAssemblyState, pipeline);
	if (result == VK_SUCCESS)
		result = dynamic_state(info->pDynamicState, pipeline);
	if (result == VK_SUCCESS)
		result = viewport_state(info->pViewportState, pipeline);
	if (result == VK_SUCCESS)
		result = raster_state(info->pRasterizationState, pipeline);
	// Without a depth attachment the depth and stencil state is ignored (Vulkan 1.0 spec,
	// VkGraphicsPipelineCreateInfo).
	if (result == VK_SUCCESS)
		result = depth_stencil_state(render_pass->has_depth ? info->pDepthStencilState : NULL, pipeline);
	if (result == VK_SUCCESS)
		result = blend_state(info->pColorBlendState, pipeline);
	return result;
}
