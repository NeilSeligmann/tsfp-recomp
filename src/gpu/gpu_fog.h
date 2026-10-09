/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_FOG_H
#define TSFP_GPU_FOG_H
#include "gpu_pgraph.h"
/* xemu programmable fog postlude, not fixed-function eye/normal generation.
 * mode0..5: LINEAR, EXP, EXP2, LINEAR_ABS, EXP_ABS, EXP2_ABS.
 * std140 row after live_raster; that row's fixed-point-size w remains intact.
 */
typedef struct {
  float enabled, mode, param0, param1;
} gpu_fog_control;
bool gpu_fog_decode(const gpu_pgraph_state *state, gpu_fog_control *out,
                    char *error, size_t error_bytes);
float gpu_fog_factor(const gpu_fog_control *control,
                     float programmable_distance);
#endif
