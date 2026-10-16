#version 450
// Passes a position on, and a coordinate of up to four components to the fragment shader.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_coord;
layout(location = 0) out vec4 v_coord;

void main()
{
	gl_Position = a_position;
	v_coord = a_coord;
}
