#version 450
// A strip of bands, each its own level: the position, the texture coordinate and the band's number, which runs from 0
// at the left edge to the band count at the right (milestone 22).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec2 a_band;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_band;

void main()
{
	gl_Position = a_position;
	v_uv = a_uv;
	v_band = a_band;
}
