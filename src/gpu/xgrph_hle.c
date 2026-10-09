/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_hle.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("xdk_surface.h")
#include "xdk_surface.h"
_Static_assert(sizeof(xgrph_xdk_row) == sizeof(xdk_surface_entry),
               "xgrph_xdk_row size has drifted from the generated xdk_surface_entry");
_Static_assert(offsetof(xgrph_xdk_row, address) == offsetof(xdk_surface_entry, address),
               "xgrph_xdk_row.address offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xgrph_xdk_row, section) == offsetof(xdk_surface_entry, section),
               "xgrph_xdk_row.section offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xgrph_xdk_row, name) == offsetof(xdk_surface_entry, name),
               "xgrph_xdk_row.name offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xgrph_xdk_row, sites) == offsetof(xdk_surface_entry, sites),
               "xgrph_xdk_row.sites offset has drifted from xdk_surface_entry");
#endif
#endif

static xgrph_entry *entries;
static size_t entry_count;
static uint64_t unknown_count;
static xgrph_fatal_fn fatal_handler;
static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}
static xgrph_log_fn printer = default_printer;
void xgrph_hle_set_log(xgrph_log_fn value) { printer = value ? value : default_printer; }
xgrph_log_fn xgrph_hle_log(void) { return printer; }
void xgrph_hle_set_fatal(xgrph_fatal_fn value) { fatal_handler = value; }
void xgrph_hle_fatal(uint32_t address, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printer("xgrph: FATAL at guest address 0x%08x: %s\n", (unsigned)address, message);
    if (fatal_handler) fatal_handler(address, message);
    abort();
}
static int compare_address(const void *left, const void *right)
{
    const uint32_t a = ((const xgrph_entry *)left)->address;
    const uint32_t b = ((const xgrph_entry *)right)->address;
    return a < b ? -1 : a > b ? 1 : 0;
}
bool xgrph_hle_init(const xgrph_surface_entry *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xgrph_entry)) return false;
    xgrph_entry *adopted = calloc(count, sizeof(*adopted));
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
bool xgrph_hle_adopt(const xgrph_xdk_row *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xgrph_surface_entry)) return false;
    xgrph_surface_entry *selected = malloc(count * sizeof(*selected));
    if (!selected) return false;
    size_t matched = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].section && strcmp(rows[i].section, "XGRPH") == 0) {
            selected[matched++] = (xgrph_surface_entry){rows[i].address, rows[i].name, rows[i].sites};
        }
    }
    const bool okay = xgrph_hle_init(selected, matched);
    free(selected);
    return okay;
}
void xgrph_hle_shutdown(void)
{
    free(entries);
    entries = NULL;
    entry_count = 0u;
    unknown_count = 0u;
}
static xgrph_entry *find_entry(uint32_t address)
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
const xgrph_entry *xgrph_hle_entry(uint32_t address) { return find_entry(address); }
static bool register_handler(uint32_t address, xgrph_fn handler, bool preserve_eax)
{
    xgrph_entry *entry = find_entry(address);
    if (!entry || !handler) return false;
    entry->handler = handler;
    entry->state = XGRPH_ENTRY_IMPLEMENTED;
    entry->preserve_eax = preserve_eax;
    return true;
}
bool xgrph_hle_register(uint32_t address, xgrph_fn handler)
{
    return register_handler(address, handler, false);
}
bool xgrph_hle_register_preserving_eax(uint32_t address, xgrph_fn handler)
{
    return register_handler(address, handler, true);
}
bool xgrph_hle_set_default_return(uint32_t address, uint32_t value)
{
    xgrph_entry *entry = find_entry(address);
    if (!entry) return false;
    entry->default_return = value;
    return true;
}
uint32_t xgrph_hle_call(uint32_t address, void *context)
{
    xgrph_entry *entry = find_entry(address);
    if (!entry) {
        unknown_count++;
        printer("xgrph: call to UNKNOWN address 0x%08x -- not in measured surface\n", (unsigned)address);
        return 0u;
    }
    entry->call_count++;
    if (entry->handler) return entry->handler(context);
    if (!entry->reported) {
        entry->reported = true;
        printer("xgrph: missing %s at 0x%08x\n", entry->name ? entry->name : "unnamed", (unsigned)address);
    }
    return entry->default_return;
}
size_t xgrph_hle_count(void) { return entry_count; }
size_t xgrph_hle_implemented_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < entry_count; i++)
        if (entries[i].state == XGRPH_ENTRY_IMPLEMENTED) count++;
    return count;
}
uint64_t xgrph_hle_unknown_count(void) { return unknown_count; }
void xgrph_hle_report_backlog(void)
{
    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].state == XGRPH_ENTRY_STUB)
            printer("xgrph: pending 0x%08x %s: %u sites, %llu calls\n",
                    (unsigned)entries[i].address, entries[i].name ? entries[i].name : "unnamed",
                    (unsigned)entries[i].sites, (unsigned long long)entries[i].call_count);
    }
}
