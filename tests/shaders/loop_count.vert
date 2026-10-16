#version 450
// A vertex shader's loop a dynamic uniform buffer counts, moving its quad right and adding to its colour a pass
// (tests/test_loops.c).
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Loop { vec4 scale; vec4 offset; vec4 step; vec4 color; int count; } u;

void main()
{
	vec4 p = a_position * u.scale + u.offset;
	vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < u.count; i++) {
		p.x += u.step.x;
		c.rgb += u.color.rgb;
	}
	gl_Position = p;
	v_color = c;
}
