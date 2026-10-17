#version 450
// push.vert's quad, cut by a plane in the push constants: gl_ClipDistance[0] is its distance from it.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; layout(offset = 48) vec4 plane; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	gl_ClipDistance[0] = dot(gl_Position, pc.plane);
}
