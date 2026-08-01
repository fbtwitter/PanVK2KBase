// The fragment half of tests/render_depth_probe. Byte-identical to every
// earlier probe's hardcoded-magenta fragment shader - colour output is not
// under test here, only the depth attachment/test/write path is.
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
   outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
