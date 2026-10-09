/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_TEXTURE_DIRTY_H
#define TSFP_GPU_D3D8_TEXTURE_DIRTY_H
#include <stdint.h>
#include "d3d8_pushbuffer.h"
/* These preflight functions read guest inputs and reject unsupported work before
 * any cache or command writes. They never clear dirty bits. */
void d3d8_texture_stages_preflight(uint32_t dirty);
void d3d8_fog_preflight(void);
uint32_t d3d8_texture_packet_bytes(uint32_t dirty);
uint32_t d3d8_fog_packet_bytes(void);
/* Read-only planning for the recovered draw cascade: validate the emitter and advance the
 * simulated writer through its reservation preambles, refills included. Nothing is written. */
void d3d8_plan_texture_stages(uint32_t dirty, d3d8_pushbuffer_sim *sim);
void d3d8_plan_fog(d3d8_pushbuffer_sim *sim);
/* 0x003DDCB0 ret 8: only the four low dirty bits select stages; eax returns zero. */
uint32_t d3d8_emit_texture_stages(uint32_t dirty);
/* 0x003DDEA0 ret 4: enabled/disabled fog including declaration bit 2 program setup.
 * Emits guest commands only; returns final cursor. */
uint32_t d3d8_emit_fog(void);
/* 0x003D5C50 ECX=device/plain ret; declaration bit2 clear returns declaration.
 * Otherwise programs existing guest vertex constants, returns final cursor. */
uint32_t d3d8_emit_fog_vertex_program(void);
/* T461: 0x003D5C50 as the library's own callers run it, with its sized reservation (0x003D6B30) in front,
 * which can roll the ring over. The plan is read-only and refuses what the run would refuse. Both do
 * nothing for a declaration without flag 2. */
void d3d8_plan_vertex_program_helper(d3d8_pushbuffer_sim *sim);
void d3d8_run_vertex_program_helper(void);
/* Compatibility entry used by the recovered draw cascade; also handles enabled fog. */
uint32_t d3d8_emit_disabled_fog(void);
#endif
