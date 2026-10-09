/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one function every XDK call lands in. See xdk_thunk.h for why it is keyed by
 * address, why there is only one of it, and what it cannot see.
 */

#include "xdk_thunk.h"
#include "guest_frame_trace.h"
#include "xdk_original.h"
#include "xmv_original.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "d3d8_hle.h"
#include "function_census.h"
#include "dsound_hle.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "monitor_thunk.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xinput_hle.h"
#include "xgrph_hle.h"
#include "xonline_hle.h"
#include "xnet_hle.h"

extern int recomp_has_dispatch_boundary(uint32_t address) __attribute__((weak));
bool xdk_thunk_dispatch_boundaries_ready(const uint32_t *addresses,size_t count)
{
    if(addresses==NULL || count==0u || recomp_has_dispatch_boundary==NULL)return false;
    for(size_t i=0u;i<count;i++)
        if(addresses[i]==0u || recomp_has_dispatch_boundary(addresses[i])!=1)return false;
    return true;
}

extern int recomp_has_stop_boundary(uint32_t address) __attribute__((weak));
bool xdk_thunk_stream_stops_ready(void)
{
    static const uint32_t required[] = {
        0x00384859u, 0x0040686Cu, 0x00406879u, 0x0040688Au,
        0x0040723Fu, 0x00407286u, 0x004072D4u, 0x0040733Bu,
        0x00407388u, 0x004073D3u, 0x00407424u, 0x00407883u,
        0x0040788Du, 0x004093ADu,
    };
    if (recomp_has_stop_boundary == NULL) return false;
    for (size_t i = 0u; i < sizeof(required) / sizeof(required[0]); i++)
        if (recomp_has_stop_boundary(required[i]) != 1) return false;
    return true;
}

/* Thread-local, for the same reason every guest register is. See the long note above
 * `xdk_dispatch` for why a lock cannot substitute. */
#if defined(__GNUC__) || defined(__clang__)
#define XDK_TLS __thread
#else
#define XDK_TLS _Thread_local
#endif

/* One adopted row: the measurement, plus whatever ABI has been established for it.
 *
 * The two halves are deliberately in one struct even though xdk_thunk.h keeps the
 * INJECTED row free of ABI fields. The injected type mirrors what the generator
 * produces and must not grow fields the measurement cannot fill; this one is live
 * dispatch state and is ours. */
typedef struct {
    uint32_t address;
    const char *name;
    xdk_module module;
    bool abi_known;
    xdk_cc cc;
    uint32_t stack_args;
    uint32_t register_args;
} xdk_row;

/* Sorted by address at init, so the lookup is a binary search and a duplicate is
 * detectable in one pass. */
static xdk_row *g_rows;
static size_t g_row_count;
/* Bounds of the adopted table, for an O(1) reject. This matters: the lifter's
 * dispatch macros consult `recomp_lookup_manual` on EVERY indirect call in the
 * program, so the common answer -- "not an XDK address" -- has to be cheap. */
static uint32_t g_low;
static uint32_t g_high;

static bool g_stop_on_missing = true;

static uint64_t g_calls;
static uint64_t g_module_calls[XDK_MODULE_COUNT];
static uint64_t g_abi_refused;
static uint64_t g_unknown;
static uint64_t g_unrouted;

typedef struct stop_row {
    uint32_t address;
    char label[XDK_STOP_LABEL_BYTES];
    char reason[XDK_STOP_REASON_BYTES];
} stop_row;
static stop_row *stop_rows;
static size_t stop_count;
static uint64_t stop_calls;
static uint64_t read_counter(const uint64_t *counter);

#define STREAM_VIRTUAL_ADDRESS 0x0040733Bu
#define STREAM_STATUS_ADDRESS 0x004073D3u
/* T602: the second measured Discontinuity caller, sub_000299C0 after SetFormat and Pause(1). */
#define STREAM_REFORMAT_CALLER 0x00029A5Au
/* T1198: the streaming source pump (call [ecx+0x14] at 0x2A1A2, return 0x2A1A5) also discontinues a stream mid-feed. */
#define STREAM_PUMP_CALLER 0x0002A1A5u
/* T605: the measured GetStatus caller on that re-formatted stream, sub_00029D10 (callers sub_00028000, sub_00028A60). */
#define STREAM_STATUS_REFORMAT_CALLER 0x00029D28u
static pthread_mutex_t virtual_lock = PTHREAD_MUTEX_INITIALIZER;
static xdk_stream_virtual_handler virtual_handler;
static xdk_stream_status_handler virtual_status_handler;
static size_t virtual_pending, virtual_active;
static uint64_t virtual_calls, virtual_refused;
static XDK_TLS uint32_t virtual_lookup_pending;
/* T392: movie stream methods. Handler changes are quiescent configuration. */
static xdk_movie_method_owner movie_owner;
static xdk_movie_method_handler movie_handler;
static uint64_t movie_calls;
static XDK_TLS uint32_t pending_movie_address;
/* T681: the opt-in passive completion model answers GetStatus, Process and GetInfo of the passive title streams.
 * Same callback types as the movie route. A handler that returns false leaves the call to the route it took before. */
static xdk_movie_method_owner completion_owner;
static xdk_movie_method_handler completion_handler;
static uint64_t completion_calls;
typedef struct movie_method { uint32_t address; const char *label; } movie_method;
static const movie_method movie_methods[] = {
    {0x0040723Fu, "StreamAddRef"}, {0x00407286u, "StreamRelease"}, {0x004073D3u, "StreamGetStatus"},
    {0x00407424u, "StreamProcess"}, {0x0040733Bu, "StreamDiscontinuity"},
    {0x00407388u, "StreamFlush"},
};
static const movie_method *find_movie_method(uint32_t address)
{
    for (size_t i = 0u; i < sizeof(movie_methods) / sizeof(movie_methods[0]); i++)
        if (movie_methods[i].address == address) return &movie_methods[i];
    return NULL;
}
#define STREAM_INFO_ADDRESS 0x004072D4u
bool xdk_thunk_set_movie_method_handler(xdk_movie_method_owner owns, xdk_movie_method_handler handler)
{
    pthread_mutex_lock(&virtual_lock);
    bool valid = virtual_pending == 0u && virtual_active == 0u && (owns == NULL) == (handler == NULL);
    if (valid && handler != NULL) valid = xdk_thunk_stream_stops_ready();
    if (valid) { movie_owner = owns; movie_handler = handler; }
    pthread_mutex_unlock(&virtual_lock);
    return valid;
}
uint64_t xdk_thunk_movie_method_call_count(void) { return read_counter(&movie_calls); }
bool xdk_thunk_set_completion_method_handler(xdk_movie_method_owner owns, xdk_movie_method_handler handler)
{
    pthread_mutex_lock(&virtual_lock);
    bool valid = virtual_pending == 0u && virtual_active == 0u && (handler == NULL) == (owns == NULL);
    if (valid && handler != NULL) valid = valid && xdk_thunk_stream_stops_ready();
    if (valid) { completion_owner = owns; completion_handler = handler; }
    pthread_mutex_unlock(&virtual_lock);
    return valid;
}
uint64_t xdk_thunk_completion_method_call_count(void) { return read_counter(&completion_calls); }
/* T421: IDirectSound::SynchPlayback (0x407A4C), a DIRECT call from the XMV GetNextFrame. It has
 * its own route because its argument is the device interface, not a stream. */
#define SYNCH_ADDRESS 0x00407A4Cu
static xdk_synch_handler synch_handler;
static uint64_t synch_calls;
bool xdk_thunk_synch_stop_ready(void)
{
    return recomp_has_stop_boundary != NULL && recomp_has_stop_boundary(SYNCH_ADDRESS) == 1;
}
bool xdk_thunk_set_synch_handler(xdk_synch_handler handler)
{
    pthread_mutex_lock(&virtual_lock);
    bool valid = virtual_pending == 0u && virtual_active == 0u;
    if (valid && handler != NULL) valid = xdk_thunk_synch_stop_ready();
    if (valid) synch_handler = handler;
    pthread_mutex_unlock(&virtual_lock);
    return valid;
}
uint64_t xdk_thunk_synch_call_count(void) { return read_counter(&synch_calls); }
bool xdk_thunk_synch_route_enabled(void)
{
    pthread_mutex_lock(&virtual_lock);
    const bool enabled = synch_handler != NULL;
    pthread_mutex_unlock(&virtual_lock);
    return enabled;
}
static bool virtual_busy(void)
{
    pthread_mutex_lock(&virtual_lock);
    const bool busy = virtual_pending != 0u || virtual_active != 0u;
    pthread_mutex_unlock(&virtual_lock);
    return busy;
}
bool xdk_thunk_set_stream_virtual_handlers(xdk_stream_virtual_handler handler,
                                           xdk_stream_status_handler status)
{
    pthread_mutex_lock(&virtual_lock);
    bool valid = virtual_pending == 0u && virtual_active == 0u;
    if (valid && (handler != NULL || status != NULL)) {
        valid = xdk_thunk_stream_stops_ready() && g_rows != NULL;
        size_t sound_rows = 0u;
        for (size_t i = 0u; valid && i < g_row_count; i++) {
            if (g_rows[i].module == XDK_MODULE_DSOUND) {
                sound_rows++;
                valid = recomp_has_dispatch_boundary != NULL &&
                        recomp_has_dispatch_boundary(g_rows[i].address) == 1;
            }
        }
        valid = valid && sound_rows == 41u;
    }
    if (valid) { virtual_handler = handler; virtual_status_handler = status; }
    pthread_mutex_unlock(&virtual_lock);
    return valid;
}
bool xdk_thunk_set_stream_virtual_handler(xdk_stream_virtual_handler handler)
{
    return xdk_thunk_set_stream_virtual_handlers(handler, NULL);
}
void xdk_thunk_stream_virtual_cancel_pending(void)
{
    pthread_mutex_lock(&virtual_lock);
    if (virtual_lookup_pending) {
        virtual_lookup_pending = 0u;
        virtual_pending--;
    }
    pthread_mutex_unlock(&virtual_lock);
}
uint64_t xdk_thunk_stream_virtual_call_count(void) { return read_counter(&virtual_calls); }
uint64_t xdk_thunk_stream_virtual_refused_count(void) { return read_counter(&virtual_refused); }
size_t xdk_thunk_stream_virtual_pending_count(void)
{
    pthread_mutex_lock(&virtual_lock);
    const size_t count = virtual_pending;
    pthread_mutex_unlock(&virtual_lock);
    return count;
}
size_t xdk_thunk_stream_virtual_active_count(void)
{
    pthread_mutex_lock(&virtual_lock);
    const size_t count = virtual_active;
    pthread_mutex_unlock(&virtual_lock);
    return count;
}


/* Counters are shared across guest threads. Every critical section below is a few
 * plain stores with no calls in it, for the reason thunk_trace.c documents: a lock
 * held across anything that can `host_run_stop` is a lock held across a
 * `siglongjmp`, and stopping is the expected end of a run. */
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static xdk_log_fn g_log;

static int default_log(const char *format, ...);

static int default_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

void xdk_thunk_set_log(xdk_log_fn printer)
{
    g_log = printer;
}

xdk_log_fn xdk_thunk_log(void)
{
    return g_log ? g_log : default_log;
}

const char *xdk_module_name(xdk_module module)
{
    switch (module) {
    case XDK_MODULE_D3D8:
        return "D3D8";
    case XDK_MODULE_DSOUND:
        return "DSOUND";
    case XDK_MODULE_XINPUT:
        return "XAPI input";
    case XDK_MODULE_XGRPH:
        return "XGRPH";
    case XDK_MODULE_XONLINE:
        return "XONLINE";
    case XDK_MODULE_XNET:
        return "XNET";
    case XDK_MODULE_NONE:
    case XDK_MODULE_COUNT:
        break;
    }
    return "no module";
}

/*
 * Section string -> module, in ONE place.
 *
 * The strings are the generator's own section names, which come from the XBE's
 * section headers rather than from us. XMV deliberately falls
 * through to NONE: they are measured boundaries with nothing behind them, and a
 * default that quietly routed them to the nearest module would produce a wrong answer
 * from a module that has no business seeing the call.
 */
xdk_module xdk_module_for_section(const char *section)
{
    if (!section) {
        return XDK_MODULE_NONE;
    }
    if (strcmp(section, "D3D") == 0) {
        return XDK_MODULE_D3D8;
    }
    if (strcmp(section, "DSOUND") == 0) {
        return XDK_MODULE_DSOUND;
    }
    if (strcmp(section, "XPP") == 0) {
        return XDK_MODULE_XINPUT;
    }
    if (strcmp(section, "XGRPH") == 0) {
        return XDK_MODULE_XGRPH;
    }
    if (strcmp(section, "XONLINE") == 0) {
        return XDK_MODULE_XONLINE;
    }
    if (strcmp(section, "XNET") == 0) {
        return XDK_MODULE_XNET;
    }
    return XDK_MODULE_NONE;
}

const char *xdk_cc_name(xdk_cc cc)
{
    switch (cc) {
    case XDK_CC_STDCALL:
        return "__stdcall";
    case XDK_CC_FASTCALL:
        return "__fastcall";
    case XDK_CC_THISCALL:
        return "__thiscall";
    case XDK_CC_CDECL:
        return "__cdecl";
    }
    return "<unknown convention>";
}

/* --- the table ------------------------------------------------------------ */

static int compare_rows(const void *left, const void *right)
{
    const uint32_t a = ((const xdk_row *)left)->address;
    const uint32_t b = ((const xdk_row *)right)->address;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* EXACT match or NULL. Never a neighbour: returning the next row down would make
 * every call to an address we have not measured look like a call to the one before
 * it, which is a wrong answer wearing a right answer's clothes. */
static xdk_row *find_row(uint32_t address)
{
    if (!g_rows || address < g_low || address > g_high) {
        return NULL;
    }
    size_t lo = 0;
    size_t hi = g_row_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        if (g_rows[mid].address == address) {
            return &g_rows[mid];
        }
        if (g_rows[mid].address < address) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

void xdk_thunk_shutdown(void)
{
    if (!xdk_thunk_set_stream_virtual_handler(NULL)) abort();
    if (!xdk_thunk_set_movie_method_handler(NULL, NULL)) abort();
    if (!xdk_thunk_set_completion_method_handler(NULL, NULL)) abort();
    completion_calls = 0u;
    if (!xdk_thunk_set_synch_handler(NULL)) abort();
    synch_calls = 0u;
    virtual_calls = 0u;
    virtual_refused = 0u;
    xdk_thunk_stop_override_reset();
    free(g_rows);
    g_rows = NULL;
    g_row_count = 0;
    g_low = 0u;
    g_high = 0u;
    xonline_hle_shutdown();
    xnet_hle_shutdown();
    xdk_thunk_reset_counts();
}

void xdk_thunk_stop_override_reset(void)
{
    free(stop_rows);
    stop_rows = NULL;
    stop_count = 0u;
    stop_calls = 0u;
}
size_t xdk_thunk_stop_override_count(void) { return stop_count; }
uint64_t xdk_thunk_stop_override_call_count(void) { return read_counter(&stop_calls); }
static const stop_row *find_stop(uint32_t address)
{
    for (size_t i = 0u; i < stop_count; i++)
        if (stop_rows[i].address == address) return &stop_rows[i];
    return NULL;
}
bool xdk_thunk_stop_override_adopt(const xdk_stop_override *entries, size_t count)
{
    if (!entries || count == 0u || count > XDK_STOP_MAX_ENTRIES) return false;
    stop_row *rows = calloc(count, sizeof(*rows));
    if (!rows) return false;
    for (size_t i = 0u; i < count; i++) {
        const xdk_stop_override *entry = &entries[i];
        bool valid = entry->address != 0u &&
            !(entry->address >= KERNEL_THUNK_VA_BASE &&
              (uint64_t)entry->address < (uint64_t)KERNEL_THUNK_VA_BASE + KERNEL_THUNK_WINDOW_BYTES) &&
            find_row(entry->address) == NULL && entry->label && entry->reason;
        size_t label_bytes = valid ? strnlen(entry->label, XDK_STOP_LABEL_BYTES) : 0u;
        size_t reason_bytes = valid ? strnlen(entry->reason, XDK_STOP_REASON_BYTES) : 0u;
        valid = valid && label_bytes != 0u && label_bytes < XDK_STOP_LABEL_BYTES &&
            reason_bytes != 0u && reason_bytes < XDK_STOP_REASON_BYTES;
        for (size_t j = 0u; j < i; j++)
            if (rows[j].address == entry->address) valid = false;
        if (!valid) { free(rows); return false; }
        rows[i].address = entry->address;
        memcpy(rows[i].label, entry->label, label_bytes + 1u);
        memcpy(rows[i].reason, entry->reason, reason_bytes + 1u);
    }
    xdk_thunk_stop_override_reset();
    stop_rows = rows;
    stop_count = count;
    return true;
}

bool xdk_thunk_init(const xdk_dispatch_entry *table, size_t count)
{
    if (!table || count == 0u || virtual_busy()) {
        return false;
    }
    xdk_row *rows = calloc(count, sizeof(*rows));
    if (!rows) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        rows[i].address = table[i].address;
        rows[i].name = table[i].name;
        rows[i].module = table[i].module;
        rows[i].abi_known = false;
    }
    qsort(rows, count, sizeof(*rows), compare_rows);
    for (size_t i = 1; i < count; i++) {
        if (rows[i].address == rows[i - 1u].address) {
            /* Refused rather than tolerated: with two rows at one address the lookup
             * becomes order-dependent, and "which row won" is not a question a
             * dispatcher should have an answer to. */
            xdk_thunk_log()("xdk: REFUSING the surface -- address 0x%08X appears twice\n",
                            rows[i].address);
            free(rows);
            return false;
        }
    }

    /* Keep the XONLINE module address table in the same adoption transaction as
     * the shared XDK dispatcher. A failed table replacement must not leave a
     * usable dispatch row paired with stale module rows. */
    size_t xonline_count = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].module == XDK_MODULE_XONLINE) {
            xonline_count++;
        }
    }
    if (xonline_count != 0u) {
        xonline_surface_entry *xonline_rows = calloc(xonline_count, sizeof(*xonline_rows));
        if (!xonline_rows) {
            free(rows);
            return false;
        }
        size_t destination = 0u;
        for (size_t i = 0u; i < count; i++) {
            if (rows[i].module == XDK_MODULE_XONLINE) {
                xonline_rows[destination++] = (xonline_surface_entry){
                    .address = rows[i].address,
                    .name = rows[i].name,
                    .sites = 0u,
                };
            }
        }
        const bool xonline_adopted = xonline_hle_init(xonline_rows, xonline_count);
        free(xonline_rows);
        if (!xonline_adopted) {
            free(rows);
            return false;
        }
    } else {
        xonline_hle_shutdown();
    }

    /* Keep the XNET module address table in the same adoption transaction as
     * the shared XDK dispatcher. A failed table replacement must not leave a
     * usable dispatch row paired with stale module rows. */
    size_t xnet_count = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].module == XDK_MODULE_XNET) {
            xnet_count++;
        }
    }
    if (xnet_count != 0u) {
        xnet_surface_entry *xnet_rows = calloc(xnet_count, sizeof(*xnet_rows));
        if (!xnet_rows) {
            free(rows);
            return false;
        }
        size_t destination = 0u;
        for (size_t i = 0u; i < count; i++) {
            if (rows[i].module == XDK_MODULE_XNET) {
                xnet_rows[destination++] = (xnet_surface_entry){
                    .address = rows[i].address,
                    .name = rows[i].name,
                    .sites = 0u,
                };
            }
        }
        const bool xnet_adopted = xnet_hle_init(xnet_rows, xnet_count);
        free(xnet_rows);
        if (!xnet_adopted) {
            free(rows);
            return false;
        }
    } else {
        xnet_hle_shutdown();
    }

    /* Swap in only once the new table is known good, so a failed re-init leaves the
     * previous one intact and usable. Losing a working surface is worse than failing
     * to upgrade it. */
    xdk_thunk_stop_override_reset();
    free(g_rows);
    g_rows = rows;
    g_row_count = count;
    g_low = rows[0].address;
    g_high = rows[count - 1u].address;
    xdk_thunk_reset_counts();
    return true;
}

size_t xdk_thunk_count(void)
{
    return g_row_count;
}

bool xdk_thunk_is_target(uint32_t address)
{
    return find_row(address) != NULL;
}

xdk_module xdk_thunk_module_of(uint32_t address)
{
    const xdk_row *row = find_row(address);
    return row ? row->module : XDK_MODULE_NONE;
}

const char *xdk_thunk_name_of(uint32_t address)
{
    const xdk_row *row = find_row(address);
    return row ? row->name : NULL;
}

/* How a row identifies itself in a log: its name, or its address when the image
 * named nothing. 94 of 236 rows on this image are unnamed, so the address branch is
 * the common one and must not be an afterthought. */
#define XDK_LABEL_BYTES 24

static const char *row_label(const xdk_row *row, uint32_t address, char *buffer,
                             size_t len)
{
    if (row && row->name) {
        return row->name;
    }
    if (!buffer || len == 0u) {
        return "<unnamed>";
    }
    (void)snprintf(buffer, len, "0x%08X", address);
    return buffer;
}

/* --- the ABI -------------------------------------------------------------- */

/* Shared validation for both declare paths. Separate from the quorum gates, which
 * apply to a measurement and not to a hand verification. */
static bool abi_shape_ok(xdk_cc cc, uint32_t stack_args, uint32_t register_args)
{
    switch (cc) {
    case XDK_CC_STDCALL:
    case XDK_CC_CDECL:
        /* Nothing travels in a register under either. A nonzero count here means the
         * caller has the convention wrong, and accepting it would attach registers to
         * a frame whose handler will read the stack. */
        if (register_args != 0u) {
            return false;
        }
        break;
    case XDK_CC_THISCALL:
        /* Exactly one: `this`, in ecx. Zero would mean it is not thiscall and two
         * would mean it is fastcall. */
        if (register_args != 1u) {
            return false;
        }
        break;
    case XDK_CC_FASTCALL:
        if (register_args == 0u || register_args > 2u) {
            return false;
        }
        break;
    default:
        return false;
    }
    return stack_args <= XDK_ABI_MAX_STACK_ARGS;
}

static bool declare(uint32_t address, xdk_cc cc, uint32_t stack_args,
                    uint32_t register_args)
{
    xdk_row *row = find_row(address);
    if (!row) {
        xdk_thunk_log()("xdk: no ABI can be declared for 0x%08X -- it is not in the "
                        "measured surface\n",
                        address);
        return false;
    }
    if (!abi_shape_ok(cc, stack_args, register_args)) {
        xdk_thunk_log()("xdk: REFUSING an ABI for 0x%08X -- %s with %u register and %u "
                        "stack argument(s) is not a shape this convention has\n",
                        address, xdk_cc_name(cc), (unsigned)register_args,
                        (unsigned)stack_args);
        return false;
    }
    row->cc = cc;
    row->stack_args = stack_args;
    row->register_args = register_args;
    row->abi_known = true;
    return true;
}

bool xdk_thunk_declare_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                           uint32_t register_args)
{
    return declare(address, cc, stack_args, register_args);
}

bool xdk_thunk_declare_measured_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                                    uint32_t register_args, uint32_t sites,
                                    bool unanimous)
{
    /*
     * THE TWO GATES, mirroring `stack_args_for()` in kernel_thunk.c exactly.
     *
     * A NON-UNANIMOUS measurement is refused, not used. On the kernel side, of 13
     * ordinals whose arity was independently verified against their call sites, the
     * measured table was WRONG for 6 -- and every one of those six is flagged
     * non-unanimous. The flag was correct and the code was ignoring it.
     *
     * AND A ROW WITH TOO FEW VOTERS IS ALSO REFUSED, because "unanimous" over one site
     * is a tautology rather than a measurement. Ordinal 196 is the case that proved it:
     * 12 arguments, 1 site, flagged unanimous, and 12 is more than that export takes.
     * Over-popping is the worse direction -- it eats the caller's own locals and esp
     * never recovers.
     *
     * The asymmetry settles it in both directions. Refusing a row that was actually
     * right costs a reported stop naming the exact address, which is a bug report
     * somebody can act on in minutes. Accepting a row that was wrong costs a
     * permanently desynced esp and a plausible, wrong trace that cannot be falsified
     * from the inside.
     */
    if (!unanimous) {
        xdk_thunk_log()("xdk: REFUSING a measured ABI for 0x%08X -- the %u site(s) did "
                        "not agree, so %u is a minimum at best\n",
                        address, (unsigned)sites, (unsigned)stack_args);
        return false;
    }
    if (sites < XDK_MEASURED_ARITY_MIN_SITES) {
        xdk_thunk_log()("xdk: REFUSING a measured ABI for 0x%08X -- %u site(s) is below "
                        "the %u needed; one voter always agrees with itself\n",
                        address, (unsigned)sites,
                        (unsigned)XDK_MEASURED_ARITY_MIN_SITES);
        return false;
    }
    return declare(address, cc, stack_args, register_args);
}

bool xdk_thunk_declare_callee_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                                  uint32_t register_args, uint32_t terminators,
                                  bool unanimous)
{
    /*
     * NO QUORUM HERE, DELIBERATELY, and the reason is that a site count is not evidence
     * about this number at all. `ret imm16` states the callee-cleanup byte count
     * outright, read once at the function itself. The gates above defend against an
     * estimator that minimises across call sites; there is no estimator on this path.
     * Applying them anyway would refuse 165 of 199 decisively-measured rows.
     *
     * WHAT IS REFUSED INSTEAD IS A CONFLICT BETWEEN THE CALLEE'S OWN RETURNS, and that
     * asymmetry is the whole argument for a separate entry point. A push count can only
     * be inflated by a register save, so its errors run one way and a minimum is a
     * defensible reduction. Two `ret` instructions disagreeing means the walk crossed a
     * function boundary, and a wrong extent is not biased in any knowable direction --
     * reducing it would launder an extent bug into an arity, which is precisely the
     * class of defect that desyncs esp with a plausible-looking number.
     */
    if (terminators == 0u) {
        xdk_thunk_log()("xdk: REFUSING a callee ABI for 0x%08X -- no terminating "
                        "instruction was read, so nothing was measured\n",
                        address);
        return false;
    }
    if (!unanimous) {
        xdk_thunk_log()("xdk: REFUSING a callee ABI for 0x%08X -- its %u terminator(s) "
                        "disagreed, which means the extent is wrong, not that %u is a "
                        "minimum\n",
                        address, (unsigned)terminators, (unsigned)stack_args);
        return false;
    }
    return declare(address, cc, stack_args, register_args);
}

size_t xdk_thunk_declare_abi_rows(const xdk_abi_row *rows, size_t count, size_t *refused)
{
    size_t accepted = 0;
    size_t rejected = 0;
    for (size_t i = 0; i < count; i++) {
        const xdk_abi_row *row = &rows[i];
        bool ok = false;
        switch (row->evidence) {
        case XDK_ABI_FROM_CALLEE_RET:
            ok = xdk_thunk_declare_callee_abi(row->address, row->cc, row->stack_args,
                                              row->register_args, row->count, true);
            break;
        case XDK_ABI_FROM_CALLER_VOTES:
            ok = xdk_thunk_declare_measured_abi(row->address, row->cc, row->stack_args,
                                                row->register_args, row->count, true);
            break;
        default:
            /* An evidence class this build does not know is refused, not defaulted. */
            break;
        }
        if (ok) {
            accepted++;
        } else {
            rejected++;
        }
    }
    if (refused) {
        *refused = rejected;
    }
    return accepted;
}

/* Generated from the user's executable and gitignored, like the surface it is keyed by,
 * so a fresh clone has no table and builds with a zero-row one. A committed copy could
 * go stale against an uncommitted surface with nothing in CI able to notice. */
#if defined(TSFP_HAVE_XDK_ABI_INC)
#include "xdk_abi.inc"
#else
static const xdk_abi_row XDK_MEASURED_ABIS[1] = {{0u, XDK_CC_STDCALL, 0u, 0u,
                                                  XDK_ABI_FROM_CALLEE_RET, 0u}};
#define XDK_MEASURED_ABI_COUNT 0u
#endif

size_t xdk_thunk_declare_generated_abis(size_t *refused)
{
    return xdk_thunk_declare_abi_rows(XDK_MEASURED_ABIS, (size_t)XDK_MEASURED_ABI_COUNT,
                                      refused);
}

size_t xdk_thunk_generated_abi_count(void)
{
    return (size_t)XDK_MEASURED_ABI_COUNT;
}

bool xdk_thunk_abi_known(uint32_t address)
{
    const xdk_row *row = find_row(address);
    return row && row->abi_known;
}

/* Bytes the callee pops. The return address always, plus the stack arguments a real
 * `ret N` would have taken with it -- and under cdecl, NOT those, because the caller
 * cleans up and popping them here would eat its locals. Register arguments are not on
 * the stack and are never counted. */
static uint32_t pop_bytes_for(const xdk_row *row)
{
    if (row->cc == XDK_CC_CDECL) {
        return 4u;
    }
    return 4u + 4u * row->stack_args;
}

bool xdk_thunk_pop_bytes(uint32_t address, uint32_t *out)
{
    const xdk_row *row = find_row(address);
    if (!row || !row->abi_known) {
        return false;
    }
    if (out) {
        *out = pop_bytes_for(row);
    }
    return true;
}

size_t xdk_thunk_abi_count(void)
{
    size_t total = 0;
    for (size_t i = 0; i < g_row_count; i++) {
        if (g_rows[i].abi_known) {
            total++;
        }
    }
    return total;
}

/* --- counters ------------------------------------------------------------- */

void xdk_thunk_set_stop_on_missing(bool stop)
{
    g_stop_on_missing = stop;
}

static void bump(uint64_t *counter)
{
    pthread_mutex_lock(&count_lock);
    (*counter)++;
    pthread_mutex_unlock(&count_lock);
}

static uint64_t read_counter(const uint64_t *counter)
{
    pthread_mutex_lock(&count_lock);
    const uint64_t snapshot = *counter;
    pthread_mutex_unlock(&count_lock);
    return snapshot;
}

uint64_t xdk_thunk_call_count(void)
{
    return read_counter(&g_calls);
}

uint64_t xdk_thunk_module_call_count(xdk_module module)
{
    if ((unsigned)module >= (unsigned)XDK_MODULE_COUNT) {
        return 0u;
    }
    return read_counter(&g_module_calls[module]);
}

uint64_t xdk_thunk_abi_refused_count(void)
{
    return read_counter(&g_abi_refused);
}

uint64_t xdk_thunk_unknown_count(void)
{
    return read_counter(&g_unknown);
}

uint64_t xdk_thunk_unrouted_count(void)
{
    return read_counter(&g_unrouted);
}

void xdk_thunk_reset_counts(void)
{
    pthread_mutex_lock(&count_lock);
    g_calls = 0u;
    g_abi_refused = 0u;
    g_unknown = 0u;
    g_unrouted = 0u;
    for (unsigned i = 0; i < (unsigned)XDK_MODULE_COUNT; i++) {
        g_module_calls[i] = 0u;
    }
    pthread_mutex_unlock(&count_lock);
}

/* --- routing -------------------------------------------------------------- */

/*
 * Whether the owning module has a REAL implementation at this address.
 *
 * Asked of the module rather than tracked here, because the module owns that fact and
 * a second copy of it would drift. Note what is NOT accepted: all three modules
 * represent a measured-but-unwritten address as a STUB that reports once and returns
 * a default, and a stub is exactly the plausible-wrong-answer this dispatcher must not
 * produce. Only `*_ENTRY_IMPLEMENTED` counts.
 */
static bool module_has_implementation(const xdk_row *row)
{
    switch (row->module) {
    case XDK_MODULE_D3D8: {
        const d3d8_entry *entry = d3d8_hle_entry(row->address);
        return entry && entry->state == D3D8_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_DSOUND: {
        const dsound_entry *entry = dsound_hle_entry(row->address);
        return entry && entry->state == DSOUND_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_XINPUT: {
        const xinput_entry *entry = xinput_hle_entry(row->address);
        return entry && entry->state == XINPUT_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_XGRPH: {
        const xgrph_entry *entry = xgrph_hle_entry(row->address);
        return entry && entry->state == XGRPH_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_XONLINE: {
        const xonline_entry *entry = xonline_hle_entry(row->address);
        return entry && entry->state == XONLINE_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_XNET: {
        const xnet_entry *entry = xnet_hle_entry(row->address);
        return entry && entry->state == XNET_ENTRY_IMPLEMENTED;
    }
    case XDK_MODULE_NONE:
    case XDK_MODULE_COUNT:
        break;
    }
    return false;
}

/*
 * The one place a call crosses into an HLE module.
 *
 * Every arm passes the SAME `kernel_call_frame`, which is why none of the three
 * `*_call` signatures had to change: all three already took `void *context`, and this
 * is what `context` is. A D3D handler reads its arguments with `kernel_frame_arg` and
 * its `this` with `kernel_frame_reg_arg(frame, 0)`, exactly as a `Kf*` kernel handler
 * does.
 */
static uint32_t module_dispatch(const xdk_row *row, kernel_call_frame *frame)
{
    bump(&g_module_calls[row->module]);
    switch (row->module) {
    case XDK_MODULE_D3D8:
        return d3d8_hle_call(row->address, frame);
    case XDK_MODULE_DSOUND:
        return dsound_hle_call(row->address, frame);
    case XDK_MODULE_XINPUT:
        return xinput_hle_call(row->address, frame);
    case XDK_MODULE_XGRPH:
        return xgrph_hle_call(row->address, frame);
    case XDK_MODULE_XONLINE:
        return xonline_hle_call(row->address, frame);
    case XDK_MODULE_XNET:
        return xnet_hle_call(row->address, frame);
    case XDK_MODULE_NONE:
    case XDK_MODULE_COUNT:
        break;
    }
    /* Unreachable: an unrouted row stops the run above, unconditionally. Kept as a
     * hard stop rather than a `return 0` so that if the ordering above is ever
     * changed, the consequence is a report and not a fabricated value. */
    host_run_stop(HOST_STOP_XDK_UNROUTED, row->address, 0u,
                  "dispatch reached a row with no module");
    return 0u;
}

/*
 * The pending address.
 *
 * THREAD-LOCAL, AND A LOCK WOULD NOT DO. The generated dispatch macros do
 * `_fn = recomp_lookup_manual(_va); ... RECOMP_ABI_CALL(_va, _fn);` -- the store and
 * the call are separated by statements in code we do not own. A second guest thread
 * overwriting this between them would make us look up the wrong row, call the wrong
 * module with this thread's frame, and then advance `g_esp` by the wrong number of
 * bytes: a PERMANENT esp desync, non-deterministic, producing exactly the
 * plausible-but-wrong trace the ABI refusal exists to prevent. A mutex cannot fix it,
 * because the critical section would have to span generated code with no place to
 * release it. Thread-local storage can, and does.
 */
static XDK_TLS uint32_t g_pending_address;

static XDK_TLS uint32_t pending_stop_address;
void xdk_thunk_stop_at(uint32_t address, const char *label, const char *reason)
{
    uint32_t caller = 0u;
    (void)kernel_guest_read_u32(g_esp, &caller);
    bump(&stop_calls);
    (void)thunk_trace_thread_id();
    (void)thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address, caller, false);
    const char *name = label ? label : "<unnamed stop-only entry>";
    const char *detail = reason ? reason : "explicit stop-only policy";
    xdk_thunk_log()("xdk: stop-only %s at 0x%08X: %s\n", name, address, detail);
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, detail);
    abort(); /* host_run_stop never returns; enforce this API contract as well. */
}
static void stop_override_dispatch(void)
{
    const uint32_t address = pending_stop_address;
    const stop_row *row = find_stop(address);
    xdk_thunk_stop_at(address, row ? row->label : NULL,
                     row ? row->reason : "stop-only registry changed before dispatch");
}

/* --- T51 dispatch probe (diagnostic, off unless TSFP_XDK_PROBE is set) ---------------
 *
 * Logs one line per dispatch at the named addresses, BEFORE the retained-original,
 * stop and ABI checks, so a call that stops the run is still logged and a retained
 * original is still seen. It reads guest memory and never writes it. Grammar, clauses
 * separated by ';':  at=ADDR[,ADDR...]  word=ADDR[,ADDR...]  hash=ADDR:PTRARG:LENARG
 * `at` picks the dispatch addresses, `word` adds guest dwords sampled on every logged
 * call, `hash` adds an FNV-1a 64 of the buffer at stack argument PTRARG, LENARG bytes
 * long (stack arguments count from 0). A malformed spec ends the process, because a
 * silently disabled probe reads as "the game never made that call". */
#define PROBE_MAX 32u
#define PROBE_ARGS 6u
#define PROBE_HASH_CAP 65536u
typedef struct {
    uint32_t address;
    int hash_ptr_arg;
    int hash_len_arg;
} probe_site;
static probe_site probe_sites[PROBE_MAX];
static size_t probe_site_count;
#define PROBE_WORDS_MAX 8u
static uint32_t probe_words[PROBE_WORDS_MAX];
static size_t probe_word_count;
static uint64_t probe_sequence;
static pthread_once_t probe_once = PTHREAD_ONCE_INIT;

static void probe_bad(const char *why, const char *text)
{
    fprintf(stderr, "TSFP_XDK_PROBE: %s near \"%s\"\n", why, text);
    exit(2);
}

static uint32_t probe_number(const char *text)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 0);
    if (end == text || value > 0xFFFFFFFFul) {
        probe_bad("not a 32-bit number", text);
    }
    return (uint32_t)value;
}

static probe_site *probe_add_site(uint32_t address)
{
    for (size_t i = 0; i < probe_site_count; i++) {
        if (probe_sites[i].address == address) {
            return &probe_sites[i];
        }
    }
    if (probe_site_count >= PROBE_MAX) {
        probe_bad("too many addresses", "");
    }
    probe_site *site = &probe_sites[probe_site_count++];
    site->address = address;
    site->hash_ptr_arg = -1;
    site->hash_len_arg = -1;
    return site;
}

static void probe_parse(void)
{
    const char *spec = getenv("TSFP_XDK_PROBE");
    if (!spec || !*spec) {
        return;
    }
    char text[512];
    if (strlen(spec) >= sizeof(text)) {
        probe_bad("spec too long", spec);
    }
    strcpy(text, spec);
    char *clause_state = NULL;
    for (char *clause = strtok_r(text, ";", &clause_state); clause;
         clause = strtok_r(NULL, ";", &clause_state)) {
        if (strncmp(clause, "hash=", 5) == 0) {
            char *field_state = NULL;
            char *address = strtok_r(clause + 5, ":", &field_state);
            char *pointer = strtok_r(NULL, ":", &field_state);
            char *length = strtok_r(NULL, ":", &field_state);
            if (!address || !pointer || !length) {
                probe_bad("hash needs ADDR:PTRARG:LENARG", clause);
            }
            probe_site *site = probe_add_site(probe_number(address));
            site->hash_ptr_arg = (int)probe_number(pointer);
            site->hash_len_arg = (int)probe_number(length);
        } else if (strncmp(clause, "at=", 3) == 0 || strncmp(clause, "word=", 5) == 0) {
            const bool words = clause[0] == 'w';
            char *item_state = NULL;
            for (char *item = strtok_r(clause + (words ? 5 : 3), ",", &item_state); item;
                 item = strtok_r(NULL, ",", &item_state)) {
                if (!words) {
                    (void)probe_add_site(probe_number(item));
                } else if (probe_word_count < PROBE_WORDS_MAX) {
                    probe_words[probe_word_count++] = probe_number(item);
                } else {
                    probe_bad("too many words", item);
                }
            }
        } else {
            probe_bad("unknown clause", clause);
        }
    }
}

static void probe_dispatch(uint32_t address)
{
    (void)pthread_once(&probe_once, probe_parse);
    const probe_site *site = NULL;
    for (size_t i = 0; i < probe_site_count; i++) {
        if (probe_sites[i].address == address) {
            site = &probe_sites[i];
            break;
        }
    }
    if (!site) {
        return;
    }
    char line[640];
    size_t used = 0;
    uint32_t return_address = 0u;
    uint32_t args[PROBE_ARGS] = {0};
    (void)kernel_guest_read_u32((kernel_guest_ptr)g_esp, &return_address);
    for (uint32_t i = 0; i < PROBE_ARGS; i++) {
        (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u + 4u * i), &args[i]);
    }
    const uint64_t number = __atomic_add_fetch(&probe_sequence, 1u, __ATOMIC_RELAXED);
    used += (size_t)snprintf(line, sizeof(line),
                             "probe: n=%llu t%u addr=0x%08X caller=0x%08X args=",
                             (unsigned long long)number, thunk_trace_thread_id(), address,
                             return_address);
    for (uint32_t i = 0; i < PROBE_ARGS; i++) {
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s0x%X", i ? "," : "",
                                 args[i]);
    }
    used += (size_t)snprintf(line + used, sizeof(line) - used, " words=");
    for (size_t i = 0; i < probe_word_count; i++) {
        uint32_t value = 0u;
        const bool readable = kernel_guest_read_u32((kernel_guest_ptr)probe_words[i], &value);
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s0x%X:%s0x%X",
                                 i ? "," : "", probe_words[i], readable ? "" : "unreadable=",
                                 value);
    }
    if (site->hash_ptr_arg >= 0 && site->hash_len_arg >= 0 &&
        site->hash_ptr_arg < (int)PROBE_ARGS && site->hash_len_arg < (int)PROBE_ARGS) {
        const uint32_t length = args[site->hash_len_arg];
        uint64_t hash = 14695981039346656037ull;
        bool readable = length <= PROBE_HASH_CAP;
        for (uint32_t i = 0; readable && i < length; i++) {
            uint8_t byte = 0u;
            readable = kernel_guest_read_u8((kernel_guest_ptr)(args[site->hash_ptr_arg] + i),
                                            &byte);
            hash = (hash ^ byte) * 1099511628211ull;
        }
        used += (size_t)snprintf(line + used, sizeof(line) - used, " hash=%s%016llx/%u",
                                 readable ? "" : "unreadable:", (unsigned long long)hash,
                                 length);
    }
    xdk_thunk_log()("%s\n", line);
}

static void xdk_dispatch_body(void);

/* T1289: the guest frame trace times every XDK call (exclusive of the phases it enters). */
static void xdk_dispatch(void)
{
    gft_enter(GFT_XDK, g_pending_address);
    xdk_dispatch_body();
    gft_leave();
}

static void xdk_dispatch_body(void)
{
    const uint32_t address = g_pending_address;
    probe_dispatch(address);
    /* Retained originals own their guest ABI and all state effects. Both manual
     * lookup and generated trampolines enter here; disabled routing is unchanged. */
    if (xdk_original_dispatch(address) || xmv_original_dispatch(address)) {
        return;
    }
    char label_buffer[XDK_LABEL_BYTES];

    bump(&g_calls);
    (void)thunk_trace_thread_id();

    uint32_t return_address = 0u;
    (void)kernel_guest_read_u32((kernel_guest_ptr)g_esp, &return_address);

    const xdk_row *row = find_row(address);
    const bool implemented = row && module_has_implementation(row);
    const char *label = row_label(row, address, label_buffer, sizeof(label_buffer));

    /* Recorded BEFORE any stop, so the trace ENDS WITH the thing that stopped it
     * rather than without it. The result is not known yet and is patched below on the
     * paths that get that far. */
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address,
                                                 return_address, implemented);

    if (!row) {
        /* The adopted surface and the binary disagree. Worse than unfinished work, so
         * it reports EVERY time and is NOT relaxed by --continue-on-missing: that flag
         * means "proceed past something nobody has written yet", and this is not that. */
        bump(&g_unknown);
        xdk_thunk_log()("xdk: dispatch to 0x%08X, which is NOT in the measured surface "
                        "(%zu rows) -- the table and the binary disagree\n",
                        address, g_row_count);
        host_run_stop(HOST_STOP_XDK_NOT_MEASURED, address, 0u,
                      "address absent from the measured XDK surface");
    }
    if (row->module == XDK_MODULE_NONE) {
        /* Also unconditional. There is no module here, so there is nothing to supply
         * a default and no honest value to return. */
        bump(&g_unrouted);
        xdk_thunk_log()("xdk: %s (0x%08X) is in a section with no HLE module -- nothing "
                        "can answer this call\n",
                        label, address);
        host_run_stop(HOST_STOP_XDK_UNROUTED, address, 0u,
                      "measured XDK section has no HLE module");
    }
    if (!implemented) {
        /* The expected end of a bring-up run that reaches this boundary. Relaxed by
         * --continue-on-missing, which then lets the module's own stub answer and
         * announce itself. */
        if (row->module == XDK_MODULE_DSOUND &&
            dsound_hle_requires_implementation(address)) {
            host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, label);
        }
        if (g_stop_on_missing) {
            host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, label);
        }
    }
    if (!row->abi_known) {
        /* We know what it is called and nothing about how to unwind it. Guessing zero
         * arguments would desync esp for every later call, and a desynced run does not
         * crash -- it keeps going and lies. Unconditional, exactly as the kernel
         * path's HOST_STOP_KERNEL_ABI_UNKNOWN is. */
        bump(&g_abi_refused);
        xdk_thunk_log()("xdk: REFUSING to call %s (0x%08X, %s) -- no calling convention "
                        "or argument count has been established for it\n",
                        label, address, xdk_module_name(row->module));
        host_run_stop(HOST_STOP_XDK_ABI_UNKNOWN, address, 0u, label);
    }

    kernel_call_frame frame = {
        .stack_ptr = (kernel_guest_ptr)g_esp,
        .stack_limit = 0u,
        .ecx = 0u,
        .edx = 0u,
        .has_registers = false,
    };
    /* Attached unconditionally. A stdcall handler never asks for them, a fastcall or
     * thiscall one refuses the call without them, and supplying them always is
     * therefore strictly more informative and never wrong. */
    kernel_frame_set_registers(&frame, g_ecx, g_edx);

    const uint32_t entry_eax = g_eax;
    const xgrph_entry *xgrph = row->module == XDK_MODULE_XGRPH ?
        xgrph_hle_entry(row->address) : NULL;
    const bool preserve_eax = xgrph != NULL &&
        xgrph->state == XGRPH_ENTRY_IMPLEMENTED && xgrph->preserve_eax;
    const uint32_t result = module_dispatch(row, &frame);
    const uint32_t published_eax = preserve_eax ? entry_eax : result;

    /* Patch OUR pre-appended slot with the guest-visible result, even if another
     * thread returned first. A full trace's NO_SLOT is ignored. */
    thunk_trace_patch_result(slot, published_eax);

    g_eax = published_eax;
    g_esp += pop_bytes_for(row);
}

/*
 * The production entry point.
 *
 * Every indirect-dispatch macro the lifter emits consults this FIRST, before the
 * lifted-function table and before the kernel window, which is what makes it the
 * override seam. Answering here for an XDK address is how a measured boundary takes
 * priority over the lifted body of the same function.
 *
 * Declared here as well as in the generated header: this file is built under the
 * project's strict warning flags and cannot include 2.56 M lines of generated C.
 */
static void virtual_release_active(void)
{
    pthread_mutex_lock(&virtual_lock);
    virtual_active--;
    pthread_mutex_unlock(&virtual_lock);
}
static void stream_virtual_dispatch_at(uint32_t address)
{
    const uint32_t entry_eax = g_eax, entry_esp = g_esp;
    pthread_mutex_lock(&virtual_lock);
    const bool matching = virtual_lookup_pending == address;
    if (matching) {
        virtual_lookup_pending = 0u;
        virtual_pending--;
    }
    const xdk_stream_virtual_handler handler = virtual_handler;
    const xdk_stream_status_handler status = virtual_status_handler;
    const bool is_status = address == STREAM_STATUS_ADDRESS;
    const char *label = is_status ? "StreamGetStatus" : "StreamDiscontinuity";
    if (!matching || (is_status ? status == NULL : handler == NULL) || virtual_active == SIZE_MAX) {
        pthread_mutex_unlock(&virtual_lock);
        xdk_thunk_stop_at(address, label,
                          "owned virtual stream route is disabled");
    }
    virtual_active++;
    pthread_mutex_unlock(&virtual_lock);
    bump(&virtual_calls);
    uint32_t caller = 0u;
    (void)kernel_guest_read_u32(entry_esp, &caller);
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u,
        address, caller, true);
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    if (!host_run_armed() || !host_run_scope_init(&scope)) {
        virtual_release_active();
        abort();
    }
    if (sigsetjmp(*host_run_scope_jmp(&scope), 0) != 0) {
        const host_stop stop = *host_run_result();
        g_eax = entry_eax;
        g_esp = entry_esp;
        if (!host_run_scope_pop(&scope)) abort();
        bump(&virtual_refused);
        virtual_release_active();
        host_run_rethrow(&stop);
    }
    if (!host_run_scope_push(&scope)) {
        bump(&virtual_refused);
        virtual_release_active();
        xdk_thunk_stop_at(address, label,
                          "owned virtual stream stop scope unavailable");
    }
    const uint32_t frame_bytes = is_status ? 12u : 8u;
    uint32_t words[3] = {0u, 0u, 0u};
    if (entry_esp > UINT32_MAX - frame_bytes ||
        !kernel_guest_read_bytes(entry_esp, words, frame_bytes) ||
        (words[0] != (is_status ? 0x00029CEAu : 0x00029B72u) &&
         (is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {
        static char caller_detail[160];
        snprintf(caller_detail, sizeof caller_detail,
                 "owned virtual stream caller/frame is outside measured scope (caller %#x, %s)",
                 words[0], label);
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, caller_detail);
    }
    if (is_status && ((uint64_t)words[2] + 4u > UINT64_C(0x100000000) ||
        ((uint64_t)words[2] < (uint64_t)entry_esp + frame_bytes &&
         (uint64_t)entry_esp < (uint64_t)words[2] + 4u))) {
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u,
                      "owned virtual stream output overlaps frame or wraps");
    }
    const uint32_t result = is_status ? status(words[1], words[2]) : handler(words[1]);
    if (!host_run_scope_pop(&scope)) abort();
    thunk_trace_patch_result(slot, result);
    g_eax = result;
    g_esp = entry_esp + frame_bytes;
    virtual_release_active();
}

static void stream_virtual_dispatch(void)
{ stream_virtual_dispatch_at(STREAM_VIRTUAL_ADDRESS); }
static void stream_status_dispatch(void)
{ stream_virtual_dispatch_at(STREAM_STATUS_ADDRESS); }

/* The startup virtual route for the two bounded methods. NULL when it does not apply. */
static recomp_func_t virtual_lookup(uint32_t xbox_va)
{
    if (xbox_va != STREAM_VIRTUAL_ADDRESS && xbox_va != STREAM_STATUS_ADDRESS) return NULL;
    pthread_mutex_lock(&virtual_lock);
    const bool enabled = xbox_va == STREAM_STATUS_ADDRESS ?
                         virtual_status_handler != NULL : virtual_handler != NULL;
    const bool conflict = enabled && virtual_lookup_pending != 0u &&
                          virtual_lookup_pending != xbox_va;
    const bool overflow = enabled && virtual_lookup_pending == 0u && virtual_pending == SIZE_MAX;
    if (enabled && !conflict && !overflow && virtual_lookup_pending == 0u) {
        virtual_lookup_pending = xbox_va;
        virtual_pending++;
    }
    pthread_mutex_unlock(&virtual_lock);
    if (conflict || overflow) {
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, xbox_va, 0u,
                      "conflicting or exhausted virtual lookup reservation");
    }
    if (enabled) return xbox_va == STREAM_STATUS_ADDRESS ? stream_status_dispatch : stream_virtual_dispatch;
    return NULL;
}
/* T392: a movie stream method. The movie handler answers a stream it owns. Anything else takes
 * the route it took before this existed: the startup virtual route, then the compiled stop. */
static void movie_method_dispatch(void)
{
    const uint32_t address = pending_movie_address;
    const uint32_t entry_esp = g_esp;
    pthread_mutex_lock(&virtual_lock);
    const xdk_movie_method_owner owns = movie_owner;
    const xdk_movie_method_handler handler = movie_handler;
    pthread_mutex_unlock(&virtual_lock);
    if (owns != NULL && handler != NULL && owns(entry_esp)) {
        uint32_t result = 0u, pop_bytes = 0u, caller = 0u;
        (void)kernel_guest_read_u32(entry_esp, &caller);
        const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address, caller, true);
        if (!handler(address, entry_esp, &result, &pop_bytes)) abort();
        bump(&movie_calls);
        thunk_trace_patch_result(slot, result);
        g_eax = result;
        g_esp = entry_esp + 4u + pop_bytes;
        return;
    }
    pthread_mutex_lock(&virtual_lock);
    const xdk_movie_method_owner completion_owns = completion_owner;
    const xdk_movie_method_handler completion_answer = completion_handler;
    pthread_mutex_unlock(&virtual_lock);
    if (completion_owns != NULL && completion_answer != NULL && completion_owns(entry_esp)) {
        uint32_t result = 0u, pop_bytes = 0u, caller = 0u;
        if (completion_answer(address, entry_esp, &result, &pop_bytes)) {
            (void)kernel_guest_read_u32(entry_esp, &caller);
            const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address, caller, true);
            bump(&completion_calls);
            thunk_trace_patch_result(slot, result);
            g_eax = result;
            g_esp = entry_esp + pop_bytes + 4u;
            return;
        }
    }
    const recomp_func_t startup = virtual_lookup(address);
    if (startup != NULL) {
        startup();
        return;
    }
    const movie_method *method = find_movie_method(address);
    xdk_thunk_stop_at(address, method != NULL ? method->label : NULL,
                      "Unrecovered stream or shared reference method; original child state is unavailable");
}

/* T421: SynchPlayback (stdcall, one argument, RET 4). The handler validates the caller and the device
 * interface and refuses by stopping the run. With no handler the compiled stop is the answer. */
static void synch_dispatch(void)
{
    const uint32_t entry_esp = g_esp;
    pthread_mutex_lock(&virtual_lock);
    const xdk_synch_handler handler = synch_handler;
    pthread_mutex_unlock(&virtual_lock);
    if (handler == NULL)
        xdk_thunk_stop_at(SYNCH_ADDRESS, "DeviceSynchPlayback",
                          "owned SynchPlayback route is disabled");
    uint32_t result = 0u, caller = 0u;
    (void)kernel_guest_read_u32(entry_esp, &caller);
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, SYNCH_ADDRESS, caller, true);
    if (!handler(entry_esp, &result)) abort();
    bump(&synch_calls);
    thunk_trace_patch_result(slot, result);
    g_eax = result;
    g_esp = entry_esp + 8u;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /* T821: the read-only census of indirect call targets (opt-in, one load and a branch when off). */
    function_census_note(xbox_va, g_esp);
    /* The Prcb debug-monitor notify, which is a synthetic VA above the kernel
     * ordinals rather than an XDK address. It is answered HERE, and not in
     * `recomp_lookup_kernel`, because this is the lookup that lives in a library a
     * ctest binary can link: see src/host/monitor_thunk.h. One compare, before the
     * reject below, because the XDK bounds say nothing about it. */
    recomp_func_t notify = monitor_thunk_lookup(xbox_va);
    if (notify) {
        return notify;
    }
    if (find_stop(xbox_va)) {
        pending_stop_address = xbox_va;
        return stop_override_dispatch;
    }
    if (xbox_va == SYNCH_ADDRESS) {
        pthread_mutex_lock(&virtual_lock);
        const bool synch = synch_handler != NULL;
        pthread_mutex_unlock(&virtual_lock);
        if (synch) return synch_dispatch;
    }
    /* T681: GetInfo is only ever routed for the completion model, so a movie-only boot keeps its lookup. */
    if (find_movie_method(xbox_va) != NULL || xbox_va == STREAM_INFO_ADDRESS) {
        pthread_mutex_lock(&virtual_lock);
        const bool movie = movie_handler != NULL && movie_owner != NULL;
        const bool completion = completion_handler != NULL && completion_owner != NULL;
        pthread_mutex_unlock(&virtual_lock);
        if (xbox_va == STREAM_INFO_ADDRESS ? completion : (movie || completion)) {
            pending_movie_address = xbox_va;
            return movie_method_dispatch;
        }
    }
    {
        const recomp_func_t startup = virtual_lookup(xbox_va);
        if (startup != NULL) return startup;
    }
    /* The O(1) reject first. This runs on every indirect call in the program. */
    if (!g_rows || xbox_va < g_low || xbox_va > g_high) {
        return NULL;
    }
    if (!find_row(xbox_va)) {
        return NULL;
    }
    g_pending_address = xbox_va;
    return xdk_dispatch;
}

void xdk_thunk_dispatch_at(uint32_t address)
{
    /* The same thread-local store the lookup path makes, for the same reason: the
     * dispatcher reads an address, not an argument, because every lifted function is
     * `void(void)`. Setting it here rather than passing a parameter keeps ONE dispatch
     * body, so the trampoline route cannot drift from the lookup route. */
    g_pending_address = address;
    xdk_dispatch();
}

/* --- reporting ------------------------------------------------------------ */

void xdk_thunk_report(void)
{
    printf("owned virtual stream calls %llu, refused %llu (outside measured surface)\n",
           (unsigned long long)xdk_thunk_stream_virtual_call_count(),
           (unsigned long long)xdk_thunk_stream_virtual_refused_count());
    xdk_log_fn out = xdk_thunk_log();

    out("\n--- the XDK address boundary ---\n");
    if (g_row_count == 0u) {
        out("no surface adopted, so no XDK address is reachable. Generate\n"
            "src/xbox/xdk_surface.c and pass it to xdk_thunk_init.\n");
        return;
    }

    size_t per_module[XDK_MODULE_COUNT] = {0};
    for (size_t i = 0; i < g_row_count; i++) {
        per_module[g_rows[i].module]++;
    }
    out("surface        %zu address(es), 0x%08X..0x%08X\n", g_row_count, g_low, g_high);
    for (unsigned m = 0; m < (unsigned)XDK_MODULE_COUNT; m++) {
        out("  %-10s %4zu row(s), %llu dispatch(es)\n", xdk_module_name((xdk_module)m),
            per_module[m], (unsigned long long)xdk_thunk_module_call_count((xdk_module)m));
    }
    out("ABI known      %zu of %zu -- every other address STOPS the run rather than\n"
        "               being called with a guessed argument count\n",
        xdk_thunk_abi_count(), g_row_count);
    out("dispatches     %llu attempted, %llu refused for want of an ABI,\n"
        "               %llu unrouted, %llu outside the surface\n",
        (unsigned long long)xdk_thunk_call_count(),
        (unsigned long long)xdk_thunk_abi_refused_count(),
        (unsigned long long)xdk_thunk_unrouted_count(),
        (unsigned long long)xdk_thunk_unknown_count());

    /*
     * Printed UNCONDITIONALLY, because the number above it is the one most likely to
     * be misread. MEASURED in docs/d3d8-usage.md §9.
     */
    out("\nWHAT THIS BOUNDARY CANNOT SEE, and it is not a small thing:\n"
        "  D3DDevice_SetRenderState is INLINED into game .text at 0x000211C0. It\n"
        "  reads D3D8's header table at 0x475B08, ORs bits into D3D8's dirty mask at\n"
        "  0x3E3AB8 (43 references from game chunks) and stores into the deferred\n"
        "  shadow array at 0x3E3CC0 (3 references). Sampler state is written the same\n"
        "  way, into 0x3E3AC0 + stage*0x80 (5 references). Those are not calls, so no\n"
        "  address-keyed dispatcher intercepts them. A full count here does NOT mean\n"
        "  all GPU traffic is intercepted.\n");
}
