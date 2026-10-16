#version 450
// instanced.vert's grid without gl_InstanceIndex: each instance's column and row its own attribute (milestone 24).
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_place;
layout(location = 0) out vec4 v_color;

void main()
{
	gl_Position = vec4((a_place + vec2(0.2) + a_corner * 0.6) * vec2(0.25, 1.0 / 3.0) - vec2(1.0), 0.0, 1.0);
	v_color = a_color;
}
