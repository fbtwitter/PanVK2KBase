// The fragment half of tests/render_vbo_probe. Unchanged from
// render_triangle_probe's triangle.frag - only the vertex side is under
// test here, so the fragment side stays exactly what already proved
// correct.
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
   outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
