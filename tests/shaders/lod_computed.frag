#version 450
// textureLod at a LOD each pixel works out: its band's number, a quarter past it, whichever way the GPU rounds.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_band;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = textureLod(u_texture, v_uv, floor(v_band.x) + 0.25);
}
