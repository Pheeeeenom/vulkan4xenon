#version 450
// Writes the colour in the fragment range of the push constants.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 color; } pc;

void main()
{
	o_color = pc.color;
}
