#version 450
// The vertex shader's colour times a white texture of the fragment shader's own, so a stage reading the other's
// texture shows (milestone 22).
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(set = 0, binding = 2) uniform sampler2D u_tint;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = v_color * texture(u_tint, v_uv);
}
