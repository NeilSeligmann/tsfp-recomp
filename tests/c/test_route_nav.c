/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1640: the closed loop menu navigation. The `# nav:` line and menu table grammars, the state machine driven against a fake
 * menu in fake guest memory (cursor moving on d-pad edges, with wrap, not-ready polls, swallowed presses, a menu that moves two
 * steps, a null indirection, an item name table), the route player integration (cursor freeze, jump to `to`, marks at either end,
 * fallback, failure) and the host wiring (host_route.c, record `# nav:` lines, the flags). FABRICATED input, synthetic data,
 * nothing of the title.
 */
#include "host_options.h"
#include "host_route.h"
#include "recomp_abi.h"
#include "xinput_record.h"
#include "xinput_route.h"
#include "xinput_route_nav.h"
#include "route_probe.h"
#include "xinput_nav_record.h"
#include "xinput_route_nav_spec.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)
/* Text equality with both sides printed on a failure. */
#define CHECK_STR(actual, expected)                                                                   \
    do {                                                                                              \
        checks++;                                                                                     \
        if (strcmp((actual), (expected)) != 0) {                                                      \
            printf("FAIL %s:%d  got '%s' want '%s'\n", __FILE__, __LINE__, (actual), (expected));     \
            failures++;                                                                               \
        }                                                                                             \
    } while (0)
#define CHECK_HAS(text, part)                                                                         \
    do {                                                                                              \
        checks++;                                                                                     \
        if (strstr((text), (part)) == NULL) {                                                         \
            printf("FAIL %s:%d  '%s' not in '%s'\n", __FILE__, __LINE__, (part), (text));             \
            failures++;                                                                               \
        }                                                                                             \
    } while (0)

/* ======================================================================================================================
 * The fake menu. Guest memory layout:
 *   0x1000 active word, 0x1004 ready word, 0x1008 pointer to the menu object (0 = null), 0x100C pointer to the names table
 *   0x2000 menu object: +0 cursor, +4 count        0x3000 names (stride 16 ascii | 32 utf16 | pointers + strings at 0x3800)
 * ==================================================================================================================== */
#define MS_DEFAULT_PER_POLL 16u
static unsigned g_ms_per_poll = MS_DEFAULT_PER_POLL; /* wall ms one host poll takes in the fake clock */
#define MS_PER_POLL g_ms_per_poll
#define ADDR_ACTIVE 0x1000u
#define ADDR_READY 0x1004u
#define ADDR_OBJECT_POINTER 0x1008u
#define ADDR_NAMES_POINTER 0x100Cu
#define ADDR_OBJECT 0x2000u
#define ADDR_NAMES 0x3000u
#define ADDR_STRINGS 0x3800u
/* The page widget world (E1 to E3): 6 slots at 0x8000, stride 0x23C, row nodes at 0xC000, strings at 0xE000. */
#define ADDR_WIDGETS 0x8000u
#define WIDGET_STRIDE 0x23Cu
#define WIDGET_SLOTS 6u
#define ADDR_NODES 0xC000u
#define ADDR_TEXTS 0xE000u
#define BUILDER_LIST 0x2D2350u
#define BUILDER_OTHER 0x2C8940u

/* Sparse guest memory: the main block plus the regions the real table (tools/data/menu_nav.txt) reads. */
enum { REGION_RAM, REGION_WIDGETS, REGION_GLOBALS, REGION_HEAP, REGION_COUNT };
typedef struct {
    uint8_t ram[0x10000];
    uint8_t widgets[0x3000];  /* 0x6FEBE8 .. */
    uint8_t globals[0x7000];  /* 0x78A000 .. */
    uint8_t heap[0x6000];     /* 0x42000000 .. */
    bool vertical;
    bool wrap;
    unsigned step;            /* rows one accepted press moves */
    bool invert;              /* the menu moves the wrong way */
    uint32_t swallow_mask;    /* bit i: the i-th d-pad press edge is swallowed */
    unsigned edges;
    unsigned not_ready_polls; /* the ready word is 0 for this many polls */
    bool a_closes, a_moves;
    unsigned activations;     /* accepted select presses */
    unsigned a_edges;         /* select press edges seen */
    uint32_t swallow_a_mask;  /* bit i: the i-th select edge is swallowed */
    bool null_after_edge;     /* the object pointer goes null at the first accepted press */
    uint16_t prev_buttons;
    uint8_t prev_a;
    uint64_t now;
    char trace[512];
    size_t trace_len;
    /* the live menu object: where its cursor and count are, which rows the cursor skips, the item ids by row (page world) */
    uint32_t cursor_addr, count_addr, itemid_addr, page_addr;
    uint32_t row_skip;        /* bit k: row k is skipped by the cursor */
    uint32_t row_ids[32];
    bool a_closes_page, a_opens_child;
    /* post-condition fake */
    unsigned arm_calls, arm_step, hold_calls;
    unsigned expect_after_polls; /* expect_holds is true once this many polls since the activation */
    uint64_t last_expect_polls;
    char progress[8][320];
    unsigned progress_count;
    unsigned reads;           /* read_mem calls, for the cost of the recorder side */
} world;
static world w;

static uint8_t *region_of(uint32_t address, size_t width)
{
    static const struct { uint32_t base; size_t size; } map[REGION_COUNT] = {
        {0u, sizeof w.ram}, {0x6FEBE8u, sizeof w.widgets}, {0x78A000u, sizeof w.globals}, {0x42000000u, sizeof w.heap}};
    uint8_t *data[REGION_COUNT] = {w.ram, w.widgets, w.globals, w.heap};
    for (int i = 0; i < REGION_COUNT; i++)
        if (address >= map[i].base && (uint64_t)address + width <= (uint64_t)map[i].base + map[i].size) return data[i] + (address - map[i].base);
    return NULL;
}
static void set32(uint32_t address, uint32_t value)
{
    uint8_t *at = region_of(address, 4u);
    if (at != NULL) memcpy(at, &value, 4u);
    else printf("test bug: set32 outside the fake memory 0x%X\n", (unsigned)address), failures++;
}
static uint32_t get32(uint32_t address)
{
    uint32_t value = 0u;
    const uint8_t *at = region_of(address, 4u);
    if (at != NULL) memcpy(&value, at, 4u);
    return value;
}
static uint32_t cursor_now(void) { return get32(w.cursor_addr); }

static bool fake_read_mem(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    w.reads++;
    const uint8_t *at = (width == 1u || width == 2u || width == 4u) ? region_of(address, width) : NULL;
    if (at == NULL) return false;
    uint32_t out = 0u;
    memcpy(&out, at, width);
    *value = out;
    return true;
}
static bool fake_read_bytes(uint32_t address, void *buffer, size_t length, void *user)
{
    (void)user;
    const uint8_t *at = region_of(address, length);
    if (at == NULL) return false;
    memcpy(buffer, at, length);
    return true;
}
static uint64_t fake_now(void *user)
{
    (void)user;
    return w.now;
}
static void fake_arm(unsigned step_number, void *user)
{
    (void)user;
    w.arm_calls++;
    w.arm_step = step_number;
}
static bool fake_expect(unsigned step_number, uint64_t polls, uint64_t started_ms, char *note, size_t note_size, void *user)
{
    (void)user;
    (void)started_ms;
    (void)step_number;
    w.hold_calls++;
    w.last_expect_polls = polls;
    snprintf(note, note_size, "WAIT mem=0x1010:4==0x1 (read 0x0)");
    return w.expect_after_polls != 0u && polls >= w.expect_after_polls;
}
static void fake_progress(const char *line, void *user)
{
    (void)user;
    if (w.progress_count < 8u) snprintf(w.progress[w.progress_count], sizeof w.progress[0], "%s", line);
    w.progress_count++;
}

static void reset_world(unsigned count, unsigned cursor, bool vertical, bool wrap)
{
    memset(&w, 0, sizeof w);
    g_ms_per_poll = MS_DEFAULT_PER_POLL;
    w.vertical = vertical;
    w.wrap = wrap;
    w.step = 1u;
    w.cursor_addr = ADDR_OBJECT;
    w.count_addr = ADDR_OBJECT + 4u;
    set32(ADDR_ACTIVE, 1u);
    set32(ADDR_READY, 1u);
    set32(ADDR_OBJECT_POINTER, ADDR_OBJECT);
    set32(ADDR_NAMES_POINTER, ADDR_NAMES);
    set32(ADDR_OBJECT, cursor);
    set32(ADDR_OBJECT + 4u, count);
}
static void set_name(unsigned index, const char *name) { snprintf((char *)&w.ram[ADDR_NAMES + 16u * index], 16u, "%s", name); }

/* ---- the page world ---- */
typedef struct {
    const char *text;
    uint32_t id, flags, type;
} fake_row;
static uint32_t g_text_stride = 16u; /* bytes one row text may take in the fake memory (63 characters need 64) */

/* Slot `slot` of the widget table: a live page of `builder` in `state`, its rows as a node chain (a decoy node of type 3 after the first
 * row) at `node_base`, texts at `text_base`. Returns the page address. */
static uint32_t build_page_at(uint32_t table, uint32_t stride, unsigned slot, uint32_t builder, uint32_t state, const fake_row *rows, unsigned count,
                              uint32_t cursor, uint32_t node_base, uint32_t text_base)
{
    const uint32_t page = table + slot * stride;
    set32(page + 0x0u, 1u);
    set32(page + 0x4u, 0x500000u + slot);
    set32(page + 0x8u, builder);
    set32(page + 0x170u, 0u);
    set32(page + 0x180u, count);
    set32(page + 0x184u, cursor);
    set32(page + 0x1B0u, state);
    uint32_t node = node_base, previous_next = page + 0x178u;
    for (unsigned i = 0u; i < count; i++) {
        for (int decoy = 0; decoy < (i == 1u ? 2 : 1); decoy++) { /* a non row node (type 3) in front of row 1 */
            const bool is_decoy = i == 1u && decoy == 0;
            set32(previous_next, node);
            set32(node + 0x0u, is_decoy ? 3u : rows[i].type);
            set32(node + 0x8u, 0u);
            set32(node + 0x14u, is_decoy ? 0x99u : rows[i].id);
            set32(node + 0x28u, is_decoy ? 0u : rows[i].flags);
            if (!is_decoy) {
                set32(node + 0x20u, text_base + g_text_stride * i);
                snprintf((char *)region_of(text_base + g_text_stride * i, g_text_stride), g_text_stride, "%s", rows[i].text);
            }
            previous_next = node + 0x8u;
            node += 0x30u;
        }
    }
    set32(page + 0x18Cu, cursor < count ? rows[cursor].id : 0xFFFFFFFFu);
    return page;
}
static uint32_t build_page(unsigned slot, uint32_t builder, uint32_t state, const fake_row *rows, unsigned count, uint32_t cursor, uint32_t node_base,
                           uint32_t text_base)
{
    return build_page_at(ADDR_WIDGETS, WIDGET_STRIDE, slot, builder, state, rows, count, cursor, node_base, text_base);
}
/* The page the fake game drives. */
static void drive_page(uint32_t page, const fake_row *rows, unsigned count)
{
    w.page_addr = page;
    w.cursor_addr = page + 0x184u;
    w.count_addr = page + 0x180u;
    w.itemid_addr = page + 0x18Cu;
    w.row_skip = 0u;
    for (unsigned i = 0u; i < count && i < 32u; i++) {
        w.row_ids[i] = rows[i].id;
        if ((rows[i].flags & 0x4u) != 0u) w.row_skip |= 1u << i;
    }
    set32(w.itemid_addr, get32(w.cursor_addr) < count ? rows[get32(w.cursor_addr)].id : 0xFFFFFFFFu);
}

/* One accepted press: move the cursor `step` stops in `direction`, over the rows the cursor skips. */
static void fake_move(int direction)
{
    const uint32_t count = get32(w.count_addr);
    uint32_t cursor = get32(w.cursor_addr);
    for (unsigned k = 0u; k < w.step; k++) {
        if (cursor == ROUTE_NAV_CURSOR_NONE) {
            cursor = direction > 0 ? 0u : count - 1u;
            while (cursor < count && ((w.row_skip >> cursor) & 1u) != 0u) cursor = direction > 0 ? cursor + 1u : cursor - 1u;
            continue;
        }
        int64_t index = cursor;
        for (uint32_t tries = 0u; tries < count; tries++) {
            index += direction;
            if (index < 0 || index >= (int64_t)count) {
                if (!w.wrap) { index = cursor; break; }
                index = (index + (int64_t)count) % (int64_t)count;
            }
            if (((w.row_skip >> index) & 1u) == 0u) break;
        }
        cursor = (uint32_t)index;
    }
    set32(w.cursor_addr, cursor);
    if (w.itemid_addr != 0u) set32(w.itemid_addr, cursor < count && cursor < 32u ? w.row_ids[cursor] : 0xFFFFFFFFu);
}

/* The fake game: edge detection on the pad of this poll, applied after the machine decided it. */
static void apply_pad(const xinput_pad_state *pad)
{
    const uint16_t increase = w.vertical ? XINPUT_BUTTON_DPAD_DOWN : XINPUT_BUTTON_DPAD_RIGHT;
    const uint16_t decrease = w.vertical ? XINPUT_BUTTON_DPAD_UP : XINPUT_BUTTON_DPAD_LEFT;
    const uint16_t rising = (uint16_t)(pad->digital_buttons & ~w.prev_buttons);
    if ((rising & (increase | decrease)) != 0u) {
        const unsigned index = w.edges++;
        if (((w.swallow_mask >> index) & 1u) == 0u) {
            int direction = (rising & increase) != 0u ? 1 : -1;
            if (w.invert) direction = -direction;
            fake_move(direction);
            if (w.null_after_edge) set32(ADDR_OBJECT_POINTER, 0u);
        }
    }
    if (pad->analog[0] == 255u && w.prev_a != 255u) {
        const unsigned edge = w.a_edges++;
        if (((w.swallow_a_mask >> edge) & 1u) == 0u) {
            w.activations++;
            if (w.a_closes) set32(ADDR_ACTIVE, 0u);
            if (w.a_closes_page) set32(w.page_addr + 0x1B0u, 5u);
            if (w.a_opens_child) set32(w.page_addr + 0x170u, 1u);
            if (w.a_moves) set32(w.cursor_addr, cursor_now() + 1u);
        }
    }
    w.prev_buttons = pad->digital_buttons;
    w.prev_a = pad->analog[0];
}

static char trace_char(const xinput_pad_state *pad)
{
    if (pad->analog[0] == 255u) return 'A';
    if ((pad->digital_buttons & XINPUT_BUTTON_DPAD_DOWN) != 0u) return 'D';
    if ((pad->digital_buttons & XINPUT_BUTTON_DPAD_UP) != 0u) return 'U';
    if ((pad->digital_buttons & XINPUT_BUTTON_DPAD_LEFT) != 0u) return 'L';
    if ((pad->digital_buttons & XINPUT_BUTTON_DPAD_RIGHT) != 0u) return 'R';
    if ((pad->digital_buttons & XINPUT_BUTTON_START) != 0u) return 'S';
    return '.';
}

/* ---- machine harness ---- */
static route_nav_menu_table g_table;
static route_nav_step g_step;
static route_nav g_machine;
static unsigned g_polls_run;

/* The menu line over the fake memory. `extra` is appended (names=...). */
static const char *g_ready_spec = "0x1004:4==1"; /* a test may point the ready condition elsewhere */
static void menu_table_text(char *out, size_t size, const char *axis, int wrap, const char *extra)
{
    snprintf(out, size,
             "# fake menu\nmenu list active=0x1000:4==1 ready=%s cursor=*0x1008:4 count=*0x1008+4:4 axis=%s wrap=%d %s\n", g_ready_spec, axis, wrap,
             extra != NULL ? extra : "");
}

/* Parse the step and the table and start the machine. `menu_id` NULL: the real id "list". */
static void begin(const char *nav_text, const char *axis, int wrap, const char *extra, route_nav_mode mode, const route_nav_timing *timing)
{
    char text[600], error[300];
    menu_table_text(text, sizeof text, axis, wrap, extra);
    CHECK(route_nav_menus_parse(text, strlen(text), &g_table, error, sizeof error));
    CHECK(route_nav_line_parse(nav_text, &g_step, error, sizeof error));
    const route_nav_probe probe = {fake_read_mem, fake_read_bytes, fake_now, fake_arm, fake_expect, fake_progress, NULL};
    w.now = 0u;
    route_nav_begin(&g_machine, &g_step, 1u, route_nav_menu_find(&g_table, g_step.menu), mode, timing, &probe);
    g_polls_run = 0u;
}

/* Run until the machine stops or `max_polls` polls pass. Returns the status, the trace is in w.trace. */
static void (*g_pre_poll)(unsigned poll); /* a test hook run before the machine's poll */
static route_nav_status drive(unsigned max_polls)
{
    for (unsigned poll = 0u; poll < max_polls; poll++) {
        w.now = (uint64_t)poll * MS_PER_POLL;
        if (g_pre_poll != NULL) g_pre_poll(poll);
        set32(ADDR_READY, poll >= w.not_ready_polls ? 1u : 0u);
        xinput_pad_state pad;
        const route_nav_status status = route_nav_poll(&g_machine, poll, &pad);
        if (w.trace_len + 1u < sizeof w.trace) {
            w.trace[w.trace_len++] = trace_char(&pad);
            w.trace[w.trace_len] = '\0';
        }
        apply_pad(&pad);
        g_polls_run = poll + 1u;
        if (status != ROUTE_NAV_RUNNING) return status;
    }
    return ROUTE_NAV_RUNNING;
}

/* One character per press (a run of the same direction), e.g. "DDU". */
static void press_summary(char *out, size_t size)
{
    size_t used = 0u;
    char previous = '.';
    for (size_t i = 0u; i < w.trace_len && used + 1u < size; i++) {
        const char c = w.trace[i];
        if ((c == 'D' || c == 'U' || c == 'L' || c == 'R') && c != previous) out[used++] = c;
        previous = c;
    }
    out[used] = '\0';
}

#define LINE_BASIC(select) "at=10 to=40 menu=list select=" select
#define LINE_ACTIVATE(select) "at=10 to=40 menu=list select=" select " activate"

/* ======================================================================================================================
 * Grammar
 * ==================================================================================================================== */
static void test_nav_line_parser(void)
{
    route_nav_step step;
    char error[300];
    CHECK(route_nav_line_parse("at=10 to=40 menu=map-list select=index:3", &step, error, sizeof error));
    CHECK(step.at == 10u && step.to == 40u && strcmp(step.menu, "map-list") == 0 && step.kind == ROUTE_NAV_SELECT_INDEX && step.index == 3u);
    CHECK(!step.activate && !step.has_expect && step.timeout_ms == 120000u && step.retry == 5u); /* the defaults */
    CHECK(strcmp(step.select_text, "index:3") == 0);
    CHECK(route_nav_line_parse("at=0 to=0 menu=m select=index:0 activate", &step, error, sizeof error) && step.activate && step.at == 0u && step.to == 0u);
    CHECK(route_nav_line_parse("at=5 to=9 menu=m select=index:1 activate expect=file-open=anicemap.mkr,mem=0x1010==1 timeout=90000 retry=2", &step, error,
                               sizeof error));
    CHECK(step.has_expect && step.timeout_ms == 90000u && step.retry == 2u && step.expect_wait.cond_count == 2u &&
          step.expect_wait.conds[0].kind == RCOND_FILE_OPEN && step.expect_wait.conds[1].kind == RCOND_MEM);
    CHECK(strcmp(step.expect, "file-open=anicemap.mkr,mem=0x1010==1") == 0);
    /* fields in another order, extra spaces */
    CHECK(route_nav_line_parse("  select=index:7   menu=m to=3 at=3 retry=0 ", &step, error, sizeof error) && step.retry == 0u && step.index == 7u);
    /* percent decoding */
    CHECK(route_nav_line_parse("at=1 to=2 menu=m select=name:Cave%20of%20%25%3D%7E", &step, error, sizeof error));
    CHECK_STR(step.name, "Cave of %=~");
    CHECK(step.kind == ROUTE_NAV_SELECT_NAME && strcmp(step.select_text, "name:Cave%20of%20%25%3D%7E") == 0);
    CHECK(route_nav_line_parse("at=1 to=2 menu=m select=name:lower%2fcase%2F", &step, error, sizeof error));
    CHECK_STR(step.name, "lower/case/"); /* hex digits of either case */

    /* every refusal names its reason */
    const struct { const char *line; const char *reason; } bad[] = {
        {"at=x to=4 menu=m select=index:1", "bad number in at=x"},
        {"at=1 to=4x menu=m select=index:1", "bad number in to=4x"},
        {"at=-1 to=4 menu=m select=index:1", "bad number in at=-1"},
        {"at= to=4 menu=m select=index:1", "bad number in at="},
        {"at=9 to=4 menu=m select=index:1", "at=9 is above to=4"},
        {"to=4 menu=m select=index:1", "missing at="},
        {"at=1 menu=m select=index:1", "missing to="},
        {"at=1 to=4 select=index:1", "missing menu="},
        {"at=1 to=4 menu=m", "missing select="},
        {"at=1 to=4 menu=m select=3", "want index:N, id:N or name:TEXT"},
        {"at=1 to=4 menu=m select=index:", "want index:N, id:N or name:TEXT"},
        {"at=1 to=4 menu=m select=index:x", "bad number in select=index:x"},
        {"at=1 to=4 menu=m select=name:", "want index:N, id:N or name:TEXT"},
        {"at=1 to=4 menu=m select=id:", "want index:N, id:N or name:TEXT"},
        {"at=1 to=4 menu=m select=id:4294967296", "bad number in select=id:4294967296"},
        {"at=1 to=4 menu=m select=name:a%zz", "bad percent escape"},
        {"at=1 to=4 menu=m select=name:a%2", "bad percent escape"},
        {"at=1 to=4 menu=m select=name:a%0Ab", "outside printable ASCII"},
        {"at=1 to=4 menu=m select=index:1 select=index:2", "'select' given twice"},
        {"at=1 to=4 menu=m select=index:1 bogus=2", "unknown field 'bogus=2'"},
        {"at=1 to=4 menu=m select=index:1 expect=bogus=1 activate", "expect=bogus=1"},
        {"at=1 to=4 menu=m select=index:1 expect=min=3", "give `activate` too"},
        {"at=1 to=4 menu=m select=index:1 timeout=0", "bad number in timeout=0"},
        {"at=1 to=4 menu=m select=index:1 retry=101", "bad number in retry=101"},
        {"at=1 to=4 menu=bad/id select=index:1", "bad menu id"},
        {"at=1 to=4 menu=m select=index:1 activate activate", "'activate' given twice"},
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        error[0] = '\0';
        const bool parsed = route_nav_line_parse(bad[i].line, &step, error, sizeof error);
        CHECK(!parsed);
        CHECK(error[0] != '\0');
        if (error[0] != '\0') CHECK_HAS(error, bad[i].reason);
    }
    char longline[700];
    memset(longline, 'a', sizeof longline);
    longline[sizeof longline - 1u] = '\0';
    CHECK(!route_nav_line_parse(longline, &step, error, sizeof error) && strstr(error, "longer than") != NULL);
    /* a 63 byte name fits, 64 does not */
    char name_line[200];
    snprintf(name_line, sizeof name_line, "at=1 to=2 menu=m select=name:%.63s", "0123456789012345678901234567890123456789012345678901234567890123456789");
    CHECK(route_nav_line_parse(name_line, &step, error, sizeof error) && strlen(step.name) == 63u);
    snprintf(name_line, sizeof name_line, "at=1 to=2 menu=m select=name:%.64s", "0123456789012345678901234567890123456789012345678901234567890123456789");
    CHECK(!route_nav_line_parse(name_line, &step, error, sizeof error) && strstr(error, "longer than 63") != NULL);
}

static void test_steps_check(void)
{
    static route_nav_step steps[ROUTE_NAV_MAX_STEPS + 1u];
    char error[300];
    memset(steps, 0, sizeof steps);
    for (size_t i = 0u; i < ROUTE_NAV_MAX_STEPS + 1u; i++) {
        steps[i].at = i * 10u;
        steps[i].to = i * 10u + 5u;
    }
    CHECK(route_nav_steps_check(steps, ROUTE_NAV_MAX_STEPS, NULL, 0u, 10000u, error, sizeof error)); /* exactly 64 */
    CHECK(!route_nav_steps_check(steps, ROUTE_NAV_MAX_STEPS + 1u, NULL, 0u, 10000u, error, sizeof error));
    CHECK_HAS(error, "65 nav lines");
    CHECK(route_nav_steps_check(steps, 0u, NULL, 0u, 0u, error, sizeof error)); /* none is fine */
    /* ascending, touching ranges are fine, overlap is not */
    route_nav_step pair[2];
    memset(pair, 0, sizeof pair);
    pair[0].at = 10u; pair[0].to = 20u;
    pair[1].at = 20u; pair[1].to = 30u;
    CHECK(route_nav_steps_check(pair, 2u, NULL, 0u, 100u, error, sizeof error));
    pair[1].at = 19u;
    CHECK(!route_nav_steps_check(pair, 2u, NULL, 0u, 100u, error, sizeof error));
    CHECK_HAS(error, "nav step 2");
    CHECK_HAS(error, "overlaps");
    pair[0].at = 40u; pair[0].to = 50u; pair[1].at = 10u; pair[1].to = 15u; /* descending */
    CHECK(!route_nav_steps_check(pair, 2u, NULL, 0u, 100u, error, sizeof error));
    /* past the record */
    pair[0].at = 10u; pair[0].to = 101u;
    CHECK(!route_nav_steps_check(pair, 1u, NULL, 0u, 100u, error, sizeof error));
    CHECK_HAS(error, "past the record");
    pair[0].to = 100u;
    CHECK(route_nav_steps_check(pair, 1u, NULL, 0u, 100u, error, sizeof error)); /* exactly the end */
    /* marks: at either end is fine, strictly inside is not */
    const uint64_t at_ends[] = {10u, 20u};
    const uint64_t inside[] = {15u};
    const uint64_t just_inside_low[] = {11u};
    const uint64_t just_inside_high[] = {19u};
    pair[0].at = 10u; pair[0].to = 20u;
    CHECK(route_nav_steps_check(pair, 1u, at_ends, 2u, 100u, error, sizeof error));
    CHECK(!route_nav_steps_check(pair, 1u, inside, 1u, 100u, error, sizeof error));
    CHECK_HAS(error, "mark 1 at 15 lies inside");
    CHECK(!route_nav_steps_check(pair, 1u, just_inside_low, 1u, 100u, error, sizeof error));
    CHECK(!route_nav_steps_check(pair, 1u, just_inside_high, 1u, 100u, error, sizeof error));
    /* an empty range (at == to) holds no mark strictly inside it */
    pair[0].at = 15u; pair[0].to = 15u;
    CHECK(route_nav_steps_check(pair, 1u, inside, 1u, 100u, error, sizeof error));
}

static void test_menu_table_parser(void)
{
    static route_nav_menu_table table;
    char error[300];
    const char *good =
        "# menu table\n\n"
        "menu map-list active=0x1000:4==1,*0x2000+8:1!=0 cursor=*0x1008+4:1&0xF count=0x1010:2 axis=v wrap=1 # trailing comment\n"
        "menu opts active=0x1000:4==2 ready=0x1004:4==1,0x1008:4>=3 cursor=0x1010:4 count=0x1014:4 axis=h wrap=0 select=START conf=MEASURED\n"
        "menu named active=0x1000:4==3 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 names=*0x3000+4/32/utf16/20 select=WHITE\n"
        "menu ptrs active=0x1000:4==4 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 names=0x3000/4/ptr-ascii/24\n";
    CHECK(route_nav_menus_parse(good, strlen(good), &table, error, sizeof error));
    CHECK(table.count == 4u);
    const route_nav_menu *map = route_nav_menu_find(&table, "map-list");
    CHECK(map != NULL && map->active_count == 2u && map->ready_count == 0u && map->vertical && map->wrap && map->line == 3u);
    CHECK(map != NULL && map->active[1].cond.indirect && map->active[1].cond.offset == 8u && map->active[1].cond.width == 1u &&
          map->active[1].cond.cmp == RCMP_NE && !map->active[1].page);
    CHECK(map != NULL && map->cursor.cond.indirect && map->cursor.cond.offset == 4u && map->cursor.cond.width == 1u && map->cursor.cond.mask == 0xFu);
    CHECK(map != NULL && !map->count.cond.indirect && map->count.cond.address == 0x1010u && map->count.cond.width == 2u);
    CHECK(map != NULL && map->has_cursor && !map->page_builder && !map->has_itemid && !map->items.present);
    CHECK(map != NULL && map->select_analog == 0 && strcmp(map->select_name, "A") == 0 && map->names.kind == ROUTE_NAV_NAMES_NONE);
    const route_nav_menu *opts = route_nav_menu_find(&table, "opts");
    CHECK(opts != NULL && !opts->vertical && !opts->wrap && opts->ready_count == 2u && opts->ready[1].cond.cmp == RCMP_GE);
    CHECK(opts != NULL && opts->select_analog == -1 && opts->select_digital == XINPUT_BUTTON_START && strcmp(opts->conf, "MEASURED") == 0);
    const route_nav_menu *named = route_nav_menu_find(&table, "named");
    CHECK(named != NULL && named->names.kind == ROUTE_NAV_NAMES_UTF16 && named->names.stride == 32u && named->names.max_len == 20u &&
          named->names.base.cond.indirect && named->names.base.cond.offset == 4u && named->select_analog == 5);
    const route_nav_menu *ptrs = route_nav_menu_find(&table, "ptrs");
    CHECK(ptrs != NULL && ptrs->names.kind == ROUTE_NAV_NAMES_PTR_ASCII && ptrs->names.stride == 4u && !ptrs->names.base.cond.indirect &&
          ptrs->names.base.cond.address == 0x3000u);
    CHECK(route_nav_menu_find(&table, "nope") == NULL && route_nav_menu_find(NULL, "opts") == NULL);
    /* the empty table is valid */
    CHECK(route_nav_menus_parse("# nothing\n\n", 11u, &table, error, sizeof error) && table.count == 0u);

    const char *head = "menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0";
    char text[400];
    const struct { const char *tail; const char *reason; } bad[] = {
        {" bogus=1", "line 1: unknown key 'bogus'"},
        {" axis=h", "'axis' given twice"},
        {" ready=0x1004:4==1 ready=0x1004:4==1", "'ready' given twice"},
        {" names=0x3000/4/binary/20", "unknown encoding 'binary'"},
        {" names=0x3000/0/ascii/20", "names=: want MEM/STRIDE"},
        {" names=0x3000/4/ascii/64", "names=: want MEM/STRIDE"},
        {" names=0x3000/4/ascii", "names=: want MEM/STRIDE"},
        {" select=Z", "select= wants"},
        {" cursor", "not key=value"},
        {" itemid=@0x10", "bad memory reference"},
        {" ready=@+0x1B0:4", "bad condition"},
        {" conf=", "conf= wants"},
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(text, sizeof text, "%s%s\n", head, bad[i].tail);
        error[0] = '\0';
        CHECK(!route_nav_menus_parse(text, strlen(text), &table, error, sizeof error));
        CHECK(error[0] != '\0');
        if (error[0] != '\0') CHECK_HAS(error, bad[i].reason);
    }
    /* the line number is the line of the offence */
    snprintf(text, sizeof text, "# a\n%s\n\n%s bogus=1\n", head, "menu n active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0");
    CHECK(!route_nav_menus_parse(text, strlen(text), &table, error, sizeof error));
    CHECK_HAS(error, "line 4:");
    const struct { const char *line; const char *reason; } missing[] = {
        {"menu m cursor=0x1010:4 count=0x1014:4 axis=v wrap=0\n", "is missing active="},
        {"menu m active=0x1000:4==1 count=0x1014:4 axis=v wrap=0\n", "needs cursor= and count= together"},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 axis=v wrap=0\n", "needs cursor= and count= together"},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 wrap=0\n", "is missing axis="},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v\n", "is missing wrap="},
        {"menu m active=0x1000:4==1 axis=v wrap=0 itemid=0x1010:4\n", "without a cursor"},
        {"menu m active=@+0x1B0:4==3\n", "has no page=builder:VA"},
        {"menu m page=builder:0x2D2350 active=0x1000:4==1\n", "no widgets line"},
        {"widgets table=0x6FEBE8 stride=0x23C count=20\nwidgets table=0x6FEBE8 stride=0x23C count=20\n", "a second widgets line"},
        {"widgets table=0x6FEBE8 stride=0x23C\n", "widgets needs table=, stride= and count="},
        {"widgets table=0x6FEBE8 stride=0x10 count=20\n", "widgets: bad, repeated or unknown field"},
        {"widgets table=0x6FEBE8 stride=0x23C count=20 closing=\n", "closing="},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 items=walk(@+0x178:4;type=+0:4)\n", "items=walk: needs next="},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 items=walk(0x1010:4;next=+8;type=+0:4;skip=4)\n", "needs next=, type= and flags="},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 items=walk(0x1010:4;next=+8;type=+0:4|5|6|7|8)\n", "bad type list"},
        {"menu m active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0 items=lines\n", "items=: want walk("},
        {"menu m page=sideways active=0x1000:4==1\n", "page= wants none or builder:0xVA"},
    };
    for (size_t i = 0u; i < sizeof missing / sizeof missing[0]; i++) {
        CHECK(!route_nav_menus_parse(missing[i].line, strlen(missing[i].line), &table, error, sizeof error));
        CHECK(error[0] != '\0');
        CHECK_HAS(error, missing[i].reason);
    }
    snprintf(text, sizeof text, "%s\n%s\n", head, head);
    CHECK(!route_nav_menus_parse(text, strlen(text), &table, error, sizeof error));
    CHECK_HAS(error, "already defined on line 1");
    CHECK(!route_nav_menus_parse("menu bad/id active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0\n", 80u, &table, error, sizeof error));
    CHECK(!route_nav_menus_parse("menu m active=0x1001:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0\n", 76u, &table, error, sizeof error)); /* unaligned */
    CHECK_HAS(error, "bad condition");
    CHECK(!route_nav_menus_parse("menu m active=0x1000:4==1 cursor=0x1010:4==3 count=0x1014:4 axis=v wrap=0\n", 80u, &table, error, sizeof error));
    CHECK_HAS(error, "bad memory reference");
    /* more than ROUTE_NAV_MAX_MENUS menus */
    static char big[ROUTE_NAV_MAX_MENUS * 100u + 200u];
    size_t used = 0u;
    for (unsigned i = 0u; i < ROUTE_NAV_MAX_MENUS + 1u; i++)
        used += (size_t)snprintf(big + used, sizeof big - used, "menu m%u active=0x1000:4==1 cursor=0x1010:4 count=0x1014:4 axis=v wrap=0\n", i);
    CHECK(!route_nav_menus_parse(big, used, &table, error, sizeof error));
    CHECK_HAS(error, "more than 32 menus");
}

static void test_timing_and_mode(void)
{
    route_nav_timing timing = route_nav_timing_default();
    char error[200];
    CHECK(timing.hold == 2u && timing.gap == 3u && timing.extra == 2u);
    CHECK(route_nav_timing_parse("4:7", &timing, error, sizeof error) && timing.hold == 4u && timing.gap == 7u && timing.extra == 2u);
    CHECK(route_nav_timing_parse("4:7:11", &timing, error, sizeof error) && timing.hold == 4u && timing.gap == 7u && timing.extra == 11u);
    CHECK(route_nav_timing_parse("1:1:0", &timing, error, sizeof error) && timing.extra == 0u);
    CHECK(!route_nav_timing_parse("1:1:121", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("1:1:", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("1:1:2:3", &timing, error, sizeof error));
    CHECK(route_nav_timing_parse("1:1", &timing, error, sizeof error) && timing.hold == 1u && timing.gap == 1u);
    CHECK(route_nav_timing_parse("120:120", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("0:3", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("3:0", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("121:3", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse("3", &timing, error, sizeof error) && strstr(error, "HOLD:GAP[:EXTRA]") != NULL);
    CHECK(!route_nav_timing_parse("a:b", &timing, error, sizeof error));
    CHECK(!route_nav_timing_parse(NULL, &timing, error, sizeof error));
    route_nav_mode mode = ROUTE_NAV_MODE_OFF;
    CHECK(route_nav_mode_parse("on", &mode) && mode == ROUTE_NAV_MODE_ON);
    CHECK(route_nav_mode_parse("strict", &mode) && mode == ROUTE_NAV_MODE_STRICT);
    CHECK(route_nav_mode_parse("off", &mode) && mode == ROUTE_NAV_MODE_OFF);
    CHECK(!route_nav_mode_parse("ON", &mode) && !route_nav_mode_parse("", &mode) && !route_nav_mode_parse(NULL, &mode));
}

/* ======================================================================================================================
 * The state machine
 * ==================================================================================================================== */
static void test_machine_exact_pad_output(void)
{
    /* 0 -> 2, no wrap, vertical, no activation: HOLD 2 polls DOWN, GAP 3 polls rest, twice, then done on the next poll */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "DD...DD....");
    CHECK(g_machine.presses == 2u && g_machine.lost == 0u && cursor_now() == 2u && g_polls_run == 11u);
    CHECK_STR(route_nav_message(&g_machine), "");
    /* with the activation: A (analog 255) for HOLD polls, rest for GAP, then the verdict */
    reset_world(5u, 0u, true, false);
    w.a_closes = true;
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "DD...DD...AA....");
    CHECK(w.activations == 1u && cursor_now() == 2u);
    /* the exact button bits of a poll: DOWN is 0x0002 alone, A is analog 0 alone */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    xinput_pad_state pad;
    w.now = 0u;
    CHECK(route_nav_poll(&g_machine, 0u, &pad) == ROUTE_NAV_RUNNING);
    CHECK(pad.digital_buttons == XINPUT_BUTTON_DPAD_DOWN && XINPUT_BUTTON_DPAD_DOWN == 0x0002u);
    for (unsigned i = 0u; i < XINPUT_ANALOG_COUNT; i++) CHECK(pad.analog[i] == 0u);
    CHECK(pad.thumb_left_x == 0 && pad.thumb_left_y == 0 && pad.thumb_right_x == 0 && pad.thumb_right_y == 0);
    /* custom timing 3:1 and 1:2 */
    reset_world(5u, 0u, true, false);
    const route_nav_timing slow = {3u, 1u, 2u};
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, &slow);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "DDD.DDD..");
    reset_world(5u, 0u, true, false);
    const route_nav_timing quick = {1u, 2u, 2u};
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, &quick);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "D..D...");
    /* the select button of the menu is used: START is a digital bit, not analog */
    reset_world(5u, 1u, true, false);
    w.a_closes = true;
    begin(LINE_ACTIVATE("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    g_machine.menu = &g_table.menus[0];
    ((route_nav_menu *)g_machine.menu)->select_analog = -1;
    ((route_nav_menu *)g_machine.menu)->select_digital = XINPUT_BUTTON_START;
    w.now = 0u;
    CHECK(route_nav_poll(&g_machine, 0u, &pad) == ROUTE_NAV_RUNNING);
    CHECK(pad.digital_buttons == XINPUT_BUTTON_START && pad.analog[0] == 0u);
}

static void test_machine_already_on_target(void)
{
    reset_world(5u, 3u, true, false);
    w.a_closes = true;
    begin(LINE_ACTIVATE("index:3"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "AA....");
    CHECK(g_machine.presses == 0u && w.edges == 0u && w.activations == 1u);
    /* without activate: done on the first poll, nothing pressed at all */
    reset_world(5u, 3u, true, false);
    begin(LINE_BASIC("index:3"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, ".");
    CHECK(g_machine.presses == 0u && w.activations == 0u && g_polls_run == 1u);
}

static void test_machine_directions(void)
{
    char summary[80];
    /* wrap: 8 items, 1 -> 7 is two UP presses through 0, the long way would be 6 DOWN */
    reset_world(8u, 1u, true, true);
    begin(LINE_BASIC("index:7"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "UU");
    CHECK(cursor_now() == 7u);
    /* wrap the other way: 6 -> 1 is three DOWN (6,7,0,1), the long way would be 5 UP */
    reset_world(8u, 6u, true, true);
    begin(LINE_BASIC("index:1"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DDD");
    CHECK(cursor_now() == 1u);
    /* wrap, no wrap needed: 2 -> 4 is DOWN DOWN either way */
    reset_world(8u, 2u, true, true);
    begin(LINE_BASIC("index:4"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DD");
    /* a tie goes increasing: 8 items, 0 -> 4 is four DOWN, and 6 items 1 -> 4 too */
    reset_world(8u, 0u, true, true);
    begin(LINE_BASIC("index:4"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DDDD");
    reset_world(8u, 6u, true, true);
    begin(LINE_BASIC("index:2"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL); /* 6 -> 2: forward 4 (7,0,1,2), backward 4 */
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DDDD");
    /* an odd count: 5 items, 0 -> 3: forward 3, backward 2 -> UP UP */
    reset_world(5u, 0u, true, true);
    begin(LINE_BASIC("index:3"), "v", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "UU");
    /* no wrap: 7 -> 1 is six UP (with wrap it would be two DOWN), 0 -> 7 seven DOWN */
    reset_world(8u, 7u, true, false);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "UUUUUU");
    reset_world(8u, 0u, true, false);
    begin(LINE_BASIC("index:7"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DDDDDDD");
    CHECK(cursor_now() == 7u);
    /* the machine's wrap knob is the menu table's, not the fake's: a wrap=0 table over a wrapping menu still goes the long way */
    reset_world(8u, 7u, true, true);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "UUUUUU");
    /* horizontal axis: RIGHT increases, LEFT decreases */
    reset_world(5u, 0u, false, false);
    begin(LINE_BASIC("index:2"), "h", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "RR...RR....");
    reset_world(5u, 3u, false, false);
    begin(LINE_BASIC("index:2"), "h", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "LL....");
    reset_world(6u, 0u, false, true);
    begin(LINE_BASIC("index:5"), "h", 1, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "LL....");
}

static void test_machine_ready_gating(void)
{
    /* the menu is not ready for 7 polls: the pad stays at rest, the first press is on poll 7 */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 7u;
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, ".......DD....");
    CHECK(g_machine.presses == 1u);
    /* T1770: the machine records the step poll at which the menu was first on screen and ready (a measurement for the log line) */
    CHECK(g_machine.ready_seen && g_machine.ready_polls == 8u);
    /* ready from the start: the first poll of the step */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(60u) == ROUTE_NAV_DONE);
    CHECK(g_machine.ready_seen && g_machine.ready_polls == 1u);
    /* not active either: the same */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 7u;
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(3u) == ROUTE_NAV_RUNNING);
    CHECK(!g_machine.ready_seen);
    /* not active either: the same */
    reset_world(5u, 0u, true, false);
    set32(ADDR_ACTIVE, 0u);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(10u) == ROUTE_NAV_RUNNING);
    CHECK_STR(w.trace, "..........");
    CHECK(g_machine.presses == 0u && cursor_now() == 0u && w.edges == 0u);
    set32(ADDR_ACTIVE, 1u);
    CHECK(route_nav_poll(&g_machine, 10u, &(xinput_pad_state){0}) == ROUTE_NAV_RUNNING);
    /* ready is checked again before each press: a cursor change that makes the menu busy delays the next press */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    xinput_pad_state pad;
    for (unsigned poll = 0u; poll < 5u; poll++) {
        w.now = poll * MS_PER_POLL;
        CHECK(route_nav_poll(&g_machine, poll, &pad) == ROUTE_NAV_RUNNING);
        apply_pad(&pad);
    }
    CHECK(cursor_now() == 1u && g_machine.state == ROUTE_NAV_PLAN);
    set32(ADDR_READY, 0u);
    w.now = 5u * MS_PER_POLL;
    CHECK(route_nav_poll(&g_machine, 5u, &pad) == ROUTE_NAV_RUNNING && pad.digital_buttons == 0u && g_machine.presses == 1u);
    set32(ADDR_READY, 1u);
    w.now = 6u * MS_PER_POLL;
    CHECK(route_nav_poll(&g_machine, 6u, &pad) == ROUTE_NAV_RUNNING && pad.digital_buttons == XINPUT_BUTTON_DPAD_DOWN && g_machine.presses == 2u);
    /* progress every 5 s of wall time while waiting, naming the unmet condition */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    begin("at=1 to=2 menu=list select=index:1 timeout=100000", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1300u) == ROUTE_NAV_RUNNING); /* 1300 * 16 ms = 20.8 s */
    CHECK(w.progress_count == 4u);
    CHECK_HAS(w.progress[0], "route nav: step 1 (menu=list select=index:1) still working after 5.0 s");
    CHECK_HAS(w.progress[0], "waiting for the menu, unmet: ready mem=0x1004:4==0x1 (read 0x0)");
    CHECK_HAS(w.progress[3], "after 20.0 s");
}

static void test_machine_timeout(void)
{
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    begin("at=10 to=40 menu=list select=index:1 timeout=500", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    const char *message = route_nav_message(&g_machine);
    CHECK_HAS(message, "route FAILED: nav step 1 (menu=list select=index:1) timed out after 512 ms (limit 500 ms)");
    CHECK_HAS(message, "waiting for the menu, unmet: ready mem=0x1004:4==0x1 (read 0x0)");
    CHECK_HAS(message, "seen cursor=0 count=5 active=yes ready=no");
    CHECK(g_polls_run == 33u && g_machine.presses == 0u);
    for (size_t i = 0u; i < w.trace_len; i++) CHECK(w.trace[i] == '.'); /* never pressed */
    CHECK(w.trace_len == 33u);
    /* the limit is exact: 15 ms short of it keeps waiting */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    begin("at=10 to=40 menu=list select=index:1 timeout=513", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(33u) == ROUTE_NAV_RUNNING); /* polls 0..32, 32 * 16 = 512 < 513 */
    {
        xinput_pad_state pad;
        w.now = 33u * MS_PER_POLL;
        CHECK(route_nav_poll(&g_machine, 33u, &pad) == ROUTE_NAV_FAILED); /* 528 >= 513 */
    }
    /* the limit is inclusive: spent == timeout fails on that very poll (poll 32 = 512 ms) */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    begin("at=10 to=40 menu=list select=index:1 timeout=512", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED && g_polls_run == 33u);
    /* an unreadable condition is named as such */
    reset_world(5u, 0u, true, false);
    set32(ADDR_OBJECT_POINTER, 0u);
    g_ready_spec = "*0x1008:4==1";
    begin("at=10 to=40 menu=list select=index:1 timeout=100", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    g_ready_spec = "0x1004:4==1";
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "unreadable");
    CHECK_HAS(route_nav_message(&g_machine), "seen cursor=unreadable count=unreadable");
    /* a timeout in the middle of the moves (the menu never follows the pad) fails too, never silently continues */
    reset_world(9u, 0u, true, false);
    w.swallow_mask = 0xFFFFFFFFu;
    begin("at=10 to=40 menu=list select=index:4 timeout=400 retry=100", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "timed out after");
    CHECK_HAS(route_nav_message(&g_machine), "moving the cursor 0 -> 4");
}

static void test_machine_lost_press(void)
{
    /* the first press is swallowed: one extra press after GAP + 2 polls of waiting */
    reset_world(5u, 0u, true, false);
    w.swallow_mask = 1u;
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "DD.....DD....");
    CHECK(g_machine.presses == 2u && g_machine.lost == 1u && w.edges == 2u && cursor_now() == 1u);
    /* retry=5: five lost presses in a row are survived */
    reset_world(5u, 0u, true, false);
    w.swallow_mask = 0x1Fu;
    begin("at=10 to=40 menu=list select=index:1 retry=5", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE);
    CHECK(g_machine.lost == 5u && g_machine.presses == 6u && w.edges == 6u && cursor_now() == 1u);
    /* the sixth in a row exhausts it */
    reset_world(5u, 0u, true, false);
    w.swallow_mask = 0x3Fu;
    begin("at=10 to=40 menu=list select=index:1 retry=5", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=list select=index:1) the press was lost 6 times in a row (retry=5)");
    CHECK(g_machine.presses == 6u && w.edges == 6u && cursor_now() == 0u);
    /* retry=0: the first lost press fails */
    reset_world(5u, 0u, true, false);
    w.swallow_mask = 1u;
    begin("at=10 to=40 menu=list select=index:1 retry=0", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "lost 1 times in a row (retry=0)");
    CHECK(g_machine.presses == 1u);
    /* the count is of presses lost in a row: lost, lost, moved, lost, lost, moved is fine with retry=2 */
    reset_world(9u, 0u, true, false);
    w.swallow_mask = 0x1Bu; /* edges 0,1 lost, 2 moves, 3,4 lost... bits: 0b11011 = presses 0,1,3,4 */
    begin("at=10 to=40 menu=list select=index:2 retry=2", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE);
    CHECK(g_machine.lost == 4u && cursor_now() == 2u);
    /* a lost press is only declared after GAP + 2 polls: a late move at the last poll of the window is a move, not a loss */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:1"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    xinput_pad_state pad;
    for (unsigned poll = 0u; poll < 6u; poll++) { /* press polls 0,1; release polls 2..6; the fake game never saw the press */
        w.now = poll * MS_PER_POLL;
        CHECK(route_nav_poll(&g_machine, poll, &pad) == ROUTE_NAV_RUNNING);
        if (poll == 5u) set32(ADDR_OBJECT, 1u); /* the cursor changes during the 4th release poll */
    }
    w.now = 6u * MS_PER_POLL;
    CHECK(route_nav_poll(&g_machine, 6u, &pad) == ROUTE_NAV_RUNNING);
    CHECK(g_machine.lost == 0u && g_machine.presses == 1u);
    w.now = 7u * MS_PER_POLL;
    CHECK(route_nav_poll(&g_machine, 7u, &pad) == ROUTE_NAV_DONE);
}

static void test_machine_odd_menus(void)
{
    /* two items per press: 0 -> 4 needs two presses, both unexpected moves, tolerated within retry */
    reset_world(10u, 0u, true, false);
    w.step = 2u;
    begin("at=10 to=40 menu=list select=index:4 retry=5", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    CHECK(g_machine.anomalies == 2u && g_machine.presses == 2u && cursor_now() == 4u);
    /* 0 -> 5 is never reachable in steps of two: it keeps re-planning, then fails as a menu that does not follow the pad */
    reset_world(10u, 0u, true, false);
    w.step = 2u;
    begin("at=10 to=40 menu=list select=index:5 retry=3", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the menu does not follow the pad");
    CHECK(g_machine.anomalies == 4u); /* retry + 1 */
    /* the menu moves the wrong way */
    reset_world(10u, 3u, true, false);
    w.invert = true;
    begin("at=10 to=40 menu=list select=index:5 retry=2", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "does not follow the pad");
    CHECK(g_machine.anomalies == 3u);
    /* a menu that wraps although the table says it does not: 0 -> 3 is still three DOWN presses, no anomaly */
    reset_world(4u, 0u, true, true);
    begin("at=10 to=40 menu=list select=index:3", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_DONE && cursor_now() == 3u && g_machine.presses == 3u && g_machine.anomalies == 0u);
    /* the pointer goes null once the menu moved: after a press there is no fallback any more */
    reset_world(5u, 0u, true, false);
    w.null_after_edge = true;
    begin(LINE_BASIC("index:3"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "cannot continue after 1 press(es), no fallback any more");
    CHECK(strstr(route_nav_message(&g_machine), "falling back") == NULL);
}

static void names_ascii(void)
{
    set_name(0u, "Alpha");
    set_name(1u, "Beta");
    set_name(2u, "Gamma Ray");
    set_name(3u, "Delta");
    set_name(4u, "EPS%=");
}

static void test_machine_targets(void)
{
    /* index beyond the count */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:5"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=list select=index:5) the index 5 is beyond the menu's 5 item(s) (valid 0..4)");
    CHECK_HAS(route_nav_message(&g_machine), "seen cursor=0 count=5");
    CHECK(g_machine.presses == 0u && w.edges == 0u);
    /* index == count - 1 is fine */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:4"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 4u);
    /* name lookup over an ascii inline table, case blind, a name with a space, % and = */
    const char *ascii = "names=*0x100C/16/ascii/15";
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:gamma%20RAY", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 2u);
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:eps%25%3D", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 4u);
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:ALPHA", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 0u && g_machine.presses == 0u);
    /* a prefix is not a match */
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:gamma", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_FAILED);
    /* not found: FAILS (also in mode on, no fallback), listing the names it saw */
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:Omega", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=list select=name:Omega) name 'Omega' is not among the menu's 5 item(s)");
    CHECK_HAS(route_nav_message(&g_machine), "names=[0:'Alpha', 1:'Beta', 2:'Gamma Ray', 3:'Delta', 4:'EPS%=']");
    CHECK(strstr(route_nav_message(&g_machine), "falling back") == NULL && g_machine.presses == 0u);
    /* ambiguous */
    reset_world(5u, 0u, true, false);
    names_ascii();
    set_name(3u, "BETA");
    begin("at=10 to=40 menu=list select=name:beta", "v", 0, ascii, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "name 'beta' is ambiguous, it matches items 1, 3");
    CHECK(g_machine.presses == 0u);
    /* utf16 inline: characters above 0x7E read as '?' */
    reset_world(3u, 0u, true, false);
    {
        const char *items[] = {"One", "Two", "Th_e"};
        for (unsigned i = 0u; i < 3u; i++)
            for (unsigned c = 0u; items[i][c] != '\0'; c++) w.ram[ADDR_NAMES + 32u * i + 2u * c] = (uint8_t)items[i][c];
        w.ram[ADDR_NAMES + 32u * 2u + 2u * 2u] = 0xE9u; /* U+00E9 */
        w.ram[ADDR_NAMES + 32u * 2u + 2u * 2u + 1u] = 0x00u;
    }
    begin("at=10 to=40 menu=list select=name:th?e", "v", 0, "names=*0x100C/32/utf16/15", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 2u);
    reset_world(3u, 0u, true, false);
    for (unsigned c = 0u; c < 3u; c++) w.ram[ADDR_NAMES + 32u * 1u + 2u * c] = (uint8_t)"Two"[c];
    w.ram[ADDR_NAMES + 32u * 1u + 2u * 1u + 1u] = 0x04u; /* 'w' replaced by U+04xx */
    begin("at=10 to=40 menu=list select=name:two", "v", 0, "names=*0x100C/32/utf16/15", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_FAILED); /* "T?o" is not "two" */
    /* ptr-ascii: a table of pointers, one null (an empty name) */
    reset_world(3u, 0u, true, false);
    set32(ADDR_NAMES + 0u, ADDR_STRINGS);
    set32(ADDR_NAMES + 4u, 0u);
    set32(ADDR_NAMES + 8u, ADDR_STRINGS + 16u);
    snprintf((char *)&w.ram[ADDR_STRINGS], 16u, "First");
    snprintf((char *)&w.ram[ADDR_STRINGS + 16u], 16u, "Third");
    begin("at=10 to=40 menu=list select=name:third", "v", 0, "names=*0x100C/4/ptr-ascii/15", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 2u);
    /* ptr-utf16 */
    reset_world(2u, 0u, true, false);
    set32(ADDR_NAMES + 0u, ADDR_STRINGS);
    set32(ADDR_NAMES + 4u, ADDR_STRINGS + 32u);
    for (unsigned c = 0u; c < 3u; c++) {
        w.ram[ADDR_STRINGS + 2u * c] = (uint8_t)"Abc"[c];
        w.ram[ADDR_STRINGS + 32u + 2u * c] = (uint8_t)"Xyz"[c];
    }
    begin("at=10 to=40 menu=list select=name:xyz", "v", 0, "names=*0x100C/4/ptr-utf16/15", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 1u);
    /* the name is cut at MAXLEN: "Gamma Ray" with a limit of 5 reads "Gamma" */
    reset_world(5u, 0u, true, false);
    names_ascii();
    begin("at=10 to=40 menu=list select=name:gamma", "v", 0, "names=*0x100C/16/ascii/5", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(400u) == ROUTE_NAV_DONE && cursor_now() == 2u);
}

static void test_machine_fallback_and_strict(void)
{
    /* the object pointer is null while the menu says it is active: the cursor cannot be read */
    reset_world(5u, 0u, true, false);
    set32(ADDR_OBJECT_POINTER, 0u);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_STR(route_nav_message(&g_machine),
              "route nav: step 1 falling back to the recorded presses (the menu is active but its cursor is unreadable (null pointer or bad address))");
    CHECK(g_machine.presses == 0u && w.edges == 0u && g_polls_run == 1u);
    CHECK_STR(w.trace, "."); /* the pad of that poll is at rest */
    /* strict: the same is a failure */
    reset_world(5u, 0u, true, false);
    set32(ADDR_OBJECT_POINTER, 0u);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_STRICT, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=list select=index:2) cannot run (strict): the menu is active but its cursor is unreadable");
    /* the count unreadable (cursor readable): its own reason */
    reset_world(5u, 0u, true, false);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    ((route_nav_menu *)g_machine.menu)->count.cond.address = 0x90000u; /* beyond the fake ram */
    ((route_nav_menu *)g_machine.menu)->count.cond.indirect = false;
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "its count is unreadable");
    /* an implausible count / cursor outside it */
    reset_world(0u, 0u, true, false);
    begin(LINE_BASIC("index:0"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "the item count 0 is not plausible");
    reset_world(5u, 7u, true, false);
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "the cursor 7 is not below the item count 5");
    reset_world(5u, 5u, true, false); /* cursor == count is outside as well */
    begin(LINE_BASIC("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "the cursor 5 is not below the item count 5");
    reset_world(5u, 4u, true, false); /* the last item is a valid cursor */
    begin(LINE_BASIC("index:4"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_DONE);
    /* unknown menu */
    reset_world(5u, 0u, true, false);
    begin("at=10 to=40 menu=missing select=index:2", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(g_machine.menu == NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_STR(route_nav_message(&g_machine),
              "route nav: step 1 falling back to the recorded presses (unknown menu 'missing', the menu table has no such entry)");
    reset_world(5u, 0u, true, false);
    begin("at=10 to=40 menu=missing select=index:2", "v", 0, NULL, ROUTE_NAV_MODE_STRICT, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=missing select=index:2) cannot run (strict): unknown menu 'missing'");
    /* a name select over a menu without names= cannot run */
    reset_world(5u, 0u, true, false);
    begin("at=10 to=40 menu=list select=name:beta", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "menu list has no names= or items= entry");
    /* unreadable names table */
    reset_world(5u, 0u, true, false);
    set32(ADDR_NAMES_POINTER, 0u);
    begin("at=10 to=40 menu=list select=name:beta", "v", 0, "names=*0x100C/16/ascii/15", ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(40u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "names table address is unreadable");
    /* a timeout is never a fallback in mode on */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    begin("at=10 to=40 menu=list select=index:2 timeout=200", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK(strstr(route_nav_message(&g_machine), "falling back") == NULL);
    /* after the activation began there is no fallback either */
    reset_world(5u, 2u, true, false);
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    xinput_pad_state pad;
    w.now = 0u;
    CHECK(route_nav_poll(&g_machine, 0u, &pad) == ROUTE_NAV_RUNNING && pad.analog[0] == 255u);
    set32(ADDR_OBJECT_POINTER, 0u);
    set32(ADDR_ACTIVE, 1u);
    for (unsigned poll = 1u; poll < 10u && g_machine.status == ROUTE_NAV_RUNNING; poll++) {
        w.now = poll * MS_PER_POLL;
        route_nav_poll(&g_machine, poll, &pad);
    }
    CHECK(g_machine.status == ROUTE_NAV_DONE); /* an unreadable cursor after the activation means the menu went away */
}

static void test_machine_postcondition(void)
{
    /* default post-condition, the menu closes (active no longer holds) */
    reset_world(5u, 2u, true, false);
    w.a_closes = true;
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    CHECK(w.arm_calls == 0u && w.hold_calls == 0u); /* no expect: the evaluator is not consulted */
    /* the cursor moves on its own after the activation */
    reset_world(5u, 2u, true, false);
    w.a_moves = true;
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    /* nothing changes: fails after the 3 s grace (measured from the first A poll) */
    reset_world(5u, 2u, true, false);
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=list select=index:2) the activation had no visible effect");
    /* A on poll 0, the first check >= 3000 ms is poll 188 (3008 ms): the menu did not react, A is pressed once more (poll 188), and again 3008 ms later */
    CHECK_HAS(route_nav_message(&g_machine), "3008 ms after the last press (2 press(es) of A;");
    CHECK(w.activations == 2u && w.a_edges == 2u); /* once more, not a third time */
    CHECK(g_polls_run == 377u);
    CHECK(w.progress_count == 2u); /* the re-press line (3 s) and the 5 s cadence line */
    CHECK_HAS(w.progress[0], "the menu did not react to A within 3000 ms, pressing it once more");
    CHECK_HAS(w.progress[1], "still working after 5.0 s: waiting for the post-condition");
    /* the grace is inclusive: with 600 ms polls the verdict poll (poll 5) is exactly 3000 ms after the A press and fails there */
    reset_world(5u, 2u, true, false);
    g_ms_per_poll = 600u;
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED && g_polls_run == 11u); /* the second press is on poll 5 (3000 ms), its verdict on poll 10 (3000 ms later) */
    CHECK_HAS(route_nav_message(&g_machine), "3000 ms after the last press");
    /* just inside the grace the verdict is still open: 2992 ms (poll 187) */
    reset_world(5u, 2u, true, false);
    begin(LINE_ACTIVATE("index:2"), "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(188u) == ROUTE_NAV_RUNNING);
    /* with expect: consulted after the activation, armed once with the step number, polls counted from the A press */
    reset_world(5u, 2u, true, false);
    w.expect_after_polls = 9u;
    begin("at=10 to=40 menu=list select=index:2 activate expect=mem=0x1010==1", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    CHECK(w.arm_calls == 1u && w.arm_step == 1u);
    CHECK(w.last_expect_polls == 9u && w.hold_calls == 4u); /* VERIFY starts at the 6th poll since the press began (5 pressing/releasing + ...) */
    /* the expect result decides, not the menu: a closing menu does not satisfy a failing expect */
    reset_world(5u, 2u, true, false);
    w.a_closes = true;
    begin("at=10 to=40 menu=list select=index:2 activate expect=mem=0x1010==1 timeout=2000", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "timed out after");
    CHECK_HAS(route_nav_message(&g_machine), "waiting for the post-condition: WAIT mem=0x1010:4==0x1 (read 0x0)");
    /* the expect's own timeout= ends it earlier with its own text */
    reset_world(5u, 2u, true, false);
    begin("at=10 to=40 menu=list select=index:2 activate expect=mem=0x1010==1,timeout=300", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the expect=mem=0x1010==1,timeout=300 post-condition did not hold");
    CHECK_HAS(route_nav_message(&g_machine), "WAIT mem=0x1010:4==0x1 (read 0x0)");
    CHECK(g_machine.polls >= 19u && g_machine.polls <= 30u);
    /* max= polls of the expect */
    reset_world(5u, 2u, true, false);
    begin("at=10 to=40 menu=list select=index:2 activate expect=mem=0x1010==1,max=12", "v", 0, NULL, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "post-condition did not hold");
    CHECK(g_machine.polls == 12u && w.last_expect_polls == 12u);
}

/* ======================================================================================================================
 * The page widget world (E1 to E3 of tools/data/menu_nav.txt): pages found by builder among stale slots, rows from a node chain
 * ==================================================================================================================== */
static const char PAGE_TABLE[] =
    "widgets table=0x8000 stride=0x23C count=6 closing=4,5\n"
    "menu pg page=builder:0x2D2350 active=0x1000:4==1 ready=@+0x1B0:4==3,@+0x170:4==0 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=1 itemid=@+0x18C:4 "
    "items=walk(@+0x178:4;next=+0x8;type=+0x0:4|7|8;id=+0x14;text=+0x20;flags=+0x28;skip=0x4;grey=0x1) select=A back=B conf=MEASURED\n"
    "menu pg-nowrap page=builder:0x2D2350 active=0x1000:4==1 ready=@+0x1B0:4==3 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=0 "
    "items=walk(@+0x178:4;next=+0x8;type=+0x0:4|7|8;id=+0x14;text=+0x20;flags=+0x28;skip=0x4;grey=0x1)\n"
    "menu pg-plain page=builder:0x2D2350 active=0x1000:4==1 ready=@+0x1B0:4==3 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=1\n"
    "menu title page=none active=0x1000:4==1 select=A\n";

static const fake_row LIST_ROWS[] = {
    {"Choose one", 0xFFFFFFFDu, 0x4u, 4u}, /* 0: a title, skipped by the cursor */
    {"Alpha", 1u, 0u, 4u},                 /* 1 */
    {"Beta", 2u, 0u, 7u},                  /* 2 */
    {"Gamma Ray", 3u, 0x1u, 8u},           /* 3: greyed */
    {"Delta", 4u, 0u, 4u},                 /* 4 */
    {"hint", 0xFFFFFFFFu, 0x4u, 8u},       /* 5: a hint line, skipped */
};
#define LIST_ROW_COUNT 6u
static const fake_row DECOY_ROWS[] = {{"Old one", 50u, 0u, 4u}, {"Old two", 51u, 0u, 4u}, {"Old three", 52u, 0u, 4u}};
static const fake_row GAP_ROWS[] = {{"A", 10u, 0u, 4u}, {"s1", 0xFFFFFFFFu, 0x4u, 8u}, {"s2", 0xFFFFFFFFu, 0x4u, 8u}, {"B", 11u, 0u, 4u}};

static void begin_with(const char *nav_text, const char *table_text, route_nav_mode mode, const route_nav_timing *timing)
{
    char error[300];
    CHECK(route_nav_menus_parse(table_text, strlen(table_text), &g_table, error, sizeof error));
    CHECK(route_nav_line_parse(nav_text, &g_step, error, sizeof error));
    const route_nav_probe probe = {fake_read_mem, fake_read_bytes, fake_now, fake_arm, fake_expect, fake_progress, NULL};
    w.now = 0u;
    route_nav_begin(&g_machine, &g_step, 1u, route_nav_menu_find(&g_table, g_step.menu), mode, timing, &probe);
    g_polls_run = 0u;
}

/* Six slots with decoys: 0 a stale (closing) page of the builder, 1 another builder, 2 the builder still opening (state 2), 3 and 4 the builder fully
 * open (state 3: the highest slot wins), 5 the builder closing. The fake game drives slot 4. */
static uint32_t setup_page_world(uint32_t cursor)
{
    reset_world(LIST_ROW_COUNT, 0u, true, true);
    build_page(0u, BUILDER_LIST, 5u, DECOY_ROWS, 3u, 2u, ADDR_NODES + 0x000u, ADDR_TEXTS + 0x000u);
    build_page(1u, BUILDER_OTHER, 3u, DECOY_ROWS, 3u, 1u, ADDR_NODES + 0x400u, ADDR_TEXTS + 0x100u);
    build_page(2u, BUILDER_LIST, 2u, DECOY_ROWS, 3u, 0u, ADDR_NODES + 0x800u, ADDR_TEXTS + 0x200u);
    build_page(3u, BUILDER_LIST, 3u, LIST_ROWS, LIST_ROW_COUNT, 2u, ADDR_NODES + 0xC00u, ADDR_TEXTS + 0x300u);
    const uint32_t page = build_page(4u, BUILDER_LIST, 3u, LIST_ROWS, LIST_ROW_COUNT, cursor, ADDR_NODES + 0x1000u, ADDR_TEXTS + 0x400u);
    build_page(5u, BUILDER_LIST, 4u, DECOY_ROWS, 3u, 1u, ADDR_NODES + 0x1400u, ADDR_TEXTS + 0x500u);
    drive_page(page, LIST_ROWS, LIST_ROW_COUNT);
    return page;
}
/* One live page only (slot 2), a stale one in slot 0. */
static uint32_t setup_single_page(const fake_row *rows, unsigned count, uint32_t cursor)
{
    reset_world(count, 0u, true, true);
    build_page(0u, BUILDER_LIST, 5u, DECOY_ROWS, 3u, 2u, ADDR_NODES, ADDR_TEXTS);
    const uint32_t page = build_page(2u, BUILDER_LIST, 3u, rows, count, cursor, ADDR_NODES + 0x800u, ADDR_TEXTS + 0x200u);
    drive_page(page, rows, count);
    return page;
}
#define PG(select) "at=10 to=40 menu=pg select=" select
#define PG_ACT(select) "at=10 to=40 menu=pg select=" select " activate"

static void test_page_world_selection(void)
{
    char summary[40];
    /* the page is found among stale and decoy slots: slot 4 (state 3, highest slot of the two state 3 pages). Cursor 1 (Alpha) -> Delta: the
     * stops are rows 1,2,3,4 (rows 0 and 5 are skipped) and the wrap makes it ONE press UP (1 -> 4 through the wrap). */
    uint32_t page = setup_page_world(1u);
    begin_with(PG("name:DELTA"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "U");
    CHECK(cursor_now() == 4u && get32(page + 0x184u) == 4u && g_machine.presses == 1u && g_machine.anomalies == 0u);
    CHECK(get32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x184u) == 2u); /* the decoy of the same builder was not touched */
    CHECK(get32(ADDR_WIDGETS + 0u * WIDGET_STRIDE + 0x184u) == 2u && get32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x184u) == 0u);
    CHECK(g_machine.stop_count == 4u && g_machine.row_count == LIST_ROW_COUNT && g_machine.stops[0] == 1u && g_machine.stops[3] == 4u);
    /* the highest STATE wins over the highest slot: slot 3 at state 3, slot 4 only opening (state 2) -> slot 3 is the page */
    page = setup_page_world(1u);
    set32(ADDR_WIDGETS + 4u * WIDGET_STRIDE + 0x1B0u, 2u);
    drive_page(ADDR_WIDGETS + 3u * WIDGET_STRIDE, LIST_ROWS, LIST_ROW_COUNT);
    set32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x184u, 1u);
    begin_with(PG("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    CHECK(get32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x184u) == 2u && get32(page + 0x184u) == 1u);
    /* one press moves several rows: A, (skipped, skipped), B. DOWN from A lands on B (cursor 0 -> 3), which is the expected move, not an anomaly */
    page = setup_single_page(GAP_ROWS, 4u, 0u);
    begin_with(PG("name:b"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    CHECK(cursor_now() == 3u && g_machine.presses == 1u && g_machine.anomalies == 0u && g_machine.stop_count == 2u);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "D");
    /* and back: wrap 1 with two stops, B -> A is a tie of one step either way, a tie goes increasing (DOWN wraps from B to A) */
    page = setup_single_page(GAP_ROWS, 4u, 3u);
    begin_with(PG("id:10"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "D");
    CHECK(cursor_now() == 0u && g_machine.anomalies == 0u);
    /* without wrap the same move is UP */
    page = setup_single_page(GAP_ROWS, 4u, 3u);
    w.wrap = false;
    begin_with("at=10 to=40 menu=pg-nowrap select=index:0", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "U");
    CHECK(cursor_now() == 0u);

    /* select by id, by name; the decoy node (type 3, id 0x99) is not a row */
    page = setup_page_world(1u);
    begin_with(PG("id:3"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE && cursor_now() == 3u);
    page = setup_page_world(1u);
    begin_with(PG("id:0x99"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=pg select=id:0x99) no row has the id 0x99 among the menu's 6 row(s)");
    CHECK_HAS(route_nav_message(&g_machine), "names=[0:'Choose one'#0xFFFFFFFD(skipped), 1:'Alpha'#0x1, 2:'Beta'#0x2, 3:'Gamma Ray'#0x3(greyed), 4:'Delta'#0x4, 5:'hint'#0xFFFFFFFF(skipped)]");
    CHECK(g_machine.presses == 0u);
    page = setup_single_page(GAP_ROWS, 4u, 0u);
    begin_with(PG("id:0xFFFFFFFF"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the id 0xFFFFFFFF is ambiguous, it matches rows 1, 2");
    /* an id select needs a row list with ids */
    page = setup_page_world(1u);
    begin_with("at=10 to=40 menu=pg-plain select=id:3", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "menu pg-plain has no items= with id=");

    /* a skipped row cannot be selected (a title, a hint): FAILS naming the row, never a fallback */
    page = setup_page_world(1u);
    begin_with(PG("index:0"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "row 0 'Choose one' is skipped by the cursor (flags 0x4), it cannot be selected");
    page = setup_page_world(1u);
    begin_with(PG("name:HINT"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "name 'HINT' is not among the menu's 6 item(s)"); /* a skipped row is not an item to match */
    CHECK(g_machine.presses == 0u);
    /* the cursor on none yet (0xFFFFFFFF): the first DOWN gives the first stop (row 1), then one more DOWN reaches Beta; no anomaly */
    page = setup_page_world(ROUTE_NAV_CURSOR_NONE);
    begin_with(PG("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DD");
    CHECK(cursor_now() == 2u && g_machine.anomalies == 0u && g_machine.presses == 2u);
    /* a count that disagrees with the row list cannot be trusted: fallback before any press */
    page = setup_page_world(1u);
    set32(page + 0x180u, 5u);
    begin_with(PG("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "the count is 5 but the row list has 6 rows");
    /* an unreadable row node, and a cyclic chain */
    page = setup_page_world(1u);
    set32(page + 0x178u, 0x90000000u);
    begin_with(PG("index:2"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "the row node at 0x90000000 is unreadable");
    page = setup_page_world(1u);
    set32(get32(page + 0x178u) + 0x30u + 0x8u, get32(page + 0x178u) + 0x30u); /* the decoy node (not a row) links to itself */
    begin_with(PG("index:2"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FALLBACK);
    CHECK_HAS(route_nav_message(&g_machine), "longer than 512 nodes");
    /* strict: the same is a failure */
    begin_with(PG("index:2"), PAGE_TABLE, ROUTE_NAV_MODE_STRICT, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "cannot run (strict): the row list is longer than 512 nodes");
}

static void hook_open_page(unsigned poll)
{
    const uint32_t page = ADDR_WIDGETS + 4u * WIDGET_STRIDE;
    if (poll == 0u) set32(page + 0x8u, 0u);                /* no page of the builder on screen */
    if (poll == 4u) set32(page + 0x8u, BUILDER_LIST);      /* it appears... */
    if (poll == 4u) set32(page + 0x1B0u, 1u);              /* ...opening */
    if (poll == 6u) set32(page + 0x1B0u, 2u);
    if (poll == 9u) set32(page + 0x1B0u, 3u);              /* fully open: ready */
    if (poll == 9u) set32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x8u, 0u); /* the other copy goes away */
}
static void test_page_world_readiness(void)
{
    /* the page is not there, then opening (states 1 and 2), the first press is on the poll it reaches state 3 (poll 9) */
    uint32_t page = setup_page_world(1u);
    set32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x1B0u, 5u); /* slot 3 closing from the start */
    (void)page;
    begin_with(PG("name:delta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    g_pre_poll = hook_open_page;
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    g_pre_poll = NULL;
    CHECK_STR(w.trace, ".........UU....");
    CHECK(g_machine.presses == 1u && cursor_now() == 4u);
    /* the page in the LAST slot of the table is found as well */
    reset_world(LIST_ROW_COUNT, 0u, true, true);
    page = build_page(5u, BUILDER_LIST, 3u, LIST_ROWS, LIST_ROW_COUNT, 1u, ADDR_NODES + 0x1400u, ADDR_TEXTS + 0x500u);
    drive_page(page, LIST_ROWS, LIST_ROW_COUNT);
    begin_with(PG("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE && cursor_now() == 2u);
    /* and a page of the builder whose slot is not in use (dword +0 is 0) is no page */
    reset_world(LIST_ROW_COUNT, 0u, true, true);
    page = build_page(2u, BUILDER_LIST, 3u, LIST_ROWS, LIST_ROW_COUNT, 1u, ADDR_NODES + 0x800u, ADDR_TEXTS + 0x200u);
    set32(page + 0x0u, 0u);
    drive_page(page, LIST_ROWS, LIST_ROW_COUNT);
    begin_with("at=10 to=40 menu=pg select=name:beta timeout=200", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "page builder:0x2D2350 is not on screen");
    /* the unmet text names the page, then the page spec */
    page = setup_page_world(1u);
    set32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x8u, 0u);
    set32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x8u, 0u);
    set32(page + 0x8u, 0u);
    begin_with("at=10 to=40 menu=pg select=index:2 timeout=300", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "waiting for the menu, unmet: page builder:0x2D2350 is not on screen (no live widget slot)");
    CHECK_HAS(route_nav_message(&g_machine), "page=absent");
    CHECK(g_machine.presses == 0u);
    /* a child page on top (+0x170 != 0) means input is not accepted: waits, naming the page condition */
    page = setup_page_world(1u);
    set32(page + 0x170u, 1u);
    set32(ADDR_WIDGETS + 3u * WIDGET_STRIDE + 0x8u, 0u);
    begin_with("at=10 to=40 menu=pg select=index:2 timeout=300", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "ready page+mem=0x170:4==0x0 (read 0x1)");
    for (size_t i = 0u; i < w.trace_len; i++) CHECK(w.trace[i] == '.');
    /* the global condition of active= is checked as well */
    page = setup_page_world(1u);
    set32(ADDR_ACTIVE, 0u);
    begin_with("at=10 to=40 menu=pg select=index:2 timeout=300", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "active mem=0x1000:4==0x1 (read 0x0)");
}

static void hook_cursor_arrives(unsigned poll)
{
    if (poll == 10u) {
        set32(w.cursor_addr, 2u); /* the recorded stick movement lands the cursor on Beta */
        set32(w.itemid_addr, 2u);
    }
}
static void hook_clear_grey(unsigned poll)
{
    if (poll == 10u) set32(get32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x178u) + 0x30u * 4u + 0x28u, 0u); /* row 3 (node 4: the decoy is node 1) loses its grey flag */
}
static void test_page_world_activation(void)
{
    /* a greyed target (Gamma Ray, flags 0x1): the cursor goes there, the select button is NOT pressed, after 3 s the step FAILS naming the row */
    uint32_t page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    w.a_closes_page = true;
    begin_with(PG_ACT("name:gamma%20ray"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the target row 3 'Gamma Ray' is greyed (flags 0x1) and ignores the select press");
    CHECK(w.activations == 0u && w.a_edges == 0u && cursor_now() == 3u && g_machine.presses == 2u);
    CHECK(g_polls_run > 180u && g_polls_run < 200u); /* 3000 ms of 16 ms polls after the cursor arrived */
    for (size_t i = 0u; i < w.trace_len; i++) CHECK(w.trace[i] != 'A');
    /* the grace is inclusive: the cursor is on the greyed row from the start, 1000 ms polls, first seen on poll 0 (0 ms), it fails on poll 3 (3000 ms) */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 3u);
    g_ms_per_poll = 1000u;
    begin_with(PG_ACT("index:3"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(50u) == ROUTE_NAV_FAILED && g_polls_run == 4u && w.a_edges == 0u);
    /* a row that is greyed only for a moment is activated once it is enabled */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 3u);
    w.a_closes_page = true;
    begin_with(PG_ACT("index:3"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    g_pre_poll = hook_clear_grey;
    CHECK(drive(1000u) == ROUTE_NAV_DONE);
    g_pre_poll = NULL;
    CHECK(w.activations == 1u && g_polls_run < 40u);
    CHECK(w.trace[9] != 'A' && w.trace[10] == 'A'); /* greyed until the hook clears the flag on poll 10 */

    /* the item id under the cursor must be the target row's id */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.itemid_addr = 0u; /* the fake stops updating the id: it stays that of the starting row */
    set32(page + 0x18Cu, 0x77u);
    begin_with(PG_ACT("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the cursor is on the item id 0x77 but the target row 2 has the id 0x2");
    CHECK(w.activations == 0u);

    /* default post-condition: the page leaves the screen (closing) */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.a_closes_page = true;
    begin_with(PG_ACT("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "AA....");
    CHECK(w.activations == 1u && g_machine.represses == 0u);
    /* ... or another page opens on top (the page is no longer ready) */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.a_opens_child = true;
    begin_with(PG_ACT("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(200u) == ROUTE_NAV_DONE);
    /* ... but a page that did not react is pressed ONCE more after the 3 s grace, then waits for another 3 s */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.a_closes_page = true;
    w.swallow_a_mask = 1u; /* the first select press is lost */
    begin_with(PG_ACT("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_DONE);
    CHECK(w.a_edges == 2u && w.activations == 1u && g_machine.represses == 1u);
    CHECK(w.progress_count == 1u); /* only the re-press line: the 5 s cadence is not reached */
    CHECK_HAS(w.progress[0], "the menu did not react to A within 3000 ms, pressing it once more");
    CHECK(g_polls_run > 190u && g_polls_run < 200u); /* the second press is on poll 188 (3008 ms), the page closes at once */
    int presses_a = 0;
    for (size_t i = 0u; i < w.trace_len; i++) presses_a += w.trace[i] == 'A' ? 1 : 0;
    CHECK(presses_a == 4); /* two presses of two polls (HOLD) */
    /* both lost: FAILS after the second grace, 2 presses made */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.a_closes_page = true;
    w.swallow_a_mask = 3u;
    begin_with(PG_ACT("name:beta"), PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "the activation had no visible effect");
    CHECK_HAS(route_nav_message(&g_machine), "(2 press(es) of A;");
    CHECK(w.a_edges == 2u && w.activations == 0u && g_polls_run > 370u && g_polls_run < 390u);
    /* with expect= the select button is never pressed twice */
    page = setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.swallow_a_mask = 3u;
    begin_with("at=10 to=40 menu=pg select=name:beta activate expect=mem=0x1010==1 timeout=8000", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(2000u) == ROUTE_NAV_FAILED);
    CHECK(w.a_edges == 1u);

    /* a menu without a cursor (the title's press start) is waited for and activated */
    reset_world(1u, 0u, true, false);
    w.a_closes = true;
    begin_with("at=10 to=40 menu=title select=index:0 activate", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "AA....");
    CHECK(w.activations == 1u && g_machine.presses == 0u);
    reset_world(1u, 0u, true, false); /* not active yet: waits */
    set32(ADDR_ACTIVE, 0u);
    begin_with("at=10 to=40 menu=title select=index:0 activate timeout=100", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "seen active=no ready=yes");
    reset_world(1u, 0u, true, false);
    begin_with("at=10 to=40 menu=title select=index:0", PAGE_TABLE, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE && w.trace_len == 1u && w.activations == 0u);
}

/* The real table, tools/data/menu_nav.txt: it parses, and its own offsets drive a fake game page by page. */
static bool read_real_table(char *text, size_t size)
{
    char path[1200];
    const char *here = __FILE__;
    const char *slash = strrchr(here, '/');
    if (slash == NULL) return false;
    snprintf(path, sizeof path, "%.*s/../../tools/data/menu_nav.txt", (int)(slash - here), here);
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    const size_t got = fread(text, 1u, size - 1u, file);
    fclose(file);
    text[got] = '\0';
    return got != 0u && got < size - 1u;
}
static void test_real_menu_table(void)
{
    static char text[80000];
    static route_nav_menu_table real;
    char error[300];
    CHECK(read_real_table(text, sizeof text));
    CHECK(route_nav_menus_parse(text, strlen(text), &real, error, sizeof error));
    if (error[0] != '\0' && real.count == 0u) printf("real table: %s\n", error);
    CHECK(real.widgets.present && real.widgets.table == 0x6FEBE8u && real.widgets.stride == 0x23Cu && real.widgets.count == 20u);
    CHECK(real.widgets.closing_count == 2u && real.widgets.closing[0] == 4u && real.widgets.closing[1] == 5u);
    CHECK(real.count >= 10u);
    unsigned with_items = 0u, cursorless = 0u;
    for (size_t i = 0u; i < real.count; i++) {
        const route_nav_menu *menu = &real.menus[i];
        with_items += menu->items.present ? 1u : 0u;
        cursorless += menu->has_cursor ? 0u : 1u;
        CHECK(!menu->page_builder || (menu->widgets.present && menu->widgets.table == 0x6FEBE8u));
        if (menu->items.present) {
            CHECK(menu->has_cursor && menu->items.type_count == 3u && menu->items.types[0] == 4u && menu->items.types[1] == 7u && menu->items.types[2] == 8u);
            CHECK(menu->items.next_off == 0x8u && menu->items.id_off == 0x14u && menu->items.text_off == 0x20u && menu->items.flags_off == 0x28u);
            CHECK(menu->items.skip_mask == 0x4u && menu->items.grey_mask == 0x1u && menu->items.head.page && menu->items.head.cond.address == 0x178u);
            CHECK(menu->cursor.page && menu->cursor.cond.address == 0x184u && menu->count.page && menu->count.cond.address == 0x180u);
            CHECK(menu->has_itemid && menu->itemid.page && menu->itemid.cond.address == 0x18Cu && menu->vertical && menu->wrap);
        }
    }
    CHECK(with_items >= 8u && cursorless >= 3u);
    const route_nav_menu *main_menu = route_nav_menu_find(&real, "main-menu");
    const route_nav_menu *map_list = route_nav_menu_find(&real, "map-list");
    CHECK(main_menu != NULL && map_list != NULL);
    if (main_menu == NULL) return;
    {
        const char *editors[] = {"editor-menu", "editor-map-settings", "editor-tools", "editor-weapon-sets"};
        for (size_t i = 0u; i < 4u; i++) {
            const route_nav_menu *editor = route_nav_menu_find(&real, editors[i]);
            CHECK(editor != NULL && editor->dpad_off && editor->selectby == ROUTE_NAV_BY_ID);
        }
        const route_nav_menu *profiles = route_nav_menu_find(&real, "profile-select"), *maps = route_nav_menu_find(&real, "map-list");
        const route_nav_menu *storage = route_nav_menu_find(&real, "storage-select");
        CHECK(profiles != NULL && profiles->selectby == ROUTE_NAV_BY_NAME && !profiles->dpad_off);
        CHECK(maps != NULL && maps->selectby == ROUTE_NAV_BY_NAME && !maps->dpad_off);
        CHECK(storage != NULL && storage->selectby == ROUTE_NAV_BY_ID && !route_nav_menu_find(&real, "main-menu")->dpad_off);
    }
    CHECK(main_menu->page_builder && main_menu->builder == 0x2D0BE0u && main_menu->active_count >= 1u && main_menu->ready_count == 2u);

    /* the fake game as the table describes the main menu: 8 rows (ids 0..7), a page in slot 7 of the real widget table, mode word 0x65 */
    reset_world(8u, 0u, true, true);
    static const fake_row rows[8] = {{"Story", 0u, 0u, 4u}, {"Arcade", 1u, 0u, 4u}, {"Challenge", 2u, 0u, 4u}, {"Xbox Live", 3u, 0u, 4u},
                                     {"Mapmaker", 4u, 0u, 4u}, {"Player Progress", 5u, 0u, 4u}, {"Settings", 6u, 0u, 4u}, {"Extras", 7u, 0u, 4u}};
    set32(0x79094Cu, 0x65u);
    build_page_at(0x6FEBE8u, 0x23Cu, 3u, 0x2D0BE0u, 5u, rows, 3u, 0u, 0x42000000u, 0x42001000u); /* a stale copy of the page, closing */
    const uint32_t page = build_page_at(0x6FEBE8u, 0x23Cu, 7u, 0x2D0BE0u, 3u, rows, 8u, 0u, 0x42002000u, 0x42003000u);
    drive_page(page, rows, 8u);
    begin_with("at=10 to=40 menu=main-menu select=name:mapmaker activate", text, ROUTE_NAV_MODE_ON, NULL);
    w.a_closes_page = true;
    CHECK(drive(300u) == ROUTE_NAV_DONE);
    char summary[40];
    press_summary(summary, sizeof summary);
    CHECK_STR(summary, "DDDD"); /* 0 -> 4: a tie both ways round, which goes increasing */
    CHECK(cursor_now() == 4u && w.activations == 1u && g_machine.anomalies == 0u && g_machine.presses == 4u);
    /* the same menu through the id of the row */
    reset_world(8u, 0u, true, true);
    set32(0x79094Cu, 0x65u);
    const uint32_t page2 = build_page_at(0x6FEBE8u, 0x23Cu, 12u, 0x2D0BE0u, 3u, rows, 8u, 6u, 0x42002000u, 0x42003000u);
    drive_page(page2, rows, 8u);
    begin_with("at=10 to=40 menu=main-menu select=id:7", text, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(300u) == ROUTE_NAV_DONE && cursor_now() == 7u && g_machine.presses == 1u); /* 6 -> 7 */
    /* the mode word of another game state: the menu is not on screen, the step waits */
    set32(0x79094Cu, 0x66u);
    reset_world(8u, 0u, true, true);
    set32(0x79094Cu, 0x66u);
    drive_page(build_page_at(0x6FEBE8u, 0x23Cu, 7u, 0x2D0BE0u, 3u, rows, 8u, 0u, 0x42002000u, 0x42003000u), rows, 8u);
    begin_with("at=10 to=40 menu=main-menu select=id:7 timeout=200", text, ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "active mem=0x79094C:4==0x65 (read 0x66)");
}

/* ======================================================================================================================
 * The recorder side: the menu table sampled while a route is recorded
 * ==================================================================================================================== */
static const char REC_TABLE[] =
    "widgets table=0x8000 stride=0x23C count=6 closing=4,5\n"
    "menu pg page=builder:0x2D2350 active=0x1000:4==1 ready=@+0x1B0:4==3,@+0x170:4==0 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=1 itemid=@+0x18C:4 "
    "items=walk(@+0x178:4;next=+0x8;type=+0x0:4|7|8;id=+0x14;text=+0x20;flags=+0x28;skip=0x4;grey=0x1) select=A back=B\n"
    "menu box page=builder:0x2C8940 active=0x1000:4==1\n"
    "menu title page=none active=0x1000:4==1,0x1010:4==0 select=A\n";
static route_nav_menu_table g_rec_table;
static char g_log[96][300];
static unsigned g_log_count;
static char g_rec_path[64];
static nav_recorder *g_recorder;
static const xinput_record_effect A_EFFECT = {0u, 0, -1};

static void cap_log(const char *line, void *user)
{
    (void)user;
    if (g_log_count < 96u) snprintf(g_log[g_log_count], sizeof g_log[0], "%s", line);
    g_log_count++;
}
static const char *cap_add(uint64_t at, uint64_t to, uint64_t edge, const char *body, int analog, uint16_t digital, void *user)
{
    (void)user;
    return xinput_record_add_nav(at, to, edge, body, analog, digital);
}
static void cap_observe(uint64_t poll, const xinput_pad_state *recorded, void *user) { nav_recorder_observe(user, poll, recorded); }
static void cap_close(uint64_t total, void *user) { nav_recorder_close(user, total); }
static bool log_has(const char *part)
{
    for (unsigned i = 0u; i < g_log_count && i < 96u; i++)
        if (strstr(g_log[i], part) != NULL) return true;
    return false;
}
/* Open a record, with the recorder side hooked in when `with_feature`; the fake memory is what reset_world / setup_* made. */
static void rec_begin(const char *table_text, bool with_feature)
{
    char error[2400];
    snprintf(g_rec_path, sizeof g_rec_path, "/tmp/tsfp-nav-rec-XXXXXX");
    const int descriptor = mkstemp(g_rec_path);
    if (descriptor >= 0) close(descriptor);
    g_log_count = 0u;
    CHECK(xinput_record_open(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error));
    CHECK(xinput_record_enable_marks(g_rec_path));
    g_recorder = NULL;
    if (!with_feature) return;
    CHECK(route_nav_menus_parse(table_text, strlen(table_text), &g_rec_table, error, sizeof error));
    const nav_record_hooks hooks = {{fake_read_mem, fake_read_bytes, fake_now, NULL, NULL, NULL, NULL}, cap_log, NULL, cap_add, NULL};
    g_recorder = nav_recorder_create(&g_rec_table, &hooks);
    CHECK(g_recorder != NULL);
    xinput_record_set_nav_hooks(cap_observe, cap_close, cap_log, g_recorder);
}
/* One poll: the pad of `ch` is observed by the record (and so by the recorder side), then the fake game consumes it.
 * . rest  D U down/up  A select  S START  M mark then rest  m mark and DOWN  a mark and select  C O page closes / opens  1 3 page opening (not ready) / open
 * k i the count / item id word change * X rest, then a hotkey chord takes A out of the record from `suppress_from`  Y select, then the same chord */
static void rec_poll(uint64_t poll, char ch, uint64_t suppress_from)
{
    xinput_pad_state pad;
    memset(&pad, 0, sizeof pad);
    if (ch == 'D' || ch == 'm') pad.digital_buttons = XINPUT_BUTTON_DPAD_DOWN;
    if (ch == 'U') pad.digital_buttons = XINPUT_BUTTON_DPAD_UP;
    if (ch == 'A' || ch == 'a' || ch == 'Y') pad.analog[0] = 255u;
    if (ch == 'S') pad.digital_buttons = XINPUT_BUTTON_START;
    if (ch == 'C' && w.page_addr != 0u) set32(w.page_addr + 0x1B0u, 5u);  /* the page is closing */
    if (ch == 'O' && w.page_addr != 0u) set32(w.page_addr + 0x1B0u, 3u);  /* it is open again */
    if (ch == '1' && w.page_addr != 0u) set32(w.page_addr + 0x1B0u, 2u);  /* opening: not ready */
    if (ch == '3' && w.page_addr != 0u) set32(w.page_addr + 0x1B0u, 3u);
    if (ch == 'k' && w.page_addr != 0u) set32(w.page_addr + 0x180u, 5u);   /* the count changes on its own */
    if (ch == 'i' && w.page_addr != 0u) set32(w.page_addr + 0x18Cu, 0x77u); /* so does the item id */
    if (ch == 'M' || ch == 'm' || ch == 'a') raise(SIGUSR2);
    w.now = poll * MS_PER_POLL;
    xinput_record_observe(poll, 0u, &pad);
    apply_pad(&pad);
    if (ch == 'X' || ch == 'Y') xinput_record_suppress_begin(&A_EFFECT, suppress_from);
}
static void rec_run(const char *plan, uint64_t suppress_from)
{
    for (size_t i = 0u; plan[i] != '\0'; i++) rec_poll(i, plan[i], suppress_from);
}
static char g_rec_text[12000];
static void rec_end(void)
{
    xinput_record_close();
    xinput_record_set_nav_hooks(NULL, NULL, NULL, NULL);
    if (g_recorder != NULL) nav_recorder_free(g_recorder);
    g_recorder = NULL;
    FILE *file = fopen(g_rec_path, "r");
    const size_t got = file != NULL ? fread(g_rec_text, 1u, sizeof g_rec_text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    g_rec_text[got] = '\0';
    char pid_path[96];
    snprintf(pid_path, sizeof pid_path, "%s.pid", g_rec_path);
    remove(pid_path);
}
static unsigned count_text(const char *part)
{
    unsigned n = 0u;
    for (const char *at = g_rec_text; (at = strstr(at, part)) != NULL; at += strlen(part)) n++;
    return n;
}
/* Record `plan` on a fresh single page world, return its text in g_rec_text. */
typedef struct {
    const fake_row *rows;
    unsigned count;
    uint32_t cursor;
    bool closes;     /* the select press closes the page */
    uint32_t state;  /* the page state (3 = ready) */
} rec_world;
static void rec_session_table(const rec_world *world, const char *table, const char *plan, uint64_t suppress_from);
static void rec_session(const rec_world *world, const char *plan, uint64_t suppress_from)
{
    rec_session_table(world, REC_TABLE, plan, suppress_from);
}
static void rec_session_table(const rec_world *world, const char *table, const char *plan, uint64_t suppress_from)
{
    (void)setup_single_page(world->rows, world->count, world->cursor);
    set32(0x1010u, 1u); /* the title of the table is not on screen */
    w.a_closes_page = world->closes;
    set32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x1B0u, world->state);
    rec_begin(table, true);
    rec_run(plan, suppress_from);
    rec_end();
}
static const rec_world LIST_WORLD = {LIST_ROWS, LIST_ROW_COUNT, 1u, true, 3u};

static void test_record_nav_lines(void)
{
    char error[300];
    /* the profile list: 3 DOWN presses (1 -> 2 -> 3 -> 4) then A on 'Delta' (id 4): exactly one nav line, at = the first press (poll 5),
     * to = the first poll with A up (poll 25), the row has a unique id */
    rec_session(&LIST_WORLD, ".....DD...DD...DD...." "...AA...", 0u);
    CHECK_HAS(g_rec_text, "# nav: at=5 to=26 menu=pg select=id:4 activate\n");
    CHECK(count_text("# nav:") == 1u && count_text("# mark:") == 0u);
    CHECK(xinput_record_navs_written() == 1u);
    CHECK(log_has("nav-line at=5 to=26 edge=24 menu=pg select=id:4 activate"));
    /* the record carries the line before the run it belongs to, and parses with the replay side's own parser and checks */
    uint64_t frames = 0u;
    CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && frames == 29u);
    CHECK(xinput_replay_nav_count() == 1u && strcmp(xinput_replay_nav(0u), "at=5 to=26 menu=pg select=id:4 activate") == 0);
    route_nav_step step;
    CHECK(route_nav_line_parse(xinput_replay_nav(0u), &step, error, sizeof error));
    const uint64_t *marks = NULL;
    const size_t mark_count = xinput_replay_marks(&marks);
    CHECK(route_nav_steps_check(&step, 1u, marks, mark_count, frames, error, sizeof error));
    CHECK(step.at == 5u && step.to == 26u && step.kind == ROUTE_NAV_SELECT_ID && step.index == 4u && step.activate);
    {
        const char *nav = strstr(g_rec_text, "# nav:"), *first_run_with_dpad = strstr(g_rec_text, "2 DOWN");
        CHECK(nav != NULL && first_run_with_dpad != NULL && nav < first_run_with_dpad); /* in front of the run of poll 5 */
    }
    /* the pad states are byte identical to a record made without the feature */
    xinput_pad_state with[40], without[40];
    for (uint64_t i = 0u; i < frames && i < 40u; i++) with[i] = xinput_script_state_at(xinput_replay_script(), i);
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    w.a_closes_page = true;
    rec_begin(REC_TABLE, false);
    rec_run(".....DD...DD...DD...." "...AA...", 0u);
    rec_end();
    CHECK(count_text("# nav:") == 0u);
    uint64_t frames_without = 0u;
    CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &frames_without) && frames_without == frames);
    CHECK(frames == 29u);
    for (uint64_t i = 0u; i < frames && i < 40u; i++) {
        without[i] = xinput_script_state_at(xinput_replay_script(), i);
        CHECK(memcmp(&with[i], &without[i], sizeof with[i]) == 0);
    }
    remove(g_rec_path);

    /* the event log: changes only, then the page leaving */
    CHECK(g_log_count == 0u); /* the second session had no recorder */
    rec_session(&LIST_WORLD, ".....DD...DD...DD...." "...AA...", 0u);
    CHECK(g_log_count == 6u);
    CHECK_STR(g_log[0], "nav menu=pg cursor=1 count=6 id=0x1 ready=1");
    CHECK_STR(g_log[1], "nav menu=pg cursor=2 count=6 id=0x2 ready=1");
    CHECK_STR(g_log[2], "nav menu=pg cursor=3 count=6 id=0x3 ready=1");
    CHECK_STR(g_log[3], "nav menu=pg cursor=4 count=6 id=0x4 ready=1");
    CHECK_STR(g_log[4], "nav menu=none");
    CHECK_STR(g_log[5], "nav-line at=5 to=26 edge=24 menu=pg select=id:4 activate");
    remove(g_rec_path);

    /* a press while the menu is not ready (state 2) is not an activation */
    {
        const rec_world opening = {LIST_ROWS, LIST_ROW_COUNT, 1u, true, 2u};
        rec_session(&opening, ".....DD...DD...AA...", 0u);
        CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=not-ready") && log_has("ready=0"));
        remove(g_rec_path);
    }
    /* a greyed row (3), a skipped row (0), the cursor on no row (none) */
    {
        const rec_world greyed = {LIST_ROWS, LIST_ROW_COUNT, 3u, true, 3u};
        rec_session(&greyed, "...AA...", 0u);
        CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=greyed-row row=3 flags=0x1"));
        remove(g_rec_path);
        const rec_world skipped = {LIST_ROWS, LIST_ROW_COUNT, 0u, true, 3u};
        rec_session(&skipped, "...AA...", 0u);
        CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=skipped-row row=0 flags=0x4"));
        remove(g_rec_path);
        const rec_world none = {LIST_ROWS, LIST_ROW_COUNT, ROUTE_NAV_CURSOR_NONE, true, 3u};
        rec_session(&none, "...AA...", 0u);
        CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=no-cursor-row"));
        remove(g_rec_path);
    }
    /* the line sits between the idle run and the run of the first press, where `at` is */
    rec_session(&LIST_WORLD, ".....DD......AA...", 0u);
    CHECK_HAS(g_rec_text, "5\n# nav: at=5 to=15 menu=pg select=id:2 activate\n2 DOWN\n");
    remove(g_rec_path);
    /* a cursor equal to the count is on no row */
    {
        const rec_world past = {LIST_ROWS, LIST_ROW_COUNT, LIST_ROW_COUNT, true, 3u};
        rec_session(&past, "...AA...", 0u);
        CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=no-cursor-row"));
        remove(g_rec_path);
    }
    /* the presses of an earlier page instance do not count: the page closes and opens again before the select press */
    rec_session(&LIST_WORLD, "..DD..CC.OO...AA...", 0u);
    CHECK_HAS(g_rec_text, "# nav: at=14 to=16 menu=pg select=id:2 activate\n");
    remove(g_rec_path);
    /* a press of another button is not a d-pad press */
    rec_session(&LIST_WORLD, "..SS.....AA...", 0u);
    CHECK_HAS(g_rec_text, "# nav: at=9 to=11 menu=pg select=id:1 activate\n");
    remove(g_rec_path);
    /* the ready flag is part of what is logged: opening (not ready) then open */
    rec_session(&LIST_WORLD, "1...3..", 0u);
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=0") && log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=1"));
    remove(g_rec_path);
    /* each logged value alone: the count, the item id, and the cursor of a menu without item ids */
    rec_session(&LIST_WORLD, ".k..", 0u);
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=1") && log_has("nav menu=pg cursor=1 count=5 id=0x1 ready=1"));
    remove(g_rec_path);
    rec_session(&LIST_WORLD, ".i..", 0u);
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=1") && log_has("nav menu=pg cursor=1 count=6 id=0x77 ready=1"));
    remove(g_rec_path);
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    rec_begin("widgets table=0x8000 stride=0x23C count=6 closing=4,5\nmenu pg page=builder:0x2D2350 active=0x1000:4==1 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=1\n", true);
    rec_run("DD...", 0u);
    rec_end();
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=- ready=1") && log_has("nav menu=pg cursor=2 count=6 id=- ready=1"));
    remove(g_rec_path);
    /* no select press on a nav menu at all (the d-pad only): no line */
    rec_session(&LIST_WORLD, "...DD...DD....", 0u);
    CHECK(count_text("# nav:") == 0u);
    remove(g_rec_path);
    /* no d-pad press: at is the select edge itself (poll 3), to the first poll with A up (5) */
    rec_session(&LIST_WORLD, "...AA...", 0u);
    CHECK_HAS(g_rec_text, "# nav: at=3 to=5 menu=pg select=id:1 activate\n");
    remove(g_rec_path);

    /* a mark strictly inside (at, to) drops the line and says why; a mark exactly at `at` or at `to` keeps it */
    rec_session(&LIST_WORLD, ".....DD.M...AA....", 0u);
    CHECK(count_text("# nav:") == 0u && count_text("# mark: at=8\n") == 1u && log_has("nav-skip menu=pg reason=mark-inside at=5 to=14"));
    remove(g_rec_path);
    rec_session(&LIST_WORLD, ".....mD......AAM...", 0u); /* a mark on the first press poll (= at) and one on the first poll with A up (= to) */
    CHECK_HAS(g_rec_text, "# mark: at=5\n");
    CHECK_HAS(g_rec_text, "# mark: at=15\n");
    CHECK_HAS(g_rec_text, "# nav: at=5 to=15 menu=pg select=id:2 activate\n");
    CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && xinput_replay_nav_count() == 1u);
    CHECK(route_nav_line_parse(xinput_replay_nav(0u), &step, error, sizeof error));
    CHECK(xinput_replay_marks(&marks) == 2u && route_nav_steps_check(&step, 1u, marks, 2u, frames, error, sizeof error));
    remove(g_rec_path);
    /* a mark one poll after `at`, or on the select edge poll, is strictly inside; one before `at` is not */
    rec_session(&LIST_WORLD, ".....DM......AA...", 0u);
    CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=mark-inside at=5 to=15"));
    remove(g_rec_path);
    rec_session(&LIST_WORLD, ".....DD.....aA...", 0u);
    CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=mark-inside at=5 to=14") && count_text("# mark: at=12\n") == 1u);
    remove(g_rec_path);
    rec_session(&LIST_WORLD, "....MDD......AA...", 0u);
    CHECK_HAS(g_rec_text, "# mark: at=4\n");
    CHECK_HAS(g_rec_text, "# nav: at=5 to=15 menu=pg select=id:2 activate\n");
    remove(g_rec_path);
}

static void test_record_nav_suppression_and_edges(void)
{
    /* a chord key that leaked A into the record is cut out by the suppression: live (the press is already suppressed when it arrives) ... */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    w.a_closes_page = true;
    rec_begin(REC_TABLE, true);
    xinput_record_suppress_begin(&A_EFFECT, 0u);
    rec_run(".....DD......AA...", 0u);
    xinput_record_suppress_end(&A_EFFECT, 18u);
    rec_end();
    CHECK(count_text("# nav:") == 0u && xinput_record_navs_written() == 0u);
    remove(g_rec_path);
    /* ... or retroactively, after the nav line was made (the key went down at poll 13, the chord was recognised at poll 19) */
    rec_session(&LIST_WORLD, ".....DD......AA..X.", 13u);
    CHECK(count_text("# nav:") == 0u && log_has("nav-skip reason=suppressed at=5 to=15 menu=pg"));
    remove(g_rec_path);
    /* the chord is recognised while the select button is still down (poll 14): the line is refused when it is made, the edge is gone */
    rec_session(&LIST_WORLD, ".....DD......AYA.", 13u);
    CHECK(count_text("# nav:") == 0u && log_has("nav-skip menu=pg reason=suppressed at=5 to=15"));
    remove(g_rec_path);
    /* a suppression that only starts after the press (from poll 15) leaves the select edge in the record: the line stays */
    rec_session(&LIST_WORLD, ".....DD......AA..X.", 15u);
    CHECK_HAS(g_rec_text, "# nav: at=5 to=15 menu=pg select=id:2 activate\n");
    remove(g_rec_path);

    /* the select button still down at the close: to is the total number of polls */
    rec_session(&LIST_WORLD, ".....DD......AAAA", 0u);
    CHECK_HAS(g_rec_text, "# nav: at=5 to=17 menu=pg select=id:2 activate\n");
    remove(g_rec_path);

    /* ids: a row id that is not unique in the list, or 0xFFFFFFFF, is selected by its index */
    {
        static const fake_row dup[] = {{"X", 5u, 0u, 4u}, {"Y", 5u, 0u, 4u}, {"Z", 6u, 0u, 4u}, {"W", 0xFFFFFFFFu, 0u, 4u}};
        const rec_world on_dup = {dup, 4u, 1u, true, 3u}, on_unique = {dup, 4u, 2u, true, 3u}, on_ffff = {dup, 4u, 3u, true, 3u};
        rec_session(&on_dup, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=2 to=4 menu=pg select=index:1 activate\n");
        remove(g_rec_path);
        rec_session(&on_unique, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=2 to=4 menu=pg select=id:6 activate\n");
        remove(g_rec_path);
        rec_session(&on_ffff, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=2 to=4 menu=pg select=index:3 activate\n");
        remove(g_rec_path);
    }
    /* a menu without a cursor (the title) is activated with select=index:0 */
    reset_world(1u, 0u, true, false);
    w.a_closes = true;
    rec_begin(REC_TABLE, true);
    rec_run("...AA...", 0u);
    rec_end();
    CHECK_HAS(g_rec_text, "# nav: at=3 to=5 menu=title select=index:0 activate\n");
    CHECK(log_has("nav menu=title cursor=- count=- id=- ready=1"));
    remove(g_rec_path);
    /* and with the extra condition of the title not holding it is not a menu */
    reset_world(1u, 0u, true, false);
    set32(0x1010u, 1u);
    rec_begin(REC_TABLE, true);
    rec_run("...AA...", 0u);
    rec_end();
    CHECK(count_text("# nav:") == 0u && log_has("nav menu=none") && !log_has("menu=title"));
    remove(g_rec_path);

    /* two activations on a page that stays: the second line begins after the first ended and the lines are ascending */
    {
        const rec_world stays = {LIST_ROWS, LIST_ROW_COUNT, 1u, false, 3u};
        rec_session(&stays, ".DD...AA...UU...AA...", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=1 to=8 menu=pg select=id:2 activate\n");
        CHECK_HAS(g_rec_text, "# nav: at=11 to=18 menu=pg select=id:1 activate\n");
        CHECK(count_text("# nav:") == 2u && strstr(g_rec_text, "at=1 to=8") < strstr(g_rec_text, "at=11 to=18"));
        remove(g_rec_path);
    }
}

static void test_real_table_recorder(void)
{
    static char text[80000];
    static route_nav_menu_table real;
    char error[300];
    CHECK(read_real_table(text, sizeof text) && route_nav_menus_parse(text, strlen(text), &real, error, sizeof error));
    /* the recorder side over the real table: its menus all start with the mode word, so one read per poll decides when the mode is none of theirs, and
     * a mode of the table costs the 20 used words of the widget table plus the page conditions of the menus whose builder is open */
    {
        reset_world(8u, 0u, true, true);
        set32(0x79094Cu, 0x77u);
        const nav_record_hooks hooks = {{fake_read_mem, fake_read_bytes, fake_now, NULL, NULL, NULL, NULL}, cap_log, NULL, cap_add, NULL};
        nav_recorder *recorder = nav_recorder_create(&real, &hooks);
        CHECK(recorder != NULL);
        if (recorder != NULL) {
            g_log_count = 0u;
            w.reads = 0u;
            for (uint64_t i = 0u; i < 5u; i++) nav_recorder_observe(recorder, i, &(xinput_pad_state){0});
            uint64_t gated = 0u, scanned = 0u;
            nav_recorder_stats(recorder, &gated, &scanned, NULL, NULL);
            CHECK(w.reads == 5u && gated == 5u && scanned == 0u && g_log_count == 1u);
            set32(0x79094Cu, 0x65u);
            w.reads = 0u;
            nav_recorder_observe(recorder, 5u, &(xinput_pad_state){0});
            nav_recorder_stats(recorder, &gated, &scanned, NULL, NULL);
            CHECK(scanned == 1u && w.reads >= 1u + 20u && w.reads < 1u + 20u + 20u); /* no widget in use: the title (page=none) is the only menu evaluated */
            nav_recorder_free(recorder);
        }
    }
}

/* ---- B1, B2, B5 (agent E's measurements on the real title) ---- */
#define PG_LINE_COMMON "menu pgx page=builder:0x2D2350 active=0x1000:4==1 ready=@+0x1B0:4==3,@+0x170:4==0 cursor=@+0x184:4 count=@+0x180:4 axis=v wrap=1 itemid=@+0x18C:4 " \
    "items=walk(@+0x178:4;next=+0x8;type=+0x0:4|7|8;id=+0x14;text=+0x20;flags=+0x28;skip=0x4;grey=0x1) select=A"
static const fake_row PROFILE_ROWS[] = {
    {"Player 1", 0xFFFFFFFDu, 0x4u, 4u}, /* the title row says the same as a profile: skipped by the cursor */
    {"Create New Profile", 0xFFFFFFFAu, 0u, 4u},
    {"Player 1", 0u, 0u, 4u},            /* the id of a profile is its list position */
    {"Player 2", 1u, 0u, 4u},
    {"hint", 0xFFFFFFFFu, 0x4u, 8u},
};
static void table_with(char *out, size_t size, const char *extra, const char *id)
{
    snprintf(out, size, "widgets table=0x8000 stride=0x23C count=6 closing=4,5\n%s %s\n", PG_LINE_COMMON, extra);
    char *at = strstr(out, "menu pgx");
    if (at != NULL) memcpy(at + 5, id, 3u); /* "pgx" -> the 3 letter id given */
}

static void test_skipped_names_and_dpad(void)
{
    char table[1400];
    /* B1: the title row (skipped) says "Player 1" as well: the name matches the profile only */
    (void)setup_single_page(PROFILE_ROWS, 5u, 1u);
    begin_with("at=10 to=40 menu=pgx select=name:Player%201", (table_with(table, sizeof table, "", "pgx"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(300u) == ROUTE_NAV_DONE && cursor_now() == 2u);
    /* two non skipped rows with one text are still ambiguous (case blind), and the message names both */
    {
        static const fake_row twice[] = {{"Title", 0xFFFFFFFDu, 0x4u, 4u}, {"Same", 1u, 0u, 4u}, {"same", 2u, 0u, 4u}, {"Other", 3u, 0u, 4u}};
        (void)setup_single_page(twice, 4u, 1u);
        begin_with("at=10 to=40 menu=pgx select=name:SAME", (table_with(table, sizeof table, "", "pgx"), table), ROUTE_NAV_MODE_ON, NULL);
        CHECK(drive(300u) == ROUTE_NAV_FAILED);
        CHECK_HAS(route_nav_message(&g_machine), "name 'SAME' is ambiguous, it matches items 1, 2");
        /* a text that only a skipped row has is not found, and the list says which rows are skipped */
        (void)setup_single_page(twice, 4u, 1u);
        begin_with("at=10 to=40 menu=pgx select=name:title", (table_with(table, sizeof table, "", "pgx"), table), ROUTE_NAV_MODE_ON, NULL);
        CHECK(drive(300u) == ROUTE_NAV_FAILED);
        CHECK_HAS(route_nav_message(&g_machine), "name 'title' is not among the menu's 4 item(s)");
        CHECK_HAS(route_nav_message(&g_machine), "0:'Title'#0xFFFFFFFD(skipped)");
    }
    /* a greyed row still matches (so the greyed message stays) */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 3u);
    begin_with("at=10 to=40 menu=pgx select=name:gamma%20ray activate", (table_with(table, sizeof table, "", "pgx"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "is greyed (flags 0x1)");

    /* B2: dpad=0. The cursor is on the target: activated at once, nothing pressed before */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    w.a_closes_page = true;
    begin_with("at=10 to=40 menu=ptr select=name:beta activate", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE);
    CHECK_STR(w.trace, "AA....");
    CHECK(g_machine.presses == 0u && w.edges == 0u && w.activations == 1u);
    /* the cursor is elsewhere: the d-pad is never pressed, the cursor arrives by itself at poll 10 (the recorded stick movement), then A */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    w.a_closes_page = true;
    begin_with("at=10 to=40 menu=ptr select=name:beta activate", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    g_pre_poll = hook_cursor_arrives;
    CHECK(drive(300u) == ROUTE_NAV_DONE);
    g_pre_poll = NULL;
    CHECK(w.edges == 0u && g_machine.presses == 0u && w.activations == 1u);
    CHECK(w.trace[9] == '.' && w.trace[10] == 'A' && w.trace[11] == 'A');
    for (size_t i = 0u; i < w.trace_len; i++) CHECK(w.trace[i] == '.' || w.trace[i] == 'A'); /* never a d-pad bit */
    /* it never gets there: FAILS after 3 s with the verbatim reason and the names */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    begin_with("at=10 to=40 menu=ptr select=name:beta activate", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "route FAILED: nav step 1 (menu=ptr select=name:beta) menu ptr does not take the d-pad (pointer driven), cursor is at 1, wanted 2: "
                                             "the recorded stick movement before the step did not reach the item;");
    CHECK_HAS(route_nav_message(&g_machine), "names=[0:'Choose one'#0xFFFFFFFD(skipped), 1:'Alpha'#0x1, 2:'Beta'#0x2");
    CHECK(w.edges == 0u && g_machine.presses == 0u && g_polls_run == 189u); /* 3008 ms after the first poll that saw it (poll 0 at 0 ms): 3000 ms is reached at poll 188 */
    for (size_t i = 0u; i < w.trace_len; i++) CHECK(w.trace[i] == '.');
    /* an index: select lists the names too; the cursor none is named */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, ROUTE_NAV_CURSOR_NONE);
    begin_with("at=10 to=40 menu=ptr select=index:2 timeout=100000", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(1000u) == ROUTE_NAV_FAILED);
    CHECK_HAS(route_nav_message(&g_machine), "cursor is at none, wanted 2");
    CHECK_HAS(route_nav_message(&g_machine), "names=[");
    /* the grace is inclusive: 1000 ms polls, first seen at 0 ms, it fails on poll 3 */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    g_ms_per_poll = 1000u;
    begin_with("at=10 to=40 menu=ptr select=index:2", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(50u) == ROUTE_NAV_FAILED && g_polls_run == 4u);
    /* without activate: done as soon as the cursor is on the target */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 2u);
    begin_with("at=10 to=40 menu=ptr select=index:2", (table_with(table, sizeof table, "dpad=0", "ptr"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(50u) == ROUTE_NAV_DONE && g_polls_run == 1u);
    /* dpad=1 is the default and presses */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    begin_with("at=10 to=40 menu=pgd select=index:2", (table_with(table, sizeof table, "dpad=1", "pgd"), table), ROUTE_NAV_MODE_ON, NULL);
    CHECK(drive(100u) == ROUTE_NAV_DONE && g_machine.presses == 1u);

    /* the table keys */
    static route_nav_menu_table parsed;
    char error[300];
    table_with(table, sizeof table, "dpad=0 selectby=name", "ptr");
    CHECK(route_nav_menus_parse(table, strlen(table), &parsed, error, sizeof error));
    CHECK(parsed.menus[0].dpad_off && parsed.menus[0].selectby == ROUTE_NAV_BY_NAME);
    table_with(table, sizeof table, "selectby=index", "ptr");
    CHECK(route_nav_menus_parse(table, strlen(table), &parsed, error, sizeof error) && !parsed.menus[0].dpad_off && parsed.menus[0].selectby == ROUTE_NAV_BY_INDEX);
    table_with(table, sizeof table, "selectby=id dpad=1", "ptr");
    CHECK(route_nav_menus_parse(table, strlen(table), &parsed, error, sizeof error) && !parsed.menus[0].dpad_off && parsed.menus[0].selectby == ROUTE_NAV_BY_ID);
    table_with(table, sizeof table, "", "ptr");
    CHECK(route_nav_menus_parse(table, strlen(table), &parsed, error, sizeof error) && !parsed.menus[0].dpad_off && parsed.menus[0].selectby == ROUTE_NAV_BY_ID);
    const struct { const char *keys; const char *reason; } bad[] = {
        {"dpad=2", "dpad= wants 0 or 1"}, {"dpad=0 dpad=1", "'dpad' given twice"}, {"selectby=text", "selectby= wants id, name or index"},
        {"selectby=name selectby=id", "'selectby' given twice"}, {"dpad=", "dpad= wants 0 or 1"},
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        table_with(table, sizeof table, bad[i].keys, "ptr");
        error[0] = '\0';
        CHECK(!route_nav_menus_parse(table, strlen(table), &parsed, error, sizeof error));
        CHECK(error[0] != '\0');
        CHECK_HAS(error, bad[i].reason);
    }
    const char *cursorless = "menu t active=0x1000:4==1 dpad=0\n";
    CHECK(!route_nav_menus_parse(cursorless, strlen(cursorless), &parsed, error, sizeof error) && strstr(error, "without a cursor") != NULL);
    cursorless = "menu t active=0x1000:4==1 selectby=name\n";
    CHECK(!route_nav_menus_parse(cursorless, strlen(cursorless), &parsed, error, sizeof error) && strstr(error, "without a cursor") != NULL);
}

static void test_record_selectby(void)
{
    char table[1400], error[300];
    /* selectby=name: the profile list writes the text, percent encoded, instead of the list position */
    {
        const rec_world profiles = {PROFILE_ROWS, 5u, 2u, true, 3u};
        table_with(table, sizeof table, "selectby=name", "pgx");
        rec_session_table(&profiles, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=2 to=4 menu=pgx select=name:Player%201 activate\n"); /* the skipped title with the same text does not make it ambiguous */
        CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &(uint64_t){0}) && xinput_replay_nav_count() == 1u);
        route_nav_step step;
        CHECK(route_nav_line_parse(xinput_replay_nav(0u), &step, error, sizeof error) && step.kind == ROUTE_NAV_SELECT_NAME && strcmp(step.name, "Player 1") == 0);
        remove(g_rec_path);
        const rec_world second = {PROFILE_ROWS, 5u, 3u, true, 3u};
        rec_session_table(&second, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=name:Player%202 activate\n");
        remove(g_rec_path);
        /* the default (id) and index write what they did */
        table_with(table, sizeof table, "", "pgx");
        rec_session_table(&profiles, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:0 activate\n");
        remove(g_rec_path);
        table_with(table, sizeof table, "selectby=index", "pgx");
        rec_session_table(&profiles, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=index:2 activate\n");
        remove(g_rec_path);
        table_with(table, sizeof table, "selectby=id", "pgx");
        rec_session_table(&profiles, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:0 activate\n");
        remove(g_rec_path);
    }
    /* the text is not unique among the cursor rows (case blind): the id; no unique id either: the index */
    {
        static const fake_row same[] = {{"Map", 5u, 0u, 4u}, {"MAP", 6u, 0u, 4u}, {"Other", 7u, 0u, 4u}, {"Map", 0xFFFFFFFFu, 0u, 4u}};
        table_with(table, sizeof table, "selectby=name", "pgx");
        const rec_world dup = {same, 4u, 0u, true, 3u}, last = {same, 4u, 3u, true, 3u}, other = {same, 4u, 2u, true, 3u};
        rec_session_table(&dup, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:5 activate\n");
        remove(g_rec_path);
        rec_session_table(&last, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=index:3 activate\n");
        remove(g_rec_path);
        rec_session_table(&other, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=name:Other activate\n");
        remove(g_rec_path);
    }
    /* texts that differ only in case are one name to the replay (case blind): not unique */
    {
        static const fake_row cased[] = {{"Abc", 1u, 0u, 4u}, {"aBC", 2u, 0u, 4u}, {"Xyz", 3u, 0u, 4u}};
        table_with(table, sizeof table, "selectby=name", "pgx");
        const rec_world first = {cased, 3u, 0u, true, 3u};
        rec_session_table(&first, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:1 activate\n");
        remove(g_rec_path);
    }
    /* an empty text, and text that does not fit a select= after encoding, fall back to the id */
    {
        char long_text[64];
        memset(long_text, ' ', 40u);
        long_text[40] = '\0'; /* 40 spaces encode to 120 characters, a select= takes 111 */
        static fake_row odd[5];
        char edge_fit[64], edge_over[64];
        memset(edge_fit, ' ', 35u);
        memcpy(edge_fit + 35, "x", 2u);   /* 35 spaces + x = 106 characters encoded: the longest that fits */
        memset(edge_over, ' ', 35u);
        memcpy(edge_over + 35, "xy", 3u); /* 107: one too many */
        odd[0] = (fake_row){"", 8u, 0u, 4u};
        odd[1] = (fake_row){long_text, 9u, 0u, 4u};
        odd[2] = (fake_row){"A b=c%d", 10u, 0u, 4u};
        odd[3] = (fake_row){edge_fit, 11u, 0u, 4u};
        odd[4] = (fake_row){edge_over, 12u, 0u, 4u};
        table_with(table, sizeof table, "selectby=name", "pgx");
        g_text_stride = 64u;
        const rec_world empty = {odd, 5u, 0u, true, 3u}, wide = {odd, 5u, 1u, true, 3u}, mixed = {odd, 5u, 2u, true, 3u};
        const rec_world fits = {odd, 5u, 3u, true, 3u}, over = {odd, 5u, 4u, true, 3u};
        rec_session_table(&empty, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:8 activate\n");
        remove(g_rec_path);
        rec_session_table(&wide, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:9 activate\n"); /* 40 spaces would be 120 characters */
        remove(g_rec_path);
        rec_session_table(&fits, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=name:%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20x activate\n");
        CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &(uint64_t){0}) && xinput_replay_nav_count() == 1u);
        remove(g_rec_path);
        rec_session_table(&over, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=id:12 activate\n");
        remove(g_rec_path);
        g_text_stride = 16u;
        rec_session_table(&mixed, table, "..AA..", 0u);
        CHECK_HAS(g_rec_text, "menu=pgx select=name:A%20b%3Dc%25d activate\n");
        CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &(uint64_t){0}) && xinput_replay_nav_count() == 1u);
        route_nav_step step;
        CHECK(route_nav_line_parse(xinput_replay_nav(0u), &step, error, sizeof error) && strcmp(step.name, "A b=c%d") == 0);
        remove(g_rec_path);
    }
    /* B2 recorder: a menu that does not take the d-pad has no presses to replace: at is the select edge even after d-pad presses */
    {
        const rec_world ptr = {LIST_ROWS, LIST_ROW_COUNT, 1u, true, 3u};
        table_with(table, sizeof table, "dpad=0", "ptr");
        rec_session_table(&ptr, table, ".DD......AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=9 to=11 menu=ptr select=id:2 activate\n");
        remove(g_rec_path);
        table_with(table, sizeof table, "", "ptr");
        rec_session_table(&ptr, table, ".DD......AA..", 0u);
        CHECK_HAS(g_rec_text, "# nav: at=1 to=11 menu=ptr select=id:2 activate\n");
        remove(g_rec_path);
    }
}

static void test_record_nav_limits_cost_and_host(void)
{
    char error[300];
    /* at most 64 nav lines: the 65th is refused with its reason (direct calls, the select edge is beyond the record) */
    reset_world(1u, 0u, true, false);
    rec_begin(REC_TABLE, false);
    for (uint64_t i = 0u; i < 70u; i++) xinput_record_observe(i, 0u, &(xinput_pad_state){0});
    unsigned accepted = 0u;
    const char *last_reason = NULL;
    for (uint64_t i = 0u; i < 65u; i++) {
        const char *reason = xinput_record_add_nav(i, i + 1u, 1000000u, "menu=pg select=index:0 activate", 0, 0u);
        if (reason == NULL) accepted++;
        else last_reason = reason;
    }
    CHECK(accepted == 64u && last_reason != NULL && strcmp(last_reason, "limit-64") == 0);
    rec_end();
    CHECK(count_text("# nav:") == 64u);
    CHECK(xinput_replay_load(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error, &(uint64_t){0}) && xinput_replay_nav_count() == 64u);
    remove(g_rec_path);
    /* at above to, overlapping the previous line (at is raised to its end, then it is past `to`) */
    reset_world(1u, 0u, true, false);
    rec_begin(REC_TABLE, false);
    for (uint64_t i = 0u; i < 70u; i++) xinput_record_observe(i, 0u, &(xinput_pad_state){0});
    CHECK(xinput_record_add_nav(10u, 20u, 1000000u, "menu=pg select=index:0 activate", 0, 0u) == NULL);
    CHECK(strcmp(xinput_record_add_nav(30u, 25u, 1000000u, "menu=pg select=index:0 activate", 0, 0u), "at-above-to") == 0);
    CHECK(strcmp(xinput_record_add_nav(12u, 15u, 1000000u, "menu=pg select=index:0 activate", 0, 0u), "overlap") == 0);
    CHECK(xinput_record_add_nav(15u, 25u, 1000000u, "menu=pg select=index:0 activate", 0, 0u) == NULL); /* at raised from 15 to 20 */
    rec_end();
    CHECK_HAS(g_rec_text, "# nav: at=10 to=20 ");
    CHECK_HAS(g_rec_text, "# nav: at=20 to=25 ");
    remove(g_rec_path);

    /* the cost: when the mode word is not one of the table's, one read per poll; when it is but no page of the table is open, one read plus
     * the used word of each of the 6 slots */
    reset_world(1u, 0u, true, false);
    set32(ADDR_ACTIVE, 0u);
    rec_begin(REC_TABLE, true);
    w.reads = 0u;
    for (uint64_t i = 0u; i < 10u; i++) xinput_record_observe(i, 0u, &(xinput_pad_state){0});
    CHECK(w.reads == 10u);
    uint64_t gated = 0u, scanned = 0u;
    nav_recorder_stats(g_recorder, &gated, &scanned, NULL, NULL);
    CHECK(gated == 10u && scanned == 0u);
    CHECK(log_has("nav menu=none") && g_log_count == 1u); /* once, not at every poll */
    rec_end();
    remove(g_rec_path);
    reset_world(1u, 0u, true, false); /* the gate is open (word 1) but no widget slot is in use: title is page=none, its 2nd condition fails */
    set32(0x1010u, 1u);
    rec_begin(REC_TABLE, true);
    w.reads = 0u;
    for (uint64_t i = 0u; i < 10u; i++) xinput_record_observe(i, 0u, &(xinput_pad_state){0});
    CHECK(w.reads == 10u * (1u + 6u + 2u)); /* gate, the 6 used words, the title's two conditions (the second fails) */
    nav_recorder_stats(g_recorder, &gated, &scanned, NULL, NULL);
    CHECK(gated == 0u && scanned == 10u);
    rec_end();
    remove(g_rec_path);

    /* two menus on screen at once: the one with a cursor and ready wins over a cursorless one; a menu alone is logged with its dashes */
    (void)setup_page_world(1u);
    set32(ADDR_WIDGETS + 1u * WIDGET_STRIDE + 0x1B0u, 3u);
    rec_begin(REC_TABLE, true);
    rec_run("..", 0u);
    rec_end();
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=1") && !log_has("menu=box"));
    remove(g_rec_path);
    reset_world(1u, 0u, true, false);
    (void)build_page(1u, BUILDER_OTHER, 3u, DECOY_ROWS, 3u, 0u, ADDR_NODES, ADDR_TEXTS);
    rec_begin(REC_TABLE, true);
    rec_run("..", 0u);
    rec_end();
    CHECK(log_has("nav menu=box cursor=- count=- id=- ready=1") && !log_has("menu=pg"));
    remove(g_rec_path);
    /* a menu that is not ready is still the menu (ready=0) */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    set32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x1B0u, 2u);
    rec_begin(REC_TABLE, true);
    rec_run(".", 0u);
    rec_end();
    CHECK(log_has("nav menu=pg cursor=1 count=6 id=0x1 ready=0"));
    remove(g_rec_path);
    /* an unreadable cursor shows as ? */
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    set32(ADDR_WIDGETS + 2u * WIDGET_STRIDE + 0x184u, 1u);
    rec_begin("widgets table=0x8000 stride=0x23C count=6 closing=4,5\nmenu pg page=builder:0x2D2350 active=0x1000:4==1 cursor=@+0x9000:4 count=@+0x180:4 axis=v wrap=1\n", true);
    rec_run(".", 0u);
    rec_end();
    CHECK(log_has("nav menu=pg cursor=? count=6 id=- ready=1"));
    remove(g_rec_path);

    /* the host glue: table text, the record's hooks, the event log file; refusals */
    host_route_config config;
    memset(&config, 0, sizeof config);
    config.read_mem = fake_read_mem;
    config.read_bytes = fake_read_bytes;
    config.nav_menus_text = "# nothing\n";
    CHECK(!host_route_nav_record_start(&config, error, sizeof error) && strstr(error, "the table has no menu") != NULL);
    config.nav_menus_text = "menu broken active=nonsense\n";
    CHECK(!host_route_nav_record_start(&config, error, sizeof error) && strstr(error, "--route-nav-menus (text): line 1:") != NULL);
    config.nav_menus_text = NULL;
    config.nav_menus_file = "/nonexistent/menu_nav.txt";
    CHECK(!host_route_nav_record_start(&config, error, sizeof error) && strstr(error, "cannot read the menu table") != NULL);
    char log_path[] = "/tmp/tsfp-nav-log-XXXXXX";
    const int log_descriptor = mkstemp(log_path);
    if (log_descriptor >= 0) close(log_descriptor);
    route_probe_reset();
    route_probe_enable();
    CHECK(route_probe_log_open(log_path, error, sizeof error));
    (void)setup_single_page(LIST_ROWS, LIST_ROW_COUNT, 1u);
    set32(0x1010u, 1u);
    w.a_closes_page = true;
    config.nav_menus_file = NULL;
    config.nav_menus_text = REC_TABLE;
    snprintf(g_rec_path, sizeof g_rec_path, "/tmp/tsfp-nav-rec-XXXXXX");
    const int descriptor = mkstemp(g_rec_path);
    if (descriptor >= 0) close(descriptor);
    CHECK(xinput_record_open(g_rec_path, "aa11", "bb22", "--x 1", error, sizeof error));
    CHECK(host_route_nav_record_start(&config, error, sizeof error));
    rec_run(".....DD......AA...", 0u);
    xinput_record_close(); /* the close hook of the host glue runs */
    host_route_nav_record_stop();
    route_probe_log_close();
    FILE *log_file = fopen(log_path, "r");
    char log_text[3000];
    const size_t log_got = log_file != NULL ? fread(log_text, 1u, sizeof log_text - 1u, log_file) : 0u;
    if (log_file != NULL) fclose(log_file);
    log_text[log_got] = '\0';
    CHECK_HAS(log_text, " nav menu=pg cursor=1 count=6 id=0x1 ready=1\n");
    CHECK_HAS(log_text, " nav menu=none\n");
    CHECK_HAS(log_text, " nav-line at=5 to=15 edge=13 menu=pg select=id:2 activate\n");
    CHECK(strstr(log_text, "t=") != NULL && strstr(log_text, "poll=") != NULL);
    {
        FILE *file = fopen(g_rec_path, "r");
        const size_t got = file != NULL ? fread(g_rec_text, 1u, sizeof g_rec_text - 1u, file) : 0u;
        if (file != NULL) fclose(file);
        g_rec_text[got] = '\0';
        char pid_path[96];
        snprintf(pid_path, sizeof pid_path, "%s.pid", g_rec_path);
        remove(pid_path);
    }
    CHECK_HAS(g_rec_text, "# nav: at=5 to=15 menu=pg select=id:2 activate\n");
    remove(g_rec_path);
    remove(log_path);
    /* the flags: --route-nav-record needs --record-input, --route-nav-menus is allowed with either, the identity ignores it */
    options parsed;
    const char *rec_ok[] = {"host", "--synthetic-pad", "--record-input", "r.txt", "--route-nav-menus", "m.txt", "--route-nav-record", "off", "game.xbe"};
    CHECK(parse_options(9, (char **)rec_ok, &parsed) && parsed.route_nav_record != NULL && strcmp(parsed.route_nav_record, "off") == 0 &&
          parsed.route_nav_menus != NULL);
    const char *rec_menus_only[] = {"host", "--synthetic-pad", "--record-input", "r.txt", "--route-nav-menus", "m.txt", "game.xbe"};
    CHECK(parse_options(7, (char **)rec_menus_only, &parsed) && parsed.route_nav_record == NULL);
    const char *rec_no_record[] = {"host", "--route-nav-record", "on", "game.xbe"};
    CHECK(!parse_options(4, (char **)rec_no_record, &parsed));
    const char *rec_replay_only[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav-record", "on", "game.xbe"};
    CHECK(!parse_options(7, (char **)rec_replay_only, &parsed));
    const char *rec_bad[] = {"host", "--synthetic-pad", "--record-input", "r.txt", "--route-nav-record", "maybe", "game.xbe"};
    CHECK(!parse_options(7, (char **)rec_bad, &parsed));
    const char *rec_nav_on_record[] = {"host", "--synthetic-pad", "--record-input", "r.txt", "--route-nav", "on", "game.xbe"};
    CHECK(!parse_options(7, (char **)rec_nav_on_record, &parsed)); /* --route-nav drives a replay */
    const char *menus_alone[] = {"host", "--route-nav-menus", "m.txt", "game.xbe"};
    CHECK(!parse_options(4, (char **)menus_alone, &parsed));
    char with[300], without[300];
    const char *plain[] = {"host", "game.xbe", "--skip-intro", "--record-input", "r.txt"};
    const char *navy[] = {"host", "game.xbe", "--skip-intro", "--record-input", "r.txt", "--route-nav-menus", "m.txt", "--route-nav-record", "on"};
    CHECK(xinput_record_identity_flags(5, (char **)plain, without, sizeof without) && xinput_record_identity_flags(9, (char **)navy, with, sizeof with));
    CHECK(without[0] != '\0' && strcmp(with, without) == 0);
    const char *help = host_options_route_nav_help();
    CHECK(strstr(help, "--route-nav-record") != NULL && strstr(help, "nav menu=ID cursor=C count=K id=I ready=R") != NULL);
}

/* ======================================================================================================================
 * The route player
 * ==================================================================================================================== */
typedef struct {
    int count;
    xinput_route_event events[40];
    char details[40][900];
} event_log;
static event_log g_events;
static void on_route_event(const xinput_route_event *event, void *user)
{
    (void)user;
    if (g_events.count >= 40) return;
    g_events.events[g_events.count] = *event;
    snprintf(g_events.details[g_events.count], sizeof g_events.details[0], "%s", event->detail != NULL ? event->detail : "");
    g_events.events[g_events.count].detail = g_events.details[g_events.count];
    g_events.count++;
}
static unsigned count_events(xinput_route_event_kind kind)
{
    unsigned n = 0u;
    for (int i = 0; i < g_events.count; i++) n += g_events.events[i].kind == kind ? 1u : 0u;
    return n;
}
static int first_event(xinput_route_event_kind kind)
{
    for (int i = 0; i < g_events.count; i++)
        if (g_events.events[i].kind == kind) return i;
    return -1;
}

static xinput_script *script_of(const char *text)
{
    char error[200];
    xinput_script *script = xinput_script_parse(text, strlen(text), error, sizeof error);
    if (script == NULL) printf("script error: %s\n", error);
    return script;
}

static route_nav_step g_steps[4];
static size_t g_step_count;
static uint64_t g_clock_poll;
static uint64_t route_clock(void *user)
{
    (void)user;
    return g_clock_poll * MS_PER_POLL;
}

/* A route over `script` with `marks` and the nav steps given as lines. */
static xinput_route *make_route(const xinput_script *script, const uint64_t *marks, size_t mark_count, const xinput_route_wait *waits,
                                size_t wait_count, const char *const *nav_lines, size_t nav_count, const char *table_text, route_nav_mode mode,
                                char *error, size_t error_size)
{
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.read_mem = fake_read_mem;
    hooks.on_event = on_route_event;
    hooks.probe.now_ms = route_clock;
    memset(&g_events, 0, sizeof g_events);
    xinput_route *route = xinput_route_create(script, marks, mark_count, waits, wait_count, &hooks, error, error_size);
    if (route == NULL) return NULL;
    for (size_t i = 0u; i < nav_count; i++) {
        if (!route_nav_line_parse(nav_lines[i], &g_steps[i], error, error_size)) { xinput_route_free(route); return NULL; }
    }
    g_step_count = nav_count;
    if (table_text != NULL && !route_nav_menus_parse(table_text, strlen(table_text), &g_table, error, error_size)) {
        xinput_route_free(route);
        return NULL;
    }
    const xinput_route_nav_config config = {g_steps, nav_count, table_text != NULL ? &g_table : NULL, mode, NULL, fake_read_bytes, NULL};
    if (!xinput_route_set_nav(route, &config, error, error_size)) {
        xinput_route_free(route);
        return NULL;
    }
    return route;
}

static const char *const ROUTE_TABLE =
    "menu list active=0x1000:4==1 ready=0x1004:4==1 cursor=*0x1008:4 count=*0x1008+4:4 axis=v wrap=0 names=*0x100C/16/ascii/15\n"
    "menu second active=0x1000:4==1 cursor=*0x1008:4 count=*0x1008+4:4 axis=v wrap=0\n";

/* Record: 4 rest polls, 12 polls of START (the recorded open loop presses, positions 4..15), 4 polls of X, total 20.
 * mark1 at 4 (= at), mark2 at 16 (= to). */
static const char NAV_SCRIPT[] = "4\n12 START\n4 X=255\n";
static const uint64_t NAV_MARKS[] = {4u, 16u};

/* One host poll: the route decides the pad, the fake game consumes it. */
static xinput_pad_state route_poll(xinput_route *route, uint64_t poll)
{
    xinput_pad_state pad;
    g_clock_poll = poll;
    set32(ADDR_READY, poll >= w.not_ready_polls ? 1u : 0u);
    CHECK(xinput_route_source(poll, &pad, route));
    apply_pad(&pad);
    return pad;
}

static void test_route_nav_jump(void)
{
    xinput_script *script = script_of(NAV_SCRIPT);
    reset_world(5u, 0u, true, false);
    w.a_closes = true;
    char error[300];
    const char *lines[] = {"at=4 to=16 menu=list select=index:2"};
    xinput_route *route = make_route(script, NAV_MARKS, 2u, NULL, 0u, lines, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    if (route == NULL) { printf("%s\n", error); xinput_script_free(script); return; }
    char trace[64];
    size_t used = 0u;
    uint64_t jump_poll = 0u;
    for (uint64_t poll = 0u; poll < 40u; poll++) {
        const xinput_pad_state pad = route_poll(route, poll);
        if (pad.analog[2] == 255u) trace[used++] = 'X';
        else trace[used++] = trace_char(&pad);
        if (jump_poll == 0u && xinput_route_cursor(route) > 16u) jump_poll = poll;
    }
    trace[used] = '\0';
    /* 4 rest from the record, the nav's presses (never the recorded START), then the record resumes at position 16 (X, 4 polls) */
    CHECK_STR(trace, "....DD...DD...XXXX......................");
    for (size_t i = 0u; i < used; i++) CHECK(trace[i] != 'S');
    CHECK(cursor_now() == 2u);
    CHECK(jump_poll == 14u); /* the poll that reached the cursor plays position 16 itself */
    CHECK(xinput_route_ended(route) && !xinput_route_failed(route) && xinput_route_cursor(route) == 20u);
    CHECK(count_events(XINPUT_ROUTE_NAV_START) == 1u && count_events(XINPUT_ROUTE_NAV_OK) == 1u && count_events(XINPUT_ROUTE_NAV_FAIL) == 0u &&
          count_events(XINPUT_ROUTE_NAV_FALLBACK) == 0u);
    CHECK(xinput_route_nav_done(route) == 1u && xinput_route_nav_polls_total(route) == 11u);
    /* marks: mark1 (at) fires before the nav starts, mark2 (to) after it finished, in that order */
    int mark1 = -1, mark2 = -1;
    for (int i = 0; i < g_events.count; i++) {
        if (g_events.events[i].kind == XINPUT_ROUTE_MARK && g_events.events[i].index == 1u) mark1 = i;
        if (g_events.events[i].kind == XINPUT_ROUTE_MARK && g_events.events[i].index == 2u) mark2 = i;
    }
    const int start = first_event(XINPUT_ROUTE_NAV_START), ok = first_event(XINPUT_ROUTE_NAV_OK);
    CHECK(mark1 >= 0 && mark2 >= 0 && start >= 0 && ok >= 0);
    CHECK(mark1 < start && start < ok && ok < mark2);
    CHECK(g_events.events[start].cursor == 4u && g_events.events[ok].cursor == 16u && g_events.events[ok].poll == 14u);
    CHECK_HAS(g_events.details[start], "step 1 (menu=list select=index:2) at record position 4 replaces the recorded presses up to 16");
    CHECK_HAS(g_events.details[ok], "step 1 done after 2 press(es), 0 lost");
    CHECK_HAS(g_events.details[ok], "(menu first ready after 1 polls)"); /* T1770 */
    CHECK(g_events.events[mark2].poll == 14u);
    xinput_route_free(route);

    /* the cursor is frozen at `at` during the nav: while the menu is not ready the record does not advance */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 6u;
    route = make_route(script, NAV_MARKS, 2u, NULL, 0u, lines, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 4u + 6u; poll++) {
        w.now = poll;
        (void)route_poll(route, poll);
        if (poll >= 4u) CHECK(xinput_route_cursor(route) == 4u);
    }
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_route_nav_modes(void)
{
    xinput_script *script = script_of(NAV_SCRIPT);
    char error[300];
    const char *lines[] = {"at=4 to=16 menu=list select=index:2"};
    /* off: the nav line is ignored, the recorded START presses replay as they are */
    reset_world(5u, 0u, true, false);
    xinput_route *route = make_route(script, NAV_MARKS, 2u, NULL, 0u, lines, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_OFF, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    unsigned starts = 0u;
    for (uint64_t poll = 0u; poll < 22u; poll++) starts += (route_poll(route, poll).digital_buttons & XINPUT_BUTTON_START) != 0u ? 1u : 0u;
    CHECK(starts == 12u && cursor_now() == 0u && count_events(XINPUT_ROUTE_NAV_START) == 0u && xinput_route_nav_done(route) == 0u);
    xinput_route_free(route);
    /* on, unknown menu: one fallback event, the recorded presses [4,16) replay open loop, then the route goes on */
    reset_world(5u, 0u, true, false);
    const char *unknown[] = {"at=4 to=16 menu=nosuch select=index:2"};
    route = make_route(script, NAV_MARKS, 2u, NULL, 0u, unknown, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    char trace[40];
    size_t used = 0u;
    for (uint64_t poll = 0u; poll < 22u; poll++) {
        const xinput_pad_state pad = route_poll(route, poll);
        trace[used++] = pad.analog[2] == 255u ? 'X' : trace_char(&pad);
    }
    trace[used] = '\0';
    CHECK_STR(trace, "....SSSSSSSSSSSSXXXX..");
    CHECK(count_events(XINPUT_ROUTE_NAV_FALLBACK) == 1u && count_events(XINPUT_ROUTE_NAV_OK) == 0u && !xinput_route_failed(route));
    CHECK_HAS(g_events.details[first_event(XINPUT_ROUTE_NAV_FALLBACK)], "route nav: step 1 falling back to the recorded presses (unknown menu 'nosuch'");
    CHECK(xinput_route_nav_done(route) == 1u && xinput_route_ended(route));
    xinput_route_free(route);
    /* strict with an unknown menu is refused when the nav is set up */
    reset_world(5u, 0u, true, false);
    route = make_route(script, NAV_MARKS, 2u, NULL, 0u, unknown, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_STRICT, error, sizeof error);
    CHECK(route == NULL);
    CHECK_HAS(error, "route nav step 1 names menu 'nosuch' which the menu table does not define (--route-nav strict)");
    /* on without any table: the step falls back */
    reset_world(5u, 0u, true, false);
    route = make_route(script, NAV_MARKS, 2u, NULL, 0u, lines, 1u, NULL, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 22u; poll++) (void)route_poll(route, poll);
    CHECK(count_events(XINPUT_ROUTE_NAV_FALLBACK) == 1u && cursor_now() == 0u);
    xinput_route_free(route);
    /* a failing step: the route FAILED, the event carries the whole line, the pad is at rest from then on and the record never resumes */
    reset_world(5u, 0u, true, false);
    w.not_ready_polls = 100000u;
    const char *slow[] = {"at=4 to=16 menu=list select=index:2 timeout=200"};
    route = make_route(script, NAV_MARKS, 2u, NULL, 0u, slow, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 60u; poll++) {
        const xinput_pad_state pad = route_poll(route, poll);
        CHECK(pad.digital_buttons == 0u && pad.analog[2] == 0u);
    }
    CHECK(xinput_route_failed(route) && count_events(XINPUT_ROUTE_NAV_FAIL) == 1u && count_events(XINPUT_ROUTE_NAV_OK) == 0u);
    const int fail = first_event(XINPUT_ROUTE_NAV_FAIL);
    CHECK(fail >= 0);
    if (fail >= 0) {
        CHECK_HAS(g_events.details[fail], "route FAILED: nav step 1 (menu=list select=index:2) timed out after");
        CHECK(g_events.events[fail].cursor == 4u);
    }
    CHECK(xinput_route_cursor(route) == 4u && !xinput_route_ended(route));
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_route_nav_waits_and_sequences(void)
{
    char error[300];
    /* a mark wait at `at` stalls first, the nav begins only after it holds */
    xinput_script *script = script_of(NAV_SCRIPT);
    xinput_route_wait wait;
    CHECK(xinput_route_wait_parse("mark1:min=3", &wait, error, sizeof error));
    reset_world(5u, 0u, true, false);
    const char *lines[] = {"at=4 to=16 menu=list select=index:1"};
    xinput_route *route = make_route(script, NAV_MARKS, 2u, &wait, 1u, lines, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    char trace[40];
    size_t used = 0u;
    for (uint64_t poll = 0u; poll < 20u; poll++) trace[used++] = trace_char(&(xinput_pad_state){.digital_buttons = route_poll(route, poll).digital_buttons});
    trace[used] = '\0';
    CHECK_STR(trace, ".......DD...........");
    xinput_route_free(route);
    xinput_script_free(script);

    /* two steps back to back (to == at of the next): the second starts in the poll the first finished, a mark between them fires once */
    script = script_of("2\n6 START\n6 X=255\n");
    const uint64_t marks[] = {8u};
    reset_world(5u, 0u, true, false);
    w.a_closes = false;
    const char *pair[] = {"at=2 to=8 menu=list select=index:1", "at=8 to=14 menu=list select=index:2"};
    route = make_route(script, marks, 1u, NULL, 0u, pair, 2u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 40u; poll++) (void)route_poll(route, poll);
    CHECK(cursor_now() == 2u && count_events(XINPUT_ROUTE_NAV_OK) == 2u && count_events(XINPUT_ROUTE_MARK) == 1u && xinput_route_ended(route));
    CHECK(xinput_route_nav_done(route) == 2u);
    xinput_route_free(route);
    xinput_script_free(script);

    /* a step with at == to replaces nothing but still drives the menu, then continues at the same position */
    script = script_of("3\n3 X=255\n");
    reset_world(5u, 0u, true, false);
    const char *empty[] = {"at=3 to=3 menu=list select=index:1"};
    route = make_route(script, NULL, 0u, NULL, 0u, empty, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 20u; poll++) (void)route_poll(route, poll);
    CHECK(cursor_now() == 1u && count_events(XINPUT_ROUTE_NAV_OK) == 1u && xinput_route_ended(route));
    xinput_route_free(route);
    /* a step beyond the record is refused */
    const char *beyond[] = {"at=3 to=99 menu=list select=index:1"};
    CHECK(make_route(script, NULL, 0u, NULL, 0u, beyond, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error) == NULL);
    CHECK_HAS(error, "not inside the record");
    xinput_script_free(script);

    /* the expect post-condition runs through the route's own wait machinery: a memory condition, baselines, min polls */
    script = script_of("2\n4 START\n4 X=255\n");
    reset_world(3u, 0u, true, false);
    w.a_closes = false;
    const char *expect[] = {"at=2 to=6 menu=list select=index:0 activate expect=mem=0x1010==1,min=4 timeout=5000"};
    route = make_route(script, NULL, 0u, NULL, 0u, expect, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    uint64_t finished = 0u;
    for (uint64_t poll = 0u; poll < 60u; poll++) {
        if (poll == 20u) set32(0x1010u, 1u);
        (void)route_poll(route, poll);
        if (finished == 0u && count_events(XINPUT_ROUTE_NAV_OK) == 1u) finished = poll;
    }
    CHECK(finished == 20u); /* the memory flips at poll 20, the expect holds on that poll, the route jumps */
    CHECK(!xinput_route_failed(route));
    xinput_route_free(route);
    reset_world(3u, 0u, true, false);
    w.a_closes = false;
    const char *never[] = {"at=2 to=6 menu=list select=index:0 activate expect=mem=0x1010==1 timeout=400"};
    route = make_route(script, NULL, 0u, NULL, 0u, never, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error);
    if (route == NULL) { CHECK(route != NULL); xinput_script_free(script); return; }
    for (uint64_t poll = 0u; poll < 100u; poll++) (void)route_poll(route, poll);
    CHECK(xinput_route_failed(route) && count_events(XINPUT_ROUTE_NAV_FAIL) == 1u);
    CHECK_HAS(g_events.details[first_event(XINPUT_ROUTE_NAV_FAIL)], "timed out after");
    CHECK_HAS(g_events.details[first_event(XINPUT_ROUTE_NAV_FAIL)], "mem=0x1010:4==0x1");
    xinput_route_free(route);
    /* an expect= condition this host cannot observe is refused up front, with the step named */
    const char *file[] = {"at=2 to=6 menu=list select=index:0 activate expect=file-open=x.mkr"};
    CHECK(make_route(script, NULL, 0u, NULL, 0u, file, 1u, ROUTE_TABLE, ROUTE_NAV_MODE_ON, error, sizeof error) == NULL);
    CHECK_HAS(error, "route nav expect=: file-open=x.mkr cannot be observed");
    xinput_script_free(script);
}

/* ======================================================================================================================
 * Host wiring: record lines, host_route.c, the flags
 * ==================================================================================================================== */
static bool wire_poll_hook(uint64_t poll, xinput_pad_state *pad)
{
    w.now = poll * MS_PER_POLL;
    g_clock_poll = poll;
    const bool ok = xinput_route_source(poll, pad, host_route_current());
    apply_pad(pad);
    return ok;
}

static void test_record_and_host_wiring(void)
{
    char path[] = "/tmp/tsfp-route-nav-XXXXXX";
    int descriptor = mkstemp(path);
    close(descriptor);
    char error[2400];
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    CHECK(xinput_record_enable_marks(path));
    xinput_pad_state rest = {0};
    for (uint64_t i = 0; i < 4; i++) xinput_record_observe(i, 0u, &rest);
    raise(SIGUSR2);
    for (uint64_t i = 4; i < 12; i++) xinput_record_observe(i, 0u, &rest);
    xinput_record_close();
    char pid_path[512];
    snprintf(pid_path, sizeof pid_path, "%s.pid", path);
    remove(pid_path);
    FILE *file = fopen(path, "r");
    char text[4096];
    size_t got = file != NULL ? fread(text, 1u, sizeof text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    text[got] = '\0';
    char *mark_line = strstr(text, "# mark: at=4\n");
    CHECK(mark_line != NULL);
    char edited[5000];
    snprintf(edited, sizeof edited,
             "%.*s# nav: at=4 to=4 menu=list select=name:Beta%%20Two activate expect=mem=0x1010==1 timeout=9000\n"
             "# nav: at=6 to=9 menu=second select=index:3\n%s",
             (int)(mark_line + 13 - text), text, mark_line + 13);
    file = fopen(path, "w");
    if (file != NULL) { fputs(edited, file); fclose(file); }
    uint64_t frames = 0u;
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && frames == 12u);
    CHECK(xinput_replay_nav_count() == 2u);
    CHECK(xinput_replay_nav(0u) != NULL && strcmp(xinput_replay_nav(0u), "at=4 to=4 menu=list select=name:Beta%20Two activate expect=mem=0x1010==1 timeout=9000") == 0);
    CHECK(xinput_replay_nav(1u) != NULL && strcmp(xinput_replay_nav(1u), "at=6 to=9 menu=second select=index:3") == 0);
    CHECK(xinput_replay_nav(2u) == NULL);
    /* an old record without nav lines has none */
    file = fopen(path, "w");
    if (file != NULL) { fputs(text, file); fclose(file); }
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && xinput_replay_nav_count() == 0u);
    /* 65 nav lines refuse the record at load, 64 load */
    static char many[40000];
    size_t used = (size_t)snprintf(many, sizeof many, "%.*s", (int)(mark_line + 13 - text), text);
    for (unsigned i = 0u; i < 65u; i++) {
        used += (size_t)snprintf(many + used, sizeof many - used, "# nav: at=%u to=%u menu=list select=index:1\n", i % 5u, i % 5u);
        if (i == 63u) {
            snprintf(many + used, sizeof many - used, "%s", mark_line + 13);
            file = fopen(path, "w");
            if (file != NULL) { fputs(many, file); fclose(file); }
            CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && xinput_replay_nav_count() == 64u);
        }
    }
    snprintf(many + used, sizeof many - used, "%s", mark_line + 13);
    file = fopen(path, "w");
    if (file != NULL) { fputs(many, file); fclose(file); }
    CHECK(!xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames));
    CHECK_HAS(error, "bad '# nav:' line");
    remove(path);

    /* host_route_setup_from: lines, table, mode and timing become the route's nav */
    xinput_script *script = script_of("4\n4 START\n4 X=255\n");
    const uint64_t marks[] = {4u};
    xinput_route_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.now_ms = route_clock;
    host_route_config config;
    memset(&config, 0, sizeof config);
    config.probe = &probe;
    config.read_mem = fake_read_mem;
    config.read_bytes = fake_read_bytes;
    const char *record_navs[] = {"at=4 to=8 menu=list select=name:beta activate"};
    config.record_navs = record_navs;
    config.record_nav_count = 1u;
    config.nav_menus_text = ROUTE_TABLE;
    config.nav_timing = "1:2";
    reset_world(5u, 0u, true, false);
    names_ascii();
    w.a_closes = true;
    CHECK(host_route_setup_from(&config, script, marks, 1u, error, sizeof error));
    if (host_route_current() == NULL) { xinput_script_free(script); return; } /* a refused setup is already a failure above */
    char trace[60];
    used = 0u;
    for (uint64_t poll = 0u; poll < 24u; poll++) {
        xinput_pad_state pad;
        CHECK(wire_poll_hook(poll, &pad));
        trace[used++] = pad.analog[2] == 255u ? 'X' : trace_char(&pad);
    }
    trace[used] = '\0';
    CHECK_STR(trace, "....D..A..XXXX..........");
    CHECK(cursor_now() == 1u);
    host_route_teardown();
    /* mode off through the config: the recorded presses play */
    config.nav_mode = "off";
    reset_world(5u, 0u, true, false);
    CHECK(host_route_setup_from(&config, script, marks, 1u, error, sizeof error));
    if (host_route_current() == NULL) { xinput_script_free(script); return; } /* a refused setup is already a failure above */
    unsigned starts = 0u;
    for (uint64_t poll = 0u; poll < 14u; poll++) {
        xinput_pad_state pad;
        CHECK(wire_poll_hook(poll, &pad));
        starts += (pad.digital_buttons & XINPUT_BUTTON_START) != 0u ? 1u : 0u;
    }
    CHECK(starts == 4u && cursor_now() == 0u);
    host_route_teardown();
    /* a table is given but mode is not: on by default; no table and no mode: off, with the recorded presses */
    config.nav_mode = NULL;
    config.nav_menus_text = NULL;
    reset_world(5u, 0u, true, false);
    CHECK(host_route_setup_from(&config, script, marks, 1u, error, sizeof error));
    if (host_route_current() == NULL) { xinput_script_free(script); return; } /* a refused setup is already a failure above */
    starts = 0u;
    for (uint64_t poll = 0u; poll < 14u; poll++) {
        xinput_pad_state pad;
        CHECK(wire_poll_hook(poll, &pad));
        starts += (pad.digital_buttons & XINPUT_BUTTON_START) != 0u ? 1u : 0u;
    }
    CHECK(starts == 4u && xinput_route_nav_done(host_route_current()) == 0u);
    host_route_teardown();
    /* refusals name what is wrong */
    config.nav_menus_text = ROUTE_TABLE;
    config.nav_mode = "sometimes";
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "--route-nav sometimes: want on, off or strict") != NULL);
    config.nav_mode = NULL;
    config.nav_timing = "0:3";
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "HOLD:GAP") != NULL);
    config.nav_timing = NULL;
    config.nav_menus_text = "menu broken active=nonsense\n";
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "--route-nav-menus (text): line 1:") != NULL);
    config.nav_menus_text = NULL;
    config.nav_menus_file = "/nonexistent/menu_nav.txt";
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "cannot read the menu table") != NULL);
    config.nav_menus_file = NULL;
    config.nav_menus_text = ROUTE_TABLE;
    const char *bad_line[] = {"at=4 to=8 menu=list select=index:1", "at=5 to=9 menu=list select=index:1"};
    config.record_navs = bad_line;
    config.record_nav_count = 2u;
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "record '# nav:' lines: nav step 2") != NULL);
    const char *bad_text[] = {"at=4 to=8 menu=list"};
    config.record_navs = bad_text;
    config.record_nav_count = 1u;
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "record '# nav:' line 1: missing select=") != NULL);
    const char *inside[] = {"at=2 to=6 menu=list select=index:1"};
    config.record_navs = inside;
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "mark 1 at 4 lies inside") != NULL);
    xinput_script_free(script);
}

static void test_options_and_identity(void)
{
    options parsed;
    const char *base[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav", "strict", "--route-nav-menus", "tools/data/menu_nav.txt",
                          "--route-nav-timing", "3:4", "game.xbe"};
    CHECK(parse_options(11, (char **)base, &parsed));
    CHECK(parsed.route_nav != NULL && strcmp(parsed.route_nav, "strict") == 0);
    CHECK(parsed.route_nav_menus != NULL && strcmp(parsed.route_nav_menus, "tools/data/menu_nav.txt") == 0);
    CHECK(parsed.route_nav_timing != NULL && strcmp(parsed.route_nav_timing, "3:4") == 0);
    const char *none[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "game.xbe"};
    CHECK(parse_options(5, (char **)none, &parsed) && parsed.route_nav == NULL && parsed.route_nav_menus == NULL && parsed.route_nav_timing == NULL);
    /* refused without a replay, with a bad value, or with a missing value */
    const char *no_replay[] = {"host", "--route-nav", "on", "game.xbe"};
    CHECK(!parse_options(4, (char **)no_replay, &parsed));
    const char *no_replay_menus[] = {"host", "--route-nav-menus", "t.txt", "game.xbe"};
    CHECK(!parse_options(4, (char **)no_replay_menus, &parsed));
    const char *no_replay_timing[] = {"host", "--route-nav-timing", "2:3", "game.xbe"};
    CHECK(!parse_options(4, (char **)no_replay_timing, &parsed));
    const char *bad_mode[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav", "maybe", "game.xbe"};
    CHECK(!parse_options(7, (char **)bad_mode, &parsed));
    const char *bad_timing[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav-timing", "0:0", "game.xbe"};
    CHECK(!parse_options(7, (char **)bad_timing, &parsed));
    const char *missing[] = {"host", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav-menus"};
    CHECK(!parse_options(5, (char **)missing, &parsed));
    /* the flags are not part of the flag identity: a record made without them replays with them */
    char with[300], without[300];
    const char *recorded[] = {"host", "game.xbe", "--skip-intro", "--synthetic-pad", "--record-input", "r.txt"};
    const char *replay[] = {"host", "game.xbe", "--skip-intro", "--synthetic-pad", "--replay-input", "r.txt", "--route-nav", "on", "--route-nav-menus",
                            "m.txt", "--route-nav-timing", "2:3"};
    CHECK(xinput_record_identity_flags(6, (char **)recorded, without, sizeof without));
    CHECK(xinput_record_identity_flags(12, (char **)replay, with, sizeof with));
    CHECK(without[0] != '\0' && strcmp(with, without) == 0);
    CHECK(strstr(with, "route-nav") == NULL);
    /* the help text carries the marker and every flag */
    const char *help = host_options_route_nav_help();
    CHECK(help != NULL && strstr(help, "T1640") != NULL && strstr(help, "--route-nav ") != NULL && strstr(help, "--route-nav-menus") != NULL &&
          strstr(help, "--route-nav-timing") != NULL && strstr(help, "# nav:") != NULL);
}

int main(void)
{
    test_nav_line_parser();
    test_steps_check();
    test_menu_table_parser();
    test_timing_and_mode();
    test_machine_exact_pad_output();
    test_machine_already_on_target();
    test_machine_directions();
    test_machine_ready_gating();
    test_machine_timeout();
    test_machine_lost_press();
    test_machine_odd_menus();
    test_machine_targets();
    test_machine_fallback_and_strict();
    test_machine_postcondition();
    test_page_world_selection();
    test_page_world_readiness();
    test_page_world_activation();
    test_skipped_names_and_dpad();
    test_real_menu_table();
    test_record_nav_lines();
    test_record_nav_suppression_and_edges();
    test_record_selectby();
    test_record_nav_limits_cost_and_host();
    test_real_table_recorder();
    test_route_nav_jump();
    test_route_nav_modes();
    test_route_nav_waits_and_sequences();
    test_record_and_host_wiring();
    test_options_and_identity();
    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
