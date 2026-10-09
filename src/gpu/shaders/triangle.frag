#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later */
layout(location = 0) in vec3 vertex_colour;
layout(location = 0) out vec4 fragment_colour;

void main(void)
{
    fragment_colour = vec4(vertex_colour, 1.0);
}
