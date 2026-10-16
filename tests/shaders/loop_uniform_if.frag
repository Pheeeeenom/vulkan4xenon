#version 450
// An if/else on a push constant in an unrolled loop's body, each pass's arms in execs on the predicate
// (tests/test_branches.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; float mode; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < 3; i++) {
		if (pc.mode > 0.5)
			sum.rgb = sum.rgb * 0.5 + pc.step.rgb;
		else
			sum.b += 0.25;
	}
	o_color = sum;
}
