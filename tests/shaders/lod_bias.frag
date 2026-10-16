#version 450
// texture() biased by an amount each pixel works out, from the LOD 2 the strip is drawn at to its band's number.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_band;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = texture(u_texture, v_uv, floor(v_band.x) - 1.75);
}
