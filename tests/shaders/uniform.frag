#version 450
// Tints the colour by a uniform buffer in the second set.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 0) uniform Tint { vec4 tint; } t;

void main()
{
	o_color = v_color * t.tint;
}
