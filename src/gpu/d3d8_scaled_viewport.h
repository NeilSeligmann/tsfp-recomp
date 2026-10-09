/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The scaled viewport and clip recomputation 0x003D7B80 (T533), and the SetViewport(NULL) it ends in
 * (0x003D3E40 with a null argument).
 *
 * ONE PIECE, TWO CALLERS. SetRenderTarget (d3d8_bind.c) runs 0x003D7B80 with a viewport of its own, render states
 * 0x9A (the render target is the first back buffer) and 0x9B (it is not) run it with none. What 0x003D7B80
 * computes is the same: the sample scales from the target and the device (+0x96C, +0x970), the clip size (+0x954,
 * +0x958), the scales (+0x95C, +0x960, +0x964, +0x968), the device flag 0x8000, and the packets 0x40208, 0x40380
 * and 0x41D7C. `d3d8_scaled_viewport_compute` is that computation, read-only, with every refusal, and
 * `d3d8_scaled_viewport_store` its stores. SetRenderTarget builds its own command stream around the same words,
 * `d3d8_scaled_viewport_run` is the standalone call.
 *
 * REFUSALS, all before the first write. A sample scale word that is NaN or infinite (the x87 and the oracle's
 * emulator disagree on NaN payloads, and the viewport matrix that follows refuses them too), a float constant of
 * `.rdata` that is NaN, a table word 0x003E1B48 + 4 * index that is not mapped guest memory, and whatever the
 * sites that follow would refuse (a ring roll-over device flag 4 cannot take, the vertex program helper, the
 * viewport matrix, the scissor).
 */

#ifndef TSFP_GPU_D3D8_SCALED_VIEWPORT_H
#define TSFP_GPU_D3D8_SCALED_VIEWPORT_H

#include <stdbool.h>
#include <stdint.h>

/* What 0x003D7B80 leaves and emits. `device_flags` is device+8 afterwards (bit 0x8000 follows the sample mode),
 * `changed` whether the new sample scale differs from device+0x964 (then +0x964, +0x968 and the dirty bits 0x10F are
 * written and 0x40380 is emitted too). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t sample_scale;
    uint32_t device_flags;
    bool changed;
    uint32_t table_word;
    uint32_t method_208;
    uint32_t method_380;
    uint32_t method_41d7c;
} d3d8_scaled_viewport;

/* Read-only. `mode` is the sample mode the branch reads: [0x003E3F28] when the render target is the first back
 * buffer, [0x003E3F2C] when it is not (render states 0x9A and 0x9B store it first). The branch follows the device's
 * render target (+0x1A04) against the first back buffer (+0x1A14). */
void d3d8_scaled_viewport_compute(d3d8_scaled_viewport *plan, uint32_t mode);

/* The stores of 0x003D7B80 (device flags, +0x95C/0x960/0x954/0x958, then +0x964, +0x968 and the dirty mask when the
 * sample scale changed). */
void d3d8_scaled_viewport_store(const d3d8_scaled_viewport *plan);

/* 0x003D7B80(device, edx = 0): the whole call, plan first. Stores `mode` at the state global `state_address` as the
 * caller (0x003D81F0 or 0x003D8220) does, after every refusal. */
void d3d8_scaled_viewport_run(uint32_t mode, uint32_t state_address);

/* Direct SDK calls already published the shadow. Retains existing finite/math
 * guards, but rereads changed-scale/point/mask/declaration/scissor values at their
 * original execution sites across actual entry refills. Legacy run is unchanged. */
uint32_t d3d8_scaled_viewport_run_direct(uint32_t mode);

#endif /* TSFP_GPU_D3D8_SCALED_VIEWPORT_H */
