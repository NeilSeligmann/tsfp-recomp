/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T460: the owner thread waits for its own vertical blank. After the second event, with the budget
 * set (`recomp_second_vblank_set_owner_waits`), the coupled device effects and the quiescence check
 * on, each completed owner wait delivers one callback from inside the wait. Everything else stays
 * the T183 refusal. The owner is a REAL guest thread of the thread model (so the quiescence check
 * sees a live table), a second real thread spins to be the runnable reader, and every refusal is
 * checked to precede every write.
 *
 * T592 adds the loading bar worker: a real guest thread with the worker's start routine delivers a
 * blank in its own completed wait (--vblank-worker-blanks), with the owner proven parked by location
 * in the loading bar gate (recomp_second_vblank_note_call), or the worker idles in the hold.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_vblank_effects.h"
#include "host_runtime.h"
#include "kernel_sync.h"
#include "kernel_clock.h"
#include "kernel_thread.h"
#include "recomp_second_vblank.h"
#include "recomp_vblank_quiescence.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

#define DEV 0x3E3F60u
#define COUNTER 0x563918u
#define FRAME_LAST_EXIT 0x7A58B0u /* T549: the counter the frame wait's previous exit stored */
static const uint32_t fs = 0x7C0000u, low = 0x7D0000u, high = 0x7D1000u;
static uint32_t esp, scratch, owner_handle;
static unsigned calls;
static uint32_t expected_record;
static uint32_t expected_index, expected_flags; /* T513: the helper record's flip index and flags */
static int leaf_mode;
static bool probe_budget_in_callback, budget_accepted_in_callback, worker_budget_accepted_in_callback;
static atomic_bool release_spinner, impostor_returned;
/* T587: a second REAL guest thread (start context 2) that completes one wait on command. */
static atomic_bool second_go, second_done, second_returned;
static host_stop second_stop;
static char second_detail[512], producer_detail[512];

static void host_fatal(uint32_t address, const char *text)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, text);
}
static bool confirmed(uint32_t handle, uint32_t base)
{
    CHECK_EQ_U32(handle, 2u);
    CHECK_EQ_U32(base, 0x7E0000u);
    return true;
}
static void pause_ms(long milliseconds)
{
    const struct timespec tick = {0, milliseconds * 1000000L};
    (void)nanosleep(&tick, NULL);
}

/* ---- the leaf stand-in -------------------------------------------------------------------- */
enum { LEAF_OK = 0, LEAF_FALSE, LEAF_STOP, LEAF_NO_COUNTER, LEAF_NESTED_WAIT, LEAF_OTHER_THREAD_WAIT };
static void producer_wait(void);
static bool callback(uint32_t address, uint32_t lo, uint32_t hi, const uint32_t payload[3])
{
    CHECK_EQ_U32(address, 0x22020u);
    CHECK_EQ_U32(lo, low);
    CHECK_EQ_U32(hi, high);
    CHECK_EQ_U32(payload[1], expected_index);
    CHECK_EQ_U32(payload[2], expected_flags);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), payload[0]);
    if (expected_record != 0u) CHECK_EQ_U32(payload[0], expected_record);
    calls++;
    if (probe_budget_in_callback) {
        budget_accepted_in_callback = recomp_second_vblank_set_owner_waits(9u);
        worker_budget_accepted_in_callback = recomp_second_vblank_set_worker_blanks(9u);
    }
    if (leaf_mode == LEAF_FALSE) return false;
    if (leaf_mode == LEAF_STOP) host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, 0x22020u, 77u, "leaf stop");
    if (leaf_mode == LEAF_NESTED_WAIT) recomp_second_vblank_note_wait_completed(true, owner_handle, fs);
    if (leaf_mode == LEAF_OTHER_THREAD_WAIT) producer_wait(); /* refused there, poisons the policy */
    if (leaf_mode != LEAF_NO_COUNTER) store(COUNTER, load(COUNTER) + 1u);
    return true;
}

/* ---- the real guest threads --------------------------------------------------------------- */
/* One thread that serves one job at a time inside an armed stop scope: the owner, and (T592) the worker. */
typedef struct {
    atomic_uint request, served;
    void (*volatile job)(void);
    atomic_bool returned;
    host_stop stop;
    char detail[256];
    atomic_bool release;
} server;
static server owner_server, worker_server;
static uint32_t worker_handle, worker_fs;

static bool has_code(uint32_t address) { return address == 0x123456u || address == 0x123457u || address == 0x37FE1Du || address == 0x156CB0u; }
static void terminate(uint32_t status)
{
    (void)status;
    host_run_stop(HOST_STOP_THREAD_EXITED, 0u, 0u, "owner-wait test thread exit");
}
static bool confirm_end(const kernel_thread_launch *launch)
{
    (void)launch;
    return host_run_result()->reason == HOST_STOP_THREAD_EXITED;
}
static void refused_wait(uint32_t handle, kernel_thread_wait_refusal reason)
{
    (void)handle;
    (void)reason;
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, 0x38004Cu, 234u, "unexpected wait refusal");
}
static void serve(server *who)
{
    unsigned done = 0u;
    while (!atomic_load(&who->release)) {
        if (atomic_load(&who->request) == done) {
            pause_ms(1);
            continue;
        }
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            who->job();
            atomic_store(&who->returned, true);
        } else {
            who->stop = *host_run_result();
            snprintf(who->detail, sizeof who->detail, "%s", who->stop.detail ? who->stop.detail : "");
            atomic_store(&who->returned, false);
        }
        host_run_disarm();
        done++;
        atomic_store(&who->served, done);
    }
}
static void enter(const kernel_thread_launch *launch)
{
    if (launch->start_routine == 0x156CB0u || launch->start_routine == 0x123457u) {
        serve(&worker_server); /* T592: the loading bar worker, or an impostor with its own control block */
    } else if (launch->start_context == 1u) {
        serve(&owner_server); /* the owner */
    } else if (launch->start_context == 0u) {
        while (!atomic_load(&release_spinner)) { /* a runnable reader: polls with no wait */
            (void)load(COUNTER);
        }
    } else if (launch->start_context == 5u) {
        /* T592: a thread that waits for the worker's termination, so it is parked by the worker's own rule. */
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            const uint32_t wait_args[4] = {worker_handle, 1u, 0u, 0u};
            kernel_call_frame wait_frame = {0};
            (void)kernel_frame_build(&wait_frame, launch->stack_low, 20u, wait_args, 4u);
            (void)kernel_hle_call(234u, &wait_frame);
        }
        host_run_disarm();
    } else if (launch->start_context == 2u) {
        while (!atomic_load(&second_go)) pause_ms(1);
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            recomp_second_vblank_note_wait_completed(true, launch->handle, fs + 0x1000u);
            atomic_store(&second_returned, true);
        } else {
            second_stop = *host_run_result();
            snprintf(second_detail, sizeof second_detail, "%s", second_stop.detail ? second_stop.detail : "");
        }
        host_run_disarm();
        atomic_store(&second_done, true);
    }
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        kernel_call_frame frame = {0};
        const uint32_t status = 0u;
        (void)kernel_frame_build(&frame, launch->stack_low, 8u, &status, 1u);
        (void)kernel_hle_call(258u, &frame);
    }
    host_run_disarm();
}
static const kernel_thread_host_ops ops = {
    .has_code = has_code, .enter = enter, .terminate = terminate,
    .termination_confirmed = confirm_end, .wait_refused = refused_wait,
};
static uint32_t create_thread(unsigned mode)
{
    static unsigned serial;
    const uint32_t out = scratch + 0x800u + (serial++ % 32u) * 4u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, 0x123456u, mode, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(out, &handle));
    return handle;
}
static bool wait_for_state(uint32_t handle, bool want_finished)
{
    for (unsigned i = 0u; i < 5000u; i++) {
        kernel_thread_record record;
        if (kernel_thread_get(handle, &record) && record.started &&
            (record.finished == want_finished) && (want_finished ? record.terminated : true))
            return true;
        pause_ms(1);
    }
    return false;
}
/* Start `fn` on a served thread, then wait for it: true when it returned (false when it stopped the host). */
static void start_on(server *who, void (*fn)(void))
{
    who->job = fn;
    atomic_store(&who->returned, false);
    atomic_fetch_add(&who->request, 1u);
}
static bool finish_on(server *who, unsigned want)
{
    for (unsigned i = 0u; i < 10000u && atomic_load(&who->served) < want; i++) pause_ms(1);
    CHECK(atomic_load(&who->served) >= want);
    return atomic_load(&who->returned);
}
static bool on_server(server *who, void (*fn)(void))
{
    const unsigned want = atomic_load(&who->served) + 1u;
    start_on(who, fn);
    return finish_on(who, want);
}
/* Run `fn` on the owner thread, true when it returned (false when it stopped the host). */
static bool on_owner(void (*fn)(void)) { return on_server(&owner_server, fn); }
static bool server_stopped_with(const server *who, const char *text)
{
    return !atomic_load(&who->returned) && who->stop.reason == HOST_STOP_XDK_UNIMPLEMENTED &&
           strstr(who->detail, text) != NULL;
}
static bool stopped_with(const char *text) { return server_stopped_with(&owner_server, text); }

/* ---- jobs the owner runs ------------------------------------------------------------------ */
static void job_bind(void)
{
    recomp_second_vblank_bind_owner(owner_handle, fs, 0x3801D9u, 0x37FE1Du);
    recomp_second_vblank_note_registration(0x22020u, owner_handle, fs);
}
static void job_poll(void) { recomp_second_vblank_poll(0x1538C0u, owner_handle, fs, esp, 0u); }
static void job_wait(void) { recomp_second_vblank_note_wait_completed(true, owner_handle, fs); }
static void job_wait_failed(void) { recomp_second_vblank_note_wait_completed(false, owner_handle, fs); }
static uint32_t previous_level;
static void job_raise(void) { previous_level = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH); }
static void job_lower(void) { kernel_sync_restore_irql(previous_level); }
static void job_wait_wrong_fs(void) { recomp_second_vblank_note_wait_completed(true, owner_handle, fs + 0x1000u); }
static void job_wait_wrong_handle(void) { recomp_second_vblank_note_wait_completed(true, owner_handle + 4u, fs); }

static void *producer(void *unused)
{
    (void)unused;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_second_vblank_note_wait_completed(true, 2u, 0x7E0000u);
    } else {
        snprintf(producer_detail, sizeof producer_detail, "%s", host_run_result()->detail ? host_run_result()->detail : "");
    }
    host_run_disarm();
    return NULL;
}
static void *impostor(void *unused)
{
    (void)unused;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_second_vblank_note_wait_completed(true, owner_handle, fs);
        atomic_store(&impostor_returned, true);
    }
    host_run_disarm();
    return NULL;
}
static void producer_wait(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}

/* ---- scenarios ---------------------------------------------------------------------------- */
/* T513: how the device was brought to the second event. FLIPS_OFF is the measured empty state (the
 * flip model off). FLIPS_NONE has the flip model on and no flip ever queued, FLIPS_QUEUED has every
 * Swap queue its flip and the next blank complete it, as the boot does with --model-flips. */
enum { FLIPS_OFF = 0, FLIPS_NONE, FLIPS_QUEUED };
static int flip_mode;
static void flip_queue(uint32_t present_interval)
{
    const uint32_t data = d3d8_flip_swap_method_data(0x1234000u, present_interval);
    d3d8_flip_lock();
    CHECK(d3d8_flip_queue_refusal_locked(data) == NULL);
    (void)d3d8_flip_queue_locked(data);
    d3d8_flip_unlock();
}
/* One blank. With queued flips, a Swap's flip comes first and this blank completes it. */
static void blank(uint32_t timestamp)
{
    if (flip_mode == FLIPS_QUEUED) flip_queue(0u);
    (void)d3d8_vblank_effects_apply(timestamp);
}
static void begin(uint32_t stack_low, uint32_t stack_high)
{
    d3d8_vblank_effects_configure(true);
    recomp_vblank_quiescence_configure(true);
    leaf_mode = LEAF_OK;
    expected_record = 0u;
    expected_index = expected_flags = 0u;
    d3d8_flip_configure(flip_mode != FLIPS_OFF);
    d3d8_flip_reset();
    store(DEV + 0x1DE8u, 0u);
    store(DEV + 0x1DDCu, 0x02480104u);
    const uint32_t flip_words[] = {0x1DE4u, 0x1DECu, 0x1DF0u, 0x1DF4u, 0x1D9Cu, 0x1DA0u, 0x1DA4u, 0x1DA8u, 0x1DACu, 0x1DB0u};
    for (unsigned index = 0u; index < sizeof flip_words / sizeof flip_words[0]; index++) store(DEV + flip_words[index], 0u);
    for (unsigned wait = 0u; wait < 3u; wait++) blank(0x1000u * (wait + 1u));
    store(0x3E3F58u, DEV);
    store(COUNTER, 0u);
    store(esp, 0x3D455u);
    const uint32_t offsets[] = {0x1DB8u, 0x2448u, 0x244Cu};
    for (unsigned index = 0u; index < sizeof offsets / sizeof offsets[0]; index++) store(DEV + offsets[index], 0u);
    CHECK(kernel_guest_write_u8(fs + 0x24u, 0u));
    CHECK(recomp_second_vblank_configure(true, callback, stack_low, stack_high, confirmed));
    CHECK(on_owner(job_bind));
    store(DEV + 0x1DB8u, 0x22020u);
}
/* The measured boot up to the credit: startup event, then the producer's wait and its credit. */
static void reach_credit(void)
{
    begin(low, high);
    if (flip_mode == FLIPS_QUEUED) {
        expected_index = 3u; /* three flips completed by the setup waits, the callback gets the consumer index */
        expected_flags = 2u; /* the new count meets the threshold the queue left, which the helper bumps */
    }
    CHECK(on_owner(job_poll)); /* the startup event: count 3 + 1 */
    store(esp, 0x3D66Du);
    blank(0x9000u); /* the producer's Swap wait: count 5 */
    producer_wait();
}
/* The credit, then the credited second event, delivered never recounted. */
static void reach_second_event(uint32_t budget)
{
    reach_credit();
    if (flip_mode == FLIPS_QUEUED) {
        expected_record = 5u;
        expected_index = 4u; /* the producer's Swap flip */
        expected_flags = 1u; /* a processed flip */
    }
    CHECK(on_owner(job_poll));
    CHECK_EQ_U32(load(COUNTER), 2u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 5u);
    if (budget != 0u) CHECK(recomp_second_vblank_set_owner_waits(budget));
}
static void owner_blank(void)
{
    blank(0xA000u + 0x1000u * (uint32_t)(calls));
}
static void expect_untouched(unsigned calls_before, uint32_t counter, uint32_t count)
{
    CHECK_EQ_U32(calls, calls_before);
    CHECK_EQ_U32(load(COUNTER), counter);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK(!snapshot.inflight);
}
/* A wait, after the blank was applied, that must refuse with `text` and write nothing. */
static void refused_after_blank(const char *text)
{
    const unsigned before = calls;
    const uint32_t counter = load(COUNTER);
    const uint32_t count = load(DEV + 0x1DE8u);
    CHECK(!on_owner(job_wait));
    CHECK(stopped_with(text));
    if (!stopped_with(text)) printf("    got: %s\n", owner_server.detail);
    expect_untouched(before, counter, count);
}
static void refused_owner_wait(const char *text)
{
    owner_blank();
    refused_after_blank(text);
}

static void test_deliveries(void)
{
    reach_second_event(3u);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, 3u);
    CHECK_EQ_U32(snapshot.owner_delivered, 0u);
    const unsigned before = calls;
    probe_budget_in_callback = true;
    for (uint32_t index = 1u; index <= 3u; index++) {
        owner_blank();
        expected_record = 5u + index; /* count: 5 after the producer, one per owner wait */
        CHECK(on_owner(job_wait));
        CHECK_EQ_U32(calls, before + index);
        CHECK_EQ_U32(load(COUNTER), 2u + index); /* the counter delta equals the waits */
        CHECK_EQ_U32(load(DEV + 0x1DE8u), 5u + index);
        recomp_second_vblank_get_snapshot(&snapshot);
        CHECK_EQ_U32(snapshot.owner_delivered, index);
        CHECK(!snapshot.inflight && !snapshot.refused);
        CHECK(!budget_accepted_in_callback); /* the budget cannot change while a callback runs */
        CHECK_EQ_U32(snapshot.owner_budget, 3u);
    }
    probe_budget_in_callback = false;
    /* The budget is spent: a named stop, no callback, no counter movement, never coalesced. */
    refused_owner_wait("budget exhausted");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, 0u); /* configure clears the budget */
    CHECK_EQ_U32(snapshot.owner_delivered, 0u);
}

/* ---- T587: a completed wait by another guest thread after the owner-wait model began ---------- */
static const char *const PLAIN_REFUSAL = "unsupported second-event refill/producer/backlog";
static void second_thread_wait(bool others_parked)
{
    atomic_store(&second_go, false);
    atomic_store(&second_done, false);
    atomic_store(&second_returned, false);
    second_detail[0] = '\0';
    const uint32_t second = create_thread(2u);
    CHECK(second != 0u);
    CHECK(wait_for_state(second, false));
    if (others_parked) {
        atomic_store(&owner_server.release, true); /* the measured worker is the only thread left */
        CHECK(wait_for_state(owner_handle, true));
    }
    atomic_store(&second_go, true);
    for (unsigned i = 0u; i < 5000u && !atomic_load(&second_done); i++) pause_ms(1);
    CHECK(atomic_load(&second_done));
    CHECK(wait_for_state(second, true));
}
static void test_later_thread_wait(void)
{
    /* Budget on, the owner runs: the wait of a second real thread refuses by name, the text says
     * which thread, where it started and why the quiescence predicate cannot hold. Nothing written. */
    reach_second_event(2u);
    owner_blank();
    const unsigned before = calls;
    const uint32_t counter = load(COUNTER), count = load(DEV + 0x1DE8u);
    second_thread_wait(false);
    CHECK(!atomic_load(&second_returned));
    CHECK(second_stop.reason == HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK(strncmp(second_detail, PLAIN_REFUSAL, strlen(PLAIN_REFUSAL)) == 0);
    CHECK(strstr(second_detail, "(T587)") != NULL);
    CHECK(strstr(second_detail, "(start 0x123456), no second-thread delivery (T587)") != NULL);
    CHECK(strstr(second_detail, "quiescence refused: another guest thread is runnable") != NULL);
    /* The whole text fits: it ends with the refusing thread and where it started (the owner). */
    CHECK(strstr(second_detail, "(thread 0x") != NULL && strstr(second_detail, "start 0x123456)") != NULL);
    CHECK(strlen(second_detail) < 255u); /* host_stop keeps 255 bytes, a longer text would be cut */
    CHECK(strstr(second_detail, "quiescence holds") == NULL);
    if (strstr(second_detail, "(T587)") == NULL) printf("    got: %s\n", second_detail);
    expect_untouched(before, counter, count);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK(snapshot.refused && snapshot.owner_delivered == 0u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* A thread the model never saw start has no live record: the same stop says so. */
    reach_second_event(2u);
    producer_detail[0] = '\0';
    producer_wait();
    CHECK(strstr(producer_detail, "(T587)") != NULL);
    CHECK(strstr(producer_detail, "(start 0x0), no second-thread delivery (T587)") != NULL);
    CHECK(strstr(producer_detail, "quiescence refused: delivering thread has no live started record") != NULL);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* Before the second event (the credit is outstanding, a second producer waits): the T183 text. */
    begin(low, high);
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(on_owner(job_poll));
    (void)d3d8_vblank_effects_apply(0x9000u);
    producer_wait();
    producer_detail[0] = '\0';
    producer_wait();
    CHECK(strcmp(producer_detail, PLAIN_REFUSAL) == 0);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* Budget off: the text is exactly the T183 one, byte for byte, with the second event delivered. */
    reach_second_event(0u);
    producer_detail[0] = '\0';
    producer_wait();
    CHECK(strcmp(producer_detail, PLAIN_REFUSAL) == 0);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* The owner's own wrong identity is still the plain refusal, with the budget on. */
    reach_second_event(2u);
    CHECK(!on_owner(job_wait_wrong_fs));
    CHECK(stopped_with(PLAIN_REFUSAL));
    CHECK(!stopped_with("(T587)"));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}
/* Last, it ends the owner: with every other thread terminated the predicate holds and the text says
 * so, the stop is still taken because no delivery model exists for a second thread. */
static void test_later_thread_wait_quiescent(void)
{
    reach_second_event(2u);
    owner_blank();
    const unsigned before = calls;
    const uint32_t counter = load(COUNTER), count = load(DEV + 0x1DE8u);
    second_thread_wait(true);
    CHECK(!atomic_load(&second_returned));
    CHECK(strstr(second_detail, "(T587)") != NULL);
    CHECK(strstr(second_detail, "quiescence holds") != NULL);
    CHECK(strstr(second_detail, "quiescence refused") == NULL);
    if (strstr(second_detail, "quiescence holds") == NULL) printf("    got: %s\n", second_detail);
    expect_untouched(before, counter, count);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}

static void test_default_off_and_ordering(void)
{
    /* Budget 0: the owner's wait after the second event is the T183 refusal. */
    reach_second_event(0u);
    refused_owner_wait("unsupported second-event");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* The budget does not admit an owner wait BEFORE the credited second event. */
    begin(low, high);
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(on_owner(job_poll));
    refused_owner_wait("unsupported second-event");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* Nor while the producer's credit is outstanding. */
    begin(low, high);
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(on_owner(job_poll));
    (void)d3d8_vblank_effects_apply(0x9000u);
    producer_wait();
    refused_owner_wait("unsupported second-event");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* A failed owner wait never delivers, a wait by another identity is the old refusal. */
    reach_second_event(2u);
    owner_blank();
    CHECK(!on_owner(job_wait_failed));
    CHECK(stopped_with("unsuccessful GPU wait"));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    CHECK(!on_owner(job_wait_wrong_fs));
    CHECK(stopped_with("unsupported second-event"));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    CHECK(!on_owner(job_wait_wrong_handle));
    CHECK(stopped_with("unsupported second-event"));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    const unsigned before = calls;
    producer_wait(); /* a producer wait after the second event, with a budget: still refused */
    CHECK_EQ_U32(calls, before);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK(snapshot.owner_delivered == 0u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    /* Another host thread presenting the owner's handle and PCR is not the owner. */
    reach_second_event(2u);
    owner_blank();
    atomic_store(&impostor_returned, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, impostor, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!atomic_load(&impostor_returned));
    CHECK_EQ_U32(calls, before + 2u); /* the two events of reach_second_event only */
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK(snapshot.owner_delivered == 0u && snapshot.refused);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}

static void test_budget_api(void)
{
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    CHECK(!recomp_second_vblank_set_owner_waits(1u)); /* needs an enabled policy */
    CHECK(recomp_second_vblank_set_owner_waits(0u));
    CHECK(recomp_second_vblank_configure(true, callback, low, high, confirmed));
    CHECK(!recomp_second_vblank_set_owner_waits(RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX + 1u));
    CHECK(recomp_second_vblank_set_owner_waits(RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX));
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX);
    CHECK(recomp_second_vblank_set_owner_waits(0u));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, 0u);
    CHECK(recomp_second_vblank_set_owner_waits(4u));
    CHECK(recomp_second_vblank_reset()); /* an epoch reset keeps the configured budget */
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, 4u);
    CHECK_EQ_U32(snapshot.owner_delivered, 0u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}

static void test_preconditions(void)
{
    /* Each of the three opt-in dependencies is checked at delivery, not only by the option parser. */
    reach_second_event(2u);
    recomp_vblank_quiescence_configure(false);
    refused_owner_wait("requires the quiescence check");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    owner_blank();
    d3d8_vblank_effects_configure(false);
    unsigned before = calls;
    CHECK(!on_owner(job_wait));
    CHECK(stopped_with("requires the coupled vblank effects"));
    expect_untouched(before, 2u, 6u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* Every device word of the measured empty state. */
    const uint32_t words[] = {0x1DB8u, 0x1DE4u, 0x1DECu, 0x1DDCu, 0x1D9Cu, 0x1DA8u, 0x2448u, 0x244Cu};
    for (unsigned index = 0u; index < sizeof words / sizeof words[0]; index++) {
        reach_second_event(2u);
        owner_blank();
        const uint32_t saved = load(DEV + words[index]);
        store(DEV + words[index], saved ^ 0x10u);
        refused_after_blank("nonempty or changed device bookkeeping");
        store(DEV + words[index], saved);
        CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    }
    /* The count is the waits applied plus the startup event, off by one either way refuses. */
    for (int delta = -1; delta <= 1; delta += 2) {
        reach_second_event(2u);
        owner_blank();
        store(DEV + 0x1DE8u, load(DEV + 0x1DE8u) + (uint32_t)delta);
        const uint32_t count = load(DEV + 0x1DE8u);
        const unsigned before_calls = calls;
        CHECK(!on_owner(job_wait));
        CHECK(stopped_with("nonempty or changed device bookkeeping"));
        expect_untouched(before_calls, 2u, count);
        CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    }
    /* The title counter must equal the deliveries so far plus 2. */
    reach_second_event(2u);
    store(COUNTER, 3u);
    refused_owner_wait("caller/PCR/device/counter");
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    /* The owner at DISPATCH_LEVEL (a raised thread level) is outside the measured state. */
    reach_second_event(2u);
    CHECK(on_owner(job_raise));
    refused_owner_wait("caller/PCR/device/counter");
    CHECK(on_owner(job_lower));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    /* PCR level byte, the fixed device pointer. */
    reach_second_event(2u);
    CHECK(kernel_guest_write_u8(fs + 0x24u, 2u));
    refused_owner_wait("caller/PCR/device/counter");
    CHECK(kernel_guest_write_u8(fs + 0x24u, 0u));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    store(0x3E3F58u, DEV + 4u);
    refused_owner_wait("caller/PCR/device/counter");
    store(0x3E3F58u, DEV);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}

static void test_quiescence(void)
{
    /* A runnable second reader: refused, nothing written. */
    reach_second_event(2u);
    atomic_store(&release_spinner, false);
    const uint32_t spinner = create_thread(0u);
    CHECK(wait_for_state(spinner, false));
    owner_blank();
    const unsigned before = calls;
    CHECK(!on_owner(job_wait));
    CHECK(stopped_with("quiescence refused"));
    CHECK(stopped_with("runnable"));
    CHECK_EQ_U32(owner_server.stop.guest_address, 0x3D3550u); /* the wait that was about to deliver */
    expect_untouched(before, 2u, 6u);
    /* The same state once the reader ended (the measured worker is terminated): it delivers. */
    atomic_store(&release_spinner, true);
    CHECK(wait_for_state(spinner, true));
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    reach_second_event(2u);
    owner_blank();
    CHECK(on_owner(job_wait));
    CHECK_EQ_U32(load(COUNTER), 3u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}

/* ---- T549: later entries of the frame wait ------------------------------------------------ */
static void end_epoch(void);
static void admitted_frames(uint32_t want)
{
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.frames_admitted, want);
    CHECK(!snapshot.inflight && !snapshot.refused);
}
static void test_later_frame_waits(void)
{
    /* The last exit word is not mapped yet: the poll refuses by name before anything is written. */
    reach_second_event(3u);
    unsigned before = calls;
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with("last exit unreadable"));
    CHECK_EQ_U32(calls, before);
    CHECK_EQ_U32(load(COUNTER), 2u);
    end_epoch();
    map_fixed(FRAME_LAST_EXIT & ~4095u, 4096u);

    /* With the budget already on, the credited SECOND event is still delivered by its poll and is
     * not mistaken for a later frame wait (counter 1 to 2, nothing admitted). */
    reach_credit();
    CHECK(recomp_second_vblank_set_owner_waits(3u));
    store(FRAME_LAST_EXIT, 0u);
    before = calls;
    CHECK(on_owner(job_poll));
    CHECK_EQ_U32(calls, before + 1u);
    CHECK_EQ_U32(load(COUNTER), 2u);
    recomp_second_vblank_snapshot second;
    recomp_second_vblank_get_snapshot(&second);
    CHECK(second.second_delivered && second.frames_admitted == 0u);
    end_epoch();

    /* The title's exit test already holds (counter 2, last exit 0): admitted, nothing delivered,
     * nothing written, counted, and the next owner wait still delivers its callback. */
    reach_second_event(3u);
    store(FRAME_LAST_EXIT, 0u);
    before = calls;
    const uint32_t count = load(DEV + 0x1DE8u);
    CHECK(on_owner(job_poll));
    CHECK(on_owner(job_poll));
    admitted_frames(2u);
    expect_untouched(before, 2u, count);
    CHECK_EQ_U32(load(FRAME_LAST_EXIT), 0u);
    owner_blank();
    expected_record = 6u;
    CHECK(on_owner(job_wait));
    CHECK_EQ_U32(calls, before + 1u);
    CHECK_EQ_U32(load(COUNTER), 3u);
    /* Counter 3 against last exit 2: one blank owed to the title and already delivered. */
    store(FRAME_LAST_EXIT, 2u);
    CHECK(on_owner(job_poll));
    admitted_frames(3u);
    /* Counter 3 against last exit 3: the title would spin. A named stop, nothing written. */
    store(FRAME_LAST_EXIT, 3u);
    const unsigned stopped_calls = calls;
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with("would spin for a blank nobody delivers"));
    CHECK_EQ_U32(calls, stopped_calls);
    CHECK_EQ_U32(load(COUNTER), 3u);
    end_epoch();

    /* A counter ahead of the last exit by exactly one is enough, by none or a wrapped distance not. */
    const struct { uint32_t counter, last; bool runs; } cases[] = {
        {2u, 1u, true}, {2u, 2u, false}, {2u, 3u, false}, {1u, 0xFFFFFFFFu, true},
        {0u, 0x80000000u, false}, {0x80000000u, 0u, false}, {0x7FFFFFFFu, 0u, true},
    };
    for (unsigned index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
        reach_second_event(2u);
        store(COUNTER, cases[index].counter);
        store(FRAME_LAST_EXIT, cases[index].last);
        if (cases[index].runs) {
            CHECK(on_owner(job_poll));
            admitted_frames(1u);
        } else {
            CHECK(!on_owner(job_poll));
            CHECK(stopped_with("would spin for a blank nobody delivers"));
        }
        end_epoch();
    }

    /* Without the owner-wait budget a third entry is the T183 refusal, whatever the title's state. */
    reach_second_event(0u);
    store(FRAME_LAST_EXIT, 0u);
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with("second-event credit/attempt refused"));
    end_epoch();

    /* A refused policy refuses a frame wait too (the reentry guard comes first). */
    reach_second_event(2u);
    store(FRAME_LAST_EXIT, 0u);
    leaf_mode = LEAF_FALSE;
    owner_blank();
    CHECK(!on_owner(job_wait));
    leaf_mode = LEAF_OK;
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with("owner/registration/reentry refused"));
    end_epoch();
}

static void test_probes_and_leaf(void)
{
    /* A page that stops being writable between the first deliveries and a later one. */
    struct { uint32_t page; const char *what; } pages[] = {
        {fs, "write probes"}, {COUNTER & ~4095u, "write probes"}, {low, "write probes"},
        {(DEV + 0x1DE8u) & ~4095u, "write probes"},
    };
    for (unsigned index = 0u; index < sizeof pages / sizeof pages[0]; index++) {
        reach_second_event(2u);
        owner_blank();
        const uint32_t count = load(DEV + 0x1DE8u);
        const unsigned before_calls = calls;
        CHECK(mprotect((void *)(uintptr_t)pages[index].page, 4096u, PROT_READ) == 0);
        CHECK(!on_owner(job_wait));
        CHECK(stopped_with(pages[index].what));
        CHECK(mprotect((void *)(uintptr_t)pages[index].page, 4096u, PROT_READ | PROT_WRITE) == 0);
        expect_untouched(before_calls, 2u, count);
        CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    }
    /* A leaf that fails, stops the host, forgets the counter or waits again. */
    const struct { int mode; const char *text; } leaves[] = {
        {LEAF_FALSE, "invocation failed"}, {LEAF_STOP, "leaf stop"},
        {LEAF_NO_COUNTER, "did not advance the counter"}, {LEAF_NESTED_WAIT, "wait completion during callback"},
        {LEAF_OTHER_THREAD_WAIT, "refill during callback"},
    };
    for (unsigned index = 0u; index < sizeof leaves / sizeof leaves[0]; index++) {
        reach_second_event(2u);
        owner_blank();
        leaf_mode = leaves[index].mode;
        const unsigned before = calls;
        CHECK(!on_owner(job_wait));
        CHECK(stopped_with(leaves[index].text));
        CHECK_EQ_U32(calls, before + 1u);
        recomp_second_vblank_snapshot snapshot;
        recomp_second_vblank_get_snapshot(&snapshot);
        CHECK(!snapshot.inflight && snapshot.refused && snapshot.owner_delivered == 0u);
        leaf_mode = LEAF_OK;
        /* A refused policy refuses the next wait too, it does not deliver again. */
        owner_blank();
        CHECK(!on_owner(job_wait));
        CHECK_EQ_U32(calls, before + 1u);
        CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    }
}

/* ---- T513: the flip model ----------------------------------------------------------------- */
static void end_epoch(void) { CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL)); }
/* The credit is outstanding: the second event's poll must refuse with `text`, nothing written. */
static void refused_second_event(const char *text)
{
    const unsigned before = calls;
    const uint32_t counter = load(COUNTER), count = load(DEV + 0x1DE8u);
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with(text));
    if (!stopped_with(text)) printf("    got: %s\n", owner_server.detail);
    CHECK_EQ_U32(calls, before);
    CHECK_EQ_U32(load(COUNTER), counter);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count);
}

static void test_flip_deliveries(void)
{
    /* The boot's own sequence with --model-flips: the second event and each owner wait get the
     * record the helper built (count, consumer index after the flips, flags 1), the budget bounds
     * them and the flip words are never written by the preflight. */
    flip_mode = FLIPS_QUEUED;
    reach_second_event(3u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, 4u);
    CHECK_EQ_U32(load(DEV + 0x1DE4u), 4u);
    CHECK_EQ_U32(load(DEV + 0x1DF4u), 4u);
    CHECK_EQ_U32(load(DEV + 0x1D9Cu), 0u);
    CHECK_EQ_U32(load(DEV + 0x1DA8u), 0u);
    const unsigned before = calls;
    for (uint32_t index = 1u; index <= 3u; index++) {
        owner_blank();
        expected_record = 5u + index;
        expected_index = 4u + index;
        expected_flags = 1u;
        CHECK(on_owner(job_wait));
        CHECK_EQ_U32(calls, before + index);
        CHECK_EQ_U32(load(COUNTER), 2u + index);
        CHECK_EQ_U32(load(DEV + 0x1DE4u), 4u + index);
        CHECK_EQ_U32(load(DEV + 0x1DF4u), 4u + index);
    }
    refused_owner_wait("budget exhausted");
    end_epoch();

    /* A blank with no flip due whose count meets the threshold: flags 2, the helper bumped it. */
    reach_second_event(2u);
    uint32_t guard = 0u;
    d3d8_vblank_record last = d3d8_vblank_effects_last_record();
    while (last.flags != 2u && guard++ < 8u) {
        (void)d3d8_vblank_effects_apply(0xB000u + 0x1000u * guard);
        last = d3d8_vblank_effects_last_record();
    }
    CHECK_EQ_U32(last.flags, 2u);
    CHECK_EQ_U32(load(DEV + 0x1DECu), last.count + 1u);
    expected_record = last.count;
    expected_index = 4u;
    expected_flags = 2u;
    CHECK(on_owner(job_wait));
    CHECK_EQ_U32(load(COUNTER), 3u);
    /* The record says the threshold was met, the threshold word says it was not. */
    owner_blank();
    last = d3d8_vblank_effects_last_record();
    CHECK_EQ_U32(last.flags, 1u);
    end_epoch();
    reach_second_event(2u);
    while (d3d8_vblank_effects_last_record().flags != 2u && guard++ < 16u)
        (void)d3d8_vblank_effects_apply(0xC000u + 0x1000u * guard);
    CHECK_EQ_U32(d3d8_vblank_effects_last_record().flags, 2u);
    store(DEV + 0x1DECu, load(DEV + 0x1DECu) + 5u);
    refused_after_blank("owner-wait helper record disagrees with the flip words");
    end_epoch();

    /* The flip model on and no flip ever queued: the measured empty state, a callback record of
     * [count, 0, 0], and a threshold nothing set is refused. */
    flip_mode = FLIPS_NONE;
    reach_second_event(2u);
    owner_blank();
    expected_record = 6u;
    CHECK(on_owner(job_wait));
    owner_blank();
    store(DEV + 0x1DECu, 9u);
    refused_after_blank("owner-wait flip threshold set without a queued flip");
    end_epoch();
    reach_credit();
    store(DEV + 0x1DECu, 9u);
    refused_second_event("second-event flip threshold set without a queued flip");
    end_epoch();
    flip_mode = FLIPS_OFF;
}

static void test_flip_refusals(void)
{
    flip_mode = FLIPS_QUEUED;
    /* The producer index is not the flips processed: a flip queued and not yet completed. */
    reach_second_event(2u);
    owner_blank();
    store(DEV + 0x1DF4u, load(DEV + 0x1DF4u) + 1u);
    refused_after_blank("owner-wait flip queue not drained");
    end_epoch();
    reach_credit();
    store(DEV + 0x1DF4u, load(DEV + 0x1DF4u) + 1u);
    refused_second_event("second-event flip queue not drained");
    end_epoch();
    reach_credit();
    store(DEV + 0x1DF4u, load(DEV + 0x1DF4u) - 1u);
    refused_second_event("second-event flip queue not drained");
    end_epoch();
    /* A flip queued by the model that no blank completed (a pending slot, producer one ahead). */
    reach_second_event(2u);
    owner_blank();
    flip_queue(2u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().queued, 6u);
    refused_after_blank("owner-wait nonempty or changed device bookkeeping");
    end_epoch();
    /* A flip the queue processed at once (immediate) AFTER the blank that built the record: the
     * consumer index moved past the record the callback would carry. */
    reach_second_event(2u);
    owner_blank();
    flip_queue(0x80000000u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, d3d8_flip_hardware_get().queued);
    refused_after_blank("owner-wait helper record disagrees with the flip words");
    end_epoch();
    reach_credit();
    flip_queue(0x80000000u);
    refused_second_event("second-event helper record disagrees with the flip words");
    end_epoch();
    /* The consumer index and both slots are device words of the measured state. */
    const uint32_t words[] = {0x1DE4u, 0x1D9Cu, 0x1DA8u};
    for (unsigned index = 0u; index < sizeof words / sizeof words[0]; index++) {
        reach_second_event(2u);
        owner_blank();
        store(DEV + words[index], load(DEV + words[index]) + 1u);
        refused_after_blank("owner-wait nonempty or changed device bookkeeping");
        end_epoch();
        reach_credit();
        store(DEV + words[index], load(DEV + words[index]) + 1u);
        refused_second_event("second-event nonempty or changed device bookkeeping");
        end_epoch();
    }
    /* Every guest word says drained but the model still holds a queued flip nobody completed. */
    reach_second_event(2u);
    owner_blank();
    flip_queue(2u);
    store(DEV + 0x1DF4u, load(DEV + 0x1DE4u));
    store(DEV + 0x1D9Cu, 0u);
    store(DEV + 0x1DA8u, 0u);
    CHECK(d3d8_flip_hardware_get().queued != d3d8_flip_hardware_get().flips);
    refused_after_blank("owner-wait flip queue not drained");
    end_epoch();
    /* The count the device word and the waits applied agree on, with the record of another count
     * (the effects were reconfigured, one blank applied, the word then set to the new relation). */
    reach_second_event(2u);
    owner_blank();
    d3d8_vblank_effects_configure(true);
    store(DEV + 0x1DECu, 0u); /* no threshold hit: the record's flags stay out of the way */
    (void)d3d8_vblank_effects_apply(0xE000u);
    CHECK_EQ_U32(d3d8_vblank_effects_last_record().flags, 0u);
    store(DEV + 0x1DE8u, 2u);
    refused_after_blank("owner-wait helper record disagrees with the flip words");
    end_epoch();
    /* A record from no blank of this configuration (the effects were reconfigured: the count the
     * device word and the waits applied agree on is 1, the record the helper left is empty). */
    reach_second_event(2u);
    owner_blank();
    d3d8_vblank_effects_configure(true);
    store(DEV + 0x1DE8u, 1u);
    refused_after_blank("owner-wait helper record disagrees with the flip words");
    end_epoch();

    /* NOTHING LOOSENS with the flip model off: the same flip state is the T183 refusal. */
    reach_credit();
    d3d8_flip_configure(false);
    refused_second_event("second-event nonempty or changed device bookkeeping");
    end_epoch();
    reach_second_event(2u);
    owner_blank();
    d3d8_flip_configure(false);
    refused_after_blank("owner-wait nonempty or changed device bookkeeping");
    end_epoch();
    /* ... and with the effects off the second event is the uncoupled constants (count 2). */
    reach_credit();
    d3d8_vblank_effects_configure(false);
    store(DEV + 0x1DE8u, 2u);
    store(DEV + 0x1DECu, 0u);
    refused_second_event("second-event nonempty or changed device bookkeeping");
    end_epoch();
    flip_mode = FLIPS_OFF;
    /* The measured zero threshold is still demanded at the second event with the flip model off. */
    reach_credit();
    store(DEV + 0x1DECu, 9u);
    refused_second_event("second-event nonempty or changed device bookkeeping");
    end_epoch();
}

/* ---- T592: the loading bar worker's blanks ------------------------------------------------ */
#define GATE_ENTRY 0x156840u
#define GATE_WAIT_CALL 0x3800BFu
#define GETTER_CALL 0x22030u
#define ONE_BITS 0x3F800000u
static uint32_t worker_creation_serial;
static uint32_t worker_system = 0x37FE1Du, worker_context, worker_start = 0x156CB0u;
static uint32_t create_worker(void)
{
    const uint32_t out = scratch + 0xA00u + (worker_creation_serial++ % 8u) * 4u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, worker_start, worker_context, 0u, 0u, worker_system};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(out, &handle));
    return handle;
}
/* A fresh worker thread: the old one is released and ended first, its served counters restart. */
static void end_worker(void)
{
    if (worker_handle == 0u) return;
    atomic_store(&worker_server.release, true);
    CHECK(wait_for_state(worker_handle, true));
    worker_handle = 0u;
}
static void new_worker(void)
{
    end_worker();
    atomic_store(&worker_server.request, 0u);
    atomic_store(&worker_server.served, 0u);
    atomic_store(&worker_server.returned, false);
    atomic_store(&worker_server.release, false);
    worker_server.detail[0] = '\0';
    worker_handle = create_worker();
    CHECK(worker_handle != 0u);
    CHECK(wait_for_state(worker_handle, false));
    kernel_thread_record record;
    CHECK(kernel_thread_get(worker_handle, &record));
    CHECK_EQ_U32(record.start_routine, worker_start);
    worker_fs = record.control_base;
}
static void job_worker_wait(void) { recomp_second_vblank_note_wait_completed(true, worker_handle, worker_fs); }
static void job_worker_wait_wrong_fs(void) { recomp_second_vblank_note_wait_completed(true, worker_handle, worker_fs + 0x1000u); }
static void job_worker_poll(void) { recomp_second_vblank_note_call(GETTER_CALL, worker_handle); }
static uint32_t owner_callee;
static void job_owner_call(void) { recomp_second_vblank_note_call(owner_callee, owner_handle); }
static void owner_calls(uint32_t callee)
{
    owner_callee = callee;
    CHECK(on_owner(job_owner_call));
}
static void set_gate_memory(uint32_t flag, uint32_t running, uint32_t stage, uint32_t target, uint32_t progress)
{
    store(0x4E7A94u, flag);
    store(0x74A9ACu, running);
    store(0x4E7968u, stage);
    store(0x74A990u, target);
    store(0x475C78u, ONE_BITS);
    store(0x749848u, progress);
}
/* The measured state at the worker's wait: owner budget on, the credited second event delivered, the
 * worker budget on, a fresh worker thread, the loading bar memory as the gate prologue leaves it. */
static void worker_epoch(uint32_t worker_budget)
{
    reach_second_event(4u);
    CHECK(recomp_second_vblank_set_worker_blanks(worker_budget));
    recomp_second_vblank_set_worker_hold_ms(60u);
    new_worker();
    set_gate_memory(0u, 1u, 0xAu, ONE_BITS, 0u);
}
static void worker_blank_applied(void) { blank(0xA000u + 0x1000u * (uint32_t)(calls)); }
static void expect_worker_untouched(unsigned calls_before, uint32_t counter, uint32_t count)
{
    expect_untouched(calls_before, counter, count);
}
static bool worker_stopped_with(const char *text)
{
    const bool found = server_stopped_with(&worker_server, text);
    if (!found) printf("    worker got: %s\n", worker_server.detail);
    return found;
}
/* Park the owner for real: it blocks on the worker's termination (NtWaitForSingleObjectEx, untimed). */
static uint32_t wait_args_handle;
static void job_owner_blocks(void)
{
    const uint32_t args[4] = {wait_args_handle, 1u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x300u, 20u, args, 4u));
    (void)kernel_hle_call(234u, &frame);
}
static unsigned blocked_job_want;
static void owner_blocks_on_worker(void)
{
    wait_args_handle = worker_handle;
    blocked_job_want = atomic_load(&owner_server.served) + 1u;
    start_on(&owner_server, job_owner_blocks);
    kernel_thread_record record;
    bool blocked = false;
    for (unsigned i = 0u; i < 3000u && !blocked; i++) {
        blocked = kernel_thread_get(owner_handle, &record) && record.block_state == KERNEL_THREAD_BLOCK_THREAD &&
                  record.block_target == worker_handle;
        if (!blocked) pause_ms(1);
    }
    CHECK(blocked);
}
static void release_blocked_owner(void)
{
    end_worker();
    (void)finish_on(&owner_server, blocked_job_want);
}

static void test_worker_budget_api(void)
{
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    CHECK(!recomp_second_vblank_set_worker_blanks(1u)); /* needs an enabled policy */
    CHECK(recomp_second_vblank_set_worker_blanks(0u));
    CHECK(recomp_second_vblank_configure(true, callback, low, high, confirmed));
    CHECK(!recomp_second_vblank_set_worker_blanks(1u)); /* and the owner waits */
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(!recomp_second_vblank_set_worker_blanks(RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX + 1u));
    CHECK(recomp_second_vblank_set_worker_blanks(RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX));
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_budget, RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX);
    CHECK_EQ_U32(snapshot.worker_delivered, 0u);
    CHECK(recomp_second_vblank_set_worker_blanks(0u));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_budget, 0u);
    CHECK(recomp_second_vblank_set_worker_blanks(3u));
    CHECK(recomp_second_vblank_reset()); /* an epoch reset keeps the configured budget */
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_budget, 3u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_budget, 0u); /* configure clears it */
}

static void test_worker_deliveries(void)
{
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_gate_entries, 1u);
    const unsigned before = calls;
    probe_budget_in_callback = true;
    for (uint32_t index = 1u; index <= 3u; index++) {
        worker_blank_applied();
        expected_record = 5u + index;
        CHECK(on_server(&worker_server, job_worker_wait));
        CHECK(!worker_budget_accepted_in_callback && !budget_accepted_in_callback); /* not while a callback runs */
        CHECK_EQ_U32(calls, before + index);
        CHECK_EQ_U32(load(COUNTER), 2u + index);
        CHECK_EQ_U32(load(DEV + 0x1DE8u), 5u + index);
        recomp_second_vblank_get_snapshot(&snapshot);
        CHECK_EQ_U32(snapshot.worker_delivered, index);
        CHECK_EQ_U32(snapshot.owner_delivered, 0u);
        CHECK(!snapshot.inflight && !snapshot.refused);
        CHECK_EQ_U32(snapshot.worker_held, 0u); /* the owner was already parked, nothing waited */
    }
    probe_budget_in_callback = false;
    /* The budget is spent: a named stop before the hold, no callback, never coalesced. */
    worker_blank_applied();
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait callback budget exhausted"));
    CHECK_EQ_U32(calls, before + 3u);
    CHECK_EQ_U32(load(COUNTER), 5u);
    end_worker();
    end_epoch();

    /* The title counter the delivery expects counts every blank: a wrong one refuses by name. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    store(COUNTER, 9u);
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait caller/PCR/device/counter scope refused"));
    end_worker();
    end_epoch();

    /* A device word off the measured state refuses with the worker's own text, nothing written. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    store(DEV + 0x1DECu, 7u);
    const unsigned calls_before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait nonempty or changed device bookkeeping"));
    expect_worker_untouched(calls_before, 2u, 6u);
    end_worker();
    end_epoch();

    /* The leaf fails: the worker's text, the policy is refused afterwards. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    leaf_mode = LEAF_NO_COUNTER;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait callback did not advance the counter by one"));
    leaf_mode = LEAF_FALSE;
    end_worker();
    end_epoch();
    leaf_mode = LEAF_OK;
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    leaf_mode = LEAF_FALSE;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait callback invocation failed"));
    leaf_mode = LEAF_OK;
    end_worker();
    end_epoch();
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    leaf_mode = LEAF_NESTED_WAIT;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("wait completion during callback"));
    leaf_mode = LEAF_OK;
    end_worker();
    end_epoch();
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    leaf_mode = LEAF_OTHER_THREAD_WAIT;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait refill during callback refused"));
    leaf_mode = LEAF_OK;
    end_worker();
    end_epoch();

    /* The flip model: the helper's record is the callback's, the worker's refusal texts differ. */
    flip_mode = FLIPS_QUEUED;
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    expected_record = 6u;
    expected_index = 5u;
    expected_flags = 1u;
    CHECK(on_server(&worker_server, job_worker_wait));
    CHECK_EQ_U32(load(DEV + 0x1DE4u), 5u);
    worker_blank_applied();
    store(DEV + 0x1DF4u, load(DEV + 0x1DF4u) + 1u);
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker-wait flip queue not drained"));
    end_worker();
    end_epoch();
    flip_mode = FLIPS_OFF;
}

static void test_interactive_worker_continuation(void)
{
    worker_epoch(1u);
    owner_calls(GATE_ENTRY);
    CHECK(recomp_second_vblank_set_interactive(true));
    for (uint32_t index = 1u; index <= 4u; index++) {
        worker_blank_applied(); expected_record = 5u + index;
        CHECK(on_server(&worker_server, job_worker_wait));
        CHECK_EQ_U32(load(COUNTER), 2u + index);
    }
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_budget, 1u);
    CHECK_EQ_U32(snapshot.worker_delivered, 4u);
    end_worker(); end_epoch();
}

static bool shutdown_requested(void) { return true; }
static void test_interactive_worker_shutdown(void)
{
    worker_epoch(1u);
    CHECK(recomp_second_vblank_set_interactive(true));
    recomp_second_vblank_set_worker_hold_ms(30000u);
    recomp_second_vblank_set_shutdown_query(shutdown_requested);
    worker_blank_applied();
    const unsigned before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(strstr(worker_server.detail, "interactive host shutdown while holding worker") != NULL);
    CHECK(worker_server.stop.reason == HOST_STOP_HOST_SHUTDOWN);
    CHECK(strcmp(host_stop_reason_str(HOST_STOP_HOST_SHUTDOWN), "host shutdown requested") == 0);
    CHECK_EQ_U32(calls, before);
    CHECK_EQ_U32(load(COUNTER), 2u);
    end_worker();
    end_epoch(); /* next configure clears the query; ordinary worker tests follow */
}

static void test_worker_hold(void)
{
    /* The owner is elsewhere: the worker idles in its wait for the limit, then a named stop that says
     * why, with nothing written. */
    worker_epoch(2u);
    worker_blank_applied();
    unsigned before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("worker blank held 60 ms without quiescence (T592)"));
    CHECK(strstr(worker_server.detail, "another guest thread is runnable") != NULL);
    CHECK_EQ_U32(calls, before);
    CHECK_EQ_U32(load(COUNTER), 2u);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_held, 1u);
    CHECK_EQ_U32(snapshot.worker_delivered, 0u);
    end_worker();
    end_epoch();

    /* The owner arrives while the worker idles: it is released and delivers. */
    worker_epoch(2u);
    recomp_second_vblank_set_worker_hold_ms(5000u);
    worker_blank_applied();
    before = calls;
    start_on(&worker_server, job_worker_wait);
    for (unsigned i = 0u; i < 2000u; i++) {
        recomp_second_vblank_get_snapshot(&snapshot);
        if (snapshot.worker_held != 0u) break;
        pause_ms(1);
    }
    CHECK_EQ_U32(snapshot.worker_held, 1u);
    CHECK_EQ_U32(calls, before);
    owner_calls(GATE_ENTRY);
    CHECK(finish_on(&worker_server, 1u));
    CHECK_EQ_U32(calls, before + 1u);
    CHECK_EQ_U32(load(COUNTER), 3u);
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_delivered, 1u);
    end_worker();
    end_epoch();

    /* The gate prologue stores are not visible yet: parked by location is not enough. Stage 9. */
    worker_epoch(2u);
    set_gate_memory(0u, 1u, 9u, 0u, 0u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("quiescence holds but the owner's gate prologue stores are not visible"));
    CHECK_EQ_U32(calls, before);
    end_worker();
    end_epoch();
    /* The stage alone is not enough either, nor the target alone. */
    for (unsigned variant = 0u; variant < 2u; variant++) {
        worker_epoch(2u);
        set_gate_memory(0u, 1u, variant == 0u ? 9u : 0xAu, variant == 0u ? ONE_BITS : 0u, 0u);
        owner_calls(GATE_ENTRY);
        worker_blank_applied();
        before = calls;
        CHECK(!on_server(&worker_server, job_worker_wait));
        CHECK(worker_stopped_with("gate prologue stores are not visible"));
        CHECK_EQ_U32(calls, before);
        end_worker();
        end_epoch();
    }
    /* ... they land while the worker idles: released. */
    worker_epoch(2u);
    recomp_second_vblank_set_worker_hold_ms(5000u);
    set_gate_memory(0u, 1u, 9u, 0u, 0u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    before = calls;
    start_on(&worker_server, job_worker_wait);
    pause_ms(30);
    CHECK_EQ_U32(calls, before);
    store(0x4E7968u, 0xAu);
    pause_ms(30);
    CHECK_EQ_U32(calls, before); /* the target is still the old one */
    store(0x74A990u, ONE_BITS);
    CHECK(finish_on(&worker_server, 1u));
    CHECK_EQ_U32(calls, before + 1u);
    end_worker();
    end_epoch();

    /* The two entry conditions of the proof: a gate that is already passed or a bar that is not running. */
    for (unsigned variant = 0u; variant < 2u; variant++) {
        worker_epoch(2u);
        set_gate_memory(variant == 0u ? 1u : 0u, variant == 0u ? 1u : 0u, 0xAu, ONE_BITS, 0u);
        owner_calls(GATE_ENTRY);
        recomp_second_vblank_get_snapshot(&snapshot);
        CHECK_EQ_U32(snapshot.owner_gate_entries, 0u);
        worker_blank_applied();
        CHECK(!on_server(&worker_server, job_worker_wait));
        CHECK(worker_stopped_with("held 60 ms without quiescence"));
        end_worker();
        end_epoch();
    }

    /* Any later call leaves the gate: the owner is elsewhere again and the worker idles. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    owner_calls(GATE_WAIT_CALL); /* its one wait: still parked by location while the wrapper runs up to the block */
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    owner_calls(0x123456u); /* the wrapper's next call (not blocked in this test): the location ended */
    worker_blank_applied();
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("held 60 ms without quiescence"));
    end_worker();
    end_epoch();
}

static void test_worker_final_hold(void)
{
    /* The blank that fills the bar (progress 1.0 at the wait): delivered, then the worker idles until the
     * owner has blocked on it. The owner has not, so after the limit a named stop, the blank already counted. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    store(0x749848u, ONE_BITS);
    worker_blank_applied();
    const unsigned before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("the owner left the gate but never blocked on the worker (T592)"));
    CHECK_EQ_U32(calls, before + 1u);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_delivered, 1u);
    CHECK_EQ_U32(snapshot.worker_final_held, 1u);
    end_worker();
    end_epoch();

    /* A bar that is not full never takes the final hold, whatever the owner is doing. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    store(0x749848u, 0x3F7FFFFBu); /* the title's 60th step: 0.99999976 < 1.0 */
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_final_held, 0u);
    end_worker();
    end_epoch();

    /* The owner really blocked on the worker: the verdict holds by the blocked rule alone (the gate call
     * was never made), the final hold passes at once, the wait returns. */
    worker_epoch(3u);
    set_gate_memory(0u, 1u, 9u, 0u, 0x3F822220u); /* the prologue stores are not what releases it */
    worker_blank_applied();
    owner_blocks_on_worker();
    CHECK(on_server(&worker_server, job_worker_wait));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_delivered, 1u);
    CHECK_EQ_U32(snapshot.worker_final_held, 0u);
    CHECK_EQ_U32(snapshot.worker_held, 0u);
    release_blocked_owner();
    end_epoch();

    /* An owner blocked on ANOTHER thread (one that itself waits for the worker, so it is parked) is not the
     * owner blocked on the worker: the blank is delivered, the final hold does not end. */
    worker_epoch(3u);
    const uint32_t middle = create_thread(5u);
    CHECK(middle != 0u);
    for (unsigned i = 0u; i < 3000u; i++) {
        kernel_thread_record waiting;
        if (kernel_thread_get(middle, &waiting) && waiting.block_state == KERNEL_THREAD_BLOCK_THREAD) break;
        pause_ms(1);
    }
    store(0x749848u, ONE_BITS);
    worker_blank_applied();
    wait_args_handle = middle;
    blocked_job_want = atomic_load(&owner_server.served) + 1u;
    start_on(&owner_server, job_owner_blocks);
    for (unsigned i = 0u; i < 3000u; i++) {
        kernel_thread_record blocked;
        if (kernel_thread_get(owner_handle, &blocked) && blocked.block_state == KERNEL_THREAD_BLOCK_THREAD) break;
        pause_ms(1);
    }
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("never blocked on the worker"));
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_delivered, 1u);
    end_worker(); /* the worker ends, the middle thread's wait returns, then the owner's */
    (void)finish_on(&owner_server, blocked_job_want);
    CHECK(wait_for_state(middle, true));
    end_epoch();

    /* A NaN progress leaves the title's loop, so it counts as full too. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    store(0x749848u, 0x7FC00000u);
    worker_blank_applied();
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("never blocked on the worker"));
    end_worker();
    end_epoch();
}

static void test_worker_poll_hold(void)
{
    /* Before the credited second event the poll hook does nothing (the producer polls then). */
    begin(low, high);
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(recomp_second_vblank_set_worker_blanks(2u));
    recomp_second_vblank_set_worker_hold_ms(60u);
    new_worker();
    set_gate_memory(0u, 1u, 0xAu, ONE_BITS, 0u);
    CHECK(on_server(&worker_server, job_worker_poll));
    end_worker();
    end_epoch();

    /* After it, a worker poll with the owner elsewhere idles for the limit. */
    worker_epoch(2u);
    CHECK(!on_server(&worker_server, job_worker_poll));
    CHECK(worker_stopped_with("worker blank held 60 ms without quiescence (T592)"));
    end_worker();
    end_epoch();
    /* ... with the owner in the gate it passes at once and counts nothing as held. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    CHECK(on_server(&worker_server, job_worker_poll));
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.worker_held, 0u);
    end_worker();
    end_epoch();
    /* A poll by a thread that is not the worker (the owner's own frame wait poll) is not held. */
    worker_epoch(2u);
    owner_calls(GETTER_CALL);
    end_worker();
    end_epoch();
    /* A poll once the owner blocked on the worker is the stop test lost to a skipped frame. */
    worker_epoch(2u);
    owner_blocks_on_worker();
    CHECK(!on_server(&worker_server, job_worker_poll));
    CHECK(worker_stopped_with("polled the vblank counter after the owner blocked on its termination"));
    release_blocked_owner();
    end_epoch();
    /* A refused policy does not hold the worker (nothing will be delivered anyway). */
    worker_epoch(2u);
    owner_blank();
    CHECK(!on_owner(job_wait_failed)); /* poisons the policy */
    CHECK(on_server(&worker_server, job_worker_poll));
    end_worker();
    end_epoch();
    /* Budget off: the hook is inert, a poll is never held. */
    reach_second_event(2u);
    new_worker();
    CHECK(on_server(&worker_server, job_worker_poll));
    end_worker();
    end_epoch();
}

static void test_worker_owner_tracking(void)
{
    /* A worker epoch (a blank delivered, the worker alive): the owner reading the counter is a named stop. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    owner_callee = GETTER_CALL; /* from the gate it is a call outside the proof */
    CHECK(!on_owner(job_owner_call));
    CHECK(stopped_with("left the loading bar gate through a call outside the proof (T592)"));
    owner_calls(GATE_ENTRY);
    owner_calls(GATE_WAIT_CALL);
    owner_callee = GETTER_CALL; /* after its wait the owner must not read while the worker lives */
    CHECK(!on_owner(job_owner_call));
    CHECK(stopped_with("owner sampled the vblank counter while the worker's blanks were delivered (T592)"));
    end_worker(); /* the worker ended: the owner may read again */
    owner_callee = GETTER_CALL;
    CHECK(on_owner(job_owner_call));
    end_epoch();

    /* A call out of the gate other than its one wait is outside the proof. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    owner_callee = 0x123456u;
    CHECK(!on_owner(job_owner_call));
    CHECK(stopped_with("left the loading bar gate through a call outside the proof (T592)"));
    end_worker();
    end_epoch();
    /* The wait is the one call: it ends the location without a stop, the next call is not the gate's. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    owner_calls(GATE_WAIT_CALL);
    owner_calls(0x123456u);
    end_worker();
    end_epoch();

    /* Without an epoch (no blank delivered yet) the same calls are not stops. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    owner_calls(0x123456u);
    owner_calls(GETTER_CALL);
    end_worker();
    end_epoch();

    /* A call of the gate with the conditions false is not the gate. */
    worker_epoch(3u);
    set_gate_memory(1u, 1u, 0xAu, ONE_BITS, 0u);
    owner_calls(GATE_ENTRY);
    owner_calls(0x123456u);
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_gate_entries, 0u);
    end_worker();
    end_epoch();

    /* Budget off: the hook tracks nothing. */
    reach_second_event(2u);
    owner_calls(GATE_ENTRY);
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_gate_entries, 0u);
    end_epoch();
}

/* ---- T604: a call of a running flag writer entry by a thread other than the owner ----------- */
#define RUNNING_STOP_CALL 0x155550u
#define RUNNING_START_CALL 0x156D80u
#define RUNNING_BODY_CALL 0x156D10u
static const uint32_t writer_entries[] = {RUNNING_STOP_CALL, RUNNING_START_CALL, RUNNING_BODY_CALL, GATE_ENTRY};
#define WRITER_ENTRIES (sizeof writer_entries / sizeof writer_entries[0])
static uint32_t worker_callee;
static void job_worker_call(void) { recomp_second_vblank_note_call(worker_callee, worker_handle); }
static bool worker_calls(uint32_t callee)
{
    worker_callee = callee;
    return on_server(&worker_server, job_worker_call);
}
/* The stop text names the thread, its start, the entry and why the claim is relied on. */
static bool writer_stop(uint32_t callee, uint32_t start, const char *why)
{
    char head[96];
    snprintf(head, sizeof head, "thread 0x%x (start 0x%x) called 0x%x, a running flag writer entry, while ",
             (unsigned)worker_handle, (unsigned)start, (unsigned)callee);
    const bool found = !worker_calls(callee) && worker_stopped_with(head) && worker_stopped_with(why) &&
                       worker_stopped_with("the owner is not the only writer of [0x74A9AC] (T604)");
    return found && strlen(worker_server.detail) < 255u; /* host_stop keeps 255 bytes, the end is intact */
}
/* A worker epoch with the owner out of the gate: the owner went through its one wait and made another call. */
static void epoch_with_owner_past_the_gate(uint32_t budget)
{
    worker_epoch(budget);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    owner_calls(GATE_WAIT_CALL);
    owner_calls(0x123456u);
}
static void test_running_flag_writers(void)
{
    /* In a worker epoch with the owner past the gate, another thread (the worker here) calling any entry stops. */
    epoch_with_owner_past_the_gate(3u);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++)
        CHECK(writer_stop(writer_entries[index], 0x156CB0u, "the worker's blanks are delivered"));
    /* The owner calls them freely there (the stop function runs after the gate in the original). */
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) owner_calls(writer_entries[index]);
    CHECK(worker_calls(0x123456u)); /* a call that is no entry is not a stop */
    end_worker();
    end_epoch();

    /* The owner in the gate, no blank delivered yet: the claim is relied on, so the same stop with that reason. */
    worker_epoch(3u);
    owner_calls(GATE_ENTRY);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++)
        CHECK(writer_stop(writer_entries[index], 0x156CB0u, "the owner is in the loading bar gate"));
    owner_calls(GATE_WAIT_CALL); /* the owner waiting in the gate is still the gate */
    CHECK(writer_stop(RUNNING_STOP_CALL, 0x156CB0u, "the owner is in the loading bar gate"));
    end_worker();
    end_epoch();

    /* An impostor with another start is no exception: any thread but the owner, and its start is named. */
    worker_start = 0x123457u;
    worker_epoch(3u);
    worker_start = 0x156CB0u;
    owner_calls(GATE_ENTRY);
    CHECK(writer_stop(RUNNING_START_CALL, 0x123457u, "the owner is in the loading bar gate"));
    end_worker();
    end_epoch();

    /* Outside both states the claim is not relied on, a call is not a stop: no gate and no epoch. */
    worker_epoch(3u);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) CHECK(worker_calls(writer_entries[index]));
    owner_calls(0x123456u);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) CHECK(worker_calls(writer_entries[index]));
    end_worker();
    end_epoch();

    /* The epoch ended (the worker terminated) and the owner is past the gate: a later thread may call. */
    epoch_with_owner_past_the_gate(3u);
    end_worker();
    worker_start = 0x123457u;
    new_worker();
    worker_start = 0x156CB0u;
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) CHECK(worker_calls(writer_entries[index]));
    end_worker();
    end_epoch();

    /* The gate conditions false (the gate is not entered): no gate, no stop. */
    worker_epoch(3u);
    set_gate_memory(1u, 1u, 0xAu, ONE_BITS, 0u);
    owner_calls(GATE_ENTRY);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) CHECK(worker_calls(writer_entries[index]));
    end_worker();
    end_epoch();

    /* Budget off: the hook tracks nothing, not even an owner in a gate. */
    reach_second_event(2u);
    new_worker();
    set_gate_memory(0u, 1u, 0xAu, ONE_BITS, 0u);
    owner_calls(GATE_ENTRY);
    for (unsigned index = 0u; index < WRITER_ENTRIES; index++) CHECK(worker_calls(writer_entries[index]));
    end_worker();
    end_epoch();
}

static void test_worker_identity(void)
{
    /* The thread table decides: the worker's start routine with another system routine or another start
     * context is not the worker, and its wait is the T587 refusal whatever the owner and the memory say. */
    for (unsigned variant = 0u; variant < 3u; variant++) {
        reach_second_event(2u);
        CHECK(recomp_second_vblank_set_worker_blanks(2u));
        recomp_second_vblank_set_worker_hold_ms(60u);
        worker_system = variant == 0u ? 0u : 0x37FE1Du;
        worker_context = variant == 1u ? 1u : 0u;
        worker_start = variant == 2u ? 0x123457u : 0x156CB0u; /* the right shim and context, another start */
        new_worker();
        worker_system = 0x37FE1Du;
        worker_context = 0u;
        worker_start = 0x156CB0u;
        set_gate_memory(0u, 1u, 0xAu, ONE_BITS, 0u);
        owner_calls(GATE_ENTRY);
        worker_blank_applied();
        const unsigned variant_calls = calls;
        CHECK(!on_server(&worker_server, job_worker_wait));
        CHECK(worker_stopped_with("unsupported second-event refill/producer/backlog"));
        CHECK_EQ_U32(calls, variant_calls);
        end_worker();
        end_epoch();
    }
    /* Before the second event a worker's wait is still the producer's credit, never a worker blank. */
    begin(low, high);
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(recomp_second_vblank_set_worker_blanks(2u));
    recomp_second_vblank_set_worker_hold_ms(60u);
    new_worker();
    set_gate_memory(0u, 1u, 0xAu, ONE_BITS, 0u);
    owner_calls(GATE_ENTRY);
    CHECK(on_owner(job_poll)); /* the startup event */
    (void)d3d8_vblank_effects_apply(0x9000u);
    const unsigned credit_calls = calls;
    CHECK(on_server(&worker_server, job_worker_wait));
    recomp_second_vblank_snapshot credit;
    recomp_second_vblank_get_snapshot(&credit);
    CHECK_EQ_U32((uint32_t)credit.credits, 1u);
    CHECK_EQ_U32(credit.worker_delivered, 0u);
    CHECK_EQ_U32(calls, credit_calls);
    end_worker();
    end_epoch();
    /* The owner waits switched off again: the worker's wait is the T587 refusal, byte for byte. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    CHECK(recomp_second_vblank_set_owner_waits(0u));
    worker_blank_applied();
    const unsigned off_calls = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with(PLAIN_REFUSAL));
    CHECK_EQ_U32(calls, off_calls);
    end_worker();
    end_epoch();
    /* A refused policy delivers nothing more, to the worker either. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    CHECK(on_server(&worker_server, job_worker_wait));
    CHECK(!on_owner(job_wait_failed)); /* poisons the policy */
    worker_blank_applied();
    const unsigned refused_calls = calls;
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with("unsupported second-event refill/producer/backlog"));
    CHECK_EQ_U32(calls, refused_calls);
    end_worker();
    end_epoch();

    /* The worker's identity is the thread table's: a wrong control block is the T587 refusal. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    const unsigned before = calls;
    CHECK(!on_server(&worker_server, job_worker_wait_wrong_fs));
    CHECK(worker_stopped_with("unsupported second-event refill/producer/backlog"));
    CHECK(strstr(worker_server.detail, "(T587)") != NULL);
    CHECK_EQ_U32(calls, before);
    end_worker();
    end_epoch();
    /* A thread with another start routine is never the worker, even with the budget and the gate. */
    worker_epoch(2u);
    owner_calls(GATE_ENTRY);
    worker_blank_applied();
    const unsigned before_second = calls;
    second_thread_wait(false);
    CHECK(!atomic_load(&second_returned));
    CHECK(strncmp(second_detail, PLAIN_REFUSAL, strlen(PLAIN_REFUSAL)) == 0);
    CHECK_EQ_U32(calls, before_second);
    end_worker();
    end_epoch();
    /* Budget off: a real worker's wait is the T587 refusal, byte for byte. */
    reach_second_event(2u);
    new_worker();
    worker_blank_applied();
    CHECK(!on_server(&worker_server, job_worker_wait));
    CHECK(worker_stopped_with(PLAIN_REFUSAL));
    CHECK(strstr(worker_server.detail, "no second-thread delivery (T587)") != NULL);
    end_worker();
    end_epoch();
}

static void test_worker_with_held_owner_wait(void)
{
    /* The owner delivers its own blank while the worker idles in its hold: the worker is parked by this
     * module, so the owner's quiescence holds, and the worker's later blank counts both. */
    worker_epoch(3u);
    recomp_second_vblank_set_worker_hold_ms(5000u);
    worker_blank_applied();
    start_on(&worker_server, job_worker_wait);
    recomp_second_vblank_snapshot snapshot;
    for (unsigned i = 0u; i < 2000u; i++) {
        recomp_second_vblank_get_snapshot(&snapshot);
        if (snapshot.worker_held != 0u) break;
        pause_ms(1);
    }
    CHECK_EQ_U32(snapshot.worker_held, 1u);
    owner_blank();
    expected_record = 7u;
    CHECK(on_owner(job_wait));
    CHECK_EQ_U32(load(COUNTER), 3u);
    owner_calls(GATE_ENTRY);
    expected_record = 0u;
    CHECK(finish_on(&worker_server, 1u));
    CHECK_EQ_U32(load(COUNTER), 4u);
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_delivered, 1u);
    CHECK_EQ_U32(snapshot.worker_delivered, 1u);
    end_worker();
    end_epoch();
    /* Without the hold the runnable worker still refuses the owner's blank (the T460 rule). */
    worker_epoch(3u);
    owner_blank();
    CHECK(!on_owner(job_wait));
    CHECK(stopped_with("quiescence refused"));
    end_worker();
    end_epoch();
}


/* ---- T696: the frame wait poll delivers the one blank its exit test is owed --------------- */
static void poll_snapshot(uint32_t delivered, uint32_t from_poll, uint32_t admitted)
{
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_delivered, delivered);
    CHECK_EQ_U32(snapshot.poll_delivered, from_poll);
    CHECK_EQ_U32(snapshot.frames_admitted, admitted);
    CHECK(!snapshot.inflight && !snapshot.refused);
}
/* A poll that must stop with `text` having written nothing: no callback, counter, device count or applied blank. */
static void poll_refused(const char *text)
{
    const unsigned before = calls;
    const uint32_t counter = load(COUNTER), count = load(DEV + 0x1DE8u), last = load(FRAME_LAST_EXIT);
    const uint64_t applied = d3d8_vblank_effects_applied();
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with(text));
    if (!stopped_with(text)) printf("    got: %s\n", owner_server.detail);
    CHECK_EQ_U32(calls, before);
    CHECK_EQ_U32(load(COUNTER), counter);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count);
    CHECK_EQ_U32(load(FRAME_LAST_EXIT), last);
    CHECK(d3d8_vblank_effects_applied() == applied);
}
static void test_poll_blank_api(void)
{
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    CHECK(!recomp_second_vblank_set_poll_blank(true)); /* needs an enabled policy */
    CHECK(recomp_second_vblank_set_poll_blank(false));
    CHECK(recomp_second_vblank_configure(true, callback, low, high, confirmed));
    CHECK(!recomp_second_vblank_set_poll_blank(true)); /* and the owner waits */
    CHECK(recomp_second_vblank_set_owner_waits(2u));
    CHECK(recomp_second_vblank_set_poll_blank(true));
    CHECK(recomp_second_vblank_reset()); /* an epoch reset keeps it, like the budget */
    CHECK(recomp_second_vblank_set_owner_waits(0u)); /* the budget off again does not leave a poll blank behind */
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
}
static void test_model_blank(void)
{
    /* One blank's device effects without a wait: off, nothing happens, on it is one coupled helper run at the
     * 60 Hz frame floor (the display mode word has 0x400000), so two runs are one 60 Hz period apart. */
    begin(low, high);
    d3d8_vblank_effects_configure(false);
    const uint32_t count = load(DEV + 0x1DE8u);
    CHECK(!d3d8_gpu_model_blank());
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count);
    d3d8_vblank_effects_configure(true);
    const uint64_t applied = d3d8_vblank_effects_applied();
    CHECK(d3d8_gpu_model_blank());
    CHECK(d3d8_vblank_effects_applied() == applied + 1u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count + 1u);
    CHECK(d3d8_gpu_model_blank());
    CHECK_EQ_U32(load(DEV + 0x1DE8u), count + 2u);
    CHECK(load(DEV + 0x1DFCu) == 12222222u || load(DEV + 0x1DFCu) == 12222223u);
    end_epoch();
}
static void test_poll_blank(void)
{
    /* Exit test fails by one blank: the poll delivers it through the owner-wait delivery. The counter is moved by
     * the callback alone, the last exit word is not written, and the next poll finds its test holding. */
    reach_second_event(3u);
    CHECK(recomp_second_vblank_set_poll_blank(true));
    store(FRAME_LAST_EXIT, 2u);
    unsigned before = calls;
    expected_record = 6u;
    CHECK(on_owner(job_poll));
    CHECK_EQ_U32(calls, before + 1u);
    CHECK_EQ_U32(load(COUNTER), 3u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 6u);
    CHECK_EQ_U32(load(FRAME_LAST_EXIT), 2u);
    poll_snapshot(1u, 1u, 0u);
    /* Holding (counter 3, last exit 2): admitted, nothing delivered, not a poll blank. */
    expected_record = 0u;
    CHECK(on_owner(job_poll));
    CHECK_EQ_U32(calls, before + 1u);
    poll_snapshot(1u, 1u, 1u);
    /* The next exit stored 3: a second owed blank is delivered, then the budget (3) is shared with owner waits. */
    store(FRAME_LAST_EXIT, 3u);
    expected_record = 7u;
    CHECK(on_owner(job_poll));
    CHECK_EQ_U32(load(COUNTER), 4u);
    poll_snapshot(2u, 2u, 1u);
    owner_blank();
    expected_record = 8u;
    CHECK(on_owner(job_wait));
    CHECK_EQ_U32(load(COUNTER), 5u);
    poll_snapshot(3u, 2u, 1u);
    store(FRAME_LAST_EXIT, 5u);
    poll_refused("would spin for a blank nobody delivers (counter 5, last exit 5, owner blanks delivered 3 of 3)");
    end_epoch();

    /* Off (the default), the same state is the named stop and nothing was delivered. */
    reach_second_event(3u);
    store(FRAME_LAST_EXIT, 2u);
    poll_refused("would spin for a blank nobody delivers (counter 2, last exit 2");
    end_epoch();

    /* Only exactly one owed blank is delivered: a counter BEHIND the last exit (also wrapped) is not helped by one. */
    const struct { uint32_t counter, last; } not_owed[] = {{2u, 3u}, {0u, 0x80000000u}, {0x80000000u, 0u}};
    for (unsigned index = 0u; index < sizeof not_owed / sizeof not_owed[0]; index++) {
        reach_second_event(3u);
        CHECK(recomp_second_vblank_set_poll_blank(true));
        store(COUNTER, not_owed[index].counter);
        store(FRAME_LAST_EXIT, not_owed[index].last);
        poll_refused("would spin for a blank nobody delivers");
        poll_snapshot(0u, 0u, 0u);
        end_epoch();
    }

    /* Configure clears the opt-in. */
    reach_second_event(3u);
    CHECK(recomp_second_vblank_set_poll_blank(true));
    end_epoch();
    reach_second_event(3u);
    store(FRAME_LAST_EXIT, 2u);
    poll_refused("would spin for a blank nobody delivers");
    end_epoch();

    /* The refusals come before the blank's own device effects: nothing applied, nothing written. */
    reach_second_event(3u);
    CHECK(recomp_second_vblank_set_poll_blank(true));
    store(FRAME_LAST_EXIT, 2u);
    recomp_vblank_quiescence_configure(false);
    poll_refused("owner-wait callback requires the quiescence check");
    end_epoch();
    reach_second_event(3u);
    CHECK(recomp_second_vblank_set_poll_blank(true));
    store(FRAME_LAST_EXIT, 2u);
    d3d8_vblank_effects_configure(false);
    poll_refused("owner-wait callback requires the coupled vblank effects");
    end_epoch();

    /* A callback that does not advance the counter refuses by the owner's name, as for a wait. */
    reach_second_event(3u);
    CHECK(recomp_second_vblank_set_poll_blank(true));
    store(FRAME_LAST_EXIT, 2u);
    leaf_mode = LEAF_NO_COUNTER;
    CHECK(!on_owner(job_poll));
    CHECK(stopped_with("owner-wait callback did not advance the counter by one"));
    leaf_mode = LEAF_OK;
    end_epoch();
}

static void job_startup_worker_poll(void)
{
    recomp_second_vblank_poll(GETTER_CALL,worker_handle,worker_fs,scratch + 0x500u,0u);
}
/* Reproduce the captured startup wait cycle using two real guest threads. The owner
 * has stopped the producer before its second FRAME, and awaits its termination. */
static void test_interactive_startup_worker(void)
{
    uint32_t mapped;
    if (!kernel_guest_read_u32(FRAME_LAST_EXIT,&mapped)) map_fixed(FRAME_LAST_EXIT & ~0xFFFu,4096u);
    for (unsigned scenario = 0u; scenario < 8u; scenario++) {
        begin(low,high);
        CHECK(recomp_second_vblank_set_owner_waits(2u));
        CHECK(recomp_second_vblank_set_worker_blanks(2u));
        CHECK(recomp_second_vblank_set_interactive(scenario != 0u));
        recomp_second_vblank_set_worker_hold_ms(60u);
        new_worker();
        CHECK(on_owner(job_poll));
        blank(0x9000u);
        CHECK(on_server(&worker_server,job_worker_wait));
        set_gate_memory(1u,0u,0xAu,ONE_BITS,0u);
        store(0x74A9B0u,scenario == 4u ? 0u : 1u);
        store(0x7497D8u,1u);
        store(scratch + 0x500u,scenario == 3u ? 0x156CB7u : 0x156CB6u);
        if (scenario != 1u) owner_blocks_on_worker();
        if (scenario == 5u) CHECK(kernel_guest_write_u8(worker_fs + 0x24u,2u));
        if (scenario == 6u) recomp_vblank_quiescence_configure(false);
        if (scenario == 7u) leaf_mode = LEAF_NO_COUNTER;
        const unsigned before = calls;
        const uint64_t clock = kernel_clock_peek();
        const uint64_t effects = d3d8_vblank_effects_applied();
        const bool returned = on_server(&worker_server,job_startup_worker_poll);
        recomp_second_vblank_snapshot snapshot;
        recomp_second_vblank_get_snapshot(&snapshot);
        if (scenario < 2u) {
            CHECK(returned);
            CHECK_EQ_U32(calls,before);
            CHECK_EQ_U32(load(COUNTER),1u);
            CHECK_EQ_U32((uint32_t)snapshot.credits,1u);
            CHECK(!snapshot.second_attempted && !snapshot.second_via_worker);
        } else if (scenario == 2u) {
            CHECK(returned);
            CHECK_EQ_U32(calls,before + 1u);
            CHECK_EQ_U32(load(COUNTER),2u);
            CHECK_EQ_U32((uint32_t)snapshot.credits,0u);
            CHECK(snapshot.second_delivered && snapshot.second_via_worker && !snapshot.inflight);
            CHECK_EQ_U32(snapshot.worker_delivered,0u);
            CHECK(kernel_clock_peek() == clock && d3d8_vblank_effects_applied() == effects);
            /* Its final title step may complete another wait before testing stop. */
            blank(0xA000u);expected_record = 6u;
            CHECK(on_server(&worker_server,job_worker_wait));
            CHECK_EQ_U32(load(COUNTER),3u);
            recomp_second_vblank_get_snapshot(&snapshot);
            CHECK_EQ_U32(snapshot.worker_delivered,1u);
        } else {
            CHECK(!returned);
            const char *reason = scenario <= 4u ? "interactive startup worker caller/counter/termination scope refused" :
                scenario == 5u ? "worker-wait caller/PCR/device/counter scope refused" :
                scenario == 6u ? "worker-wait callback requires the quiescence check" :
                "worker-wait callback did not advance the counter by one";
            CHECK(worker_stopped_with(reason));
            CHECK_EQ_U32(load(COUNTER),1u);
            CHECK_EQ_U32(calls,before + (scenario == 7u ? 1u : 0u));
        }
        leaf_mode = LEAF_OK;
        CHECK(kernel_guest_write_u8(worker_fs + 0x24u,0u));
        if (scenario != 1u) release_blocked_owner(); else end_worker();
        if (scenario == 2u) {
            store(FRAME_LAST_EXIT,1u);store(esp,0x3D66Du);
            CHECK(on_owner(job_poll));
            CHECK_EQ_U32(load(COUNTER),3u);
        }
        end_epoch();
    }
}

static void test_interactive_continuation(void)
{
    reach_second_event(1u);
    CHECK(recomp_second_vblank_set_interactive(true));
    for (uint32_t index = 1u; index <= 4u; index++) {
        owner_blank();
        expected_record = 5u + index;
        CHECK(on_owner(job_wait));
        CHECK_EQ_U32(load(COUNTER), 2u + index);
    }
    recomp_second_vblank_snapshot snapshot;
    recomp_second_vblank_get_snapshot(&snapshot);
    CHECK_EQ_U32(snapshot.owner_budget, 1u);
    CHECK_EQ_U32(snapshot.owner_delivered, 4u);
    CHECK(recomp_second_vblank_set_interactive(false));
    refused_owner_wait("budget exhausted");
    end_epoch();
    reach_second_event(1u);
    CHECK(recomp_second_vblank_set_interactive(true));
    CHECK(recomp_second_vblank_set_poll_blank(true));
    for (uint32_t index = 1u; index <= 4u; index++) {
        store(FRAME_LAST_EXIT, load(COUNTER));
        expected_record = 5u + index;
        CHECK(on_owner(job_poll));
        CHECK_EQ_U32(load(COUNTER), 2u + index);
    }
    end_epoch();
    reach_second_event(1u); /* configure clears interactive */
    owner_blank(); expected_record = 6u;
    CHECK(on_owner(job_wait));
    refused_owner_wait("budget exhausted");
    end_epoch();
}

int main(int argc, char **argv)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_set_fatal(host_fatal);
    esp = SCRATCH_DATA + 128u;
    map_fixed(fs, 4096u);
    map_fixed(low, 4096u);
    map_fixed(0x563000u, 4096u);
    /* T592: the loading bar gate's words, mapped unless the environment already covers them. */
    const uint32_t gate_pages[] = {0x4E7000u, 0x74A000u, 0x749000u, 0x475000u};
    for (unsigned index = 0u; index < sizeof gate_pages / sizeof gate_pages[0]; index++) {
        uint32_t probe;
        if (!kernel_guest_read_u32(gate_pages[index] + 0x10u, &probe)) map_fixed(gate_pages[index], 4096u);
    }
    scratch = 0x600000u;
    map_fixed(scratch, 0x4000u);
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(&ops));
    owner_handle = create_thread(1u);
    CHECK(owner_handle != 0u);
    CHECK(wait_for_state(owner_handle, false));

#ifdef TSFP_STARTUP_WORKER_ONLY
    (void)argc; (void)argv;
    const bool startup_only = true;
#else
    const bool startup_only = argc == 2 && strcmp(argv[1],"--startup-worker") == 0;
#endif
    if (startup_only) {
        test_interactive_startup_worker();
    } else {
    test_budget_api();
    test_deliveries();
    test_flip_deliveries();
    test_flip_refusals();
    test_default_off_and_ordering();
    test_later_frame_waits();
    test_interactive_continuation();
    test_poll_blank_api();
    test_model_blank();
    test_poll_blank();
    test_preconditions();
    test_quiescence();
    test_probes_and_leaf();
    test_worker_budget_api();
    test_worker_deliveries();
    test_interactive_worker_continuation();
    test_interactive_worker_shutdown();
    test_worker_hold();
    test_worker_final_hold();
    test_worker_poll_hold();
    test_worker_owner_tracking();
    test_running_flag_writers();
    test_worker_identity();
    test_worker_with_held_owner_wait();
    test_later_thread_wait();
    test_later_thread_wait_quiescent();
    }

    atomic_store(&owner_server.release, true);
    CHECK(wait_for_state(owner_handle, true));
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(kernel_thread_set_host_ops(NULL));
    d3d8_vblank_effects_configure(false);
    recomp_vblank_quiescence_configure(false);
    environment_end();
    printf("second vblank owner waits: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
