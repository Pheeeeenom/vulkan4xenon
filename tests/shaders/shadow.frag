#version 450
// Compares the depth image bound at set 0, binding 0, at the coordinate's xy against the reference in its z, by the
// sampler's compare op.
layout(location = 0) in vec4 v_coord;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2DShadow u_shadow;

void main()
{
	o_color = vec4(texture(u_shadow, v_coord.xyz));
}
