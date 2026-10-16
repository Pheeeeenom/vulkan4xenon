#version 450
// push.vert's quad, with the unit quad's corner as an interpolator, so the pixel parameters land past one, as in most
// shaders (milestone 22).
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec2 v_corner;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	v_corner = a_position.xy;
}
