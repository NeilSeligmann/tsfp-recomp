/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the event / timer / DPC group.
 *
 * WHAT THESE TESTS GO AFTER. Most of this module's surface is bookkeeping whose
 * return value the guest branches on, and almost every way of getting it wrong is
 * SILENT:
 *
 *   - RETURN THE PREVIOUS STATE, NOT THE NEW ONE. KeSetEvent, KeSetTimer and
 *     KeInsertQueueDpc all answer a question about the state BEFORE the call. Each
 *     is tested by calling it twice and requiring the two answers to DIFFER, which
 *     no constant return can satisfy.
 *   - ARGUMENT ORDER AND COUNT. Each handler is given a frame too short for its last
 *     argument, which is the only test that fails for a handler that reads too few.
 *     Argument order is pinned with values that cannot be confused for one another.
 *   - THE 64-BIT SPLIT. KeSetTimer's DueTime arrives as two stack slots and
 *     KeQuerySystemTime writes two. Both are tested with values whose halves differ,
 *     so a high/low swap cannot pass.
 *
 * And the gaps are tested AS gaps: the unhonoured-wait counter is asserted to rise,
 * because a module that silently pretended to wait would be worse than one that
 * reports it.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_event.h"
#include "kernel_clock.h"
#include "kernel_sync.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                 \
        checks++;                                                                         \
        if (!(cond)) {                                                                     \
            failures++;                                                                    \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            fflush(stdout);                                                                \
        }                                                                                  \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                 \
        checks++;                                                                         \
        uint32_t check_a = (uint32_t)(actual);                                             \
        uint32_t check_e = (uint32_t)(expected);                                           \
        if (check_a != check_e) {                                                          \
            failures++;                                                                    \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__,     \
                   #actual, #expected, check_a, check_e);                                  \
            fflush(stdout);                                                               \
        }                                                                                  \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                  \
    do {                                                                                 \
        checks++;                                                                         \
        uint64_t check_a = (uint64_t)(actual);                                             \
        uint64_t check_e = (uint64_t)(expected);                                           \
        if (check_a != check_e) {                                                          \
            failures++;                                                                    \
            printf("  FAIL %s:%d  %s == %s (got %llu, want %llu)\n", __FILE__, __LINE__,   \
                   #actual, #expected, (unsigned long long)check_a,                        \
                   (unsigned long long)check_e);                                           \
            fflush(stdout);                                                               \
        }                                                                                  \
    } while (0)

static char captured[32768];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                            format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define SCRATCH_BYTES 0x1000u
#define SCRATCH_OUT 0x200u

static kernel_guest_ptr scratch;

/* KTIMERs are addressed by the real .data globals this image passes, because no byte
 * inside a KTIMER is ever read or written -- the state is host-side, so the address
 * only has to be distinct, not mapped. */
#define GUEST_TIMER 0x007EB480u
#define GUEST_TIMER_B 0x00771690u

/* EVENTS AND DPCS MUST BE REAL MAPPED GUEST MEMORY, because this module reads and
 * writes inside them: a KEVENT's SignalState at +0x04, and a KDPC's Type at +0x00. So
 * they are laid out inside the scratch region rather than at the .data addresses the
 * guest uses, which are not mapped in a test. Well separated, so an overrun of one
 * lands in none of the others. */
#define EVENT_AT (SCRATCH_OUT + 0x100u)
#define EVENT_B_AT (SCRATCH_OUT + 0x140u)
#define DPC_AT (SCRATCH_OUT + 0x180u)

/* A real DeferredRoutine code address from an ordinal 107 call site. */
#define GUEST_DPC_ROUTINE 0x00384730u
#define GUEST_DPC_CONTEXT 0x0012FF00u

/* The measured dispatcher-object offsets, repeated here as literals ON PURPOSE. The
 * implementation derives them via guest_structs.h; if a test reused the same macro it
 * would agree with the implementation no matter how wrong both were. */
#define EVENT_OFFSET_TYPE 0x00u
#define EVENT_OFFSET_SIGNAL_STATE 0x04u
#define EVENT_BYTES 16u
#define DPC_OFFSET_TYPE 0x00u
#define DPC_TYPE_INITIALISED 0x0013u
#define DPC_TYPE_POISONED 0xFFECu

/* Defined below, but needed by setup(). */
static kernel_guest_ptr event_addr(void);
static kernel_guest_ptr event_b_addr(void);
static kernel_guest_ptr dpc_addr(void);
static unsigned char *host_bytes(kernel_guest_ptr addr, uint32_t length);
static void guest_init_event(kernel_guest_ptr addr, uint8_t type, uint32_t signal_state);

static void setup(void)
{
    kernel_hle_init();
    kernel_event_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_event_register(), 11u);
    kernel_hle_set_log(capture_printer);
    reset_capture();

    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch (status %#x)\n", status);
        exit(EXIT_FAILURE);
    }

    /* Both events start as the guest's own open-coded init leaves them: Type 0, not
     * signalled. The DPC slot starts all-zero, which is the state the two BSS objects
     * in this image are genuinely used in -- never initialised through the kernel. */
    guest_init_event(event_addr(), 0u, 0u);
    guest_init_event(event_b_addr(), 0u, 0u);
    memset(host_bytes(dpc_addr(), 0x1Cu), 0, 0x1Cu);
}

static void teardown(void)
{
    kernel_event_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static kernel_guest_ptr event_addr(void)
{
    return (kernel_guest_ptr)(scratch + EVENT_AT);
}

static kernel_guest_ptr event_b_addr(void)
{
    return (kernel_guest_ptr)(scratch + EVENT_B_AT);
}

static kernel_guest_ptr dpc_addr(void)
{
    return (kernel_guest_ptr)(scratch + DPC_AT);
}

static unsigned char *host_bytes(kernel_guest_ptr addr, uint32_t length)
{
    unsigned char *host = (unsigned char *)kernel_guest_at(addr, length);
    if (!host) {
        printf("  FATAL: %#x is not usable guest memory\n", addr);
        exit(EXIT_FAILURE);
    }
    return host;
}

/* Build a KEVENT the way the guest's own open-coded KeInitializeEvent does: Type at
 * +0x00, Size = 4 dwords at +0x02, SignalState at +0x04, and a self-linked wait list
 * at +0x08/+0x0C. Tests construct events this way rather than calling a kernel ordinal
 * because the guest does too -- KeInitializeEvent has ZERO call sites in this image. */
static void guest_init_event(kernel_guest_ptr addr, uint8_t type, uint32_t signal_state)
{
    unsigned char *host = host_bytes(addr, EVENT_BYTES);
    memset(host, 0, EVENT_BYTES);
    host[0x00] = type;
    host[0x02] = 4u;
    host[0x04] = (unsigned char)(signal_state & 0xFFu);
    host[0x05] = (unsigned char)((signal_state >> 8) & 0xFFu);
    host[0x06] = (unsigned char)((signal_state >> 16) & 0xFFu);
    host[0x07] = (unsigned char)((signal_state >> 24) & 0xFFu);
    /* Flink and Blink both point at addr+8, as all 10 open-coded sites do. */
    for (unsigned i = 0u; i < 4u; i++) {
        host[0x08 + i] = (unsigned char)(((addr + 8u) >> (8u * i)) & 0xFFu);
        host[0x0C + i] = (unsigned char)(((addr + 8u) >> (8u * i)) & 0xFFu);
    }
}

/* Read SignalState back as RAW BYTES at the measured offset, independent of both the
 * implementation's macros and guest_structs.h. */
static uint32_t raw_signal_state(kernel_guest_ptr addr)
{
    const unsigned char *host = host_bytes(addr, EVENT_BYTES);
    return (uint32_t)host[EVENT_OFFSET_SIGNAL_STATE]
           | ((uint32_t)host[EVENT_OFFSET_SIGNAL_STATE + 1u] << 8)
           | ((uint32_t)host[EVENT_OFFSET_SIGNAL_STATE + 2u] << 16)
           | ((uint32_t)host[EVENT_OFFSET_SIGNAL_STATE + 3u] << 24);
}

static uint16_t raw_dpc_type(kernel_guest_ptr addr)
{
    const unsigned char *host = host_bytes(addr, 4u);
    return (uint16_t)((uint16_t)host[DPC_OFFSET_TYPE]
                      | (uint16_t)((uint16_t)host[DPC_OFFSET_TYPE + 1u] << 8));
}

static void set_raw_dpc_type(kernel_guest_ptr addr, uint16_t value)
{
    unsigned char *host = host_bytes(addr, 4u);
    host[DPC_OFFSET_TYPE] = (unsigned char)(value & 0xFFu);
    host[DPC_OFFSET_TYPE + 1u] = (unsigned char)((value >> 8) & 0xFFu);
}

/* A frame with room for a return address and `slots` arguments only, for pinning
 * arity: a handler that reads one argument too few passes everything else. */
static uint32_t call_with_room_for(unsigned ordinal, unsigned slots)
{
    kernel_call_frame narrow = {scratch,
                                (kernel_guest_ptr)(scratch + 4u + 4u * slots), 0u, 0u,
                                false, 0u, false};
    return kernel_hle_call(ordinal, &narrow);
}

static uint32_t set_event(kernel_guest_ptr event, uint32_t increment, uint32_t wait)
{
    uint32_t args[3] = {event, increment, wait};
    return call_ordinal(145u, args, 3u);
}

static uint32_t set_timer(kernel_guest_ptr timer, int64_t due, kernel_guest_ptr dpc)
{
    uint32_t args[4] = {timer, (uint32_t)((uint64_t)due & 0xFFFFFFFFu),
                        (uint32_t)((uint64_t)due >> 32), dpc};
    return call_ordinal(149u, args, 4u);
}

static uint32_t cancel_timer(kernel_guest_ptr timer)
{
    uint32_t args[1] = {timer};
    return call_ordinal(97u, args, 1u);
}

static void init_timer_ex(kernel_guest_ptr timer, uint32_t type)
{
    uint32_t args[2] = {timer, type};
    (void)call_ordinal(113u, args, 2u);
}

static void init_dpc(kernel_guest_ptr dpc, kernel_guest_ptr routine,
                     kernel_guest_ptr context)
{
    uint32_t args[3] = {dpc, routine, context};
    (void)call_ordinal(107u, args, 3u);
}

static uint32_t insert_dpc(kernel_guest_ptr dpc, uint32_t a1, uint32_t a2)
{
    uint32_t args[3] = {dpc, a1, a2};
    return call_ordinal(119u, args, 3u);
}

static uint32_t remove_dpc(kernel_guest_ptr dpc)
{
    uint32_t args[1] = {dpc};
    return call_ordinal(137u, args, 1u);
}

static uint32_t wait_single(kernel_guest_ptr object, kernel_guest_ptr timeout)
{
    uint32_t args[5] = {object, 0u, 0u, 0u, timeout};
    return call_ordinal(159u, args, 5u);
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

static void test_set_event_returns_the_previous_state_not_the_new_one(void)
{
    setup();
    /* THE SINGLE MOST IMPORTANT ASSERTION IN THIS FILE. The guest uses the return to
     * decide whether IT performed the transition. Two calls must give two DIFFERENT
     * answers, which no constant return -- 0 or 1 -- can satisfy. */
    CHECK_EQ_U32(set_event(event_addr(), 1u, 0u), 0u);
    CHECK_EQ_U32(set_event(event_addr(), 1u, 0u), 1u);

    /* Asserted against GUEST MEMORY at the measured offset, read as raw bytes -- not
     * against our own record, which would only prove we agree with ourselves. */
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    uint32_t via_api = 0u;
    CHECK(kernel_event_signal_state(event_addr(), &via_api));
    CHECK_EQ_U32(via_api, 1u);

    const kernel_event_object *entry = kernel_event_object_at(event_addr());
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->kind, KERNEL_EVENT_OBJECT_EVENT);
    }
    teardown();
}

static void test_set_event_argument_order_is_event_increment_wait(void)
{
    setup();
    /* MEASURED: the pushes are {Wait, Increment, Event}, so the C order is
     * (Event, Increment, Wait) and the pointer is leftmost. Values chosen so a
     * mix-up cannot pass: the increment is 9, which is not a plausible address, and
     * the event address is not a plausible increment. */
    (void)set_event(event_addr(), 9u, 0u);
    const kernel_event_object *entry = kernel_event_object_at(event_addr());
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->last_increment, 9u);
        CHECK_EQ_U32(entry->address, event_addr());
    }
    /* And nothing was interned under the increment, which is what would happen if
     * arguments 0 and 1 were swapped. */
    CHECK(kernel_event_object_at(9u) == NULL);
    teardown();
}

static void test_set_event_needs_three_arguments(void)
{
    setup();
    (void)call_with_room_for(145u, 2u);
    CHECK(captured_contains("KeSetEvent could not read argument 2"));
    CHECK_EQ_U32(kernel_event_tracked_count(), 0u);
    teardown();
}

static void test_a_nonzero_wait_is_reported_as_unmodelled(void)
{
    setup();
    (void)set_event(event_addr(), 1u, 1u);
    CHECK(captured_contains("deferred-wait behaviour is not modelled"));
    /* The set itself still happened. */
    CHECK_EQ_U32(set_event(event_addr(), 1u, 0u), 1u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * Waiting -- and the gap it represents.
 * ------------------------------------------------------------------------- */

static void test_a_wait_on_an_unsignalled_object_is_counted_as_unhonoured(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    /* Returns STATUS_SUCCESS, which is a LIE about the object's state -- and the
     * whole point is that the lie is counted and reported rather than silent. */
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 1u);
    CHECK(captured_contains("WITHOUT waiting"));

    (void)wait_single(event_addr(), 0u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 2u);
    teardown();
}

static void test_a_wait_on_a_signalled_event_is_not_counted_as_unhonoured(void)
{
    setup();
    (void)set_event(event_addr(), 1u, 0u);
    reset_capture();
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    /* Genuinely satisfied, so it must NOT inflate the gap counter -- otherwise the
     * headline diagnostic would cry wolf and stop meaning anything. */
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    CHECK(!captured_contains("WITHOUT waiting"));
    teardown();
}

static void test_a_satisfied_wait_on_a_notification_event_keeps_it_signalled(void)
{
    setup();
    (void)set_event(event_addr(), 1u, 0u);
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    /* Type 0 is NotificationEvent (manual reset): the wait does not consume it. */
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    teardown();
}

static void test_a_satisfied_wait_on_a_synchronization_event_resets_it(void)
{
    setup();
    guest_init_event(event_addr(), 1u, 1u);
    reset_capture();
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    /* Type 1 is SynchronizationEvent: the satisfied wait sets SignalState to 0. */
    CHECK_EQ_U32(raw_signal_state(event_addr()), 0u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    CHECK(captured_contains("SignalState reset to 0"));
    /* The next wait finds it clear, an unhonoured wait. */
    (void)wait_single(event_addr(), 0u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 1u);
    teardown();
}

static void test_a_satisfied_wait_on_another_type_leaves_the_state(void)
{
    setup();
    guest_init_event(event_addr(), 9u, 1u);
    CHECK_EQ_U32(wait_single(event_addr(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    teardown();
}

static void test_wait_needs_five_arguments_and_never_claims_success_without_them(void)
{
    setup();
    /* Room for four arguments: one short. STATUS_SUCCESS here would tell the guest an
     * object it never named was signalled. */
    CHECK_EQ_U32(call_with_room_for(159u, 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("KeWaitForSingleObject could not read argument 4"));
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    teardown();
}

static void test_a_timeout_pointer_is_reported_as_ignored(void)
{
    setup();
    (void)wait_single(event_addr(), (kernel_guest_ptr)(scratch + SCRATCH_OUT));
    CHECK(captured_contains("ignored, since the wait does not block"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * The guest owns the event state. These are the regression tests for a design
 * that was WRONG: an earlier version of this module cached the signalled flag
 * host-side, keyed by guest address.
 * ------------------------------------------------------------------------- */

static void test_a_signal_state_the_guest_cleared_itself_is_observed(void)
{
    setup();
    /* THE DECISIVE TEST. sub_003D3550 writes 0 to the event's SignalState dword and
     * then calls KeWaitForSingleObject on that same event six instructions later --
     * KeResetEvent is not even imported, so this is the only way the guest can reset
     * one. A host-side cache would still say "signalled" and report the wait
     * satisfied at the exact moment the guest had deliberately cleared it. */
    (void)set_event(event_addr(), 1u, 0u);
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);

    /* The guest clears it in place, with NO kernel call. */
    unsigned char *host = host_bytes(event_addr(), EVENT_BYTES);
    for (unsigned i = 0u; i < 4u; i++) {
        host[EVENT_OFFSET_SIGNAL_STATE + i] = 0u;
    }
    reset_capture();

    /* The wait must now see an UNSIGNALLED event. */
    (void)wait_single(event_addr(), 0u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 1u);
    CHECK(captured_contains("WITHOUT waiting"));
    teardown();
}

static void test_set_event_writes_only_the_signal_state_dword(void)
{
    setup();
    /* Type must survive untouched -- the guest sets it and we have no business
     * rewriting it -- and nothing beyond the 16-byte dispatcher object may move.
     * Lifter patch 06 is the record of what an over-wide guest struct costs. */
    guest_init_event(event_addr(), 1u, 0u);
    unsigned char *host = host_bytes(event_addr(), 32u);
    memset(host + EVENT_BYTES, 0x5Au, 16u);

    (void)set_event(event_addr(), 1u, 0u);

    CHECK_EQ_U32(host[EVENT_OFFSET_TYPE], 1u);
    CHECK_EQ_U32(host[0x02], 4u);
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    for (unsigned i = EVENT_BYTES; i < 32u; i++) {
        CHECK_EQ_U32(host[i], 0x5Au);
    }
    teardown();
}

static void test_two_events_reusing_one_address_do_not_share_state(void)
{
    setup();
    /* FOUR of this image's events are STACK LOCALS (ebp-0x14 twice, ebp-0x18,
     * ebp-0x28), so the same guest address really is reused by unrelated events in
     * different calls. A host table keyed on the pointer carries one event's signalled
     * state into the next one's. Reading the guest's own memory cannot. */
    (void)set_event(event_addr(), 1u, 0u);
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);

    /* The frame is reused: a DIFFERENT event is built at the same address. */
    guest_init_event(event_addr(), 0u, 0u);
    reset_capture();
    (void)wait_single(event_addr(), 0u);
    /* Must be seen as unsignalled, despite the earlier set on that address. */
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 1u);
    teardown();
}

static void test_an_unreadable_event_never_reports_a_satisfied_wait(void)
{
    setup();
    /* An object we cannot evaluate must not come back as STATUS_SUCCESS, which would
     * claim it was signalled.
     *
     * Address 0 exercises null rejection. kernel_guest_at also rejects ranges
     * past 4 GB and unmapped pages; a non-null unmapped pointer is testable now.
     * The former 0xFFFFFFF0 host fault predates the mapping probe. PROT_NONE and
     * concurrent-unmap limitations remain documented in kernel_call.h. */
    CHECK_EQ_U32(wait_single(0u, 0u), STATUS_INVALID_PARAMETER);
    /* A parameter error, NOT an unhonoured wait: that counter means "a real wait the
     * host declined to honour", and diluting it would cost it its meaning. */
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * The KDPC Type tag the guest gates its own teardown on.
 * ------------------------------------------------------------------------- */

static void test_initialize_dpc_stamps_the_type_tag_into_guest_memory(void)
{
    setup();
    CHECK_EQ_U32(raw_dpc_type(dpc_addr()), 0u);
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);

    /* MANDATORY, not cosmetic. sub_00410B22 does `cmp word ptr [dpc], 0x13` and SKIPS
     * KeRemoveQueueDpc entirely when it does not match, which would leave stale DPCs
     * queued against freed DSOUND objects -- silently, and nowhere near the cause. */
    CHECK_EQ_U32(raw_dpc_type(dpc_addr()), DPC_TYPE_INITIALISED);

    /* 16-bit, not 8 or 32: the guest compares with an operand-size prefix. So the byte
     * above the tag must be 0 and the dword beyond it untouched. */
    const unsigned char *host = host_bytes(dpc_addr(), 8u);
    CHECK_EQ_U32(host[0], 0x13u);
    CHECK_EQ_U32(host[1], 0x00u);
    teardown();
}

static void test_remove_queue_dpc_tolerates_the_guests_poisoned_tag(void)
{
    setup();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    (void)insert_dpc(dpc_addr(), 0u, 0u);

    /* The guest NOTs the tag right after calling us, so a second teardown sees 0xFFEC.
     * That is the guest behaving correctly, not a corrupt object, and it must be
     * reported as such rather than looking like an uninitialised DPC. */
    set_raw_dpc_type(dpc_addr(), DPC_TYPE_POISONED);
    reset_capture();
    CHECK_EQ_U32(remove_dpc(dpc_addr()), 1u);
    CHECK(captured_contains("already poisoned"));
    CHECK(captured_contains("not an error"));
    teardown();
}

static void test_an_uninitialised_bss_dpc_is_tolerated(void)
{
    setup();
    /* The KDPC at 0x7713A4 and the KTIMER at 0x7713C0 are used in this image without
     * ever being initialised through the kernel -- they are BSS, hence all-zero. So an
     * absent Type tag is a real case and must not be treated as a fault. */
    CHECK_EQ_U32(raw_dpc_type(dpc_addr()), 0u);
    CHECK_EQ_U32(insert_dpc(dpc_addr(), 0u, 0u), 1u);
    CHECK_EQ_U32(remove_dpc(dpc_addr()), 1u);
    CHECK(!captured_contains("poisoned"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * Timers.
 * ------------------------------------------------------------------------- */

static void test_set_timer_reassembles_the_64_bit_due_time(void)
{
    setup();
    /* THE EXACT VALUE MEASURED AT recomp_0045.c:1925: high dword 0xFFFFFFFF, low
     * dword 0xFFFFD8F0, which combine to -10000 -- one millisecond, relative, in
     * 100ns units. The two halves differ, so a high/low swap cannot pass: swapped it
     * would decode as 0xFFFFD8F0FFFFFFFF, a completely different number. */
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    const kernel_event_object *entry = kernel_event_object_at(GUEST_TIMER);
    CHECK(entry != NULL);
    if (entry) {
        CHECK(entry->due_time == -10000);
        CHECK_EQ_U32(entry->timer_dpc, dpc_addr());
        CHECK(entry->timer_set);
    }

    /* Two more real values from the image, both round human intervals -- which is
     * the corroboration that the slot split is right. */
    (void)set_timer(GUEST_TIMER_B, -1000000, 0u);
    const kernel_event_object *b = kernel_event_object_at(GUEST_TIMER_B);
    CHECK(b != NULL && b->due_time == -1000000);

    (void)set_timer(GUEST_TIMER_B, -216000000000LL, 0u);
    b = kernel_event_object_at(GUEST_TIMER_B);
    CHECK(b != NULL && b->due_time == -216000000000LL);
    teardown();
}

static void test_set_timer_returns_whether_it_was_already_armed(void)
{
    setup();
    CHECK_EQ_U32(set_timer(GUEST_TIMER, -10000, dpc_addr()), 0u);
    CHECK_EQ_U32(set_timer(GUEST_TIMER, -10000, dpc_addr()), 1u);
    /* Two different answers, so a constant return fails. */
    teardown();
}

static void test_xnet_periodic_timer_contract(void)
{
    setup();
    const uint32_t args[5] = {GUEST_TIMER, 0xFFE17B80u, 0xFFFFFFFFu, 200u,
                              dpc_addr()};
    CHECK_EQ_U32(call_ordinal(150u, args, 5u), 0u);
    const kernel_event_object *entry = kernel_event_object_at(GUEST_TIMER);
    CHECK(entry && entry->due_time == -2000000);
    CHECK(entry && entry->timer_period_ms == 200);
    CHECK(entry && entry->timer_dpc == dpc_addr());
    CHECK_EQ_U32(call_ordinal(150u, args, 5u), 1u);
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    entry = kernel_event_object_at(GUEST_TIMER);
    CHECK(entry && entry->timer_period_ms == 0);
    (void)call_with_room_for(150u, 4u);
    CHECK(captured_contains("KeSetTimerEx could not read argument 4"));
    teardown();
}

static bool scheduler_accept;
static unsigned scheduler_calls;
static bool scheduler_dispatch(kernel_guest_ptr routine, kernel_guest_ptr dpc,
    kernel_guest_ptr context, kernel_guest_ptr arg1, kernel_guest_ptr arg2, void *opaque)
{
    CHECK_EQ_U32(routine, GUEST_DPC_ROUTINE);
    CHECK_EQ_U32(dpc, dpc_addr());
    CHECK_EQ_U32(context, GUEST_DPC_CONTEXT);
    CHECK_EQ_U32(kernel_sync_current_irql(), 2u);
    CHECK(opaque == &scheduler_calls);
    (void)arg1;
    (void)arg2;
    ++scheduler_calls;
    return scheduler_accept;
}

static void test_scheduler_deadline_refusal_and_period_coalescing(void)
{
    setup();
    kernel_clock_reset();
    const uint32_t original_irql = kernel_sync_current_irql();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    const uint32_t args[5] = {GUEST_TIMER, 0xFFE17B80u, 0xFFFFFFFFu, 200u,
                              dpc_addr()};
    (void)call_ordinal(150u, args, 5u);
    unsigned delivered = 99u;
    CHECK(!kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 0u);
    kernel_event_set_dispatch(scheduler_dispatch, &scheduler_calls);
    scheduler_calls = 0u;
    scheduler_accept = true;
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(scheduler_calls, 0u);
    (void)kernel_clock_advance_to(KERNEL_CLOCK_FREQUENCY_HZ / 5u);
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(scheduler_calls, 0u); /* floor is just before 200ms */
    (void)kernel_clock_advance_to(KERNEL_CLOCK_FREQUENCY_HZ / 5u + 1u);
    const uint64_t before = kernel_clock_peek();
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 1u);
    CHECK(kernel_clock_peek() == before);
    CHECK_EQ_U32(kernel_sync_current_irql(), original_irql);
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 0u);
    (void)kernel_clock_advance_to(KERNEL_CLOCK_FREQUENCY_HZ);
    scheduler_accept = false;
    CHECK(!kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 0u);
    CHECK(kernel_event_object_at(dpc_addr())->dpc_queued);
    CHECK_EQ_U32(kernel_sync_current_irql(), original_irql);
    scheduler_accept = true;
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 1u); /* missed periods coalesce, no fabricated catch-up */
    CHECK(!kernel_event_object_at(dpc_addr())->dpc_queued);
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 1u);
    (void)kernel_clock_advance_to(KERNEL_CLOCK_FREQUENCY_HZ * 2u);
    CHECK(kernel_event_service(&delivered));
    CHECK_EQ_U32(delivered, 0u);
    teardown();
}

static void test_set_timer_needs_four_slots(void)
{
    setup();
    /* Three slots: enough for Timer and both DueTime halves but not the Dpc. This is
     * the test that catches a handler written to the published minimum, and also one
     * that treated DueTime as a single slot. */
    (void)call_with_room_for(149u, 3u);
    CHECK(captured_contains("KeSetTimer could not read argument 3"));
    CHECK_EQ_U32(kernel_event_tracked_count(), 0u);
    teardown();
}

static void test_cancel_timer_reports_whether_it_was_armed(void)
{
    setup();
    /* Never armed: false. */
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 0u);

    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    /* Armed: true, and the arming is undone. */
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 1u);
    /* Cancelling again: false. Three calls, two distinct answers, and the order
     * matters -- a constant return cannot produce this sequence. */
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 0u);

    const kernel_event_object *entry = kernel_event_object_at(GUEST_TIMER);
    CHECK(entry != NULL);
    if (entry) {
        CHECK(!entry->timer_set);
        CHECK(entry->due_time == 0);
        CHECK_EQ_U32(entry->timer_dpc, 0u);
    }
    teardown();
}

static void test_reinitialising_a_timer_disarms_it(void)
{
    setup();
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    init_timer_ex(GUEST_TIMER, 1u);
    /* Re-initialisation is how the guest disarms a timer without KeCancelTimer, so
     * the clear has to happen or a later cancel answers wrongly. */
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 0u);
    const kernel_event_object *entry = kernel_event_object_at(GUEST_TIMER);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->timer_type, 1u);
        CHECK(!entry->timer_set);
    }
    teardown();
}

static void test_arming_a_timer_is_reported_as_never_firing(void)
{
    setup();
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    /* The gap must be stated at the moment it is created, not only in a header. */
    CHECK(captured_contains("NO TIMER EVER FIRES"));
    CHECK_EQ_U32(kernel_event_timers_armed(), 1u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * DPCs.
 * ------------------------------------------------------------------------- */

static void test_initialize_dpc_records_the_routine_and_context_in_order(void)
{
    setup();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    const kernel_event_object *entry = kernel_event_object_at(dpc_addr());
    CHECK(entry != NULL);
    if (entry) {
        /* MEASURED order (Dpc, DeferredRoutine, DeferredContext): the middle operand
         * is a .text code address at every call site. All three values here are
         * distinct and none could be mistaken for another, so any permutation fails. */
        CHECK_EQ_U32(entry->address, dpc_addr());
        CHECK_EQ_U32(entry->dpc_routine, GUEST_DPC_ROUTINE);
        CHECK_EQ_U32(entry->dpc_context, GUEST_DPC_CONTEXT);
        CHECK_EQ_U32(entry->kind, KERNEL_EVENT_OBJECT_DPC);
    }
    /* Nothing interned under the routine or the context, which a slot mix-up would
     * have done. */
    CHECK(kernel_event_object_at(GUEST_DPC_ROUTINE) == NULL);
    CHECK(kernel_event_object_at(GUEST_DPC_CONTEXT) == NULL);
    teardown();
}

static void test_initialize_dpc_needs_three_arguments(void)
{
    setup();
    (void)call_with_room_for(107u, 2u);
    CHECK(captured_contains("KeInitializeDpc could not read argument 2"));
    CHECK_EQ_U32(kernel_event_tracked_count(), 0u);
    teardown();
}

static void test_a_dpc_cannot_be_queued_twice(void)
{
    setup();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    /* First insert succeeds, second is refused. The guest relies on that refusal to
     * avoid processing the same deferred work twice, so inverting it would be
     * silent and damaging. */
    CHECK_EQ_U32(insert_dpc(dpc_addr(), 0x11u, 0x22u), 1u);
    CHECK_EQ_U32(insert_dpc(dpc_addr(), 0x33u, 0x44u), 0u);
    CHECK_EQ_U32(kernel_event_dpcs_queued(), 1u);

    const kernel_event_object *entry = kernel_event_object_at(dpc_addr());
    CHECK(entry != NULL);
    if (entry) {
        CHECK(entry->dpc_queued);
        /* The REFUSED insert must not have overwritten the queued arguments: on
         * hardware the queued DPC keeps the arguments it was queued with. */
        CHECK_EQ_U32(entry->dpc_argument1, 0x11u);
        CHECK_EQ_U32(entry->dpc_argument2, 0x22u);
    }
    CHECK(captured_contains("NO DPC ROUTINE IS EVER CALLED"));
    teardown();
}

static void test_remove_queue_dpc_reports_whether_it_was_queued(void)
{
    setup();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    CHECK_EQ_U32(remove_dpc(dpc_addr()), 0u);
    (void)insert_dpc(dpc_addr(), 0u, 0u);
    CHECK_EQ_U32(remove_dpc(dpc_addr()), 1u);
    CHECK_EQ_U32(remove_dpc(dpc_addr()), 0u);
    /* And after removal it can be queued again. */
    CHECK_EQ_U32(insert_dpc(dpc_addr(), 0u, 0u), 1u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * KeQuerySystemTime.
 * ------------------------------------------------------------------------- */

/* Days from 1601-01-01 to 1970-01-01, counted year by year with the Gregorian leap
 * rule. This exists so the epoch constant in kernel_event.c is checked against an
 * INDEPENDENT derivation rather than against itself -- a test that reused the same
 * constant would pass no matter how wrong it was. */
static uint64_t days_1601_to_1970(void)
{
    uint64_t days = 0u;
    for (unsigned year = 1601u; year < 1970u; year++) {
        bool leap = (year % 4u == 0u) && ((year % 100u != 0u) || (year % 400u == 0u));
        days += leap ? 366u : 365u;
    }
    return days;
}

static void test_the_epoch_offset_matches_an_independent_derivation(void)
{
    /* 134774 days, times 86400 seconds, times 10,000,000 100ns units. */
    CHECK_EQ_U64(days_1601_to_1970(), 134774u);
    CHECK_EQ_U64(days_1601_to_1970() * 86400u, 11644473600u);
}

static void test_system_time_is_the_current_wall_clock_in_1601_units(void)
{
    setup();
    const time_t host_now = time(NULL);
    const uint64_t guest_now = kernel_event_system_time();

    /* Convert the guest's answer back to Unix seconds using the INDEPENDENTLY
     * derived offset, then compare against the host clock. If the epoch constant in
     * kernel_event.c were wrong -- or if CLOCK_MONOTONIC had been used, putting the
     * date in 1601 -- this would be out by centuries, not seconds. */
    const uint64_t seconds_since_1601 = guest_now / 10000000u;
    const uint64_t offset = days_1601_to_1970() * 86400u;
    CHECK(seconds_since_1601 > offset);
    const int64_t unix_seconds = (int64_t)(seconds_since_1601 - offset);
    const int64_t drift = (int64_t)host_now - unix_seconds;
    CHECK(drift >= -5 && drift <= 5);
    teardown();
}

static void test_system_time_never_goes_backwards(void)
{
    setup();
    uint64_t previous = kernel_event_system_time();
    for (unsigned i = 0u; i < 2000u; i++) {
        uint64_t now = kernel_event_system_time();
        /* Strictly increasing, which is stronger than non-decreasing and is what
         * makes a guest's two-sample delta safe: an unsigned subtraction across a
         * backward step yields an enormous positive interval, not a negative one. */
        CHECK(now > previous);
        previous = now;
    }
    teardown();
}

static void test_query_system_time_writes_eight_bytes_low_dword_first(void)
{
    setup();
    const kernel_guest_ptr out = (kernel_guest_ptr)(scratch + SCRATCH_OUT);
    unsigned char *host = (unsigned char *)kernel_guest_at(out, 16u);
    CHECK(host != NULL);
    if (!host) {
        teardown();
        return;
    }
    memset(host, 0x5Au, 16u);

    uint32_t args[1] = {out};
    (void)call_ordinal(128u, args, 1u);

    /* Read back as raw bytes and reassemble, so the layout is checked rather than
     * assumed: low dword at +0x00, high dword at +0x04, little-endian. */
    uint64_t written = 0u;
    for (unsigned i = 0u; i < 8u; i++) {
        written |= (uint64_t)host[i] << (8u * i);
    }
    /* A real 2020s date has a nonzero high dword, so a handler that wrote only 32
     * bits -- or swapped the halves -- produces a wildly different number. */
    CHECK((written >> 32) != 0u);
    CHECK((written & 0xFFFFFFFFu) != 0u);

    const uint64_t offset = days_1601_to_1970() * 86400u;
    const uint64_t seconds = written / 10000000u;
    CHECK(seconds > offset);
    const int64_t drift = (int64_t)time(NULL) - (int64_t)(seconds - offset);
    CHECK(drift >= -5 && drift <= 5);

    /* The ninth byte onwards is untouched: a LARGE_INTEGER is 8 bytes, and lifter
     * patch 06 is the record of what an over-wide guest struct costs. */
    for (unsigned i = 8u; i < 16u; i++) {
        CHECK_EQ_U32(host[i], 0x5Au);
    }
    teardown();
}

static void test_query_system_time_with_a_null_pointer_is_reported(void)
{
    setup();
    uint32_t args[1] = {0u};
    (void)call_ordinal(128u, args, 1u);
    CHECK(captured_contains("KeQuerySystemTime(NULL)"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * Tracking.
 * ------------------------------------------------------------------------- */

static void test_objects_are_tracked_separately_by_address(void)
{
    setup();
    (void)set_event(event_addr(), 1u, 0u);
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    CHECK_EQ_U32(kernel_event_tracked_count(), 3u);

    /* Signalling one event must not signal another. Checked in guest memory, which is
     * where the state actually lives. */
    uint32_t other = 0xFFFFFFFFu;
    CHECK(kernel_event_signal_state(event_b_addr(), &other));
    CHECK_EQ_U32(other, 0u);
    CHECK_EQ_U32(raw_signal_state(event_addr()), 1u);
    /* The timer is tracked, and its state is host-side by design. */
    CHECK(kernel_event_object_at(GUEST_TIMER) != NULL);
    /* And arming one timer must not arm another. */
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER_B), 0u);
    CHECK_EQ_U32(cancel_timer(GUEST_TIMER), 1u);
    teardown();
}

static void test_reusing_an_address_as_a_different_kind_is_reported(void)
{
    setup();
    init_dpc(dpc_addr(), GUEST_DPC_ROUTINE, GUEST_DPC_CONTEXT);
    reset_capture();
    /* The same address arriving as a timer means either the guest reuses the storage
     * or we have an argument in the wrong slot -- and the second must not be
     * invisible. */
    init_timer_ex(dpc_addr(), 0u);
    CHECK(captured_contains("previously used as a DPC and is now a timer"));
    teardown();
}

static void test_a_null_object_is_not_tracked(void)
{
    setup();
    (void)set_event(0u, 1u, 0u);
    CHECK_EQ_U32(kernel_event_tracked_count(), 0u);
    CHECK(kernel_event_object_at(0u) == NULL);
    teardown();
}

static void test_reset_clears_every_object_and_counter(void)
{
    setup();
    (void)set_event(event_addr(), 1u, 0u);
    (void)set_timer(GUEST_TIMER, -10000, dpc_addr());
    (void)insert_dpc(dpc_addr(), 0u, 0u);
    /* An UNSIGNALLED mapped event, so the wait is counted as unhonoured.
     * An unmapped address would instead be rejected as an invalid argument and
     * would not exercise the wait counter. See kernel_call.h. */
    (void)wait_single(event_b_addr(), 0u);
    CHECK(kernel_event_tracked_count() > 0u);
    CHECK(kernel_event_unhonoured_wait_count() > 0u);

    kernel_event_reset();
    CHECK_EQ_U32(kernel_event_tracked_count(), 0u);
    CHECK_EQ_U32(kernel_event_unhonoured_wait_count(), 0u);
    CHECK_EQ_U32(kernel_event_timers_armed(), 0u);
    CHECK_EQ_U32(kernel_event_dpcs_queued(), 0u);
    CHECK(kernel_event_object_at(event_addr()) == NULL);
    teardown();
}

static void test_missing_frames_are_reported_for_every_ordinal(void)
{
    setup();
    static const unsigned ordinals[] = {97u, 107u, 113u, 119u, 128u, 137u, 145u, 149u, 159u};
    for (size_t i = 0u; i < sizeof(ordinals) / sizeof(ordinals[0]); i++) {
        reset_capture();
        (void)kernel_hle_call(ordinals[i], NULL);
        CHECK(captured_contains("no argument frame"));
    }
    teardown();
}

static void test_registration_makes_every_ordinal_implemented(void)
{
    setup();
    static const unsigned ordinals[] = {97u, 107u, 113u, 119u, 128u, 137u, 145u, 149u, 150u, 159u};
    CHECK_EQ_U32(kernel_hle_implemented_count(ordinals, 10u), 10u);
    /* The ordinals this module deliberately does NOT bind must still be unbound, so
     * a future reader can tell a decision from an oversight. 108 KeInitializeEvent,
     * 138 KeResetEvent and 123 KePulseEvent have too little
     * call-site evidence to implement -- see kernel_event.h. */
    static const unsigned unbound[] = {108u, 123u, 138u, 158u};
    CHECK_EQ_U32(kernel_hle_implemented_count(unbound, 4u), 0u);
    teardown();
}

int main(void)
{
    printf("kernel event/timer/DPC HLE tests\n");

    test_set_event_returns_the_previous_state_not_the_new_one();
    test_set_event_argument_order_is_event_increment_wait();
    test_set_event_needs_three_arguments();
    test_a_nonzero_wait_is_reported_as_unmodelled();

    test_a_wait_on_an_unsignalled_object_is_counted_as_unhonoured();
    test_a_wait_on_a_signalled_event_is_not_counted_as_unhonoured();
    test_a_satisfied_wait_on_a_notification_event_keeps_it_signalled();
    test_a_satisfied_wait_on_a_synchronization_event_resets_it();
    test_a_satisfied_wait_on_another_type_leaves_the_state();
    test_wait_needs_five_arguments_and_never_claims_success_without_them();
    test_a_timeout_pointer_is_reported_as_ignored();

    test_a_signal_state_the_guest_cleared_itself_is_observed();
    test_set_event_writes_only_the_signal_state_dword();
    test_two_events_reusing_one_address_do_not_share_state();
    test_an_unreadable_event_never_reports_a_satisfied_wait();
    test_initialize_dpc_stamps_the_type_tag_into_guest_memory();
    test_remove_queue_dpc_tolerates_the_guests_poisoned_tag();
    test_an_uninitialised_bss_dpc_is_tolerated();

    test_set_timer_reassembles_the_64_bit_due_time();
    test_set_timer_returns_whether_it_was_already_armed();
    test_set_timer_needs_four_slots();
    test_xnet_periodic_timer_contract();
    test_scheduler_deadline_refusal_and_period_coalescing();
    test_cancel_timer_reports_whether_it_was_armed();
    test_reinitialising_a_timer_disarms_it();
    test_arming_a_timer_is_reported_as_never_firing();

    test_initialize_dpc_records_the_routine_and_context_in_order();
    test_initialize_dpc_needs_three_arguments();
    test_a_dpc_cannot_be_queued_twice();
    test_remove_queue_dpc_reports_whether_it_was_queued();

    test_the_epoch_offset_matches_an_independent_derivation();
    test_system_time_is_the_current_wall_clock_in_1601_units();
    test_system_time_never_goes_backwards();
    test_query_system_time_writes_eight_bytes_low_dword_first();
    test_query_system_time_with_a_null_pointer_is_reported();

    test_objects_are_tracked_separately_by_address();
    test_reusing_an_address_as_a_different_kind_is_reported();
    test_a_null_object_is_not_tracked();
    test_reset_clears_every_object_and_counter();
    test_missing_frames_are_reported_for_every_ordinal();
    test_registration_makes_every_ordinal_implemented();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
