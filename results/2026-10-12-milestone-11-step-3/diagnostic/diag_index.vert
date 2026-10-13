#version 450
layout(location = 0) in vec4 a_position;
layout(location = 2) in float a_band;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_band;

void main()
{
    gl_Position = a_position;
    v_color = vec4(1.0);
    v_band = vec4(a_band);
}
