/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_CUBE_SURFACE_H
#define TSFP_GPU_D3D8_CUBE_SURFACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Original 0x003D4E90, stdcall(texture, face, level), ret12. Allocates a real
 * 24-byte header in an HLE-owned guest heap; returns zero if allocation fails.
 * Recursive parent AddRef remains refused here; d3d8_reference handles bounded
 * last-reference destruction of the exact owned headers. */
uint32_t d3d8_get_cube_map_surface2(uint32_t texture, uint32_t face, uint32_t level);
/* Original 0x003D4E10, stdcall(texture, level), ret8. Same owned allocation
 * and face-zero layout. All family calls/reset/free and their owned-list access
 * are exclusive; input mappings/permissions remain quiescent through publication. */
uint32_t d3d8_get_surface_level2(uint32_t texture, uint32_t level);
size_t d3d8_cube_surface_register(void);
/* Exact allocation starts only, with live 24-byte allocation and heap generation.
 * Retired records reject repeated references before touching reclaimed heap bytes. */
bool d3d8_cube_surface_owned(uint32_t address);
bool d3d8_cube_surface_retired(uint32_t address);
bool d3d8_cube_surface_free_owned(uint32_t address);
/* Session teardown only: release the owned heap if still live, then forget it.
 * Safe after guest_mem_reset; does not implement resource Release semantics. */
void d3d8_cube_surface_reset(void);
#endif
