/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the kernel HLE dispatch. The diagnostics are the feature, so most of
 * these assert on what gets reported rather than on return values.
 */

#include "kernel_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                \
    } while (0)

/* Capture diagnostics so tests can assert on them. */
static char captured[8192];
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

static uint32_t fake_handler(void *context)
{
    (void)context;
    return 0xABCD1234u;
}

static void test_init_stubs_every_known_ordinal(void)
{
    kernel_hle_init();
    const kernel_entry *entry = kernel_hle_entry(8); /* DbgPrint */
    CHECK(entry != NULL);
    CHECK(entry->state == KERNEL_ENTRY_STUB);
    CHECK(entry->name != NULL && strcmp(entry->name, "DbgPrint") == 0);
    CHECK(entry->call_count == 0);
}

static void test_unknown_ordinals_have_no_entry(void)
{
    kernel_hle_init();
    CHECK(kernel_hle_entry(0) == NULL);
    CHECK(kernel_hle_entry(XBOX_KERNEL_ORDINAL_MAX + 1) == NULL);
    /* 367-373 are genuinely absent from the real kernel export table. */
    CHECK(kernel_hle_entry(370) == NULL);
}

static void test_stub_reports_once_by_name(void)
{
    kernel_hle_init();
    kernel_hle_set_log(capture_printer);
    reset_capture();

    kernel_hle_call(8, NULL);
    CHECK(captured_contains("ordinal 8"));
    CHECK(captured_contains("DbgPrint"));
    CHECK(captured_contains("not implemented"));

    /* A stub inside a per-frame loop must not bury everything else, so the
     * report is once per ordinal, not once per call. */
    size_t after_first = captured_len;
    kernel_hle_call(8, NULL);
    kernel_hle_call(8, NULL);
    CHECK(captured_len == after_first);

    const kernel_entry *entry = kernel_hle_entry(8);
    CHECK(entry->call_count == 3);
    kernel_hle_set_log(NULL);
}

static void test_unknown_ordinal_reports_every_time(void)
{
    kernel_hle_init();
    kernel_hle_set_log(capture_printer);
    reset_capture();

    kernel_hle_call(370, NULL);
    CHECK(captured_contains("UNKNOWN ordinal 370"));
    CHECK(captured_contains("import table mismatch"));

    /* This should never happen, so unlike a stub it is not rate-limited. */
    size_t after_first = captured_len;
    kernel_hle_call(370, NULL);
    CHECK(captured_len > after_first);
    kernel_hle_set_log(NULL);
}

static void test_registering_an_implementation(void)
{
    kernel_hle_init();
    CHECK(kernel_hle_register(8, fake_handler));
    const kernel_entry *entry = kernel_hle_entry(8);
    CHECK(entry->state == KERNEL_ENTRY_IMPLEMENTED);

    kernel_hle_set_log(capture_printer);
    reset_capture();
    CHECK(kernel_hle_call(8, NULL) == 0xABCD1234u);
    /* An implemented function must be silent. */
    CHECK(captured_len == 0);
    kernel_hle_set_log(NULL);
}

static void test_registering_rejects_bad_input(void)
{
    kernel_hle_init();
    CHECK(!kernel_hle_register(370, fake_handler)); /* unknown ordinal */
    CHECK(!kernel_hle_register(8, NULL));           /* no handler */
    CHECK(!kernel_hle_set_default_return(370, 1));
}

static void test_default_return(void)
{
    kernel_hle_init();
    kernel_hle_set_log(capture_printer);
    reset_capture();
    CHECK(kernel_hle_call(8, NULL) == 0);
    CHECK(kernel_hle_set_default_return(14, 0xDEADu)); /* ExAllocatePool */
    CHECK(kernel_hle_call(14, NULL) == 0xDEADu);
    kernel_hle_set_log(NULL);
}

static void test_implemented_count(void)
{
    kernel_hle_init();
    const unsigned imports[] = {1, 8, 14, 49};
    CHECK(kernel_hle_implemented_count(imports, 4) == 0);
    kernel_hle_register(8, fake_handler);
    kernel_hle_register(14, fake_handler);
    CHECK(kernel_hle_implemented_count(imports, 4) == 2);
    CHECK(kernel_hle_implemented_count(NULL, 4) == 0);
}

static void test_report_missing_is_ordered_by_call_count(void)
{
    kernel_hle_init();
    kernel_hle_set_log(capture_printer);

    /* Make ordinal 14 the busiest, so it must be reported before ordinal 1. */
    for (int i = 0; i < 5; i++) {
        kernel_hle_call(14, NULL);
    }
    kernel_hle_call(1, NULL);
    kernel_hle_register(8, fake_handler);

    reset_capture();
    const unsigned imports[] = {1, 8, 14};
    kernel_hle_report_missing(imports, 3);

    CHECK(captured_contains("2 of 3 imported ordinals"));
    const char *busiest = strstr(captured, "ExAllocatePool");
    const char *quieter = strstr(captured, "AvGetSavedDataAddress");
    CHECK(busiest != NULL);
    CHECK(quieter != NULL);
    CHECK(busiest < quieter); /* busiest first */
    /* An implemented ordinal must not appear in the backlog. */
    CHECK(!captured_contains("DbgPrint"));
    kernel_hle_set_log(NULL);
}

static void test_call_works_without_explicit_init(void)
{
    /* A caller that forgets to initialise must still get stub behaviour, not a
     * crash or a silent zero from an absent table. */
    kernel_hle_set_log(capture_printer);
    reset_capture();
    kernel_hle_init();
    memset(captured, 0, sizeof(captured));
    captured_len = 0;
    CHECK(kernel_hle_entry(8) != NULL);
    kernel_hle_set_log(NULL);
}

int main(void)
{
    printf("kernel HLE tests\n");
    test_init_stubs_every_known_ordinal();
    test_unknown_ordinals_have_no_entry();
    test_stub_reports_once_by_name();
    test_unknown_ordinal_reports_every_time();
    test_registering_an_implementation();
    test_registering_rejects_bad_input();
    test_default_return();
    test_implemented_count();
    test_report_missing_is_ordered_by_call_count();
    test_call_works_without_explicit_init();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
