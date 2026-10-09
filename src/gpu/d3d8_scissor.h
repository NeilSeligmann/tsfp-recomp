/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_SCISSOR_H
#define TSFP_D3D8_SCISSOR_H
#include <stddef.h>
#include <stdint.h>
#include "d3d8_pushbuffer.h"
/* Original3D4470 count1/exclusive0 only. Quiescent mappings/default host x87
 * extended RN and masked SSE without FTZ/DAZ; no refill or rendering policy. */
uint32_t d3d8_set_scissors(uint32_t count, uint32_t exclusive, uint32_t rectangles);
/* T533: the arithmetic of the viewport rectangle: trunc(float(fild(value) [+ 2^32 when negative] * scale + bias)), the
 * x87 sequence of 0x003D4470 and 0x003D7B80 and `cvttss2si`. All of `scale`, `bias` and `unsigned_adjust` are float bits. */
uint32_t d3d8_scaled_integer(uint32_t value, uint32_t scale, uint32_t bias, uint32_t unsigned_adjust);
/* T533: 0x003D4470 with count 0 (the scissor of the viewport rectangle device+0xEE0..+0xEEC, which SetViewport(NULL)
 * runs). The words 0x003D7B80 stores before it (scales at +0x95C and +0x960, clip size at +0x954 and +0x958) are
 * passed in, so the plan can run before that store. `plan` refuses what the call would refuse and writes nothing
 * (identical-byte probes apart), `set` writes the packet and the cache words. */
/* Fatal at `entry` unless the host runs x87 in extended precision with default rounding and SSE with masked default
 * exceptions (the environment the ported float sequences are proved under). */
void d3d8_check_default_fp(uint32_t entry);
typedef struct {
    uint32_t scale_x, scale_y, width, height;
} d3d8_viewport_scissor;
void d3d8_plan_viewport_scissor(d3d8_pushbuffer_sim *sim);
void d3d8_set_viewport_scissor(const d3d8_viewport_scissor *in);
size_t d3d8_scissor_register(void);
#endif
