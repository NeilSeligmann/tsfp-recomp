/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_DIRTY_H
#define TSFP_GPU_D3D8_DIRTY_H
#include <stdbool.h>
#include <stdint.h>
#include "d3d8_pushbuffer.h"
/* Original 0x003DD7C0, ret 4. Emits point state without clearing dirty bits.
 * Return value is the original quantized hardware point size. */
uint32_t d3d8_emit_point_state(void);
/* 0x003DD930 bound-shader branch. Null binding and refill refuse before mutation. */
uint32_t d3d8_emit_shader_stage_program(void);
/* Original 0x003DE830, ret 8: default packet and zero-light branch only.
 * Unsupported enabled lighting, non-null lights and refill refuse before writes.
 * Returns the original EAX residue; leaves the global dirty mask unchanged. */
/* Read-only branch preflight; cursor/refill validation is the caller's concern. */
void d3d8_validate_default_lighting(uint32_t dirty);
uint32_t d3d8_emit_default_lighting_state(uint32_t dirty);
/* Read-only planning for the recovered draw cascade: validate the emitter's branches and advance
 * the simulated writer through its reservation preamble, a roll-over included. Nothing is
 * written, so a refusal here precedes every write of the cascade. */
void d3d8_plan_point_state(d3d8_pushbuffer_sim *sim);
void d3d8_plan_shader_stage_program(d3d8_pushbuffer_sim *sim);
void d3d8_plan_default_lighting(uint32_t dirty, d3d8_pushbuffer_sim *sim);
/* T443: the fixed-function branches of the cascade families (the pixel shader unbound). */

/** The test every family makes: a vertex shader or the shader mode 1 makes the transform and
 * texture transform families return early without writing. */
bool d3d8_dirty_programmable(void);

/** 0x003DE080 (dirty 0x400). Fixed function only: one 0x40420+4*stage pair per stage when its texture
 * transform flags (texture state 0x15) are zero. A non-zero flags word is a named refusal. */
void d3d8_emit_texture_transforms(void);
void d3d8_plan_texture_transforms(d3d8_pushbuffer_sim *sim);

/** 0x003DEB80 (dirty 0x200 with bit 31 clear). Fixed function only: the world and view product at
 * device+0xDE0 times device+0xC60 as the 0x400480 packet, times device+0x980 as the 0x400680 packet.
 * T533: with render state 0x66 or a wrapped texture stage (device+0x950) the inverse of the first product
 * follows as the 12 dword 0x300580 packet (d3d8_matrix_inverse.h), and vertex blending (0x3E3EE4) writes the
 * viewport matrix itself, makes a second preamble and loops over three blend matrices (0x4004C0 + 0x40 * i, each
 * with a 0x3005C0 + 0x40 * i inverse packet when lit). Named refusals before the first write: a NaN word that
 * goes through the x87, and a singular matrix in the lit path. */
void d3d8_emit_fixed_function_matrices(uint32_t dirty);
void d3d8_plan_fixed_function_matrices(uint32_t dirty, d3d8_pushbuffer_sim *sim);

#endif
