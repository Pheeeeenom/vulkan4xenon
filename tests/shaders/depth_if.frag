#version 450
// gl_FragDepth stored in both arms of an if/else on the pixel's u, its colour's red (tests/test_branches.c).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(1.0, 0.0, 0.0, 1.0);
	if (v_color.r > 0.5)
		gl_FragDepth = 0.25;
	else
		gl_FragDepth = 0.75;
}
