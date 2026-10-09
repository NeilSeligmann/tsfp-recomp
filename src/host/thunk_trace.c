/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The shared ordered call trace. See thunk_trace.h for why there is only one.
 */

#include "thunk_trace.h"

#include <pthread.h>

#include "call_profile.h"

/* Thread-local, like every guest register. The thread id has to be per-thread for
 * the same reason the guest register file is: there is one host thread per guest
 * thread and nothing else distinguishes them. */
#if defined(__GNUC__) || defined(__clang__)
#define TRACE_TLS __thread
#else
#define TRACE_TLS _Thread_local
#endif

static thunk_trace_entry g_trace[THUNK_TRACE_MAX];
static size_t g_trace_count;
/* Sized by the enum rather than by a literal. The array was `[2]` and a third kind
 * would have written past it; one name is why that cannot happen again. */
static uint64_t g_total[THUNK_KIND_COUNT];

/* See the header: never held across anything that can host_run_stop. */
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned next_thread_id = 1u;
static TRACE_TLS unsigned this_thread_id;
/* T821: this thread's own dispatch count, read by the spin detector (async_io_spin.c). */
static TRACE_TLS uint64_t this_thread_dispatches;

/* T1492: this thread's last THUNK_TAIL_MAX calls, a ring written without the lock (it is this
 * thread's own), and the run's last THUNK_FAILURE_MAX non-success kernel statuses (shared, under the lock). */
static TRACE_TLS thunk_trace_entry this_tail[THUNK_TAIL_MAX];
static TRACE_TLS uint64_t this_tail_count;
static thunk_trace_entry g_failures[THUNK_FAILURE_MAX];
static uint64_t g_failure_total;

bool thunk_trace_status_is_failure(uint32_t result)
{
    return (result >> 30) >= 2u;
}

/* The caller holds trace_lock. */
static void note_failure_locked(const thunk_trace_entry *entry)
{
    g_failures[g_failure_total % THUNK_FAILURE_MAX] = *entry;
    g_failure_total++;
}

unsigned thunk_trace_thread_tail(thunk_trace_entry *out, unsigned max)
{
    const uint64_t kept = this_tail_count < THUNK_TAIL_MAX ? this_tail_count : THUNK_TAIL_MAX;
    const unsigned want = max < kept ? max : (unsigned)kept;
    for (unsigned i = 0; i < want; i++) {
        out[i] = this_tail[(this_tail_count - want + i) % THUNK_TAIL_MAX];
    }
    return want;
}

unsigned thunk_trace_recent_failures(thunk_trace_entry *out, unsigned max, uint64_t *total)
{
    pthread_mutex_lock(&trace_lock);
    const uint64_t kept = g_failure_total < THUNK_FAILURE_MAX ? g_failure_total : THUNK_FAILURE_MAX;
    const unsigned want = max < kept ? max : (unsigned)kept;
    for (unsigned i = 0; i < want; i++) {
        out[i] = g_failures[(g_failure_total - want + i) % THUNK_FAILURE_MAX];
    }
    if (total) {
        *total = g_failure_total;
    }
    pthread_mutex_unlock(&trace_lock);
    return want;
}

uint64_t thunk_trace_thread_dispatches(void)
{
    return this_thread_dispatches;
}

unsigned thunk_trace_thread_id(void)
{
    if (this_thread_id == 0u) {
        pthread_mutex_lock(&trace_lock);
        this_thread_id = next_thread_id++;
        pthread_mutex_unlock(&trace_lock);
    }
    return this_thread_id;
}

/* Index into g_total. Bounded here rather than trusted, because an out-of-range
 * kind would otherwise be a write past the array.
 *
 * AN UNKNOWN KIND FALLS BACK TO ORDINAL rather than being dropped, which is the
 * pre-existing behaviour kept deliberately: losing a call from the totals is worse
 * than attributing it to the wrong column, because the total is what tells a reader
 * whether the trace they are looking at is complete. */
static size_t total_index(thunk_kind kind)
{
    if ((unsigned)kind >= (unsigned)THUNK_KIND_COUNT) {
        return (size_t)THUNK_KIND_ORDINAL;
    }
    return (size_t)kind;
}

static size_t append_entry(thunk_kind kind, unsigned ordinal, uint32_t address,
                           uint32_t return_address, uint32_t result, bool implemented,
                           bool result_known)
{
    size_t index = THUNK_TRACE_NO_SLOT;
    this_thread_dispatches++;
    const thunk_trace_entry mine = {.thread = this_thread_id, .kind = kind, .ordinal = ordinal,
                                    .address = address, .return_address = return_address,
                                    .result = result, .result_known = result_known,
                                    .implemented = implemented};
    this_tail[this_tail_count % THUNK_TAIL_MAX] = mine;
    this_tail_count++;
    pthread_mutex_lock(&trace_lock);
    g_total[total_index(kind)]++;
    if (result_known && kind == THUNK_KIND_ORDINAL && thunk_trace_status_is_failure(result)) {
        note_failure_locked(&mine);
    }
    if (g_trace_count < THUNK_TRACE_MAX) {
        index = g_trace_count;
        g_trace[index].thread = this_thread_id;
        g_trace[index].kind = kind;
        g_trace[index].ordinal = ordinal;
        g_trace[index].address = address;
        g_trace[index].return_address = return_address;
        g_trace[index].result = result;
        g_trace[index].result_known = result_known;
        g_trace[index].implemented = implemented;
        g_trace_count++;
    }
    pthread_mutex_unlock(&trace_lock);
    /* T422, default off: outside the lock because it may stop the run (a siglongjmp). */
    call_profile_note_dispatch(kind, ordinal, address, return_address, result_known, result);
    return index;
}

size_t thunk_trace_append(thunk_kind kind, unsigned ordinal, uint32_t address,
                          uint32_t return_address, uint32_t result, bool implemented)
{
    return append_entry(kind, ordinal, address, return_address, result, implemented, true);
}

size_t thunk_trace_append_pending(thunk_kind kind, unsigned ordinal, uint32_t address,
                                  uint32_t return_address, bool implemented)
{
    return append_entry(kind, ordinal, address, return_address, 0u, implemented, false);
}

void thunk_trace_patch_result(size_t slot, uint32_t result)
{
    call_profile_note_return(result);
    /* T1492: the most recent call of this thread still pending is the one returning (calls nest
     * LIFO), whether or not the first-2048 trace still has a slot for it. */
    for (uint64_t back = 1u; back <= (this_tail_count < THUNK_TAIL_MAX ? this_tail_count : THUNK_TAIL_MAX); back++) {
        thunk_trace_entry *pending = &this_tail[(this_tail_count - back) % THUNK_TAIL_MAX];
        if (pending->result_known) {
            continue;
        }
        pending->result = result;
        pending->result_known = true;
        if (pending->kind == THUNK_KIND_ORDINAL && thunk_trace_status_is_failure(result)) {
            pthread_mutex_lock(&trace_lock);
            note_failure_locked(pending);
            pthread_mutex_unlock(&trace_lock);
        }
        break;
    }
    if (slot >= THUNK_TRACE_MAX) {
        return;
    }
    pthread_mutex_lock(&trace_lock);
    if (slot < g_trace_count) {
        g_trace[slot].result = result;
        g_trace[slot].result_known = true;
    }
    pthread_mutex_unlock(&trace_lock);
}

const thunk_trace_entry *thunk_trace_entries(size_t *count)
{
    pthread_mutex_lock(&trace_lock);
    const size_t snapshot = g_trace_count;
    pthread_mutex_unlock(&trace_lock);
    if (count) {
        *count = snapshot;
    }
    return g_trace;
}

uint64_t thunk_trace_total(void)
{
    pthread_mutex_lock(&trace_lock);
    uint64_t snapshot = 0u;
    for (unsigned kind = 0; kind < (unsigned)THUNK_KIND_COUNT; kind++) {
        snapshot += g_total[kind];
    }
    pthread_mutex_unlock(&trace_lock);
    return snapshot;
}

uint64_t thunk_trace_total_of_kind(thunk_kind kind)
{
    pthread_mutex_lock(&trace_lock);
    const uint64_t snapshot = g_total[total_index(kind)];
    pthread_mutex_unlock(&trace_lock);
    return snapshot;
}

void thunk_trace_reset(void)
{
    pthread_mutex_lock(&trace_lock);
    g_trace_count = 0;
    g_failure_total = 0u;
    this_tail_count = 0u;
    for (unsigned kind = 0; kind < (unsigned)THUNK_KIND_COUNT; kind++) {
        g_total[kind] = 0u;
    }
    pthread_mutex_unlock(&trace_lock);
}
