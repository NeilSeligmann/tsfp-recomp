/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HAL ordinal 47, HalRegisterShutdownNotification.
 *
 * WHAT THERE IS TO GET WRONG HERE, in order of how badly it hurts:
 *
 *   1. THE ARITY. Our handler is the __stdcall callee, so the dispatcher pops the
 *      argument bytes. 2 is the count, hand-verified at all 12 call sites in this
 *      image (see src/xbox/kernel_hal.h); reading one argument instead of two would
 *      pass a naive "did it register?" test and desync the guest's esp forever. So
 *      one test gives the handler a frame too short for the second argument and
 *      requires it to refuse.
 *   2. THE ARGUMENT ORDER. stdcall pushes right to left, so the registration
 *      pointer is argument 0 and the BOOLEAN is argument 1. Swap them and the
 *      handler starts treating 0 and 1 as block addresses. That is invisible to a
 *      test that only ever registers, which is why the tests below assert on the
 *      register/DEregister pair the guest actually performs at 0x0043AC8B and
 *      0x0043ACBB with one pointer.
 *   3. THE SET ITSELF. A deregister that does not clear its slot leaves a block
 *      registered forever, and nothing in a shutdown-free run would ever notice.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. The handler reads two stack
 * arguments from a frame built in scratch guest memory and keeps the registration
 * set host-side, so there is no image address to resolve. The one real address this
 * suite uses, 0x549650, is used only as an OPAQUE VALUE -- the static registration
 * block the guest passes at 0x00381288 -- never dereferenced, so no mapping of the
 * image is needed for it to be the honest input.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "kernel_hal.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,  \
                   (unsigned)a_, (unsigned)e_);                                           \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

static char captured[16384];
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
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define ORD_HAL_REGISTER_SHUTDOWN 47u
#define ORD_HAL_RETURN_TO_FIRMWARE 49u

/* Scratch for call frames. The frame is guest memory because kernel_frame_arg reads
 * the arguments out of guest memory, exactly as it does for a real call. */
#define SCRATCH_BYTES 0x1000u
#define SCRATCH_FRAME 0x200u

static kernel_guest_ptr scratch;

/* The static registration block this image passes at guest 0x00381288. An opaque
 * value here, never dereferenced -- which is also all the real kernel would need it
 * for if it were not threading links through it. */
#define GUEST_STATIC_REGISTRATION 0x549650u

static void setup(void)
{
    kernel_hle_init();
    kernel_hal_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_hal_register(), 4u);
    kernel_hle_set_log(capture_printer);
    reset_capture();

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FATAL could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hal_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

/* Call the ordinal with `count` arguments. `count` is a parameter rather than a
 * constant 2 so that the arity test can hand over a frame that is one slot short. */
static uint32_t call_hal(const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch, SCRATCH_FRAME, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* The frame built above is `count` slots long. Clamping the limit to exactly
     * that is what makes a read of argument 1 from a 1-argument frame FAIL rather
     * than silently returning whatever sits past the arguments -- which is the
     * entire point of the arity test below. */
    frame.stack_limit = scratch + (count + 1u) * 4u;
    return kernel_hle_call(ORD_HAL_REGISTER_SHUTDOWN, &frame);
}

static uint32_t register_block(kernel_guest_ptr block)
{
    const uint32_t args[2] = {block, 1u};
    return call_hal(args, 2u);
}

static uint32_t deregister_block(kernel_guest_ptr block)
{
    const uint32_t args[2] = {block, 0u};
    return call_hal(args, 2u);
}

/* ------------------------------------------------------------------------- */

/*
 * The ordinal stops being a stub. This is the line that moves the bring-up run on.
 *
 * MUTATION: remove the binding from `kernel_hal_register`'s table and this fails.
 */
/* --- ordinal 49, HalReturnToFirmware ---------------------------------------
 *
 * The terminus of the boot path. Three of its four call sites sit in code the title
 * reaches when storage fails, so getting it wrong does not look like a reboot bug, it
 * looks like the trace just ending.
 */

static uint32_t sink_routine;
static unsigned sink_pending;
static unsigned sink_calls;

static void recording_sink(uint32_t routine, unsigned pending)
{
    sink_routine = routine;
    sink_pending = pending;
    sink_calls++;
}

static uint32_t call_firmware(const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch, SCRATCH_FRAME, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    frame.stack_limit = scratch + (count + 1u) * 4u;
    return kernel_hle_call(ORD_HAL_RETURN_TO_FIRMWARE, &frame);
}

static void test_firmware_return_is_implemented_not_a_stub(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_HAL_RETURN_TO_FIRMWARE);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
    }
    teardown();
}

static void test_the_routine_argument_reaches_the_sink_unchanged(void)
{
    setup();
    sink_calls = 0u;
    sink_routine = 0xFFFFFFFFu;
    kernel_hal_set_firmware_sink(recording_sink);

    /* 2 is the value three of the four measured sites push. Asserted as a literal,
     * not as a named constant: this module deliberately does not name the routine
     * enumerators, so a test that used a name would be testing a label we declined
     * to derive. */
    const uint32_t args[1] = {2u};
    (void)call_firmware(args, 1u);

    CHECK_EQ_U32(sink_calls, 1u);
    CHECK_EQ_U32(sink_routine, 2u);
    CHECK_EQ_U32(kernel_hal_last_firmware_routine(), 2u);
    CHECK_EQ_U32(kernel_hal_firmware_return_count(), 1u);

    /* The 4 at 0x00381253 must arrive as 4, not be clamped or folded into the
     * common case. A handler that ignored the argument would pass every assertion
     * above and fail this one. */
    kernel_hal_reset();
    const uint32_t other[1] = {4u};
    (void)call_firmware(other, 1u);
    CHECK_EQ_U32(sink_routine, 4u);
    CHECK_EQ_U32(kernel_hal_last_firmware_routine(), 4u);

    kernel_hal_set_firmware_sink(NULL);
    teardown();
}

static void test_the_sink_is_told_how_many_registrations_were_pending(void)
{
    setup();
    sink_calls = 0u;
    sink_pending = 0xFFFFFFFFu;
    kernel_hal_set_firmware_sink(recording_sink);

    /* Two live registrations, so a count of 0 or 1 would both be wrong. Asserting
     * against a nonzero number matters: the real kernel walks this set, we do not,
     * and the count is the only honest part of that claim. */
    CHECK_EQ_U32(register_block(GUEST_STATIC_REGISTRATION), 0u);
    CHECK_EQ_U32(register_block(GUEST_STATIC_REGISTRATION + 0x40u), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 2u);

    const uint32_t args[1] = {2u};
    (void)call_firmware(args, 1u);
    CHECK_EQ_U32(sink_pending, 2u);
    /* And it is REPORTED, not merely passed: the log is what a human reads. */
    CHECK(captured_contains("ASKED TO REBOOT"));

    kernel_hal_set_firmware_sink(NULL);
    teardown();
}

static void test_no_sink_continues_but_says_so_loudly(void)
{
    setup();
    kernel_hal_set_firmware_sink(NULL);

    const uint32_t args[1] = {2u};
    (void)call_firmware(args, 1u);

    /* Continuing past a reboot is wrong, and the only thing worse than doing it is
     * doing it quietly. The count still moves, so a report can show it happened. */
    CHECK_EQ_U32(kernel_hal_firmware_return_count(), 1u);
    CHECK(captured_contains("no firmware sink"));
    CHECK(captured_contains("cannot honestly survive"));
    teardown();
}

static void test_a_sink_that_returns_is_reported(void)
{
    setup();
    sink_calls = 0u;
    kernel_hal_set_firmware_sink(recording_sink);

    const uint32_t args[1] = {2u};
    (void)call_firmware(args, 1u);

    /* recording_sink DOES return, which no real firmware call can. That must not pass
     * silently, because every later observation is from a console that rebooted. */
    CHECK_EQ_U32(sink_calls, 1u);
    CHECK(captured_contains("RETURNED from"));
    CHECK(captured_contains("faithful trace"));

    kernel_hal_set_firmware_sink(NULL);
    teardown();
}

static void test_a_second_firmware_return_is_called_out(void)
{
    setup();
    kernel_hal_set_firmware_sink(NULL);
    const uint32_t args[1] = {2u};
    (void)call_firmware(args, 1u);
    reset_capture();
    (void)call_firmware(args, 1u);

    CHECK_EQ_U32(kernel_hal_firmware_return_count(), 2u);
    CHECK(captured_contains("did not stop anything"));
    teardown();
}

static void test_a_missing_firmware_frame_is_reported_not_guessed(void)
{
    setup();
    sink_calls = 0u;
    kernel_hal_set_firmware_sink(recording_sink);

    /* A frameless call must still stop -- the reboot happened regardless -- but the
     * routine is UNKNOWN and the report has to say that rather than quietly passing 0,
     * which is a legitimate routine value. */
    (void)kernel_hle_call(ORD_HAL_RETURN_TO_FIRMWARE, NULL);
    CHECK_EQ_U32(sink_calls, 1u);
    CHECK(captured_contains("no argument frame"));
    CHECK(captured_contains("routine value is unknown"));

    kernel_hal_set_firmware_sink(NULL);
    teardown();
}

static void test_reset_clears_the_firmware_counters_but_keeps_the_sink(void)
{
    setup();
    sink_calls = 0u;
    kernel_hal_set_firmware_sink(recording_sink);
    const uint32_t args[1] = {4u};
    (void)call_firmware(args, 1u);
    CHECK_EQ_U32(kernel_hal_firmware_return_count(), 1u);

    kernel_hal_reset();
    CHECK_EQ_U32(kernel_hal_firmware_return_count(), 0u);
    CHECK_EQ_U32(kernel_hal_last_firmware_routine(), 0u);

    /* THE SINK SURVIVES A RESET, deliberately. It is host wiring installed once; a
     * reset that detached it would leave the next reboot request unable to stop the
     * run, and nothing would say the sink had gone. */
    sink_calls = 0u;
    (void)call_firmware(args, 1u);
    CHECK_EQ_U32(sink_calls, 1u);

    kernel_hal_set_firmware_sink(NULL);
    teardown();
}

static void test_registration_makes_the_ordinal_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_HAL_REGISTER_SHUTDOWN);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
        /* Resolved against tools/kernel_ordinals.py, which is what generated the
         * name table this reads back. */
        CHECK(entry->name != NULL && strcmp(entry->name,
                                            "HalRegisterShutdownNotification") == 0);
    }
    teardown();
}

/*
 * A real implementation must never emit the stub notice, or the backlog report keeps
 * listing work that is already done.
 */
static void test_an_implemented_ordinal_does_not_report_itself_as_a_stub(void)
{
    setup();
    (void)register_block(GUEST_STATIC_REGISTRATION);
    CHECK(!captured_contains("not implemented"));
    CHECK(!captured_contains("stub"));
    teardown();
}

/*
 * The guest's own first call: the static block at 0x549650 with Register = TRUE.
 *
 * MUTATION: make `add_registration` a no-op and this fails.
 */
static void test_the_guests_own_first_call_records_the_block(void)
{
    setup();
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    (void)register_block(GUEST_STATIC_REGISTRATION);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);
    CHECK(kernel_hal_shutdown_registered(GUEST_STATIC_REGISTRATION));
    CHECK_EQ_U32(kernel_hal_shutdown_duplicate_count(), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_unmatched_count(), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_overflow_count(), 0u);
    teardown();
}

/*
 * THE ARGUMENT ORDER, which is the single most likely way to get this ordinal wrong.
 *
 * MUTATION: swap the two `kernel_frame_arg` indices in the handler and this fails.
 * It fails for a reason worth spelling out: with the arguments swapped, a register
 * call reads the registration pointer as 1 and the flag as the low byte of the
 * pointer, so the block the guest asked about is NOT registered and a block at
 * address 1 is. Both halves are asserted, because asserting only
 * `shutdown_count() == 1` would pass against the swap.
 */
static void test_the_pointer_is_argument_zero_and_the_flag_argument_one(void)
{
    setup();
    (void)register_block(GUEST_STATIC_REGISTRATION);
    CHECK(kernel_hal_shutdown_registered(GUEST_STATIC_REGISTRATION));
    /* The low byte of 0x549650 is 0x50, so a swapped handler would have registered
     * the block at address 1 instead. */
    CHECK(!kernel_hal_shutdown_registered(1u));
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);
    teardown();
}

/*
 * The register/deregister pair the guest performs on ONE pointer at 0x0043AC8B and
 * 0x0043ACBB.
 *
 * MUTATION: delete the `registrations[slot] = 0u;` from `remove_registration` and
 * this fails -- the block stays registered forever, which nothing in a run that
 * never shuts down would otherwise notice.
 */
static void test_a_register_deregister_pair_leaves_nothing_behind(void)
{
    setup();
    const kernel_guest_ptr block = 0x40B128u;
    (void)register_block(block);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);

    (void)deregister_block(block);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK(!kernel_hal_shutdown_registered(block));
    /* Neither half of the pair is an anomaly, so neither counter may move. An
     * unmatched count of 1 here would mean the flag was misread. */
    CHECK_EQ_U32(kernel_hal_shutdown_duplicate_count(), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_unmatched_count(), 0u);
    teardown();
}

/*
 * Register=FALSE really is a deregister, not a second register.
 *
 * MUTATION: make the handler ignore the flag and always call `add_registration`, and
 * this fails. Without it, every test above still passes -- `register_block` would
 * work and the pair test would be the only casualty -- so this states the branch
 * directly.
 */
static void test_the_flag_selects_deregister_not_a_second_register(void)
{
    setup();
    const kernel_guest_ptr first = 0x549650u;
    const kernel_guest_ptr second = 0x40B128u;
    (void)register_block(first);
    (void)register_block(second);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 2u);

    (void)deregister_block(first);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);
    CHECK(!kernel_hal_shutdown_registered(first));
    CHECK(kernel_hal_shutdown_registered(second));
    teardown();
}

/*
 * A nonzero flag that is not 1 is still TRUE. BOOLEAN is a byte and three of the
 * measured sites pass a register rather than a literal.
 */
static void test_any_nonzero_flag_byte_registers(void)
{
    setup();
    const uint32_t args[2] = {GUEST_STATIC_REGISTRATION, 0xFFu};
    (void)call_hal(args, 2u);
    CHECK(kernel_hal_shutdown_registered(GUEST_STATIC_REGISTRATION));
    teardown();
}

/*
 * Registering the same block twice is counted, and held once.
 *
 * MUTATION: delete the duplicate check from `add_registration` and this fails: the
 * count goes to 2 and the duplicate counter stays at 0. On the real kernel the same
 * LIST_ENTRY would be threaded twice and the list corrupted, so this is not
 * pedantry.
 */
static void test_a_duplicate_registration_is_counted_and_held_once(void)
{
    setup();
    (void)register_block(GUEST_STATIC_REGISTRATION);
    (void)register_block(GUEST_STATIC_REGISTRATION);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);
    CHECK_EQ_U32(kernel_hal_shutdown_duplicate_count(), 1u);
    CHECK(captured_contains("already registered"));
    teardown();
}

/*
 * Deregistering something that was never registered is counted, not ignored.
 *
 * MUTATION: delete the `unmatched_count++` and this fails. The counter is the
 * tripwire for a swapped argument pair: with the arguments reversed, EVERY call
 * lands in `remove_registration`, so a run reporting a nonzero unmatched count and a
 * zero duplicate count has that bug and nothing else.
 */
static void test_an_unmatched_deregister_is_counted_and_reported(void)
{
    setup();
    (void)deregister_block(0x40B128u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_unmatched_count(), 1u);
    CHECK(captured_contains("never registered"));
    teardown();
}

/*
 * THE ARITY. A frame one slot short of the second argument must be refused.
 *
 * MUTATION: make the handler read only argument 0 and default the flag, and this
 * fails -- it is the ONLY test here that does, which is exactly why it exists. An
 * arity error does not crash: it makes the dispatcher pop the wrong number of bytes
 * and the guest's esp drifts, with the damage appearing arbitrarily far away.
 */
static void test_two_arguments_are_required(void)
{
    setup();
    const uint32_t args[1] = {GUEST_STATIC_REGISTRATION};
    (void)call_hal(args, 1u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK(captured_contains("could not read its arguments"));
    teardown();
}

/*
 * A null registration block is reported, not registered.
 *
 * MUTATION: delete the `registration == 0u` check and this fails: slot 0 means
 * "free" in the table, so a null registration would be accepted and then be
 * invisible to `shutdown_registered`, which is a silently wrong answer rather than a
 * refusal.
 */
static void test_a_null_registration_is_refused_and_reported(void)
{
    setup();
    const uint32_t args[2] = {0u, 1u};
    (void)call_hal(args, 2u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK(captured_contains("null registration block"));
    teardown();
}

/* A call with no frame at all is reported rather than treated as a null register. */
static void test_a_missing_frame_is_reported(void)
{
    setup();
    (void)kernel_hle_call(ORD_HAL_REGISTER_SHUTDOWN, NULL);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK(captured_contains("no argument frame"));
    teardown();
}

/*
 * The bound is reached loudly. An untracked registration would be a silent one.
 *
 * MUTATION: make the overflow path fall through and overwrite slot 0 instead of
 * refusing, and this fails on the first block having been evicted.
 */
static void test_the_registration_bound_is_refused_not_exceeded(void)
{
    setup();
    const unsigned capacity = kernel_hal_shutdown_capacity();
    CHECK(capacity > 0u);
    /* Addresses are spaced so none is zero and none collides. */
    for (unsigned i = 0u; i < capacity; i++) {
        (void)register_block(0x1000u + i * 0x40u);
    }
    CHECK_EQ_U32(kernel_hal_shutdown_count(), capacity);
    CHECK_EQ_U32(kernel_hal_shutdown_overflow_count(), 0u);

    (void)register_block(0x900000u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), capacity);
    CHECK(!kernel_hal_shutdown_registered(0x900000u));
    CHECK_EQ_U32(kernel_hal_shutdown_overflow_count(), 1u);
    /* And the first block is still there: refusing must not evict. */
    CHECK(kernel_hal_shutdown_registered(0x1000u));
    CHECK(captured_contains("REFUSED"));
    teardown();
}

/* reset() must actually clear, or one test leaks into the next and the suite's own
 * results stop meaning anything. */
static void test_reset_clears_the_set_and_the_counters(void)
{
    setup();
    (void)register_block(GUEST_STATIC_REGISTRATION);
    (void)deregister_block(0x40B128u);
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 1u);
    CHECK_EQ_U32(kernel_hal_shutdown_unmatched_count(), 1u);

    kernel_hal_reset();
    CHECK_EQ_U32(kernel_hal_shutdown_count(), 0u);
    CHECK_EQ_U32(kernel_hal_shutdown_unmatched_count(), 0u);
    CHECK(!kernel_hal_shutdown_registered(GUEST_STATIC_REGISTRATION));
    teardown();
}

/* --- ordinals 253 PhyInitialize and 252 PhyGetLinkState ---------------------
 *
 * There is no PHY. The title's own failure states are the answers: a failing NTSTATUS from
 * PhyInitialize (sign-tested at 0x43AF83) and 0 from PhyGetLinkState (bit 0 clear is "no
 * cable" at all seven wrapper callers). The status literal is the NT value 0xC000000E.
 */

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch, SCRATCH_FRAME, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    frame.stack_limit = scratch + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

static void test_phy_ordinals_are_implemented(void)
{
    setup();
    const kernel_entry *init = kernel_hle_entry(253u);
    const kernel_entry *link = kernel_hle_entry(252u);
    CHECK(init != NULL && link != NULL);
    if (init && link) {
        CHECK_EQ_U32(init->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK_EQ_U32(link->state, KERNEL_ENTRY_IMPLEMENTED);
    }
    teardown();
}

static void test_phy_initialize_fails_with_no_such_device_and_says_so(void)
{
    setup();
    const uint32_t args[2] = {0u, 0u};
    CHECK_EQ_U32(call_ordinal(253u, args, 2u), 0xC000000Eu);
    CHECK(captured_contains("NO Ethernet PHY"));
    CHECK_EQ_U32(kernel_hal_phy_initialize_count(), 1u);
    /* Announced once, counted every time. */
    reset_capture();
    CHECK_EQ_U32(call_ordinal(253u, args, 2u), 0xC000000Eu);
    CHECK(!captured_contains("NO Ethernet PHY"));
    CHECK_EQ_U32(kernel_hal_phy_initialize_count(), 2u);
    teardown();
}

static void test_phy_link_state_reports_no_link_and_says_so(void)
{
    setup();
    const uint32_t args[1] = {0u};
    CHECK_EQ_U32(call_ordinal(252u, args, 1u), 0u);
    CHECK(captured_contains("FABRICATED: no link"));
    CHECK_EQ_U32(kernel_hal_phy_link_query_count(), 1u);
    /* The second measured form: the NIC code passes (arg == 0), a nonzero mode. */
    const uint32_t other[1] = {1u};
    CHECK_EQ_U32(call_ordinal(252u, other, 1u), 0u);
    CHECK_EQ_U32(kernel_hal_phy_link_query_count(), 2u);
    teardown();
}

static void test_phy_arguments_are_required_and_the_refusal_is_visible(void)
{
    setup();
    const uint32_t one[1] = {0u};
    /* PhyInitialize takes two: a frame one slot short is refused with a failure status, not
     * answered as if the arguments were zero. */
    CHECK_EQ_U32(call_ordinal(253u, one, 1u), 0xC000000Du);
    CHECK(captured_contains("could not read its two stack arguments"));
    CHECK_EQ_U32(kernel_hal_phy_initialize_count(), 0u);
    reset_capture();
    CHECK_EQ_U32(call_ordinal(252u, NULL, 0u), 0u);
    CHECK(captured_contains("could not read its stack argument"));
    CHECK_EQ_U32(kernel_hal_phy_link_query_count(), 0u);
    teardown();
}

static void test_phy_counters_reset(void)
{
    setup();
    const uint32_t two[2] = {0u, 0u};
    const uint32_t one[1] = {0u};
    (void)call_ordinal(253u, two, 2u);
    (void)call_ordinal(252u, one, 1u);
    kernel_hal_reset();
    CHECK_EQ_U32(kernel_hal_phy_initialize_count(), 0u);
    CHECK_EQ_U32(kernel_hal_phy_link_query_count(), 0u);
    teardown();
}

int main(void)
{
    printf("kernel HAL HLE tests\n");

    test_registration_makes_the_ordinal_implemented();
    test_an_implemented_ordinal_does_not_report_itself_as_a_stub();
    test_the_guests_own_first_call_records_the_block();
    test_the_pointer_is_argument_zero_and_the_flag_argument_one();
    test_a_register_deregister_pair_leaves_nothing_behind();
    test_the_flag_selects_deregister_not_a_second_register();
    test_any_nonzero_flag_byte_registers();
    test_a_duplicate_registration_is_counted_and_held_once();
    test_an_unmatched_deregister_is_counted_and_reported();
    test_two_arguments_are_required();
    test_a_null_registration_is_refused_and_reported();
    test_a_missing_frame_is_reported();
    test_the_registration_bound_is_refused_not_exceeded();
    test_reset_clears_the_set_and_the_counters();

    test_firmware_return_is_implemented_not_a_stub();
    test_the_routine_argument_reaches_the_sink_unchanged();
    test_the_sink_is_told_how_many_registrations_were_pending();
    test_no_sink_continues_but_says_so_loudly();
    test_a_sink_that_returns_is_reported();
    test_a_second_firmware_return_is_called_out();
    test_a_missing_firmware_frame_is_reported_not_guessed();
    test_reset_clears_the_firmware_counters_but_keeps_the_sink();

    test_phy_ordinals_are_implemented();
    test_phy_initialize_fails_with_no_such_device_and_says_so();
    test_phy_link_state_reports_no_link_and_says_so();
    test_phy_arguments_are_required_and_the_refusal_is_visible();
    test_phy_counters_reset();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
