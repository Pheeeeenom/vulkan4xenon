#version 450
// A loop a uniform buffer counts, with an if/else on the pixel's u, its colour's red, in its pass, a block's array at
// the pass's index and the index as a float, and a break once what it sums passes a limit (tests/test_loops.c).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Loop { vec4 colors[8]; vec4 limit; int count; } u;

void main()
{
	vec4 seen = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 sum = vec4(0.0);
	for (int i = 0; i < u.count; i++) {
		if (v_color.r > float(i) * 0.125)
			seen.rgb += u.colors[i].rgb;
		else
			seen.b += 0.0625;
		sum += u.colors[i];
		if (sum.a >= u.limit.a)
			break;
	}
	o_color = seen;
}
