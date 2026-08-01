// The vertex half of tests/render_texture_probe. Byte-identical to every
// earlier probe's vertex shader - vertex fetch is already proven, so
// nothing changes here. Only texture.frag differs, which is the one new
// variable this probe tests.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
