/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for DbgPrint (ordinal 8), the one __cdecl export this title imports.
 *
 * WHAT IS AT STAKE. Two failure modes, and the tests split along that line:
 *
 *   - THE ARGUMENT CURSOR. DbgPrint is variadic and this title passes it an
 *     ALREADY-FORMATTED message (D3D's sub_003DF990 vsprintfs first), so the
 *     "format" can contain stray '%' bytes the guest never meant as specifiers. A
 *     formatter that consumed an argument for an unrecognised specifier would read
 *     stack bytes that are not arguments -- not a crash, a plausible wrong trace.
 *     So the raw-passthrough rule is pinned: unknown specifiers emit verbatim and
 *     consume NOTHING, checked by interleaving them with known ones.
 *   - GUEST MEMORY. Format and %s pointers come from the guest and can be NULL,
 *     unmapped, or unterminated. Every such case must end in an in-line marker,
 *     never a fault: a debug print must not be able to take the host down.
 *
 * The handler never pops anything -- cleanup is the thunk's `{8u, THUNK_CC_CDECL,
 * 0u, 0u}` row -- so there is nothing stack-disciplinary to test HERE; that zero is
 * pinned in tests/test_arity_oracle.py where the ABI table is parsed.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_dbg.h"
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
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            fflush(stdout);                                                              \
        }                                                                                \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                        \
        const char *check_a = (actual);                                                   \
        const char *check_e = (expected);                                                 \
        if (strcmp(check_a, check_e) != 0) {                                              \
            failures++;                                                                   \
            printf("  FAIL %s:%d  got \"%s\", want \"%s\"\n", __FILE__, __LINE__,         \
                   check_a, check_e);                                                     \
            fflush(stdout);                                                               \
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

/* A capturing fatal hook. It RETURNS, unlike the host's, so each handler's post-hook
 * return value is observable and is part of the tested contract. */
static unsigned fatal_calls;
static unsigned fatal_ordinal;
static char fatal_detail_copy[256];

static void capture_fatal(unsigned ordinal, const char *detail)
{
    fatal_calls++;
    fatal_ordinal = ordinal;
    (void)snprintf(fatal_detail_copy, sizeof(fatal_detail_copy), "%s", detail);
}

static void install_fatal_capture(void)
{
    fatal_calls = 0u;
    fatal_ordinal = 0u;
    fatal_detail_copy[0] = '\0';
    kernel_hle_set_fatal(capture_fatal);
}

#define SCRATCH_BYTES 0x8000u
#define FORMAT_AT 0x1000u
#define STRING_AT 0x2000u

/* An address no test maps. kernel_guest_read_u8 refuses 0 outright; this one is
 * nonzero and outside the single region the tests allocate. */
#define UNMAPPED_AT 0x7FF00010u

static kernel_guest_ptr scratch;

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_dbg_reset();
    CHECK(kernel_dbg_register() == 2u);
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
}

static void teardown(void)
{
    kernel_hle_set_fatal(NULL);
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

static kernel_guest_ptr put_format(const char *text)
{
    kernel_guest_ptr at = scratch + FORMAT_AT;
    if (!kernel_guest_write_bytes(at, text, strlen(text) + 1u)) {
        printf("  FATAL: could not write the format string\n");
        exit(EXIT_FAILURE);
    }
    return at;
}

static kernel_guest_ptr put_string(const char *text)
{
    kernel_guest_ptr at = scratch + STRING_AT;
    if (!kernel_guest_write_bytes(at, text, strlen(text) + 1u)) {
        printf("  FATAL: could not write the %%s argument\n");
        exit(EXIT_FAILURE);
    }
    return at;
}

static void build_frame(kernel_call_frame *frame, const uint32_t *args, unsigned count)
{
    /* Bounded EXACTLY to the return-address slot plus `count` arguments, so a read
     * one slot past the last supplied vararg is refused by stack_limit rather than
     * answered with whatever the scratch region holds. That refusal is what the
     * <arg?> marker tests below are testing. */
    if (!kernel_frame_build(frame, scratch, (count + 1u) * 4u, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
}

static void format_with(char *out, size_t out_size, const char *format,
                        const uint32_t *varargs, unsigned vararg_count)
{
    uint32_t args[8];
    args[0] = put_format(format);
    for (unsigned i = 0; i < vararg_count && i + 1u < 8u; i++) {
        args[i + 1u] = varargs[i];
    }
    kernel_call_frame frame;
    build_frame(&frame, args, vararg_count + 1u);
    (void)kernel_dbg_format(&frame, args[0], out, out_size);
}

/* --- the formatter ---------------------------------------------------------- */

static void test_plain_text_passes_through_unchanged(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    format_with(out, sizeof(out), "D3D: SetRenderState overflow", NULL, 0u);
    CHECK_STR_EQ(out, "D3D: SetRenderState overflow");
    teardown();
}

static void test_the_supported_specifiers_format(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    const uint32_t negative[] = {(uint32_t)-5};
    format_with(out, sizeof(out), "d=%d", negative, 1u);
    CHECK_STR_EQ(out, "d=-5");

    const uint32_t hex[] = {0xBEEFu};
    format_with(out, sizeof(out), "x=%x X=%X", hex, 1u);
    /* ONE vararg for TWO specifiers: the second must say so, not invent a value. */
    CHECK_STR_EQ(out, "x=beef X=<arg?>");

    const uint32_t both[] = {0xBEEFu, 0xBEEFu};
    format_with(out, sizeof(out), "x=%x X=%X", both, 2u);
    CHECK_STR_EQ(out, "x=beef X=BEEF");

    const uint32_t big[] = {0xFFFFFFFFu};
    format_with(out, sizeof(out), "u=%u", big, 1u);
    CHECK_STR_EQ(out, "u=4294967295");

    const uint32_t pointer[] = {0x1234u};
    format_with(out, sizeof(out), "p=%p", pointer, 1u);
    CHECK_STR_EQ(out, "p=0x00001234");

    const uint32_t letter[] = {(uint32_t)'A'};
    format_with(out, sizeof(out), "c=%c", letter, 1u);
    CHECK_STR_EQ(out, "c=A");

    const uint32_t control[] = {7u}; /* BEL: non-printable must not reach the log raw */
    format_with(out, sizeof(out), "c=%c", control, 1u);
    CHECK_STR_EQ(out, "c=.");

    format_with(out, sizeof(out), "100%%", NULL, 0u);
    CHECK_STR_EQ(out, "100%");
    teardown();
}

static void test_s_reads_the_guest_string(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    uint32_t varargs[1];
    varargs[0] = put_string("surface");
    format_with(out, sizeof(out), "name=%s.", varargs, 1u);
    CHECK_STR_EQ(out, "name=surface.");
    teardown();
}

static void test_unknown_specifiers_pass_through_raw_and_consume_no_argument(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    /* THE CURSOR PIN. If "%q" or the width syntax consumed an argument, the %d at
     * the end would miss its 42 and print the wrong value or the marker. */
    const uint32_t one[] = {42u};
    format_with(out, sizeof(out), "%q %08x %-5d end %d", one, 1u);
    CHECK_STR_EQ(out, "%q %08x %-5d end 42");
    teardown();
}

static void test_a_stray_trailing_percent_is_literal(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    format_with(out, sizeof(out), "loading 50%", NULL, 0u);
    CHECK_STR_EQ(out, "loading 50%");
    teardown();
}

static void test_a_null_s_pointer_prints_a_marker_not_a_fault(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    const uint32_t null_ptr[] = {0u};
    format_with(out, sizeof(out), "s=%s", null_ptr, 1u);
    CHECK_STR_EQ(out, "s=(null)");

    const uint32_t bad_ptr[] = {UNMAPPED_AT};
    format_with(out, sizeof(out), "s=%s", bad_ptr, 1u);
    CHECK_STR_EQ(out, "s=<bad %s ptr>");
    teardown();
}

static void test_an_unmapped_format_ends_in_a_marker_not_a_fault(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    uint32_t args[1] = {UNMAPPED_AT};
    kernel_call_frame frame;
    build_frame(&frame, args, 1u);
    (void)kernel_dbg_format(&frame, args[0], out, sizeof(out));
    CHECK_STR_EQ(out, "<format unreadable>");
    teardown();
}

static void test_a_missing_frame_marks_every_argument_unreadable(void)
{
    setup();
    char out[KERNEL_DBG_OUTPUT_MAX];
    kernel_guest_ptr format = put_format("v=%d");
    (void)kernel_dbg_format(NULL, format, out, sizeof(out));
    CHECK_STR_EQ(out, "v=<arg?>");
    teardown();
}

/* --- the handler through the dispatcher -------------------------------------- */

static void test_dispatch_prints_once_per_call_and_returns_success(void)
{
    setup();
    uint32_t args[1];
    args[0] = put_format("D3D says hello\n");
    kernel_call_frame frame;
    build_frame(&frame, args, 1u);

    reset_capture();
    uint32_t result = kernel_hle_call(KERNEL_DBG_ORD_DBG_PRINT, &frame);
    CHECK(result == 0u); /* STATUS_SUCCESS, unconditionally */
    CHECK(captured_contains("DbgPrint: D3D says hello"));
    /* The guest's trailing newline is stripped so the sink's is the only one. */
    CHECK(!captured_contains("hello\n\n"));
    CHECK(kernel_dbg_print_count() == 1u);

    (void)kernel_hle_call(KERNEL_DBG_ORD_DBG_PRINT, &frame);
    CHECK(kernel_dbg_print_count() == 2u);
    teardown();
}

static void test_a_missing_frame_is_refused_and_reported(void)
{
    setup();
    reset_capture();
    uint32_t result = kernel_hle_call(KERNEL_DBG_ORD_DBG_PRINT, NULL);
    CHECK(result == 0u);
    CHECK(kernel_dbg_refused_count() == 1u);
    CHECK(kernel_dbg_print_count() == 0u);
    CHECK(captured_contains("could not read its Format argument"));
    teardown();
}

static void test_a_null_format_is_refused_and_reported(void)
{
    setup();
    uint32_t args[1] = {0u};
    kernel_call_frame frame;
    build_frame(&frame, args, 1u);
    reset_capture();
    (void)kernel_hle_call(KERNEL_DBG_ORD_DBG_PRINT, &frame);
    CHECK(kernel_dbg_refused_count() == 1u);
    CHECK(kernel_dbg_print_count() == 0u);
    CHECK(captured_contains("Format=NULL"));
    teardown();
}

static void test_registration_makes_ordinal_8_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(KERNEL_DBG_ORD_DBG_PRINT);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    teardown();
}

/* --- KeBugCheck (ordinal 95) -------------------------------------------------- */

static uint32_t bug_check_with(const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    build_frame(&frame, args, count);
    return kernel_hle_call(KERNEL_DBG_ORD_KE_BUG_CHECK, &frame);
}

/* Replay of site 0x003CC9AA in sub_003CC97D: `push 0xC0000144`, nothing after but an
 * int3 slide byte. The literal is pinned so a handler that formats anything but the
 * code it read cannot pass. */
static void test_bug_check_replays_the_0xc0000144_site(void)
{
    setup();
    install_fatal_capture();
    const uint32_t args[1] = {0xC0000144u};
    CHECK(bug_check_with(args, 1u) == 0u); /* only the capturing hook sees this return */
    CHECK(fatal_calls == 1u);
    CHECK(fatal_ordinal == 95u);
    CHECK(strstr(fatal_detail_copy, "C0000144") != NULL);
    CHECK(strstr(fatal_detail_copy, "halted itself") != NULL);
    CHECK(captured_contains("FATAL ordinal 95"));
    CHECK(captured_contains("C0000144"));
    teardown();
}

/* Replay of site 0x003CB24B in sub_003CB1BA: `push ebx` with ebx = 0 on the only path
 * that reaches the call. A zero code is a real bugcheck and must still stop. */
static void test_bug_check_replays_the_ebx_zero_site(void)
{
    setup();
    install_fatal_capture();
    const uint32_t args[1] = {0u};
    CHECK(bug_check_with(args, 1u) == 0u);
    CHECK(fatal_calls == 1u);
    CHECK(fatal_ordinal == 95u);
    CHECK(strstr(fatal_detail_copy, "0x00000000") != NULL);
    teardown();
}

/* Frame refusal convention for this export: never silent. A frame that cannot supply
 * the code still goes fatal and says so, because refusing quietly would let the guest
 * run on past a halt. */
static void test_bug_check_with_an_unreadable_frame_still_goes_fatal(void)
{
    setup();
    install_fatal_capture();
    CHECK(kernel_hle_call(KERNEL_DBG_ORD_KE_BUG_CHECK, NULL) == 0u);
    CHECK(fatal_calls == 1u);
    CHECK(fatal_ordinal == 95u);
    CHECK(strstr(fatal_detail_copy, "code unreadable") != NULL);

    install_fatal_capture();
    const uint32_t none[1] = {0u};
    kernel_call_frame frame;
    build_frame(&frame, none, 0u); /* return-address slot only, argument 0 is out of range */
    CHECK(kernel_hle_call(KERNEL_DBG_ORD_KE_BUG_CHECK, &frame) == 0u);
    CHECK(fatal_calls == 1u);
    CHECK(strstr(fatal_detail_copy, "code unreadable") != NULL);
    teardown();
}

static void test_registration_makes_ordinal_95_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(KERNEL_DBG_ORD_KE_BUG_CHECK);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    teardown();
}

int main(void)
{
    printf("kernel DbgPrint HLE tests\n");

    test_plain_text_passes_through_unchanged();
    test_the_supported_specifiers_format();
    test_s_reads_the_guest_string();
    test_unknown_specifiers_pass_through_raw_and_consume_no_argument();
    test_a_stray_trailing_percent_is_literal();
    test_a_null_s_pointer_prints_a_marker_not_a_fault();
    test_an_unmapped_format_ends_in_a_marker_not_a_fault();
    test_a_missing_frame_marks_every_argument_unreadable();
    test_dispatch_prints_once_per_call_and_returns_success();
    test_a_missing_frame_is_refused_and_reported();
    test_a_null_format_is_refused_and_reported();
    test_registration_makes_ordinal_8_implemented();
    test_bug_check_replays_the_0xc0000144_site();
    test_bug_check_replays_the_ebx_zero_site();
    test_bug_check_with_an_unreadable_frame_still_goes_fatal();
    test_registration_makes_ordinal_95_implemented();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
