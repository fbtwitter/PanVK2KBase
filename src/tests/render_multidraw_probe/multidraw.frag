// The fragment half of tests/render_multidraw_probe. Byte-identical to
// render_push_probe's push.frag - reused deliberately, since pushing a
// different colour per draw is exactly what makes two draws in the same
// render pass independently identifiable in the readback.
#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
   vec4 color;
} pc;

void main()
{
   outColor = pc.color;
}
