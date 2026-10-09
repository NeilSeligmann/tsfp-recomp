/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ExQueryNonVolatileSetting (ordinal 24): the console configuration query.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. THE ARITY. 5 stack arguments, and the measured table has NO row for this
 *      ordinal at all -- every real call site reaches it through an import jump stub
 *      the measurement cannot see through, so `callsites.py` classifies it as a DATA
 *      export. The count is entirely hand-derived, which makes it the weakest link in
 *      the chain and therefore the thing tested hardest: a frame one slot short must
 *      be refused, because our handler is the __stdcall callee and a wrong count
 *      desyncs the guest's esp permanently.
 *   2. THE ARGUMENT ORDER. (ValueIndex, Type*, Value, ValueLength, ResultLength*).
 *      Type and Value are two out-pointers next to each other, so a swap writes the
 *      type code where the setting belongs and vice versa -- and a test that only
 *      checked the status would miss it entirely. Every test below that writes
 *      anything asserts on BOTH slots.
 *   3. A NULL ResultLength. Six of the twelve measured sites pass a literal 0 for
 *      it. A handler that dereferenced it unconditionally would fault on the guest's
 *      very first query, so that case is a test rather than a code comment.
 *   4. THE FABRICATION BEING SILENT. We hold no EEPROM data. The run can proceed on
 *      zeros, but only if it is impossible to do so without saying so.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Arguments come from a frame built
 * in scratch guest memory and the out-parameters are read back from the same scratch.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "kernel_config.h"

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
        }                                                                                 \
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

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define ORD_EX_QUERY_NON_VOLATILE 24u

#define SCRATCH_BYTES 0x2000u
/* The frame lives at the bottom of the scratch region and the out-parameters well
 * above it, so a handler that wrote through the wrong pointer could not accidentally
 * land on a plausible-looking slot. */
#define FRAME_BYTES 0x100u
#define TYPE_OFFSET 0x400u
#define VALUE_OFFSET 0x500u
#define RESULT_OFFSET 0x600u

static kernel_guest_ptr scratch;

/* A sentinel that is not zero and not any type code or length this module reports, so
 * "untouched" is never confusable with "written". Asserting a zero over memory that
 * started at zero would pass against a handler that wrote nothing at all. */
#define SENTINEL_BYTE 0xA5u

static kernel_guest_ptr type_slot(void)
{
    return scratch + TYPE_OFFSET;
}

static kernel_guest_ptr value_slot(void)
{
    return scratch + VALUE_OFFSET;
}

static kernel_guest_ptr result_slot(void)
{
    return scratch + RESULT_OFFSET;
}

/* Fill the three out-parameter areas with a byte pattern, so every assertion below is
 * against a KNOWN prior value. Asserting a zero result over memory that started at
 * zero would pass against a handler that wrote nothing at all. */
static void poison_out_slots(void)
{
    /* Spans TYPE_OFFSET through the end of the result slot and then some. The first
     * version of this stopped at RESULT_OFFSET exclusive and left the result slot
     * unpoisoned, so the "writes nothing" assertion on it was comparing against
     * whatever the allocator left there -- a check that would have passed against a
     * handler that wrote a zero. The span is derived from the offsets rather than
     * written as a literal for that reason. */
    for (uint32_t i = 0u; i < (RESULT_OFFSET - TYPE_OFFSET) + 0x100u; i++) {
        if (!kernel_guest_write_u8(scratch + TYPE_OFFSET + i, SENTINEL_BYTE)) {
            printf("FATAL could not poison the out slots\n");
            exit(EXIT_FAILURE);
        }
    }
}

static uint32_t read_u32_at(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

static bool bytes_at_are(kernel_guest_ptr address, const uint8_t *expected, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        uint8_t actual = 0u;
        if (!kernel_guest_read_u8(address + i, &actual) || actual != expected[i]) {
            return false;
        }
    }
    return true;
}

static bool all_bytes_at_are(kernel_guest_ptr address, uint8_t expected, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        uint8_t actual = 0u;
        if (!kernel_guest_read_u8(address + i, &actual) || actual != expected) {
            return false;
        }
    }
    return true;
}

static void setup(void)
{
    kernel_hle_init();
    kernel_config_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_config_register(), 1u);
    kernel_hle_set_log(capture_printer);
    captured[0] = '\0';
    captured_len = 0u;

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
    poison_out_slots();
}

static void teardown(void)
{
    kernel_config_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

/* `count` is a parameter rather than a constant 5 so the arity test can hand over a
 * frame that is one slot short. */
static uint32_t call_query(const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch, FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamped to exactly the slots that were built, which is what makes a read past
     * the last argument FAIL instead of returning whatever follows it. */
    frame.stack_limit = scratch + (count + 1u) * 4u;
    return kernel_hle_call(ORD_EX_QUERY_NON_VOLATILE, &frame);
}

/* The shape six of the twelve measured sites use: a 4-byte setting into two stack
 * locals, with a literal NULL ResultLength. */
static uint32_t query_4byte(uint32_t index)
{
    const uint32_t args[5] = {index, type_slot(), value_slot(), 4u, 0u};
    return call_query(args, 5u);
}

/* The shape the three longer sites use: a real ResultLength out-pointer. */
static uint32_t query_with_result_length(uint32_t index, uint32_t length)
{
    const uint32_t args[5] = {index, type_slot(), value_slot(), length, result_slot()};
    return call_query(args, 5u);
}

/* ------------------------------------------------------------------------- */

/*
 * The ordinal stops being a stub, which is the line that moves the bring-up on.
 *
 * MUTATION: remove the binding from `kernel_config_register`'s table and this fails.
 */
static void test_registration_makes_the_ordinal_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_EX_QUERY_NON_VOLATILE);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK(entry->name != NULL &&
              strcmp(entry->name, "ExQueryNonVolatileSetting") == 0);
    }
    teardown();
}

static void test_an_implemented_ordinal_does_not_report_itself_as_a_stub(void)
{
    setup();
    (void)query_4byte(7u);
    CHECK(!captured_contains("not implemented"));
    teardown();
}

/*
 * THE ARGUMENT ORDER, with a stored value so both out-slots carry distinguishable
 * data.
 *
 * MUTATION: swap the Type and Value argument indices in the handler and this fails --
 * the setting bytes land in the Type slot and the type code in the Value slot. A test
 * that only asserted STATUS_SUCCESS would pass against that swap, which is why both
 * slots are checked here and in every other writing test below.
 */
static void test_the_five_arguments_are_in_the_measured_order(void)
{
    setup();
    const uint8_t stored[4] = {0xEFu, 0xBEu, 0xADu, 0xDEu};
    CHECK(kernel_config_set_setting(7u, stored, sizeof(stored)));

    CHECK_EQ_U32(query_with_result_length(7u, 4u), STATUS_SUCCESS);

    /* Value: the stored bytes, at argument 2's pointer. */
    CHECK(bytes_at_are(value_slot(), stored, sizeof(stored)));
    /* Type: a 4-byte setting, so the DWORD code -- which is 4, deliberately NOT a
     * value that could be confused with the stored bytes. */
    CHECK_EQ_U32(read_u32_at(type_slot()), 4u);
    /* ResultLength: how many bytes were produced. */
    CHECK_EQ_U32(read_u32_at(result_slot()), 4u);
    /* Served from the store, not fabricated. */
    CHECK_EQ_U32(kernel_config_served_count(), 1u);
    CHECK_EQ_U32(kernel_config_fabricated_count(), 0u);
    teardown();
}

/*
 * THE ARITY. A frame one slot short of the fifth argument must be refused.
 *
 * MUTATION: read 4 arguments instead of 5 and default ResultLength, and this is the
 * ONLY test here that fails -- which is exactly why it exists. An arity error does
 * not crash; it makes the dispatcher pop the wrong number of bytes and the guest's
 * esp drifts, with the damage surfacing arbitrarily far away.
 */
static void test_five_arguments_are_required(void)
{
    setup();
    const uint32_t args[4] = {7u, type_slot(), value_slot(), 4u};
    CHECK_EQ_U32(call_query(args, 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 4"));
    /* And nothing was written: a refusal must not half-complete. */
    CHECK(all_bytes_at_are(value_slot(), SENTINEL_BYTE, 4u));
    CHECK(all_bytes_at_are(type_slot(), SENTINEL_BYTE, 4u));
    teardown();
}

/*
 * A NULL ResultLength is tolerated, because six of the twelve measured sites pass a
 * literal 0 for it.
 *
 * MUTATION: drop the `result_length_out != 0u` guard and this fails -- a write to
 * guest address 0 is refused by `kernel_guest_at`, so the handler would return
 * STATUS_INVALID_PARAMETER for the shape the guest uses most.
 */
static void test_a_null_result_length_is_tolerated(void)
{
    setup();
    const uint8_t stored[4] = {1u, 2u, 3u, 4u};
    CHECK(kernel_config_set_setting(8u, stored, sizeof(stored)));
    CHECK_EQ_U32(query_4byte(8u), STATUS_SUCCESS);
    CHECK(bytes_at_are(value_slot(), stored, sizeof(stored)));
    CHECK_EQ_U32(read_u32_at(type_slot()), 4u);
    teardown();
}

/*
 * An index with no stored value is fabricated as zeros, over the WHOLE requested
 * length, and announces itself.
 *
 * MUTATION: zero only the first 4 bytes rather than `value_length`, and this fails on
 * a 0x10-byte query -- the guest would read our zeros for one DWORD and its own
 * uninitialised stack for the other three, which is worse than a known-wrong value
 * because it is not reproducible.
 *
 * MUTATION: drop the FABRICATED log line, and this fails too. A made-up value that
 * does not announce itself is the failure this whole module is arranged to avoid.
 */
static void test_an_unheld_setting_is_fabricated_as_zeros_and_announced(void)
{
    setup();
    CHECK_EQ_U32(kernel_config_setting_count(), 0u);

    /* The 0x10-byte shape from the site at 0x00415CCE. */
    CHECK_EQ_U32(query_with_result_length(0x102u, 0x10u), STATUS_SUCCESS);

    CHECK(all_bytes_at_are(value_slot(), 0u, 0x10u));
    /* Not 4 bytes, so the binary type code rather than the DWORD one. */
    CHECK_EQ_U32(read_u32_at(type_slot()), 3u);
    CHECK_EQ_U32(read_u32_at(result_slot()), 0x10u);
    CHECK_EQ_U32(kernel_config_fabricated_count(), 1u);
    CHECK_EQ_U32(kernel_config_served_count(), 0u);
    CHECK(captured_contains("FABRICATED"));
    teardown();
}

/*
 * The FAIL policy refuses and writes NOTHING.
 *
 * MUTATION: let the FAIL path fall through to the writes, and this fails on the
 * sentinels having been overwritten. A refusal that still scribbles on the guest's
 * buffer is the worst of both answers.
 */
static void test_the_fail_policy_refuses_and_writes_nothing(void)
{
    setup();
    kernel_config_set_unknown_policy(KERNEL_CONFIG_UNKNOWN_FAIL);

    CHECK_EQ_U32(query_with_result_length(0x104u, 4u),
                 KERNEL_CONFIG_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(all_bytes_at_are(value_slot(), SENTINEL_BYTE, 4u));
    CHECK(all_bytes_at_are(type_slot(), SENTINEL_BYTE, 4u));
    CHECK(all_bytes_at_are(result_slot(), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(kernel_config_refused_count(), 1u);
    CHECK_EQ_U32(kernel_config_fabricated_count(), 0u);
    CHECK(captured_contains("REFUSED"));

    /* The policy is a choice, not a one-way door: a stored value is still served. */
    const uint8_t stored[4] = {9u, 0u, 0u, 0u};
    CHECK(kernel_config_set_setting(0x104u, stored, sizeof(stored)));
    CHECK_EQ_U32(query_with_result_length(0x104u, 4u), STATUS_SUCCESS);
    CHECK(bytes_at_are(value_slot(), stored, sizeof(stored)));
    teardown();
}

/*
 * A buffer smaller than the stored setting is a buffer error, with nothing written.
 *
 * MUTATION: replace the `stored->length > value_length` test with `>=`, or let it
 * copy `value_length` bytes anyway, and this fails. Copying the stored length into a
 * shorter buffer would overrun the guest's stack local, which is the class of bug
 * that `docs/guest-structs.md` records IO_STATUS_BLOCK being overrun by 8 bytes for.
 */
static void test_a_buffer_too_small_for_the_stored_setting_is_refused(void)
{
    setup();
    const uint8_t stored[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    CHECK(kernel_config_set_setting(0x100u, stored, sizeof(stored)));

    CHECK_EQ_U32(query_with_result_length(0x100u, 4u),
                 KERNEL_CONFIG_STATUS_BUFFER_TOO_SMALL);
    CHECK(all_bytes_at_are(value_slot(), SENTINEL_BYTE, 8u));
    CHECK(all_bytes_at_are(type_slot(), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(kernel_config_served_count(), 0u);

    /* The same setting with a big enough buffer is served, and only its own length is
     * written -- the byte after must keep its sentinel. */
    CHECK_EQ_U32(query_with_result_length(0x100u, 0xCu), STATUS_SUCCESS);
    CHECK(bytes_at_are(value_slot(), stored, sizeof(stored)));
    CHECK(all_bytes_at_are(value_slot() + sizeof(stored), SENTINEL_BYTE, 1u));
    CHECK_EQ_U32(read_u32_at(result_slot()), 8u);
    teardown();
}

/* A query with no buffer at all is a parameter error, not a zero-length success. */
static void test_a_missing_value_buffer_is_refused(void)
{
    setup();
    const uint32_t no_pointer[5] = {7u, type_slot(), 0u, 4u, 0u};
    CHECK_EQ_U32(call_query(no_pointer, 5u), STATUS_INVALID_PARAMETER);
    const uint32_t no_length[5] = {7u, type_slot(), value_slot(), 0u, 0u};
    CHECK_EQ_U32(call_query(no_length, 5u), STATUS_INVALID_PARAMETER);
    CHECK(all_bytes_at_are(value_slot(), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(kernel_config_fabricated_count(), 0u);
    teardown();
}

/*
 * A length above what this module holds is refused rather than truncated.
 *
 * MUTATION: delete the `value_length > KERNEL_CONFIG_VALUE_MAX` check and the
 * fabrication path would zero an arbitrary span of guest memory on the guest's say-so.
 */
static void test_a_length_above_the_module_bound_is_refused(void)
{
    setup();
    const uint32_t args[5] = {0xFFFFu, type_slot(), value_slot(),
                              KERNEL_CONFIG_VALUE_MAX + 1u, result_slot()};
    CHECK_EQ_U32(call_query(args, 5u), STATUS_INVALID_PARAMETER);
    CHECK(all_bytes_at_are(value_slot(), SENTINEL_BYTE, 4u));
    /* And the largest length any measured site asks for is still accepted. */
    CHECK_EQ_U32(query_with_result_length(0xFFFFu, KERNEL_CONFIG_VALUE_MAX),
                 STATUS_SUCCESS);
    CHECK(all_bytes_at_are(value_slot(), 0u, KERNEL_CONFIG_VALUE_MAX));
    teardown();
}

/*
 * The distinct indices the title asks for are RECORDED, oldest first, and a repeat is
 * not a second entry.
 *
 * MUTATION: delete the early return from `record_query_locked` and this fails -- the
 * log becomes a per-call tally, and the one thing a bring-up run is for is knowing
 * WHICH settings the title wants.
 */
static void test_the_queried_indices_are_recorded_once_each_in_order(void)
{
    setup();
    (void)query_4byte(7u);
    (void)query_4byte(0x103u);
    (void)query_4byte(7u);
    (void)query_4byte(0xAu);

    uint32_t indices[8];
    memset(indices, 0, sizeof(indices));
    CHECK_EQ_U32(kernel_config_queried_indices(indices, 8u), 3u);
    CHECK_EQ_U32(indices[0], 7u);
    CHECK_EQ_U32(indices[1], 0x103u);
    CHECK_EQ_U32(indices[2], 0xAu);
    /* Four calls, three distinct indices: the fabrication count is per CALL. */
    CHECK_EQ_U32(kernel_config_fabricated_count(), 4u);
    teardown();
}

/* A call with no frame at all is reported rather than treated as index 0. */
static void test_a_missing_frame_is_reported(void)
{
    setup();
    CHECK_EQ_U32(kernel_hle_call(ORD_EX_QUERY_NON_VOLATILE, NULL),
                 STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("no argument frame"));
    CHECK_EQ_U32(kernel_config_fabricated_count(), 0u);
    teardown();
}

/* The store's own refusals, so a caller cannot install a setting that would later
 * overrun the fixed buffer. */
static void test_the_store_refuses_what_it_cannot_hold(void)
{
    setup();
    static const uint8_t bytes[4] = {0u, 0u, 0u, 0u};
    CHECK(!kernel_config_set_setting(1u, bytes, KERNEL_CONFIG_VALUE_MAX + 1u));
    CHECK(!kernel_config_set_setting(1u, NULL, 4u));
    CHECK_EQ_U32(kernel_config_setting_count(), 0u);
    /* A zero-length setting with no bytes is legitimate: it is a setting that exists
     * and is empty, which is not the same as one we do not hold. */
    CHECK(kernel_config_set_setting(1u, NULL, 0u));
    CHECK_EQ_U32(kernel_config_setting_count(), 1u);
    /* And storing the same index twice replaces rather than duplicating. */
    CHECK(kernel_config_set_setting(1u, bytes, 4u));
    CHECK_EQ_U32(kernel_config_setting_count(), 1u);
    teardown();
}

/* reset() must clear, or one test leaks into the next and the suite stops meaning
 * anything -- including the policy, which is global state. */
static void test_reset_clears_the_store_the_counters_and_the_policy(void)
{
    setup();
    const uint8_t stored[4] = {1u, 0u, 0u, 0u};
    CHECK(kernel_config_set_setting(7u, stored, sizeof(stored)));
    (void)query_4byte(9u);
    kernel_config_set_unknown_policy(KERNEL_CONFIG_UNKNOWN_FAIL);
    CHECK_EQ_U32(kernel_config_setting_count(), 1u);
    CHECK_EQ_U32(kernel_config_fabricated_count(), 1u);

    kernel_config_reset();
    CHECK_EQ_U32(kernel_config_setting_count(), 0u);
    CHECK_EQ_U32(kernel_config_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_config_queried_indices(NULL, 0u), 0u);
    /* Back to the ZEROS policy: a reset that left FAIL installed would silently
     * change what every later test is testing. */
    CHECK_EQ_U32(query_4byte(9u), STATUS_SUCCESS);
    teardown();
}

int main(void)
{
    printf("kernel config (ExQueryNonVolatileSetting) tests\n");

    test_registration_makes_the_ordinal_implemented();
    test_an_implemented_ordinal_does_not_report_itself_as_a_stub();
    test_the_five_arguments_are_in_the_measured_order();
    test_five_arguments_are_required();
    test_a_null_result_length_is_tolerated();
    test_an_unheld_setting_is_fabricated_as_zeros_and_announced();
    test_the_fail_policy_refuses_and_writes_nothing();
    test_a_buffer_too_small_for_the_stored_setting_is_refused();
    test_a_missing_value_buffer_is_refused();
    test_a_length_above_the_module_bound_is_refused();
    test_the_queried_indices_are_recorded_once_each_in_order();
    test_a_missing_frame_is_reported();
    test_the_store_refuses_what_it_cannot_hold();
    test_reset_clears_the_store_the_counters_and_the_policy();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
