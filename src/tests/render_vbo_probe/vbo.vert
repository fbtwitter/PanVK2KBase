// The vertex half of tests/render_vbo_probe.
//
// Unlike triangle.vert (render_triangle_probe), position is a real vertex
// attribute fetched from a bound VkBuffer, not a hardcoded array indexed by
// gl_VertexIndex. That is the one thing this probe changes relative to the
// already-proven render_triangle_probe.
#version 450

layout(location = 0) in vec2 inPosition;

void main()
{
   gl_Position = vec4(inPosition, 0.0, 1.0);
}
