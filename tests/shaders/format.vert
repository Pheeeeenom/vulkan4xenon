#version 450
// Scales and moves a unit quad by the vertex range of the push constants; its texture coordinates are its corner's,
// moved as far again, so a pixel's centre samples where a test wants it.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec2 v_uv;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 uv_offset; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	v_uv = a_position.xy + pc.uv_offset.xy;
}
