/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XGRPH_HLE_H
#define TSFP_GPU_XGRPH_HLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mirrors xdk_surface_entry from the generated, gitignored src/xbox/xdk_surface.h, declared
 * locally so this header compiles in a fresh clone where that file has not been generated
 * (T255). Layout is pinned against the real type in xgrph_hle.c whenever it exists. */
typedef struct {
    uint32_t address;
    const char *section;
    const char *name;
    uint32_t sites;
} xgrph_xdk_row;

typedef uint32_t (*xgrph_fn)(void *context);
typedef int (*xgrph_log_fn)(const char *format, ...);
typedef void (*xgrph_fatal_fn)(uint32_t address, const char *message);
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
} xgrph_surface_entry;
typedef enum { XGRPH_ENTRY_STUB = 0, XGRPH_ENTRY_IMPLEMENTED } xgrph_entry_state;
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
    xgrph_fn handler;
    xgrph_entry_state state;
    uint32_t default_return;
    uint64_t call_count;
    bool reported;
    /* Opt-in original contract: dispatch retains entry EAX instead of the transport result. */
    bool preserve_eax;
} xgrph_entry;
/* Rows are copied; names are borrowed and must outlive the adopted registry.
 * Invalid replacement leaves the old registry intact. Initialization is session setup. */
bool xgrph_hle_init(const xgrph_surface_entry *rows, size_t count);
bool xgrph_hle_adopt(const xgrph_xdk_row *rows, size_t count);
void xgrph_hle_shutdown(void);
/* Registration is session setup; ordinary replacement clears any preservation policy. */
bool xgrph_hle_register(uint32_t address, xgrph_fn handler);
/* Use only for an authenticated original EAX-preserving body. Frame stays unchanged. */
bool xgrph_hle_register_preserving_eax(uint32_t address, xgrph_fn handler);
bool xgrph_hle_set_default_return(uint32_t address, uint32_t value);
const xgrph_entry *xgrph_hle_entry(uint32_t address);
/* Missing calls report once and return the configured default; the production
 * thunk enforces stop-on-missing before calling this registry. */
uint32_t xgrph_hle_call(uint32_t address, void *context);
size_t xgrph_hle_count(void);
size_t xgrph_hle_implemented_count(void);
uint64_t xgrph_hle_unknown_count(void);
void xgrph_hle_report_backlog(void);
void xgrph_hle_set_log(xgrph_log_fn printer);
xgrph_log_fn xgrph_hle_log(void);
void xgrph_hle_set_fatal(xgrph_fatal_fn handler);
void xgrph_hle_fatal(uint32_t address, const char *format, ...)
    __attribute__((format(printf, 2, 3), noreturn));
#endif
