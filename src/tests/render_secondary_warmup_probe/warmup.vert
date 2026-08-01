// The vertex half of tests/render_secondary_warmup_probe. Identical to
// render_vbo_probe's vbo.vert - vertex-buffer-fetched position is already
// proven, and is not what this probe is testing (secondary command
// buffers + indirect draws are).
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
