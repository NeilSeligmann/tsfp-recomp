/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T1289: where the GUEST thread's time goes, per vblank interval (the interval the `present vblank` histogram counts).
 *
 * The title's game thread (the thread that Swaps) is tracked. Its time is split into phases by an exclusive time stack: a phase is
 * entered and left around a region, the time inside belongs to the innermost open phase and the rest to GFT_GUEST (lifted game
 * code and the guest memory helpers). At every vblank hook exit one record is closed: wall ms, thread CPU ms, the phase split, the
 * presenter's work in the same interval (deltas of the gpu_phase table) and up to GFT_EVENTS_PER_FRAME slow events (a kernel or
 * XDK call, a wait for the presenter, a texture decode burst, pipeline creation, module translation, each of 1 ms or more).
 * The slowest GFT_SLOWEST intervals and the distributions are printed in the stop report so a user can paste them.
 *
 * Costs 2 TSC reads per region entered and nothing on other threads (one thread local pointer test). It reads and writes no guest
 * visible state. `--no-guest-frame-trace` turns it off.
 */
#ifndef TSFP_GUEST_FRAME_TRACE_H
#define TSFP_GUEST_FRAME_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    GFT_GUEST,         /* lifted game code, the guest memory helpers, everything not named below */
    GFT_XDK,           /* an XDK call (D3D8 and DSound HLE, push buffer encoding, retained originals), exclusive of what it enters */
    GFT_KERNEL,        /* a kernel call (NtReadFile, waits, allocation), exclusive */
    GFT_SAFEPOINT,     /* the cooperative safepoint provider: its async io service and its frame wait poll are timed, every call is counted */
    GFT_DECODE,        /* push buffer decode into the NV2A model (gpu_pgraph_decode) */
    GFT_SWAP_CAPTURE,  /* at the Swap: clone the frame model, the SetTexture history and the texture byte inputs (T1283) */
    GFT_SYNC_SLOT,     /* blocked: waiting for the presenter's job slot (the frame in flight) */
    GFT_SYNC_EXEC,     /* blocked: a synchronous presenter job (vblank job, observer, serial frame, register) running */
    GFT_PACE,          /* the vblank pace sleep (the 60 Hz governor, intended idle time) */
    GFT_AUDIO,         /* the audio work route (mixer render on the guest thread) */
    GFT_PHASES
} gft_phase;

/* Presenter side work in the same interval (deltas of gpu_phase_global, ns). */
typedef enum {
    GFT_PRES_FRAME_JOB,
    GFT_PRES_TEXTURE_SYNC,
    GFT_PRES_TEXTURE_DECODE,
    GFT_PRES_PIPELINE,
    GFT_PRES_MODULE,
    GFT_PRES_VBLANK_OP,
    GFT_PRES_KINDS
} gft_presenter;

/* Names an XDK address and a kernel ordinal for the report (main.c wires the real functions). */
typedef struct {
    const char *(*xdk_name)(uint32_t address);
    const char *(*kernel_name)(unsigned ordinal);
} gft_names;

/* Set once before the guest runs. Off by default until enabled by the host. */
void guest_frame_trace_enable(bool on);
bool guest_frame_trace_enabled(void);
/* The calling thread becomes the tracked thread (first caller wins): called at the Swap. */
void guest_frame_trace_claim(void);
/* Close the interval at a vblank hook exit (tracked thread only). */
void guest_frame_trace_boundary(void);
/* A Swap happened on the tracked thread (counts title frames per interval). */
void guest_frame_trace_swap(void);
/* The draws of the frame just swapped (the cost of a frame grows with them: the report relates guest time to draws per frame). */
void guest_frame_trace_draws(uint64_t draws);

/* The tracked thread's state starts with the scope counters, so a hot path that is not worth two TSC reads (the safepoint provider runs about
 * 30 thousand times per frame) counts itself with gft_count and times only its rare slow parts with gft_enter. */
typedef struct {
    uint64_t scopes[GFT_PHASES];
} gft_counters;

extern _Thread_local void *gft_tls;
void gft_enter_slow(gft_phase phase, uint64_t argument);
void gft_leave_slow(void);

static inline void gft_count(gft_phase phase)
{
    if (gft_tls != NULL) {
        ((gft_counters *)gft_tls)->scopes[phase]++;
    }
}

static inline void gft_enter(gft_phase phase, uint64_t argument)
{
    if (gft_tls != NULL) {
        gft_enter_slow(phase, argument);
    }
}

static inline void gft_leave(void)
{
    if (gft_tls != NULL) {
        gft_leave_slow();
    }
}

/* Statistics, for the report and the tests. */
typedef struct {
    uint64_t intervals;
    double wall_ms_mean, cpu_ms_mean, wall_ms_p50, wall_ms_p95, wall_ms_p99, wall_ms_max;
    double cpu_ms_p50, cpu_ms_p95, cpu_ms_p99, cpu_ms_max;
    double phase_ms_total[GFT_PHASES];
    double unexplained_ms_total; /* wall minus thread CPU minus every blocked phase: descheduled or blocked where no scope watches */
    uint64_t hist[8];            /* wall ms: <10 <15 <18 <22 <34 <50 <100 >=100 */
    uint64_t depth_overflows;
    double scopes_per_interval[GFT_PHASES]; /* scopes entered per interval, by phase (an XDK scope is one XDK call) */
    double trace_ms_per_interval;           /* the trace's own CPU: scopes times the measured cost of one */
    /* Least squares over the intervals that had a Swap: thread CPU ms = intercept + slope * draws. */
    uint64_t fit_intervals;
    double fit_intercept_ms, fit_slope_us_per_draw, fit_r_squared;
} gft_summary;

gft_summary guest_frame_trace_summary(void);
void guest_frame_trace_print(FILE *out, const gft_names *names);

/* Test seam: forget everything (the tracked thread, the records). */
void guest_frame_trace_reset_for_test(void);

#endif
