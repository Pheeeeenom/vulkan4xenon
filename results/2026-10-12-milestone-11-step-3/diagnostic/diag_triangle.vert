#version 450
// Passes a clip-space position and a colour on to the fragment shader.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;

void main()
{
	gl_Position = a_position;
	v_color = a_color;
}
