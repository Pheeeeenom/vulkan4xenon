#version 450
// Writes a colour to each of four colour attachments from the fragment range of the push constants, the last a float
// alone, as an R32_SFLOAT attachment takes (milestone 25).
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(location = 2) out vec4 o_color2;
layout(location = 3) out float o_color3;
layout(push_constant) uniform Push {
	layout(offset = 32) vec4 color0;
	vec4 color1;
	vec4 color2;
	vec4 color3;
} pc;

void main()
{
	o_color0 = pc.color0;
	o_color1 = pc.color1;
	o_color2 = pc.color2;
	o_color3 = pc.color3.x;
}
