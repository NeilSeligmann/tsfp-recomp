/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the generated kernel ordinal table. These guard the generator as
 * much as the table: a regeneration that silently dropped entries or shifted
 * indices would break them.
 */

#include "kernel_ordinals.h"

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

static void test_known_ordinals_resolve(void)
{
    /* Spot-checks across the ranges TSFP actually imports. */
    CHECK(strcmp(xbox_kernel_ordinal_name(1), "AvGetSavedDataAddress") == 0);
    CHECK(strcmp(xbox_kernel_ordinal_name(8), "DbgPrint") == 0);
    CHECK(strcmp(xbox_kernel_ordinal_name(14), "ExAllocatePool") == 0);
    /* Ordinal 49 matters specifically: on XDK 5849 it differs from older XDKs,
     * and TSFP imports it. Getting this wrong misroutes engine setup. */
    CHECK(xbox_kernel_ordinal_name(49) != NULL);
}

static void test_unknown_ordinals_return_null(void)
{
    CHECK(xbox_kernel_ordinal_name(0) == NULL);
    CHECK(xbox_kernel_ordinal_name(XBOX_KERNEL_ORDINAL_MAX + 1) == NULL);
    CHECK(xbox_kernel_ordinal_name(100000) == NULL);
    /* 367-373 are genuinely absent from the real kernel export table. */
    for (unsigned ordinal = 367; ordinal <= 373; ordinal++) {
        CHECK(xbox_kernel_ordinal_name(ordinal) == NULL);
    }
}

static void test_table_size(void)
{
    CHECK(xbox_kernel_ordinal_count() == 371);
    CHECK(XBOX_KERNEL_ORDINAL_MAX == 378);
}

static void test_no_name_is_empty(void)
{
    for (unsigned ordinal = 0; ordinal <= XBOX_KERNEL_ORDINAL_MAX; ordinal++) {
        const char *name = xbox_kernel_ordinal_name(ordinal);
        if (name) {
            CHECK(name[0] != '\0');
        }
    }
}

int main(void)
{
    printf("kernel ordinal tests\n");
    test_known_ordinals_resolve();
    test_unknown_ordinals_return_null();
    test_table_size();
    test_no_name_is_empty();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
