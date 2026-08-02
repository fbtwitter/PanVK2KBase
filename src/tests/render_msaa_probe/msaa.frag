// The fragment half of tests/render_msaa_probe. Byte-identical to
// render_vbo_probe's vbo.frag - only the render-pass/pipeline multisample
// and resolve state is under test here, so the fragment side stays exactly
// what already proved correct.
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
   outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
