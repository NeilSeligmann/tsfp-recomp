/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_surface_adapter.h for why this module exists and why it re-declares
 * the measured row shape instead of including the generated header.
 */

#include "d3d8_surface_adapter.h"

#include <stdlib.h>
#include <string.h>

/* Defensive guard against the local row drifting from the generated one. Only
 * active when the generated header actually exists, which it does not in a
 * fresh clone before tools/gen_d3d8_surface.py has run. When it is absent these
 * checks compile out entirely, and when it is present a drifted duplicate
 * struct fails the build instead of silently corrupting every field after the
 * point of drift. */
#if defined(__has_include)
#if __has_include("xdk_surface.h")
#include "xdk_surface.h"

_Static_assert(sizeof(d3d8_xdk_row) == sizeof(xdk_surface_entry),
               "d3d8_xdk_row size has drifted from the generated xdk_surface_entry");
_Static_assert(offsetof(d3d8_xdk_row, address) == offsetof(xdk_surface_entry, address),
               "d3d8_xdk_row.address offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(d3d8_xdk_row, section) == offsetof(xdk_surface_entry, section),
               "d3d8_xdk_row.section offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(d3d8_xdk_row, name) == offsetof(xdk_surface_entry, name),
               "d3d8_xdk_row.name offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(d3d8_xdk_row, sites) == offsetof(xdk_surface_entry, sites),
               "d3d8_xdk_row.sites offset has drifted from xdk_surface_entry");
#endif
#endif

d3d8_surface_entry *d3d8_surface_select(const d3d8_xdk_row *source, size_t count,
                                        const char *section, size_t *out_count)
{
    /* out_count is where every other outcome gets reported, so it has to be
     * checked before anything else: there is no safe way to report "nothing
     * matched" without it. */
    if (!out_count) {
        return NULL;
    }
    *out_count = 0;

    if (!source || count == 0 || !section) {
        return NULL;
    }

    /* First pass: count matches so the allocation is exactly sized rather than
     * sized for the whole (possibly much larger) source array. */
    size_t matched = 0;
    for (size_t i = 0; i < count; i++) {
        const char *row_section = source[i].section;
        /* A NULL section is a measurement with no section recorded. It can
         * never match a real section name, and passing NULL to strcmp is
         * undefined, so it is excluded before the comparison. */
        if (row_section && strcmp(row_section, section) == 0) {
            matched++;
        }
    }

    if (matched == 0) {
        return NULL;
    }

    d3d8_surface_entry *selected = malloc(matched * sizeof(*selected));
    if (!selected) {
        return NULL;
    }

    /* Second pass: fill in source order, copying field by field. The two row
     * shapes are not layout-compatible, so this is deliberately not a memcpy
     * of the whole row and not a cast of the array. */
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        const char *row_section = source[i].section;
        if (row_section && strcmp(row_section, section) == 0) {
            selected[filled].address = source[i].address;
            selected[filled].name = source[i].name;
            selected[filled].sites = source[i].sites;
            filled++;
        }
    }

    *out_count = matched;
    return selected;
}

bool d3d8_surface_adopt(const d3d8_xdk_row *source, size_t count, const char *section)
{
    size_t selected_count = 0;
    d3d8_surface_entry *selected = d3d8_surface_select(source, count, section, &selected_count);
    if (!selected) {
        return false;
    }

    bool adopted = d3d8_hle_init(selected, selected_count);
    /* d3d8_hle_init copies the rows it needs out of this table, so the
     * temporary is freed unconditionally regardless of whether it succeeded. */
    free(selected);
    return adopted;
}
