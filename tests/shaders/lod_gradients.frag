#version 450
// textureGrad with gradients each pixel works out, a 256-texel texture's LOD the band's number a quarter past it.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_band;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) out vec4 o_color;

void main()
{
	float step = exp2(floor(v_band.x) + 0.25) / 256.0;
	o_color = textureGrad(u_texture, v_uv, vec2(step, 0.0), vec2(0.0, step));
}
