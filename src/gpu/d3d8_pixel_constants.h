/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_PIXEL_CONSTANTS_H
#define TSFP_D3D8_PIXEL_CONSTANTS_H
#include <stdint.h>
#define D3D8_PIXEL_CONSTANT_SLOTS 16u
typedef struct d3d8_pixel_constants_plan {
    uint32_t source[D3D8_PIXEL_CONSTANT_SLOTS][4];
    uint32_t usage[D3D8_PIXEL_CONSTANT_SLOTS];
    uint32_t maps[3];
    uint32_t site[D3D8_PIXEL_CONSTANT_SLOTS]; /* where each vector's commands start (T525) */
} d3d8_pixel_constants_plan;
/* The original checks the limit once per VECTOR (0x003D95B5) and refills there, so a call is planned as one site
 * per vector over a simulated writer that carries what each refill moves (T525, T526): `site` is where each
 * vector's commands start, after any refill. Exclusive shader-family access and quiescent input values, mappings and permissions
 * are required through apply. The positive-count16-slot policy bounds original unchecked
 * table/cache indexing; zero count retains the mandatory bound-wrapper read only. */
void d3d8_pixel_constants_prepare(uint32_t index,uint32_t data,uint32_t count,
                                  d3d8_pixel_constants_plan *plan);
#endif
