#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_band;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform BandsByInterpolantCf { vec4 colors[10]; } u;

void main()
{
    o_color = u.colors[int(v_band.x + 0.5)] * v_color;
}
