/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_INDEXED_H
#define TSFP_GPU_D3D8_INDEXED_H
#include <stdint.h>
#include <stddef.h>
/* Original 0x003D5050 stdcall(primitive,count,index_data), ret12.
 * Inline guest 16-bit indices; no index-buffer resource/backend is implied.
 * Kickoff and large reservation rollover refuse before deferred-state writes. */
uint32_t d3d8_draw_indexed_vertices(uint32_t primitive, uint32_t count, uint32_t index_data);
size_t d3d8_indexed_register(void);
#endif
