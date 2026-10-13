#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(gl_FragCoord.x * 0.00078125, gl_FragCoord.y * 0.0013888889, 0.5, 1.0) * v_color;
}
