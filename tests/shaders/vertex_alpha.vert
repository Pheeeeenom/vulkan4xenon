#version 450
// vertex_format.vert, the colour's alpha in red, green and blue.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 color_scale; vec4 color_bias; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	v_color = a_color.wwww * pc.color_scale + pc.color_bias;
}
