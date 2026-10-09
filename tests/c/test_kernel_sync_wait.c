/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T8g: ordinals 233 NtWaitForSingleObject and 234 NtWaitForSingleObjectEx on EVENT and MUTANT
 * handles, and the real 221 NtReleaseMutant they enable.
 *
 * Every status below is a LITERAL (0, 0x102, 0xC0000046, ...) so an expectation cannot move
 * with a macro the code under test also uses. What the guest's own callers pass is in the
 * comments: the lock class Lock 0x003ABD40 waits on a mutant with a NULL (infinite) timeout, the
 * title's polling site 0x003BCD80 passes a ZERO timeout, and every site has WaitMode 1 and
 * Alertable 0 (the wrapper 0x00380029 re-pushes `1`, then the caller's alertable flag).
 *
 * Mutation notes sit next to each test as "MUTATION:".
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_event_handle.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "nt_status.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a, b) do { checks++; const uint32_t got_ = (uint32_t)(a); \
    const uint32_t want_ = (uint32_t)(b); if (got_ != want_) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s == %#x, wanted %#x\n", __FILE__, __LINE__, #a, \
            (unsigned)got_, (unsigned)want_); } } while (0)

#define WAIT_OK 0x00000000u
#define WAIT_TIMEOUT_STATUS 0x00000102u
#define NOT_OWNED 0xC0000046u
#define LIMIT_EXCEEDED 0xC0000191u
#define OFF_TIMEOUT 0x700u

static uint32_t scratch;
static int quiet(const char *format, ...) { (void)format; return 0; }

static jmp_buf wait_jump;
static uint32_t refused_handle;
static kernel_thread_wait_refusal refused_reason;
static unsigned refusals;
static void refuse_wait(uint32_t handle, kernel_thread_wait_refusal reason)
{
    refused_handle = handle;
    refused_reason = reason;
    refusals++;
    longjmp(wait_jump, 1);
}
static const kernel_thread_host_ops refusing_ops = {.wait_refused = refuse_wait};

static void setup(void)
{
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(&refusing_ops));
    guest_mem_reset();
    kernel_hle_init();
    kernel_hle_set_log(quiet);
    kernel_sync_reset();
    (void)kernel_object_register();
    (void)kernel_thread_register();
    (void)kernel_sync_register();
    (void)kernel_event_handle_register();
    guest_region_request request = {.bytes = 0x4000u, .state = MEM_COMMIT,
                                   .protect = PAGE_READWRITE};
    nt_status status = 0u;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
    refusals = 0u;
    refused_handle = 0u;
    refused_reason = (kernel_thread_wait_refusal)99;
}

static uint32_t call(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, count));
    return kernel_hle_call(ordinal, &frame);
}

/* Writes an 8-byte LARGE_INTEGER timeout and returns its guest address. */
static uint32_t timeout_at(int64_t value)
{
    CHECK(kernel_guest_write_bytes(scratch + OFF_TIMEOUT, &value, sizeof(value)));
    return scratch + OFF_TIMEOUT;
}

/* Runs a wait and reports whether the host refused it (the real host aborts the run there).
 * `*status` is the guest-visible result when it was not refused. */
static bool wait_ex_refused(uint32_t handle, uint32_t mode, uint32_t alertable,
                            uint32_t timeout, uint32_t *status)
{
    const unsigned before = refusals;
    if (setjmp(wait_jump) == 0) {
        const uint32_t args[4] = {handle, mode, alertable, timeout};
        *status = call(234u, args, 4u);
    }
    return refusals != before;
}
static uint32_t wait_ex(uint32_t handle, uint32_t timeout)
{
    uint32_t status = 0xDEADBEEFu;
    CHECK(!wait_ex_refused(handle, 1u, 0u, timeout, &status));
    return status;
}
static bool wait_refused(uint32_t handle, uint32_t timeout)
{
    uint32_t status = 0xDEADBEEFu;
    return wait_ex_refused(handle, 1u, 0u, timeout, &status);
}
static uint32_t wait_plain(uint32_t handle, uint32_t timeout)
{
    uint32_t status = 0xDEADBEEFu;
    const unsigned before = refusals;
    if (setjmp(wait_jump) == 0) {
        const uint32_t args[3] = {handle, 0u, timeout};
        status = call(233u, args, 3u);
    }
    CHECK(refusals == before);
    return status;
}
static bool wait_plain_refused(uint32_t handle, uint32_t timeout)
{
    const unsigned before = refusals;
    if (setjmp(wait_jump) == 0) {
        const uint32_t args[3] = {handle, 0u, timeout};
        (void)call(233u, args, 3u);
    }
    return refusals != before;
}

static uint32_t new_event(void)
{
    const uint32_t args[4] = {scratch + 0x500u, 0u, 1u, 0u};
    EQ(call(189u, args, 4u), STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(scratch + 0x500u, &handle));
    return handle;
}
/* NtCreateEvent Type 0 (NotificationEvent, manual reset), initially clear: the XNET 0x431D5F shape. */
static uint32_t new_notification_event(void)
{
    const uint32_t args[4] = {scratch + 0x508u, 0u, 0u, 0u};
    EQ(call(189u, args, 4u), STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(scratch + 0x508u, &handle));
    return handle;
}
static uint32_t new_mutant(void)
{
    const uint32_t args[3] = {scratch + 0x504u, 0u, 0u};
    EQ(call(192u, args, 3u), STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(scratch + 0x504u, &handle));
    return handle;
}
static uint32_t set_event(uint32_t handle)
{
    const uint32_t args[2] = {handle, 0u};
    return call(225u, args, 2u);
}
static uint32_t release_mutant(uint32_t handle)
{
    const uint32_t args[2] = {handle, 0u};
    return call(221u, args, 2u);
}
static bool event_state(uint32_t handle)
{
    bool state = false;
    CHECK(kernel_object_event_signaled(handle, &state));
    return state;
}
static uint32_t owner_of(uint32_t handle)
{
    uint32_t owner = 0xAAAAAAAAu, count = 0xBBBBBBBBu;
    CHECK(kernel_object_mutant_state(handle, &owner, &count));
    return owner;
}
static uint32_t count_of(uint32_t handle)
{
    uint32_t owner = 0xAAAAAAAAu, count = 0xBBBBBBBBu;
    CHECK(kernel_object_mutant_state(handle, &owner, &count));
    return count;
}

/* ------------------------------------------------------------------------------------------ */
/* EVENT                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* An unsignaled event polled with a ZERO timeout (the shape of the title's poll site
 * 0x003BCD80) is STATUS_TIMEOUT, nothing blocks, nothing is refused, and the event stays clear.
 * MUTATION: answer SUCCESS, answer INVALID_HANDLE/anything else, treat a zero timeout as
 * infinite (refuses), or set the event while polling. */
static void test_an_unsignaled_event_polls_to_timeout(void)
{
    setup();
    const uint32_t handle = new_event();
    EQ(wait_ex(handle, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    EQ(wait_plain(handle, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    CHECK(!event_state(handle));
    EQ(refusals, 0u);
    EQ(kernel_thread_active_wait_count(), 0u);
}

/* A signaled event satisfies a wait at once and, being the only measured type 1 (NT
 * SynchronizationEvent, auto-reset), is consumed by it: the next poll times out and the state is
 * clear. Both 234 and 233 reach it. MUTATION: do not clear the event (manual-reset behaviour),
 * clear it on a failed poll, clear it before the signal test, return TIMEOUT for a signaled
 * event, or drop the 233 forwarding. */
static void test_a_signaled_event_wait_succeeds_and_auto_resets(void)
{
    setup();
    const uint32_t handle = new_event();
    EQ(set_event(handle), STATUS_SUCCESS);
    CHECK(event_state(handle));
    EQ(wait_ex(handle, 0u), WAIT_OK);
    CHECK(!event_state(handle));
    EQ(wait_ex(handle, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    EQ(set_event(handle), STATUS_SUCCESS);
    EQ(wait_plain(handle, 0u), WAIT_OK);
    CHECK(!event_state(handle));
    EQ(wait_plain(handle, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    EQ(refusals, 0u);
}

/* The signal is judged BEFORE the timeout shape: a signaled event returns immediately for a NULL
 * (infinite), a zero and a finite relative timeout alike, because nothing would block.
 * MUTATION: refuse any non-zero timeout up front, or refuse the NULL timeout up front. */
static void test_a_signaled_event_returns_at_once_for_every_timeout_shape(void)
{
    setup();
    const uint32_t handle = new_event();
    const int64_t finite = -10000000; /* one second relative, as the 0x3E8 ms waiters use */
    EQ(set_event(handle), STATUS_SUCCESS);
    EQ(wait_ex(handle, 0u), WAIT_OK);
    EQ(set_event(handle), STATUS_SUCCESS);
    EQ(wait_ex(handle, timeout_at(0)), WAIT_OK);
    EQ(set_event(handle), STATUS_SUCCESS);
    EQ(wait_ex(handle, timeout_at(finite)), WAIT_OK);
    CHECK(!event_state(handle));
    EQ(refusals, 0u);
}

/* Auto-reset consumption is per event: waiting on one leaves another signaled event alone.
 * MUTATION: clear every event, or clear the first event found. */
static void test_consumption_is_per_event(void)
{
    setup();
    const uint32_t first = new_event();
    const uint32_t second = new_event();
    EQ(set_event(first), STATUS_SUCCESS);
    EQ(set_event(second), STATUS_SUCCESS);
    EQ(wait_ex(second, 0u), WAIT_OK);
    CHECK(event_state(first));
    CHECK(!event_state(second));
    EQ(wait_ex(first, 0u), WAIT_OK);
    CHECK(!event_state(first));
}

/* A NotificationEvent (type 0, manual-reset) keeps its signal. It is created through ordinal 189
 * Type 0 (T270, the XNET 0x431D5F shape), and the wait side honours the recorded type.
 * MUTATION: clear a type 0 event on wait. */
static void test_a_notification_event_stays_signaled(void)
{
    setup();
    const uint32_t handle = new_notification_event();
    CHECK(handle != 0u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.event_type, 0u);
    EQ(wait_ex(handle, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    EQ(kernel_object_event_set(handle, NULL), STATUS_SUCCESS);
    EQ(wait_ex(handle, 0u), WAIT_OK);
    CHECK(event_state(handle));
    EQ(wait_ex(handle, timeout_at(0)), WAIT_OK);
    CHECK(event_state(handle));
}

/* A wait that would genuinely block is REFUSED, loudly, through the host hook (which stops the
 * run): NULL (infinite) and every finite timeout on an unsignaled event, through 234 and 233.
 * Nothing is consumed or recorded, the handle is named, no wait stays pinned, and the thread
 * lock was released before the hook ran. MUTATION: return TIMEOUT or SUCCESS for a would-block
 * wait, fall through to a real cond wait, name the wrong handle or reason, leave a pin. */
static void test_a_wait_that_would_block_is_refused(void)
{
    setup();
    const uint32_t handle = new_event();
    CHECK(wait_refused(handle, 0u));
    EQ(refused_handle, handle);
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    EQ(kernel_thread_active_wait_count(), 0u);
    CHECK(wait_refused(handle, timeout_at(-10000)));
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK(wait_plain_refused(handle, 0u));
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK(wait_plain_refused(handle, timeout_at(-10000)));
    EQ(refusals, 4u);
    CHECK(!event_state(handle));
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.references, 1u);
    CHECK(kernel_thread_set_host_ops(&refusing_ops)); /* the lock is free again */
    /* A positive value is an ABSOLUTE time: unmeasured, refused when it would block. */
    CHECK(wait_refused(handle, timeout_at(1)));
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
}

/* The scope gate runs first and is unchanged: alertable waits and WaitMode != 1 are refused as
 * UNSUPPORTED_SCOPE, and a SIGNALED event is NOT consumed by the refused call.
 * MUTATION: look at the event before the scope gate, or drop the alertable / mode terms. */
static void test_scope_is_judged_before_the_object(void)
{
    setup();
    const uint32_t handle = new_event();
    EQ(set_event(handle), STATUS_SUCCESS);
    uint32_t status = 0u;
    CHECK(wait_ex_refused(handle, 1u, 1u, 0u, &status));
    EQ(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    CHECK(wait_ex_refused(handle, 0u, 0u, 0u, &status));
    EQ(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    CHECK(event_state(handle));
    /* The in-scope call then consumes it, so the gate (not the object) was what refused. */
    EQ(wait_ex(handle, 0u), WAIT_OK);
}

/* A dead handle is STATUS_INVALID_HANDLE whatever the timeout, an unreadable timeout pointer is
 * STATUS_ACCESS_VIOLATION before any object is touched, and a live handle of a kind this wait
 * does not model (FILE) is still refused as before. MUTATION: judge the kind before the
 * handle, consume on an unreadable timeout, accept a FILE. */
static void test_bad_handles_pointers_and_other_kinds(void)
{
    setup();
    EQ(wait_ex(0x12345u, 0u), STATUS_INVALID_HANDLE);
    /* A finite timeout on a non-event/mutant handle is judged before the handle, as it was before
     * T8g (the THREAD policy: only NULL or zero), so a dead handle with one is a scope refusal. */
    CHECK(wait_refused(0x12345u, timeout_at(-10000)));
    EQ(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    EQ(wait_ex(0u, 0u), STATUS_INVALID_HANDLE);
    const uint32_t handle = new_event();
    EQ(set_event(handle), STATUS_SUCCESS);
    EQ(wait_ex(handle, 0x1000u), STATUS_ACCESS_VIOLATION);
    CHECK(event_state(handle));
    const uint32_t file = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    CHECK(file != 0u);
    CHECK(wait_refused(file, 0u));
    EQ(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    CHECK(wait_refused(file, timeout_at(0)));
    EQ(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
}

/* ------------------------------------------------------------------------------------------ */
/* MUTANT                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* The lock class: Lock waits with a NULL timeout, Unlock calls ReleaseMutex (221, NULL
 * PreviousCount). An unowned mutant is acquired at once by the calling thread (the boot thread
 * here), a re-acquire by the owner recurses, and each 221 unwinds one level, the last one making
 * it unowned; one more is STATUS_MUTANT_NOT_OWNED. MUTATION: no owner recorded, count not
 * incremented or not decremented, ownership kept at count 0, release of an unowned mutant
 * succeeding, the owner left set at count 0. */
static void test_the_lock_class_sequence(void)
{
    setup();
    const uint32_t mutant = new_mutant();
    EQ(owner_of(mutant), 0u);
    EQ(count_of(mutant), 0u);
    EQ(wait_ex(mutant, 0u), WAIT_OK);
    EQ(owner_of(mutant), kernel_thread_current_identity());
    EQ(count_of(mutant), 1u);
    EQ(wait_plain(mutant, 0u), WAIT_OK);
    EQ(count_of(mutant), 2u);
    EQ(wait_ex(mutant, timeout_at(0)), WAIT_OK);
    EQ(count_of(mutant), 3u);
    EQ(release_mutant(mutant), STATUS_SUCCESS);
    EQ(count_of(mutant), 2u);
    EQ(release_mutant(mutant), STATUS_SUCCESS);
    EQ(release_mutant(mutant), STATUS_SUCCESS);
    EQ(count_of(mutant), 0u);
    EQ(owner_of(mutant), 0u); /* unowned means no owner, not a stale one */
    const unsigned unowned_before = kernel_object_mutant_unowned_release_count();
    EQ(release_mutant(mutant), NOT_OWNED);
    EQ(kernel_object_mutant_unowned_release_count(), unowned_before + 1u);
    EQ(count_of(mutant), 0u);
    /* Fully released, it can be taken again. */
    EQ(wait_ex(mutant, 0u), WAIT_OK);
    EQ(count_of(mutant), 1u);
    EQ(refusals, 0u);
}

/* A mutant held by ANOTHER thread cannot be acquired: a poll is STATUS_TIMEOUT, a wait that
 * would block (NULL or finite timeout) is refused, and a release by a non-owner is
 * STATUS_MUTANT_NOT_OWNED with the owner and count untouched. The other owner is made through
 * the object API so the test needs no second host thread. MUTATION: ignore the owner identity on
 * acquire (recurse for anyone), on release (let anyone release), or release/decrement on the
 * NOT_OWNED path. */
static void test_a_mutant_held_by_another_thread(void)
{
    setup();
    const uint32_t mutant = new_mutant();
    const uint32_t other = 0x0BADBEEFu;
    CHECK(other != kernel_thread_current_identity());
    bool acquired = false;
    EQ(kernel_object_mutant_try_acquire(mutant, other, &acquired), STATUS_SUCCESS);
    CHECK(acquired);
    EQ(wait_ex(mutant, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    CHECK(wait_refused(mutant, 0u));
    EQ(refused_handle, mutant);
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK(wait_refused(mutant, timeout_at(-10000)));
    CHECK(wait_plain_refused(mutant, 0u));
    EQ(owner_of(mutant), other);
    EQ(count_of(mutant), 1u);
    EQ(release_mutant(mutant), NOT_OWNED);
    EQ(owner_of(mutant), other);
    EQ(count_of(mutant), 1u);
    /* The real owner releases it, then this thread can take it. */
    uint32_t previous = 0u;
    EQ(kernel_object_mutant_release(mutant, other, &previous), STATUS_SUCCESS);
    EQ(previous, 1u);
    EQ(wait_ex(mutant, 0u), WAIT_OK);
    EQ(owner_of(mutant), kernel_thread_current_identity());
    EQ(kernel_thread_active_wait_count(), 0u);
}

/* The object-level release reports the recursion count before the release (INFERRED, the value a
 * PreviousCount output would carry; no measured site asks for it) and leaves *previous alone on
 * a failure. MUTATION: report the count after the release, write on failure. */
static void test_mutant_release_reports_the_previous_count(void)
{
    setup();
    const uint32_t mutant = new_mutant();
    const uint32_t owner = 0x1111u;
    bool acquired = false;
    EQ(kernel_object_mutant_try_acquire(mutant, owner, &acquired), STATUS_SUCCESS);
    EQ(kernel_object_mutant_try_acquire(mutant, owner, &acquired), STATUS_SUCCESS);
    CHECK(acquired);
    uint32_t previous = 0x5A5Au;
    EQ(kernel_object_mutant_release(mutant, owner, &previous), STATUS_SUCCESS);
    EQ(previous, 2u);
    EQ(owner_of(mutant), owner);
    EQ(kernel_object_mutant_release(mutant, owner, &previous), STATUS_SUCCESS);
    EQ(previous, 1u);
    EQ(owner_of(mutant), 0u); /* unowned means no owner, not a stale one (the boot owner IS 0) */
    previous = 0x5A5Au;
    EQ(kernel_object_mutant_release(mutant, owner, &previous), NOT_OWNED);
    EQ(previous, 0x5A5Au);
    EQ(kernel_object_mutant_release(mutant, owner, NULL), NOT_OWNED);
    EQ(kernel_object_mutant_try_acquire(mutant, owner, &acquired), STATUS_SUCCESS);
    EQ(kernel_object_mutant_release(mutant, owner, NULL), STATUS_SUCCESS);
}

/* The recursion count is bounded (INFERRED host bound, NT's own is 2^31): the acquire past it is
 * STATUS_MUTANT_LIMIT_EXCEEDED (0xC0000191), the count is not moved, and a release still
 * unwinds. MUTATION: no bound (count wraps), off-by-one on the bound, wrong status. */
static void test_the_recursion_count_is_bounded(void)
{
    setup();
    const uint32_t mutant = new_mutant();
    for (uint32_t i = 0u; i < KERNEL_OBJECT_MUTANT_RECURSION_MAX; i++) {
        bool acquired = false;
        EQ(kernel_object_mutant_try_acquire(mutant, 0x2222u, &acquired), STATUS_SUCCESS);
        if (!acquired) { CHECK(false); return; }
    }
    EQ(count_of(mutant), KERNEL_OBJECT_MUTANT_RECURSION_MAX);
    EQ(KERNEL_OBJECT_MUTANT_RECURSION_MAX, 0x10000u);
    bool acquired = true;
    EQ(kernel_object_mutant_try_acquire(mutant, 0x2222u, &acquired), LIMIT_EXCEEDED);
    CHECK(!acquired);
    EQ(count_of(mutant), KERNEL_OBJECT_MUTANT_RECURSION_MAX);
    EQ(kernel_object_mutant_release(mutant, 0x2222u, NULL), STATUS_SUCCESS);
    EQ(count_of(mutant), KERNEL_OBJECT_MUTANT_RECURSION_MAX - 1u);
    /* Through the wait ordinals the limit is the guest-visible (negative) result, not a refusal,
     * and the count stays put. This thread (boot identity) owns it at the bound. */
    const uint32_t via_wait = new_mutant();
    for (uint32_t i = 0u; i < KERNEL_OBJECT_MUTANT_RECURSION_MAX; i++) {
        bool taken = false;
        EQ(kernel_object_mutant_try_acquire(via_wait, kernel_thread_current_identity(), &taken),
           STATUS_SUCCESS);
    }
    EQ(wait_ex(via_wait, 0u), LIMIT_EXCEEDED);
    EQ(wait_plain(via_wait, timeout_at(0)), LIMIT_EXCEEDED);
    EQ(count_of(via_wait), KERNEL_OBJECT_MUTANT_RECURSION_MAX);
    EQ(refusals, 0u);
}

/* Two mutants hold independent ownership, and an unowned release of the second leaves the first
 * alone. MUTATION: one global owner/count. */
static void test_mutants_are_independent(void)
{
    setup();
    const uint32_t first = new_mutant();
    const uint32_t second = new_mutant();
    EQ(wait_ex(first, 0u), WAIT_OK);
    EQ(count_of(first), 1u);
    EQ(count_of(second), 0u);
    EQ(release_mutant(second), NOT_OWNED);
    EQ(count_of(first), 1u);
    EQ(wait_ex(second, 0u), WAIT_OK);
    EQ(wait_ex(second, 0u), WAIT_OK);
    EQ(count_of(first), 1u);
    EQ(count_of(second), 2u);
}

/* The object API rejects wrong kinds and dead handles without touching state. MUTATION: skip a
 * kind check, return SUCCESS for a dead handle. */
static void test_object_api_kind_and_handle_checks(void)
{
    setup();
    const uint32_t event = new_event();
    const uint32_t mutant = new_mutant();
    bool acquired = true;
    EQ(kernel_object_event_try_wait(mutant, &acquired), STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(!acquired);
    acquired = true;
    EQ(kernel_object_event_try_wait(0x12345u, &acquired), STATUS_INVALID_HANDLE);
    CHECK(!acquired);
    acquired = true;
    EQ(kernel_object_mutant_try_acquire(event, 1u, &acquired), STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(!acquired);
    acquired = true;
    EQ(kernel_object_mutant_try_acquire(0x12345u, 1u, &acquired), STATUS_INVALID_HANDLE);
    CHECK(!acquired);
    EQ(kernel_object_mutant_release(event, 1u, NULL), STATUS_OBJECT_TYPE_MISMATCH);
    EQ(kernel_object_mutant_release(0x12345u, 1u, NULL), STATUS_INVALID_HANDLE);
    uint32_t owner = 0u, count = 0u;
    CHECK(!kernel_object_mutant_state(event, &owner, &count));
    CHECK(!kernel_object_mutant_state(0x12345u, &owner, &count));
    EQ(release_mutant(event), STATUS_OBJECT_TYPE_MISMATCH);
    EQ(count_of(mutant), 0u);
    CHECK(!event_state(event));
}

/* Ownership dies with the handle: a recycled slot starts unowned and clear. MUTATION: do not
 * zero the mutant fields when a slot is reissued. */
static void test_ownership_dies_with_the_handle(void)
{
    setup();
    const uint32_t mutant = new_mutant();
    EQ(wait_ex(mutant, 0u), WAIT_OK);
    EQ(wait_ex(mutant, 0u), WAIT_OK);
    const uint32_t close_args[1] = {mutant};
    EQ(call(187u, close_args, 1u), STATUS_SUCCESS);
    EQ(wait_ex(mutant, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(mutant), STATUS_INVALID_HANDLE);
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        const uint32_t next = new_mutant();
        CHECK(next != mutant);
        EQ(owner_of(next), 0u);
        EQ(count_of(next), 0u);
        EQ(wait_ex(next, 0u), WAIT_OK);
        const uint32_t close_next[1] = {next};
        EQ(call(187u, close_next, 1u), STATUS_SUCCESS);
        const uint32_t ev = new_event();
        CHECK(!event_state(ev));
        EQ(set_event(ev), STATUS_SUCCESS);
        const uint32_t close_ev[1] = {ev};
        EQ(call(187u, close_ev, 1u), STATUS_SUCCESS);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* A real guest thread owns by its own handle                                                  */
/* ------------------------------------------------------------------------------------------ */

static uint32_t thread_mutant;
static uint32_t thread_status[3];
static uint32_t thread_identity;
static uint32_t thread_release_status;
static bool thread_has_code(uint32_t guest_va) { return guest_va >= 0x10000u; }
static void thread_enter(const kernel_thread_launch *launch)
{
    thread_identity = kernel_thread_current_identity();
    const uint32_t args[4] = {thread_mutant, 1u, 0u, 0u};
    for (unsigned i = 0u; i < 2u; i++) {
        kernel_call_frame frame = {0};
        if (!kernel_frame_build(&frame, launch->stack_low, 20u, args, 4u)) { return; }
        thread_status[i] = kernel_hle_call(234u, &frame);
    }
    const uint32_t release[2] = {thread_mutant, 0u};
    kernel_call_frame frame = {0};
    if (!kernel_frame_build(&frame, launch->stack_low, 12u, release, 2u)) { return; }
    thread_release_status = kernel_hle_call(221u, &frame);
}
static const kernel_thread_host_ops thread_ops = {
    .has_code = thread_has_code, .enter = thread_enter,
};

/* A guest thread created through PsCreateSystemThreadEx acquires under ITS handle, not the boot
 * identity: it acquires twice (recursion), releases once, and the mutant stays owned by it with
 * count 1 after it has exited. The boot thread then cannot take it (poll TIMEOUT, release
 * NOT_OWNED), which is the cross-thread rule with a real second thread. NT would mark the mutant
 * abandoned at thread exit: that is NOT modelled, so a wait that would block on it is refused.
 * MUTATION: identity always the boot value, identity read from the wrong thread-local. */
static void test_a_real_guest_thread_owns_by_its_handle(void)
{
    setup();
    CHECK(kernel_thread_set_host_ops(&thread_ops));
    thread_mutant = new_mutant();
    thread_identity = 0u;
    memset(thread_status, 0xEE, sizeof(thread_status));
    thread_release_status = 0xEEEEEEEEu;
    const uint32_t out = scratch + 0x800u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, 0x123456u, 0u, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x1000u, 44u, args, 10u));
    EQ(kernel_hle_call(255u, &frame), STATUS_SUCCESS);
    uint32_t thread = 0u;
    CHECK(kernel_guest_read_u32(out, &thread));
    EQ(kernel_thread_join_all(5000u), 0u);
    CHECK(thread != 0u);
    EQ(thread_status[0], WAIT_OK);
    EQ(thread_status[1], WAIT_OK);
    EQ(thread_release_status, STATUS_SUCCESS);
    EQ(thread_identity, thread);
    CHECK(thread_identity != kernel_thread_current_identity());
    EQ(owner_of(thread_mutant), thread);
    EQ(count_of(thread_mutant), 1u);
    CHECK(kernel_thread_set_host_ops(&refusing_ops));
    EQ(wait_ex(thread_mutant, timeout_at(0)), WAIT_TIMEOUT_STATUS);
    EQ(release_mutant(thread_mutant), NOT_OWNED);
    CHECK(wait_refused(thread_mutant, 0u));
    EQ(kernel_thread_current_identity(), 0u);
}

int main(void)
{
    test_an_unsignaled_event_polls_to_timeout();
    test_a_signaled_event_wait_succeeds_and_auto_resets();
    test_a_signaled_event_returns_at_once_for_every_timeout_shape();
    test_consumption_is_per_event();
    test_a_notification_event_stays_signaled();
    test_a_wait_that_would_block_is_refused();
    test_scope_is_judged_before_the_object();
    test_bad_handles_pointers_and_other_kinds();
    test_the_lock_class_sequence();
    test_a_mutant_held_by_another_thread();
    test_mutant_release_reports_the_previous_count();
    test_the_recursion_count_is_bounded();
    test_mutants_are_independent();
    test_object_api_kind_and_handle_checks();
    test_ownership_dies_with_the_handle();
    test_a_real_guest_thread_owns_by_its_handle();
    printf("kernel_sync_wait: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
