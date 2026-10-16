#version 450
// Textures read in a vertex shader, as terrain reads its heights: the height map at the vertex's coordinate moves it
// up, a height of 1 by 255 pixels, and a texture's level at a LOD the vertex gives colours it where a_level.y is 1,
// its own colour elsewhere (milestone 22).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec2 a_level;
layout(set = 0, binding = 0) uniform sampler2D u_height;
layout(set = 0, binding = 1) uniform sampler2D u_levels;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main()
{
	float height = texture(u_height, a_uv).x;
	gl_Position = a_position - vec4(0.0, height * (255.0 / 360.0), 0.0, 0.0);
	v_color = mix(a_color, textureLod(u_levels, a_uv, a_level.x), a_level.y);
	v_uv = a_uv;
}
