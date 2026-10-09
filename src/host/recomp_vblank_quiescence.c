/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_vblank_quiescence.h"
#include "host_runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool enabled_flag;
static atomic_uint getter_checks;

rvq_thread_state recomp_vblank_quiescence_classify(const kernel_thread_record *record)
{
    if (record->terminated) {
        return RVQ_TERMINATED;
    }
    if (record->finished) {
        return RVQ_HOST_STOPPED;
    }
    if (!record->started || record->suspend_count != 0u) {
        return RVQ_UNSTARTED;
    }
    if (record->block_state == KERNEL_THREAD_BLOCK_THREAD) {
        return RVQ_BLOCKED;
    }
    if (record->block_state == KERNEL_THREAD_BLOCK_HOST_TIMER) {
        return RVQ_HOST_TIMER;
    }
    return RVQ_RUNNABLE;
}

const char *recomp_vblank_quiescence_state_name(rvq_thread_state state)
{
    switch (state) {
    case RVQ_UNSTARTED: return "unstarted";
    case RVQ_TERMINATED: return "terminated";
    case RVQ_HOST_STOPPED: return "host-stopped";
    case RVQ_BLOCKED: return "blocked";
    case RVQ_HOST_TIMER: return "host-timer";
    case RVQ_RUNNABLE: return "runnable";
    }
    return "invalid";
}

const char *recomp_vblank_quiescence_reason_name(rvq_reason reason)
{
    switch (reason) {
    case RVQ_HOLDS: return "holds";
    case RVQ_REFUSE_DELIVERER_UNKNOWN: return "delivering thread has no live started record";
    case RVQ_REFUSE_RUNNABLE: return "another guest thread is runnable and could sample the counter";
    case RVQ_REFUSE_HOST_TIMER: return "another guest thread sleeps to a host deadline";
    case RVQ_REFUSE_HOST_STOPPED: return "another guest thread ended by a host stop, not a guest exit";
    case RVQ_REFUSE_BLOCKED_ON_ACTIVE: return "another guest thread is blocked on a thread that is not quiescent";
    }
    return "invalid";
}

static const kernel_thread_record *find_record(const kernel_thread_record *records, unsigned count,
                                               uint32_t handle)
{
    for (unsigned i = 0u; i < count; i++) {
        if (records[i].in_use && records[i].handle == handle) {
            return &records[i];
        }
    }
    return NULL;
}

static bool is_non_reader(const kernel_thread_record *record, const uint32_t *starts, unsigned count)
{
    for (unsigned i = 0u; i < count; i++) {
        if (record->start_routine == starts[i]) {
            return true;
        }
    }
    return false;
}

static const uint32_t default_non_reader_starts[] = {RVQ_NON_READER_NET_POLL_START,
                                                     RVQ_NON_READER_LOADER_START};

const uint32_t *recomp_vblank_quiescence_default_non_readers(unsigned *count)
{
    *count = (unsigned)(sizeof default_non_reader_starts / sizeof default_non_reader_starts[0]);
    return default_non_reader_starts;
}

rvq_verdict recomp_vblank_quiescence_evaluate(const kernel_thread_record *records, unsigned count,
                                              uint32_t deliverer)
{
    unsigned table_count = 0u;
    const uint32_t *table = recomp_vblank_quiescence_default_non_readers(&table_count);
    return recomp_vblank_quiescence_evaluate_with(records, count, deliverer, table, table_count);
}

rvq_verdict recomp_vblank_quiescence_evaluate_with(const kernel_thread_record *records,
                                                   unsigned count, uint32_t deliverer,
                                                   const uint32_t *non_reader_starts,
                                                   unsigned non_reader_count)
{
    return recomp_vblank_quiescence_evaluate_parked(records, count, deliverer, non_reader_starts,
                                                    non_reader_count, NULL, 0u);
}

static bool in_handles(const uint32_t *handles, unsigned count, uint32_t handle)
{
    for (unsigned i = 0u; handles && i < count; i++) {
        if (handles[i] == handle) {
            return true;
        }
    }
    return false;
}

rvq_verdict recomp_vblank_quiescence_evaluate_parked(const kernel_thread_record *records,
                                                     unsigned count, uint32_t deliverer,
                                                     const uint32_t *non_reader_starts,
                                                     unsigned non_reader_count,
                                                     const uint32_t *parked_handles,
                                                     unsigned parked_count)
{
    rvq_verdict verdict = {.holds = false, .reason = RVQ_REFUSE_DELIVERER_UNKNOWN};
    const kernel_thread_record *self = records ? find_record(records, count, deliverer) : NULL;
    if (!self || recomp_vblank_quiescence_classify(self) != RVQ_RUNNABLE) {
        return verdict;
    }
    /* Parked: UNSTARTED, or BLOCKED on the deliverer or on a parked thread. A thread blocked on a
     * TERMINATED target is about to wake at a host timing moment, so it is not parked. The set
     * grows to a least fixed point, a wait cycle never enters it. */
    bool parked[KERNEL_THREAD_MAX] = {false};
    const unsigned limit = count < KERNEL_THREAD_MAX ? count : KERNEL_THREAD_MAX;
    /* T592: a thread the caller proves parked by location is parked while it is runnable (a thread
     * in any other state keeps its own rule), it is still examined and counted. */
    for (unsigned i = 0u; i < limit; i++) {
        parked[i] = records[i].in_use && records[i].handle != deliverer &&
                    in_handles(parked_handles, parked_count, records[i].handle) &&
                    recomp_vblank_quiescence_classify(&records[i]) == RVQ_RUNNABLE;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (unsigned i = 0u; i < limit; i++) {
            if (parked[i] || !records[i].in_use || records[i].handle == deliverer ||
                is_non_reader(&records[i], non_reader_starts, non_reader_count)) {
                continue;
            }
            const rvq_thread_state state = recomp_vblank_quiescence_classify(&records[i]);
            bool now = state == RVQ_UNSTARTED;
            if (state == RVQ_BLOCKED) {
                if (records[i].block_target == deliverer) {
                    now = true;
                } else {
                    for (unsigned j = 0u; j < limit; j++) {
                        if (parked[j] && records[j].handle == records[i].block_target) {
                            now = true;
                        }
                    }
                }
            }
            if (now) {
                parked[i] = true;
                changed = true;
            }
        }
    }
    verdict.holds = true;
    verdict.reason = RVQ_HOLDS;
    for (unsigned i = 0u; i < limit; i++) {
        if (!records[i].in_use || records[i].handle == deliverer ||
            is_non_reader(&records[i], non_reader_starts, non_reader_count)) {
            continue;
        }
        verdict.examined++;
        const rvq_thread_state state = recomp_vblank_quiescence_classify(&records[i]);
        if (state == RVQ_TERMINATED || parked[i] || !verdict.holds) {
            continue; /* keep counting, the first refusing thread stays the one named */
        }
        verdict.holds = false;
        verdict.handle = records[i].handle;
        verdict.start_routine = records[i].start_routine;
        verdict.reason = state == RVQ_RUNNABLE ? RVQ_REFUSE_RUNNABLE
                       : state == RVQ_HOST_TIMER ? RVQ_REFUSE_HOST_TIMER
                       : state == RVQ_HOST_STOPPED ? RVQ_REFUSE_HOST_STOPPED
                       : RVQ_REFUSE_BLOCKED_ON_ACTIVE;
    }
    return verdict;
}

void recomp_vblank_quiescence_configure(bool enabled)
{
    atomic_store(&enabled_flag, enabled);
}

bool recomp_vblank_quiescence_enabled(void)
{
    return atomic_load(&enabled_flag);
}

rvq_verdict recomp_vblank_quiescence_check(uint32_t deliverer, uint32_t site)
{
    return recomp_vblank_quiescence_check_parked(deliverer, site, NULL, 0u);
}

rvq_verdict recomp_vblank_quiescence_check_parked(uint32_t deliverer, uint32_t site,
                                                  const uint32_t *parked_handles,
                                                  unsigned parked_count)
{
    rvq_verdict verdict = {.holds = true, .reason = RVQ_HOLDS};
    if (!recomp_vblank_quiescence_enabled()) {
        return verdict;
    }
    kernel_thread_record records[KERNEL_THREAD_MAX];
    const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
    unsigned table_count = 0u;
    const uint32_t *table = recomp_vblank_quiescence_default_non_readers(&table_count);
    verdict = recomp_vblank_quiescence_evaluate_parked(records, count, deliverer, table, table_count,
                                                       parked_handles, parked_count);
    if (!verdict.holds) {
        static _Thread_local char text[192]; /* host_stop keeps the pointer */
        snprintf(text, sizeof text,
                 "vblank delivery quiescence refused: %s (thread %#x started at %#x)",
                 recomp_vblank_quiescence_reason_name(verdict.reason),
                 (unsigned)verdict.handle, (unsigned)verdict.start_routine);
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, site, 0u, text);
        abort();
    }
    return verdict;
}

bool recomp_vblank_quiescence_is_registered_reader(uint32_t return_address, uint32_t start_routine)
{
    return (return_address == RVQ_OWNER_RETURN && start_routine == RVQ_OWNER_START) ||
           (return_address == RVQ_WORKER_RETURN && start_routine == RVQ_WORKER_START);
}

void recomp_vblank_quiescence_check_getter(uint32_t handle, uint32_t return_address)
{
    if (!recomp_vblank_quiescence_enabled()) {
        return;
    }
    atomic_fetch_add(&getter_checks, 1u);
    kernel_thread_record record;
    if (!kernel_thread_get(handle, &record) ||
        !recomp_vblank_quiescence_is_registered_reader(return_address, record.start_routine)) {
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, RVQ_GETTER, 0u,
                      "vblank counter getter reached by an unregistered reader");
        abort();
    }
}

unsigned recomp_vblank_quiescence_getter_checks(void)
{
    return atomic_load(&getter_checks);
}

void recomp_vblank_quiescence_describe(const kernel_thread_record *record, char *out, unsigned size)
{
    snprintf(out, size, "member=0x%X start=0x%X state=%s", (unsigned)record->handle,
             (unsigned)record->start_routine,
             recomp_vblank_quiescence_state_name(recomp_vblank_quiescence_classify(record)));
}
