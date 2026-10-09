/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_VIEWPORT_MATRIX_H
#define TSFP_D3D8_VIEWPORT_MATRIX_H
#include <stdint.h>
/* Internal original helpers only; parent viewport command emission remains separate.
 * Quiescent readable inputs/writable outputs, aligned one-page output matrices,
 * x86 SSE/x87 default rounding/precision and masked exceptions are required.
 * NaN inputs refuse before mutation: original-byte Oracle payload selection
 * disagrees with physical SSE; native operand ordering is never overridden. */
uint32_t d3d8_multiply_matrix(uint32_t destination,uint32_t left,uint32_t right);
uint32_t d3d8_rebuild_viewport_matrix(void);
/* Every refusal of d3d8_rebuild_viewport_matrix with no guest write (identical-byte probes only), for a
 * caller that must refuse before its own first write. */
void d3d8_rebuild_viewport_matrix_check(void);
/* The same check over the scale words (device+0x95C and +0x960) a caller is about to store (T533). */
void d3d8_rebuild_viewport_matrix_check_scaled(uint32_t scale_x,uint32_t scale_y);
#endif
