/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the XDK-surface-to-D3D8-table adapter. The adapter's whole job is
 * to filter by section and copy fields one at a time instead of casting or
 * memcpy-ing a struct whose layout does not match, so most of these tests
 * exist to catch exactly that kind of reinterpretation bug.
 *
 * This file uses its own small fake multi-section source array and never
 * refers to the generated xdk_surface symbol, so it builds and passes in a
 * fresh clone that has not run tools/gen_d3d8_surface.py yet.
 */

#include "d3d8_surface_adapter.h"

#include "d3d8_hle.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                          \
    } while (0)

/* A mixed-section fake surface, standing in for the generated one. D3D rows
 * are deliberately scattered among XNET and DSOUND rows rather than kept
 * contiguous, so a selection that merely copies a contiguous run would fail
 * the ordering checks below. */
static const d3d8_xdk_row mixed_source[] = {
    {0x00500010u, "XNET", "XNetStartup", 7u},
    {0x00400020u, "D3D", "D3DDevice_SetRenderState", 93u},
    {0x00600030u, "DSOUND", NULL, 4u},
    {0x00400040u, "D3D", NULL, 41u},
    {0x00500060u, "XNET", NULL, 2u},
    {0x00400050u, "D3D", "D3DDevice_Present", 12u},
};
static const size_t mixed_source_count =
    sizeof(mixed_source) / sizeof(mixed_source[0]);

/* Expected D3D rows from mixed_source, in source order. */
static const uint32_t mixed_d3d_addresses[] = {0x00400020u, 0x00400040u, 0x00400050u};
static const size_t mixed_d3d_count =
    sizeof(mixed_d3d_addresses) / sizeof(mixed_d3d_addresses[0]);

static void test_selects_only_the_requested_section(void)
{
    size_t out_count = 123;
    d3d8_surface_entry *selected =
        d3d8_surface_select(mixed_source, mixed_source_count, D3D8_SECTION_D3D, &out_count);

    CHECK(selected != NULL);
    CHECK(out_count == mixed_d3d_count);
    if (selected) {
        for (size_t i = 0; i < mixed_d3d_count && i < out_count; i++) {
            CHECK(selected[i].address == mixed_d3d_addresses[i]);
        }
    }
    free(selected);
}

/*
 * The point of this test: xdk_surface_entry is {address, section, name, sites}
 * and d3d8_surface_entry is {address, name, sites}. The `section` field sits
 * exactly where a naive memcpy or a pointer cast of the source row would make
 * `name` land, and the real `name` pointer would make `sites` land on garbage.
 * The source row below gives `section`, `name` and `sites` all distinct,
 * non-overlapping values, so any reinterpretation bug produces a value that
 * cannot coincidentally match the correct one.
 */
static void test_fields_are_copied_not_reinterpreted(void)
{
    static const char unnamed_section[] = "D3D";
    static const d3d8_xdk_row source[] = {
        {0x00401000u, "D3D", "D3DDevice_Clear", 777u},
        {0x00402000u, "XNET", "XNetCleanup", 3u},
        /* NULL name: must survive untouched, not get confused with section. */
        {0x00403000u, "D3D", NULL, 5u},
    };
    static const size_t source_count = sizeof(source) / sizeof(source[0]);
    (void)unnamed_section;

    size_t out_count = 0;
    d3d8_surface_entry *selected =
        d3d8_surface_select(source, source_count, "D3D", &out_count);

    CHECK(selected != NULL);
    CHECK(out_count == 2u);
    if (selected && out_count == 2u) {
        CHECK(selected[0].address == source[0].address);
        CHECK(selected[0].name == source[0].name);
        CHECK(selected[0].sites == source[0].sites);
        /* If `name` had been read from the `section` field instead, this
         * pointer would equal source[0].section rather than source[0].name. */
        CHECK(selected[0].name != source[0].section);

        CHECK(selected[1].address == source[2].address);
        CHECK(selected[1].name == source[2].name);
        CHECK(selected[1].sites == source[2].sites);
    }
    free(selected);
}

static void test_exact_section_match_only(void)
{
    static const d3d8_xdk_row source[] = {
        {0x00410000u, "D3DX", "NotReal", 9u},
        {0x00410010u, "D3", "AlsoNotReal", 9u},
        {0x00410020u, "D3D", "D3DDevice_Present", 12u},
    };
    static const size_t source_count = sizeof(source) / sizeof(source[0]);

    size_t out_count = 0;
    d3d8_surface_entry *selected =
        d3d8_surface_select(source, source_count, "D3D", &out_count);

    CHECK(selected != NULL);
    CHECK(out_count == 1u);
    if (selected && out_count == 1u) {
        CHECK(selected[0].address == source[2].address);
    }
    free(selected);
}

static void test_null_section_row_is_skipped_safely(void)
{
    static const d3d8_xdk_row source[] = {
        {0x00420000u, NULL, "Mystery", 1u},
        {0x00420010u, "D3D", "D3DDevice_Present", 12u},
    };
    static const size_t source_count = sizeof(source) / sizeof(source[0]);

    size_t out_count = 0;
    d3d8_surface_entry *selected =
        d3d8_surface_select(source, source_count, "D3D", &out_count);

    CHECK(selected != NULL);
    CHECK(out_count == 1u);
    if (selected && out_count == 1u) {
        CHECK(selected[0].address == source[1].address);
    }
    free(selected);
}

static void test_rejects_bad_arguments(void)
{
    size_t out_count;

    out_count = 123;
    CHECK(d3d8_surface_select(NULL, mixed_source_count, D3D8_SECTION_D3D, &out_count) == NULL);
    CHECK(out_count == 0u);

    out_count = 123;
    CHECK(d3d8_surface_select(mixed_source, 0, D3D8_SECTION_D3D, &out_count) == NULL);
    CHECK(out_count == 0u);

    out_count = 123;
    CHECK(d3d8_surface_select(mixed_source, mixed_source_count, NULL, &out_count) == NULL);
    CHECK(out_count == 0u);

    /* No out_count to inspect here: a NULL out_count is exactly the case where
     * there is nowhere safe to report the zero, so only the return matters. */
    CHECK(d3d8_surface_select(mixed_source, mixed_source_count, D3D8_SECTION_D3D, NULL) == NULL);
}

static void test_no_match_returns_null(void)
{
    size_t out_count = 123;
    d3d8_surface_entry *selected =
        d3d8_surface_select(mixed_source, mixed_source_count, "XGRPH", &out_count);

    CHECK(selected == NULL);
    CHECK(out_count == 0u);
}

static void test_adopt_feeds_the_dispatcher(void)
{
    d3d8_hle_shutdown();

    CHECK(d3d8_surface_adopt(mixed_source, mixed_source_count, D3D8_SECTION_D3D));
    CHECK(d3d8_hle_count() == mixed_d3d_count);

    for (size_t i = 0; i < mixed_d3d_count; i++) {
        const d3d8_entry *entry = d3d8_hle_entry(mixed_d3d_addresses[i]);
        CHECK(entry != NULL);
        if (entry) {
            CHECK(entry->address == mixed_d3d_addresses[i]);
        }
    }

    /* An address from a non-selected section must not resolve. */
    CHECK(d3d8_hle_entry(0x00500010u) == NULL);

    d3d8_hle_shutdown();
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case;

int main(void)
{
    static const test_case cases[] = {
        {"selects_only_the_requested_section", test_selects_only_the_requested_section},
        {"fields_are_copied_not_reinterpreted", test_fields_are_copied_not_reinterpreted},
        {"exact_section_match_only", test_exact_section_match_only},
        {"null_section_row_is_skipped_safely", test_null_section_row_is_skipped_safely},
        {"rejects_bad_arguments", test_rejects_bad_arguments},
        {"no_match_returns_null", test_no_match_returns_null},
        {"adopt_feeds_the_dispatcher", test_adopt_feeds_the_dispatcher},
    };

    printf("d3d8 surface adapter tests\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = failures;
        cases[i].run();
        printf("  %-56s %s\n", cases[i].name, failures == before ? "ok" : "FAILED");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
