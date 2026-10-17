#version 450
// The colours vkCmdClearAttachments clears a subpass's colour attachments to, one for each, drawn over the whole
// target with the scissor at its rectangle; the write masks keep the attachments it doesn't clear.
layout(push_constant) uniform Clear { vec4 color0; vec4 color1; vec4 color2; vec4 color3; } pc;
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(location = 2) out vec4 o_color2;
layout(location = 3) out vec4 o_color3;

void main()
{
	o_color0 = pc.color0;
	o_color1 = pc.color1;
	o_color2 = pc.color2;
	o_color3 = pc.color3;
}
