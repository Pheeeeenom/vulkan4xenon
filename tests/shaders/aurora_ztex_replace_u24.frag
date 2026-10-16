#version 450
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec2 in_tex1_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex1_uv = in_tex1_uv;
    vec4 sampled0 = texture(tex0, tex1_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (sampled0.rgb), (in_cc0.rgb)) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (0.0), (0.0)) + in_cc0.a)), 0.0, 1.0);
    o_color = vec4(s0_color, s0_alpha);
    // Z texture (replace)
    vec3 zt_t = floor(sampled0.rgb * 255.0 + 0.5);
    float zt_tex = zt_t.r * 65536.0 + zt_t.g * 256.0 + zt_t.b;
    float zt_sum = 0.0 + zt_tex + 291.0;
    float zt_z = zt_sum - floor(zt_sum * (1.0 / 16777216.0)) * 16777216.0;
    gl_FragDepth = 1.0 - zt_z * (1.0 / 16777215.0);
}
