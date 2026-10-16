#version 450
// The colour vkCmdClearAttachments clears to, drawn over the whole target with the scissor at its rectangle.
layout(push_constant) uniform Clear { vec4 color; } pc;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = pc.color;
}
