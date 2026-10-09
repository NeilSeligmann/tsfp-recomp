/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1633: the event driven route. The event grammar, the route player's event waits (file open/read/idle, call, frame, memory
 * with ops and pointer indirection, min/timeout, `any`, per segment baselines, progress and failure text), the host observers
 * (route_probe.c: watches, facts, event log, sampled memory), the record's `# wait:` and `# mark-info:` lines and the merge of
 * the record's waits with the command line in host_route.c. Synthetic scripts and a fake clock, nothing of the title.
 */
#include "host_options.h"
#include "host_route.h"
#include "kernel_file.h"
#include "recomp_abi.h"
#include "route_probe.h"
#include "xinput_record.h"
#include "xinput_route.h"
#include "xinput_route_spec.h"

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

/* ---- fake world ---- */
static uint64_t g_now = 1000u;
static uint64_t fake_clock(void *user) { (void)user; return g_now; }

typedef struct {
    int count;
    xinput_route_event events[40];
    char details[40][700];
    uint32_t memory[8]; /* address 0x1000 + 4*i */
    uint32_t pointer;   /* dword at 0x2000 */
    uint32_t pointee[4]; /* at pointer + 8 */
} world;
static world w;

static void on_event(const xinput_route_event *event, void *user)
{
    (void)user;
    if (w.count >= 40) return;
    w.events[w.count] = *event;
    snprintf(w.details[w.count], sizeof w.details[0], "%s", event->detail != NULL ? event->detail : "");
    w.events[w.count].detail = w.details[w.count];
    w.count++;
}
static bool read_mem(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    (void)width;
    if (address >= 0x1000u && address < 0x1020u && address % 4u == 0u) { *value = w.memory[(address - 0x1000u) / 4u]; return true; }
    if (address == 0x2000u) { *value = w.pointer; return true; }
    if (address == 8u) { *value = 6u; return true; } /* what a null pointer + 8 would read if it were followed */
    if (w.pointer != 0u && address >= w.pointer + 8u && address < w.pointer + 8u + 16u) {
        *value = w.pointee[(address - w.pointer - 8u) / 4u];
        return true;
    }
    return false;
}

/* Script: polls 0..2 rest, 3..4 START, 5..6 A=255. mark1 at 3, mark2 at 5, total 7. */
static const char SCRIPT[] = "3\n2 START\n2 A=255\n";
static const uint64_t MARKS[] = {3u, 5u};

static xinput_script *script_of(const char *text)
{
    char error[200];
    xinput_script *script = xinput_script_parse(text, strlen(text), error, sizeof error);
    if (script == NULL) printf("script error: %s\n", error);
    return script;
}

static xinput_route *make(const xinput_script *script, const char *const *specs, size_t count, char *error, size_t error_size)
{
    xinput_route_wait waits[4];
    for (size_t i = 0; i < count; i++)
        if (!xinput_route_event_wait_parse(specs[i], &waits[i], error, error_size)) return NULL;
    memset(&w, 0, sizeof w);
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.read_mem = read_mem;
    hooks.on_event = on_event;
    hooks.probe = *route_probe_view();
    return xinput_route_create(script, MARKS, 2u, waits, count, &hooks, error, error_size);
}
static void fresh(void)
{
    route_probe_reset();
    route_probe_set_clock(fake_clock, NULL);
    g_now = 1000u;
    route_probe_enable();
}
static xinput_pad_state poll(xinput_route *route, uint64_t index)
{
    xinput_pad_state state;
    CHECK(xinput_route_source(index, &state, route));
    return state;
}
static bool parses(const char *spec)
{
    xinput_route_wait wait;
    char error[200];
    return xinput_route_event_wait_parse(spec, &wait, error, sizeof error);
}
static const char *refusal(const char *spec)
{
    static char error[300];
    xinput_route_wait wait;
    error[0] = '\0';
    return xinput_route_event_wait_parse(spec, &wait, error, sizeof error) ? "" : error;
}

/* ---- grammar ---- */
static void test_grammar(void)
{
    xinput_route_wait wait;
    char error[300];
    CHECK(xinput_route_event_wait_parse("mark2:file-open=AniceMap.MKR,file-idle=800@Pak,timeout=120000", &wait, error, sizeof error));
    CHECK(wait.mark == 2u && wait.event && wait.cond_count == 2u && wait.timeout_ms == 120000u && wait.max_polls == 0u);
    CHECK(wait.conds[0].kind == RCOND_FILE_OPEN && strcmp(wait.conds[0].text, "anicemap.mkr") == 0); /* lower cased */
    CHECK(wait.conds[1].kind == RCOND_FILE_IDLE && wait.conds[1].amount == 800u && strcmp(wait.conds[1].text, "pak") == 0);
    /* defaults: timeout 600000 ms when neither timeout nor max is given, none when only max is */
    CHECK(xinput_route_event_wait_parse("mark1:file-open=a", &wait, error, sizeof error) && wait.timeout_ms == XINPUT_ROUTE_DEFAULT_TIMEOUT_MS &&
          wait.max_polls == 0u);
    CHECK(xinput_route_event_wait_parse("mark1:file-open=a,max=50", &wait, error, sizeof error) && wait.timeout_ms == 0u && wait.max_polls == 50u);
    /* every field kind */
    CHECK(parses("mark1:file-read=map@4096") && parses("mark1:file-read=map") && parses("mark1:file-idle=500"));
    CHECK(xinput_route_event_wait_parse("mark1:call=0x302A60@3", &wait, error, sizeof error) && wait.conds[0].kind == RCOND_CALL &&
          wait.conds[0].va == 0x302A60u && wait.conds[0].amount == 3u);
    CHECK(xinput_route_event_wait_parse("mark1:call=0x302A60", &wait, error, sizeof error) && wait.conds[0].amount == 1u);
    CHECK(parses("mark1:frame-change,frame-stable=300") && parses("mark3:min=10") && parses("mark3:min-ms=500,timeout=1000"));
    /* memory conditions: ops, width, mask, indirection, offset */
    CHECK(xinput_route_event_wait_parse("mark1:mem=0x79094C==0x66", &wait, error, sizeof error) && wait.conds[0].kind == RCOND_MEM &&
          wait.conds[0].cmp == RCMP_EQ && !wait.conds[0].indirect && wait.conds[0].width == 4u && wait.conds[0].value == 0x66u);
    CHECK(xinput_route_event_wait_parse("mark1:mem=*0x7844A8+0x11908:4>=6", &wait, error, sizeof error) && wait.conds[0].indirect &&
          wait.conds[0].address == 0x7844A8u && wait.conds[0].offset == 0x11908u && wait.conds[0].cmp == RCMP_GE);
    CHECK(xinput_route_event_wait_parse("mark1:mem=*0x784014==0xC77667", &wait, error, sizeof error) && wait.conds[0].indirect &&
          wait.conds[0].offset == 0u);
    CHECK(xinput_route_event_wait_parse("mark1:mem=0x1000:1!=0", &wait, error, sizeof error) && wait.conds[0].cmp == RCMP_NE && wait.conds[0].width == 1u);
    CHECK(xinput_route_event_wait_parse("mark1:mem=0x1000:2<=7&0xFF", &wait, error, sizeof error) && wait.conds[0].cmp == RCMP_LE && wait.conds[0].mask == 0xFFu);
    CHECK(xinput_route_event_wait_parse("mark1:mem=0x1000<5", &wait, error, sizeof error) && wait.conds[0].cmp == RCMP_LT);
    CHECK(xinput_route_event_wait_parse("mark1:mem=0x1000>5", &wait, error, sizeof error) && wait.conds[0].cmp == RCMP_GT);
    /* the legacy fields still parse in the event grammar, and the legacy parser refuses the new ones */
    CHECK(parses("mark3:mem=0x52D3FC==6,max=36000"));
    xinput_route_wait legacy;
    CHECK(xinput_route_wait_parse("mark3:mem=0x52D3FC==6,max=36000", &legacy, error, sizeof error) && !legacy.event && legacy.max_polls == 36000u);
    CHECK(!xinput_route_wait_parse("mark2:file-open=x", &legacy, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark2:timeout=100,min=3", &legacy, error, sizeof error));
    CHECK(xinput_route_wait_parse("mark2:min=120", &legacy, error, sizeof error) && legacy.max_polls == XINPUT_ROUTE_DEFAULT_MAX_POLLS);
    /* refusals, each with a message naming the spec */
    CHECK(!parses("file-open=a") && !parses("mark0:file-open=a") && !parses("mark17:file-open=a") && !parses("mark1"));
    CHECK(!parses("mark1:") && !parses("mark1:timeout=100") && !parses("mark1:max=5"));            /* nothing to wait for */
    CHECK(!parses("mark1:file-open=") && !parses("mark1:file-idle=0") && !parses("mark1:file-idle=abc"));
    CHECK(!parses("mark1:file-read=a@0") && !parses("mark1:call=0") && !parses("mark1:call=0x4000000") && !parses("mark1:call=0x10@0"));
    CHECK(!parses("mark1:frame-stable=0") && !parses("mark1:frame-change=1") && !parses("mark1:bogus=1"));
    CHECK(!parses("mark1:file-open=a,file-open=b,file-open=c,file-open=d,file-open=e,file-open=f,file-open=g")); /* > ROUTE_COND_MAX */
    CHECK(!parses("mark1:min=3,min=4") && !parses("mark1:file-open=a,timeout=1,timeout=2") && !parses("mark1:any,any,file-open=a,file-open=b"));
    CHECK(!parses("mark1:file-open=a,any")); /* any needs two conditions */
    CHECK(parses("mark1:file-open=a,file-open=b,any"));
    CHECK(!parses("mark1:file-open=a,min-ms=900,timeout=100") && !parses("mark1:file-open=a,min=9,max=3"));
    CHECK(!parses("mark1:mem=0x1001==1") && !parses("mark1:mem=0x1000:3==1") && !parses("mark1:mem=0x4000000==1")); /* unaligned, bad width, space */
    CHECK(!parses("mark1:mem=*0x1002==1")); /* the pointer dword must be aligned */
    CHECK(!parses("mark1:mem=0x1000:1==0x100&0xFF")); /* a value the mask hides can never match */
    CHECK(!parses("mark1:mem=0x1000") && !parses("mark1:mem=0x1000=5") && !parses("mark1:mem=*==1"));
    CHECK(strstr(refusal("mark1:bogus=1"), "bogus") != NULL);
    CHECK(strstr(refusal("mark1:timeout=5"), "never waits") != NULL);
    /* cond text survives a round trip for the log lines */
    char text[100];
    xinput_route_event_wait_parse("mark1:file-idle=800@pak,call=0x302A60@2,frame-stable=300,mem=*0x784014+0x4:2>=6", &wait, error, sizeof error);
    xinput_route_cond_format(&wait.conds[0], text, sizeof text);
    CHECK(strcmp(text, "file-idle=800@pak") == 0);
    xinput_route_cond_format(&wait.conds[1], text, sizeof text);
    CHECK(strcmp(text, "call=0x302A60@2") == 0);
    xinput_route_cond_format(&wait.conds[2], text, sizeof text);
    CHECK(strcmp(text, "frame-stable=300") == 0);
    xinput_route_cond_format(&wait.conds[3], text, sizeof text);
    CHECK(strstr(text, "*0x784014+0x4") != NULL && strstr(text, ">=0x6") != NULL);
}

/* ---- the route player with event waits ---- */
static void test_file_open_and_segments(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    const char *specs[] = {"mark1:file-open=anicemap.mkr,timeout=60000", "mark2:file-open=tsftrpft,file-read=tsftrpft@100,timeout=60000"};
    xinput_route *route = make(script, specs, 2u, error, sizeof error);
    CHECK(route != NULL);
    /* an open BEFORE the first pad poll never counts (the baseline is taken at the first poll) */
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\U:\\3E67F296FD7B\\anicemap.mkr", 0u);
    for (uint64_t i = 0; i < 3; i++) CHECK(poll(route, i).digital_buttons == 0u);
    for (uint64_t i = 3; i < 13; i++) {
        g_now += 100u;
        CHECK(poll(route, i).digital_buttons == 0u && xinput_route_cursor(route) == 3u);
    }
    CHECK(xinput_route_stalled_total(route) == 10u);
    /* a different file, and a READ of the right file, are not an open */
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\d:\\pak\\arcade\\l_103.pak", 0u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\x\\ANICEMAP.MKR", 32756u);
    CHECK(poll(route, 13u).digital_buttons == 0u && xinput_route_cursor(route) == 3u);
    /* segment 1 activity on mark 2's files: the baseline taken when mark 1 fires must hide it from mark 2 */
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "u:\\early\\tsftrpft", 0u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "u:\\early\\tsftrpft", 500u);
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\U:\\3E67F296FD7B\\ANICEMAP.mkr", 0u); /* case blind */
    CHECK(poll(route, 14u).digital_buttons == XINPUT_BUTTON_START && xinput_route_cursor(route) == 4u);
    CHECK(w.events[0].kind == XINPUT_ROUTE_WAIT_OK && w.events[0].index == 1u && strstr(w.details[0], "OK   file-open=anicemap.mkr (1 open(s))") != NULL);
    CHECK(w.events[0].stalled == 11u && w.events[0].stalled_ms == 900u);
    /* segment 2 starts at mark 1: opening tsftrpft BEFORE that does not satisfy mark 2 */
    CHECK(w.events[1].kind == XINPUT_ROUTE_MARK && w.events[1].index == 1u);
    CHECK(poll(route, 15u).digital_buttons == XINPUT_BUTTON_START);
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "u:\\16d15bd8d404\\tsftrpft", 0u);
    CHECK(poll(route, 16u).digital_buttons == 0u && xinput_route_cursor(route) == 5u); /* waiting: opened but read < 100 bytes */
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "u:\\16d15bd8d404\\tsftrpft", 60u);
    CHECK(poll(route, 17u).digital_buttons == 0u && xinput_route_cursor(route) == 5u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "u:\\16d15bd8d404\\tsftrpft", 60u);
    CHECK(poll(route, 18u).analog[0] == 255u); /* 120 bytes read: the wait held and the stream resumed */
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_idle_call_frame_min(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    const char *specs[] = {"mark1:file-idle=500,timeout=60000", "mark2:call=0x302A60@2,frame-stable=300,min-ms=200,timeout=60000"};
    xinput_route *route = make(script, specs, 2u, error, sizeof error);
    CHECK(route != NULL);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "before.bin", 10u); /* before the first pad poll: outside the segment */
    g_now += 20000u;
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    /* idle needs I/O first: 10 s of silence is not idle, and the I/O from before the segment does not count */
    g_now += 10000u;
    CHECK(poll(route, 3u).digital_buttons == 0u && xinput_route_cursor(route) == 3u);
    CHECK(w.count == 0);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "a.bin", 10u);
    g_now += 499u;
    poll(route, 4u);
    CHECK(xinput_route_cursor(route) == 3u); /* 499 ms quiet: not yet */
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "b.bin", 10u); /* activity restarts the quiet period */
    g_now += 499u;
    poll(route, 5u);
    CHECK(xinput_route_cursor(route) == 3u);
    g_now += 1u;
    CHECK(poll(route, 6u).digital_buttons == XINPUT_BUTTON_START && xinput_route_cursor(route) == 4u);
    /* mark 2: the call twice, the frame stable for 300 ms, min-ms 200 */
    poll(route, 7u);
    CHECK(xinput_route_cursor(route) == 5u);
    xinput_pad_state state = poll(route, 8u);
    CHECK(state.digital_buttons == 0u && xinput_route_cursor(route) == 5u);
    route_probe_note_call(0x302A60u);
    route_probe_note_call(0x111111u); /* not watched */
    route_probe_note_frame(1u);
    g_now += 1000u;
    poll(route, 9u);
    CHECK(xinput_route_cursor(route) == 5u); /* one call of two */
    route_probe_note_call(0x302A60u);
    route_probe_note_frame(2u);          /* the frame changed just now: not stable */
    poll(route, 10u);
    CHECK(xinput_route_cursor(route) == 5u);
    g_now += 299u;
    poll(route, 11u);
    CHECK(xinput_route_cursor(route) == 5u);
    g_now += 1u;
    CHECK(poll(route, 12u).analog[0] == 255u && xinput_route_cursor(route) == 6u);
    CHECK(strstr(w.details[w.count - 2], "call=0x302A60@2 (2 call(s))") != NULL);
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_frame_stable_needs_a_change(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    const char *specs[] = {"mark1:frame-stable=100,timeout=60000"};
    xinput_route *route = make(script, specs, 1u, error, sizeof error);
    CHECK(route != NULL);
    route_probe_note_frame(3u); /* a change before the first pad poll: outside the segment */
    g_now += 20000u;
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    g_now += 10000u; /* a frame that never changed is not a screen that settled */
    poll(route, 3u);
    CHECK(xinput_route_cursor(route) == 3u);
    route_probe_note_frame(5u);
    g_now += 99u;
    poll(route, 4u);
    CHECK(xinput_route_cursor(route) == 3u);
    g_now += 1u;
    poll(route, 5u);
    CHECK(xinput_route_cursor(route) == 4u);
    xinput_route_free(route);
    /* frame-change alone holds at the change */
    fresh();
    const char *change[] = {"mark1:frame-change,timeout=60000"};
    route = make(script, change, 1u, error, sizeof error);
    for (uint64_t i = 0; i < 4; i++) poll(route, i);
    CHECK(xinput_route_cursor(route) == 3u);
    route_probe_note_frame(1u);
    poll(route, 4u);
    CHECK(xinput_route_cursor(route) == 4u);
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_mem_ops_indirect_any(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    const char *specs[] = {"mark1:mem=*0x2000+8:4>=6,mem=0x1000:4!=0,timeout=60000", "mark2:mem=0x1004==1,mem=0x1008==2,any,timeout=60000"};
    xinput_route *route = make(script, specs, 2u, error, sizeof error);
    CHECK(route != NULL);
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    poll(route, 3u);
    CHECK(xinput_route_cursor(route) == 3u);
    w.memory[0] = 5u;
    poll(route, 4u);
    CHECK(xinput_route_cursor(route) == 3u); /* the pointer is still null: unreadable, not zero */
    w.pointer = 0x3000u;
    w.pointee[0] = 5u;
    poll(route, 5u);
    CHECK(xinput_route_cursor(route) == 3u); /* 5 >= 6 false */
    w.pointee[0] = 6u;
    CHECK(poll(route, 6u).digital_buttons == XINPUT_BUTTON_START);
    poll(route, 7u);
    poll(route, 8u);
    CHECK(xinput_route_cursor(route) == 5u); /* any: neither holds */
    w.memory[2] = 2u;                        /* the second alternative */
    CHECK(poll(route, 9u).analog[0] == 255u);
    xinput_route_free(route);
    /* a mask and a narrow width compare only the masked bits */
    fresh();
    const char *masked[] = {"mark1:mem=0x1000:4==2&0xFF,timeout=60000"};
    route = make(script, masked, 1u, error, sizeof error);
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    w.memory[0] = 0x00000102u;
    poll(route, 3u);
    CHECK(xinput_route_cursor(route) == 4u); /* 0x102 & 0xFF == 2 */
    xinput_route_free(route);
    fresh();
    const char *narrow[] = {"mark1:mem=0x1000:2==0x0102,timeout=60000"};
    route = make(script, narrow, 1u, error, sizeof error);
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    w.memory[0] = 0x00AB0102u; /* the fake memory returns the whole dword; the route masks to the 2 byte width */
    poll(route, 3u);
    CHECK(xinput_route_cursor(route) == 4u);
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_failure_progress_timeout(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    const char *specs[] = {"mark1:file-open=never.mkr,mem=0x1000==9,timeout=12000"};
    xinput_route *route = make(script, specs, 1u, error, sizeof error);
    CHECK(route != NULL);
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    for (uint64_t i = 3; i < 8; i++) { g_now += 1000u; poll(route, i); }
    CHECK(!xinput_route_failed(route));
    int progress = 0;
    for (int i = 0; i < w.count; i++) progress += w.events[i].kind == XINPUT_ROUTE_WAIT_PROGRESS;
    CHECK(progress == 0); /* 5 s of waiting: the first progress line is due at 5000 ms */
    g_now += 1000u;
    poll(route, 8u);
    progress = 0;
    int progress_index = -1;
    for (int i = 0; i < w.count; i++) if (w.events[i].kind == XINPUT_ROUTE_WAIT_PROGRESS) { progress++; progress_index = i; }
    CHECK(progress == 1 && strstr(w.details[progress_index], "WAIT file-open=never.mkr (0 open(s))") != NULL);
    CHECK(strstr(w.details[progress_index], "WAIT mem=0x1000:4==0x9 (read 0x0)") != NULL);
    for (uint64_t i = 9; i < 11; i++) { g_now += 1000u; poll(route, i); }
    CHECK(!xinput_route_failed(route));
    g_now += 5000u; /* 12 s past the stall start */
    poll(route, 11u);
    CHECK(xinput_route_failed(route));
    CHECK(w.events[w.count - 1].kind == XINPUT_ROUTE_WAIT_TIMEOUT && w.events[w.count - 1].index == 1u);
    CHECK(strstr(w.details[w.count - 1], "never.mkr") != NULL && w.events[w.count - 1].stalled_ms >= 12000u);
    const int events_after_failure = w.count;
    CHECK(poll(route, 12u).digital_buttons == 0u && w.count == events_after_failure); /* nothing more is replayed */
    xinput_route_free(route);

    /* max polls still works in an event wait */
    fresh();
    const char *polls[] = {"mark1:file-open=never.mkr,max=4"};
    route = make(script, polls, 1u, error, sizeof error);
    for (uint64_t i = 0; i < 12; i++) poll(route, i);
    CHECK(xinput_route_failed(route) && w.events[w.count - 1].kind == XINPUT_ROUTE_WAIT_TIMEOUT && w.events[w.count - 1].stalled == 4u);
    xinput_route_free(route);

    /* min-ms gates a condition that already holds */
    fresh();
    const char *gated[] = {"mark1:file-open=x,min-ms=300,timeout=9000"};
    route = make(script, gated, 1u, error, sizeof error);
    for (uint64_t i = 0; i < 3; i++) poll(route, i);
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "x", 0u);
    poll(route, 3u);
    CHECK(xinput_route_cursor(route) == 3u);
    g_now += 299u;
    poll(route, 4u);
    CHECK(xinput_route_cursor(route) == 3u);
    g_now += 1u;
    poll(route, 5u);
    CHECK(xinput_route_cursor(route) == 4u);
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_creation_refusals(void)
{
    fresh();
    xinput_script *script = script_of(SCRIPT);
    char error[300];
    xinput_route_wait wait;
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.read_mem = read_mem;
    /* a host without the observers refuses the route up front, naming the condition */
    xinput_route_event_wait_parse("mark1:file-open=a.mkr", &wait, error, sizeof error);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "file-open=a.mkr") != NULL &&
          strstr(error, "cannot be observed") != NULL);
    xinput_route_event_wait_parse("mark1:call=0x10", &wait, error, sizeof error);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "guest call") != NULL);
    xinput_route_event_wait_parse("mark1:frame-change", &wait, error, sizeof error);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "frame") != NULL);
    hooks.read_mem = NULL;
    xinput_route_event_wait_parse("mark1:mem=0x1000==1", &wait, error, sizeof error);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "memory reader") != NULL);
    /* a wait on a mark the record lacks */
    hooks.read_mem = read_mem;
    hooks.probe = *route_probe_view();
    xinput_route_event_wait_parse("mark3:file-open=a", &wait, error, sizeof error);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "mark3") != NULL);
    /* more watches than the probe has room for is refused, not silently dropped */
    route_probe_reset();
    for (unsigned i = 0; i < ROUTE_PROBE_MAX_WATCHES; i++) {
        char name[16];
        snprintf(name, sizeof name, "f%u", i);
        CHECK(route_probe_view()->watch(RCOND_FILE_OPEN, name, 0u, NULL) == (int)i);
    }
    CHECK(route_probe_view()->watch(RCOND_FILE_OPEN, "one-too-many", 0u, NULL) < 0);
    CHECK(route_probe_view()->watch(RCOND_FILE_OPEN, "f3", 0u, NULL) == 3); /* same condition: same watch */
    xinput_script_free(script);
}

/* ---- the host observers ---- */
static void test_probe(void)
{
    fresh();
    const xinput_route_probe *view = route_probe_view();
    const int open_id = view->watch(RCOND_FILE_OPEN, "map", 0u, NULL);
    const int read_id = view->watch(RCOND_FILE_READ, "map", 0u, NULL);
    const int idle_all = view->watch(RCOND_FILE_IDLE, "", 0u, NULL);
    const int idle_pak = view->watch(RCOND_FILE_IDLE, "pak", 0u, NULL);
    const int call_id = view->watch(RCOND_CALL, "", 0x302A60u, NULL);
    CHECK(open_id >= 0 && read_id >= 0 && idle_all >= 0 && idle_pak >= 0 && call_id >= 0);
    CHECK(open_id != read_id && read_id != idle_all);                                    /* kinds are separate watches */
    CHECK(view->watch(RCOND_FILE_OPEN, "map", 0u, NULL) == open_id);
    CHECK(view->watch(RCOND_FRAME_CHANGE, "", 0u, NULL) < 0 && view->watch(RCOND_MEM, "", 0u, NULL) < 0);
    CHECK(route_probe_calls_watched());
    xinput_route_watch_state state;
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\U:\\ABC\\AniceMap.mkr", 0u);
    g_now += 7u;
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\ABC\\anicemap.mkr", 4096u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\d:\\pak\\l.pak", 100u);
    route_probe_note_file(KERNEL_FILE_EVENT_WRITE, "\\??\\U:\\ABC\\anicemap.mkr", 50u);
    CHECK(view->watch_state(open_id, &state, NULL) && state.count == 1u && state.bytes == 0u);
    CHECK(view->watch_state(read_id, &state, NULL) && state.count == 1u && state.bytes == 4096u); /* writes are not reads */
    CHECK(view->watch_state(idle_all, &state, NULL) && state.count == 4u && state.last_ms == g_now);
    CHECK(view->watch_state(idle_pak, &state, NULL) && state.count == 1u);
    CHECK(!view->watch_state(99, &state, NULL) && !view->watch_state(-1, &state, NULL));
    /* "!SUBSTR": every file except those containing it (the disc streams all the time, the saves do not) */
    const int not_pak = view->watch(RCOND_FILE_IDLE, "!pak", 0u, NULL);
    const int hdd_only = view->watch(RCOND_FILE_IDLE, "u:\\", 0u, NULL);
    CHECK(not_pak >= 0 && hdd_only >= 0 && not_pak != idle_pak);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\d:\\pak\\music.pak", 10u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\ABC\\save.bin", 10u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\ABC\\other.bin", 10u);
    CHECK(view->watch_state(not_pak, &state, NULL) && state.count == 2u); /* the two files WITHOUT pak, not the one with it */
    CHECK(view->watch_state(hdd_only, &state, NULL) && state.count == 2u);
    CHECK(view->watch_state(idle_pak, &state, NULL) && state.count == 2u); /* l.pak and music.pak */
    route_probe_note_call(0x302A60u);
    route_probe_note_call(0x302A60u);
    route_probe_note_call(0x302A64u);
    CHECK(view->watch_state(call_id, &state, NULL) && state.count == 2u && state.last_ms == g_now);
    xinput_route_frame_state frame;
    route_probe_note_frame(7u);
    route_probe_note_frame(7u);
    g_now += 40u;
    route_probe_note_frame(8u);
    CHECK(view->frame_state(&frame, NULL) && frame.changes == 2u && frame.last_change_ms == g_now);
    /* disabled: nothing is counted */
    route_probe_reset();
    route_probe_set_clock(fake_clock, NULL);
    const int id = view->watch(RCOND_FILE_OPEN, "map", 0u, NULL);
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "map", 0u);
    CHECK(view->watch_state(id, &state, NULL) && state.count == 0u);
    CHECK(!route_probe_mark_facts(1u, (char[8]){0}, 8u, NULL));
}

static void test_mark_facts_and_log(void)
{
    fresh();
    char path[] = "/tmp/tsfp-route-probe-XXXXXX";
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    close(descriptor);
    char error[200];
    CHECK(route_probe_log_open(path, error, sizeof error) && route_probe_log_active());
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\U:\\16D15BD8D404\\SaveMeta.xbx", 0u);
    g_now += 100u;
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "\\??\\U:\\16D15BD8D404\\tsftrpft", 0u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\16D15BD8D404\\tsftrpft", 8040u);
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\U:\\16D15BD8D404\\tsftrpft", 100u); /* inside the 250 ms window: not logged */
    route_probe_note_frame(1u);
    route_probe_note_frame(2u);
    g_now += 300u;
    route_probe_note_file(KERNEL_FILE_EVENT_READ, "\\??\\d:\\pak\\music.pak", 5u); /* the disc: all I/O, not the save volume's */
    g_now += 600u;
    char facts[400];
    CHECK(route_probe_mark_facts(1u, facts, sizeof facts, NULL));
    CHECK(strstr(facts, "seg-ms=1000") != NULL && strstr(facts, "io=5") != NULL && strstr(facts, "opens=2") != NULL &&
          strstr(facts, "read-bytes=8145") != NULL && strstr(facts, "first-io-ms=0") != NULL && strstr(facts, "last-io-ago-ms=600 ") != NULL &&
          strstr(facts, "hdd-io=4 hdd-last-io-ago-ms=900 ") != NULL &&
          strstr(facts, "frame-changes=2") != NULL && strstr(facts, "last-open=SaveMeta.xbx,tsftrpft") != NULL);
    /* the next segment starts empty */
    g_now += 50u;
    CHECK(route_probe_mark_facts(2u, facts, sizeof facts, NULL) && strstr(facts, "seg-ms=50") != NULL && strstr(facts, "io=0") != NULL && strstr(facts, "hdd-io=0 hdd-last-io-ago-ms=0 ") != NULL &&
          strstr(facts, "last-open=") == NULL && strstr(facts, "frame-changes=0") != NULL);
    route_probe_log_close();
    FILE *file = fopen(path, "r");
    char text[4000];
    size_t got = file != NULL ? fread(text, 1u, sizeof text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    text[got] = '\0';
    CHECK(strstr(text, "# tsfp route event log v1") != NULL);
    CHECK(strstr(text, "t=0 poll=0 open \\??\\U:\\16D15BD8D404\\SaveMeta.xbx") != NULL);
    CHECK(strstr(text, "t=100 poll=0 open \\??\\U:\\16D15BD8D404\\tsftrpft") != NULL);
    CHECK(strstr(text, "read \\??\\U:\\16D15BD8D404\\tsftrpft reads=1 bytes=8040") != NULL && strstr(text, "reads=2") == NULL);
    CHECK(strstr(text, "mark 1 seg-ms=1000 io=5 opens=2 read-bytes=8145") != NULL);
    remove(path);
}

static bool sample_read(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    (void)width;
    return read_mem(address, 4u, value, NULL);
}
static void test_mem_sampling(void)
{
    fresh();
    char path[] = "/tmp/tsfp-route-mem-XXXXXX";
    int descriptor = mkstemp(path);
    close(descriptor);
    char error[200];
    CHECK(route_probe_log_open(path, error, sizeof error));
    CHECK(route_probe_mem_add("0x1000", error, sizeof error) && route_probe_mem_add("*0x2000+8:4", error, sizeof error));
    CHECK(!route_probe_mem_add("0x1001", error, sizeof error) && !route_probe_mem_add("zzz", error, sizeof error));
    memset(&w, 0, sizeof w);
    w.memory[0] = 0x65u;
    route_probe_mem_sample(sample_read, NULL);
    route_probe_mem_sample(sample_read, NULL); /* no change: no line */
    w.memory[0] = 0x66u;
    w.pointer = 0x3000u;
    w.pointee[0] = 7u;
    g_now += 30u;
    route_probe_mem_sample(sample_read, NULL);
    route_probe_log_close();
    FILE *file = fopen(path, "r");
    char text[2000];
    size_t got = file != NULL ? fread(text, 1u, sizeof text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    text[got] = '\0';
    CHECK(strstr(text, "mem 0x1000 = 0x65") != NULL && strstr(text, "mem 0x1000 0x65 -> 0x66") != NULL);
    CHECK(strstr(text, "mem *0x2000+8:4 = 0x7") != NULL); /* unreadable (null pointer) at first, so no line until it reads */
    CHECK(strstr(text, "unreadable") == NULL);
    CHECK(strstr(text, "0x65 -> 0x65") == NULL); /* a sample without a change writes nothing */
    /* the same text twice is not logged twice */
    const char *first = strstr(text, "mem 0x1000 = 0x65");
    CHECK(first != NULL && strstr(first + 5, "mem 0x1000 = 0x65") == NULL);
    remove(path);
}

/* ---- the record's lines and the merge in host_route.c ---- */
static bool g_mark_facts_called;
static bool fake_facts(unsigned mark, char *out, size_t out_size, void *user)
{
    (void)user;
    g_mark_facts_called = true;
    snprintf(out, out_size, "seg-ms=%u io=7", mark * 100u);
    return true;
}
static void test_record_lines(void)
{
    char path[] = "/tmp/tsfp-route-rec-XXXXXX";
    int descriptor = mkstemp(path);
    close(descriptor);
    char error[2400];
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    CHECK(xinput_record_enable_marks(path));
    g_mark_facts_called = false;
    xinput_record_set_mark_facts(fake_facts, NULL);
    xinput_pad_state rest = {0};
    for (uint64_t i = 0; i < 3; i++) xinput_record_observe(i, 0u, &rest);
    raise(SIGUSR2);
    for (uint64_t i = 3; i < 6; i++) xinput_record_observe(i, 0u, &rest);
    xinput_record_close();
    xinput_record_set_mark_facts(NULL, NULL);
    CHECK(g_mark_facts_called);
    char pid_path[512];
    snprintf(pid_path, sizeof pid_path, "%s.pid", path);
    remove(pid_path);
    FILE *file = fopen(path, "r");
    char text[4096];
    size_t got = file != NULL ? fread(text, 1u, sizeof text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    text[got] = '\0';
    CHECK(strstr(text, "# mark: at=3\n# mark-info: mark1 seg-ms=100 io=7\n") != NULL);
    /* insert waits the way tools.route_events does: after the mark-info line */
    char *info = strstr(text, "# mark-info:");
    char edited[4600];
    if (info != NULL) {
        char *line_end = strchr(info, '\n') + 1;
        snprintf(edited, sizeof edited, "%.*s# wait: mark1:file-open=anicemap.mkr,file-idle=500,timeout=90000\n%s", (int)(line_end - text), text, line_end);
    } else {
        edited[0] = '\0';
    }
    file = fopen(path, "w");
    if (file != NULL) { fputs(edited, file); fclose(file); }
    uint64_t frames = 0u;
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && frames == 6u);
    CHECK(xinput_replay_wait_count() == 1u && strcmp(xinput_replay_wait(0u), "mark1:file-open=anicemap.mkr,file-idle=500,timeout=90000") == 0);
    CHECK(xinput_replay_wait(1u) == NULL);

    /* host_route merges: the record's wait applies, a command line wait for the same mark replaces it */
    fresh();
    host_route_config config;
    memset(&config, 0, sizeof config);
    const xinput_route_probe *probe = route_probe_view();
    config.probe = probe;
    config.read_mem = read_mem;
    const char *record_waits[] = {"mark1:file-open=anicemap.mkr,timeout=90000"};
    config.record_waits = record_waits;
    config.record_wait_count = 1u;
    const uint64_t marks[] = {3u};
    xinput_script *script = script_of("3\n3 START\n");
    CHECK(host_route_setup_from(&config, script, marks, 1u, error, sizeof error));
    xinput_route *route = host_route_current();
    CHECK(route != NULL);
    xinput_pad_state state;
    for (uint64_t i = 0; i < 6; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(xinput_route_cursor(route) == 3u && xinput_route_stalled_total(route) == 3u); /* held at mark 1 by the record's wait */
    route_probe_note_file(KERNEL_FILE_EVENT_OPEN, "x\\anicemap.mkr", 0u);
    CHECK(xinput_route_source(6u, &state, route) && xinput_route_cursor(route) == 4u);
    host_route_teardown();
    /* command line replaces the record's wait */
    fresh();
    const char *event_waits[] = {"mark1:min=2"};
    config.event_waits = event_waits;
    config.event_wait_count = 1u;
    CHECK(host_route_setup_from(&config, script, marks, 1u, error, sizeof error));
    route = host_route_current();
    for (uint64_t i = 0; i < 8; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(xinput_route_cursor(route) > 3u); /* min=2 only, the record's file-open never needed */
    host_route_teardown();
    /* a bad record wait names its line */
    fresh();
    config.event_wait_count = 0u;
    const char *bad_record[] = {"mark1:file-open=a", "mark2:bogus=1"};
    config.record_waits = bad_record;
    config.record_wait_count = 2u;
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "'# wait:' line 2") != NULL &&
          strstr(error, "bogus") != NULL);
    /* two record waits for one mark */
    const char *twice[] = {"mark1:file-open=a", "mark1:min=3"};
    config.record_waits = twice;
    CHECK(!host_route_setup_from(&config, script, marks, 1u, error, sizeof error) && strstr(error, "two") != NULL);
    xinput_script_free(script);

    /* a malformed '# wait:' line refuses the record at load time */
    file = fopen(path, "w");
    if (file != NULL) {
        char bad[4700];
        snprintf(bad, sizeof bad, "%s", edited);
        char *line = strstr(bad, "# wait: mark1");
        if (line != NULL) memcpy(line, "# wait: nark1", 13);
        fputs(bad, file);
        fclose(file);
    }
    CHECK(!xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && strstr(error, "'# wait:' line") != NULL);
    /* more waits than the route supports */
    file = fopen(path, "w");
    if (file != NULL) {
        char many[8000];
        size_t used = (size_t)snprintf(many, sizeof many, "%s", edited);
        char *trailer = strstr(many, "# polls:");
        char tail[64];
        snprintf(tail, sizeof tail, "%s", trailer != NULL ? trailer : "");
        if (trailer != NULL) {
            used = (size_t)(trailer - many);
            for (int i = 0; i < 9; i++) used += (size_t)snprintf(many + used, sizeof many - used, "# wait: mark1:min=%d\n", i + 1);
            snprintf(many + used, sizeof many - used, "%s", tail);
        }
        fputs(many, file);
        fclose(file);
    }
    CHECK(!xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames));
    remove(path);
}

/* The waits and the log only observe the guest: a route recorded without them replays with them (identity unchanged). */
static void test_identity(void)
{
    char *with[] = {"host", "d.xbe", "--skip-intro", "--route-wait-event", "mark1:file-open=a", "--route-event-log", "/tmp/e.log",
                    "--route-log-mem", "0x79094C", "--route-wait-event", "mark2:min=3", "--native-xmv", "--route-log-mem", "*0x784014"};
    char *without[] = {"host", "d.xbe", "--skip-intro", "--native-xmv"};
    char line_with[300], line_without[300];
    CHECK(xinput_record_identity_flags(14, with, line_with, sizeof line_with));
    CHECK(xinput_record_identity_flags(4, without, line_without, sizeof line_without));
    CHECK(strcmp(line_with, "--skip-intro --native-xmv") == 0 && strcmp(line_without, line_with) == 0);
    /* look-alikes are game flags and stay */
    char *alike[] = {"host", "d.xbe", "--route-wait-events", "--route-event-logs", "x"};
    char line_alike[300];
    CHECK(xinput_record_identity_flags(5, alike, line_alike, sizeof line_alike) && strstr(line_alike, "--route-wait-events") != NULL &&
          strstr(line_alike, "--route-event-logs") != NULL);
    /* an old record that names them in its flags line still canonicalises to the same thing */
    char canonical[300];
    CHECK(xinput_record_canonical_line("--skip-intro --route-wait-event mark1:min=3 --route-log-mem 0x10 --native-xmv", canonical, sizeof canonical) &&
          strcmp(canonical, "--skip-intro --native-xmv") == 0);
}

static bool parse_args(options *out, int argc, const char *const *argv) { return parse_options(argc, (char **)argv, out); }
static void test_host_options(void)
{
    options opt;
    const char *good[] = {"p", "--synthetic-pad", "--replay-input", "r.txt", "--route-wait-event", "mark2:file-open=anicemap.mkr,file-idle=800,timeout=180000",
                          "--route-wait-event", "mark1:min=5", "--route-event-log", "log.txt", "--route-log-mem", "0x79094C",
                          "--route-log-mem", "*0x784014+4:2", "game.xbe"};
    CHECK(parse_args(&opt, 15, good));
    CHECK(opt.route_event_wait_count == 2u && opt.route_event_log != NULL && strcmp(opt.route_event_log, "log.txt") == 0 && opt.route_log_mem_count == 2u);
    CHECK(strcmp(opt.route_event_waits[0], "mark2:file-open=anicemap.mkr,file-idle=800,timeout=180000") == 0);
    /* defaults: nothing on */
    const char *bare[] = {"p", "game.xbe"};
    CHECK(parse_args(&opt, 2, bare) && opt.route_event_wait_count == 0u && opt.route_event_log == NULL && opt.route_log_mem_count == 0u);
    /* a wait needs a replay, a bad spec or missing value is refused, so is sampled memory without its log */
    const char *no_replay[] = {"p", "--route-wait-event", "mark1:min=5", "game.xbe"};
    CHECK(!parse_args(&opt, 4, no_replay));
    const char *bad_spec[] = {"p", "--synthetic-pad", "--replay-input", "r", "--route-wait-event", "mark1:bogus=1", "game.xbe"};
    CHECK(!parse_args(&opt, 7, bad_spec));
    const char *missing[] = {"p", "--synthetic-pad", "--replay-input", "r", "--route-wait-event"};
    CHECK(!parse_args(&opt, 5, missing));
    const char *mem_alone[] = {"p", "--route-log-mem", "0x1000", "game.xbe"};
    CHECK(!parse_args(&opt, 4, mem_alone));
    const char *mem_bad[] = {"p", "--route-event-log", "l", "--route-log-mem", "0x1001", "game.xbe"};
    CHECK(!parse_args(&opt, 6, mem_bad));
    const char *log_alone[] = {"p", "--route-event-log", "l", "game.xbe"};
    CHECK(parse_args(&opt, 4, log_alone) && opt.route_event_wait_count == 0u);
    /* the log and sampled memory also work in a recording session */
    const char *recording[] = {"p", "--synthetic-pad", "--record-input", "r", "--route-event-log", "l", "--route-log-mem", "0x1000", "game.xbe"};
    CHECK(parse_args(&opt, 9, recording));
    /* more than 8 waits across both flags is refused */
    const char *many[] = {"p", "--synthetic-pad", "--replay-input", "r",
                          "--route-wait", "mark1:min=1", "--route-wait", "mark2:min=1", "--route-wait", "mark3:min=1", "--route-wait", "mark4:min=1",
                          "--route-wait", "mark5:min=1", "--route-wait", "mark6:min=1", "--route-wait", "mark7:min=1", "--route-wait", "mark8:min=1",
                          "--route-wait-event", "mark9:min=1", "game.xbe"};
    CHECK(!parse_args(&opt, 23, many));
    /* the help carries the marker the scripts grep for, and its example parses */
    const char *help = host_options_route_event_help();
    CHECK(help != NULL && strstr(help, "T1633") != NULL && strstr(help, "--route-wait-event") != NULL && strstr(help, "--route-event-log") != NULL &&
          strstr(help, "--route-log-mem") != NULL && strstr(help, "# wait:") != NULL && strstr(help, "# mark-info:") != NULL);
    CHECK(strstr(help, "--route-wait-event mark2:file-open=anicemap.mkr,file-idle=800,timeout=180000") != NULL);
}

int main(void)
{
    test_identity();
    test_host_options();
    test_grammar();
    test_file_open_and_segments();
    test_idle_call_frame_min();
    test_frame_stable_needs_a_change();
    test_mem_ops_indirect_any();
    test_failure_progress_timeout();
    test_creation_refusals();
    test_probe();
    test_mark_facts_and_log();
    test_mem_sampling();
    test_record_lines();
    route_probe_reset();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
