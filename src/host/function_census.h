/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T821: a READ-ONLY census of the lifted dispatcher's indirect call targets. DEFAULT OFF, and when off
 * `function_census_note` is one load and a branch, so a normal run is byte-identical. OBSERVATION ONLY,
 * NOT A MODEL: it changes no guest state and no dispatch decision.
 *
 * WHY IT EXISTS. After two A presses the front end sits in one slot state for thousands of presents with no stop and no
 * new HLE call, so the HLE census (`--profile-calls`) cannot say which guest function is running. The title reaches
 * every screen update, state record function and widget callback through an INDIRECT call (a function pointer from a
 * record table, `call [reg]`), and every such call goes through `recomp_lookup_manual` first (RECOMP_ICALL,
 * RECOMP_ICALL_SAFE, RECOMP_ICALL_SAFE_AT and RECOMP_ITAIL all do). The census counts the targets there.
 *
 * WHAT IT SEES: every indirect call and indirect tail jump of every guest thread that is not answered by a guarded
 * direct arm (the lifter only emits those for call sites a recorded feedback run narrowed, and this lift has none).
 * DIRECT calls (`call rel32`, lifted as a C call) are NOT seen: a function that only direct calls reach shows through
 * the indirect function above it. Per target it keeps the total, the first present, the first thread, the return address
 * the guest had pushed at the first call (the caller's resume point for a call, the enclosing function's return for a
 * tail jump), the last present, the calls in the last two presents and how many presents called it. It also keeps the
 * order targets first appeared in, and, for an optional present window, every indirect call in order (the dispatcher's
 * path through one frame).
 *
 * PRESENT INDEX. The number of presents completed so far, read through a callback (`d3d8_frame_queue_total` in the
 * host) so this module has no GPU dependency. It is the guest's own present count, never wall time.
 *
 * Locked with one mutex, never held across a stop.
 */

#ifndef TSFP_HOST_FUNCTION_CENSUS_H
#define TSFP_HOST_FUNCTION_CENSUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Distinct targets kept. A full table counts the overflow. */
#define FUNCTION_CENSUS_TARGETS 4096u
/* Calls kept in the ordered window trace. A full trace counts the overflow. */
#define FUNCTION_CENSUS_TRACE 4096u

typedef struct {
    uint32_t target;
    unsigned thread;          /* thunk_trace thread id of the first call */
    uint32_t first_caller;    /* the dword at the guest stack pointer at the first call */
    uint64_t first_present;   /* presents completed when it was first called */
    uint64_t first_sequence;  /* its index among all indirect calls, from 1 */
    uint64_t calls;
    uint64_t last_present;
    uint64_t presents_called; /* distinct presents in which it was called */
    uint64_t calls_in_last;   /* calls in `last_present` */
    uint64_t calls_in_previous; /* calls in the last earlier present that called it */
    uint32_t previous_present_gap; /* last_present minus that earlier present, 0 if none */
} function_census_target;

typedef struct {
    uint64_t present;
    uint64_t sequence;
    uint32_t target;
    uint32_t caller;
    unsigned thread;
} function_census_event;

/** Turn the census on or off and clear it. Quiescent only. */
void function_census_enable(bool enabled);
bool function_census_enabled(void);

/** The present count source (completed presents). NULL reads 0. Configuration, kept across enable. */
void function_census_set_present_source(uint64_t (*present)(void));

/** Keep every indirect call made while the present count is in [first, last] in order (up to
 * FUNCTION_CENSUS_TRACE). Configuration, kept across enable. */
void function_census_set_window(uint64_t first, uint64_t last);

/** One indirect call. Called from `recomp_lookup_manual` with the target and the guest's current stack
 * pointer (to read the pushed return address). No effect when off. */
void function_census_note(uint32_t target, uint32_t guest_esp);

/** Calls counted, distinct targets, overflowed targets. */
uint64_t function_census_total(void);
size_t function_census_target_count(void);
uint64_t function_census_overflow(void);

/** The row for `target`, false when it was never called (or was dropped by overflow). */
bool function_census_get(uint32_t target, function_census_target *out);

/** Rows in order of first appearance. Copies up to `capacity`, returns the number copied. */
size_t function_census_targets(function_census_target *out, size_t capacity);

/** The ordered window events, oldest first. Returns the number copied. */
size_t function_census_events(function_census_event *out, size_t capacity);
uint64_t function_census_event_overflow(void);

/** T1502 phase marking. Dump the census accumulated since the last mark to `path` in a machine-readable text format
 * and reset the counters (the table, the totals and the overflow count, never the on/off state or the window). Returns
 * false when the file cannot be written (the counters are reset anyway so a phase never leaks into the next). */
bool function_census_dump_phase(const char *path);

/** SIGUSR1 phase marking for a long interactive run, the same protocol as the CPU sampler: the signal handler only
 * counts a request, a helper thread writes `FILE.<phase>` (phase = first word of the control file `FILE.phase`, else
 * 1, 2, 3...), resets the census and writes `FILE.ack` (`<dumps done> <phase>`). `FILE.pid` holds the host pid.
 * A SIGUSR1 handler installed earlier (the sampler's) is chained, so both sources mark on the same signal. Needs the
 * census enabled. The exit report (`function_census_report`) keeps the counts since the last mark. */
bool function_census_phases_start(const char *path);
void function_census_phases_stop(void);

/** Print the census: the totals, every target in order of first appearance, the targets still called in the last
 * presents, and the window trace. */
void function_census_report(FILE *out);

#endif /* TSFP_HOST_FUNCTION_CENSUS_H */
