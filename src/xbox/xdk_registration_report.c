/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xdk_registration_report.h"

#include <stdlib.h>
#include <string.h>

#include "d3d8_hle.h"
#include "dsound_hle.h"
#include "xinput_hle.h"
#include "xgrph_hle.h"
#include "xonline_hle.h"
#include "xnet_hle.h"

#if defined(__has_include)
#if __has_include("xdk_surface.h")
#include "xdk_surface.h"
_Static_assert(sizeof(xdk_registration_row) == sizeof(xdk_surface_entry),
               "xdk_registration_row size has drifted from the generated xdk_surface_entry");
_Static_assert(offsetof(xdk_registration_row, address) == offsetof(xdk_surface_entry, address),
               "xdk_registration_row.address offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xdk_registration_row, section) == offsetof(xdk_surface_entry, section),
               "xdk_registration_row.section offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xdk_registration_row, name) == offsetof(xdk_surface_entry, name),
               "xdk_registration_row.name offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xdk_registration_row, sites) == offsetof(xdk_surface_entry, sites),
               "xdk_registration_row.sites offset has drifted from xdk_surface_entry");
#endif
#endif

typedef struct {
    const char *module;
    bool registered;
} report_row;

static bool snapshot(const xdk_registration_row *source, report_row *result)
{
    if (source->address == 0u || source->section == NULL) {
        return false;
    }
    const d3d8_entry *graphics = d3d8_hle_entry(source->address);
    const dsound_entry *audio = dsound_hle_entry(source->address);
    const xinput_entry *input = xinput_hle_entry(source->address);
    const xgrph_entry *utility = xgrph_hle_entry(source->address);
    const xonline_entry *online = xonline_hle_entry(source->address);
    if (strcmp(source->section, "D3D") == 0) {
        if (graphics == NULL || audio != NULL || input != NULL || utility != NULL) {
            return false;
        }
        result->module = "D3D8";
        result->registered = graphics->state == D3D8_ENTRY_IMPLEMENTED;
        return result->registered == (graphics->handler != NULL);
    }
    if (strcmp(source->section, "DSOUND") == 0) {
        if (audio == NULL || graphics != NULL || input != NULL || utility != NULL) {
            return false;
        }
        result->module = "DSOUND";
        result->registered = audio->state == DSOUND_ENTRY_IMPLEMENTED;
        return result->registered == (audio->handler != NULL);
    }
    if (strcmp(source->section, "XPP") == 0) {
        if (input == NULL || graphics != NULL || audio != NULL || utility != NULL) {
            return false;
        }
        result->module = "XAPI input";
        result->registered = input->state == XINPUT_ENTRY_IMPLEMENTED;
        return result->registered == (input->handler != NULL);
    }
    if (strcmp(source->section, "XGRPH") == 0) {
        if (utility == NULL || graphics != NULL || audio != NULL || input != NULL) return false;
        result->module = "XGRPH";
        result->registered = utility->state == XGRPH_ENTRY_IMPLEMENTED;
        return result->registered == (utility->handler != NULL);
    }
    if (strcmp(source->section, "XONLINE") == 0) {
        /* T1071: registered only by the opt-in --xonline-offline policy (xonline_offline_register). */
        if (online == NULL || graphics != NULL || audio != NULL || input != NULL || utility != NULL) {
            return false;
        }
        result->module = "XONLINE";
        result->registered = online->state == XONLINE_ENTRY_IMPLEMENTED;
        return result->registered == (online->handler != NULL);
    }
    if (strcmp(source->section, "XNET") == 0) {
        const xnet_entry *network = xnet_hle_entry(source->address);
        if (network == NULL || graphics != NULL || audio != NULL || input != NULL || utility != NULL)
            return false;
        result->module = "XNET";
        result->registered = network->state == XNET_ENTRY_IMPLEMENTED;
        return result->registered == (network->handler != NULL);
    }
    if (strcmp(source->section, "XMV") != 0) {
        return false;
    }
    /* Unsupported sections must not borrow a handler from a neighboring module. */
    return graphics == NULL && audio == NULL && input == NULL && utility == NULL && online == NULL;
}

bool xdk_registration_report_write(FILE *out, const xdk_registration_row *surface, size_t count)
{
    if (out == NULL || surface == NULL || count == 0u) {
        return false;
    }
    report_row *rows = calloc(count, sizeof(*rows));
    if (rows == NULL) {
        return false;
    }
    size_t registered = 0u;
    for (size_t i = 0u; i < count; i++) {
        for (size_t previous = 0u; previous < i; previous++) {
            if (surface[i].address == surface[previous].address) {
                free(rows);
                return false;
            }
        }
        if (!snapshot(&surface[i], &rows[i])) {
            free(rows);
            return false;
        }
        registered += rows[i].registered ? 1u : 0u;
    }
    fprintf(out, "{\n  \"schema\":1,\n  \"kind\":\"xdk-handler-registration\",\n"
                 "  \"surface_count\":%zu,\n  \"registered_handler_count\":%zu,\n"
                 "  \"full_implementation_count\":null,\n  \"full_coverage\":\"unknown\",\n"
                 "  \"entries\":[\n", count, registered);
    for (size_t i = 0u; i < count; i++) {
        fprintf(out, "    {\"address\":\"0x%08X\",\"section\":\"%s\",\"module\":",
                surface[i].address, surface[i].section);
        if (rows[i].module != NULL) {
            fprintf(out, "\"%s\"", rows[i].module);
        } else {
            fputs("null", out);
        }
        fprintf(out, ",\"registered_handler\":%s,\"full_implementation\":null}%s\n",
                rows[i].registered ? "true" : "false", i + 1u < count ? "," : "");
    }
    fputs("  ]\n}\n", out);
    free(rows);
    return !ferror(out);
}
