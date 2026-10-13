#version 450
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_extra;

void main()
{
    gl_Position = a_position;
    v_color = a_color;
    v_extra = vec4(0.25);
}
