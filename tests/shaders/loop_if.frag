#version 450
// An if/else on data in an unrolled loop's body: the pixel's u, its colour's red, against each pass's threshold
// (tests/test_branches.c).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < 4; i++) {
		if (v_color.r > float(i) * 0.25)
			sum.rgb += pc.step.rgb;
		else
			sum.b += 0.125;
	}
	o_color = sum;
}
