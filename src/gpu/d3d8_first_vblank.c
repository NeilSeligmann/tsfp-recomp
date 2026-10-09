/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_first_vblank.h"
#include "d3d8_flip.h"
#include "d3d8_hle.h"
#include "d3d8_vblank_effects.h"
#include "kernel_call.h"
#include "kernel_sync.h"
#include <pthread.h>
#include <string.h>
#define FRAME 0x1538C0u
#define DEVICE 0x3E3F60u
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static d3d8_first_vblank_snapshot state;
static pthread_t owner;
static d3d8_first_vblank_invoker dispatch;
static uint32_t private_low, private_high;
static bool announced;
static bool overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}
void d3d8_first_vblank_reset(void)
{
    pthread_mutex_lock(&lock);
    uint64_t epoch = state.epoch;
    bool enabled = state.enabled;
    memset(&state, 0, sizeof(state));
    state.epoch = epoch;
    state.enabled = enabled;
    announced = false;
    pthread_mutex_unlock(&lock);
}
void d3d8_first_vblank_configure(bool enabled, d3d8_first_vblank_invoker invoker,
                                uint32_t low, uint32_t high)
{
    const char *error = NULL;
    pthread_mutex_lock(&lock);
    if (enabled && (invoker == NULL || low == 0u || high <= low ||
                    high - low < 128u || (high & 15u) != 0u)) {
        error = "invalid exclusive callback stack/invoker";
    } else if (state.epoch == UINT64_MAX) {
        error = "configuration epoch exhausted";
    }
    else {
        uint64_t epoch = state.epoch + 1u;
        memset(&state, 0, sizeof(state));
        state.epoch = epoch;
        state.enabled = enabled;
        dispatch = invoker;
        private_low = low;
        private_high = high;
        announced = false;
    }
    pthread_mutex_unlock(&lock);
    if (error != NULL) d3d8_hle_fatal(FRAME, "%s", error);
}
void d3d8_first_vblank_bind_owner(uint32_t handle, uint32_t fs, uint32_t start, uint32_t system)
{
    const char *error = NULL;
    pthread_mutex_lock(&lock);
    if (state.enabled && start == 0x3801D9u && system == 0x37FE1Du) {
        if (state.bound) {
            error = "ambiguous duplicate startup owner";
        } else if (handle == 0u || fs == 0u || (fs & 4095u) != 0u ||
                   (uint64_t)fs + 4096u > UINT64_C(0x100000000)) {
            error = "invalid startup owner identity";
        }
        else {
            state.bound = true;
            state.owner_handle = handle;
            state.owner_fs = fs;
            owner = pthread_self();
        }
    }
    pthread_mutex_unlock(&lock);
    if (error != NULL) d3d8_hle_fatal(FRAME, "%s", error);
}
void d3d8_first_vblank_get_snapshot(d3d8_first_vblank_snapshot *output)
{
    if (output == NULL) return;
    pthread_mutex_lock(&lock);
    *output = state;
    pthread_mutex_unlock(&lock);
}
void d3d8_first_vblank_poll(uint32_t callee, uint32_t fs, uint32_t esp, uint32_t irql)
{
    const char *error = NULL;
    bool notify = false;
    uint32_t payload[3] = {0};
    d3d8_first_vblank_invoker invoke = NULL;
    uint32_t low = 0u, high = 0u;
    uint64_t epoch = 0u;
    pthread_mutex_lock(&lock);
    if (!state.enabled || callee != FRAME) goto done;
    if (!state.bound || !pthread_equal(owner, pthread_self()) || fs != state.owner_fs) {
        error = "unbound or mismatched startup owner";
        goto done;
    }
    if (state.attempted) {
        error = "ongoing frame callbacks unsupported after first startup event";
        goto done;
    }
    uint32_t caller, device;
    uint8_t pcr[4096];
    if (!kernel_guest_read_u32(esp, &caller) || caller != 0x3D455u) {
        error = "unsupported startup frame caller";
        goto done;
    }
    if (irql != 0u || kernel_sync_current_irql() != irql || !kernel_guest_read_bytes(fs, pcr, sizeof(pcr)) || pcr[0x24u] != irql) {
        error = "known matching IRQL0/PCR required";
        goto done;
    }
    if (!kernel_guest_read_u32(0x3E3F58u, &device) || device != DEVICE) {
        error = "current fixed startup device required";
        goto done;
    }
    const uint32_t offsets[] = {
        0x1DB8u, 0x1DE8u, 0x1DE4u, 0x1DECu, 0x1DDCu,
        0x1D9Cu, 0x1DA8u, 0x2448u, 0x244Cu
    };
    /* T372 coupling: every completed wait already counted one blank (the count starts at 0 at
     * CreateDevice, so the measured count is the number of waits applied). Off, the measured 1. */
    const uint32_t expected_count = d3d8_vblank_effects_enabled() ?
        (uint32_t)d3d8_vblank_effects_applied() : 1u;
    uint32_t expected[] = {0x22020u, expected_count, 0u, 0u, 0x02480104u, 0u, 0u, 0u, 0u};
    /* T407: with the flip model the Swaps before this frame entry queued and completed flips. The
     * consumer index is then the number of flips processed (every queued one, the producer index
     * agrees), both slots are empty, and the threshold is whatever the last queue left. */
    const bool modelled_flips = d3d8_flip_enabled() && d3d8_vblank_effects_enabled();
    uint32_t producer = 0u;
    if (modelled_flips) {
        expected[2] = (uint32_t)d3d8_flip_hardware_get().flips;
        if (!kernel_guest_read_u32(DEVICE + D3D8_FLIP_DEV_PRODUCER, &producer) ||
            producer != expected[2]) {
            error = "flip queue not drained at the startup event";
            goto done;
        }
    }
    uint32_t actual[9];
    for (unsigned i = 0u; i < 9u; i++) {
        if (!kernel_guest_read_u32(DEVICE + offsets[i], &actual[i])) {
            error = "unproven startup callback/device bookkeeping";
            goto done;
        }
        if (modelled_flips && i == 3u) {
            continue;
        }
        if (actual[i] != expected[i]) {
            error = "unproven startup callback/device bookkeeping";
            goto done;
        }
    }
    /* The helper updates the threshold when the new count meets it and no flip ran (none is due:
     * the queue is drained). The flip queue sets a nonzero threshold, so this can happen. */
    const bool hits_threshold = modelled_flips && actual[3] != 0u && actual[1] + 1u == actual[3];
    uint64_t stack_bytes = (uint64_t)private_high - private_low;
    if (overlap(private_low,stack_bytes,DEVICE,0x2450u) || overlap(private_low,stack_bytes,0x3E3F58u,4u) ||
       overlap(private_low,stack_bytes,fs,4096u) || overlap(private_low,stack_bytes,esp,4u) ||
       overlap(fs,4096u,DEVICE,0x2450u) || overlap(esp,4u,DEVICE,0x2450u) ||
       overlap(esp,4u,fs,4096u)) {
        error = "aliased callback stack/owner/caller/device";
        goto done;
    }
    /* T186's verified leaf needs exactly the top20 bytes (return/pointer/record).
     * Its high boundary is16-aligned. Probe before count publication. */
    uint8_t frame_bytes[20];
    if (!kernel_guest_read_bytes(private_high - sizeof(frame_bytes), frame_bytes, sizeof(frame_bytes)) ||
        !kernel_guest_write_bytes(private_high - sizeof(frame_bytes), frame_bytes, sizeof(frame_bytes))) {
        error = "private callback frame read/write refused";
        goto done;
    }
    /* Same-byte frame/count probes preserve contents but ARE guest writes. Content
     * and mappings stay quiescent until publication. Only count is written (measured
     * newcount2 != threshold0), plus the threshold when the T407 flip model made it nonzero and
     * the new count meets it. */
    payload[0] = actual[1] + 1u;
    payload[1] = actual[2];
    payload[2] = payload[0] == actual[3] ? 2u : 0u;
    if(!kernel_guest_write_u32(DEVICE+0x1DE8u,actual[1]) ||
       !kernel_guest_write_u32(DEVICE+0x1DE8u,payload[0]) ||
       (hits_threshold && !kernel_guest_write_u32(DEVICE+0x1DECu,actual[3]+1u))) {
        error = "startup count write refused";
        goto done;
    }
    state.attempted = true;
    invoke = dispatch;
    low = private_low;
    high = private_high;
    epoch = state.epoch;
    if (!announced) {
        announced = true;
        notify = true;
    }
done:
    pthread_mutex_unlock(&lock);
    if (error != NULL) d3d8_hle_fatal(FRAME, "%s", error);
    if (invoke == NULL) return;
    if (notify) d3d8_hle_log()("explicit HEADLESS first startup CPU vblank callback; no timestamp/port-field/MMIO/event/clock/GPUstats advance; no IRQ/DPC equivalence\n");
    if (!invoke(0x22020u, low, high, payload))
        d3d8_hle_fatal(FRAME, "startup callback invocation failed after count publication");
    pthread_mutex_lock(&lock);
    if (state.epoch == epoch) state.delivered = true;
    pthread_mutex_unlock(&lock);
}
