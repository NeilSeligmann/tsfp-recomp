/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1616: the route replay. Spec grammars, the route player (cursor, waits, marks, events, handover), the poke triggers
 * of host_route.c (through the test seams, no guest memory) and the SIGUSR2 mark recording round trip through a file.
 * FABRICATED input, synthetic scripts, nothing of the title.
 */
#include "host_route.h"
#include "recomp_abi.h"
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
#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

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

/* ---- recording of events and a fake memory / live pad ---- */
typedef struct {
    int count;
    xinput_route_event events[32];
    uint32_t memory;      /* the sentinel value */
    unsigned mem_reads;
    unsigned live_polls;
    bool live_press;      /* the live pad holds RIGHT */
    unsigned pokes;
    char poke_labels[8][64];
} fixture;
static fixture fx;

static void on_event(const xinput_route_event *event, void *user)
{
    (void)user;
    if (fx.count < 32) fx.events[fx.count++] = *event;
}
static bool read_mem(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    (void)width;
    if (address != 0x52D3FCu) return false;
    fx.mem_reads++;
    *value = fx.memory;
    return true;
}
static bool live_fn(uint64_t poll, xinput_pad_state *out, void *user)
{
    (void)poll;
    (void)user;
    fx.live_polls++;
    memset(out, 0, sizeof *out);
    if (fx.live_press) out->digital_buttons = XINPUT_BUTTON_DPAD_RIGHT;
    return true;
}
static xinput_route *make(const xinput_script *script, const char *const *wait_specs, size_t wait_count, bool live)
{
    xinput_route_wait waits[4];
    char error[200];
    for (size_t i = 0; i < wait_count; i++)
        if (!xinput_route_wait_parse(wait_specs[i], &waits[i], error, sizeof error)) { printf("wait: %s\n", error); return NULL; }
    memset(&fx, 0, sizeof fx);
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.read_mem = read_mem;
    hooks.on_event = on_event;
    if (live) hooks.live = live_fn;
    return xinput_route_create(script, MARKS, 2u, waits, wait_count, &hooks, error, sizeof error);
}
static xinput_pad_state poll(xinput_route *route, uint64_t index)
{
    xinput_pad_state state;
    CHECK(xinput_route_source(index, &state, route));
    return state;
}

static void test_specs(void)
{
    xinput_route_wait wait;
    char error[200];
    CHECK(xinput_route_wait_parse("mark3:mem=0x52D3FC==6,min=10,max=500", &wait, error, sizeof error));
    CHECK(wait.mark == 3u && wait.has_mem && wait.address == 0x52D3FCu && wait.width == 4u && wait.value == 6u &&
          wait.mask == 0xFFFFFFFFu && wait.min_polls == 10u && wait.max_polls == 500u);
    CHECK(xinput_route_wait_parse("mark1:mem=0x100:1==0x80&0xF0", &wait, error, sizeof error));
    CHECK(wait.width == 1u && wait.value == 0x80u && wait.mask == 0xF0u && wait.max_polls == XINPUT_ROUTE_DEFAULT_MAX_POLLS);
    CHECK(xinput_route_wait_parse("mark2:min=30", &wait, error, sizeof error) && !wait.has_mem && wait.min_polls == 30u);
    /* refusals: each names the spec */
    CHECK(!xinput_route_wait_parse("mark2:max=30", &wait, error, sizeof error) && strstr(error, "never waits") != NULL);
    CHECK(!xinput_route_wait_parse("mark0:min=1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark17:min=1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("poll3:min=1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:min=5,max=2", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:min=1,min=2", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:mem=0x101:4==1", &wait, error, sizeof error)); /* unaligned */
    CHECK(!xinput_route_wait_parse("mark1:mem=0x100==0x100&0xFF", &wait, error, sizeof error)); /* hidden by the mask */
    CHECK(!xinput_route_wait_parse("mark1:mem=0x4000000==1", &wait, error, sizeof error)); /* past guest memory */
    CHECK(!xinput_route_wait_parse("mark1:mem=0x100:3==1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:bogus=1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:min=-1", &wait, error, sizeof error));
    CHECK(!xinput_route_wait_parse("mark1:max=0,min=0", &wait, error, sizeof error));

    poke_trigger_entry entry;
    CHECK(poke_trigger_parse("1500:forced_b1", &entry, error, sizeof error) && entry.where == POKE_AT_POLL && entry.value == 1500u);
    CHECK(poke_trigger_parse("mark3:forced-b2", &entry, error, sizeof error) && entry.where == POKE_AT_MARK && entry.value == 3u);
    CHECK(poke_trigger_parse("replay-end:x", &entry, error, sizeof error) && entry.where == POKE_AT_END);
    CHECK(!poke_trigger_parse("0:x", &entry, error, sizeof error));
    CHECK(!poke_trigger_parse("mark0:x", &entry, error, sizeof error));
    CHECK(!poke_trigger_parse("end:x", &entry, error, sizeof error));
    CHECK(!poke_trigger_parse("5:", &entry, error, sizeof error));
    CHECK(!poke_trigger_parse("5:a b", &entry, error, sizeof error));
    CHECK(!poke_trigger_parse("5", &entry, error, sizeof error));

    poke_trigger_set set;
    memset(&set, 0, sizeof set);
    CHECK(poke_trigger_parse("10:a", &set.entries[0], error, sizeof error));
    CHECK(poke_trigger_parse("mark2:b", &set.entries[1], error, sizeof error));
    CHECK(poke_trigger_parse("replay-end:c", &set.entries[2], error, sizeof error));
    set.count = 3u;
    CHECK(poke_trigger_take(&set, POKE_AT_POLL, 9u) == NULL);
    CHECK(poke_trigger_take(&set, POKE_AT_MARK, 1u) == NULL);
    CHECK(poke_trigger_take(&set, POKE_AT_MARK, 2u) != NULL);
    CHECK(poke_trigger_take(&set, POKE_AT_MARK, 2u) == NULL); /* once */
    const char *at_poll = poke_trigger_take(&set, POKE_AT_POLL, 10u); /* exactly at */
    CHECK(at_poll != NULL && strcmp(at_poll, "a") == 0);
    CHECK(poke_trigger_take(&set, POKE_AT_POLL, 12u) == NULL); /* once */
    poke_trigger_entry late;
    CHECK(poke_trigger_parse("10:late", &late, error, sizeof error));
    set.entries[0] = late;
    CHECK(poke_trigger_take(&set, POKE_AT_POLL, 12u) != NULL); /* or after (a skipped poll) */
    CHECK(poke_trigger_take(&set, POKE_AT_END, 0u) != NULL);
}

static void test_plain_replay_and_handover(void)
{
    xinput_script *script = script_of(SCRIPT);
    xinput_route *route = make(script, NULL, 0u, true);
    CHECK(route != NULL);
    fx.live_press = true; /* the owner leans on the pad during the replay: discarded */
    for (uint64_t i = 0; i < 3; i++) CHECK(poll(route, i).digital_buttons == 0u);
    CHECK(poll(route, 3u).digital_buttons == XINPUT_BUTTON_START);
    CHECK(poll(route, 4u).digital_buttons == XINPUT_BUTTON_START);
    CHECK(poll(route, 5u).analog[0] == 255u && poll(route, 6u).analog[0] == 255u);
    CHECK(!xinput_route_ended(route) && xinput_route_cursor(route) == 7u);
    /* handover: the 8th poll is the live pad (held RIGHT), END fires exactly once */
    xinput_pad_state live = poll(route, 7u);
    CHECK(live.digital_buttons == XINPUT_BUTTON_DPAD_RIGHT && xinput_route_ended(route));
    live = poll(route, 8u);
    CHECK(live.digital_buttons == XINPUT_BUTTON_DPAD_RIGHT);
    fx.live_press = false;
    CHECK(poll(route, 9u).digital_buttons == 0u);
    CHECK(fx.live_polls == 10u); /* drained at every poll, also during the replay */
    int ends = 0, marks = 0;
    for (int i = 0; i < fx.count; i++) {
        ends += fx.events[i].kind == XINPUT_ROUTE_END;
        marks += fx.events[i].kind == XINPUT_ROUTE_MARK;
    }
    CHECK(ends == 1 && marks == 2);
    /* mark order and positions: mark1 at cursor 3 (host poll 3), mark2 at cursor 5 */
    CHECK(fx.events[0].kind == XINPUT_ROUTE_MARK && fx.events[0].index == 1u && fx.events[0].cursor == 3u);
    CHECK(fx.events[1].kind == XINPUT_ROUTE_MARK && fx.events[1].index == 2u && fx.events[1].cursor == 5u);
    xinput_route_free(route);

    /* no live source: the pad is at rest after the record */
    route = make(script, NULL, 0u, false);
    for (uint64_t i = 0; i < 7; i++) (void)poll(route, i);
    CHECK(poll(route, 7u).digital_buttons == 0u && xinput_route_ended(route));
    xinput_route_free(route);
    xinput_script_free(script);
}

static void test_wait_stalls_the_record(void)
{
    xinput_script *script = script_of(SCRIPT);
    const char *waits[] = {"mark1:mem=0x52D3FC==6,max=100"};
    xinput_route *route = make(script, waits, 1u, false);
    CHECK(route != NULL);
    fx.memory = 0u;
    for (uint64_t i = 0; i < 3; i++) CHECK(poll(route, i).digital_buttons == 0u);
    CHECK(xinput_route_cursor(route) == 3u);
    /* 40 polls with the sentinel unset: the pad is at rest and the record does not move */
    for (uint64_t i = 3; i < 43; i++) {
        xinput_pad_state state = poll(route, i);
        CHECK(state.digital_buttons == 0u && xinput_route_cursor(route) == 3u);
    }
    CHECK(xinput_route_stalled_total(route) == 40u && fx.mem_reads >= 40u);
    for (int i = 0; i < fx.count; i++) CHECK(fx.events[i].kind != XINPUT_ROUTE_MARK); /* mark1 waits for its condition */
    fx.memory = 6u; /* the editor is up */
    CHECK(poll(route, 43u).digital_buttons == XINPUT_BUTTON_START); /* the stream resumes where it paused */
    CHECK(xinput_route_cursor(route) == 4u);
    CHECK(fx.events[0].kind == XINPUT_ROUTE_WAIT_OK && fx.events[0].index == 1u && fx.events[0].stalled == 40u);
    CHECK(fx.events[1].kind == XINPUT_ROUTE_MARK && fx.events[1].index == 1u && fx.events[1].poll == 43u);
    xinput_route_free(route);

    xinput_script_free(script);
}

static void test_wait_min_and_timeout(void)
{
    xinput_script *script = script_of(SCRIPT);
    const char *min_wait[] = {"mark2:min=5"};
    xinput_route *route = make(script, min_wait, 1u, false);
    for (uint64_t i = 0; i < 5; i++) (void)poll(route, i);
    CHECK(xinput_route_cursor(route) == 5u);
    for (uint64_t i = 5; i < 10; i++) CHECK(poll(route, i).analog[0] == 0u); /* five polls of rest, no memory needed */
    CHECK(xinput_route_cursor(route) == 5u);
    CHECK(poll(route, 10u).analog[0] == 255u);
    xinput_route_free(route);

    const char *timeout_wait[] = {"mark1:mem=0x52D3FC==6,max=20"};
    route = make(script, timeout_wait, 1u, true);
    fx.memory = 1u;
    for (uint64_t i = 0; i < 3; i++) (void)poll(route, i);
    for (uint64_t i = 3; i < 23; i++) CHECK(!xinput_route_failed(route) && poll(route, i).digital_buttons == 0u);
    CHECK(poll(route, 23u).digital_buttons == 0u && xinput_route_failed(route));
    int timeouts = 0;
    for (int i = 0; i < fx.count; i++) timeouts += fx.events[i].kind == XINPUT_ROUTE_WAIT_TIMEOUT;
    CHECK(timeouts == 1 && fx.events[fx.count - 1].index == 1u && fx.events[fx.count - 1].stalled == 20u);
    for (uint64_t i = 24; i < 40; i++) CHECK(poll(route, i).digital_buttons == 0u); /* nothing replays after a failure */
    CHECK(xinput_route_cursor(route) == 3u && !xinput_route_ended(route));
    xinput_route_free(route);

    /* a wait for a mark the record does not have, and marks past the record, are refused */
    xinput_route_wait wait;
    char error[200];
    CHECK(xinput_route_wait_parse("mark3:min=1", &wait, error, sizeof error));
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    CHECK(xinput_route_create(script, MARKS, 2u, &wait, 1u, &hooks, error, sizeof error) == NULL && strstr(error, "mark3") != NULL);
    const uint64_t past[] = {3u, 99u};
    CHECK(xinput_route_create(script, past, 2u, NULL, 0u, &hooks, error, sizeof error) == NULL);
    const uint64_t backwards[] = {5u, 3u};
    CHECK(xinput_route_create(script, backwards, 2u, NULL, 0u, &hooks, error, sizeof error) == NULL);
    xinput_script_free(script);
}

/* ---- host_route: the poke triggers through the test seams ---- */
static bool fake_poke(const char *label)
{
    if (fx.pokes < 8u) {
        snprintf(fx.poke_labels[fx.pokes], sizeof fx.poke_labels[0], "%s", label);
    }
    fx.pokes++;
    return true;
}
static bool seam_read(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    return read_mem(address, width, value, user);
}
static int failure_calls;
static char failure_reason[256];
static void on_failure(const char *reason)
{
    failure_calls++;
    snprintf(failure_reason, sizeof failure_reason, "%s", reason);
}
static void test_poke_triggers(void)
{
    xinput_script *script = script_of(SCRIPT);
    const char *wait_specs[] = {"mark2:mem=0x52D3FC==6,max=50"};
    const char *poke_specs[] = {"mark2:at_mark", "2:at_poll", "replay-end:at_end"};
    host_route_config config;
    memset(&config, 0, sizeof config);
    config.waits = wait_specs;
    config.wait_count = 1u;
    config.pokes = poke_specs;
    config.poke_count = 3u;
    config.live = live_fn;
    config.on_failure = on_failure;
    config.read_mem = seam_read;
    config.poke = fake_poke;
    memset(&fx, 0, sizeof fx);
    char error[200];
    CHECK(!host_route_handed_over()); /* T1629: no route, no handover */
    CHECK(host_route_setup_from(&config, script, MARKS, 2u, error, sizeof error));
    fx.memory = 0u;
    xinput_source_reset();
    xinput_route *route = host_route_current();
    CHECK(route != NULL);
    CHECK(!host_route_handed_over());
    xinput_pad_state state;
    for (uint64_t i = 0; i < 12; i++) CHECK(xinput_route_source(i, &state, route));
    /* poll 2 poke fires at host poll 2; the mark2 poke has NOT fired: its wait (sentinel 6) holds the record at 5 */
    CHECK(fx.pokes == 1u && strcmp(fx.poke_labels[0], "at_poll") == 0);
    CHECK(xinput_route_cursor(route) == 5u);
    CHECK(!host_route_handed_over()); /* the record is not exhausted */
    fx.memory = 6u;
    for (uint64_t i = 12; i < 30; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(fx.pokes == 3u && strcmp(fx.poke_labels[1], "at_mark") == 0 && strcmp(fx.poke_labels[2], "at_end") == 0);
    CHECK(xinput_route_ended(route));
    CHECK(host_route_handed_over()); /* T1629: the query the button dump arms on */
    for (uint64_t i = 30; i < 40; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(fx.pokes == 3u); /* each trigger fires once */
    CHECK(failure_calls == 0);
    host_route_teardown();
    CHECK(!host_route_handed_over());

    /* timeout reaches on_failure and no poke fires for the marks after it */
    memset(&fx, 0, sizeof fx);
    const char *quick[] = {"mark1:mem=0x52D3FC==6,max=5"};
    const char *after[] = {"mark2:never", "replay-end:never_either"};
    config.waits = quick;
    config.pokes = after;
    config.poke_count = 2u;
    CHECK(host_route_setup_from(&config, script, MARKS, 2u, error, sizeof error));
    route = host_route_current();
    for (uint64_t i = 0; i < 20; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(failure_calls == 1 && strstr(failure_reason, "mark 1") != NULL && fx.pokes == 0u);
    host_route_teardown();

    /* setup refusals: a poke at a mark the record lacks, a bad spec, a wait for a missing mark */
    const char *bad_mark[] = {"mark3:x"};
    config.waits = NULL;
    config.wait_count = 0u;
    config.pokes = bad_mark;
    config.poke_count = 1u;
    CHECK(!host_route_setup_from(&config, script, MARKS, 2u, error, sizeof error) && strstr(error, "mark3") != NULL);
    const char *bad_spec[] = {"nonsense"};
    config.pokes = bad_spec;
    CHECK(!host_route_setup_from(&config, script, MARKS, 2u, error, sizeof error));
    CHECK(host_route_current() == NULL);
    xinput_script_free(script);
}

/* ---- recording marks by SIGUSR2 and reading them back ---- */
static void test_mark_round_trip(void)
{
    char path[] = "/tmp/tsfp-route-test-XXXXXX";
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    close(descriptor);
    char error[2400];
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    CHECK(xinput_record_enable_marks(path));
    xinput_pad_state start = {0};
    start.digital_buttons = XINPUT_BUTTON_START;
    xinput_pad_state rest = {0};
    for (uint64_t i = 0; i < 3; i++) xinput_record_observe(i, 0u, &rest);
    raise(SIGUSR2);                                  /* the owner marks here: before poll 3 */
    for (uint64_t i = 3; i < 5; i++) xinput_record_observe(i, 0u, &start);
    raise(SIGUSR2);                                  /* a second mark before poll 5 */
    raise(SIGUSR2);                                  /* and a third at the same position */
    for (uint64_t i = 5; i < 7; i++) xinput_record_observe(i, 0u, &rest);
    xinput_record_close();
    CHECK(xinput_record_marks_written() == 3u);
    char pid_path[512];
    snprintf(pid_path, sizeof pid_path, "%s.pid", path);
    FILE *pid_file = fopen(pid_path, "r");
    CHECK(pid_file != NULL);
    int pid = 0;
    if (pid_file != NULL) { CHECK(fscanf(pid_file, "%d", &pid) == 1 && pid == (int)getpid()); fclose(pid_file); }
    remove(pid_path);

    uint64_t frames = 0u;
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames));
    CHECK(frames == 7u);
    const uint64_t *marks = NULL;
    const size_t count = xinput_replay_marks(&marks);
    CHECK(count == 3u && marks != NULL && marks[0] == 3u && marks[1] == 5u && marks[2] == 5u);
    CHECK(xinput_replay_script() != NULL);
    /* the file is still a valid script: marks are comments */
    CHECK(xinput_script_state_at(xinput_replay_script(), 3u).digital_buttons == XINPUT_BUTTON_START);
    CHECK(xinput_script_state_at(xinput_replay_script(), 5u).digital_buttons == 0u);
    /* route over the loaded record: same positions */
    memset(&fx, 0, sizeof fx);
    xinput_route_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.on_event = on_event;
    xinput_route *route = xinput_route_create(xinput_replay_script(), marks, count, NULL, 0u, &hooks, error, sizeof error);
    CHECK(route != NULL);
    xinput_pad_state state;
    for (uint64_t i = 0; i < 8; i++) CHECK(xinput_route_source(i, &state, route));
    CHECK(fx.events[0].index == 1u && fx.events[0].cursor == 3u && fx.events[2].index == 3u && fx.events[2].cursor == 5u);
    xinput_route_free(route);

    /* a mark line with a position past the record, or out of order, is refused */
    FILE *file = fopen(path, "r");
    char text[4096];
    size_t got = file != NULL ? fread(text, 1u, sizeof text - 1u, file) : 0u;
    if (file != NULL) fclose(file);
    text[got] = '\0';
    char *first_mark = strstr(text, "# mark: at=3");
    CHECK(first_mark != NULL);
    if (first_mark != NULL) first_mark[11] = '9'; /* at=9 > 7 polls */
    file = fopen(path, "w");
    if (file != NULL) { fputs(text, file); fclose(file); }
    CHECK(!xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof error, &frames) && strstr(error, "bad mark") != NULL);
    remove(path);
}

int main(void)
{
    test_specs();
    test_plain_replay_and_handover();
    test_wait_stalls_the_record();
    test_wait_min_and_timeout();
    test_poke_triggers();
    test_mark_round_trip();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
