/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_FIRST_VBLANK_H
#define TSFP_GPU_D3D8_FIRST_VBLANK_H
#include <stdbool.h>
#include <stdint.h>
/* Explicit default-off ONE cooperative startup callback, not timing/IRQ/DPC.
 * Embedding host verifies compiled safepoints/routes and T186 context restoration.
 * Invoker receives original cdecl1 callback and immutable twelve-byte HOST payload;
 * executes on same designated guest thread, outside ALL module locks. Private
 * guest stack and guest mappings/content remain exclusive/quiescent during delivery.
 * Preflight writes identical bytes to top20 stack bytes and counter; these ARE
 * guest writes that preserve contents. PCR is4096-aligned/readable, stack high
 * is16-aligned and span at least128bytes.
 * No timestamp/port-field/MMIO/event/kernelclock/GPUstats or direct title-counter
 * operations. Failed/trapped callback retains already committed count bookkeeping. */
typedef bool (*d3d8_first_vblank_invoker)(uint32_t address,uint32_t stack_low,
                                       uint32_t stack_high,const uint32_t payload[3]);
/* Quiescent configuration/reset; bind inside designated worker, not parent launcher.
 * Other start/system combinations ignored. Duplicate matching launch refuses. */
void d3d8_first_vblank_configure(bool enabled,d3d8_first_vblank_invoker invoker,
                                uint32_t stack_low,uint32_t stack_high);
void d3d8_first_vblank_bind_owner(uint32_t handle,uint32_t fs,uint32_t start,uint32_t system);
void d3d8_first_vblank_poll(uint32_t callee,uint32_t fs,uint32_t esp,uint32_t irql);
void d3d8_first_vblank_reset(void);
typedef struct d3d8_first_vblank_snapshot {
    uint64_t epoch;
    uint32_t owner_handle,owner_fs;
    bool enabled,bound,attempted,delivered;
} d3d8_first_vblank_snapshot;
void d3d8_first_vblank_get_snapshot(d3d8_first_vblank_snapshot *output);
#endif
