/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xonline_hle.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("xdk_surface.h")
#include "xdk_surface.h"
_Static_assert(sizeof(xonline_xdk_row) == sizeof(xdk_surface_entry),
               "xonline_xdk_row size has drifted from the generated xdk_surface_entry");
_Static_assert(offsetof(xonline_xdk_row, address) == offsetof(xdk_surface_entry, address),
               "xonline_xdk_row.address offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xonline_xdk_row, section) == offsetof(xdk_surface_entry, section),
               "xonline_xdk_row.section offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xonline_xdk_row, name) == offsetof(xdk_surface_entry, name),
               "xonline_xdk_row.name offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xonline_xdk_row, sites) == offsetof(xdk_surface_entry, sites),
               "xonline_xdk_row.sites offset has drifted from xdk_surface_entry");
#endif
#endif

static xonline_entry *entries;
static size_t entry_count;
static uint64_t unknown_count;
static xonline_fatal_fn fatal_handler;
static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}
static xonline_log_fn printer = default_printer;
void xonline_hle_set_log(xonline_log_fn value) { printer = value ? value : default_printer; }
xonline_log_fn xonline_hle_log(void) { return printer; }
void xonline_hle_set_fatal(xonline_fatal_fn value) { fatal_handler = value; }
void xonline_hle_fatal(uint32_t address, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printer("xonline: FATAL at guest address 0x%08x: %s\n", (unsigned)address, message);
    if (fatal_handler) fatal_handler(address, message);
    abort();
}
static int compare_address(const void *left, const void *right)
{
    const uint32_t a = ((const xonline_entry *)left)->address;
    const uint32_t b = ((const xonline_entry *)right)->address;
    return a < b ? -1 : a > b ? 1 : 0;
}
bool xonline_hle_init(const xonline_surface_entry *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xonline_entry)) return false;
    xonline_entry *adopted = calloc(count, sizeof(*adopted));
    if (!adopted) return false;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].address == 0u) { free(adopted); return false; }
        adopted[i].address = rows[i].address;
        adopted[i].name = rows[i].name;
        adopted[i].sites = rows[i].sites;
    }
    qsort(adopted, count, sizeof(*adopted), compare_address);
    for (size_t i = 1u; i < count; i++) {
        if (adopted[i - 1u].address == adopted[i].address) { free(adopted); return false; }
    }
    free(entries);
    entries = adopted;
    entry_count = count;
    unknown_count = 0u;
    return true;
}
bool xonline_hle_adopt(const xonline_xdk_row *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xonline_surface_entry)) return false;
    xonline_surface_entry *selected = malloc(count * sizeof(*selected));
    if (!selected) return false;
    size_t matched = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].section && strcmp(rows[i].section, "XONLINE") == 0) {
            selected[matched++] = (xonline_surface_entry){rows[i].address, rows[i].name, rows[i].sites};
        }
    }
    const bool okay = xonline_hle_init(selected, matched);
    free(selected);
    return okay;
}
void xonline_hle_shutdown(void)
{
    free(entries);
    entries = NULL;
    entry_count = 0u;
    unknown_count = 0u;
}
static xonline_entry *find_entry(uint32_t address)
{
    size_t low = 0u, high = entry_count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2u;
        if (entries[mid].address == address) return &entries[mid];
        if (entries[mid].address < address) low = mid + 1u;
        else high = mid;
    }
    return NULL;
}
const xonline_entry *xonline_hle_entry(uint32_t address) { return find_entry(address); }
bool xonline_hle_register(uint32_t address, xonline_fn handler)
{
    xonline_entry *entry = find_entry(address);
    if (!entry || !handler) return false;
    entry->handler = handler;
    entry->state = XONLINE_ENTRY_IMPLEMENTED;
    return true;
}
bool xonline_hle_set_default_return(uint32_t address, uint32_t value)
{
    xonline_entry *entry = find_entry(address);
    if (!entry) return false;
    entry->default_return = value;
    return true;
}
uint32_t xonline_hle_call(uint32_t address, void *context)
{
    xonline_entry *entry = find_entry(address);
    if (!entry) {
        unknown_count++;
        printer("xonline: call to UNKNOWN address 0x%08x -- not in measured surface\n", (unsigned)address);
        return 0u;
    }
    entry->call_count++;
    if (entry->handler) return entry->handler(context);
    if (!entry->reported) {
        entry->reported = true;
        printer("xonline: missing %s at 0x%08x\n", entry->name ? entry->name : "unnamed", (unsigned)address);
    }
    return entry->default_return;
}
size_t xonline_hle_count(void) { return entry_count; }
size_t xonline_hle_implemented_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < entry_count; i++)
        if (entries[i].state == XONLINE_ENTRY_IMPLEMENTED) count++;
    return count;
}
uint64_t xonline_hle_unknown_count(void) { return unknown_count; }
void xonline_hle_report_backlog(void)
{
    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].state == XONLINE_ENTRY_STUB)
            printer("xonline: pending 0x%08x %s: %u sites, %llu calls\n",
                    (unsigned)entries[i].address, entries[i].name ? entries[i].name : "unnamed",
                    (unsigned)entries[i].sites, (unsigned long long)entries[i].call_count);
    }
}
