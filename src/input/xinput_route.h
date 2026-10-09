/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_ROUTE_H
#define TSFP_INPUT_XINPUT_ROUTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_route_nav.h"
#include "xinput_route_spec.h"
#include "xinput_source.h"

/* T1616: the route player, a pad source that replays a recorded script (xinput_record.h) with three additions over the
 * plain script source (FABRICATED input like the script, docs/input-replay.md "Route replay"):
 *  - its own CURSOR into the record, advanced one per host poll but PAUSED (pad at rest) at a recorded mark whose
 *    wait (xinput_route_wait) is not yet satisfied, so a slow, variable length load no longer shifts the later inputs;
 *  - EVENTS for the host (mark reached after its wait, wait satisfied, wait timed out, record exhausted), used to fire
 *    the guarded poke at an exact route position;
 *  - HANDOVER: after the record is exhausted the `live` source (keyboard or gamepad) supplies the pad. During the
 *    replay the live source is still polled and its output discarded, so its held key model is current at the handover.
 * A mark at cursor C fires before the state at C is output, after the wait bound to it holds. */
/* T1633: what the route can observe besides guest memory. The host implements it (src/host/route_probe.c), tests fake it. A
 * `watch` is registered once per file/call condition at route creation (id >= 0, or < 0 when this host cannot observe it, which
 * refuses the route up front with a clear error) and then only read: monotonically growing counters, so the route needs no
 * history, only the baseline it takes at the start of each segment (the previous mark). */
typedef struct {
    uint64_t count;   /* events seen (opens for file-open, read calls for file-read, any file I/O for file-idle, calls) */
    uint64_t bytes;   /* bytes read (file-read) */
    uint64_t last_ms; /* now_ms() of the latest event, 0 if none */
} xinput_route_watch_state;
typedef struct {
    uint64_t changes;        /* fingerprint changes seen */
    uint64_t last_change_ms; /* now_ms() of the latest change, 0 if none */
} xinput_route_frame_state;
typedef struct {
    uint64_t (*now_ms)(void *user); /* NULL: CLOCK_MONOTONIC */
    int (*watch)(route_cond_kind kind, const char *text, uint32_t va, void *user); /* NULL: no file/call/frame conditions */
    bool (*watch_state)(int id, xinput_route_watch_state *out, void *user);
    bool (*frame_state)(xinput_route_frame_state *out, void *user);
    void *user;
} xinput_route_probe;

typedef enum {
    XINPUT_ROUTE_MARK = 1,    /* index = mark number (1 based) */
    XINPUT_ROUTE_WAIT_OK,     /* index = mark number, stalled = polls spent waiting */
    XINPUT_ROUTE_WAIT_TIMEOUT,/* index = mark number, stalled = max, the route has FAILED */
    XINPUT_ROUTE_END,         /* the record is exhausted, the live source (if any) has the pad from now on */
    XINPUT_ROUTE_WAIT_PROGRESS, /* T1633: an event wait is still unsatisfied, detail lists what each condition saw */
    /* T1640 closed loop menu navigation, index = the nav step number (1 based), cursor = the record position (NAV_START: `at`, NAV_OK:
     * `to` after the jump, else where the cursor froze), detail = the text to print (NAV_FAIL: the whole `route FAILED:` line, NAV_FALLBACK:
     * the whole `route nav: step N falling back ...` line). The steps' polls are in xinput_route_nav_polls_total. */
    XINPUT_ROUTE_NAV_START,
    XINPUT_ROUTE_NAV_OK,
    XINPUT_ROUTE_NAV_FALLBACK, /* the recorded presses of the step replay open loop */
    XINPUT_ROUTE_NAV_PROGRESS,
    XINPUT_ROUTE_NAV_FAIL,     /* the route has FAILED */
} xinput_route_event_kind;

typedef struct {
    xinput_route_event_kind kind;
    unsigned index;
    uint64_t cursor; /* record position */
    uint64_t poll;   /* host port 0 poll index */
    uint64_t stalled;
    uint64_t stalled_ms; /* T1633: wall ms spent in the current wait */
    const char *detail;  /* T1633: WAIT_OK / WAIT_PROGRESS / WAIT_TIMEOUT: one line per wait, what each condition saw; valid during the callback */
} xinput_route_event;

typedef struct {
    bool (*read_mem)(uint32_t address, unsigned width, uint32_t *value, void *user); /* NULL: mem waits never hold */
    void *mem_user;
    xinput_source_fn live; /* NULL: the pad is at rest after the record */
    void *live_user;
    void (*on_event)(const xinput_route_event *event, void *user); /* may be NULL */
    void *event_user;
    void (*on_poll)(uint64_t poll, uint64_t cursor, void *user); /* every poll, before anything else; may be NULL */
    void *poll_user;
    xinput_route_probe probe; /* T1633, all zero: no file/call/frame conditions, memory conditions as before */
} xinput_route_hooks;

#define XINPUT_ROUTE_PROGRESS_MS 5000u

typedef struct xinput_route xinput_route;

/* `marks` are record positions (non decreasing, each <= the script's total). Waits bind to a mark number that must
 * exist. The script must outlive the route. NULL with `error` on a bad argument. */
xinput_route *xinput_route_create(const xinput_script *script, const uint64_t *marks, size_t mark_count,
                                  const xinput_route_wait *waits, size_t wait_count, const xinput_route_hooks *hooks,
                                  char *error, size_t error_size);
void xinput_route_free(xinput_route *route);
bool xinput_route_source(uint64_t poll_index, xinput_pad_state *out, void *user);
void xinput_route_install(xinput_route *route);

uint64_t xinput_route_cursor(const xinput_route *route);
bool xinput_route_ended(const xinput_route *route);
bool xinput_route_failed(const xinput_route *route);
uint64_t xinput_route_stalled_total(const xinput_route *route);
uint64_t xinput_route_stalled_ms_total(const xinput_route *route); /* T1633: wall ms spent in waits that were satisfied */

/* T1640: closed loop menu navigation. The `steps` (the record's `# nav:` lines, validated by route_nav_steps_check) replace the
 * recorded presses [at,to) when the cursor reaches `at`: the route freezes the cursor, drives route_nav (xinput_route_nav.h) with the
 * guest memory of the hooks and jumps to `to` on success. `mode` OFF stores nothing. `menus` may be NULL (every step then falls back,
 * or fails in strict mode, as its menu is unknown). The steps and the table are copied. NULL timing: the defaults. False with `error`
 * (a step names a menu the table lacks in strict mode, an expect= condition this host cannot observe, bad arguments). */
typedef struct {
    const route_nav_step *steps;
    size_t step_count;
    const route_nav_menu_table *menus;
    route_nav_mode mode;
    const route_nav_timing *timing;
    bool (*read_bytes)(uint32_t address, void *buffer, size_t length, void *user); /* names; NULL: name: selects cannot run */
    void *bytes_user;
} xinput_route_nav_config;
bool xinput_route_set_nav(xinput_route *route, const xinput_route_nav_config *config, char *error, size_t error_size);
size_t xinput_route_nav_done(const xinput_route *route);      /* steps finished (DONE or FALLBACK) */
uint64_t xinput_route_nav_polls_total(const xinput_route *route); /* host polls spent inside nav steps */

#endif
