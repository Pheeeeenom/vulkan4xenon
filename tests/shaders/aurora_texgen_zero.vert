#version 450
layout(set = 0, binding = 0, std140) uniform View {
    vec4 viewport_sizes; // render, then logical
    vec4 line_params;
    vec4 proj[4];
    vec4 tex_mtx[30];
    vec4 channel_colors[8]; // per channel: ambient, material, alpha ambient, alpha material
    vec4 postmtx[60];
    vec4 fog[5];
    vec4 ind_mtx[6];
} uview;
layout(push_constant, std140) uniform Push {
    vec4 pos_mtx[3];
    vec4 nrm_mtx[3];
    vec4 line_mask[2];
} upush;
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_corner;
layout(location = 0) out vec4 out_cc0;
layout(location = 1) out vec3 out_tex0_uvw;
void main() {
    vec4 pos4 = vec4(in_pos, 1.0);
    vec4 mv_pos4 = vec4(vec3(dot(upush.pos_mtx[0], pos4), dot(upush.pos_mtx[1], pos4), dot(upush.pos_mtx[2], pos4)), 1.0);
    vec4 clip_pos = vec4(dot(uview.proj[0], mv_pos4), dot(uview.proj[1], mv_pos4), dot(uview.proj[2], mv_pos4), dot(uview.proj[3], mv_pos4));
    vec2 viewport_scale = uview.viewport_sizes.xy / max(uview.viewport_sizes.zw, vec2(1.0, 1.0));
    float line_width = uview.line_params.x * min(viewport_scale.x, viewport_scale.y);
    vec2 corner_offset = in_corner * line_width / uview.viewport_sizes.xy;
    gl_Position = vec4(clip_pos.xy + corner_offset * clip_pos.w, clip_pos.zw) * vec4(1.0, -1.0, 1.0, 1.0);
    vec4 cc0 = vec4(uview.channel_colors[1].rgb, uview.channel_colors[3].a);
    out_cc0 = cc0;
    vec4 tc0 = vec4(in_pos, 1.0);
    vec3 tc0_tmp = tc0.xyz;
    float tc0_len2 = dot(tc0_tmp, tc0_tmp);
    vec3 tc0_norm = tc0_tmp * mix(1.0, inversesqrt(max(tc0_len2, 1e-10)), step(1e-10, tc0_len2));
    float tc0_q0 = step(abs(tc0_norm.z), 0.0);
    vec3 tc0_q = mix(tc0_norm, vec3(clamp(tc0_norm.xy * 0.5, -1.0, 1.0), 0.0), tc0_q0);
    vec3 tc0_line = tc0_q + vec3(in_corner * 0.5 + 0.5, 0.0) * (uview.line_params.z * upush.line_mask[0].x);
    out_tex0_uvw = tc0_line;
}
