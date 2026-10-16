#version 450
// The push constants' colour on a front face, blue on a back one, gl_FrontFacing an if's condition, the pixel
// parameters past an interpolator (milestone 22).
layout(location = 0) in vec2 v_corner;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 color; } pc;

void main()
{
	if (gl_FrontFacing)
		o_color = pc.color;
	else
		o_color = vec4(0.1, 0.3, 0.9, 1.0);
}
