/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_VERTEX_CONSTANTS_H
#define TSFP_D3D8_VERTEX_CONSTANTS_H
#include <stddef.h>
#include <stdint.h>
/* Original3D58B0 fastcall ECX=index EDX=data, one stack count DWORD, RET4.
 * Explicit recovered192-register policy; original unchecked. Quiescent mappings,
 * no source/cache/command aliases. The entry roll-over and every refill between two full chunks (T487, T526) are made as
 * the original makes them, each planned from what the ones before it left. SIMD/x87 tag state not modeled. */
uint32_t d3d8_upload_vertex_constants(uint32_t index,uint32_t data,uint32_t count_dwords);
/* Original 3D57D0 (T734): the same emitter as 3D58B0 without the CPU shadow and without the device flags test
 * (fastcall ECX=index EDX=data, one stack count DWORD, RET4). Same chunks, roll-over, refills and return value. */
uint32_t d3d8_upload_vertex_constants_unshadowed(uint32_t index,uint32_t data,uint32_t count);
/* 3D5720: ECX=index, EDX=64-byte source, bare RET. Always updates shadow.
 * Repeated checked entry refills are supported; unsafe aliases remain refused. */
uint32_t d3d8_upload_four_vertex_constants(uint32_t index,uint32_t data);
/* 3D5670/3D56D0: one-vector checked28-byte packet, ECX index/EDX source,
 * bareRET. 5670 always shadows; 56D0 never shadows. Same repeated-refill plan. */
uint32_t d3d8_upload_one_vertex_constant(uint32_t index,uint32_t data);
uint32_t d3d8_upload_one_vertex_constant_fast(uint32_t index,uint32_t data);
size_t d3d8_vertex_constants_register(void);
#endif
