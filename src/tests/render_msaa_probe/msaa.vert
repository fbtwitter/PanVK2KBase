// The vertex half of tests/render_msaa_probe. Byte-identical to
// render_vbo_probe's vbo.vert - vertex fetch is already proven, so nothing
// changes here. Sample count and resolve are entirely render-pass/pipeline
// state on the C side; the shader itself has no reason to differ.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
