/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1289: per vblank interval guest thread time trace. See guest_frame_trace.h. */
#define _GNU_SOURCE
#include "guest_frame_trace.h"

#include "gpu_phase_timing.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define GFT_STAMP() ((uint64_t)__rdtsc())
#else
#define GFT_STAMP() gft_monotonic_ns()
#endif

#define GFT_STACK 16u
#define GFT_EVENTS_PER_FRAME 6u
#define GFT_SLOWEST 16u
#define GFT_HIST_BUCKETS 1024u /* 0.25 ms steps up to 256 ms, the last bucket holds the rest */
#define GFT_EVENT_MIN_NS 1000000u

typedef enum {
    GFT_EV_KERNEL,
    GFT_EV_XDK,
    GFT_EV_SAFEPOINT,
    GFT_EV_SYNC_SLOT,
    GFT_EV_SYNC_EXEC,
    GFT_EV_AUDIO,
    GFT_EV_SWAP_CAPTURE,
    GFT_EV_DECODE,
    GFT_EV_PRESENTER_DECODE,
    GFT_EV_PRESENTER_PIPELINE,
    GFT_EV_PRESENTER_MODULE,
    GFT_EV_KINDS
} event_kind;

typedef struct {
    uint8_t kind;
    uint64_t argument;
    uint64_t ns;
    uint64_t count;
} gft_event;

typedef struct {
    uint64_t index, swaps, title_frame, draws;
    uint64_t wall_ns, cpu_ns;
    uint64_t phase_ns[GFT_PHASES];
    uint64_t presenter_ns[GFT_PRES_KINDS];
    uint64_t presenter_count[GFT_PRES_KINDS];
    gft_event events[GFT_EVENTS_PER_FRAME];
    unsigned event_count;
} record;

typedef struct {
    uint8_t phase;
    uint64_t argument, start, child; /* ticks spent in the scopes entered from this one */
} stack_entry;

typedef struct {
    uint64_t scopes[GFT_PHASES];                  /* scopes entered in the open interval (FIRST: gft_count in the header writes it) */
    stack_entry stack[GFT_STACK];
    unsigned depth;
    uint64_t mark;                                /* TSC of the last charge */
    uint64_t ticks[GFT_PHASES];                   /* open interval */
    gft_event events[GFT_EVENTS_PER_FRAME];
    unsigned event_count;
} thread_state;

_Thread_local void *gft_tls;

static bool g_enabled;
static pthread_mutex_t g_claim_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_state g_state;
static bool g_claimed;
#if defined(__x86_64__) || defined(__i386__)
#define GFT_TICKS_PER_MS_GUESS 2000000u
#else
#define GFT_TICKS_PER_MS_GUESS 1000000u /* the stamp is nanoseconds */
#endif
static uint64_t g_ticks_per_ms = GFT_TICKS_PER_MS_GUESS; /* refined at every boundary from the monotonic clock */
static uint64_t g_tsc_start, g_mono_start, g_last_mono, g_last_cpu, g_last_tsc;
static uint64_t g_swaps_total, g_swaps_at_last;
static uint64_t g_draws_pending; /* draws of the Swap seen since the last boundary (the last Swap wins) */
/* Cost by draws per frame (intervals with exactly one Swap), buckets of 100 draws, and the least squares sums. */
#define GFT_DRAW_BUCKETS 9u
static struct {
    uint64_t intervals, draws, wall_ns, cpu_ns;
    uint64_t phase_ns[GFT_PHASES];
} g_by_draws[GFT_DRAW_BUCKETS];
static double g_fit_n, g_fit_x, g_fit_xx, g_fit_y, g_fit_xy, g_fit_yy;
static uint64_t g_intervals, g_depth_overflows;
static uint64_t g_phase_ns_total[GFT_PHASES], g_phase_scopes_total[GFT_PHASES];
static uint64_t g_pair_cost_ns; /* measured at the claim: one enter plus one leave */
static uint64_t g_wall_total_ns, g_cpu_total_ns, g_unexplained_total_ns, g_wall_max_ns, g_cpu_max_ns;
static uint64_t g_wall_hist[GFT_HIST_BUCKETS], g_cpu_hist[GFT_HIST_BUCKETS];
static uint64_t g_buckets[8];
static record g_slowest[GFT_SLOWEST];
static unsigned g_slowest_count;
static uint64_t g_presenter_last[GFT_PRES_KINDS], g_presenter_calls_last[GFT_PRES_KINDS];
static uint64_t g_event_totals_ns[GFT_EV_KINDS], g_event_totals_count[GFT_EV_KINDS];
/* The same split per window of GFT_SERIES_INTERVALS intervals, so a section of a run (a heavy scene) can be compared with the rest. */
#define GFT_SERIES_INTERVALS 256u
#define GFT_SERIES_WINDOWS 128u
static struct {
    uint64_t intervals, wall_ns, cpu_ns, first_frame, last_frame, wall_max_ns;
    uint64_t phase_ns[GFT_PHASES];
    uint64_t presenter_ns[GFT_PRES_KINDS];
} g_series[GFT_SERIES_WINDOWS];

static uint64_t gft_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void guest_frame_trace_enable(bool on)
{
    g_enabled = on;
}

bool guest_frame_trace_enabled(void)
{
    return g_enabled;
}

void guest_frame_trace_reset_for_test(void)
{
    pthread_mutex_lock(&g_claim_lock);
    gft_tls = NULL;
    memset(&g_state, 0, sizeof g_state);
    g_claimed = false;
    g_ticks_per_ms = GFT_TICKS_PER_MS_GUESS;
    g_intervals = g_depth_overflows = g_swaps_total = g_swaps_at_last = 0u;
    memset(g_phase_ns_total, 0, sizeof g_phase_ns_total);
    memset(g_phase_scopes_total, 0, sizeof g_phase_scopes_total);
    g_pair_cost_ns = 0u;
    g_wall_total_ns = g_cpu_total_ns = g_unexplained_total_ns = g_wall_max_ns = g_cpu_max_ns = 0u;
    memset(g_wall_hist, 0, sizeof g_wall_hist);
    memset(g_cpu_hist, 0, sizeof g_cpu_hist);
    memset(g_buckets, 0, sizeof g_buckets);
    memset(g_slowest, 0, sizeof g_slowest);
    g_slowest_count = 0u;
    memset(g_presenter_last, 0, sizeof g_presenter_last);
    memset(g_presenter_calls_last, 0, sizeof g_presenter_calls_last);
    memset(g_event_totals_ns, 0, sizeof g_event_totals_ns);
    memset(g_event_totals_count, 0, sizeof g_event_totals_count);
    memset(g_series, 0, sizeof g_series);
    memset(g_by_draws, 0, sizeof g_by_draws);
    g_draws_pending = 0u;
    g_fit_n = g_fit_x = g_fit_xx = g_fit_y = g_fit_xy = g_fit_yy = 0.0;
    pthread_mutex_unlock(&g_claim_lock);
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
#if defined(__x86_64__) || defined(__i386__)
    return (uint64_t)((double)ticks * 1e6 / (double)g_ticks_per_ms);
#else
    (void)g_ticks_per_ms;
    return ticks;
#endif
}

static void presenter_totals(uint64_t ns[GFT_PRES_KINDS], uint64_t calls[GFT_PRES_KINDS])
{
    static const gpu_phase phases[GFT_PRES_KINDS] = {GPU_PHASE_FRAME_JOB,       GPU_PHASE_TEXTURE_SYNC,   GPU_PHASE_TEXTURE_DECODE,
                                                     GPU_PHASE_PIPELINE_CREATE, GPU_PHASE_MODULE_TRANSLATE, GPU_PHASE_VBLANK_PRESENT};
    for (unsigned kind = 0u; kind < GFT_PRES_KINDS; kind++) {
        ns[kind] = atomic_load_explicit(&gpu_phase_global.total_ns[phases[kind]], memory_order_relaxed);
        calls[kind] = atomic_load_explicit(&gpu_phase_global.calls[phases[kind]], memory_order_relaxed);
    }
}

void guest_frame_trace_claim(void)
{
    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_claim_lock);
    if (!g_claimed) {
        g_claimed = true;
        memset(&g_state, 0, sizeof g_state);
#if defined(__x86_64__) || defined(__i386__)
        {
            /* The TSC rate, from 2 ms of the monotonic clock, so the first intervals are in milliseconds too (refined at every boundary). */
            const uint64_t tsc_a = GFT_STAMP(), mono_a = gft_monotonic_ns();
            const struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
            const uint64_t tsc_b = GFT_STAMP(), mono_b = gft_monotonic_ns();
            if (mono_b > mono_a && tsc_b > tsc_a) {
                g_ticks_per_ms = (uint64_t)((double)(tsc_b - tsc_a) * 1e6 / (double)(mono_b - mono_a));
            }
        }
#endif
        g_state.mark = GFT_STAMP();
        g_tsc_start = g_last_tsc = g_state.mark;
        g_mono_start = g_last_mono = gft_monotonic_ns();
        g_last_cpu = thread_cpu_ns();
        presenter_totals(g_presenter_last, g_presenter_calls_last);
        gft_tls = &g_state; /* thread local: only the claiming thread sees it */
        {
            /* The cost of one scope (enter plus leave) on this machine, for the report: scopes entered times this is the trace's own CPU. */
            const uint64_t begin = gft_monotonic_ns();
            for (unsigned count = 0u; count < 2000u; count++) {
                gft_enter_slow(GFT_AUDIO, 0u);
                gft_leave_slow();
            }
            g_pair_cost_ns = (gft_monotonic_ns() - begin) / 2000u;
            memset(g_state.ticks, 0, sizeof g_state.ticks);
            memset(g_state.scopes, 0, sizeof g_state.scopes);
            memset(g_state.events, 0, sizeof g_state.events);
            g_state.event_count = 0u;
            g_state.depth = 0u;
            g_state.mark = GFT_STAMP();
            g_event_totals_ns[GFT_EV_AUDIO] = g_event_totals_count[GFT_EV_AUDIO] = 0u;
            g_last_cpu = thread_cpu_ns();
            g_last_mono = gft_monotonic_ns();
        }
    }
    pthread_mutex_unlock(&g_claim_lock);
}

void guest_frame_trace_swap(void)
{
    if (gft_tls != NULL) {
        g_swaps_total++;
    }
}

void guest_frame_trace_draws(uint64_t draws)
{
    if (gft_tls != NULL) {
        g_draws_pending = draws;
    }
}

static void charge(thread_state *state, uint64_t now)
{
    const gft_phase phase = state->depth == 0u ? GFT_GUEST : (gft_phase)state->stack[state->depth - 1u].phase;
    state->ticks[phase] += now - state->mark;
    state->mark = now;
}

static void note_event(thread_state *state, event_kind kind, uint64_t argument, uint64_t ns, uint64_t count)
{
    g_event_totals_ns[kind] += ns;
    g_event_totals_count[kind] += count;
    unsigned slot = state->event_count;
    if (slot == GFT_EVENTS_PER_FRAME) {
        slot = 0u; /* replace the shortest */
        for (unsigned index = 1u; index < GFT_EVENTS_PER_FRAME; index++) {
            if (state->events[index].ns < state->events[slot].ns) {
                slot = index;
            }
        }
        if (state->events[slot].ns >= ns) {
            return;
        }
    } else {
        state->event_count++;
    }
    state->events[slot] = (gft_event){(uint8_t)kind, argument, ns, count};
}

void gft_enter_slow(gft_phase phase, uint64_t argument)
{
    thread_state *state = gft_tls;
    if (state->depth >= GFT_STACK) {
        g_depth_overflows++;
        return;
    }
    const uint64_t now = GFT_STAMP();
    charge(state, now);
    state->scopes[phase]++;
    state->stack[state->depth++] = (stack_entry){(uint8_t)phase, argument, now, 0u};
}

static event_kind event_of(gft_phase phase)
{
    switch (phase) {
    case GFT_KERNEL: return GFT_EV_KERNEL;
    case GFT_XDK: return GFT_EV_XDK;
    case GFT_SAFEPOINT: return GFT_EV_SAFEPOINT;
    case GFT_SYNC_SLOT: return GFT_EV_SYNC_SLOT;
    case GFT_SYNC_EXEC: return GFT_EV_SYNC_EXEC;
    case GFT_AUDIO: return GFT_EV_AUDIO;
    case GFT_SWAP_CAPTURE: return GFT_EV_SWAP_CAPTURE;
    case GFT_DECODE: return GFT_EV_DECODE;
    default: return GFT_EV_KINDS; /* guest and pace: no event */
    }
}

void gft_leave_slow(void)
{
    thread_state *state = gft_tls;
    if (state->depth == 0u) {
        return; /* unbalanced (a guest stop unwound through a scope) */
    }
    const uint64_t now = GFT_STAMP();
    charge(state, now);
    const stack_entry entry = state->stack[--state->depth];
    const uint64_t inclusive_ticks = now - entry.start;
    if (state->depth != 0u) {
        state->stack[state->depth - 1u].child += inclusive_ticks;
    }
    /* The time of the scope itself, without the scopes it entered (they report their own events). Compared in ticks: 1 ms is g_ticks_per_ms. */
    const uint64_t exclusive_ticks = inclusive_ticks > entry.child ? inclusive_ticks - entry.child : 0u;
    if (exclusive_ticks >= g_ticks_per_ms) {
        const event_kind kind = event_of((gft_phase)entry.phase);
        if (kind != GFT_EV_KINDS) {
            note_event(state, kind, entry.argument, ticks_to_ns(exclusive_ticks), 1u);
        }
    }
}

static unsigned bucket_of(uint64_t ns)
{
    const uint64_t index = ns / 250000u;
    return index >= GFT_HIST_BUCKETS ? GFT_HIST_BUCKETS - 1u : (unsigned)index;
}

static double percentile(const uint64_t *hist, uint64_t total, double fraction)
{
    if (total == 0u) {
        return 0.0;
    }
    uint64_t want = (uint64_t)((double)total * fraction);
    if (want >= total) {
        want = total - 1u;
    }
    uint64_t seen = 0u;
    for (unsigned index = 0u; index < GFT_HIST_BUCKETS; index++) {
        seen += hist[index];
        if (seen > want) {
            return (double)(index + 1u) * 0.25;
        }
    }
    return (double)GFT_HIST_BUCKETS * 0.25;
}

static void keep_slowest(const record *entry)
{
    unsigned slot = 0u;
    if (g_slowest_count < GFT_SLOWEST) {
        slot = g_slowest_count++;
    } else {
        for (unsigned index = 1u; index < GFT_SLOWEST; index++) {
            if (g_slowest[index].wall_ns < g_slowest[slot].wall_ns) {
                slot = index;
            }
        }
        if (g_slowest[slot].wall_ns >= entry->wall_ns) {
            return;
        }
    }
    g_slowest[slot] = *entry;
}

void guest_frame_trace_boundary(void)
{
    thread_state *state = gft_tls;
    if (state == NULL) {
        return;
    }
    const uint64_t now = GFT_STAMP();
    charge(state, now);
    const uint64_t mono = gft_monotonic_ns();
    const uint64_t cpu = thread_cpu_ns();
    if (mono - g_mono_start > 200000000u && now > g_tsc_start) {
        g_ticks_per_ms = (uint64_t)((double)(now - g_tsc_start) * 1e6 / (double)(mono - g_mono_start));
        if (g_ticks_per_ms == 0u) {
            g_ticks_per_ms = 1u;
        }
    }
    record entry;
    memset(&entry, 0, sizeof entry);
    entry.index = g_intervals;
    entry.swaps = g_swaps_total - g_swaps_at_last;
    entry.draws = entry.swaps != 0u ? g_draws_pending : 0u;
    g_draws_pending = 0u;
    entry.title_frame = g_swaps_total;
    entry.wall_ns = mono - g_last_mono;
    entry.cpu_ns = cpu - g_last_cpu;
    uint64_t blocked_ns = 0u;
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
        entry.phase_ns[phase] = ticks_to_ns(state->ticks[phase]);
        state->ticks[phase] = 0u;
        g_phase_scopes_total[phase] += state->scopes[phase];
        state->scopes[phase] = 0u;
        g_phase_ns_total[phase] += entry.phase_ns[phase];
        if (phase == GFT_SYNC_SLOT || phase == GFT_SYNC_EXEC || phase == GFT_PACE) {
            blocked_ns += entry.phase_ns[phase];
        }
    }
    uint64_t presenter[GFT_PRES_KINDS], calls[GFT_PRES_KINDS];
    presenter_totals(presenter, calls);
    for (unsigned kind = 0u; kind < GFT_PRES_KINDS; kind++) {
        entry.presenter_ns[kind] = presenter[kind] - g_presenter_last[kind];
        entry.presenter_count[kind] = calls[kind] - g_presenter_calls_last[kind];
        g_presenter_last[kind] = presenter[kind];
        g_presenter_calls_last[kind] = calls[kind];
    }
    static const struct {
        gft_presenter kind;
        event_kind event;
    } presenter_events[3] = {{GFT_PRES_TEXTURE_DECODE, GFT_EV_PRESENTER_DECODE},
                             {GFT_PRES_PIPELINE, GFT_EV_PRESENTER_PIPELINE},
                             {GFT_PRES_MODULE, GFT_EV_PRESENTER_MODULE}};
    for (unsigned index = 0u; index < 3u; index++) {
        const gft_presenter kind = presenter_events[index].kind;
        if (entry.presenter_ns[kind] >= GFT_EVENT_MIN_NS) {
            note_event(state, presenter_events[index].event, 0u, entry.presenter_ns[kind], entry.presenter_count[kind]);
        }
    }
    memcpy(entry.events, state->events, sizeof entry.events);
    entry.event_count = state->event_count;
    memset(state->events, 0, sizeof state->events);
    state->event_count = 0u;
    g_last_mono = mono;
    g_last_cpu = cpu;
    g_last_tsc = now;
    g_swaps_at_last = g_swaps_total;
    g_intervals++;
    g_wall_total_ns += entry.wall_ns;
    g_cpu_total_ns += entry.cpu_ns;
    if (entry.wall_ns > g_wall_max_ns) g_wall_max_ns = entry.wall_ns;
    if (entry.cpu_ns > g_cpu_max_ns) g_cpu_max_ns = entry.cpu_ns;
    if (entry.wall_ns > entry.cpu_ns + blocked_ns) {
        g_unexplained_total_ns += entry.wall_ns - entry.cpu_ns - blocked_ns;
    }
    g_wall_hist[bucket_of(entry.wall_ns)]++;
    g_cpu_hist[bucket_of(entry.cpu_ns)]++;
    static const uint64_t edges_ms[7] = {10u, 15u, 18u, 22u, 34u, 50u, 100u};
    unsigned bucket = 0u;
    while (bucket < 7u && entry.wall_ns / 1000000u >= edges_ms[bucket]) {
        bucket++;
    }
    g_buckets[bucket]++;
    keep_slowest(&entry);
    if (entry.swaps == 1u) {
        const unsigned bucket_index = entry.draws / 100u >= GFT_DRAW_BUCKETS ? GFT_DRAW_BUCKETS - 1u : (unsigned)(entry.draws / 100u);
        g_by_draws[bucket_index].intervals++;
        g_by_draws[bucket_index].draws += entry.draws;
        g_by_draws[bucket_index].wall_ns += entry.wall_ns;
        g_by_draws[bucket_index].cpu_ns += entry.cpu_ns;
        for (unsigned phase = 0u; phase < GFT_PHASES; phase++) g_by_draws[bucket_index].phase_ns[phase] += entry.phase_ns[phase];
        if (entry.wall_ns < 60000000u) { /* not a load or movie stall: those are not the per draw cost */
            const double x = (double)entry.draws, y = (double)entry.cpu_ns / 1e6;
            g_fit_n += 1.0; g_fit_x += x; g_fit_xx += x * x; g_fit_y += y; g_fit_xy += x * y; g_fit_yy += y * y;
        }
    }
    uint64_t window = entry.index / GFT_SERIES_INTERVALS;
    if (window >= GFT_SERIES_WINDOWS) {
        window = GFT_SERIES_WINDOWS - 1u;
    }
    if (g_series[window].intervals == 0u) {
        g_series[window].first_frame = entry.title_frame;
    }
    g_series[window].intervals++;
    g_series[window].wall_ns += entry.wall_ns;
    g_series[window].cpu_ns += entry.cpu_ns;
    g_series[window].last_frame = entry.title_frame;
    if (entry.wall_ns > g_series[window].wall_max_ns) g_series[window].wall_max_ns = entry.wall_ns;
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) g_series[window].phase_ns[phase] += entry.phase_ns[phase];
    for (unsigned kind = 0u; kind < GFT_PRES_KINDS; kind++) g_series[window].presenter_ns[kind] += entry.presenter_ns[kind];
}

gft_summary guest_frame_trace_summary(void)
{
    gft_summary out;
    memset(&out, 0, sizeof out);
    out.intervals = g_intervals;
    if (g_intervals == 0u) {
        return out;
    }
    out.wall_ms_mean = (double)g_wall_total_ns / 1e6 / (double)g_intervals;
    out.cpu_ms_mean = (double)g_cpu_total_ns / 1e6 / (double)g_intervals;
    out.wall_ms_p50 = percentile(g_wall_hist, g_intervals, 0.50);
    out.wall_ms_p95 = percentile(g_wall_hist, g_intervals, 0.95);
    out.wall_ms_p99 = percentile(g_wall_hist, g_intervals, 0.99);
    out.cpu_ms_p50 = percentile(g_cpu_hist, g_intervals, 0.50);
    out.cpu_ms_p95 = percentile(g_cpu_hist, g_intervals, 0.95);
    out.cpu_ms_p99 = percentile(g_cpu_hist, g_intervals, 0.99);
    out.wall_ms_max = (double)g_wall_max_ns / 1e6;
    out.cpu_ms_max = (double)g_cpu_max_ns / 1e6;
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
        out.phase_ms_total[phase] = (double)g_phase_ns_total[phase] / 1e6;
    }
    out.unexplained_ms_total = (double)g_unexplained_total_ns / 1e6;
    uint64_t scopes_total = 0u;
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
        out.scopes_per_interval[phase] = (double)g_phase_scopes_total[phase] / (double)g_intervals;
        if (phase != GFT_SAFEPOINT) { /* counted, not timed: 30 thousand a frame, only its slow parts are scopes */
            scopes_total += g_phase_scopes_total[phase];
        }
    }
    out.trace_ms_per_interval = (double)scopes_total * (double)g_pair_cost_ns / 1e6 / (double)g_intervals;
    memcpy(out.hist, g_buckets, sizeof out.hist);
    out.depth_overflows = g_depth_overflows;
    out.fit_intervals = (uint64_t)g_fit_n;
    const double denominator = g_fit_n * g_fit_xx - g_fit_x * g_fit_x;
    if (g_fit_n > 2.0 && denominator > 1e-9) {
        const double slope = (g_fit_n * g_fit_xy - g_fit_x * g_fit_y) / denominator;
        out.fit_slope_us_per_draw = slope * 1000.0;
        out.fit_intercept_ms = (g_fit_y - slope * g_fit_x) / g_fit_n;
        const double variance_y = g_fit_n * g_fit_yy - g_fit_y * g_fit_y;
        out.fit_r_squared = variance_y > 1e-9 ? (slope * slope * denominator) / variance_y : 0.0;
    }
    return out;
}

static const char *const phase_names[GFT_PHASES] = {
    "guest code and memory helpers", "XDK calls (D3D8/DSound HLE, push buffer encode)", "kernel calls (exclusive)",
    "safepoint (counted, its async io service and frame wait poll timed)", "push buffer decode", "Swap capture (clone, SetTexture history, texture bytes)",
    "blocked: presenter job slot", "blocked: presenter job running", "vblank pace sleep", "audio work route"};
static const char *const presenter_names[GFT_PRES_KINDS] = {"frame job",       "texture sync waits", "texture decode", "pipeline create",
                                                            "module translate", "vblank op"};
static const char *const event_names[GFT_EV_KINDS] = {"kernel call",          "XDK call",       "safepoint",         "waited for the presenter job slot",
                                                      "presenter job (blocked)", "audio pump",    "Swap capture",      "push buffer decode",
                                                      "presenter texture decodes", "presenter pipeline creation", "presenter module translation"};

static void print_event(FILE *out, const gft_event *event, const gft_names *names)
{
    fprintf(out, " [%s", event_names[event->kind]);
    if (event->kind == GFT_EV_KERNEL) {
        const char *name = names != NULL && names->kernel_name != NULL ? names->kernel_name((unsigned)event->argument) : NULL;
        fprintf(out, " %s (ordinal %llu)", name != NULL ? name : "?", (unsigned long long)event->argument);
    } else if (event->kind == GFT_EV_XDK) {
        const char *name = names != NULL && names->xdk_name != NULL ? names->xdk_name((uint32_t)event->argument) : NULL;
        fprintf(out, " %s (0x%08llX)", name != NULL ? name : "?", (unsigned long long)event->argument);
    } else if (event->kind == GFT_EV_SYNC_EXEC || event->kind == GFT_EV_SYNC_SLOT) {
        static const char *const kinds[5] = {"other", "vblank", "observer", "serial frame", "target register"};
        fprintf(out, " %s", event->argument < 5u ? kinds[event->argument] : "?");
    }
    if (event->count > 1u) {
        fprintf(out, " x%llu", (unsigned long long)event->count);
    }
    fprintf(out, " %.1f ms]", (double)event->ns / 1e6);
}

void guest_frame_trace_print(FILE *out, const gft_names *names)
{
    if (!g_enabled || g_intervals == 0u) {
        return;
    }
    const gft_summary sum = guest_frame_trace_summary();
    fprintf(out,
            "guest frame trace (T1289)  %llu vblank intervals of the Swap thread: wall %.3f ms mean (p50 %.2f, p95 %.2f, p99 %.2f, max %.1f), thread CPU %.3f ms mean "
            "(p50 %.2f, p95 %.2f, p99 %.2f, max %.1f), %llu TSC ticks per ms, scope depth overflows %llu\n",
            (unsigned long long)sum.intervals, sum.wall_ms_mean, sum.wall_ms_p50, sum.wall_ms_p95, sum.wall_ms_p99, sum.wall_ms_max, sum.cpu_ms_mean,
            sum.cpu_ms_p50, sum.cpu_ms_p95, sum.cpu_ms_p99, sum.cpu_ms_max, (unsigned long long)g_ticks_per_ms, (unsigned long long)sum.depth_overflows);
    fprintf(out, "guest frame trace (T1289)  wall ms per interval <10 %llu, <15 %llu, <18 %llu, <22 %llu, <34 %llu, <50 %llu, <100 %llu, >=100 %llu\n",
            (unsigned long long)sum.hist[0], (unsigned long long)sum.hist[1], (unsigned long long)sum.hist[2], (unsigned long long)sum.hist[3],
            (unsigned long long)sum.hist[4], (unsigned long long)sum.hist[5], (unsigned long long)sum.hist[6], (unsigned long long)sum.hist[7]);
    fprintf(out, "guest frame trace (T1289)  mean ms per interval by phase (guest thread WALL, exclusive):");
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
        fprintf(out, " %s %.3f%s", phase_names[phase], sum.phase_ms_total[phase] / (double)sum.intervals, phase + 1u < GFT_PHASES ? "," : "");
    }
    fprintf(out, "\n");
    fprintf(out, "guest frame trace (T1289)  scopes per interval:");
    for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
        fprintf(out, " %s %.0f%s", phase_names[phase], sum.scopes_per_interval[phase], phase + 1u < GFT_PHASES ? "," : "");
    }
    fprintf(out, "; the trace itself costs about %.3f ms of guest thread CPU per interval (%llu ns per scope measured here, --no-guest-frame-trace turns it off)\n",
            sum.trace_ms_per_interval, (unsigned long long)g_pair_cost_ns);
    fprintf(out, "guest frame trace (T1289)  wall not explained by thread CPU or a watched wait (descheduled, or blocked where no scope watches): %.3f ms per interval\n",
            sum.unexplained_ms_total / (double)sum.intervals);
    fprintf(out, "guest frame trace (T1289)  slow events seen (1 ms or more each), total ms and count:");
    for (unsigned kind = 0u; kind < GFT_EV_KINDS; kind++) {
        if (g_event_totals_count[kind] != 0u) {
            fprintf(out, " %s %.1f ms x%llu;", event_names[kind], (double)g_event_totals_ns[kind] / 1e6, (unsigned long long)g_event_totals_count[kind]);
        }
    }
    fprintf(out, "\n");
    fprintf(out, "guest frame trace (T1289)  guest cost model, %llu intervals with one Swap and under 60 ms: thread CPU = %.2f ms + %.2f us per draw (r squared %.2f, how much of the CPU variance the draws explain)\n",
            (unsigned long long)sum.fit_intervals, sum.fit_intercept_ms, sum.fit_slope_us_per_draw, sum.fit_r_squared);
    for (unsigned bucket = 0u; bucket < GFT_DRAW_BUCKETS; bucket++) {
        if (g_by_draws[bucket].intervals == 0u) {
            continue;
        }
        const double count = (double)g_by_draws[bucket].intervals;
        const double draws = (double)g_by_draws[bucket].draws / count;
        const double per_draw = draws > 0.0 ? 1.0 / draws : 0.0;
        fprintf(out,
                "guest frame trace (T1289)  by draws per frame, %u to %u: %llu frames, %.0f draws, wall %.2f ms, thread CPU %.2f ms (%.2f us per draw); us per draw by phase: "
                "code %.2f, xdk %.2f, decode %.2f, capture %.2f, safepoint %.2f, audio %.2f, kernel %.2f; blocked ms per frame: slot %.2f, presenter job %.2f, pace %.2f\n",
                bucket * 100u, bucket + 1u == GFT_DRAW_BUCKETS ? 9999u : bucket * 100u + 99u, (unsigned long long)g_by_draws[bucket].intervals, draws,
                (double)g_by_draws[bucket].wall_ns / 1e6 / count, (double)g_by_draws[bucket].cpu_ns / 1e6 / count,
                (double)g_by_draws[bucket].cpu_ns / 1e3 / count * per_draw,
                (double)g_by_draws[bucket].phase_ns[GFT_GUEST] / 1e3 / count * per_draw, (double)g_by_draws[bucket].phase_ns[GFT_XDK] / 1e3 / count * per_draw,
                (double)g_by_draws[bucket].phase_ns[GFT_DECODE] / 1e3 / count * per_draw, (double)g_by_draws[bucket].phase_ns[GFT_SWAP_CAPTURE] / 1e3 / count * per_draw,
                (double)g_by_draws[bucket].phase_ns[GFT_SAFEPOINT] / 1e3 / count * per_draw, (double)g_by_draws[bucket].phase_ns[GFT_AUDIO] / 1e3 / count * per_draw,
                (double)g_by_draws[bucket].phase_ns[GFT_KERNEL] / 1e3 / count * per_draw,
                (double)g_by_draws[bucket].phase_ns[GFT_SYNC_SLOT] / 1e6 / count, (double)g_by_draws[bucket].phase_ns[GFT_SYNC_EXEC] / 1e6 / count,
                (double)g_by_draws[bucket].phase_ns[GFT_PACE] / 1e6 / count);
    }
    for (unsigned window = 0u; window < GFT_SERIES_WINDOWS; window++) {
        if (g_series[window].intervals == 0u) {
            continue;
        }
        const double count = (double)g_series[window].intervals;
        fprintf(out,
                "guest frame trace (T1289)  series, title frames %llu to %llu (%llu intervals): wall %.2f ms mean (max %.1f), thread CPU %.2f ms mean; mean ms by phase: "
                "code %.2f, xdk %.2f, decode %.2f, capture %.2f, safepoint %.2f, audio %.2f, kernel %.2f, slot wait %.2f, presenter job %.2f, pace %.2f; "
                "presenter per interval: frame job %.2f, texture decode %.2f, pipeline create %.2f\n",
                (unsigned long long)g_series[window].first_frame, (unsigned long long)g_series[window].last_frame,
                (unsigned long long)g_series[window].intervals, (double)g_series[window].wall_ns / 1e6 / count, (double)g_series[window].wall_max_ns / 1e6,
                (double)g_series[window].cpu_ns / 1e6 / count, (double)g_series[window].phase_ns[GFT_GUEST] / 1e6 / count,
                (double)g_series[window].phase_ns[GFT_XDK] / 1e6 / count, (double)g_series[window].phase_ns[GFT_DECODE] / 1e6 / count,
                (double)g_series[window].phase_ns[GFT_SWAP_CAPTURE] / 1e6 / count, (double)g_series[window].phase_ns[GFT_SAFEPOINT] / 1e6 / count,
                (double)g_series[window].phase_ns[GFT_AUDIO] / 1e6 / count, (double)g_series[window].phase_ns[GFT_KERNEL] / 1e6 / count,
                (double)g_series[window].phase_ns[GFT_SYNC_SLOT] / 1e6 / count, (double)g_series[window].phase_ns[GFT_SYNC_EXEC] / 1e6 / count,
                (double)g_series[window].phase_ns[GFT_PACE] / 1e6 / count, (double)g_series[window].presenter_ns[GFT_PRES_FRAME_JOB] / 1e6 / count,
                (double)g_series[window].presenter_ns[GFT_PRES_TEXTURE_DECODE] / 1e6 / count, (double)g_series[window].presenter_ns[GFT_PRES_PIPELINE] / 1e6 / count);
    }
    record ordered[GFT_SLOWEST];
    memcpy(ordered, g_slowest, sizeof ordered);
    for (unsigned a = 0u; a < g_slowest_count; a++) {
        for (unsigned b = a + 1u; b < g_slowest_count; b++) {
            if (ordered[b].wall_ns > ordered[a].wall_ns) {
                const record swap = ordered[a];
                ordered[a] = ordered[b];
                ordered[b] = swap;
            }
        }
    }
    for (unsigned rank = 0u; rank < g_slowest_count; rank++) {
        const record *entry = &ordered[rank];
        fprintf(out, "guest frame trace (T1289)  slowest %u: interval %llu (title frame %llu, %llu draws, %llu Swaps): wall %.1f ms, thread CPU %.1f ms; guest",
                rank + 1u, (unsigned long long)entry->index, (unsigned long long)entry->title_frame, (unsigned long long)entry->draws, (unsigned long long)entry->swaps,
                (double)entry->wall_ns / 1e6, (double)entry->cpu_ns / 1e6);
        static const char *const shorts[GFT_PHASES] = {"code", "xdk", "kernel", "safepoint", "decode", "capture", "slot", "presenter", "pace", "audio"};
        for (unsigned phase = 0u; phase < GFT_PHASES; phase++) {
            if (entry->phase_ns[phase] >= 100000u) {
                fprintf(out, " %s %.1f", shorts[phase], (double)entry->phase_ns[phase] / 1e6);
            }
        }
        fprintf(out, "; presenter in the interval:");
        for (unsigned kind = 0u; kind < GFT_PRES_KINDS; kind++) {
            if (entry->presenter_ns[kind] >= 100000u) {
                fprintf(out, " %s %.1f (x%llu)", presenter_names[kind], (double)entry->presenter_ns[kind] / 1e6, (unsigned long long)entry->presenter_count[kind]);
            }
        }
        fprintf(out, "; events:");
        for (unsigned index = 0u; index < entry->event_count; index++) {
            print_event(out, &entry->events[index], names);
        }
        if (entry->event_count == 0u) {
            fprintf(out, " none of 1 ms or more");
        }
        fprintf(out, "\n");
    }
}
