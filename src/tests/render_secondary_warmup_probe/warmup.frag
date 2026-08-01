// The fragment half of tests/render_secondary_warmup_probe. Unchanged from
// render_vbo_probe's vbo.frag - a fixed magenta output, already proven.
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
   outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
