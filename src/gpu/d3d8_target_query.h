/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_TARGET_QUERY_H
#define TSFP_GPU_D3D8_TARGET_QUERY_H
#include <stddef.h>
#include <stdint.h>
/* Original stdcall0/RET0 getters. The recovered fixed device must be live;
 * NULL binding returns0. Nonnull binding takes the original AddRef, including
 * supported parent chains. Requires exclusive quiescent guest mappings through
 * AddRef's full permission preflight and publication; no command emission. */
uint32_t d3d8_get_render_target2(void);
uint32_t d3d8_get_depth_stencil_surface2(void);
size_t d3d8_target_query_register(void);
#endif
