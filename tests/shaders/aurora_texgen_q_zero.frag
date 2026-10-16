#version 450
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec3 in_tex0_uvw;
layout(location = 0) out vec4 o_color;
void main() {
    float tex0_near = step(abs(in_tex0_uvw.z), 1e-6);
    vec2 tex0_uv = in_tex0_uvw.xy / mix(in_tex0_uvw.z, 1.0, tex0_near);
    vec4 sampled0 = texture(tex0, tex0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (sampled0.rgb), (in_cc0.rgb)) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (0.0), (0.0)) + in_cc0.a)), 0.0, 1.0);
    o_color = vec4(s0_color, s0_alpha);
}
