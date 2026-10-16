#version 450
// 16 samplers in a fragment shader, Vulkan's minimum a stage, at bindings 16 up (milestone 22).
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 16) uniform sampler2D u_0;
layout(set = 0, binding = 17) uniform sampler2D u_1;
layout(set = 0, binding = 18) uniform sampler2D u_2;
layout(set = 0, binding = 19) uniform sampler2D u_3;
layout(set = 0, binding = 20) uniform sampler2D u_4;
layout(set = 0, binding = 21) uniform sampler2D u_5;
layout(set = 0, binding = 22) uniform sampler2D u_6;
layout(set = 0, binding = 23) uniform sampler2D u_7;
layout(set = 0, binding = 24) uniform sampler2D u_8;
layout(set = 0, binding = 25) uniform sampler2D u_9;
layout(set = 0, binding = 26) uniform sampler2D u_10;
layout(set = 0, binding = 27) uniform sampler2D u_11;
layout(set = 0, binding = 28) uniform sampler2D u_12;
layout(set = 0, binding = 29) uniform sampler2D u_13;
layout(set = 0, binding = 30) uniform sampler2D u_14;
layout(set = 0, binding = 31) uniform sampler2D u_15;

void main()
{
	vec2 uv = vec2(0.5);
	o_color = texture(u_0, uv) + texture(u_1, uv) + texture(u_2, uv) + texture(u_3, uv) +
		texture(u_4, uv) + texture(u_5, uv) + texture(u_6, uv) + texture(u_7, uv) +
		texture(u_8, uv) + texture(u_9, uv) + texture(u_10, uv) + texture(u_11, uv) +
		texture(u_12, uv) + texture(u_13, uv) + texture(u_14, uv) + texture(u_15, uv);
}
