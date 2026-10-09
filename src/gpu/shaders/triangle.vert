#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The three vertices live in the shader rather than in a vertex buffer. The
 * point of this shader is to prove the pipeline rasterises and interpolates at
 * all, so dragging a buffer, an allocation and an input-assembly description
 * into the proof would only add ways for it to fail for unrelated reasons.
 */
layout(location = 0) out vec3 vertex_colour;

/* Vulkan clip space has +Y pointing DOWN, so the negative-Y vertex is the apex
 * at the top of the image. Getting this backwards renders an upside-down
 * triangle that still passes a "something was drawn" check, which is exactly
 * the kind of pass nobody looks at. */
const vec2 positions[3] = vec2[3](
    vec2( 0.0, -0.7),
    vec2( 0.7,  0.7),
    vec2(-0.7,  0.7)
);

/* One pure channel per corner. A flat colour would prove the raster but not
 * that per-vertex attributes reach the fragment stage. */
const vec3 colours[3] = vec3[3](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main(void)
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertex_colour = colours[gl_VertexIndex];
}
