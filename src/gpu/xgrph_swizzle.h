/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XGRPH_SWIZZLE_H
#define TSFP_GPU_XGRPH_SWIZZLE_H
#include <stddef.h>
#include <stdint.h>
/* Original 0x003EEABE, stdcall ret 32. Recovered scope: nonoverlapping 64x64
 * RGBA images, pitch 0 or 256, NULL or full RECT, NULL destination POINT.
 * Returns measured EAX (0xFE0 fast path; 256 general path). The native HLE
 * boundary does not reproduce the original scratch ECX=0, EDX=256 outputs. */
uint32_t xgrph_swizzle_rect(uint32_t source, uint32_t pitch, uint32_t rectangle,
    uint32_t destination, uint32_t width, uint32_t height, uint32_t point,
    uint32_t bytes_per_pixel);
size_t xgrph_swizzle_register(void);
#endif
