#version 450
// Passes a position on and the blit's source region's texture coordinates to the fragment shader, for vkCmdBlitImage
// drawn: the coordinates at the rectangle's top left, then across and down it.
layout(push_constant) uniform Region { vec4 uv; } pc;
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;

void main()
{
	gl_Position = a_position;
	v_uv = pc.uv.xy + a_uv * pc.uv.zw;
}
