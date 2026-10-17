#version 450
// push.vert's quad, cut by two planes from a uniform buffer: gl_ClipDistance[1] and [4], the position through a local.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;
layout(set = 0, binding = 0) uniform Planes { vec4 planes[2]; } u;

void main()
{
	vec4 position = a_position * pc.scale + pc.offset;
	gl_Position = position;
	gl_ClipDistance[1] = dot(position, u.planes[0]);
	gl_ClipDistance[4] = dot(u.planes[1], position);
}
