// The fragment half of tests/render_texture_probe.
//
// One variable changed from render_ubo_probe's ubo.frag: the colour comes
// from sampling a texture through a combined image sampler, not reading a
// uniform buffer. This is a genuinely different hardware path from
// render_ubo_probe - image sampling, not just a buffer descriptor - so it
// gets the same from-scratch caution as every other first-time mechanism
// this session, not an assumption that "descriptor sets work" implies
// "texture sampling works."
//
// The texture is 1x1 on purpose: texture() on a 1x1 image returns that one
// texel regardless of UV (wrapped or clamped, it is the only texel there
// is), so the UV passed here does not need to be correct for this to be a
// valid test - it isolates "does sampling a bound texture work at all"
// from "is UV math correct," which would otherwise be two unknowns tested
// at once.
#version 450

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main()
{
   outColor = texture(tex, vec2(0.5, 0.5));
}
