#version 450
// Samples the texture bound at set 0, binding 0, scaled by the push constants, so values past 1 can be shown
// (milestone 25).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 scale; } pc;

void main()
{
	o_color = texture(u_texture, v_uv) * pc.scale;
}
