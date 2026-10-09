/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_REFERENCE_H
#define TSFP_GPU_D3D8_REFERENCE_H
#include <stdint.h>
/* Original 3D4C50/3D4C90. Full parent traversal is validated before mutation.
 * Overlapping required header spans refuse before writes. AddRef guards every
 * read and probes all Common writes with identical bytes before parent-first
 * publication. Exclusive quiescent mappings/permissions are required. Release
 * and destruction retain their existing ownership and publication contracts.
 * Destruction is restricted to positive type5 live HLE-owned cube headers with
 * an empty private-data list. Other last-reference destruction stops. */
uint32_t d3d8_reference_add_ref(uint32_t header);
uint32_t d3d8_reference_release(uint32_t header);
/* Original3D4DA0 binding decrement, preserving its parent-before-child order. */
void d3d8_reference_release_binding(uint32_t header);
#endif
