#version 450
// Samples the image being drawn back into eDRAM.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_image;

void main()
{
	o_color = texture(u_image, v_uv);
}
