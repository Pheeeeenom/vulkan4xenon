#version 450
// Samples the image bound at set 0, binding 0, scaled and moved by the fragment range of the push constants, so values
// below 0 show.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { layout(offset = 48) vec4 scale; vec4 bias; } pc;

void main()
{
	o_color = texture(u_texture, v_uv) * pc.scale + pc.bias;
}
