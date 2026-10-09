/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XGRPH_SHADER_QUERY_H
#define TSFP_GPU_XGRPH_SHADER_QUERY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Original 0x003E9546: returns the unsigned word at input+2, stdcall ret4.
 * Read exactly two bytes, without validating version/count/program length.
 * Read-only and unaligned inputs are supported; checked address overflow and
 * inaccessible/short reads fail without changing the HOST output. No guest writes. */
bool xgrph_vertex_shader_length(uint32_t input, uint32_t *out);
/* The handler refuses invalid frame/input through xgrph_hle_fatal before return. */
size_t xgrph_shader_query_register(void);
#endif
