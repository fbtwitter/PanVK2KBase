// The fragment half of tests/render_ubo_probe.
//
// One variable changed from render_push_probe's push.frag: the colour now
// comes from a uniform buffer through a real descriptor set, not a push
// constant. Descriptor sets are the last basic plumbing mechanism this
// port had not exercised - the piece needed before anything resembling a
// real shader (a sampled texture, a UBO of transform matrices) becomes
// possible.
#version 450

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform ColorBlock {
   vec4 color;
} ubo;

void main()
{
   outColor = ubo.color;
}
