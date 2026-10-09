/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Swap flag 1 copy composition (T443): the four helpers 0x003D8B10 runs around the swap's own
 * front buffer binding when D3DSWAP_COPY (bit 1) is set, the default of Swap(0). Measured on the
 * original (tests/test_d3d8_copy_composition_oracle.py, docs/d3d8-copy-composition.md):
 *
 *   0x003D8370 snapshot   copies 20 render states and 11 stage 0 texture states (the tables at
 *                         0x003E1C40 and 0x003E1BE8 name them), the pixel shader binding, the vertex
 *                         shader handle, stage 1's colour operation and, with a pixel shader bound, the
 *                         57 dword render state shadow and the pixel shader texture modes into a
 *                         0x170 byte frame on the caller's stack
 *   0x003D85F0 setup      writes the copy state (only the states that differ, or every immediate state
 *                         while device flag 0x10 is set), SetPixelShader(0), the filter states from
 *                         render state 0x7E, stage 1 off, runs the dirty cascade, loads the two
 *                         instruction vertex program at 0x003E1CE0 and writes the execution mode
 *   0x003D8990 triangle   one immediate mode triangle list (Begin 5, texcoord 0 and position pairs at
 *                         (0,0), (4W,0), (0,4H), End) sampling the back buffer into the front buffer
 *   0x003D8710 restore    puts every state back from the frame, restores the pixel shader and reloads
 *                         the vertex program saved at device+0x10A8
 *
 * THE COMMANDS ARE EMITTED, exactly as the original writes them, so the recorded stream carries the
 * state, the program and the triangle and the replay (d3d8_swap_replay) can draw the copy. What stays
 * elided is what the flag 2 prepare elides, the commands of 0x003D8B10's own body (render target,
 * texture bindings and the 0x40100 group between the setup and the triangle).
 */

#ifndef TSFP_GPU_D3D8_COPY_H
#define TSFP_GPU_D3D8_COPY_H

#include <stddef.h>
#include <stdint.h>

/** The frame 0x003D8B10 keeps at [esp+0xC]: 0x170 bytes. */
#define D3D8_COPY_FRAME_DWORDS 0x5Cu

/** 0x003D8370. */
void d3d8_copy_snapshot(uint32_t *frame);
/** 0x003D85F0. */
void d3d8_copy_setup(void);
/** 0x003D8990. */
void d3d8_copy_draw_triangle(void);
/** 0x003D8710. */
void d3d8_copy_restore(const uint32_t *frame);

/**
 * 0x003D3AD0, CopyRects(source, rects, count, destination, points) over the 2D engine (T555): a state packet
 * and one blit packet per rectangle, the device fence stored in both surfaces' Lock words. Returns eax as the
 * original leaves it. Every refusal precedes the first write. See d3d8_copy.c.
 */
uint32_t d3d8_copy_rects(uint32_t source, uint32_t rects, uint32_t count, uint32_t destination,
                         uint32_t points);

/** Register the 0x003D3AD0 handler. */
size_t d3d8_copy_register(void);

#endif /* TSFP_GPU_D3D8_COPY_H */
