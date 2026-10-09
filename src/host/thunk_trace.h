/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ONE ordered call trace, shared by every HLE boundary.
 *
 * WHY IT IS ONE OBJECT AND NOT ONE PER BOUNDARY. The whole deliverable of a
 * bring-up run is the ORDER in which the guest asks for things. Two separate
 * arrays, one for kernel ordinals and one for XDK addresses, cannot be interleaved
 * after the fact: each is internally ordered and nothing relates them, so
 * "DirectSoundCreate came after RtlEqualString and before NtReadFile" stops being
 * recoverable. That sentence is the product. So the array is shared and each entry
 * says which KIND of boundary it crossed.
 *
 * This file is an extraction, not a new mechanism. The lock discipline, the
 * append-then-patch shape and the small stable thread ids were all written for
 * `kernel_thunk.c` and are reproduced here verbatim in behaviour, so that the
 * address-keyed XDK dispatcher reuses them instead of growing a second copy with
 * its own bugs.
 *
 * THE LOCK MUST NEVER BE HELD ACROSS ANYTHING THAT CAN STOP THE RUN.
 * `host_run_stop` is a `siglongjmp`: it unwinds nothing and releases nothing.
 * Stopping at an unimplemented ordinal or an unimplemented XDK address is the
 * EXPECTED end of a bring-up run, so a lock held across that call would deadlock
 * the reporting path on essentially every run. Every critical section below is
 * therefore a few plain stores with no calls in it.
 */

#ifndef TSFP_HOST_THUNK_TRACE_H
#define TSFP_HOST_THUNK_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Keep the first2048 calls in fixed ordered storage; totals remain uncapped
 * after storage fills. This is neither a ring nor a dynamic allocation. */
#define THUNK_TRACE_MAX 2048

/** Returned by thunk_trace_append when the trace is full. */
#define THUNK_TRACE_NO_SLOT ((size_t)THUNK_TRACE_MAX)

/**
 * Which boundary a traced call crossed.
 *
 * Not cosmetic. A kernel call is identified by an ORDINAL and an XDK call by an
 * ADDRESS, and the two namespaces overlap numerically -- ordinal 24 and guest VA
 * 0x18 are both small integers. A reader of the trace has to be able to tell which
 * field means anything, and so does the report.
 */
typedef enum {
    /* An xboxkrnl export, reached through the patched thunk table. */
    THUNK_KIND_ORDINAL = 0,
    /* A statically linked XDK function, reached by address. */
    THUNK_KIND_XDK,
    /* The Prcb debug-monitor notify, reached through `monitor+0x14`.
     *
     * ITS OWN KIND, NOT AN ORDINAL AND NOT AN XDK ADDRESS. It is neither: no
     * ordinal exports it and its target is a synthetic VA this host invented, so
     * folding it into either column would inflate a count that is supposed to say
     * how much of the console's own surface the guest reached. It belongs in the
     * ordered trace all the same -- a notification the guest believes succeeded is
     * a divergence from a retail console, and an invisible divergence is the one
     * kind this project cannot afford. */
    THUNK_KIND_MONITOR,
    /* Not a kind. The number of them, for sizing the per-kind totals. */
    THUNK_KIND_COUNT,
} thunk_kind;

/**
 * One entry of the ordered call trace.
 *
 * `thread` is not decoration. A single linearly-ordered array cannot by itself
 * express two interleaved guest threads: with a lock the array becomes consistent,
 * but "the order in which the guest asks for things" -- the whole deliverable of a
 * bring-up run -- is no longer recoverable from it unless each entry says which
 * thread made the call. So it says.
 */
typedef struct {
    /* 1 for the host thread that ran the entry point, 2.. for guest threads, in
     * first-call order. Small and stable, unlike a pthread_t. */
    unsigned thread;
    thunk_kind kind;
    /* Meaningful for THUNK_KIND_ORDINAL only; 0 otherwise. */
    unsigned ordinal;
    /* Meaningful for THUNK_KIND_XDK only; 0 otherwise. The guest VA of the XDK
     * function the game transferred control to. */
    uint32_t address;
    /* Guest return address the caller pushed, i.e. where in the guest this call
     * came from. The call instruction itself is a few bytes before it. */
    uint32_t return_address;
    /* What the HLE handed back in eax, meaningful only when result_known. */
    uint32_t result;
    /* False while a call is executing or when it stopped without returning. */
    bool result_known;
    /* False if nothing was implemented behind the ordinal or address. */
    bool implemented;
} thunk_trace_entry;

/**
 * A small, stable id for the calling thread, allocated on first use.
 *
 * 1 is whichever thread makes the first call of either kind, which is the host
 * thread running the entry point.
 */
unsigned thunk_trace_thread_id(void);

/** T821: how many HLE dispatches (either kind) THIS thread has made, for a spin detector. */
uint64_t thunk_trace_thread_dispatches(void);

/**
 * Append one entry and return its index, or THUNK_TRACE_NO_SLOT when full.
 *
 * The index, never "the last entry": with two guest threads appending, the last
 * entry belongs to whichever thread got there most recently, so patching it would
 * write one call's result into another call's record.
 */
size_t thunk_trace_append(thunk_kind kind, unsigned ordinal, uint32_t address,
                          uint32_t return_address, uint32_t result, bool implemented);

/** Append a call before entering the handler. Its result is unknown until patched.
 * If the handler stops via siglongjmp, it remains a recorded nonreturned call. */
size_t thunk_trace_append_pending(thunk_kind kind, unsigned ordinal, uint32_t address,
                                  uint32_t return_address, bool implemented);

/**
 * Overwrite the result of an entry this thread already appended, marking it known.
 *
 * For the stop-on-missing path, which has to record the call BEFORE stopping so the
 * trace ends with the thing that stopped it, and therefore records a result it does
 * not yet know. Out-of-range slots are ignored, so THUNK_TRACE_NO_SLOT can be passed
 * straight through without a branch at every call site.
 */
void thunk_trace_patch_result(size_t slot, uint32_t result);

/**
 * The ordered trace. `*count` is how many entries are valid.
 *
 * Returns a pointer INTO the shared array, so it is for the report phase, after
 * every guest thread has been joined. Reading it while a guest thread still runs
 * would be a torn read of entries that thread is appending.
 */
const thunk_trace_entry *thunk_trace_entries(size_t *count);

/** Total calls of either kind, including any beyond THUNK_TRACE_MAX. */
uint64_t thunk_trace_total(void);

/** Total calls of one kind, including any beyond THUNK_TRACE_MAX. */
uint64_t thunk_trace_total_of_kind(thunk_kind kind);

/** T1492: how many of a thread's most recent HLE calls are kept (a per-thread ring, never full). */
#define THUNK_TAIL_MAX 20u

/**
 * T1492: the calling thread's last calls, oldest first, up to `max` (at most THUNK_TAIL_MAX).
 * Unlike the ordered trace (the FIRST 2048 calls) this is the END of the run, which is the half
 * a stop report needs: 358 million calls leave the first 2048 irrelevant to why a run ended.
 * A call still executing has result_known false. Returns how many were written.
 */
unsigned thunk_trace_thread_tail(thunk_trace_entry *out, unsigned max);

/** T1492: how many most recent non-success kernel statuses are kept. */
#define THUNK_FAILURE_MAX 20u

/**
 * T1492: true for an NTSTATUS error or warning (severity bits 0b10 or 0b11). STATUS_PENDING
 * (0x103) is informational and is not one: the async file io path returns it by design.
 */
bool thunk_trace_status_is_failure(uint32_t result);

/**
 * T1492: the last kernel-ordinal calls (any thread) that returned a non-success status, oldest
 * first, plus the run's total of them. `*total` may exceed the entries kept. Returns how many
 * entries were written. Only THUNK_KIND_ORDINAL calls count: an XDK function's eax is not a status.
 */
unsigned thunk_trace_recent_failures(thunk_trace_entry *out, unsigned max, uint64_t *total);

/** Clear the trace and the totals. For tests. */
void thunk_trace_reset(void);

#endif /* TSFP_HOST_THUNK_TRACE_H */
