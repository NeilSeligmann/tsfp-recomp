#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fragment stage of gpu_vsh_render (T100d). The translated vertex shader writes the
 * diffuse colour oD0 at location 0 (tools/nv2a/translate.py VARYINGS), this writes it to
 * the target unchanged, so a pixel is a statement about the vertex stage only.
 */
layout(location = 0) in vec4 diffuse;
layout(location = 0) out vec4 fragment_colour;

void main(void)
{
    fragment_colour = diffuse;
}
