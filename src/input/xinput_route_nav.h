/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_ROUTE_NAV_H
#define TSFP_INPUT_XINPUT_ROUTE_NAV_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_route_nav_spec.h"
#include "xinput_source.h"

/* T1640: the closed loop menu navigation state machine (docs/t1640-nav-state-machine.md). FABRICATED input like the rest of the
 * route replay. One machine runs one nav step: it reads the menu's cursor and item count from guest memory, presses the d-pad
 * edge by edge until the cursor is on the target, optionally presses the select button and checks the post-condition.
 *
 * It is a function of its probe (guest memory, wall clock, the post-condition evaluator) and of the polls handed to
 * route_nav_poll, nothing global, so tests drive it with a fake menu. Per poll (host port 0 pad poll) it writes the pad state of
 * that poll and returns RUNNING, or one of the final statuses (the pad is then at rest, route_nav_message has the text):
 *   DONE      the target was reached (and activated, and the post-condition holds): the route jumps to the step's `to`
 *   FALLBACK  the step cannot run (unknown menu, unreadable cursor, ...) and nothing was pressed yet, mode `on`: the route plays
 *             the recorded presses [at,to) open loop
 *   FAILED    everything else: a timeout, a lost press retry exhaustion, an ambiguous name, a post-condition that does not hold,
 *             a step that cannot run in `strict` mode or after the first press */
typedef struct {
    xinput_route_read_fn read_mem; /* guest memory, widths 1, 2 and 4 */
    bool (*read_bytes)(uint32_t address, void *buffer, size_t length, void *user); /* names; NULL: name selection cannot run */
    uint64_t (*now_ms)(void *user);                                                /* required */
    /* The `expect` post-condition (optional, only called when the step has one). arm: take the baselines when the activation
     * begins. holds: `polls` since the activation press began, `started_ms` its wall time; fills `note` with what was seen. */
    void (*expect_arm)(unsigned step_number, void *user);
    bool (*expect_holds)(unsigned step_number, uint64_t polls, uint64_t started_ms, char *note, size_t note_size, void *user);
    void (*progress)(const char *line, void *user); /* every ROUTE_NAV_PROGRESS_MS while waiting; optional */
    void *user;
} route_nav_probe;

typedef enum { ROUTE_NAV_RUNNING = 0, ROUTE_NAV_DONE, ROUTE_NAV_FALLBACK, ROUTE_NAV_FAILED } route_nav_status;
typedef enum {
    ROUTE_NAV_PLAN = 0, /* waiting for the menu to be active and ready, then deciding the next press */
    ROUTE_NAV_PRESS,    /* the d-pad bit is down */
    ROUTE_NAV_RELEASE,  /* the d-pad bit is up, watching for the cursor change */
    ROUTE_NAV_ACT_PRESS,
    ROUTE_NAV_ACT_RELEASE,
    ROUTE_NAV_VERIFY,   /* the post-condition */
    ROUTE_NAV_END
} route_nav_state;

#define ROUTE_NAV_MESSAGE_MAX 2048u
typedef struct {
    const route_nav_step *step;
    const route_nav_menu *menu; /* NULL: the table has no such menu */
    unsigned number;            /* 1 based, for the messages */
    route_nav_mode mode;
    route_nav_timing timing;
    route_nav_probe probe;
    route_nav_state state;
    route_nav_status status;
    uint64_t start_ms, last_progress_ms, act_start_ms;
    uint64_t polls, phase_polls, act_polls;
    bool target_known;
    uint32_t target, count, cursor;
    uint32_t press_from, moved_to, expected_to;
    uint16_t press_bit;
    bool moved, act_started;
    uint32_t initial_count;
    /* The rows of an `items=walk` menu, reloaded at every plan. stops: the cursor rows the cursor can rest on (not skipped), in order. */
    unsigned row_count, stop_count;
    uint32_t stops[ROUTE_NAV_MAX_ITEMS];
    uint32_t row_id[ROUTE_NAV_MAX_ITEMS], row_flags[ROUTE_NAV_MAX_ITEMS];
    char row_text[ROUTE_NAV_MAX_ITEMS][ROUTE_NAV_NAME_MAX];
    unsigned represses;
    uint64_t grey_since_ms; /* +1 of the wall ms the target row was first seen greyed, 0 = not greyed */
    uint64_t dpad_since_ms; /* +1 of the wall ms a dpad=0 menu was first seen with the cursor off the target */
    bool list_names;        /* the failure message lists the names/ids even for an index: select */
    /* Statistics, valid for the tests and the log line. */
    unsigned presses, lost, anomalies, consecutive_lost;
    /* T1770: the step's own poll count when the menu was first on screen and ready (what the step waited for, a measurement only). */
    bool ready_seen;
    uint64_t ready_polls;
    char unmet[480];
    char message[ROUTE_NAV_MESSAGE_MAX];
} route_nav;

/* `timing` NULL: the defaults. The wall clock starts here. */
void route_nav_begin(route_nav *machine, const route_nav_step *step, unsigned number, const route_nav_menu *menu, route_nav_mode mode,
                     const route_nav_timing *timing, const route_nav_probe *probe);
route_nav_status route_nav_poll(route_nav *machine, uint64_t poll_index, xinput_pad_state *pad);
/* The final line (route FAILED: ... or route nav: step N falling back ...); empty while RUNNING and after DONE. */
const char *route_nav_message(const route_nav *machine);
/* T1640 recorder side: read what a menu shows, without a step. `scratch` is any route_nav (a recorder owns one: the row list lives in it).
 * The view is the machine's snapshot: the page (when the menu has page=builder:), active, ready, and with a cursor the cursor, count and
 * item id. */
typedef struct {
    bool page_found;
    uint32_t page_base;
    bool active, ready, cursor_ok, count_ok, itemid_ok;
    uint32_t cursor, count, itemid;
} route_nav_view;
void route_nav_view_read(route_nav *scratch, const route_nav_probe *probe, const route_nav_menu *menu, route_nav_view *view);
/* The row under the cursor of an `items=` menu: its id, whether the cursor skips it or it is greyed, and whether its id is unique in the list
 * (not 0xFFFFFFFF, no other row with it). false when the row list cannot be read or the cursor is not on a row. A menu without `items=`
 * gives true with known = false (nothing is known about its rows). */
typedef struct {
    bool known, skipped, greyed, unique_id;
    uint32_t id, flags;
    /* with_text: the row's text, and whether it is non empty and the only one of its text (ASCII, case blind) among the rows the cursor does not skip */
    char text[ROUTE_NAV_NAME_MAX];
    bool unique_text;
} route_nav_row_info;
bool route_nav_view_row(route_nav *scratch, const route_nav_probe *probe, const route_nav_menu *menu, const route_nav_view *view, bool with_text,
                        route_nav_row_info *info);

/* "menu=ID select=SEL" */
size_t route_nav_step_label(const route_nav_step *step, char *out, size_t out_size);

#endif
