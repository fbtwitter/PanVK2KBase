// The fragment half of tests/render_triangle_probe.
//
// Hardcoded output colour rather than a push constant or UBO - no
// descriptor sets needed, keeping this the smallest possible real draw on
// top of tests/render_clear_probe's already-proven render-pass entry.
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
   /* Magenta: 1.0/0.0 components map exactly to 0xff/0x00 in UNORM, so the
    * readback check can compare bytes exactly rather than tolerate
    * rounding.
    */
   outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
