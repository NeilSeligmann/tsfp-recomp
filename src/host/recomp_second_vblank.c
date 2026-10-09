/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_second_vblank.h"
#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_vblank_effects.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_sync.h"
#include "recomp_vblank_quiescence.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define FRAME 0x1538C0u
#define DEVICE 0x3E3F60u
/* The title counter the frame wait's getter 0x22030 returns and the counter its last exit stored. */
#define COUNTER 0x563918u
#define FRAME_LAST_EXIT 0x7A58B0u
/* The title counter after the startup event and the credited second event. */
#define OWNER_WAIT_FIRST_COUNTER 2u
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static recomp_second_vblank_snapshot state;
static pthread_t owner, producer;
static d3d8_first_vblank_invoker invoke;
static recomp_second_vblank_producer_confirmed confirm;
static uint32_t stack_low, stack_high;
/* T460: configuration, not epoch state. 0 is off, cleared by every configure. */
static uint32_t owner_budget;
static bool interactive;
static bool (*shutdown_query)(void); /* configured before guest startup */
void recomp_second_vblank_set_shutdown_query(bool (*query)(void)) { shutdown_query = query; }
/* T696: the frame wait poll may deliver one blank when its exit test fails. Configuration, cleared by configure. */
static bool poll_blank_enabled;
/* T592: the loading bar worker's blanks, see docs/vblank-delivery.md "T592 results". The loading bar
 * gate is the title's `while (1.0f > [PROGRESS])` in sub_00156840, entered from the frame loop. */
#define GATE 0x156840u
#define GATE_FLAG 0x4E7A94u
#define GATE_RUNNING 0x74A9ACu
#define GATE_WAIT 0x3800BFu
#define PROGRESS 0x749848u
#define PROGRESS_ONE 0x475C78u
#define GETTER 0x22030u
#define WORKER_START 0x156CB0u
#define WORKER_SYSTEM 0x37FE1Du
#define WORKER_HOLD_MS_DEFAULT 600000u
/* T604: the entries that reach a store to the running flag [0x74A9AC]. The stop function 0x155550 (store at
 * 0x15556B) and the start 0x156D80 with its body 0x156D10 (store at 0x156D31, entered by 0x156D80's tail jump)
 * are the writers, the gate 0x156840 tail jumps into the stop function. A tail jump is a plain C call in the
 * lifted code with no safepoint, so only a CALL (direct or indirect) of these entries is seen. */
#define RUNNING_STOP 0x155550u
#define RUNNING_START 0x156D80u
#define RUNNING_START_BODY 0x156D10u
enum { OWNER_ELSEWHERE = 0, OWNER_GATE, OWNER_GATE_WAIT };
static atomic_uint worker_budget;       /* 0 is off, cleared by every configure */
static atomic_uint worker_hold_ms;      /* the hold limit, configuration like the budget */
static atomic_uint owner_id;            /* the bound owner's handle, for the lock free call hook */
static atomic_uint worker_id;           /* the worker that delivered, for the epoch test */
static atomic_uint held_id;             /* the worker held idle by this module, else 0 */
static atomic_uint owner_location;
static _Noreturn void refuse(const char *reason)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, FRAME, 0u, reason);
    abort();
}
static void finish(void)
{
    pthread_mutex_lock(&lock);
    state.inflight = false;
    state.refused = true;
    pthread_mutex_unlock(&lock);
}
bool recomp_second_vblank_configure(bool enabled, d3d8_first_vblank_invoker fn,
                                    uint32_t low, uint32_t high,
                                    recomp_second_vblank_producer_confirmed checked)
{
    pthread_mutex_lock(&lock);
    if (state.inflight || state.epoch == UINT64_MAX ||
        (enabled && (!fn || !checked || !low || high <= low || high-low < 128u || (high & 15u)))) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    uint64_t epoch = state.epoch + 1u;
    memset(&state, 0, sizeof(state));
    state.epoch = epoch;
    state.enabled = enabled;
    invoke = fn; confirm = checked; stack_low = low; stack_high = high; owner_budget = 0u; poll_blank_enabled = false; interactive = false; shutdown_query = NULL;
    atomic_store(&worker_budget, 0u); atomic_store(&worker_hold_ms, WORKER_HOLD_MS_DEFAULT);
    atomic_store(&owner_id, 0u); atomic_store(&worker_id, 0u); atomic_store(&held_id, 0u);
    atomic_store(&owner_location, OWNER_ELSEWHERE);
    pthread_mutex_unlock(&lock);
    d3d8_first_vblank_configure(enabled, fn, low, high);
    return true;
}
bool recomp_second_vblank_reset(void)
{
    pthread_mutex_lock(&lock);
    if (state.inflight || state.epoch == UINT64_MAX) {
        pthread_mutex_unlock(&lock); return false;
    }
    uint64_t epoch = state.epoch + 1u;
    bool enabled = state.enabled;
    memset(&state, 0, sizeof(state)); state.epoch = epoch; state.enabled = enabled;
    atomic_store(&worker_id, 0u); atomic_store(&held_id, 0u); atomic_store(&owner_location, OWNER_ELSEWHERE);
    pthread_mutex_unlock(&lock);
    d3d8_first_vblank_reset(); return true;
}
bool recomp_second_vblank_set_owner_waits(uint32_t budget)
{
    pthread_mutex_lock(&lock);
    const bool accepted = state.inflight == false && state.epoch != UINT64_MAX &&
                          budget <= RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX &&
                          (budget == 0u || state.enabled);
    if (accepted) owner_budget = budget;
    pthread_mutex_unlock(&lock);
    return accepted;
}
bool recomp_second_vblank_set_interactive(bool enabled)
{
    pthread_mutex_lock(&lock);
    const bool accepted = !state.inflight && (!enabled || (state.enabled && owner_budget != 0u));
    if (accepted) interactive = enabled;
    pthread_mutex_unlock(&lock);
    return accepted;
}
bool recomp_second_vblank_set_poll_blank(bool enabled)
{
    pthread_mutex_lock(&lock);
    const bool accepted = state.inflight == false && state.epoch != UINT64_MAX &&
                          (!enabled || owner_budget != 0u); /* a budget implies an enabled policy */
    if (accepted) poll_blank_enabled = enabled;
    pthread_mutex_unlock(&lock);
    return accepted;
}
bool recomp_second_vblank_set_worker_blanks(uint32_t budget)
{
    pthread_mutex_lock(&lock);
    const bool accepted = state.inflight == false && state.epoch != UINT64_MAX &&
                          budget <= RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX &&
                          (budget == 0u || (state.enabled && owner_budget != 0u));
    if (accepted) atomic_store(&worker_budget, budget);
    pthread_mutex_unlock(&lock);
    return accepted;
}
void recomp_second_vblank_set_worker_hold_ms(uint32_t milliseconds)
{
    atomic_store(&worker_hold_ms, milliseconds);
}
void recomp_second_vblank_get_snapshot(recomp_second_vblank_snapshot *out)
{
    if (!out) return;
    pthread_mutex_lock(&lock);
    *out = state; out->owner_budget = owner_budget; out->worker_budget = atomic_load(&worker_budget);
    pthread_mutex_unlock(&lock);
}
void recomp_second_vblank_bind_owner(uint32_t handle, uint32_t fs, uint32_t start, uint32_t system)
{
    const char *error = NULL;
    pthread_mutex_lock(&lock);
    bool enabled = state.enabled;
    if (enabled && start == 0x3801D9u && system == 0x37FE1Du) {
        if (state.bound || !handle || !fs || (fs & 4095u)) error = "invalid or duplicate two-event owner";
        else { state.bound = true; state.owner_handle = handle; state.owner_fs = fs; owner = pthread_self();
               atomic_store(&owner_id, handle); }
    }
    pthread_mutex_unlock(&lock);
    if (error) refuse(error);
    if (enabled) d3d8_first_vblank_bind_owner(handle, fs, start, system);
}
void recomp_second_vblank_note_registration(uint32_t callback, uint32_t handle, uint32_t fs)
{
    const char *error = NULL;
    pthread_mutex_lock(&lock);
    if (state.enabled) {
        if (state.inflight || state.registered || !state.bound || callback != 0x22020u ||
            handle != state.owner_handle || fs != state.owner_fs || !pthread_equal(owner,pthread_self()))
            error = "unsupported callback registration/rebind";
        else {
            uint32_t device, old_callback;
            if (!kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||
                !kernel_guest_read_u32(DEVICE+0x1DB8u,&old_callback) || old_callback != 0u ||
                !kernel_guest_write_u32(DEVICE+0x1DB8u,old_callback))
                error = "callback registration requires fixed device and writable NULL slot";
            else state.registered = true;
        }
    }
    if (error) state.refused = true;
    pthread_mutex_unlock(&lock);
    if (error) refuse(error);
}
static void deliver_blank(uint32_t handle, uint32_t fs, bool worker, bool startup);
static bool worker_identity(uint32_t handle, uint32_t fs);
/* T587: a completed wait by ANOTHER guest thread after the owner-wait model began. It stays the T183
 * refusal, with the facts that decide it: which thread, where it started, and what the quiescence
 * predicate says with that thread as the deliverer. The text keeps the T183 prefix. */
static const char *later_thread_refusal(uint32_t handle)
{
    /* host_stop keeps the pointer and stores 255 bytes, so the worst case below is 245. */
    static _Thread_local char text[320];
    kernel_thread_record records[KERNEL_THREAD_MAX], self;
    const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
    const uint32_t start = kernel_thread_get(handle, &self) ? self.start_routine : 0u;
    const rvq_verdict verdict = recomp_vblank_quiescence_evaluate(records, count, handle);
    const int used = snprintf(text, sizeof text, "unsupported second-event refill/producer/backlog: wait by thread 0x%x "
                              "(start 0x%x), no second-thread delivery (T587), quiescence ", (unsigned)handle, (unsigned)start);
    if (verdict.holds) snprintf(text + used, sizeof text - (size_t)used, "holds");
    else snprintf(text + used, sizeof text - (size_t)used, "refused: %s (thread 0x%x, start 0x%x)",
                  recomp_vblank_quiescence_reason_name(verdict.reason), (unsigned)verdict.handle,
                  (unsigned)verdict.start_routine);
    return text;
}
void recomp_second_vblank_note_wait_completed(bool success, uint32_t handle, uint32_t fs)
{
    const char *error = NULL;
    bool deliver = false, later_thread = false, worker_blank = false;
    /* T592: read outside the lock, the thread table has its own. */
    const bool worker_thread = atomic_load(&worker_budget) != 0u && worker_identity(handle, fs);
    pthread_mutex_lock(&lock);
    if (state.enabled && !success) error = "unsuccessful GPU wait cannot produce callback credit";
    else if (state.enabled && success && state.inflight) error = "wait completion during callback reservation";
    else if (state.enabled && success && state.first_delivered && owner_budget != 0u && state.second_delivered &&
             !state.refused && handle && fs && handle == state.owner_handle && fs == state.owner_fs &&
             pthread_equal(owner,pthread_self())) {
        /* T460: the owner waits for its own blank. One callback per completed wait, bounded. */
        if (!interactive && state.owner_delivered >= owner_budget) error = "owner-wait callback budget exhausted";
        else { state.inflight = true; deliver = true; }
    }
    else if (worker_thread && state.enabled && success && state.first_delivered && owner_budget != 0u &&
             state.second_delivered && !state.refused && !pthread_equal(owner,pthread_self()) &&
             handle != state.owner_handle && (handle != state.producer_handle || state.second_via_worker)) {
        /* T592: the loading bar worker's own blank. The inflight flag is taken after the hold. */
        worker_blank = true;
    }
    else if (state.enabled && success && state.first_delivered) {
        if (state.inflight || state.second_attempted || state.credits != 0u || !handle || !fs ||
            (fs & 4095u) || handle == state.owner_handle || fs == state.owner_fs || pthread_equal(owner,pthread_self())) {
            error = "unsupported second-event refill/producer/backlog";
            later_thread = state.second_delivered && owner_budget != 0u && !pthread_equal(owner,pthread_self());
        }
        else { state.credits = 1u; state.producer_handle = handle; state.producer_fs = fs; producer = pthread_self(); }
    }
    if (error) state.refused = true;
    pthread_mutex_unlock(&lock);
    if (error && later_thread) error = later_thread_refusal(handle);
    if (error) refuse(error);
    if (deliver) deliver_blank(handle, fs, false, false);
    else if (worker_blank) deliver_blank(handle, fs, true, false);
}
static bool overlap(uint32_t a, uint64_t n, uint32_t b, uint64_t m)
{ return (uint64_t)a < (uint64_t)b+m && (uint64_t)b < (uint64_t)a+n; }
/* T513: the flip words of the two preflights, as d3d8_first_vblank.c proves them (T407). The flip
 * model changes state the measured empty state pins to zero, so only with BOTH --model-flips and
 * --couple-vblank-effects the proof is the drained queue instead of constants. With either off the
 * zeros stay demanded, nothing here loosens. The queue is drained when the consumer index and the
 * producer index both equal the flips processed (every flip queued was processed) and both slots are
 * empty (the loop over the nine words). The threshold is whatever the queue left, and the helper run
 * of this blank (d3d8_vblank_effects_apply) already updated it when the count met it. */
static bool flip_modelled(void)
{ return d3d8_flip_enabled() && d3d8_vblank_effects_enabled(); }
/* The consumer index the nine words must hold: the flips processed under the model, else 0. */
static uint32_t flip_expected_consumer(bool modelled)
{ return modelled ? (uint32_t)d3d8_flip_hardware_get().flips : 0u; }
enum { FLIP_REASON_UNREADABLE, FLIP_REASON_NOT_DRAINED, FLIP_REASON_PHANTOM_THRESHOLD, FLIP_REASON_RECORD, FLIP_REASONS };
/* After the nine words matched (the threshold word is free under the model): why the flip state is
 * not the proven drained one (an index into the caller's reasons), or -1, and the record this blank's
 * helper run built, which is what the callback is given. Reads only. */
static int flip_queue_refusal(uint32_t count, d3d8_vblank_record *record)
{
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    uint32_t producer_index, threshold;
    if (!kernel_guest_read_u32(DEVICE+D3D8_FLIP_DEV_PRODUCER,&producer_index) ||
        !kernel_guest_read_u32(DEVICE+D3D8_FLIP_DEV_THRESHOLD,&threshold))
        return FLIP_REASON_UNREADABLE;
    if (producer_index != (uint32_t)hardware.flips || hardware.queued != hardware.flips) return FLIP_REASON_NOT_DRAINED;
    /* No flip was ever queued: nothing set the threshold, it is the measured zero. */
    if (hardware.queued == 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;
    *record = d3d8_vblank_effects_last_record();
    if (record->count != count || record->flip_index != producer_index ||
        (record->flags == 2u && threshold != count + 1u))
        return FLIP_REASON_RECORD;
    return -1;
}
static const char *const owner_flip_reasons[FLIP_REASONS] = {
    "owner-wait flip words unreadable", "owner-wait flip queue not drained",
    "owner-wait flip threshold set without a queued flip", "owner-wait helper record disagrees with the flip words"};
static const char *const second_flip_reasons[FLIP_REASONS] = {
    "second-event flip words unreadable", "second-event flip queue not drained",
    "second-event flip threshold set without a queued flip", "second-event helper record disagrees with the flip words"};
/* T592: the loading bar worker's identity, from the thread table: the start routine and system
 * routine of the credited producer's kind, this control block, alive, and not the producer itself. */
static bool worker_identity(uint32_t handle, uint32_t fs)
{
    kernel_thread_record record;
    if (!handle || !fs || !kernel_thread_get(handle,&record)) return false;
    return record.in_use && record.started && !record.finished && record.start_routine == WORKER_START &&
           record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;
}
static uint64_t now_milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000u + (uint64_t)now.tv_nsec/1000000u;
}
static void idle_briefly(void)
{
    const struct timespec pause = {0,200000L};
    nanosleep(&pause,NULL);
}
static bool worker_record_identity(uint32_t handle)
{
    kernel_thread_record record;
    return handle != 0u && kernel_thread_get(handle,&record) && record.in_use && record.started && !record.finished &&
           record.start_routine == WORKER_START && record.system_routine == WORKER_SYSTEM && record.start_context == 0u;
}
/* The owner called its one wait on this worker's termination and is blocked in it. */
static bool owner_blocked_on(uint32_t handle)
{
    kernel_thread_record record;
    return kernel_thread_get(atomic_load(&owner_id),&record) && record.block_state == KERNEL_THREAD_BLOCK_THREAD &&
           record.block_target == handle;
}
/* The gate's prologue stores are visible: stage 0xA at 0x4E7968 (0x156856) and the target [0x74A990] equal to
 * the title's 1.0f (0x156860). The worker's steps read both, so it is released only after them, which makes
 * the number of steps it takes to fill the bar independent of when the owner's stores land. If they were
 * already these values the store timing cannot matter either. */
static bool gate_prologue_visible(void)
{
    uint32_t stage, target, one;
    return kernel_guest_read_u32(0x4E7968u,&stage) && kernel_guest_read_u32(0x74A990u,&target) &&
           kernel_guest_read_u32(PROGRESS_ONE,&one) && stage == 0xAu && target == one;
}
/* The owner is parked by location: it entered the gate (or called its one wait) and the proof says no
 * path from there reads the counter before it blocks on the worker's termination. */
static bool owner_parked_by_location(void)
{
    return atomic_load(&owner_location) != OWNER_ELSEWHERE;
}
/* The title's own gate test at 0x15686C: the owner loops while 1.0f > progress (an ordered compare, a
 * NaN leaves the loop). True while the owner still waits for the worker's steps. */
static bool gate_closed(void)
{
    uint32_t progress_bits, one_bits;
    float progress, one;
    if (!kernel_guest_read_u32(PROGRESS,&progress_bits) || !kernel_guest_read_u32(PROGRESS_ONE,&one_bits))
        refuse("worker-blank gate progress unreadable");
    memcpy(&progress,&progress_bits,sizeof progress);
    memcpy(&one,&one_bits,sizeof one);
    return one > progress;
}
/* The worker idles, at its getter poll before each step and in its completed wait before each delivery,
 * until the quiescence predicate holds with the worker as deliverer and the owner counted parked by
 * location, and the owner's gate prologue stores are visible (or the owner already blocked on the
 * worker). The loop at the wait IS the mandatory quiescence check of the delivery: nothing is evaluated
 * again after it, and a verdict that never holds is a named stop. The poll is the worker's own
 * deterministic safepoint (it spins only in the getter loop 0x156CB1). */
static void worker_hold(uint32_t handle)
{
    const uint64_t limit = (uint64_t)atomic_load(&worker_hold_ms);
    const uint64_t began = now_milliseconds();
    bool held = false;
    for (;;) {
        if (shutdown_query != NULL && shutdown_query())
            host_run_stop(HOST_STOP_HOST_SHUTDOWN, FRAME, 0u, "interactive host shutdown while holding worker");
        kernel_thread_record records[KERNEL_THREAD_MAX];
        const unsigned count = kernel_thread_snapshot(records,KERNEL_THREAD_MAX);
        unsigned table_count = 0u, parked_count = 0u;
        const uint32_t *table = recomp_vblank_quiescence_default_non_readers(&table_count);
        uint32_t parked[1];
        if (owner_parked_by_location()) parked[parked_count++] = atomic_load(&owner_id);
        const rvq_verdict verdict = recomp_vblank_quiescence_evaluate_parked(records,count,handle,table,table_count,
                                                                             parked,parked_count);
        if (verdict.holds && (owner_blocked_on(handle) || gate_prologue_visible())) break;
        if (!held) {
            held = true;
            atomic_store(&held_id,handle);
            pthread_mutex_lock(&lock);
            state.worker_held++;
            pthread_mutex_unlock(&lock);
        }
        if (now_milliseconds() - began >= limit) {
            static _Thread_local char text[320];
            if (verdict.holds)
                snprintf(text,sizeof text,"worker blank held %u ms (T592): quiescence holds but the owner's gate prologue "
                         "stores are not visible",(unsigned)limit);
            else
                snprintf(text,sizeof text,"worker blank held %u ms without quiescence (T592): %s (thread 0x%x, start 0x%x)",
                         (unsigned)limit,recomp_vblank_quiescence_reason_name(verdict.reason),
                         (unsigned)verdict.handle,(unsigned)verdict.start_routine);
            refuse(text);
        }
        idle_briefly();
    }
    atomic_store(&held_id,0u);
}
/* After the blank that let the owner out of the gate the worker idles until the owner has blocked on the
 * worker's termination, which it does only after it stored the stop flag the worker tests next. Without
 * it the worker's next getter poll and the owner's exit race, and the number of blanks would depend on
 * the host's timing. */
static void worker_hold_final(uint32_t handle)
{
    const uint64_t limit = (uint64_t)atomic_load(&worker_hold_ms);
    const uint64_t began = now_milliseconds();
    bool counted = false;
    for (;;) {
        if (shutdown_query != NULL && shutdown_query())
            host_run_stop(HOST_STOP_HOST_SHUTDOWN, FRAME, 0u, "interactive host shutdown while holding worker");
        kernel_thread_record record;
        if (kernel_thread_get(atomic_load(&owner_id),&record) && record.block_state == KERNEL_THREAD_BLOCK_THREAD &&
            record.block_target == handle) break;
        if (!counted) {
            counted = true;
            atomic_store(&held_id,handle);
            pthread_mutex_lock(&lock);
            state.worker_final_held++;
            pthread_mutex_unlock(&lock);
        }
        if (now_milliseconds() - began >= limit) refuse("worker blank: the owner left the gate but never blocked on the worker (T592)");
        idle_briefly();
    }
    atomic_store(&held_id,0u);
}
/* The refusal texts that differ by deliverer. The owner's are the T460 ones byte for byte. */
typedef struct {
    const char *scope, *quiescence_off, *coupling_off, *caller, *bookkeeping, *probes, *invoke, *advance,
               *during, *stop_scope;
    const char *const *flips;
} blank_texts;
static const blank_texts owner_texts = {
    "owner-wait caller/PCR/device/counter scope refused", "owner-wait callback requires the quiescence check",
    "owner-wait callback requires the coupled vblank effects", NULL, "owner-wait nonempty or changed device bookkeeping",
    "owner-wait write probes refused", "owner-wait callback invocation failed",
    "owner-wait callback did not advance the counter by one", "owner-wait refill during callback refused",
    "owner-wait stop scope unavailable", owner_flip_reasons};
static const char *const worker_flip_reasons[FLIP_REASONS] = {
    "worker-wait flip words unreadable", "worker-wait flip queue not drained",
    "worker-wait flip threshold set without a queued flip", "worker-wait helper record disagrees with the flip words"};
static const blank_texts worker_texts = {
    "worker-wait caller/PCR/device/counter scope refused", "worker-wait callback requires the quiescence check",
    "worker-wait callback requires the coupled vblank effects", NULL, "worker-wait nonempty or changed device bookkeeping",
    "worker-wait write probes refused", "worker-wait callback invocation failed",
    "worker-wait callback did not advance the counter by one", "worker-wait refill during callback refused",
    "worker-wait stop scope unavailable", worker_flip_reasons};
/* T460: the owner's own blank. The original runs the helper from the DPC while the owner is blocked in
 * its wait, and a single processor resumes it only after the DPC (and so the callback) finished, so the
 * owner sees the counter advanced when its wait returns and nothing else samples it in between. The
 * delivery here is that, on the waiting thread inside the completed wait, after the coupled device
 * effects (the count is already published, this only reads it). Every precondition refuses before
 * any write, the quiescence check is mandatory, nothing is retried and no credit is invented.
 *
 * T592: the same delivery for the loading bar worker's own completed wait (worker == true). It differs
 * in who is parked: the owner is runnable, proven by location (owner_parked_by_location), so the worker
 * first idles until the predicate holds (worker_hold), and a blank that opens the owner's gate is
 * followed by the final hold. The counter the delivery expects counts both kinds of blank. */
static void deliver_blank(uint32_t handle, uint32_t fs, bool worker, bool startup)
{
    const blank_texts *text = worker ? &worker_texts : &owner_texts;
    if (!host_run_armed()) { finish(); abort(); }
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    if (!host_run_scope_init(&scope)) { finish(); abort(); }
    if (sigsetjmp(*host_run_scope_jmp(&scope),0) != 0) {
        const host_stop stopped = *host_run_result();
        finish(); if (!host_run_scope_pop(&scope)) abort(); host_run_rethrow(&stopped);
    }
    if (!host_run_scope_push(&scope)) { finish(); refuse(text->stop_scope); }
    if (!recomp_vblank_quiescence_enabled()) refuse(text->quiescence_off);
    if (!d3d8_vblank_effects_enabled()) refuse(text->coupling_off);
    if (worker) {
        pthread_mutex_lock(&lock);
        const bool exhausted = !interactive && state.worker_delivered >= atomic_load(&worker_budget);
        pthread_mutex_unlock(&lock);
        if (exhausted) refuse("worker-wait callback budget exhausted");
        worker_hold(handle);
        pthread_mutex_lock(&lock);
        const bool busy = state.refused || (startup ? !state.inflight : state.inflight);
        if (!busy) { state.inflight = true; atomic_store(&worker_id,handle); }
        pthread_mutex_unlock(&lock);
        if (busy) refuse("worker-wait delivery raced another callback or a refusal");
    }
    pthread_mutex_lock(&lock);
    const uint32_t done = state.owner_delivered + state.worker_delivered;
    pthread_mutex_unlock(&lock);
    uint32_t device, title_counter;
    uint8_t pcr[4096];
    if (kernel_sync_current_irql() != 0u || !kernel_guest_read_bytes(fs,pcr,sizeof(pcr)) || pcr[0x24u] != 0u ||
        !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||
        !kernel_guest_read_u32(0x563918u,&title_counter) || title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done))
        refuse(text->scope);
    const uint32_t offsets[] = {0x1DB8u,0x1DE8u,0x1DE4u,0x1DECu,0x1DDCu,0x1D9Cu,0x1DA8u,0x2448u,0x244Cu};
    /* The waits applied include this one, the unbacked startup event adds its one count. */
    const uint32_t count = (uint32_t)d3d8_vblank_effects_applied() + 1u;
    const bool modelled = flip_modelled();
    const uint32_t expected[] = {0x22020u,count,flip_expected_consumer(modelled),0u,0x02480104u,0u,0u,0u,0u};
    for (unsigned i=0u;i<9u;i++) {
        uint32_t value;
        if (modelled && i == 3u) continue; /* T513: the threshold is what the flip queue left */
        if (!kernel_guest_read_u32(DEVICE+offsets[i],&value) || value != expected[i])
            refuse(text->bookkeeping);
    }
    d3d8_vblank_record record = {count,0u,0u};
    if (modelled) {
        const int flip_error = flip_queue_refusal(count,&record);
        if (flip_error >= 0) refuse(text->flips[flip_error]);
    }
    if (!worker) {
        /* T592: a worker held idle by this module cannot sample the counter either. */
        const uint32_t held = atomic_load(&held_id);
        (void)recomp_vblank_quiescence_check_parked(handle,0x3D3550u,&held,held != 0u ? 1u : 0u);
    }
    uint8_t frame[20];
    uint32_t fs_head;
    if (!kernel_guest_read_u32(fs,&fs_head) || !kernel_guest_write_u32(fs,fs_head) ||
        !kernel_guest_read_bytes(stack_high-20u,frame,sizeof(frame)) ||
        !kernel_guest_write_bytes(stack_high-20u,frame,sizeof(frame)) ||
        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))
        refuse(text->probes);
    const uint32_t payload[3] = {record.count,record.flip_index,record.flags};
    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke);
    uint32_t after;
    if (!kernel_guest_read_u32(0x563918u,&after) || after != title_counter + 1u)
        refuse(text->advance);
    pthread_mutex_lock(&lock);
    const bool refused_during = state.refused;
    if (!refused_during) {
        if (startup) { state.second_delivered = true; state.second_via_worker = true; }
        else if (worker) state.worker_delivered++;
        else state.owner_delivered++;
        state.inflight = false;
    }
    pthread_mutex_unlock(&lock);
    if (refused_during) refuse(text->during);
    if (worker && !gate_closed()) worker_hold_final(handle);
    if (!host_run_scope_pop(&scope)) abort();
}
/* T592: the owner's position tracked at every call boundary, see the header. Runs on every thread,
 * touches only atomics for any thread but the owner. */
static bool worker_epoch_active(void)
{
    const uint32_t handle = atomic_load(&worker_id);
    kernel_thread_record record;
    return handle != 0u && kernel_thread_get(handle,&record) && !record.terminated;
}
static bool running_flag_entry(uint32_t callee)
{
    return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;
}
/* T604: the proof of the gate region leaves the running flag writers owner only as a NAMED ASSUMPTION (the
 * entry barrier is open, T599). The claim is relied on while the owner sits in the gate or the worker's blanks
 * run, so a call of a writer entry by another thread in either state is a named stop. Outside both nothing
 * is parked on the flag and the call changes no verdict. */
static void check_running_flag_caller(uint32_t callee, uint32_t handle)
{
    const bool in_gate = atomic_load(&owner_location) != OWNER_ELSEWHERE;
    if (!in_gate && !worker_epoch_active()) return;
    static _Thread_local char text[320];
    kernel_thread_record self;
    const uint32_t start = kernel_thread_get(handle,&self) ? self.start_routine : 0u;
    snprintf(text,sizeof text,"thread 0x%x (start 0x%x) called 0x%x, a running flag writer entry, while %s: "
             "the owner is not the only writer of [0x74A9AC] (T604)",(unsigned)handle,(unsigned)start,
             (unsigned)callee,in_gate ? "the owner is in the loading bar gate" : "the worker's blanks are delivered");
    refuse(text);
}
void recomp_second_vblank_note_call(uint32_t callee, uint32_t handle)
{
    if (atomic_load(&worker_budget) == 0u || handle == 0u) return;
    if (handle != atomic_load(&owner_id)) {
        if (running_flag_entry(callee)) check_running_flag_caller(callee,handle);
        /* The worker's own getter poll, after the credited second event and not for the producer. */
        if (callee != GETTER) return;
        pthread_mutex_lock(&lock);
        const bool armed = state.second_delivered && !state.refused && handle != state.producer_handle;
        pthread_mutex_unlock(&lock);
        if (armed && worker_record_identity(handle)) {
            worker_hold(handle);
            /* The worker tests the stop flag after a step, never in its getter loop, and the owner stores it
             * before it blocks on the worker. A poll with the owner already blocked is that test lost to a
             * frame the title skipped, and no blank would ever come: a named stop instead of a hang. */
            if (owner_blocked_on(handle))
                refuse("worker polled the vblank counter after the owner blocked on its termination, no blank is owed (T592)");
        }
        return;
    }
    if (callee == GATE) {
        uint32_t flag = 1u, running = 0u;
        /* The two entry conditions of the proof: the gate is not passed yet and the bar is running (the
         * stop function returns at once otherwise). Both have owner-only writers. */
        const bool entered = kernel_guest_read_u32(GATE_FLAG,&flag) && flag == 0u &&
                             kernel_guest_read_u32(GATE_RUNNING,&running) && running != 0u;
        if (entered) { pthread_mutex_lock(&lock); state.owner_gate_entries++; pthread_mutex_unlock(&lock); }
        atomic_store(&owner_location,entered ? OWNER_GATE : OWNER_ELSEWHERE);
        return;
    }
    const unsigned location = atomic_load(&owner_location);
    if (location == OWNER_GATE) {
        if (callee == GATE_WAIT) { atomic_store(&owner_location,OWNER_GATE_WAIT); return; }
        atomic_store(&owner_location,OWNER_ELSEWHERE);
        if (worker_epoch_active()) refuse("owner left the loading bar gate through a call outside the proof (T592)");
        return;
    }
    if (location == OWNER_GATE_WAIT) atomic_store(&owner_location,OWNER_ELSEWHERE);
    if (callee == GETTER && worker_epoch_active())
        refuse("owner sampled the vblank counter while the worker's blanks were delivered (T592)");
}
/* Interactive startup can reach the owner's terminal worker wait before its second FRAME.
 * The live producer then polls counter1 forever although its completed GPU wait earned a credit.
 * Deliver that credit on the exact worker getter boundary while the owner is genuinely blocked
 * on this worker. No extra blank/clock advance; only the registered callback advances the counter.
 * Default/evidence delivery still requires the producer's termination and owner FRAME3D66D. */
static bool startup_worker_poll(uint32_t handle, uint32_t fs, uint32_t esp, uint32_t irql)
{
    pthread_mutex_lock(&lock);
    const bool candidate = interactive && owner_budget != 0u && atomic_load(&worker_budget) != 0u &&
        state.enabled && state.bound && state.registered &&
        state.first_delivered && !state.second_attempted && !state.second_delivered &&
        !state.inflight && !state.refused && state.credits == 1u &&
        handle == state.producer_handle && fs == state.producer_fs &&
        handle != state.owner_handle && pthread_equal(producer,pthread_self());
    pthread_mutex_unlock(&lock);
    if (!candidate || !worker_identity(handle,fs) || !owner_blocked_on(handle)) return false;
    uint32_t caller, counter, previous, stop, running, gate;
    if (irql != 0u || !kernel_guest_read_u32(esp,&caller) || caller != 0x156CB6u ||
        !kernel_guest_read_u32(COUNTER,&counter) || counter != 1u ||
        !kernel_guest_read_u32(0x7497D8u,&previous) || previous != counter ||
        !kernel_guest_read_u32(0x74A9B0u,&stop) || stop != 1u ||
        !kernel_guest_read_u32(GATE_RUNNING,&running) || running != 0u ||
        !kernel_guest_read_u32(GATE_FLAG,&gate) || gate != 1u)
        refuse("interactive startup worker caller/counter/termination scope refused");
    const uint64_t bytes = (uint64_t)stack_high-stack_low;
    if (overlap(stack_low,bytes,DEVICE,0x2450u) || overlap(stack_low,bytes,fs,4096u) ||
        overlap(stack_low,bytes,esp,4u) || overlap(stack_low,bytes,COUNTER,4u) ||
        overlap(fs,4096u,DEVICE,0x2450u) || overlap(fs,4096u,COUNTER,4u) ||
        overlap(esp,4u,DEVICE,0x2450u) || overlap(esp,4u,fs,4096u))
        refuse("interactive startup worker aliased callback context");
    pthread_mutex_lock(&lock);
    const bool raced = !interactive || owner_budget == 0u || atomic_load(&worker_budget) == 0u ||
        !state.enabled || !state.bound || !state.registered ||
        !state.first_delivered || state.second_delivered || state.inflight || state.refused ||
        state.second_attempted || state.credits != 1u || handle != state.producer_handle ||
        fs != state.producer_fs || handle == state.owner_handle || !pthread_equal(producer,pthread_self());
    if (!raced) { state.inflight = true; state.second_attempted = true; state.credits = 0u; }
    pthread_mutex_unlock(&lock);
    if (raced) refuse("interactive startup worker credit reservation raced");
    fprintf(stderr,"interactive vblank: FABRICATED startup callback delivery on terminal loading-worker poll; consumes existing GPU wait credit, no clock advance\n");
    deliver_blank(handle,fs,true,true);
    return true;
}
/* T1351: after the first blank the producer thread registers its one credit at its own frame wait (the else-if branch of the
 * getter hook above). The owner can poll the frame wait before the producer thread ran that far: under heavy host load the
 * producer is merely late, and the old code refused with "second-event credit/attempt refused" at startup (1 in 15 runs at
 * 160 busy loops on 32 cores, census 883). The owner now waits, bounded in real time like worker_hold, for the credit. A
 * credit that never arrives still ends in the same named stop below. */
static void await_second_credit(void)
{
    const uint64_t began = now_milliseconds();
    for (;;) {
        pthread_mutex_lock(&lock);
        const bool late = state.enabled && state.bound && state.registered && state.first_delivered && !state.second_delivered &&
            !state.second_attempted && !state.refused && !state.inflight && state.credits == 0u && pthread_equal(owner,pthread_self());
        pthread_mutex_unlock(&lock);
        if (!late || now_milliseconds() - began >= 10000u) return;
        if (shutdown_query != NULL && shutdown_query())
            host_run_stop(HOST_STOP_HOST_SHUTDOWN, FRAME, 0u, "interactive host shutdown while waiting for the second-event credit");
        idle_briefly();
    }
}
void recomp_second_vblank_poll(uint32_t callee, uint32_t handle, uint32_t fs, uint32_t esp, uint32_t irql)
{
    if (callee == GETTER) { (void)startup_worker_poll(handle,fs,esp,irql); return; }
    const char *error = NULL;
    bool first = false, admitted = false, poll_blank = false;
    uint32_t ph = 0u, pf = 0u;
    if (callee != FRAME) return; /* T1289: answered without the lock, the locked test below returns for it as well */
    await_second_credit();
    pthread_mutex_lock(&lock);
    if (!state.enabled || callee != FRAME) { pthread_mutex_unlock(&lock); return; }
    if (state.refused || !state.bound || !state.registered || state.inflight || handle != state.owner_handle ||
        fs != state.owner_fs || !pthread_equal(owner,pthread_self())) error = "two-event owner/registration/reentry refused";
    else if (!state.first_delivered) first = true;
    else if (state.second_delivered && owner_budget != 0u) {
        /* T549: a later frame wait. Its own exit test (0x153985..0x15398F) is counter - [0x7A58B0] >= 1,
         * where [0x7A58B0] is the counter its previous exit stored. When that already holds the title
         * runs through without waiting, so nothing is delivered and nothing is invented. When it does
         * not, the title would spin on a blank this model never delivers: a named stop. */
        uint32_t counter = 0u, last = 0u;
        if (!kernel_guest_read_u32(COUNTER,&counter) || !kernel_guest_read_u32(FRAME_LAST_EXIT,&last))
            error = "frame-wait counter or last exit unreadable";
        else if (counter == last && poll_blank_enabled && (interactive || state.owner_delivered < owner_budget)) {
            /* T696: the exit test fails by exactly one blank. Deliver it here, from the poll, as the
             * owner-wait blank (same budget). FABRICATED timing: the title's own poll stands in for
             * the 60 Hz interrupt. The counter and [0x7A58B0] are never written by this module. */
            state.inflight = true; poll_blank = true;
        }
        else if ((int32_t)(counter - last) < 1) {
            /* T695: say which numbers made it spin (the stop's detail is all a later reader has). */
            static _Thread_local char text[160];
            snprintf(text,sizeof text,"frame wait would spin for a blank nobody delivers (counter %u, last exit %u, "
                     "owner blanks delivered %u of %u)",(unsigned)counter,(unsigned)last,(unsigned)state.owner_delivered,
                     (unsigned)owner_budget);
            error = text;
        }
        else { state.frames_admitted++; admitted = true; }
    }
    else if (state.second_attempted || state.credits != 1u || pthread_equal(owner,producer)) error = "second-event credit/attempt refused";
    if (!error && admitted) { pthread_mutex_unlock(&lock); return; }
    if (!error && poll_blank) {
        pthread_mutex_unlock(&lock);
        /* The refusals that must precede the blank's own device effects. */
        if (!recomp_vblank_quiescence_enabled()) { finish(); refuse(owner_texts.quiescence_off); }
        if (!d3d8_vblank_effects_enabled()) { finish(); refuse(owner_texts.coupling_off); }
        if (!d3d8_gpu_model_blank()) { finish(); refuse("poll blank: the clock or the helper effects refused the blank"); }
        deliver_blank(handle,fs,false,false);
        pthread_mutex_lock(&lock);
        state.poll_delivered++;
        pthread_mutex_unlock(&lock);
        return;
    }
    if (!error) { state.inflight = true; ph = state.producer_handle; pf = state.producer_fs;
        if (!first) { state.second_attempted = true; state.credits = 0u; } }
    pthread_mutex_unlock(&lock);
    if (error) refuse(error);
    if (!host_run_armed()) { finish(); abort(); }
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    if (!host_run_scope_init(&scope)) { finish(); abort(); }
    if (sigsetjmp(*host_run_scope_jmp(&scope),0) != 0) {
        const host_stop stopped = *host_run_result();
        finish(); if (!host_run_scope_pop(&scope)) abort(); host_run_rethrow(&stopped);
    }
    if (!host_run_scope_push(&scope)) { finish(); refuse("two-event stop scope unavailable"); }
    if (first) {
        d3d8_first_vblank_poll(callee, fs, esp, irql);
        d3d8_first_vblank_snapshot previous;
        d3d8_first_vblank_get_snapshot(&previous);
        if (!previous.delivered) refuse("first callback did not complete");
        pthread_mutex_lock(&lock);
        bool poisoned = state.refused;
        if (!poisoned) { state.first_delivered = true; state.credits = 0u; state.inflight = false; }
        pthread_mutex_unlock(&lock);
        if (poisoned) refuse("wait/refill during first callback refused");
    } else {
        if (!confirm(ph,pf)) refuse("second-event producer lacks confirmed termination");
        uint32_t caller, device, title_counter;
        uint8_t pcr[4096];
        if (!kernel_guest_read_u32(esp,&caller) || caller != 0x3D66Du || irql != 0u ||
            kernel_sync_current_irql() != irql || !kernel_guest_read_bytes(fs,pcr,sizeof(pcr)) || pcr[0x24u] != 0u ||
            !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||
            !kernel_guest_read_u32(0x563918u,&title_counter) || title_counter != 1u)
            refuse("second-event caller/PCR/device scope refused");
        const uint32_t offsets[] = {0x1DB8u,0x1DE8u,0x1DE4u,0x1DECu,0x1DDCu,0x1D9Cu,0x1DA8u,0x2448u,0x244Cu};
        /* T372 coupling: the waits applied (the credit's own backing wait included) plus the
         * unbacked startup event's one count. Off, the measured 2. */
        const bool coupled = d3d8_vblank_effects_enabled();
        const uint32_t second_count = coupled ? (uint32_t)d3d8_vblank_effects_applied() + 1u : 2u;
        const bool modelled = flip_modelled();
        const uint32_t expected[] = {0x22020u,second_count,flip_expected_consumer(modelled),0u,0x02480104u,0u,0u,0u,0u};
        for (unsigned i=0u;i<9u;i++) {
            uint32_t value;
            if (modelled && i == 3u) continue; /* T513: the threshold is what the flip queue left */
            if (!kernel_guest_read_u32(DEVICE+offsets[i],&value) || value != expected[i])
                refuse("second-event nonempty or changed device bookkeeping");
        }
        d3d8_vblank_record record = {second_count,0u,0u};
        if (modelled) {
            const int flip_error = flip_queue_refusal(second_count,&record);
            if (flip_error >= 0) refuse(second_flip_reasons[flip_error]);
        }
        uint64_t bytes = (uint64_t)stack_high-stack_low;
        if (overlap(stack_low,bytes,DEVICE,0x2450u) || overlap(stack_low,bytes,0x3E3F58u,4u) ||
            overlap(stack_low,bytes,fs,4096u) || overlap(stack_low,bytes,esp,4u) ||
            overlap(fs,4096u,DEVICE,0x2450u) || overlap(esp,4u,DEVICE,0x2450u) || overlap(esp,4u,fs,4096u) ||
            overlap(stack_low,bytes,0x563918u,4u) || overlap(fs,4096u,0x563918u,4u) ||
            overlap(esp,4u,0x563918u,4u))
            refuse("second-event aliased callback context");
        uint8_t frame[20];
        uint32_t fs_head;
        if (!kernel_guest_read_u32(fs,&fs_head) || !kernel_guest_write_u32(fs,fs_head) ||
            !kernel_guest_read_bytes(stack_high-20u,frame,sizeof(frame)) ||
            !kernel_guest_write_bytes(stack_high-20u,frame,sizeof(frame)) ||
            !kernel_guest_write_u32(DEVICE+0x1DE8u,second_count) ||
            !kernel_guest_write_u32(0x563918u,title_counter)) refuse("second-event write probes refused");
        pthread_mutex_lock(&lock);
        bool poisoned = state.refused;
        /* Coupled, the backing wait already published the count: this event only reads it. */
        const uint32_t published_count = coupled ? second_count : 3u;
        bool published = !poisoned && kernel_guest_write_u32(DEVICE+0x1DE8u,published_count);
        pthread_mutex_unlock(&lock);
        if (poisoned) refuse("second-event concurrent refill refused");
        if (!published) refuse("second-event count publication refused");
        const uint32_t payload[3] = {published_count,record.flip_index,record.flags};
        if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse("second callback invocation failed after publication");
        pthread_mutex_lock(&lock);
        poisoned = state.refused;
        if (!poisoned) { state.second_delivered = true; state.inflight = false; }
        pthread_mutex_unlock(&lock);
        if (poisoned) refuse("second-event refill during callback refused");
    }
    if (!host_run_scope_pop(&scope)) abort();
}
