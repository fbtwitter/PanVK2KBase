// The vertex half of tests/render_depth_probe. Byte-identical to every
// earlier probe's vertex shader - gl_Position.z is already 0.0 here, which
// is exactly what this probe needs, so nothing changes. Only the pipeline
// (a real depth attachment, depth test and write enabled) and the C file
// (creating and reading back that attachment) differ.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
