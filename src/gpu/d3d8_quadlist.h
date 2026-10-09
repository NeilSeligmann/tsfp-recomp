/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_QUADLIST_H
#define TSFP_GPU_D3D8_QUADLIST_H
#include <stddef.h>
#include <stdint.h>
/* D3DPT_QUADLIST (8) has no Vulkan equivalent. Each group of four vertices (a,b,c,d) becomes the
 * triangles (a,b,c) and (a,c,d), the NV2A QUADS split. A trailing group of fewer than four
 * vertices draws nothing. The title's draws inline 16-bit indices (ARRAY_ELEMENT16) or draw
 * sequential vertices, so both forms are accepted. */
#define D3D8_PRIMITIVE_QUADLIST 8u
/** Triangle-list indices produced for `vertex_count` quad vertices. */
uint32_t d3d8_quadlist_triangle_index_count(uint32_t vertex_count);
/**
 * Write the triangle-list indices. `indices` NULL means sequential vertices starting at `first`;
 * otherwise indices[i] is used and `first` ignored. Returns the index count written, or
 * UINT32_MAX when `capacity` is too small or a sequential vertex id would overflow 32 bits.
 */
uint32_t d3d8_quadlist_expand(const uint16_t *indices, uint32_t first, uint32_t vertex_count,
                              uint32_t *out, uint32_t capacity);
#endif
