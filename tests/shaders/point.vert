#version 450
// A point as wide as its push constants say, moved to their offset: gl_PointSize is its width in pixels.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 offset; float size; } pc;

void main()
{
	gl_Position = a_position + pc.offset;
	gl_PointSize = pc.size;
}
