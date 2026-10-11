#version 450
// Scales and moves a unit quad by the vertex range of the push constants.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
}
