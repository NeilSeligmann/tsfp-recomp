/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T422: a whole-run call profile for the shared HLE boundary. DEFAULT OFF, and when off every entry
 * point below returns before touching anything, so a normal run is byte-identical.
 *
 * WHY IT EXISTS. `thunk_trace` keeps the first 2048 calls and a total. A boot that no longer stops
 * (the six-flag steady state after T368) makes hundreds of thousands of dispatches and the first
 * 2048 say nothing about the loop it settled into. The profile counts EVERY dispatch of either kind
 * per (kind, address-or-ordinal, caller) over the whole run, and keeps per guest thread the total,
 * the last CALL_PROFILE_RING dispatches, and the call it is inside right now (a thread blocked in a
 * wait is exactly a thread whose current call has not returned).
 *
 * DETERMINISM. Counts are functions of guest progress. The one wall-clock field is the age of a
 * thread's last dispatch, printed only in the per thread block and named as such. To compare two
 * boots cut the run by guest progress, not by the watchdog: `call_profile_set_limit` stops the run
 * when one address has been dispatched exactly N times (the call that would be the N+1th is
 * counted in the `thunk_trace` totals and recorded pending but is not counted here and is not run).
 * The stop is HOST_STOP_BUDGET, the step budget stop that nothing else used.
 *
 * Counting is locked, never held across host_run_stop (a siglongjmp releases nothing).
 */

#ifndef TSFP_HOST_CALL_PROFILE_H
#define TSFP_HOST_CALL_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "thunk_trace.h"

/* Distinct (kind, id, caller) rows kept. A full table counts the overflow and drops the row. */
#define CALL_PROFILE_ROWS 16384u
/* Guest threads tracked, by the stable thunk_trace thread id (1 is the host thread). */
#define CALL_PROFILE_THREADS 16u
/* The last dispatches kept per thread. */
#define CALL_PROFILE_RING 64u

typedef struct {
    thunk_kind kind;
    uint32_t id;      /* ordinal for THUNK_KIND_ORDINAL, guest address otherwise */
    uint32_t caller;  /* return address the guest pushed */
    uint64_t count;
} call_profile_row;

typedef struct {
    thunk_kind kind;
    uint32_t id;
    uint32_t caller;
    uint32_t result;
    bool returned;
    uint64_t sequence; /* this thread's dispatch number, from 1 */
} call_profile_event;

typedef struct {
    unsigned thread;
    uint64_t dispatches;
    uint64_t returns;
    bool in_flight;                 /* the current call has not returned */
    call_profile_event current;     /* the last dispatch, valid when dispatches > 0 */
    call_profile_event recent[CALL_PROFILE_RING]; /* oldest first */
    size_t recent_count;
    uint64_t idle_ms;               /* WALL CLOCK: time since the last dispatch or return */
} call_profile_thread;

typedef struct {
    const char *(*ordinal_name)(unsigned ordinal);
    const char *(*xdk_name)(uint32_t address);
} call_profile_names;

/** Turn the profile on or off and clear it. Quiescent only. */
void call_profile_enable(bool enabled);
bool call_profile_enabled(void);

/** Stop the run once (kind, id) has been dispatched `count` times. False when `count` is 0 or the
 * profile is off. One limit only. */
bool call_profile_set_limit(thunk_kind kind, uint32_t id, uint64_t count);

/** True once the limit stopped a dispatch. */
bool call_profile_limit_reached(void);

/** Called by thunk_trace for every append. `known` means the result is already final. May stop the
 * run (HOST_STOP_BUDGET) when the limit is exceeded. */
void call_profile_note_dispatch(thunk_kind kind, unsigned ordinal, uint32_t address,
                                uint32_t return_address, bool known, uint32_t result);

/** Called by thunk_trace when this thread's call returns. */
void call_profile_note_return(uint32_t result);

/** The return-address-like words on the stopping thread's guest stack when the limit stopped it,
 * innermost first. A word counts when it points into guest code just after a call instruction
 * (E8 rel32, FF /2). A HEURISTIC stack scrape, not an unwind: stale words below the live frames and
 * data that looks like a return address can appear, and a frame that does not push one is missed. */
#define CALL_PROFILE_STACK 48u
size_t call_profile_stack(uint32_t *out, size_t capacity);

/** The guest code span the stack scrape accepts return words from. Without it the scrape is off. */
void call_profile_set_code_range(uint32_t low, uint32_t high);

/** Total dispatches counted, and rows dropped because the table was full. */
uint64_t call_profile_total(void);
uint64_t call_profile_dropped(void);

/** Copy up to `capacity` rows, busiest first. Returns the number copied. */
size_t call_profile_rows(call_profile_row *out, size_t capacity);

/** Dispatches of one (kind, id) summed over callers. */
uint64_t call_profile_count_of(thunk_kind kind, uint32_t id);

/** Copy the per thread state. Returns the number of threads that dispatched. */
size_t call_profile_threads(call_profile_thread *out, size_t capacity);

/** Print the report: per address totals with their busiest callers and per thread state. */
void call_profile_report(FILE *out, size_t top_addresses, const call_profile_names *names);

#endif /* TSFP_HOST_CALL_PROFILE_H */
