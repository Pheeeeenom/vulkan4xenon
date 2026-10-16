#version 450
// An if/else on data: each vertex's kind, its colour's w, moves its quad right or down and picks its colour, one arm
// with an if of its own; then an && glslang makes a Phi of greens a quad that ends at the bottom right
// (tests/test_branches.c).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 tint; } pc;

void main()
{
	vec4 p = a_position;
	vec4 c;
	if (a_color.w > 0.5) {
		p.x += 0.5;
		c = vec4(a_color.rgb, 1.0);
		if (a_color.w > 0.875)
			c = pc.tint;
	} else {
		p.y += 0.5;
		c = vec4(vec3(1.0) - a_color.rgb, 1.0);
	}
	if (p.x > 0.0 && p.y > 0.0)
		c.g = 1.0;
	gl_Position = p;
	v_color = c;
}
