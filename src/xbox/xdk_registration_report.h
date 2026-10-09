/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XDK_REGISTRATION_REPORT_H
#define TSFP_XBOX_XDK_REGISTRATION_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Mirrors xdk_surface_entry from the generated, gitignored src/xbox/xdk_surface.h, declared
 * locally so this header compiles in a fresh clone where that file has not been generated
 * (T405, same pattern as xgrph_xdk_row). Layout is pinned against the real type in
 * xdk_registration_report.c whenever it exists. */
typedef struct {
    uint32_t address;
    const char *section;
    const char *name; /* NULL when unknown */
    uint32_t sites;
} xdk_registration_row;

/* Snapshot actual handler registrations for the supplied measured surface.
 * Registries must already be initialized. Does not invoke handlers or infer
 * full implementation from registration. Invalid section/registry mappings
 * refuse before any JSON is written. */
bool xdk_registration_report_write(FILE *out, const xdk_registration_row *surface, size_t count);

#endif
