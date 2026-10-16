#version 450
// A quad an instance in a grid of cells 160 by 120 pixels, 8 across and 6 down (milestone 24): its column
// gl_InstanceIndex, its row and colour the instance's own attributes.
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_place;
layout(location = 0) out vec4 v_color;

void main()
{
	vec2 cell = vec2(float(gl_InstanceIndex), a_place.y);
	gl_Position = vec4((cell + vec2(0.2) + a_corner * 0.6) * vec2(0.25, 1.0 / 3.0) - vec2(1.0), 0.0, 1.0);
	v_color = a_color;
}
