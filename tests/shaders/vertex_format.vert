#version 450
// Scales and moves a position by the push constants, and passes the colour on, scaled and moved by them too, so values
// past 0 to 1 show.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 color_scale; vec4 color_bias; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	v_color = a_color * pc.color_scale + pc.color_bias;
}
