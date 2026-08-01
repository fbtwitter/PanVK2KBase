// The fragment half of tests/render_push_probe.
//
// Unlike vbo.frag (render_vbo_probe), the output colour is not hardcoded -
// it comes from a push constant. That is the one new variable this probe
// tests: push constant plumbing through the graphics pipeline (compute
// already proved it works for compute, in driver_pipeline_probe - this is
// untested for graphics specifically).
#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
   vec4 color;
} pc;

void main()
{
   outColor = pc.color;
}
