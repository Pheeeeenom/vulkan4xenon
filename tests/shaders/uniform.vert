#version 450
// Places a unit quad with a matrix and passes a colour on, both from a dynamic uniform buffer.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Draw { mat4 transform; vec4 color; } draw;

void main()
{
	gl_Position = draw.transform * a_position;
	v_color = draw.color;
}
