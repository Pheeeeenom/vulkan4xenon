#version 450
// textureLod at two constant LODs, 1.25 left of the strip's middle, which the fetch's own field holds, and 6.25 right
// of it, which it doesn't.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_band;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = mix(textureLod(u_texture, v_uv, 6.25), textureLod(u_texture, v_uv, 1.25), step(v_band.x, 4.0));
}
