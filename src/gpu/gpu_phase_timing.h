/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T1262: wall time per phase of the synchronous presenter work, for the stop report (what the guest thread waits for on a real GPU).
 * Header only, relaxed atomics, one process wide table (a weak definition, so every target that includes it shares it). A phase is
 * measured with gpu_phase_now() before and gpu_phase_add() after. Costs two clock reads per phase, nothing is read back from the table
 * except by the report, so it changes no guest visible state.
 */
#ifndef TSFP_GPU_PHASE_TIMING_H
#define TSFP_GPU_PHASE_TIMING_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    GPU_PHASE_PRESENT_FENCE_WAIT, /* vkWaitForFences on the previous present's submit */
    GPU_PHASE_PRESENT_ACQUIRE,    /* vkAcquireNextImageKHR (the swapchain/vsync wait when every image is queued) */
    GPU_PHASE_PRESENT_UPLOAD,     /* staging allocation, CPU scale and copy of the frame into the staging buffer, readback route only */
    GPU_PHASE_PRESENT_RECORD,     /* command buffer recording of the present */
    GPU_PHASE_PRESENT_SUBMIT,     /* vkQueueSubmit of the present */
    GPU_PHASE_PRESENT_QUEUE,      /* vkQueuePresentKHR */
    GPU_PHASE_PRESENT_IDLE,       /* vkQueueWaitIdle after the present (only with --live-present-sync) */
    GPU_PHASE_DRAW_FLUSH,         /* a render target draw run: end and submit (plus vkQueueWaitIdle with --live-present-sync) */
    GPU_PHASE_TARGET_SYNC,        /* a synchronous target command buffer (copy, blit, clear): submit, vkQueueWaitIdle */
    GPU_PHASE_READBACK,           /* a render target to host readback (the front buffer of the readback route), whole call */
    GPU_PHASE_TEXTURE_SYNC,       /* texture upload submit, descriptor rewrite and batch end, each a vkQueueWaitIdle */
    GPU_PHASE_FRAME_JOB,          /* one frame job: the title's draws of a Swap recorded and run (what a vblank job waits for when the renderer is the bottleneck) */
    GPU_PHASE_WINDOW_PUMP,        /* SDL event pump of the vblank job (X11 or Wayland round trips) */
    GPU_PHASE_VBLANK_PRESENT,     /* the live present op of a vblank or picture: schedule, front readback, compose, present */
    GPU_PHASE_DRAW_SYNC,          /* T1264: the deferred vkQueueWaitIdle that retires the draw runs submitted without a wait */
    GPU_PHASE_TEXTURE_DECODE,     /* T1289: a texture cache miss: the guest bytes converted to RGBA (DXT, swizzle) */
    GPU_PHASE_PIPELINE_CREATE,    /* T1289: vkCreateGraphicsPipelines of a new pipeline key */
    GPU_PHASE_MODULE_TRANSLATE,   /* T1289: a shader module translation (T847 translator, a subprocess per module) */
    GPU_PHASE_COUNT
} gpu_phase;

typedef struct {
    _Atomic uint64_t calls[GPU_PHASE_COUNT];
    _Atomic uint64_t total_ns[GPU_PHASE_COUNT];
    _Atomic uint64_t max_ns[GPU_PHASE_COUNT];
} gpu_phase_table;

extern gpu_phase_table gpu_phase_global;
__attribute__((weak)) gpu_phase_table gpu_phase_global;

static inline uint64_t gpu_phase_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* Adds the time since `start` (from gpu_phase_now) to `phase`. */
static inline void gpu_phase_add(gpu_phase phase, uint64_t start)
{
    const uint64_t spent = gpu_phase_now() - start;
    atomic_fetch_add_explicit(&gpu_phase_global.calls[phase], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&gpu_phase_global.total_ns[phase], spent, memory_order_relaxed);
    uint64_t seen = atomic_load_explicit(&gpu_phase_global.max_ns[phase], memory_order_relaxed);
    while (spent > seen &&
           !atomic_compare_exchange_weak_explicit(&gpu_phase_global.max_ns[phase], &seen, spent, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static inline const char *gpu_phase_name(gpu_phase phase)
{
    static const char *const names[GPU_PHASE_COUNT] = {
        "present fence wait", "present acquire", "present upload (readback route)", "present record", "present submit",
        "vkQueuePresentKHR", "present vkQueueWaitIdle (sync mode)", "target draw run (end + submit, wait idle only in sync mode)",
        "target command buffer (submit + wait idle)", "front readback (render target to host)", "texture upload, descriptor and batch waits (vkQueueWaitIdle)", "frame job (draws of one Swap)",
        "window event pump (vblank job)", "live present op (vblank job: schedule, readback, compose, present)",
        "draw runs retired by one deferred vkQueueWaitIdle (T1264)", "texture decode (T1289, cache miss)",
        "graphics pipeline creation (T1289)", "shader module translation (T1289)"};
    return phase < GPU_PHASE_COUNT ? names[phase] : "?";
}

#endif
