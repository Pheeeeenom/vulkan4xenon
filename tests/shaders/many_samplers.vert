#version 450
// 16 samplers in a vertex shader, at bindings 0 up, each read at LOD 0 (milestone 22).
layout(location = 0) in vec4 a_position;
layout(set = 0, binding = 0) uniform sampler2D u_0;
layout(set = 0, binding = 1) uniform sampler2D u_1;
layout(set = 0, binding = 2) uniform sampler2D u_2;
layout(set = 0, binding = 3) uniform sampler2D u_3;
layout(set = 0, binding = 4) uniform sampler2D u_4;
layout(set = 0, binding = 5) uniform sampler2D u_5;
layout(set = 0, binding = 6) uniform sampler2D u_6;
layout(set = 0, binding = 7) uniform sampler2D u_7;
layout(set = 0, binding = 8) uniform sampler2D u_8;
layout(set = 0, binding = 9) uniform sampler2D u_9;
layout(set = 0, binding = 10) uniform sampler2D u_10;
layout(set = 0, binding = 11) uniform sampler2D u_11;
layout(set = 0, binding = 12) uniform sampler2D u_12;
layout(set = 0, binding = 13) uniform sampler2D u_13;
layout(set = 0, binding = 14) uniform sampler2D u_14;
layout(set = 0, binding = 15) uniform sampler2D u_15;

void main()
{
	vec2 uv = a_position.xy;
	vec4 sum = texture(u_0, uv) + texture(u_1, uv) + texture(u_2, uv) + texture(u_3, uv) +
		texture(u_4, uv) + texture(u_5, uv) + texture(u_6, uv) + texture(u_7, uv) +
		texture(u_8, uv) + texture(u_9, uv) + texture(u_10, uv) + texture(u_11, uv) +
		texture(u_12, uv) + texture(u_13, uv) + texture(u_14, uv) + texture(u_15, uv);
	gl_Position = a_position + sum * 0.001;
}
