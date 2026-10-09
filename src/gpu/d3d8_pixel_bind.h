/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_PIXEL_BIND_H
#define TSFP_D3D8_PIXEL_BIND_H
#include <stdbool.h>
#include <stdint.h>
typedef struct d3d8_pixel_bind_plan {
    uint32_t wrapper, definition, previous, words[60], rows[18], factor;
    uint32_t site[2]; /* where each site starts, after any refill (T525) */
} d3d8_pixel_bind_plan;
/* Exclusive shader-family/bookkeeping access and quiescent mappings are required
 * from preparation through apply. Identical-content permission probes precede writes. */
void d3d8_pixel_bind_prepare(uint32_t entry,uint32_t argument,bool definition_api,
                             d3d8_pixel_bind_plan *plan);
#endif
