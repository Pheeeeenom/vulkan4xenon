#version 450
// Colours each 128-pixel band of the screen from a uniform array, picked by the pixel's column, as range fog picks its
// factors (milestone 11): gl_FragCoord from the pixel parameters, then a read at a run-time index.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Bands { vec4 colors[10]; } u;

void main()
{
    o_color = u.colors[int(gl_FragCoord.x * 0.0078125)] * v_color;
}
