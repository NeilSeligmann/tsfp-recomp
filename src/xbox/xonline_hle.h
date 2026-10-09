/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XONLINE_HLE_H
#define TSFP_GPU_XONLINE_HLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mirrors xdk_surface_entry from the generated, gitignored src/xbox/xdk_surface.h, declared
 * locally so this header compiles in a fresh clone where that file has not been generated
 * (T255). Layout is pinned against the real type in xonline_hle.c whenever it exists. */
typedef struct {
    uint32_t address;
    const char *section;
    const char *name;
    uint32_t sites;
} xonline_xdk_row;

typedef uint32_t (*xonline_fn)(void *context);
typedef int (*xonline_log_fn)(const char *format, ...);
typedef void (*xonline_fatal_fn)(uint32_t address, const char *message);
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
} xonline_surface_entry;
typedef enum { XONLINE_ENTRY_STUB = 0, XONLINE_ENTRY_IMPLEMENTED } xonline_entry_state;
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
    xonline_fn handler;
    xonline_entry_state state;
    uint32_t default_return;
    uint64_t call_count;
    bool reported;
} xonline_entry;
/* Rows are copied; names are borrowed and must outlive the adopted registry.
 * Invalid replacement leaves the old registry intact. Initialization is session setup. */
bool xonline_hle_init(const xonline_surface_entry *rows, size_t count);
bool xonline_hle_adopt(const xonline_xdk_row *rows, size_t count);
void xonline_hle_shutdown(void);
bool xonline_hle_register(uint32_t address, xonline_fn handler);
bool xonline_hle_set_default_return(uint32_t address, uint32_t value);
const xonline_entry *xonline_hle_entry(uint32_t address);
/* Missing calls report once and return the configured default; the production
 * thunk enforces stop-on-missing before calling this registry. */
uint32_t xonline_hle_call(uint32_t address, void *context);
size_t xonline_hle_count(void);
size_t xonline_hle_implemented_count(void);
uint64_t xonline_hle_unknown_count(void);
void xonline_hle_report_backlog(void);
void xonline_hle_set_log(xonline_log_fn printer);
xonline_log_fn xonline_hle_log(void);
void xonline_hle_set_fatal(xonline_fatal_fn handler);
void xonline_hle_fatal(uint32_t address, const char *format, ...)
    __attribute__((format(printf, 2, 3), noreturn));
#endif
