#version 450
// A back face discarded, as one-sided foliage is; a front face the push constants' colour (milestone 22).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 color; } pc;

void main()
{
	if (!gl_FrontFacing)
		discard;
	o_color = pc.color;
}
