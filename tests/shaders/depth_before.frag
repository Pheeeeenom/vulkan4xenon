#version 450
// gl_FragDepth stored, then again in one arm of an if/else on the pixel's v, its colour's green, with the colour
// (tests/test_branches.c).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(1.0, 0.0, 0.0, 1.0);
	gl_FragDepth = 0.75;
	if (v_color.g > 0.5) {
		gl_FragDepth = 0.25;
		o_color = vec4(0.0, 0.0, 1.0, 1.0);
	}
}
