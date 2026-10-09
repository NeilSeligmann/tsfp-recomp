/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_ROUTE_NAV_SPEC_H
#define TSFP_INPUT_XINPUT_ROUTE_NAV_SPEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_route_spec.h"

/* T1640: the grammars of the closed loop menu navigation (docs/t1640-nav-state-machine.md).
 *
 * Record line (a comment, an old host ignores it):
 *   # nav: at=P to=Q menu=ID select=SEL [activate] [expect=FIELDS] [timeout=MS] [retry=N]
 *   P <= Q are record poll positions. SEL = index:N (0 based cursor row), id:N (the row's item id, needs the menu's `items=`) or
 *   name:TEXT (TEXT percent encoded, %20 %25 %3D, matched case blind against the item names). FIELDS = a T1633 wait field list
 *   (xinput_route_event_wait_parse), no spaces. timeout=MS wall ms for the whole step (default 120000), retry=N lost presses
 *   tolerated in a row (default 5).
 *
 * Menu table (tools/data/menu_nav.txt, --route-nav-menus; the grammar is written down in that file's header), one record per line,
 * `#` starts a comment:
 *   widgets table=ADDR stride=N count=N [closing=S,S..]            the table of UI page widgets (at most one)
 *   menu ID [page=builder:VA|none] active=COND[,COND..] [ready=COND[,COND..]] [cursor=SPEC count=SPEC axis=v|h wrap=0|1]
 *        [itemid=SPEC] [items=walk(HEAD;next=+N;type=+N:T1|T2..;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK)]
 *        [names=MEM/STRIDE/ascii|utf16|ptr-ascii|ptr-utf16/MAXLEN] [select=A|B|X|Y|BLACK|WHITE|START|BACK] [back=BTN] [conf=TEXT]
 *        [dpad=0|1] [selectby=id|name|index]
 *   dpad=0: the menu does not take the d-pad (the editor menus are stick/pointer driven): a nav step never presses it, waits for the cursor to be on the
 *   target and activates. selectby: what the RECORDER writes as select= (default id; name for lists whose ids are positions)
 *   COND = the T1633 `mem=` syntax `[*]ADDR[+OFF][:W]OP VALUE[&MASK]` (no spaces) or a PAGE spec `@+OFF[:W]OP VALUE[&MASK]`,
 *   SPEC = `[*]ADDR[+OFF][:W][&MASK]` or `@+OFF[:W][&MASK]`. A page spec reads OFF bytes into the page widget `page=` selected: the
 *   slot among `count` slots at table + i * stride whose dword +0 is non zero, dword +8 equals the builder VA and dword +0x1B0 is
 *   not in `closing`; the highest state (dword +0x1B0), then the highest slot, wins. No such slot: the menu is not on screen. A menu
 *   without cursor= is a screen that is only waited for and activated (the title's press start). */
#define ROUTE_NAV_MAX_STEPS 64u
#define ROUTE_NAV_MAX_MENUS 32u
#define ROUTE_NAV_ID_MAX 32u        /* with the NUL */
#define ROUTE_NAV_NAME_MAX 64u      /* with the NUL */
#define ROUTE_NAV_SELECT_TEXT 112u  /* the select= token as written */
#define ROUTE_NAV_EXPECT_MAX 224u
#define ROUTE_NAV_LINE_MAX 512u     /* one `# nav:` line, without the newline */
#define ROUTE_NAV_MAX_ITEMS 256u    /* cursor rows a row list or name lookup reads */
#define ROUTE_NAV_MAX_NODES 512u    /* nodes one `items=walk` follows before it calls the chain cyclic */
#define ROUTE_NAV_MAX_TYPES 4u
#define ROUTE_NAV_MAX_CLOSING 4u
#define ROUTE_NAV_CURSOR_NONE 0xFFFFFFFFu /* "no row under the cursor yet" */
#define ROUTE_NAV_PAGE_STATE_OFFSET 0x1B0u /* the page widget's state dword */
#define ROUTE_NAV_PAGE_USED_OFFSET 0x0u
#define ROUTE_NAV_PAGE_BUILDER_OFFSET 0x8u
#define ROUTE_NAV_DPAD_GRACE_MS 3000u    /* dpad=0: the cursor must be on the target within this long, then the step FAILS */
#define ROUTE_NAV_ACTIVATE_REPRESS 1u     /* no `expect`: the select button is pressed once more when the menu did not react */
#define ROUTE_NAV_DEFAULT_TIMEOUT_MS 120000u
#define ROUTE_NAV_DEFAULT_RETRY 5u
#define ROUTE_NAV_RETRY_LIMIT 100u
/* The press timing, in host polls (the title polls once per frame). HOLD: polls the d-pad bit is down, GAP: polls it is up before
 * the next press (the title must see the release, a press is an edge). The cursor must change within GAP + LOST_EXTRA polls of the
 * release or the press counts as lost. */
#define ROUTE_NAV_HOLD_POLLS 2u
#define ROUTE_NAV_GAP_POLLS 3u
#define ROUTE_NAV_LOST_EXTRA_POLLS 2u
#define ROUTE_NAV_TIMING_LIMIT 120u
#define ROUTE_NAV_ACTIVATE_GRACE_MS 3000u /* no `expect`: the menu must have changed this long after the activation */
#define ROUTE_NAV_PROGRESS_MS 5000u
#define ROUTE_NAV_SELECT_DOWN 128u        /* an analog select button counts as down from this value (recorder side) */

typedef struct {
    unsigned hold, gap; /* polls, both >= 1 */
    unsigned extra;     /* polls after the gap the cursor may still take to change before the press counts as lost (0..limit) */
} route_nav_timing;

typedef enum { ROUTE_NAV_SELECT_INDEX = 1, ROUTE_NAV_SELECT_NAME, ROUTE_NAV_SELECT_ID } route_nav_select_kind;

typedef struct {
    uint64_t at, to;
    char menu[ROUTE_NAV_ID_MAX];
    route_nav_select_kind kind;
    uint32_t index;                      /* index:N, or id:N (the item id) */
    char name[ROUTE_NAV_NAME_MAX];       /* decoded */
    char select_text[ROUTE_NAV_SELECT_TEXT]; /* "index:3" or "name:Foo%20Bar" as written, for messages */
    bool activate;
    bool has_expect;
    char expect[ROUTE_NAV_EXPECT_MAX];   /* the FIELDS text */
    xinput_route_wait expect_wait;       /* parsed as "mark1:FIELDS" */
    uint64_t timeout_ms;
    unsigned retry;
} route_nav_step;

typedef enum { ROUTE_NAV_MODE_OFF = 0, ROUTE_NAV_MODE_ON, ROUTE_NAV_MODE_STRICT } route_nav_mode;

/* Defaults ROUTE_NAV_HOLD_POLLS:ROUTE_NAV_GAP_POLLS:ROUTE_NAV_LOST_EXTRA_POLLS. `HOLD:GAP[:EXTRA]`, HOLD and GAP 1..ROUTE_NAV_TIMING_LIMIT,
 * EXTRA 0..ROUTE_NAV_TIMING_LIMIT. */
route_nav_timing route_nav_timing_default(void);
bool route_nav_timing_parse(const char *spec, route_nav_timing *out, char *error, size_t error_size);
/* "on", "off" or "strict". */
bool route_nav_mode_parse(const char *text, route_nav_mode *out);

/* The text after `# nav:` of one record line. False with the reason in `error`. Nothing is cross checked here (see
 * route_nav_steps_check). */
bool route_nav_line_parse(const char *text, route_nav_step *out, char *error, size_t error_size);
/* Percent decoding of a name:TEXT (%XX, XX hex, the result printable ASCII 0x20..0x7E, 1..63 bytes). */
bool route_nav_percent_decode(const char *text, size_t length, char *out, size_t out_size, char *error, size_t error_size);
/* The steps as a list: at most ROUTE_NAV_MAX_STEPS, ascending and not overlapping (P of a step >= Q of the previous), within
 * `total` polls, and no mark of `marks` strictly inside (P, Q) (a mark exactly at P or Q is fine). Error names the step. */
bool route_nav_steps_check(const route_nav_step *steps, size_t count, const uint64_t *marks, size_t mark_count, uint64_t total,
                           char *error, size_t error_size);

typedef enum {
    ROUTE_NAV_NAMES_NONE = 0,
    ROUTE_NAV_NAMES_ASCII,
    ROUTE_NAV_NAMES_UTF16,
    ROUTE_NAV_NAMES_PTR_ASCII,
    ROUTE_NAV_NAMES_PTR_UTF16,
} route_nav_names_kind;

/* A memory reference or condition. `page`: the offset is inside the page widget the menu's page= selected (cond.address = OFF). */
typedef struct {
    route_cond cond;
    bool page;
} route_nav_ref;

typedef struct {
    route_nav_names_kind kind;
    route_nav_ref base; /* the table base: ADDR, or with `*` the pointer at ADDR plus OFF */
    unsigned stride, max_len;
} route_nav_names;

/* items=walk(...): the rows of a menu as a linked list in guest memory. */
typedef struct {
    bool present;
    route_nav_ref head; /* its VALUE is the first node */
    uint32_t next_off, type_off, id_off, text_off, flags_off; /* id/text/flags offset 0xFFFFFFFF: absent */
    uint32_t types[ROUTE_NAV_MAX_TYPES];
    unsigned type_count;
    uint32_t skip_mask, grey_mask;
} route_nav_items;

typedef struct {
    bool present;
    uint32_t table, stride, count;
    uint32_t closing[ROUTE_NAV_MAX_CLOSING];
    unsigned closing_count;
} route_nav_widgets;

/* How the recorder names the activated row: by item id (default), by its text, or by its cursor index. */
typedef enum { ROUTE_NAV_BY_ID = 0, ROUTE_NAV_BY_NAME, ROUTE_NAV_BY_INDEX } route_nav_selectby;

typedef struct {
    char id[ROUTE_NAV_ID_MAX];
    bool dpad_off;          /* dpad=0: the menu does not take the d-pad (pointer driven), the navigator never presses it, it only waits */
    route_nav_selectby selectby;
    bool page_builder;      /* page=builder:VA, else no page widget is needed */
    uint32_t builder;
    route_nav_widgets widgets; /* a copy of the table's widgets line, for a menu with page_builder */
    route_nav_ref active[ROUTE_COND_MAX];
    unsigned active_count;
    route_nav_ref ready[ROUTE_COND_MAX];
    unsigned ready_count;
    bool has_cursor;        /* cursor= and count= (a menu without them is only waited for and activated) */
    route_nav_ref cursor, count, itemid;
    bool has_itemid;
    bool vertical, wrap;
    route_nav_items items;
    route_nav_names names;
    int select_analog;      /* index into the analog run (A..WHITE = 0..5), -1: a digital select button */
    uint16_t select_digital;
    char select_name[8];
    char back_name[8];
    char conf[24];
    unsigned line;
} route_nav_menu;

typedef struct {
    route_nav_widgets widgets;
    route_nav_menu menus[ROUTE_NAV_MAX_MENUS];
    size_t count;
} route_nav_menu_table;

bool route_nav_menus_parse(const char *text, size_t length, route_nav_menu_table *out, char *error, size_t error_size);
const route_nav_menu *route_nav_menu_find(const route_nav_menu_table *table, const char *id);

#endif
