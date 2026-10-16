#version 450
// A loop a push constant counts, a step of colour a pass, as a hardware loop the layer counts from the push constants
// (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < pc.count; i++)
		sum.rgb += pc.step.rgb;
	o_color = sum;
}
