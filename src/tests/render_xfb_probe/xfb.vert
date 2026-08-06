// The vertex half of tests/render_xfb_probe.
//
// One variable changed from render_vbo_probe's vbo.vert: this vertex
// shader also writes its clip-space position to an XFB-captured output
// (VK_EXT_transform_feedback), in addition to the normal gl_Position
// write used for rasterization. A passing readback of the XFB buffer
// should hold byte-identical clip-space positions to what got
// rasterized - that's what proves capture actually happened rather than
// the draw merely completing.
#version 450

layout(location = 0) in vec2 inPosition;

layout(location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0) out vec4 xfbPosition;

void main()
{
   vec4 pos = vec4(inPosition, 0.0, 1.0);
   gl_Position = pos;
   xfbPosition = pos;
}
