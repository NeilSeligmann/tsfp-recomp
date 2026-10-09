/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The per-frame calls: Clear and, as the boot reaches them, the present path.
 *
 * `Clear` (0x003D5EB0, `ret 0x18`, six arguments: Count, pRects, Flags, Color, Z, Stencil) changes NO
 * library state beyond the pushbuffer cursor (MEASURED for four argument shapes, the title's own
 * colour and depth clear, a colour clear to 0x00FF8040, a depth-only clear with a rectangle and a
 * stencil-only clear: the single D3D8 dword that changed was the cursor, advanced by 28 bytes), but
 * the cursor moves because it WRITES COMMANDS, and since T440 so does this port. Its body loads the
 * render target and depth surface from the device, builds the NV2A surface-format word, converts
 * the clear colour and depth for the target's format, clips each rectangle against the viewport and
 * pushes, each at its own reservation preamble, SET_SURFACE_FORMAT (only for the formats whose table
 * entry has bit 0), then 28 bytes per rectangle (the clear rectangle, the zstencil value, the colour
 * and CLEAR_SURFACE) and the surface format again. d3d8_clear plans every site over a simulated
 * writer before the first write (so a refusal leaves no partial packet), then writes them. The
 * original is the oracle: tests/test_d3d8_clear_oracle.py replays 283 states through both and
 * compares the ring, the D3D region and the refill sites. docs/d3d8-usage.md 13.6 has the layout.
 *
 * WHAT STILL WRITES NO PIXELS. There is no GPU, so the surfaces stay zero (black), which is what the
 * title clears to (color 0) at its first two calls (0x00024032 and 0x0002404E). THAT IS A STATEMENT ABOUT
 * THE TITLE'S FIRST TWO CLEARS, NOT ABOUT CLEAR: a later non-zero colour or a depth clear to something
 * other than zero is a pixel change only a replay of the recorded CLEAR_SURFACE makes. The handler
 * says so, once, in the log, counts the calls and remembers the last arguments.
 *
 * It depends on viewport words the original's SetRenderTarget recomputes (device+0x954 to +0x964 and
 * +0xEE0 to +0xEF4), which d3d8_set_render_target now seeds (d3d8_bind.c). Unseeded they are zero
 * and every rectangle clips to nothing, which is what the port did before it wrote anything.
 */

#ifndef TSFP_GPU_D3D8_FRAME_H
#define TSFP_GPU_D3D8_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* What Clear was last called with. */
typedef struct {
    uint32_t count;
    uint32_t rects;
    uint32_t flags;
    uint32_t color;
    uint32_t depth_bits;
    uint32_t stencil;
} d3d8_clear_call;

/** 0x003D5EB0. */
void d3d8_clear(const d3d8_clear_call *call);

/** How many Clear calls have run, and how many of them asked for a colour other than 0 (the
 * only value the zeroed surfaces already satisfy). */
uint64_t d3d8_clear_count(void);
uint64_t d3d8_clear_nonblack_count(void);

/** The arguments of the most recent Clear, or zeros when there was none. */
d3d8_clear_call d3d8_clear_last(void);

/** Register this file's handlers; returns how many. */
size_t d3d8_frame_register(void);

/** Forget the counters. For tests. */
void d3d8_frame_reset(void);

#endif /* TSFP_GPU_D3D8_FRAME_H */
