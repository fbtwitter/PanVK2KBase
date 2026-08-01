// The vertex half of tests/render_push_probe. Byte-identical to
// render_vbo_probe's vbo.vert - vertex fetch is already proven, so nothing
// changes here. Only push.frag differs from render_vbo_probe's shaders,
// which is the one new variable this probe tests.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
