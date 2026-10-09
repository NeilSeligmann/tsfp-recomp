/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the generated XDK call surface table. These guard the generator as
 * much as the data: a regeneration that lost entries or corrupted a section name
 * would break them.
 */

#include "xdk_surface.h"

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

static void test_table_is_populated(void)
{
    /* EXACT, not `> 200`: a loose bound passes for a table that lost 35 rows. 236 is the
     * decoded surface, and it must equal the sum of the per-section counts asserted below,
     * which is what makes an omitted section (XGRPH was unpinned) visible. */
    CHECK(XDK_SURFACE_COUNT == 236);
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++) {
        CHECK(xdk_surface[i].address != 0);
        CHECK(xdk_surface[i].section != NULL);
        CHECK(xdk_surface[i].sites > 0);
    }
}

static void test_measured_section_counts(void)
{
    /* These are the measured surfaces the design depends on. The D3D figure in
     * particular is why the graphics boundary is considered replaceable, so a
     * change here is a change to the plan, not just to a number.
     *
     * D3D 85 -> 80, DSOUND 39 -> 41 and XMV 8 -> 7 when the scanner was corrected to
     * DECODE rather than byte-sweep. Its old docstring warned only about
     * over-counting; measurement showed it was wrong in both directions, and the
     * under-count was the larger error -- the sweep's own 5-byte skip stepped over
     * genuine instructions. After decoding, 0 of 236 rows are suspect, against 4
     * before. These are the post-correction figures. */
    CHECK(xdk_surface_section_count("D3D") == 80);
    CHECK(xdk_surface_section_count("DSOUND") == 41);
    CHECK(xdk_surface_section_count("XMV") == 7);
    CHECK(xdk_surface_section_count("XGRPH") == 24);
    CHECK(xdk_surface_section_count("XNET") == 41);
    CHECK(xdk_surface_section_count("XONLINE") == 30);
    CHECK(xdk_surface_section_count("XPP") == 13);
    /* The seven sections must account for EVERY row. Pinning each section individually
     * leaves an eighth, unlisted one free to hold rows nobody asserted. */
    CHECK(xdk_surface_section_count("D3D") + xdk_surface_section_count("DSOUND") +
              xdk_surface_section_count("XMV") + xdk_surface_section_count("XGRPH") +
              xdk_surface_section_count("XNET") + xdk_surface_section_count("XONLINE") +
              xdk_surface_section_count("XPP") ==
          XDK_SURFACE_COUNT);
    CHECK(xdk_surface_section_count("NOSUCH") == 0);
    CHECK(xdk_surface_section_count(NULL) == 0);
}

static void test_find_by_address(void)
{
    const xdk_surface_entry *first = &xdk_surface[0];
    const xdk_surface_entry *found = xdk_surface_find(first->address);
    CHECK(found != NULL);
    CHECK(found->address == first->address);
    CHECK(xdk_surface_find(0x1u) == NULL);
}

static void test_addresses_are_unique(void)
{
    /* A duplicate would mean the generator double-counted a target, which would
     * also mean its site counts are wrong. */
    size_t duplicates = 0;
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++) {
        for (size_t j = i + 1; j < XDK_SURFACE_COUNT; j++) {
            if (xdk_surface[i].address == xdk_surface[j].address) {
                duplicates++;
            }
        }
    }
    CHECK(duplicates == 0);
}

static void test_some_entries_are_named(void)
{
    /* .XTLID names a good share of the surface; zero would mean the naming step
     * silently did nothing. */
    size_t named = 0;
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++) {
        if (xdk_surface[i].name != NULL) {
            named++;
        }
    }
    CHECK(named > 100);
}

static void test_entries_are_ordered_by_section_then_sites(void)
{
    /* Within a section, busiest first: that ordering is what makes the table a
     * work queue rather than just a list. */
    for (size_t i = 1; i < XDK_SURFACE_COUNT; i++) {
        if (strcmp(xdk_surface[i].section, xdk_surface[i - 1].section) == 0) {
            CHECK(xdk_surface[i].sites <= xdk_surface[i - 1].sites);
        }
    }
}

int main(void)
{
    printf("XDK surface tests\n");
    test_table_is_populated();
    test_measured_section_counts();
    test_find_by_address();
    test_addresses_are_unique();
    test_some_entries_are_named();
    test_entries_are_ordered_by_section_then_sites();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
