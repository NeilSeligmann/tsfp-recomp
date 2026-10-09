/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_VERTEX_PROGRAM_H
#define TSFP_GPU_D3D8_VERTEX_PROGRAM_H
#include <stdint.h>

#define D3D8_VERTEX_PROGRAM_SLOTS 136u

/* Original003D59E0 stdcall(program_header, first_slot), RET8. The uint16 at
 * header+2 counts16-byte instructions; its low header word is never read.
 * Zero count still emits its empty packet.
 * This host policy bounds the unchecked original to136 cached instruction slots.
 * Preserve cache flag10, 32-DWORD chunks, strict bulk reservation and cursor EAX;
 * unsupported rollover, overflow and source/output/device aliases stop.
 * The embedding caller keeps mappings/content exclusive and quiescent. Output,
 * cursor and selected cache permissions are preflighted by identical-byte writes
 * (these ARE guest writes). No new guest content is published on refusal within
 * that contract. No GPU shader execution or renderer behavior is supplied. */
uint32_t d3d8_upload_vertex_program(uint32_t packed, uint32_t first_slot);
#endif
