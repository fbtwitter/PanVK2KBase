// The vertex half of tests/render_triangle_probe.
//
// No vertex buffers - positions are hardcoded and indexed by gl_VertexIndex,
// the standard fullscreen/no-input-state trick, so this stays the smallest
// possible real draw: no VkBuffer, no vertex input state, nothing beyond
// vkCmdDraw(3, 1, 0, 0).
//
// The triangle deliberately does not cover the whole viewport - see the .c
// file for why partial coverage is the point.
#version 450

vec2 positions[3] = vec2[](
   vec2(-0.8, -0.8),
   vec2( 0.8, -0.8),
   vec2(-0.8,  0.8)
);

void main()
{
   gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
