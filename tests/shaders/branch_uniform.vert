#version 450
// An if/else on a push constant, every vertex taking the same arm: right in its colour's swizzle, or down in its colour
// (tests/test_branches.c).
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 color; float mode; } pc;

void main()
{
	vec4 p = a_position * pc.scale + pc.offset;
	if (pc.mode > 0.5) {
		p.x += 0.5;
		v_color = pc.color.bgra;
	} else {
		p.y += 0.5;
		v_color = pc.color;
	}
	gl_Position = p;
}
