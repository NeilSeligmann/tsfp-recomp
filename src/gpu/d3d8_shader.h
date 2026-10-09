/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_SHADER_H
#define TSFP_GPU_D3D8_SHADER_H

#include <stddef.h>
#include <stdint.h>

/* 0x003D92E0 (definition) and 0x003D9320 (shader wrapper).
 * Both bindings preserve the original guest cache and write original pushbuffer words.
 * Texture-stage program word 54 is cached for its later emitter.
 * No pixels are rendered. Return value is the original's final cursor in eax.
 * Calls require exclusive shader-family access and quiescent mappings/permissions
 * through publication. Unproved aliases and refill paths are refused. */
uint32_t d3d8_set_pixel_shader(uint32_t definition);
uint32_t d3d8_set_pixel_shader_v(uint32_t wrapper);
/* 0x003D59E0, stdcall ret 8: packed instruction-count header and target slot.
 * Guarded by d3d8_vertex_program's bounded policy and publication preflight. */
uint32_t d3d8_upload_vertex_shader_constants(uint32_t packed, uint32_t index);
/* Recover the original CreateDevice vertex-shader defaults. */
void d3d8_shader_create(void);
/* Binder calls require exclusive shader-family/bookkeeping access and quiescent
 * guest mappings through apply. Unproved aliases/refill paths are refused. */
uint32_t d3d8_set_vertex_shader(uint32_t declaration, uint32_t index);
uint32_t d3d8_set_vertex_shader_handle(uint32_t tagged, uint32_t index);
/* 0x003D9520, stdcall ret 12: float4 pixel constants, register index first.
 * Positive counts use the local16-slot policy; exclusive shader-family access
 * and quiescent inputs/mappings/permissions are required through publication. */
uint32_t d3d8_set_pixel_shader_constants(uint32_t index, uint32_t data, uint32_t count);
/* 0x003D7860 (T443): the viewport and depth range packets written at `cursor`, returns the cursor after.
 * `shader_flags` is the declaration flags dword at device+0x794 + 4. */
uint32_t d3d8_viewport_emit(uint32_t cursor, uint32_t shader_flags);
size_t d3d8_shader_register(void);

#endif
