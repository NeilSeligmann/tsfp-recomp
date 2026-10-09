/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T371: the two-consumer quiescence predicate for vblank delivery. Part A is the pure predicate over
 * synthetic thread records, every state and every refusal. Part B drives real guest threads of the
 * thread model: a spinning second reader, a host timer sleep, a pseudo handle sleep and an untimed
 * thread wait, and checks that the recorded block state and the verdict follow them. */
#define _POSIX_C_SOURCE 200809L
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "nt_status.h"
#include "recomp_vblank_quiescence.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

enum { OWNER = 0x100, A = 0x101, B = 0x102, C = 0x103 };
/* A start routine standing for one a future scan proves closed. Never in the default table. */
#define PROVEN 0x5150u
static const uint32_t proven_table[] = {PROVEN};

static kernel_thread_record record(uint32_t handle, uint32_t start)
{
    kernel_thread_record r;
    memset(&r, 0, sizeof r);
    r.in_use = true;
    r.handle = handle;
    r.start_routine = start;
    r.started = true;
    return r;
}
static kernel_thread_record terminated(uint32_t handle, uint32_t start)
{
    kernel_thread_record r = record(handle, start);
    r.finished = r.terminated = r.termination_requested = true;
    return r;
}
static kernel_thread_record blocked_on(uint32_t handle, uint32_t target)
{
    kernel_thread_record r = record(handle, RVQ_WORKER_START);
    r.block_state = KERNEL_THREAD_BLOCK_THREAD;
    r.block_target = target;
    return r;
}
static rvq_verdict run(const kernel_thread_record *records, unsigned count)
{
    return recomp_vblank_quiescence_evaluate(records, count, OWNER);
}

static void test_classification(void)
{
    kernel_thread_record r = record(A, RVQ_WORKER_START);
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_RUNNABLE);
    r.block_state = KERNEL_THREAD_BLOCK_HOST_TIMER;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_HOST_TIMER);
    r.block_state = KERNEL_THREAD_BLOCK_THREAD;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_BLOCKED);
    r = record(A, 1u);
    r.started = false;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_UNSTARTED);
    r = record(A, 1u);
    r.suspend_count = 1u;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_UNSTARTED);
    r = record(A, 1u);
    r.finished = true;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_HOST_STOPPED);
    r.terminated = true;
    CHECK(recomp_vblank_quiescence_classify(&r) == RVQ_TERMINATED);
    CHECK(strcmp(recomp_vblank_quiescence_state_name(RVQ_RUNNABLE), "runnable") == 0);
    CHECK(strcmp(recomp_vblank_quiescence_state_name(RVQ_HOST_TIMER), "host-timer") == 0);
}

static void test_measured_shapes(void)
{
    /* Step 5 of the measured schedule: only the owner exists. Vacuous, examined stays 0. */
    kernel_thread_record alone[] = {record(OWNER, RVQ_OWNER_START)};
    rvq_verdict v = run(alone, 1u);
    CHECK(v.holds && v.reason == RVQ_HOLDS && v.examined == 0u);
    /* Step 9: the worker terminated before the credited event. */
    kernel_thread_record after[] = {record(OWNER, RVQ_OWNER_START), terminated(A, RVQ_WORKER_START)};
    v = run(after, 2u);
    CHECK(v.examined == 1u);
    CHECK(v.holds && v.reason == RVQ_HOLDS && v.handle == 0u);
}

static void test_refusals(void)
{
    /* The negative case: a second reader that is runnable (spinning at the getter) refuses. */
    kernel_thread_record spin[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    rvq_verdict v = run(spin, 2u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == A &&
          v.start_routine == RVQ_WORKER_START);
    /* A host deadline sleep wakes by wall time, so it refuses too. */
    kernel_thread_record timer[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    timer[1].block_state = KERNEL_THREAD_BLOCK_HOST_TIMER;
    v = run(timer, 2u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_HOST_TIMER && v.handle == A);
    /* Finished by a host stop or fault, never a guest exit. */
    kernel_thread_record stopped[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    stopped[1].finished = true;
    v = run(stopped, 2u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_HOST_STOPPED && v.handle == A);
    /* The first refusing thread is named, parked and terminated ones are skipped. */
    kernel_thread_record mixed[] = {record(OWNER, RVQ_OWNER_START), terminated(A, RVQ_WORKER_START),
                                    record(B, RVQ_WORKER_START)};
    v = run(mixed, 3u);
    CHECK(v.examined == 2u);
    CHECK(!v.holds && v.handle == B);
}

static void test_parked_threads(void)
{
    kernel_thread_record unstarted[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    unstarted[1].started = false;
    rvq_verdict v = run(unstarted, 2u);
    CHECK(v.examined == 1u);
    CHECK(v.holds);
    kernel_thread_record suspended[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    suspended[1].suspend_count = 1u;
    v = run(suspended, 2u);
    CHECK(v.examined == 1u);
    CHECK(v.holds);
    /* Blocked on the delivering thread: only the deliverer can release it. */
    kernel_thread_record on_owner[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, OWNER)};
    v = run(on_owner, 2u);
    CHECK(v.examined == 1u);
    CHECK(v.holds);
    /* A chain of waits that ends at the deliverer is parked all the way. */
    kernel_thread_record chain[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, B), blocked_on(B, OWNER)};
    v = run(chain, 3u);
    CHECK(v.examined == 2u);
    CHECK(v.holds);
    /* Blocked on a thread that never starts. */
    kernel_thread_record on_unstarted[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, B),
                                           record(B, RVQ_WORKER_START)};
    on_unstarted[2].started = false;
    v = run(on_unstarted, 3u);
    CHECK(v.examined == 2u);
    CHECK(v.holds);
    /* Blocked on a TERMINATED thread: the wait is already satisfied, the wake is a host timing
     * moment, so it is not parked. */
    kernel_thread_record on_dead[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, B),
                                      terminated(B, RVQ_WORKER_START)};
    v = run(on_dead, 3u);
    CHECK(v.examined == 2u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_BLOCKED_ON_ACTIVE && v.handle == A);
    /* Blocked on a runnable thread: that one refuses first, in table order here the waiter does. */
    kernel_thread_record on_runner[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, B),
                                        record(B, RVQ_WORKER_START)};
    v = run(on_runner, 3u);
    CHECK(v.examined == 2u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_BLOCKED_ON_ACTIVE && v.handle == A);
    /* A wait cycle never parks. */
    kernel_thread_record cycle[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, B), blocked_on(B, A)};
    v = run(cycle, 3u);
    CHECK(v.examined == 2u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_BLOCKED_ON_ACTIVE);
    /* Blocked on an unknown handle is not parked either. */
    kernel_thread_record on_unknown[] = {record(OWNER, RVQ_OWNER_START), blocked_on(A, 0x999u)};
    v = run(on_unknown, 2u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds);
}

static void test_non_reader_table(void)
{
    kernel_thread_record small[] = {record(OWNER, RVQ_OWNER_START), record(A, PROVEN)};
    /* A thread the table names may run, it can neither poll the counter nor spawn a reader. */
    rvq_verdict v = recomp_vblank_quiescence_evaluate_with(small, 2u, OWNER, proven_table, 1u);
    CHECK(v.holds && v.examined == 0u);
    /* The default table does not hold it: the same record is refused. */
    v = run(small, 2u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == A);
    /* An explicit table replaces the default, so under it the loader start 0x30160 (T593) is not
     * skipped either. */
    kernel_thread_record loader_only[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x30160u)};
    v = recomp_vblank_quiescence_evaluate_with(loader_only, 2u, OWNER, proven_table, 1u);
    CHECK(v.examined == 1u && !v.holds && v.handle == A);
    /* An explicit table replaces the default, so under it 0x3C0FE0 is not skipped. */
    kernel_thread_record other[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x3C0FE0u)};
    v = recomp_vblank_quiescence_evaluate_with(other, 2u, OWNER, proven_table, 1u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.handle == A);
    /* A reader waiting on the non-reader is not parked by it, that thread keeps running. */
    kernel_thread_record wait_on_nonreader[] = {record(OWNER, RVQ_OWNER_START),
        record(A, PROVEN), blocked_on(B, A)};
    v = recomp_vblank_quiescence_evaluate_with(wait_on_nonreader, 3u, OWNER, proven_table, 1u);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.handle == B);
}

/* T424: the default table is exactly the proven network polling thread. */
static void test_default_non_reader_table(void)
{
    unsigned count = 0u;
    const uint32_t *table = recomp_vblank_quiescence_default_non_readers(&count);
    CHECK(table != NULL && count == 2u && table[0] == 0x3C0FE0u && table[1] == 0x30160u);
    /* Neither reader start routine may ever be skipped. */
    for (unsigned i = 0u; i < count; i++) {
        CHECK(table[i] != RVQ_OWNER_START && table[i] != RVQ_WORKER_START);
    }
    /* A runnable polling thread does not refuse the delivery, and it is not even examined. */
    kernel_thread_record net_poll[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x3C0FE0u)};
    rvq_verdict v = run(net_poll, 2u);
    CHECK(v.holds && v.examined == 0u);
    /* Only the start routine decides: the same state under another start routine refuses. */
    kernel_thread_record other_start[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x3C0FE1u)};
    v = run(other_start, 2u);
    CHECK(v.examined == 1u && !v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == A);
    /* A reader that waits on the polling thread is not parked by it and still refuses. */
    kernel_thread_record reader_waits[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x3C0FE0u),
                                           blocked_on(B, A)};
    v = run(reader_waits, 3u);
    CHECK(v.examined == 1u && !v.holds && v.handle == B);
    /* A runnable second worker beside the polling thread still refuses. */
    kernel_thread_record worker_beside[] = {record(OWNER, RVQ_OWNER_START),
                                            record(A, RVQ_WORKER_START), record(B, 0x3C0FE0u)};
    v = run(worker_beside, 3u);
    CHECK(v.examined == 1u && !v.holds && v.handle == A);
    /* An explicit empty table replaces the default: nothing is skipped. */
    v = recomp_vblank_quiescence_evaluate_with(net_poll, 2u, OWNER, NULL, 0u);
    CHECK(v.examined == 1u && !v.holds && v.handle == A);
}

/* T592: handles the caller proves parked by location (the owner in the loading bar gate). */
static void test_parked_by_location(void)
{
    unsigned table_count = 0u;
    const uint32_t *table = recomp_vblank_quiescence_default_non_readers(&table_count);
    const uint32_t gate_owner[] = {OWNER};
    /* The worker delivers, the owner is runnable: refused plainly, parked once the caller says so. */
    kernel_thread_record owner_runs[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    rvq_verdict v = recomp_vblank_quiescence_evaluate_with(owner_runs, 2u, A, table, table_count);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == OWNER);
    v = recomp_vblank_quiescence_evaluate_parked(owner_runs, 2u, A, table, table_count, gate_owner, 1u);
    CHECK(v.holds && v.reason == RVQ_HOLDS && v.examined == 1u); /* examined, not skipped */
    /* No handles: exactly evaluate_with. */
    v = recomp_vblank_quiescence_evaluate_parked(owner_runs, 2u, A, table, table_count, NULL, 0u);
    CHECK(!v.holds && v.handle == OWNER);
    v = recomp_vblank_quiescence_evaluate_parked(owner_runs, 2u, A, table, table_count, gate_owner, 0u);
    CHECK(!v.holds && v.handle == OWNER);
    /* Another runnable thread is still refused, naming that thread. */
    kernel_thread_record two_run[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START),
                                      record(B, RVQ_WORKER_START)};
    v = recomp_vblank_quiescence_evaluate_parked(two_run, 3u, A, table, table_count, gate_owner, 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == B && v.examined == 2u);
    /* The deliverer is never parked by being listed: a listed deliverer is judged as the deliverer. */
    const uint32_t listed_deliverer[] = {A};
    v = recomp_vblank_quiescence_evaluate_parked(owner_runs, 2u, A, table, table_count, listed_deliverer, 1u);
    CHECK(!v.holds && v.handle == OWNER);
    /* Only a RUNNABLE thread is parked by the list, every other state keeps its own rule. */
    kernel_thread_record owner_timer[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    owner_timer[0].block_state = KERNEL_THREAD_BLOCK_HOST_TIMER;
    v = recomp_vblank_quiescence_evaluate_parked(owner_timer, 2u, A, table, table_count, gate_owner, 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_HOST_TIMER && v.handle == OWNER);
    kernel_thread_record owner_stopped[] = {terminated(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START)};
    owner_stopped[0].terminated = false; /* host stopped, never confirmed */
    v = recomp_vblank_quiescence_evaluate_parked(owner_stopped, 2u, A, table, table_count, gate_owner, 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_HOST_STOPPED);
    /* A thread blocked on a listed (parked) thread is parked through it. */
    kernel_thread_record waits_on_owner[] = {record(OWNER, RVQ_OWNER_START), record(A, RVQ_WORKER_START),
                                             blocked_on(B, OWNER)};
    v = recomp_vblank_quiescence_evaluate_parked(waits_on_owner, 3u, A, table, table_count, gate_owner, 1u);
    CHECK(v.holds && v.examined == 2u);
    /* An unlisted handle is not parked. */
    const uint32_t other[] = {B};
    v = recomp_vblank_quiescence_evaluate_parked(owner_runs, 2u, A, table, table_count, other, 1u);
    CHECK(!v.holds && v.handle == OWNER);
}

/* T593: a proven non-reader is skipped in EVERY state, so a loader sleeping to a host deadline is
 * not "parked", it is irrelevant, while the same state under an unproven start is still refused. */
static void test_non_reader_any_state(void)
{
    const uint32_t proven[] = {0x3C0FE0u, 0x30160u};
    for (unsigned entry = 0u; entry < 2u; entry++) {
        kernel_thread_record runnable = record(A, proven[entry]);
        kernel_thread_record host_timer = record(A, proven[entry]);
        host_timer.block_state = KERNEL_THREAD_BLOCK_HOST_TIMER;
        kernel_thread_record host_stopped = record(A, proven[entry]);
        host_stopped.finished = true;
        kernel_thread_record unstarted = record(A, proven[entry]);
        unstarted.started = false;
        const kernel_thread_record variants[] = {runnable, host_timer, host_stopped, unstarted,
                                                 terminated(A, proven[entry])};
        for (unsigned i = 0u; i < 5u; i++) {
            kernel_thread_record both[] = {record(OWNER, RVQ_OWNER_START), variants[i]};
            rvq_verdict v = run(both, 2u);
            CHECK(v.holds && v.reason == RVQ_HOLDS && v.examined == 0u);
        }
        /* The reason names differ only by state under an unproven start. */
        kernel_thread_record other[] = {record(OWNER, RVQ_OWNER_START), host_timer};
        other[1].start_routine = proven[entry] + 1u;
        rvq_verdict v = run(other, 2u);
        CHECK(v.examined == 1u && !v.holds && v.reason == RVQ_REFUSE_HOST_TIMER && v.handle == A);
        other[1] = host_stopped;
        other[1].start_routine = proven[entry] + 1u;
        v = run(other, 2u);
        CHECK(v.examined == 1u && !v.holds && v.reason == RVQ_REFUSE_HOST_STOPPED);
    }
    /* An unproven sleeper is not parked by the host timer, and a reader beside the loader is
     * still examined and refused. */
    kernel_thread_record sleeper[] = {record(OWNER, RVQ_OWNER_START), record(A, 0x30160u),
                                      record(B, RVQ_WORKER_START)};
    sleeper[2].block_state = KERNEL_THREAD_BLOCK_HOST_TIMER;
    rvq_verdict v = run(sleeper, 3u);
    CHECK(v.examined == 1u && !v.holds && v.reason == RVQ_REFUSE_HOST_TIMER && v.handle == B);
}

static void test_deliverer(void)
{
    kernel_thread_record others[] = {terminated(A, RVQ_WORKER_START)};
    rvq_verdict v = run(others, 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_DELIVERER_UNKNOWN);
    kernel_thread_record gone[] = {terminated(OWNER, RVQ_OWNER_START), terminated(A, RVQ_WORKER_START)};
    v = run(gone, 2u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_DELIVERER_UNKNOWN);
    kernel_thread_record waiting[] = {record(OWNER, RVQ_OWNER_START)};
    waiting[0].block_state = KERNEL_THREAD_BLOCK_THREAD;
    v = run(waiting, 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_DELIVERER_UNKNOWN);
    v = recomp_vblank_quiescence_evaluate(NULL, 0u, OWNER);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_DELIVERER_UNKNOWN);
    kernel_thread_record unused = record(A, 1u);
    unused.in_use = false;
    kernel_thread_record hole[] = {record(OWNER, RVQ_OWNER_START), unused};
    v = run(hole, 2u);
    CHECK(v.holds && v.examined == 0u);
    CHECK(strlen(recomp_vblank_quiescence_reason_name(RVQ_REFUSE_RUNNABLE)) > 8u);
}

static void test_registered_readers(void)
{
    CHECK(recomp_vblank_quiescence_is_registered_reader(RVQ_OWNER_RETURN, RVQ_OWNER_START));
    CHECK(recomp_vblank_quiescence_is_registered_reader(RVQ_WORKER_RETURN, RVQ_WORKER_START));
    CHECK(!recomp_vblank_quiescence_is_registered_reader(RVQ_OWNER_RETURN, RVQ_WORKER_START));
    CHECK(!recomp_vblank_quiescence_is_registered_reader(RVQ_WORKER_RETURN, RVQ_OWNER_START));
    CHECK(!recomp_vblank_quiescence_is_registered_reader(RVQ_WORKER_RETURN, 0x30160u));
    CHECK(!recomp_vblank_quiescence_is_registered_reader(0x153976u, RVQ_OWNER_START));
    CHECK(!recomp_vblank_quiescence_is_registered_reader(0u, 0u));
}

/* ---------------- Part B: real guest threads ---------------- */

static uint32_t scratch;
static uint32_t counter_address;
static atomic_bool release_flag[8];
static atomic_uint spin_polls;
static atomic_uint owner_request;
static atomic_uint request_kind; /* 0 delivery check, 1 getter census */
static atomic_uint request_handle, request_return;
static atomic_uint owner_done;
static atomic_bool owner_returned;
static host_stop owner_stop;
static rvq_verdict owner_verdict;
static uint32_t owner_handle_global;
static _Thread_local unsigned thread_mode;

static bool has_code(uint32_t address)
{
    return address == 0x123456u || address == RVQ_OWNER_START || address == RVQ_WORKER_START ||
           address == 0x30160u;
}
static void terminate(uint32_t status)
{
    (void)status;
    host_run_stop(HOST_STOP_THREAD_EXITED, 0u, 0u, "quiescence test thread exit");
}
static bool confirm(const kernel_thread_launch *launch)
{
    (void)launch;
    return host_run_result()->reason == HOST_STOP_THREAD_EXITED;
}
static void refused(uint32_t handle, kernel_thread_wait_refusal reason)
{
    (void)handle; (void)reason;
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, 0x38004Cu, 234u, "unexpected wait refusal");
}
static void pause_ms(long milliseconds)
{
    const struct timespec tick = {0, milliseconds * 1000000L};
    (void)nanosleep(&tick, NULL);
}
static void call(unsigned ordinal, const kernel_thread_launch *launch, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0};
    (void)kernel_frame_build(&frame, launch->stack_low, 4u * (count + 1u), args, count);
    (void)kernel_hle_call(ordinal, &frame);
}
static void owner_loop(void)
{
    unsigned served = 0u;
    while (!atomic_load(&release_flag[1])) {
        if (atomic_load(&owner_request) == served) { pause_ms(1); continue; }
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            if (atomic_load(&request_kind) == 0u) {
                owner_verdict = recomp_vblank_quiescence_check(owner_handle_global, 0x1538C0u);
            } else {
                recomp_vblank_quiescence_check_getter(atomic_load(&request_handle),
                                                      atomic_load(&request_return));
            }
            atomic_store(&owner_returned, true);
        } else {
            owner_stop = *host_run_result();
            atomic_store(&owner_returned, false);
        }
        host_run_disarm();
        served++;
        atomic_store(&owner_done, served);
    }
}
static void run_mode(const kernel_thread_launch *launch)
{
    switch (thread_mode) {
    case 0u: /* spinning second reader: polls the counter word with no wait */
        while (!atomic_load(&release_flag[0])) {
            uint32_t value;
            (void)kernel_guest_read_u32(counter_address, &value);
            atomic_fetch_add(&spin_polls, 1u);
        }
        break;
    case 2u: { /* untimed thread wait on the owner */
        const uint32_t args[4] = {owner_handle_global, 1u, 0u, 0u};
        call(234u, launch, args, 4u);
        break;
    }
    case 3u: { /* host deadline sleep of 500 ms through KeDelayExecutionThread */
        const int64_t interval = -5000000;
        (void)kernel_guest_write_bytes(scratch + 0x200u, &interval, sizeof interval);
        const uint32_t args[3] = {1u, 0u, scratch + 0x200u};
        call(99u, launch, args, 3u);
        break;
    }
    case 4u: { /* the title worker's pause: pseudo handle, exactly -80000 units, repeated */
        const uint64_t interval = UINT64_C(0xFFFFFFFFFFFEC780);
        (void)kernel_guest_write_bytes(scratch + 0x240u, &interval, sizeof interval);
        while (!atomic_load(&release_flag[4])) {
            const uint32_t args[4] = {0xFFFFFFFEu, 1u, 0u, scratch + 0x240u};
            call(234u, launch, args, 4u);
        }
        break;
    }
    default:
        break;
    }
}
static void enter(const kernel_thread_launch *launch)
{
    thread_mode = launch->start_context;
    if (thread_mode == 1u) {
        owner_loop(); /* the delivering owner evaluates the checked predicate on request */
    } else if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        run_mode(launch);
    }
    host_run_disarm();
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        const uint32_t status = 0u;
        call(258u, launch, &status, 1u);
    }
    host_run_disarm();
}
static const kernel_thread_host_ops ops = {
    .has_code = has_code, .enter = enter, .terminate = terminate,
    .termination_confirmed = confirm, .wait_refused = refused,
};
static uint32_t create_thread_at(unsigned mode, uint32_t start)
{
    static unsigned serial;
    const uint32_t out = scratch + 0x800u + (serial++ % 32u) * 4u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, start, mode, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(out, &handle));
    return handle;
}
static uint32_t create_thread(unsigned mode) { return create_thread_at(mode, 0x123456u); }
static bool snapshot_of(uint32_t handle, kernel_thread_record *out)
{
    return kernel_thread_get(handle, out);
}
static bool wait_until_state(uint32_t handle, rvq_thread_state state)
{
    for (unsigned i = 0u; i < 5000u; i++) {
        kernel_thread_record r;
        if (snapshot_of(handle, &r) && recomp_vblank_quiescence_classify(&r) == state) { return true; }
        pause_ms(1);
    }
    return false;
}
static rvq_verdict live_verdict(uint32_t owner)
{
    kernel_thread_record records[KERNEL_THREAD_MAX];
    const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
    CHECK(count > 0u);
    return recomp_vblank_quiescence_evaluate(records, count, owner);
}
static void ask_owner(void)
{
    const unsigned want = atomic_load(&owner_done) + 1u;
    atomic_fetch_add(&owner_request, 1u);
    for (unsigned i = 0u; i < 5000u && atomic_load(&owner_done) < want; i++) { pause_ms(1); }
    CHECK(atomic_load(&owner_done) >= want);
}

static void test_real_threads(void)
{
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(&ops));
    for (unsigned i = 0u; i < 8u; i++) { atomic_store(&release_flag[i], false); }
    atomic_store(&owner_request, 0u);
    atomic_store(&owner_done, 0u);
    atomic_store(&request_kind, 0u);
    counter_address = scratch + 0x300u;
    CHECK(kernel_guest_write_u32(counter_address, 1u));

    const uint32_t owner = create_thread(1u);
    owner_handle_global = owner;
    CHECK(owner != 0u);
    CHECK(wait_until_state(owner, RVQ_RUNNABLE));

    /* Only the owner exists: vacuous, and the checked form returns while enabled. */
    rvq_verdict v = live_verdict(owner);
    CHECK(v.holds && v.examined == 0u);

    /* Negative: a second reader spinning at the getter, runnable, so delivery is refused. */
    const uint32_t spinner = create_thread(0u);
    CHECK(wait_until_state(spinner, RVQ_RUNNABLE));
    unsigned before = atomic_load(&spin_polls);
    for (unsigned i = 0u; i < 5000u && atomic_load(&spin_polls) < before + 1000u; i++) { pause_ms(1); }
    CHECK(atomic_load(&spin_polls) >= before + 1000u); /* it really is polling meanwhile */
    v = live_verdict(owner);
    CHECK(v.examined == 1u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_RUNNABLE && v.handle == spinner);
    recomp_vblank_quiescence_configure(true);
    ask_owner();
    CHECK(!atomic_load(&owner_returned));
    CHECK(owner_stop.reason == HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK(owner_stop.guest_address == 0x1538C0u);
    CHECK(owner_stop.detail && strstr(owner_stop.detail, "quiescence refused") != NULL);
    CHECK(owner_stop.detail && strstr(owner_stop.detail, "runnable") != NULL);
    /* The same state with the check disabled is observation free: the call returns. */
    recomp_vblank_quiescence_configure(false);
    ask_owner();
    CHECK(atomic_load(&owner_returned) && owner_verdict.holds);

    /* Once the spinner terminates the predicate holds, and the checked form returns. */
    atomic_store(&release_flag[0], true);
    CHECK(wait_until_state(spinner, RVQ_TERMINATED));
    kernel_thread_record done;
    CHECK(snapshot_of(spinner, &done) && done.block_state == KERNEL_THREAD_BLOCK_NONE);
    v = live_verdict(owner);
    CHECK(v.examined == 1u);
    CHECK(v.holds);
    recomp_vblank_quiescence_configure(true);
    ask_owner();
    CHECK(atomic_load(&owner_returned) && owner_verdict.holds && owner_verdict.examined == 1u);
    recomp_vblank_quiescence_configure(false);

    /* A host deadline sleep is recorded and refuses. */
    const uint32_t sleeper = create_thread(3u);
    CHECK(wait_until_state(sleeper, RVQ_HOST_TIMER));
    v = live_verdict(owner);
    CHECK(v.examined == 2u);
    CHECK(!v.holds && v.reason == RVQ_REFUSE_HOST_TIMER && v.handle == sleeper);
    CHECK(wait_until_state(sleeper, RVQ_TERMINATED));
    CHECK(snapshot_of(sleeper, &done) && done.block_state == KERNEL_THREAD_BLOCK_NONE);
    v = live_verdict(owner);
    CHECK(v.examined == 2u && v.holds);

    /* The title worker's 8 ms pseudo handle pause is a host timer too. */
    const uint32_t pauser = create_thread(4u);
    bool saw_timer = false;
    for (unsigned i = 0u; i < 5000u && !saw_timer; i++) {
        kernel_thread_record r;
        saw_timer = snapshot_of(pauser, &r) && r.block_state == KERNEL_THREAD_BLOCK_HOST_TIMER;
        if (!saw_timer) { pause_ms(1); }
    }
    CHECK(saw_timer);
    v = live_verdict(owner);
    CHECK(v.examined == 3u);
    CHECK(!v.holds && (v.reason == RVQ_REFUSE_HOST_TIMER || v.reason == RVQ_REFUSE_RUNNABLE) &&
          v.handle == pauser);
    atomic_store(&release_flag[4], true);
    CHECK(wait_until_state(pauser, RVQ_TERMINATED));

    /* T593: real threads started at the loader routine 0x30160, one sleeping to a host deadline and
     * one runnable and polling, do not refuse the delivery and are not examined, while the unproven
     * sleeper above did. The checked form returns too. */
    atomic_store(&release_flag[0], false);
    const uint32_t loader_sleeper = create_thread_at(3u, 0x30160u);
    CHECK(wait_until_state(loader_sleeper, RVQ_HOST_TIMER));
    const uint32_t loader_spinner = create_thread_at(0u, 0x30160u);
    CHECK(wait_until_state(loader_spinner, RVQ_RUNNABLE));
    kernel_thread_record loader_record;
    CHECK(snapshot_of(loader_sleeper, &loader_record) && loader_record.start_routine == 0x30160u);
    v = live_verdict(owner);
    CHECK(v.holds && v.reason == RVQ_HOLDS && v.examined == 3u);
    recomp_vblank_quiescence_configure(true);
    ask_owner();
    CHECK(atomic_load(&owner_returned) && owner_verdict.holds && owner_verdict.examined == 3u);
    recomp_vblank_quiescence_configure(false);
    atomic_store(&release_flag[0], true);
    CHECK(wait_until_state(loader_spinner, RVQ_TERMINATED));
    CHECK(wait_until_state(loader_sleeper, RVQ_TERMINATED));

    /* An untimed wait on the deliverer is parked until the deliverer ends. */
    const uint32_t waiter = create_thread(2u);
    CHECK(wait_until_state(waiter, RVQ_BLOCKED));
    kernel_thread_record blocked;
    CHECK(snapshot_of(waiter, &blocked));
    CHECK(blocked.block_state == KERNEL_THREAD_BLOCK_THREAD && blocked.block_target == owner);
    v = live_verdict(owner);
    CHECK(v.examined == 4u);
    CHECK(v.holds);
    recomp_vblank_quiescence_configure(true);
    ask_owner();
    CHECK(atomic_load(&owner_returned) && owner_verdict.holds && owner_verdict.examined == 4u);
    recomp_vblank_quiescence_configure(false);

    /* The checked census: registered readers pass, everything else stops at the getter. */
    const uint32_t reg_owner = create_thread_at(5u, RVQ_OWNER_START);
    const uint32_t reg_worker = create_thread_at(5u, RVQ_WORKER_START);
    CHECK(wait_until_state(reg_owner, RVQ_TERMINATED) && wait_until_state(reg_worker, RVQ_TERMINATED));
    struct { uint32_t handle, return_address; bool stops; } census[] = {
        {reg_owner, RVQ_OWNER_RETURN, false}, {reg_worker, RVQ_WORKER_RETURN, false},
        {reg_owner, RVQ_WORKER_RETURN, true}, {reg_worker, RVQ_OWNER_RETURN, true},
        {spinner, RVQ_OWNER_RETURN, true}, {spinner, RVQ_WORKER_RETURN, true},
        {0u, RVQ_OWNER_RETURN, true}, {0x777u, RVQ_WORKER_RETURN, true},
    };
    CHECK(sizeof census / sizeof census[0] == 8u);
    recomp_vblank_quiescence_configure(true);
    atomic_store(&request_kind, 1u);
    const unsigned checks_before = recomp_vblank_quiescence_getter_checks();
    for (unsigned i = 0u; i < 8u; i++) {
        atomic_store(&request_handle, census[i].handle);
        atomic_store(&request_return, census[i].return_address);
        ask_owner();
        CHECK(atomic_load(&owner_returned) == !census[i].stops);
        if (census[i].stops) {
            CHECK(owner_stop.reason == HOST_STOP_XDK_UNIMPLEMENTED && owner_stop.guest_address == RVQ_GETTER);
            CHECK(owner_stop.detail && strstr(owner_stop.detail, "unregistered reader") != NULL);
        }
    }
    CHECK(recomp_vblank_quiescence_getter_checks() == checks_before + 8u);
    recomp_vblank_quiescence_configure(false);
    atomic_store(&request_handle, spinner);
    atomic_store(&request_return, RVQ_OWNER_RETURN);
    ask_owner();
    CHECK(atomic_load(&owner_returned)); /* disabled: the same unregistered reader is only observed */
    CHECK(recomp_vblank_quiescence_getter_checks() == checks_before + 8u); /* and not counted */
    atomic_store(&request_kind, 0u);

    atomic_store(&release_flag[1], true);
    CHECK(wait_until_state(owner, RVQ_TERMINATED));
    CHECK(wait_until_state(waiter, RVQ_TERMINATED));
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(kernel_thread_active_wait_count() == 0u);
}

int main(void)
{
    test_classification();
    test_measured_shapes();
    test_refusals();
    test_parked_threads();
    test_non_reader_table();
    test_default_non_reader_table();
    test_parked_by_location();
    test_non_reader_any_state();
    test_deliverer();
    test_registered_readers();

    kernel_hle_init();
    CHECK(kernel_thread_register() == 10u);
    (void)kernel_sync_register();
    guest_region_request request = {0};
    request.bytes = 0x4000u; request.state = MEM_COMMIT; request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
    test_real_threads();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(guest_region_free(scratch));
    printf("vblank quiescence: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
