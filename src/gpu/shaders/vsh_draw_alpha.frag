#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fragment stage of gpu_vsh_render with an ALPHA TEST (T267): vsh_draw.frag plus the NV2A alpha test,
 * the comparison of the fragment's alpha with a reference. The two constants are specialisation
 * constants, so one module serves every function and reference and the pipeline carries them.
 *
 *   alpha_func   0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GEQUAL, 7 ALWAYS
 *                (the NV2A enumeration 0x200 + n, and OpenGL's)
 *   alpha_ref    0 .. 255
 *
 * The fragment's alpha (oD0.w) is clamped to [0, 1] and rounded to a byte before the comparison, the
 * reference being a byte. A fragment that fails the test is discarded, so it writes no colour.
 */
layout(constant_id = 0) const uint alpha_func = 7u;
layout(constant_id = 1) const uint alpha_ref = 0u;
layout(location = 0) in vec4 diffuse;
layout(location = 0) out vec4 fragment_colour;

void main(void)
{
    const uint alpha = uint(clamp(diffuse.w, 0.0, 1.0) * 255.0 + 0.5);
    bool passed = true;
    switch (alpha_func) {
    case 0u: passed = false; break;
    case 1u: passed = alpha < alpha_ref; break;
    case 2u: passed = alpha == alpha_ref; break;
    case 3u: passed = alpha <= alpha_ref; break;
    case 4u: passed = alpha > alpha_ref; break;
    case 5u: passed = alpha != alpha_ref; break;
    case 6u: passed = alpha >= alpha_ref; break;
    default: passed = true; break;
    }
    if (!passed) {
        discard;
    }
    fragment_colour = diffuse;
}
