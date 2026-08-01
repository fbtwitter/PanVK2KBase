// The vertex half of tests/render_ubo_probe. Byte-identical to
// render_push_probe's push.vert and render_vbo_probe's vbo.vert - vertex
// fetch is already proven, so nothing changes here. Only ubo.frag differs,
// which is the one new variable this probe tests.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
