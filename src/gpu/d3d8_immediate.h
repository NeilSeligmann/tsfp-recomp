/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Immediate mode vertices (T443): Begin 0x003D5340, SetVertexData2f 0x003D52B0 and End 0x003D5380,
 * what the swap composition's triangle (0x003D8990) draws with, measured on the original
 * (docs/d3d8-copy-composition.md). Each one EMITS what the original writes, with its reservation
 * preamble, so a differential run compares the ring byte for byte.
 *
 *   Begin(primitive)   runs the dirty cascade 0x003DED80 first, then 0x417FC with the primitive,
 *                      and sets device flag 0x800 (the refill skips its fence while it is set)
 *   SetVertexData2f(slot, x, y)   one 3 dword packet, header 0x81880 + slot * 8, the two words as
 *                      passed (the title passes float bits)
 *   End()              0x417FC with 0, clears device flags 0x1800, and when 0x1000 was set (a
 *                      refill inside the Begin and End skipped its fence) inserts the fence the
 *                      refill owed (0x003D67B0 with flags 1)
 */

#ifndef TSFP_GPU_D3D8_IMMEDIATE_H
#define TSFP_GPU_D3D8_IMMEDIATE_H

#include <stddef.h>
#include <stdint.h>

/** 0x003D5340, stdcall(primitive). */
void d3d8_begin(uint32_t primitive);
/** 0x003D52B0, stdcall(slot, x, y). `x` and `y` are the raw words (float bits). */
void d3d8_set_vertex_data_2f(uint32_t slot, uint32_t x, uint32_t y);
/** 0x003D5380, no arguments. */
void d3d8_end(void);

/** Register this file's handlers (those on the title's surface); returns how many. */
size_t d3d8_immediate_register(void);

#endif /* TSFP_GPU_D3D8_IMMEDIATE_H */
