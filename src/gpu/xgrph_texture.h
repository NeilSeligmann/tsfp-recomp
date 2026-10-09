/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XGRPH_TEXTURE_H
#define TSFP_GPU_XGRPH_TEXTURE_H
#include <stddef.h>
#include <stdint.h>
/* Original 0x003E66AE, stdcall ret 32. pool is ignored by the original.
 * Writes the five-word resource header, returning its allocation size. */
uint32_t xgrph_set_cube_texture_header(uint32_t edge, uint32_t levels, uint32_t usage,
    uint32_t format, uint32_t pool, uint32_t header, uint32_t data, uint32_t pitch);
/* Original 0x003E6684, stdcall ret36. Quiescent readable/writable header
 * mappings required; guarded publication changes exactly five words. */
uint32_t xgrph_set_texture_header(uint32_t width, uint32_t height, uint32_t levels,
    uint32_t usage, uint32_t format, uint32_t pool, uint32_t header,
    uint32_t data, uint32_t pitch);
size_t xgrph_texture_register(void);
#endif
