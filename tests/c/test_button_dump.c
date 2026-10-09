/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1629: the passive button dump, src/host/button_dump.c (pure core: mask, edges, scheduler, caps, writer, manifest) and
 * button_dump_host.c (real wiring through the xinput pre install hook and guest_dump snapshots).
 *
 * The core is driven poll by poll with FAKE capture / write / clock / present callbacks, so every expectation is exact:
 * which poll an event opens at, which polls its dumps are captured at, the label and the manifest line as a string. The
 * last test runs the real wiring over a synthetic guest region and compares the written files with live dumps.
 * Each group asserts something was produced before it compares (an empty list equals an empty list).
 */
#define _DEFAULT_SOURCE 1
#include "button_dump.h"
#include "button_dump_host.h"

#include "guest_dump.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_memory.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "xinput_devices.h"
#include "xinput_source.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the thunk library reads the guest registers, this suite defines them */
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                     \
    do {                                                                               \
        checks++;                                                                      \
        const unsigned long long a_ = (unsigned long long)(actual);                    \
        const unsigned long long e_ = (unsigned long long)(expected);                  \
        if (a_ != e_) {                                                                \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,      \
                   #actual, a_, e_);                                                   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define CHECK_STR(actual, expected)                                                    \
    do {                                                                               \
        checks++;                                                                      \
        const char *a_ = (actual);                                                     \
        const char *e_ = (expected);                                                   \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                                       \
            printf("FAIL %s:%d  %s ==\n  \"%s\"\nexpected\n  \"%s\"\n", __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_); \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* ---- mask bits by name ---- */
#define UP 0x0001u
#define DOWN 0x0002u
#define LEFT 0x0004u
#define RIGHT 0x0008u
#define START 0x0010u
#define BACK 0x0020u
#define LTHUMB 0x0040u
#define RTHUMB 0x0080u
#define A_ 0x0100u
#define B_ 0x0200u
#define X_ 0x0400u
#define Y_ 0x0800u
#define BLACK 0x1000u
#define WHITE 0x2000u
#define LT_ 0x4000u
#define RT_ 0x8000u

static xinput_pad_state state_of(unsigned mask)
{
    xinput_pad_state state;
    memset(&state, 0, sizeof state);
    state.digital_buttons = (uint16_t)(mask & 0xFFu);
    for (unsigned index = 0u; index < 8u; index++) {
        state.analog[index] = (mask & (1u << (8u + index))) != 0u ? 255u : 0u;
    }
    return state;
}

/* ---- the fakes ---- */

typedef struct {
    unsigned id;
} fake_snapshot;

typedef struct {
    pthread_mutex_t gate; /* the writer takes it for every write, the test holding it stalls the writer */
    atomic_uint captures;
    atomic_uint writes;
    atomic_uint discards;
    atomic_uint clock_calls;
    unsigned fail_capture; /* the Nth capture returns NULL (1 based), 0 = never */
    unsigned fail_write;   /* the Nth write fails without a file */
    unsigned unreadable_write; /* the Nth write makes a file but returns false */
    uint64_t present;
    bool armed;
    char log[16][200];
    atomic_uint log_count;
    pthread_mutex_t log_lock;
} fake;

static void *fake_capture(void *user)
{
    fake *f = user;
    const unsigned number = atomic_fetch_add(&f->captures, 1u) + 1u;
    if (f->fail_capture == number) {
        return NULL;
    }
    fake_snapshot *snapshot = malloc(sizeof *snapshot);
    snapshot->id = number;
    return snapshot;
}

static bool fake_write(void *user, void *snapshot, const char *path, const char *header)
{
    fake *f = user;
    pthread_mutex_lock(&f->gate);
    pthread_mutex_unlock(&f->gate);
    const unsigned number = atomic_fetch_add(&f->writes, 1u) + 1u;
    if (f->fail_write == number) {
        return false;
    }
    char temporary[700];
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "snapshot %u\n%s", ((fake_snapshot *)snapshot)->id, header);
    fclose(file);
    if (rename(temporary, path) != 0) {
        return false;
    }
    return f->unreadable_write != number;
}

static void fake_discard(void *user, void *snapshot)
{
    fake *f = user;
    atomic_fetch_add(&f->discards, 1u);
    free(snapshot);
}

static uint64_t fake_present(void *user)
{
    return ((fake *)user)->present;
}

static bool fake_armed(void *user)
{
    return ((fake *)user)->armed;
}

/* monotonic 5000 + 250 per call, wall 2026-10-08T12:00:00.000Z + 250 ms per call: every time in a manifest is exact */
static void fake_clock(void *user, uint64_t *mono_ms, uint64_t *wall_ms)
{
    fake *f = user;
    const unsigned calls = atomic_fetch_add(&f->clock_calls, 1u);
    *mono_ms = 5000u + 250u * calls;
    *wall_ms = 1791460800000ull + 250u * calls;
}

static void fake_log(void *user, const char *line)
{
    fake *f = user;
    pthread_mutex_lock(&f->log_lock);
    const unsigned at = atomic_load(&f->log_count);
    if (at < 16u) {
        snprintf(f->log[at], sizeof f->log[at], "%s", line);
    }
    atomic_store(&f->log_count, at + 1u);
    pthread_mutex_unlock(&f->log_lock);
}

typedef struct {
    button_dump *dump;
    fake f;
    char dir[64];
} run;

static void remove_tree(const char *dir)
{
    char command[160];
    snprintf(command, sizeof command, "rm -rf %s", dir);
    CHECK(system(command) == 0);
}

static button_dump_config base_config(const char *dir)
{
    button_dump_config config;
    memset(&config, 0, sizeof config);
    config.after_count = 2u;
    config.after[0] = 2u;
    config.after[1] = 30u;
    config.threshold = 60u;
    config.coalesce = 3u;
    config.max_pending = 8u;
    config.max_dumps = 2000u;
    config.max_bytes = 0x40000000ull;
    config.max_idle = 40u;
    config.bytes_per_dump = 1000u;
    config.snapshot_bytes = 1000u;
    config.ranges = 14u;
    config.dump_dir = dir;
    return config;
}

static bool g_omit_armed_query; /* create with ops.armed == NULL */
static bool g_keep_dir;         /* do not wipe the directory first (a second session in the same place) */

static bool start_run(run *r, const char *name, const button_dump_config *config)
{
    memset(r, 0, sizeof *r);
    snprintf(r->dir, sizeof r->dir, "bd_%s", name);
    if (!g_keep_dir) {
        remove_tree(r->dir);
    }
    pthread_mutex_init(&r->f.gate, NULL);
    pthread_mutex_init(&r->f.log_lock, NULL);
    r->f.present = 456u;
    button_dump_config copy = *config;
    copy.dump_dir = r->dir;
    button_dump_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.capture = fake_capture;
    ops.write = fake_write;
    ops.discard = fake_discard;
    ops.present = fake_present;
    ops.armed = g_omit_armed_query ? NULL : fake_armed;
    ops.clock = fake_clock;
    ops.log = fake_log;
    ops.user = &r->f;
    char error[200] = "";
    r->dump = button_dump_create(&copy, &ops, error, sizeof error);
    CHECK(r->dump != NULL);
    if (r->dump == NULL) {
        printf("  create failed: %s\n", error);
    }
    return r->dump != NULL;
}

static void poll_mask(run *r, uint64_t poll, unsigned mask)
{
    const xinput_pad_state state = state_of(mask);
    button_dump_poll(r->dump, poll, &state);
}

static void poll_range(run *r, uint64_t first, uint64_t last, unsigned mask)
{
    for (uint64_t poll = first; poll <= last; poll++) {
        poll_mask(r, poll, mask);
    }
}

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    char *text = calloc(1u, 1u << 17);
    if (text != NULL) {
        (void)fread(text, 1u, (1u << 17) - 1u, file);
    }
    fclose(file);
    return text;
}

static char *read_in(const run *r, const char *relative)
{
    char path[200];
    snprintf(path, sizeof path, "%s/%s", r->dir, relative);
    return slurp(path);
}

/* flush the writer, close the dump and return the manifest text (caller frees) */
static char *finish_run(run *r, const char *reason)
{
    button_dump_flush(r->dump);
    button_dump_finish(r->dump, reason);
    return read_in(r, "buttons.jsonl");
}

static void destroy_run(run *r)
{
    button_dump_destroy(r->dump);
    r->dump = NULL;
    pthread_mutex_destroy(&r->f.gate);
    pthread_mutex_destroy(&r->f.log_lock);
}

static unsigned count_of(const char *text, const char *needle)
{
    unsigned count = 0u;
    if (text == NULL) {
        return 0u;
    }
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + strlen(needle), needle)) {
        count++;
    }
    return count;
}

/* the manifest line (without the newline) that contains `needle` */
static const char *line_with(const char *text, const char *needle)
{
    static char buffer[16384];
    buffer[0] = '\0';
    if (text == NULL) {
        return buffer;
    }
    const char *at = strstr(text, needle);
    if (at == NULL) {
        return buffer;
    }
    const char *begin = at;
    while (begin > text && begin[-1] != '\n') {
        begin--;
    }
    const char *end = strchr(at, '\n');
    const size_t length = end != NULL ? (size_t)(end - begin) : strlen(begin);
    memcpy(buffer, begin, length < sizeof buffer - 1u ? length : sizeof buffer - 1u);
    buffer[length < sizeof buffer - 1u ? length : sizeof buffer - 1u] = '\0';
    return buffer;
}

static bool exists_in(const run *r, const char *relative)
{
    char path[200];
    snprintf(path, sizeof path, "%s/%s", r->dir, relative);
    return access(path, F_OK) == 0;
}

static unsigned files_in_buttons(const run *r, unsigned *temporaries)
{
    char path[200];
    snprintf(path, sizeof path, "%s/buttons", r->dir);
    DIR *directory = opendir(path);
    unsigned files = 0u;
    *temporaries = 0u;
    if (directory == NULL) {
        return 0u;
    }
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strncmp(entry->d_name, "guestdump.", 10) == 0) {
            files++;
            const size_t length = strlen(entry->d_name);
            if (length > 4u && strcmp(entry->d_name + length - 4u, ".tmp") == 0) {
                (*temporaries)++;
            }
        }
    }
    closedir(directory);
    return files;
}

static long size_of(const run *r, const char *relative)
{
    char path[200];
    snprintf(path, sizeof path, "%s/%s", r->dir, relative);
    struct stat info;
    return stat(path, &info) == 0 ? (long)info.st_size : -1;
}

/* ---- tests ---- */

static void test_mask_names_and_threshold(void)
{
    static const char *const expected_names[16] = {"UP",    "DOWN",  "LEFT", "RIGHT", "START", "BACK", "LTHUMB", "RTHUMB",
                                                   "A",     "B",     "X",    "Y",     "BLACK", "WHITE", "LT",     "RT"};
    static const unsigned expected_game_bits[16] = {0x04u, 0x08u,   0x01u,   0x02u,   0x200u,  0x100u,  0x1000u, 0x2000u,
                                                    0x80u, 0x20u,   0x10u,   0x40u,   0x4000u, 0x8000u, 0x400u,  0x800u};
    for (unsigned index = 0u; index < 16u; index++) {
        CHECK_STR(button_dump_names[index], expected_names[index]);
        CHECK_EQ(button_dump_game_bits[index], expected_game_bits[index]);
    }
    xinput_pad_state state;
    memset(&state, 0, sizeof state);
    CHECK_EQ(button_dump_mask(&state, 60u), 0u);
    /* every digital bit is its own mask bit, bits of the digital word above the 8 buttons are not buttons */
    for (unsigned bit = 0u; bit < 8u; bit++) {
        memset(&state, 0, sizeof state);
        state.digital_buttons = (uint16_t)(1u << bit);
        CHECK_EQ(button_dump_mask(&state, 60u), 1u << bit);
    }
    memset(&state, 0, sizeof state);
    state.digital_buttons = 0xFF00u;
    CHECK_EQ(button_dump_mask(&state, 60u), 0u);
    state.digital_buttons = 0xFFFFu;
    CHECK_EQ(button_dump_mask(&state, 60u), 0x00FFu);
    /* each analog button maps to bit 8 + i, the title's rule is pressure > 60: 60 is not down, 61 is */
    for (unsigned index = 0u; index < 8u; index++) {
        memset(&state, 0, sizeof state);
        state.analog[index] = 60u;
        CHECK_EQ(button_dump_mask(&state, 60u), 0u);
        state.analog[index] = 61u;
        CHECK_EQ(button_dump_mask(&state, 60u), 1u << (8u + index));
        state.analog[index] = 255u;
        CHECK_EQ(button_dump_mask(&state, 60u), 1u << (8u + index));
        state.analog[index] = 0u;
        CHECK_EQ(button_dump_mask(&state, 60u), 0u);
    }
    /* the threshold is a parameter: > T, both ends */
    memset(&state, 0, sizeof state);
    state.analog[7] = 254u;
    CHECK_EQ(button_dump_mask(&state, 254u), 0u);
    state.analog[7] = 255u;
    CHECK_EQ(button_dump_mask(&state, 254u), RT_);
    state.analog[7] = 1u;
    CHECK_EQ(button_dump_mask(&state, 1u), 0u);
    state.analog[7] = 2u;
    CHECK_EQ(button_dump_mask(&state, 1u), RT_);
    /* sticks are never buttons */
    memset(&state, 0, sizeof state);
    state.thumb_left_x = 32767;
    state.thumb_left_y = -32768;
    state.thumb_right_x = 32767;
    state.thumb_right_y = -32768;
    CHECK_EQ(button_dump_mask(&state, 60u), 0u);
    /* everything at once */
    memset(&state, 0, sizeof state);
    state.digital_buttons = 0x00FFu;
    memset(state.analog, 255, sizeof state.analog);
    CHECK_EQ(button_dump_mask(&state, 60u), 0xFFFFu);
}

static void test_create_refuses_bad_configuration(void)
{
    run r;
    button_dump_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.capture = fake_capture;
    ops.write = fake_write;
    ops.discard = fake_discard;
    char error[160] = "";
    button_dump_config config = base_config("bd_bad");
    CHECK(button_dump_create(NULL, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    config.after_count = 0u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL && error[0] != '\0');
    config = base_config("bd_bad");
    config.after_count = 5u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    config.after[1] = 2u; /* not strictly increasing */
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    config.after[0] = 0u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    config.max_pending = 0u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config.max_pending = BUTTON_DUMP_MAX_PENDING_LIMIT + 1u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    config.coalesce = 61u;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    /* the limits themselves are fine */
    config = base_config("bd_good");
    config.coalesce = 60u;
    config.max_pending = BUTTON_DUMP_MAX_PENDING_LIMIT;
    config.after_count = 4u;
    config.after[2] = 31u;
    config.after[3] = 3600u;
    ops.clock = fake_clock;
    ops.user = &r.f;
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.f.gate, NULL);
    pthread_mutex_init(&r.f.log_lock, NULL);
    ops.log = fake_log;
    button_dump *limits = button_dump_create(&config, &ops, error, sizeof error);
    CHECK(limits != NULL);
    button_dump_destroy(limits);
    pthread_mutex_destroy(&r.f.gate);
    pthread_mutex_destroy(&r.f.log_lock);
    ops.clock = NULL;
    ops.log = NULL;
    ops.user = NULL;
    config = base_config(NULL);
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    config = base_config("bd_bad");
    ops.write = NULL;
    CHECK(button_dump_create(&config, &ops, error, sizeof error) == NULL);
    /* a good one for contrast, and the directory is made */
    CHECK(start_run(&r, "good", &config));
    CHECK(exists_in(&r, "buttons.jsonl"));
    char path[100];
    snprintf(path, sizeof path, "%s/buttons", r.dir);
    struct stat info;
    CHECK(stat(path, &info) == 0 && S_ISDIR(info.st_mode));
    poll_mask(&r, 0u, A_);
    free(finish_run(&r, "exit"));
    destroy_run(&r);
    /* a second session in the same directory starts a NEW manifest (one start line, no stale events) */
    g_keep_dir = true;
    CHECK(start_run(&r, "good", &config));
    char *second = finish_run(&r, "exit");
    CHECK(second != NULL);
    CHECK_EQ(count_of(second, "\"type\":\"start\""), 1u);
    CHECK_EQ(count_of(second, "\"type\":\"event\""), 0u);
    CHECK_EQ(count_of(second, "\"type\":\"end\""), 1u);
    free(second);
    g_keep_dir = false;
    destroy_run(&r);
    remove_tree("bd_good");
}

/* exact manifest strings for one press of X held for 40 polls, with the fake clock, present and file sizes */
static void test_manifest_lines_are_exact(void)
{
    run r;
    button_dump_config config = base_config("exact");
    config.after_count = 1u;
    config.after[0] = 2u;
    CHECK(start_run(&r, "exact", &config));
    poll_range(&r, 0u, 6u, 0u);
    poll_range(&r, 7u, 12u, X_);
    poll_range(&r, 13u, 16u, 0u);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_STR(line_with(text, "\"type\":\"start\""),
              "{\"type\":\"start\",\"version\":1,\"wall_start\":\"2026-10-08T12:00:00Z\",\"after_frames\":[2],\"threshold\":60,"
              "\"coalesce_frames\":3,\"start_poll\":0,\"max_dumps\":2000,\"max_bytes\":1073741824,\"max_pending\":8,"
              "\"idle_every\":0,\"ranges\":14,\"bytes_per_dump\":1000,\"button_names\":[\"UP\",\"DOWN\",\"LEFT\",\"RIGHT\","
              "\"START\",\"BACK\",\"LTHUMB\",\"RTHUMB\",\"A\",\"B\",\"X\",\"Y\",\"BLACK\",\"WHITE\",\"LT\",\"RT\"],"
              "\"game_bits\":{\"UP\":4,\"DOWN\":8,\"LEFT\":1,\"RIGHT\":2,\"START\":512,\"BACK\":256,\"LTHUMB\":4096,"
              "\"RTHUMB\":8192,\"A\":128,\"B\":32,\"X\":16,\"Y\":64,\"BLACK\":16384,\"WHITE\":32768,\"LT\":1024,\"RT\":2048},"
              "\"dump_dir\":\"bd_exact\",\"max_idle\":40,\"after_replay\":false,\"forced_state\":false}");
    const long before_bytes = size_of(&r, "buttons/guestdump.0001_press_X_before");
    const long after_bytes = size_of(&r, "buttons/guestdump.0001_press_X_after2");
    CHECK(before_bytes > 0 && after_bytes > 0);
    /* press X at poll 7 (event 1, clock call 1 = +250 ms), after2 captured at poll 9, X still down then (raw_after = poll 8's state) */
    char expected[4096];
    snprintf(expected, sizeof expected,
             "{\"type\":\"event\",\"index\":1,\"edge\":\"press\",\"button\":\"X\",\"buttons\":[\"X\"],\"poll\":7,\"present\":456,"
             "\"mono_ms\":250,\"wall_time\":\"2026-10-08T12:00:00.250Z\",\"mask_before\":\"0x0000\",\"mask_after\":\"0x0400\","
             "\"mask_names_before\":[],\"mask_names_after\":[\"X\"],"
             "\"raw_before\":{\"digital\":\"0x0000\",\"analog\":[0,0,0,0,0,0,0,0]},"
             "\"raw_after\":{\"digital\":\"0x0000\",\"analog\":[0,0,255,0,0,0,0,0]},"
             "\"edges\":[{\"poll\":7,\"edge\":\"press\",\"button\":\"X\"}],\"coalesced\":0,\"dumps\":["
             "{\"kind\":\"before\",\"tag\":\"before\",\"offset\":0,\"poll\":7,\"present\":456,\"label\":\"0001_press_X_before\","
             "\"file\":\"buttons/guestdump.0001_press_X_before\",\"bytes\":%ld,\"ok\":true},"
             "{\"kind\":\"after\",\"tag\":\"after2\",\"offset\":2,\"poll\":9,\"present\":456,\"label\":\"0001_press_X_after2\","
             "\"file\":\"buttons/guestdump.0001_press_X_after2\",\"bytes\":%ld,\"ok\":true}],\"complete\":true}",
             before_bytes, after_bytes);
    CHECK_STR(line_with(text, "\"type\":\"event\""), expected);
    /* the release of X at poll 13 is event 2 (the press has finished all its dumps) */
    CHECK(count_of(text, "\"type\":\"event\"") == 2u);
    CHECK(strstr(line_with(text, "\"index\":2,"),
                 "\"edge\":\"release\",\"button\":\"X\",\"buttons\":[\"X\"],\"poll\":13,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"mask_before\":\"0x0400\",\"mask_after\":\"0x0000\"") != NULL);
    /* end line: 2 events x 2 dumps, no drops. Clock calls: create 0, event 1, event 2, finish = 750 ms */
    long total = 0;
    total += size_of(&r, "buttons/guestdump.0001_press_X_before") + size_of(&r, "buttons/guestdump.0001_press_X_after2");
    total += size_of(&r, "buttons/guestdump.0002_release_X_before") + size_of(&r, "buttons/guestdump.0002_release_X_after2");
    snprintf(expected, sizeof expected,
             "{\"type\":\"end\",\"events\":2,\"idle_events\":0,\"dumps\":4,\"bytes\":%ld,\"dropped\":0,\"coalesced_edges\":0,"
             "\"reason\":\"exit\",\"wall_time\":\"2026-10-08T12:00:00.750Z\"}",
             total);
    CHECK_STR(line_with(text, "\"type\":\"end\""), expected);
    /* the file carries the extra header line, in the text the snapshot write was handed */
    char *before = read_in(&r, "buttons/guestdump.0001_press_X_before");
    CHECK_STR(before, "snapshot 1\n# button-dump event=1 kind=before label=0001_press_X_before poll=7 present=456 offset=0\n");
    char *after = read_in(&r, "buttons/guestdump.0001_press_X_after2");
    CHECK_STR(after, "snapshot 2\n# button-dump event=1 kind=after label=0001_press_X_after2 poll=9 present=456 offset=2\n");
    free(before);
    free(after);
    /* the live counter lines: one per event, then the total */
    CHECK(atomic_load(&r.f.log_count) == 4u);
    CHECK_STR(r.f.log[0], "button dump: event 1 press X poll 7 -> 2 dumps");
    CHECK_STR(r.f.log[1], "button dump: event 2 release X poll 13 -> 2 dumps");
    const char *total_prefix = "button dump: 2 event(s) + 0 idle, 4 dump file(s), ";
    CHECK(strncmp(r.f.log[2], total_prefix, strlen(total_prefix)) == 0);
    const char *cost_prefix = "button dump cost: 4 capture(s) on the guest thread avg ";
    CHECK(strncmp(r.f.log[3], cost_prefix, strlen(cost_prefix)) == 0);
    button_dump_stats cost;
    button_dump_stats_get(r.dump, &cost);
    CHECK_EQ(cost.captures, 4u);
    CHECK_EQ(cost.writes, 4u);
    CHECK(cost.capture_ns_total >= cost.capture_ns_max && cost.write_ns_total >= cost.write_ns_max && cost.write_ns_max > 0u);
    /* finishing twice writes nothing more */
    button_dump_finish(r.dump, "exit");
    char *again = read_in(&r, "buttons.jsonl");
    CHECK(again != NULL && strcmp(again, text) == 0);
    free(again);
    free(text);
    destroy_run(&r);
    remove_tree("bd_exact");
}

/* T1637: the start line says whether the host runs with --forced-state (the owner script pokes with the guarded poke), both values
 * as exact strings. The dump itself behaves the same either way. */
static void test_start_line_says_forced_state(void)
{
    for (unsigned variant = 0u; variant < 2u; variant++) {
        run r;
        button_dump_config config = base_config("forced");
        config.forced_state = variant == 1u;
        config.after_count = 1u;
        config.after[0] = 2u;
        CHECK(start_run(&r, "forced", &config));
        poll_range(&r, 0u, 4u, 0u);
        poll_range(&r, 5u, 12u, X_);
        char *text = finish_run(&r, "exit");
        CHECK(text != NULL);
        CHECK_STR(line_with(text, "\"type\":\"start\""),
                  variant == 1u
                      ? "{\"type\":\"start\",\"version\":1,\"wall_start\":\"2026-10-08T12:00:00Z\",\"after_frames\":[2],\"threshold\":60,"
                        "\"coalesce_frames\":3,\"start_poll\":0,\"max_dumps\":2000,\"max_bytes\":1073741824,\"max_pending\":8,"
                        "\"idle_every\":0,\"ranges\":14,\"bytes_per_dump\":1000,\"button_names\":[\"UP\",\"DOWN\",\"LEFT\",\"RIGHT\","
                        "\"START\",\"BACK\",\"LTHUMB\",\"RTHUMB\",\"A\",\"B\",\"X\",\"Y\",\"BLACK\",\"WHITE\",\"LT\",\"RT\"],"
                        "\"game_bits\":{\"UP\":4,\"DOWN\":8,\"LEFT\":1,\"RIGHT\":2,\"START\":512,\"BACK\":256,\"LTHUMB\":4096,"
                        "\"RTHUMB\":8192,\"A\":128,\"B\":32,\"X\":16,\"Y\":64,\"BLACK\":16384,\"WHITE\":32768,\"LT\":1024,\"RT\":2048},"
                        "\"dump_dir\":\"bd_forced\",\"max_idle\":40,\"after_replay\":false,\"forced_state\":true}"
                      : "{\"type\":\"start\",\"version\":1,\"wall_start\":\"2026-10-08T12:00:00Z\",\"after_frames\":[2],\"threshold\":60,"
                        "\"coalesce_frames\":3,\"start_poll\":0,\"max_dumps\":2000,\"max_bytes\":1073741824,\"max_pending\":8,"
                        "\"idle_every\":0,\"ranges\":14,\"bytes_per_dump\":1000,\"button_names\":[\"UP\",\"DOWN\",\"LEFT\",\"RIGHT\","
                        "\"START\",\"BACK\",\"LTHUMB\",\"RTHUMB\",\"A\",\"B\",\"X\",\"Y\",\"BLACK\",\"WHITE\",\"LT\",\"RT\"],"
                        "\"game_bits\":{\"UP\":4,\"DOWN\":8,\"LEFT\":1,\"RIGHT\":2,\"START\":512,\"BACK\":256,\"LTHUMB\":4096,"
                        "\"RTHUMB\":8192,\"A\":128,\"B\":32,\"X\":16,\"Y\":64,\"BLACK\":16384,\"WHITE\":32768,\"LT\":1024,\"RT\":2048},"
                        "\"dump_dir\":\"bd_forced\",\"max_idle\":40,\"after_replay\":false,\"forced_state\":false}");
        CHECK_EQ(count_of(text, "\"forced_state\""), 1u);
        CHECK_EQ(count_of(text, "\"type\":\"event\""), 1u); /* same behaviour either way */
        free(text);
        destroy_run(&r);
    }
    remove_tree("bd_forced");
}

static void test_edge_detection_and_labels(void)
{
    run r;
    button_dump_config config = base_config("edges");
    config.coalesce = 0u;
    config.after_count = 1u;
    config.after[0] = 1u;
    CHECK(start_run(&r, "edges", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_ | B_);            /* simultaneous presses: ONE event, buttons in bit order */
    button_dump_flush(r.dump);
    CHECK_EQ(atomic_load(&r.f.writes), 1u); /* N = 0: the before dump is named and queued in the poll of the edge */
    poll_range(&r, 6u, 8u, A_ | B_);       /* held: no edge */
    poll_mask(&r, 9u, B_);                 /* release A */
    poll_mask(&r, 10u, B_ | LT_);          /* press LT */
    poll_mask(&r, 11u, Y_ | LT_);          /* release B and press Y in the same poll: mixed */
    poll_mask(&r, 12u, 0u);                /* release both */
    poll_mask(&r, 13u, RIGHT | START);     /* digital presses */
    poll_mask(&r, 14u, 0u);
    poll_range(&r, 15u, 20u, 0u);
    button_dump_stats stats;
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 7u);
    CHECK_EQ(stats.dropped, 0u);
    CHECK_EQ(stats.dumps_written, 14u);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_after1"));
    CHECK(exists_in(&r, "buttons/guestdump.0002_release_A_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0003_press_LT_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0004_mixed_B-Y_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0005_release_Y-LT_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0006_press_RIGHT-START_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0007_release_RIGHT-START_before"));
    /* event 1: the simultaneous presses are two edges listed in bit order, none coalesced */
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"edge\":\"press\",\"button\":\"A\",\"buttons\":[\"A\",\"B\"],\"poll\":5,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"edges\":[{\"poll\":5,\"edge\":\"press\",\"button\":\"A\"},{\"poll\":5,\"edge\":\"press\",\"button\":\"B\"}],"
                 "\"coalesced\":0,") != NULL);
    /* event 4: mixed, "button" is the first edge in bit order (B is bit 9, Y bit 11), edges say which way each went */
    CHECK(strstr(line_with(text, "\"index\":4,"), "\"edge\":\"mixed\",\"button\":\"B\",\"buttons\":[\"B\",\"Y\"],\"poll\":11,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":4,"),
                 "\"edges\":[{\"poll\":11,\"edge\":\"release\",\"button\":\"B\"},{\"poll\":11,\"edge\":\"press\",\"button\":\"Y\"}]") !=
          NULL);
    CHECK(strstr(line_with(text, "\"index\":4,"), "\"mask_names_before\":[\"B\",\"LT\"],\"mask_names_after\":[\"Y\",\"LT\"]") != NULL);
    /* event 4 opens at the 4th clock call after the start: +1000 ms, the milliseconds are zero padded to 3 digits */
    CHECK(strstr(line_with(text, "\"index\":4,"), "\"mono_ms\":1000,\"wall_time\":\"2026-10-08T12:00:01.000Z\",") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"mono_ms\":250,\"wall_time\":\"2026-10-08T12:00:00.250Z\",") != NULL);
    /* every one of the 7 events is in the manifest, complete */
    CHECK_EQ(count_of(text, "\"type\":\"event\""), 7u);
    CHECK_EQ(count_of(text, "\"complete\":true"), 7u);
    free(text);
    destroy_run(&r);
    remove_tree("bd_edges");
}

static void test_hold_at_arming_is_not_an_edge(void)
{
    run r;
    button_dump_config config = base_config("arming");
    config.coalesce = 0u;
    config.after_count = 1u;
    config.after[0] = 1u;
    config.start_poll = 5u;
    CHECK(start_run(&r, "arming", &config));
    poll_mask(&r, 0u, 0u);
    poll_mask(&r, 1u, X_);      /* press before the start poll: no event */
    poll_range(&r, 2u, 6u, X_); /* still held when events start at 5: no phantom press */
    button_dump_stats stats;
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 0u);
    poll_mask(&r, 7u, 0u); /* the release IS an edge */
    poll_range(&r, 8u, 10u, 0u);
    poll_mask(&r, 11u, A_); /* a press at/after start */
    poll_range(&r, 12u, 14u, A_);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    CHECK(exists_in(&r, "buttons/guestdump.0001_release_X_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0002_press_A_before"));
    CHECK(!exists_in(&r, "buttons/guestdump.0001_press_X_before"));
    CHECK(strstr(line_with(text, "\"type\":\"start\""), "\"start_poll\":5,") != NULL);
    free(text);
    destroy_run(&r);

    /* the start poll itself is the first poll that may open an event, the poll before is not */
    config.start_poll = 10u;
    CHECK(start_run(&r, "arming", &config));
    poll_mask(&r, 9u, X_);
    poll_mask(&r, 10u, 0u);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u); /* poll 9 press ignored, poll 10 release admitted */
    poll_mask(&r, 11u, Y_);
    poll_mask(&r, 12u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 3u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edge\":\"release\",\"button\":\"X\",\"buttons\":[\"X\"],\"poll\":10,") != NULL);
    free(text);
    destroy_run(&r);

    /* after-replay with no armed query at all never arms */
    config.start_poll = 0u;
    config.after_replay = true;
    g_omit_armed_query = true;
    CHECK(start_run(&r, "arming", &config));
    poll_range(&r, 0u, 3u, 0u);
    poll_range(&r, 4u, 8u, A_);
    poll_range(&r, 9u, 12u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 0u);
    free(finish_run(&r, "exit"));
    destroy_run(&r);
    g_omit_armed_query = false;
    /* --dump-button-after-replay: the armed query gates the events and a button held at the handover is no press */
    config.start_poll = 0u;
    config.after_replay = true;
    CHECK(start_run(&r, "arming", &config));
    r.f.armed = false;
    poll_mask(&r, 0u, A_);
    poll_mask(&r, 1u, 0u);
    poll_mask(&r, 2u, B_); /* B pressed during the replay and held through the handover */
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 0u);
    r.f.armed = true;
    poll_range(&r, 3u, 6u, B_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 0u);
    poll_mask(&r, 7u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"type\":\"start\""), "\"after_replay\":true,\"forced_state\":false}") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edge\":\"release\",\"button\":\"B\"") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_arming");
}

static void test_coalescing_boundary(void)
{
    run r;
    button_dump_config config = base_config("coalesce");
    config.after_count = 1u;
    config.after[0] = 5u;
    config.coalesce = 3u;
    CHECK(start_run(&r, "coalesce", &config));
    poll_range(&r, 0u, 9u, 0u);
    poll_mask(&r, 10u, A_);            /* event 1 opens, first edge poll 10 */
    poll_mask(&r, 11u, A_);
    poll_mask(&r, 12u, A_);
    poll_mask(&r, 13u, A_ | B_);       /* 13 - 10 = N: merged into event 1 */
    poll_mask(&r, 14u, A_ | B_ | X_);  /* 14 - 10 = N + 1: a NEW event 2 */
    poll_range(&r, 15u, 40u, A_ | B_ | X_);
    button_dump_stats stats;
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.coalesced_edges, 1u);
    CHECK_EQ(stats.dumps_committed, 4u);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    /* the merged edge is in event 1's list and in its label, and event 1 got no extra dumps */
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_after5"));
    CHECK(exists_in(&r, "buttons/guestdump.0002_press_X_before"));
    CHECK(!exists_in(&r, "buttons/guestdump.0001_press_A_before"));
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"buttons\":[\"A\",\"B\"],\"poll\":10,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"edges\":[{\"poll\":10,\"edge\":\"press\",\"button\":\"A\"},{\"poll\":13,\"edge\":\"press\",\"button\":\"B\"}],"
                 "\"coalesced\":1,\"dumps\":[") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"poll\":14,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"coalesced\":0,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"coalesced_edges\":1,") != NULL);
    free(text);
    destroy_run(&r);

    /* N = 0 never merges, edges of the SAME poll are always one event */
    config.coalesce = 0u;
    CHECK(start_run(&r, "coalesce", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_ | B_);
    poll_mask(&r, 6u, A_ | B_ | X_);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.coalesced_edges, 0u);
    text = finish_run(&r, "exit");
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0002_press_X_before"));
    free(text);
    destroy_run(&r);

    /* the edge that merges can be a release: the event becomes mixed, a release alone after a press is mixed too */
    config.coalesce = 3u;
    CHECK(start_run(&r, "coalesce", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_mask(&r, 6u, 0u);
    poll_range(&r, 7u, 30u, 0u);
    text = finish_run(&r, "exit");
    CHECK(exists_in(&r, "buttons/guestdump.0001_mixed_A_before"));
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"edges\":[{\"poll\":5,\"edge\":\"press\",\"button\":\"A\"},{\"poll\":6,\"edge\":\"release\",\"button\":\"A\"}],"
                 "\"coalesced\":1,") != NULL);
    free(text);
    destroy_run(&r);

    /* more edges than the list holds: the list is cut at 64, the count is not */
    config.coalesce = 60u;
    config.after[0] = 1u;
    CHECK(start_run(&r, "coalesce", &config));
    poll_range(&r, 0u, 4u, 0u);
    for (uint64_t poll = 5u; poll < 5u + 40u; poll++) {
        poll_mask(&r, poll, ((poll - 5u) % 2u == 0u) ? (A_ | B_) : 0u);
    }
    poll_range(&r, 45u, 120u, 0u);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.coalesced_edges, 78u);
    text = finish_run(&r, "exit");
    const char *line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"coalesced\":78,") != NULL);
    CHECK_EQ(count_of(line, "\"edge\":\"press\",\"button\"") + count_of(line, "\"edge\":\"release\",\"button\""), 64u);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"coalesced_edges\":78,") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_coalesce");
}

static void test_after_offsets_and_skipped_polls(void)
{
    run r;
    button_dump_config config = base_config("after");
    config.coalesce = 0u;
    CHECK(start_run(&r, "after", &config)); /* after = 2, 30 */
    poll_range(&r, 0u, 6u, 0u);
    poll_mask(&r, 7u, X_);
    poll_range(&r, 8u, 50u, X_);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    const char *line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"tag\":\"before\",\"offset\":0,\"poll\":7,") != NULL);
    CHECK(strstr(line, "\"tag\":\"after2\",\"offset\":2,\"poll\":9,") != NULL);
    CHECK(strstr(line, "\"tag\":\"after30\",\"offset\":30,\"poll\":37,") != NULL);
    CHECK(strstr(line, "\"complete\":true") != NULL);
    free(text);
    destroy_run(&r);

    /* polls the source skipped: an after dump is taken at the first poll >= its due poll */
    CHECK(start_run(&r, "after", &config));
    poll_range(&r, 0u, 6u, 0u);
    poll_mask(&r, 7u, X_);
    poll_mask(&r, 8u, X_);
    poll_mask(&r, 12u, X_); /* due 9, polls 9..11 never came */
    poll_mask(&r, 13u, X_);
    poll_mask(&r, 100u, X_); /* due 37, long gone */
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"tag\":\"after2\",\"offset\":2,\"poll\":12,") != NULL);
    CHECK(strstr(line, "\"tag\":\"after30\",\"offset\":30,\"poll\":100,") != NULL);
    CHECK(strstr(line, "\"complete\":true") != NULL);
    free(text);
    destroy_run(&r);

    /* two offsets due in the same poll are two captures and two files */
    CHECK(start_run(&r, "after", &config));
    poll_range(&r, 0u, 6u, 0u);
    poll_mask(&r, 7u, X_);
    poll_mask(&r, 200u, X_);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"tag\":\"after2\",\"offset\":2,\"poll\":200,") != NULL);
    CHECK(strstr(line, "\"tag\":\"after30\",\"offset\":30,\"poll\":200,") != NULL);
    CHECK_EQ(atomic_load(&r.f.captures), 3u);
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_X_after2") && exists_in(&r, "buttons/guestdump.0001_press_X_after30"));
    free(text);
    destroy_run(&r);

    /* four offsets, the longest 3600: the event waits for all of them and the order of the files is the order of the offsets */
    config.after_count = 4u;
    config.after[0] = 1u;
    config.after[1] = 2u;
    config.after[2] = 3u;
    config.after[3] = 3600u;
    CHECK(start_run(&r, "after", &config));
    poll_mask(&r, 0u, 0u);
    poll_mask(&r, 1u, X_);
    poll_range(&r, 2u, 3600u, X_);
    button_dump_flush(r.dump);
    CHECK_EQ(atomic_load(&r.f.writes), 4u); /* before, after1, after2, after3 so far */
    poll_range(&r, 3601u, 3605u, X_);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"tag\":\"after3600\",\"offset\":3600,\"poll\":3601,") != NULL);
    CHECK_EQ(count_of(line, "\"kind\":\"after\""), 4u);
    CHECK(strstr(line, "\"complete\":true") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_after");
}

static void test_caps(void)
{
    run r;
    button_dump_stats stats;
    char *text;
    /* max-pending: an edge while the event waits for its after dump is dropped, counted, and logged as a manifest line */
    button_dump_config config = base_config("caps");
    config.coalesce = 0u;
    config.after_count = 1u;
    config.after[0] = 5u;
    config.max_pending = 1u;
    CHECK(start_run(&r, "caps", &config));
    poll_range(&r, 0u, 9u, 0u);
    poll_mask(&r, 10u, A_);
    poll_mask(&r, 12u, A_ | B_); /* dropped: the one pending event is waiting until poll 15 */
    poll_range(&r, 13u, 14u, A_ | B_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.dropped, 1u);
    poll_range(&r, 15u, 16u, A_ | B_);
    poll_mask(&r, 17u, B_); /* A released after event 1 finished: admitted, event index 2 (the dropped edge used none) */
    poll_range(&r, 18u, 30u, B_);
    text = finish_run(&r, "exit");
    CHECK(text != NULL);
    CHECK_STR(line_with(text, "\"type\":\"dropped\""),
              "{\"type\":\"dropped\",\"poll\":12,\"reason\":\"max_pending\",\"edges\":[{\"poll\":12,\"edge\":\"press\","
              "\"button\":\"B\"}],\"mono_ms\":500}");
    CHECK(exists_in(&r, "buttons/guestdump.0002_release_A_before"));
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"events\":2,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"dropped\":1,") != NULL);
    free(text);
    destroy_run(&r);

    /* max-dumps: all of an event's dumps fit or the edge is dropped, never a partial event (3 per event, cap 7 -> 2 events) */
    config = base_config("caps");
    config.coalesce = 0u;
    config.max_dumps = 7u;
    CHECK(start_run(&r, "caps", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_mask(&r, 40u, 0u);
    poll_mask(&r, 80u, B_); /* third event: 6 committed + 3 > 7 */
    poll_mask(&r, 81u, 0u); /* and its release too, nothing was admitted */
    poll_range(&r, 82u, 150u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.dropped, 2u);
    CHECK_EQ(stats.dumps_committed, 6u);
    text = finish_run(&r, "exit");
    CHECK_EQ(count_of(text, "\"reason\":\"max_dumps\""), 2u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.dumps_written, 6u);
    unsigned temporaries = 0u;
    CHECK_EQ(files_in_buttons(&r, &temporaries), 6u);
    free(text);
    destroy_run(&r);

    /* max-dumps exactly fitting: 6 allowed, two full events, the third refused */
    config.max_dumps = 6u;
    CHECK(start_run(&r, "caps", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_mask(&r, 40u, 0u);
    poll_mask(&r, 80u, B_);
    poll_range(&r, 81u, 150u, B_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.dropped, 1u);
    text = finish_run(&r, "exit");
    free(text);
    destroy_run(&r);

    /* max-bytes with the per dump estimate: 1000 per dump, 3 per event, 2500 allows two events (600... 3000 > 2500 for the 2nd) */
    config = base_config("caps");
    config.coalesce = 0u;
    config.bytes_per_dump = 1000u;
    config.max_bytes = 5000u;
    CHECK(start_run(&r, "caps", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);   /* 3000 committed */
    poll_mask(&r, 40u, 0u);  /* 6000 > 5000: dropped */
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.dropped, 1u);
    poll_range(&r, 41u, 100u, 0u);
    text = finish_run(&r, "exit");
    CHECK_STR(line_with(text, "\"type\":\"dropped\""),
              "{\"type\":\"dropped\",\"poll\":40,\"reason\":\"max_bytes\",\"edges\":[{\"poll\":40,\"edge\":\"release\","
              "\"button\":\"A\"}],\"mono_ms\":500}");
    free(text);
    destroy_run(&r);
    /* 6000 allowed: exactly two events fit */
    config.max_bytes = 6000u;
    CHECK(start_run(&r, "caps", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_mask(&r, 40u, 0u);
    poll_mask(&r, 80u, B_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.dropped, 1u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"type\":\"dropped\""), "\"reason\":\"max_bytes\"") != NULL);
    free(text);
    destroy_run(&r);

    /* queue_full: snapshots queued for the writer are bounded. The writer is stalled, the limit holds one event (3 x 1000) */
    config = base_config("caps");
    config.coalesce = 0u;
    config.queue_limit = 4000u;
    CHECK(start_run(&r, "caps", &config));
    pthread_mutex_lock(&r.f.gate);
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);   /* reserves 3000 */
    poll_mask(&r, 6u, 0u);   /* 6000 > 4000: queue_full */
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.dropped, 1u);
    pthread_mutex_unlock(&r.f.gate);
    poll_range(&r, 7u, 60u, 0u);
    button_dump_flush(r.dump);
    poll_mask(&r, 61u, B_); /* the first event is entirely written: its reservation is back, admitted */
    poll_range(&r, 62u, 100u, B_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 2u);
    CHECK_EQ(stats.dropped, 1u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"type\":\"dropped\""), "\"reason\":\"queue_full\"") != NULL);
    CHECK_EQ(atomic_load(&r.f.discards), atomic_load(&r.f.captures));
    free(text);
    destroy_run(&r);

    /* a queue limit below one event is raised to one event, so events are still possible */
    config.queue_limit = 10u;
    CHECK(start_run(&r, "caps", &config));
    poll_mask(&r, 0u, A_);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    text = finish_run(&r, "exit");
    free(text);
    destroy_run(&r);
    remove_tree("bd_caps");
}

static void test_idle_controls(void)
{
    run r;
    button_dump_stats stats;
    button_dump_config config = base_config("idle");
    config.coalesce = 3u;
    config.after_count = 1u;
    config.after[0] = 2u;
    config.idle_every = 10u;
    config.max_idle = 2u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 100u, 0u);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.idle_events, 2u); /* at most max_idle, at polls 10 and 20 */
    CHECK_EQ(stats.events, 0u);
    char *text = finish_run(&r, "exit");
    CHECK(text != NULL);
    CHECK(exists_in(&r, "buttons/guestdump.0001_idle_none_before"));
    CHECK(exists_in(&r, "buttons/guestdump.0001_idle_none_after2"));
    CHECK(exists_in(&r, "buttons/guestdump.0002_idle_none_before"));
    CHECK(strstr(line_with(text, "\"index\":1,"),
                 "\"edge\":\"idle\",\"button\":\"none\",\"buttons\":[],\"poll\":10,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edges\":[],\"coalesced\":0,\"dumps\":[{\"kind\":\"idle-before\",\"tag\":\"before\"") !=
          NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"), "{\"kind\":\"idle-after\",\"tag\":\"after2\",\"offset\":2,\"poll\":12,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"poll\":20,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"events\":0,\"idle_events\":2,\"dumps\":4,") != NULL);
    free(text);
    destroy_run(&r);

    /* an edge lately blocks the idle control (quiet = last after + 2 = 4 polls) and an edge event pending blocks it too */
    config.max_idle = 40u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 17u, 0u);
    poll_mask(&r, 18u, A_);  /* edge at 18: poll 20 is 2 polls later, blocked, next idle due 30 */
    poll_range(&r, 19u, 60u, A_);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    /* idle at 10, [20 blocked], 30, 40, 50, 60: 5 idle events, 1 edge event, shared index counter */
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.idle_events, 5u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"edge\":\"press\""), "\"index\":2,") != NULL); /* idle 1 at 10, the press is event 2 */
    CHECK(strstr(line_with(text, "\"index\":3,"), "\"edge\":\"idle\"") != NULL);
    CHECK(strstr(line_with(text, "\"index\":3,"), "\"poll\":30,") != NULL);
    free(text);
    destroy_run(&r);

    /* idle off by default, and the idle events respect the dump cap (3rd needs room, 1 + 1 dump each) */
    config.idle_every = 0u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 200u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.idle_events, 0u);
    text = finish_run(&r, "exit");
    free(text);
    destroy_run(&r);
    config.idle_every = 10u;
    config.max_dumps = 4u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 200u, 0u);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.idle_events, 2u);
    CHECK_EQ(stats.dropped, 0u); /* a refused idle control is not a dropped edge */
    text = finish_run(&r, "exit");
    free(text);
    destroy_run(&r);

    /* --dump-button-after-replay: the idle clock starts when the replay hands over (poll 50 -> first idle at 60) */
    config.max_dumps = 2000u;
    config.after_replay = true;
    CHECK(start_run(&r, "idle", &config));
    r.f.armed = false;
    poll_range(&r, 0u, 49u, 0u);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.idle_events, 0u);
    r.f.armed = true;
    poll_range(&r, 50u, 75u, 0u);
    button_dump_flush(r.dump);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edge\":\"idle\",\"button\":\"none\",\"buttons\":[],\"poll\":60,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"poll\":70,") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_idle");

    /* the quiet window after an edge is max(after) + 2 = 32 polls with after = 2,30, inclusive: the edge at 18, its event
     * complete at 48. A control due at 50 (32 later) is skipped and retried at 100, one due at 51 (33 later) is taken. */
    for (unsigned variant = 0u; variant < 2u; variant++) {
        config = base_config("idle");
        config.coalesce = 3u;
        config.idle_every = variant == 0u ? 50u : 51u;
        CHECK(start_run(&r, "idle", &config));
        poll_range(&r, 0u, 17u, 0u);
        poll_range(&r, 18u, 140u, A_);
        button_dump_flush(r.dump);
        button_dump_stats_get(r.dump, &stats);
        CHECK_EQ(stats.events, 1u);
        CHECK_EQ(stats.idle_events, variant == 0u ? 1u : 2u);
        text = finish_run(&r, "exit");
        CHECK(strstr(line_with(text, "\"index\":2,"), variant == 0u ? "\"edge\":\"idle\",\"button\":\"none\",\"buttons\":[],\"poll\":100," : "\"edge\":\"idle\",\"button\":\"none\",\"buttons\":[],\"poll\":51,") != NULL);
        free(text);
        destroy_run(&r);
    }
    /* an event still open (its coalesce window, 10 polls) blocks the control even when the edge is older than the quiet window */
    config = base_config("idle");
    config.coalesce = 10u;
    config.after_count = 1u;
    config.after[0] = 2u;
    config.idle_every = 15u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 7u, 0u);
    poll_range(&r, 8u, 40u, A_);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.idle_events, 1u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"edge\":\"idle\",\"button\":\"none\",\"buttons\":[],\"poll\":30,") != NULL);
    free(text);
    destroy_run(&r);
    /* an idle event is never a merge target: a press right after it is an event of its own */
    config = base_config("idle");
    config.coalesce = 3u;
    config.after_count = 1u;
    config.after[0] = 2u;
    config.idle_every = 10u;
    config.max_idle = 1u;
    CHECK(start_run(&r, "idle", &config));
    poll_range(&r, 0u, 10u, 0u);
    poll_range(&r, 11u, 30u, A_);
    button_dump_flush(r.dump);
    button_dump_stats_get(r.dump, &stats);
    CHECK_EQ(stats.events, 1u);
    CHECK_EQ(stats.idle_events, 1u);
    CHECK_EQ(stats.coalesced_edges, 0u);
    text = finish_run(&r, "exit");
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edge\":\"idle\",") != NULL);
    CHECK(strstr(line_with(text, "\"index\":1,"), "\"edges\":[],\"coalesced\":0,") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"edge\":\"press\",\"button\":\"A\",\"buttons\":[\"A\"],\"poll\":11,") != NULL);
    CHECK(exists_in(&r, "buttons/guestdump.0002_press_A_before"));
    free(text);
    destroy_run(&r);
    remove_tree("bd_idle");
}

static void test_shutdown_and_failures(void)
{
    run r;
    button_dump_config config = base_config("shutdown");
    config.coalesce = 5u;
    CHECK(start_run(&r, "shutdown", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_mask(&r, 6u, A_ | B_);
    poll_range(&r, 7u, 8u, A_ | B_); /* the coalesce window is still open, after2 was taken at 7 and is held unnamed */
    char *text = finish_run(&r, "signal");
    CHECK(text != NULL);
    /* the event is closed incomplete: named with both buttons, the dumps that exist are listed, after30 never happened */
    const char *line = line_with(text, "\"type\":\"event\"");
    CHECK(strstr(line, "\"buttons\":[\"A\",\"B\"]") != NULL);
    CHECK(strstr(line, "\"complete\":false}") != NULL);
    CHECK_EQ(count_of(line, "\"kind\":\""), 2u);
    CHECK(exists_in(&r, "buttons/guestdump.0001_press_A-B_before") && exists_in(&r, "buttons/guestdump.0001_press_A-B_after2"));
    CHECK(!exists_in(&r, "buttons/guestdump.0001_press_A-B_after30"));
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"reason\":\"signal\"") != NULL);
    CHECK(strstr(text, "\"type\":\"end\"") > strstr(text, "\"type\":\"event\""));
    /* polls after the end are ignored */
    poll_mask(&r, 9u, 0u);
    CHECK_EQ(atomic_load(&r.f.captures), 2u);
    unsigned temporaries = 0u;
    CHECK_EQ(files_in_buttons(&r, &temporaries), 2u);
    CHECK_EQ(temporaries, 0u);
    free(text);
    destroy_run(&r);

    /* a capture that fails and a write that fails are listed as not ok, the run goes on */
    config = base_config("shutdown");
    config.coalesce = 0u;
    config.after_count = 1u;
    config.after[0] = 1u;
    CHECK(start_run(&r, "shutdown", &config));
    r.f.fail_capture = 1u;     /* capture 1 = the before dump of event 1: no snapshot at all */
    r.f.unreadable_write = 1u; /* write 1 = event 1 after1: a file with unreadable ranges, ok false but it exists */
    r.f.fail_write = 2u;       /* write 2 = event 2 before: no file */
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, A_);
    poll_range(&r, 6u, 9u, A_);
    poll_mask(&r, 10u, 0u);
    poll_range(&r, 11u, 20u, 0u);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"label\":\"0001_press_A_before\",\"file\":\"buttons/guestdump.0001_press_A_before\",\"bytes\":0,\"ok\":false}") !=
          NULL);
    CHECK(!exists_in(&r, "buttons/guestdump.0001_press_A_before"));
    const long unreadable_size = size_of(&r, "buttons/guestdump.0001_press_A_after1");
    CHECK(unreadable_size > 0);
    char needle[200];
    snprintf(needle, sizeof needle, "\"label\":\"0001_press_A_after1\",\"file\":\"buttons/guestdump.0001_press_A_after1\",\"bytes\":%ld,\"ok\":false}",
             unreadable_size);
    CHECK(strstr(line, needle) != NULL);
    CHECK(strstr(line, "\"complete\":true}") != NULL);
    line = line_with(text, "\"index\":2,");
    CHECK(strstr(line, "\"label\":\"0002_release_A_before\",\"file\":\"buttons/guestdump.0002_release_A_before\",\"bytes\":0,\"ok\":false}") !=
          NULL);
    CHECK(!exists_in(&r, "buttons/guestdump.0002_release_A_before"));
    CHECK(strstr(line, "\"label\":\"0002_release_A_after1\"") != NULL);
    CHECK(strstr(line, "\"ok\":true}") != NULL);
    CHECK(strstr(line, "\"complete\":true}") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"dumps\":2,") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_shutdown");
}


/* An event closed at exit before any after dump was captured shows the state it OPENED with, not the state before the edge. */
static void test_event_closed_before_any_after_dump(void)
{
    run r;
    button_dump_config config = base_config("early");
    config.coalesce = 0u;
    CHECK(start_run(&r, "early", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, X_ | LT_);
    char *text = finish_run(&r, "exit"); /* after2 would be due at poll 7 */
    CHECK(text != NULL);
    const char *line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"mask_before\":\"0x0000\",\"mask_after\":\"0x4400\",") != NULL);
    CHECK(strstr(line, "\"mask_names_before\":[],\"mask_names_after\":[\"X\",\"LT\"],") != NULL);
    CHECK(strstr(line, "\"raw_before\":{\"digital\":\"0x0000\",\"analog\":[0,0,0,0,0,0,0,0]},") != NULL);
    CHECK(strstr(line, "\"raw_after\":{\"digital\":\"0x0000\",\"analog\":[0,0,255,0,0,0,255,0]},") != NULL);
    CHECK(strstr(line, "\"complete\":false}") != NULL);
    free(text);
    destroy_run(&r);
    /* a release closed early: mask_after is the empty mask, raw_after the digital word without the bit */
    CHECK(start_run(&r, "early", &config));
    poll_range(&r, 0u, 4u, START | A_);
    poll_mask(&r, 5u, A_);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":2,");
    CHECK(strstr(line, "\"edge\":\"release\",\"button\":\"START\"") != NULL);
    CHECK(strstr(line, "\"mask_before\":\"0x0110\",\"mask_after\":\"0x0100\",") != NULL);
    CHECK(strstr(line, "\"raw_after\":{\"digital\":\"0x0000\",\"analog\":[255,0,0,0,0,0,0,0]},") != NULL);
    free(text);
    destroy_run(&r);
    /* a digital press closed early: the raw digital word of the state it opened with */
    CHECK(start_run(&r, "early", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, START | RIGHT);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"raw_after\":{\"digital\":\"0x0018\",\"analog\":[0,0,0,0,0,0,0,0]},") != NULL);
    free(text);
    destroy_run(&r);
    /* once an after dump is taken, mask_after / raw_after are the guest-visible state THEN (poll 7's, not the opening poll's) */
    config.after_count = 1u;
    config.after[0] = 3u;
    CHECK(start_run(&r, "early", &config));
    poll_range(&r, 0u, 4u, 0u);
    poll_mask(&r, 5u, X_);
    poll_range(&r, 6u, 12u, 0u);
    text = finish_run(&r, "exit");
    line = line_with(text, "\"index\":1,");
    CHECK(strstr(line, "\"mask_before\":\"0x0000\",\"mask_after\":\"0x0000\",") != NULL);
    CHECK(strstr(line, "\"raw_after\":{\"digital\":\"0x0000\",\"analog\":[0,0,0,0,0,0,0,0]},") != NULL);
    CHECK(strstr(line, "\"complete\":true}") != NULL);
    free(text);
    destroy_run(&r);
    remove_tree("bd_early");
}

/* A flood of drops: at most 200 "dropped" lines, then ONE suppressed marker, the counters keep the total. */
static void test_drop_lines_are_capped(void)
{
    run r;
    button_dump_stats stats;
    button_dump_config config = base_config("flood");
    config.coalesce = 0u;
    config.max_dumps = 3u; /* one event of three dumps, every later edge is dropped */
    for (unsigned total = 199u; total <= 250u; total += (total == 199u ? 1u : (total == 200u ? 1u : 49u))) {
        CHECK(start_run(&r, "flood", &config));
        poll_range(&r, 0u, 4u, 0u);
        poll_mask(&r, 5u, A_); /* event 1 */
        for (unsigned toggle = 1u; toggle <= total; toggle++) {
            poll_mask(&r, 5u + toggle, (toggle % 2u == 1u) ? 0u : A_); /* edge number `toggle` at poll 5 + toggle */
        }
        button_dump_flush(r.dump);
        button_dump_stats_get(r.dump, &stats);
        CHECK_EQ(stats.dropped, total);
        char *text = finish_run(&r, "exit");
        CHECK(text != NULL);
        const unsigned lines = count_of(text, "\"type\":\"dropped\"");
        const unsigned markers = count_of(text, "\"reason\":\"suppressed\"");
        char end_dropped[48];
        snprintf(end_dropped, sizeof end_dropped, "\"dropped\":%u,", total);
        CHECK(strstr(line_with(text, "\"type\":\"end\""), end_dropped) != NULL); /* the end line is the TOTAL */
        if (total <= 200u) {
            CHECK_EQ(lines, total); /* exactly 200 still gets its line, and no marker */
            CHECK_EQ(markers, 0u);
        } else {
            CHECK_EQ(lines, 201u); /* 200 lines + the marker */
            CHECK_EQ(markers, 1u);
            /* the marker is the 201st drop: poll 5 + 201, the clock calls were create 0, event 1, drops 1..200, marker 202 */
            CHECK_STR(line_with(text, "\"reason\":\"suppressed\""),
                      "{\"type\":\"dropped\",\"poll\":206,\"reason\":\"suppressed\",\"edges\":[],\"mono_ms\":50500}");
            /* it comes after the 200th real line and before the end line */
            const char *marker = strstr(text, "\"reason\":\"suppressed\"");
            const char *last_real = strstr(text, "\"poll\":205,\"reason\":\"max_dumps\"");
            CHECK(last_real != NULL && marker != NULL && marker > last_real);
            CHECK(strstr(text, "\"poll\":206,\"reason\":\"max_dumps\"") == NULL); /* the 201st drop has no ordinary line */
            CHECK(strstr(text, "\"type\":\"end\"") > marker);
        }
        free(text);
        destroy_run(&r);
    }
    remove_tree("bd_flood");
}

/* ---- the real wiring: pre install hook, guest snapshots, real files ---- */

static kernel_guest_ptr prepared_region(uint8_t seed, kernel_guest_ptr fixed_base)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.fixed_base = fixed_base;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    if (base != 0u) {
        uint8_t bytes[0x2000];
        for (unsigned index = 0u; index < sizeof bytes; index++) {
            bytes[index] = (uint8_t)(seed + index * 7u);
        }
        CHECK(kernel_guest_write_bytes(base, bytes, sizeof bytes));
    }
    return base;
}

static guest_dump_set g_wired_set;
static kernel_guest_ptr g_wired_base;
static unsigned g_wired_script[64];
static unsigned g_wired_count;
static unsigned g_wired_hook_calls;
static bool g_wired_mutate;
static bool g_wired_armed;

static bool wired_armed(void)
{
    return g_wired_armed;
}

static bool wired_source(uint64_t poll, xinput_pad_state *out, void *user)
{
    (void)user;
    *out = state_of(poll < g_wired_count ? g_wired_script[poll] : 0u);
    if (poll >= 30u && poll < 33u) {
        out->analog[1] = 50u; /* B at pressure 50: below the title's threshold 60, not a button */
    }
    /* the game "reacts" to the input of poll 6 in the memory the dump set covers: bytes change from poll 7 on */
    if (g_wired_mutate && poll == 7u) {
        const uint8_t marker[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        CHECK(kernel_guest_write_bytes(g_wired_base + 0x10u, marker, sizeof marker));
    }
    g_wired_hook_calls++;
    return true;
}

static void write_expected(const char *path, const char *label, unsigned event, const char *kind, uint64_t poll, unsigned offset)
{
    char header[300];
    snprintf(header, sizeof header, "# button-dump event=%u kind=%s label=%s poll=%llu present=0 offset=%u\n", event, kind, label,
             (unsigned long long)poll, offset);
    (void)guest_dump_write_with_header(&g_wired_set, path, header);
}

static void test_real_wiring_round_trip(void)
{
    g_wired_base = prepared_region(0x31u, 0x02600000u);
    if (g_wired_base == 0u) {
        return;
    }
    const kernel_guest_ptr cell = g_wired_base + 0x1F00u;
    uint8_t pointer[4] = {(uint8_t)(g_wired_base + 0x100u), (uint8_t)((g_wired_base + 0x100u) >> 8),
                          (uint8_t)((g_wired_base + 0x100u) >> 16), (uint8_t)((g_wired_base + 0x100u) >> 24)};
    CHECK(kernel_guest_write_bytes(cell, pointer, sizeof pointer));
    memset(&g_wired_set, 0, sizeof g_wired_set);
    char ranges[256];
    snprintf(ranges, sizeof ranges, "0x%X:0x40,*0x%X+8:0x30,0x%X:0x1200,0x3F00000:4", (unsigned)g_wired_base, (unsigned)cell,
             (unsigned)g_wired_base + 0x800u);
    CHECK(guest_dump_parse_append(&g_wired_set, ranges, NULL, 0u));
    remove_tree("bd_wired");
    memset(g_wired_script, 0, sizeof g_wired_script);
    g_wired_count = 40u;
    for (unsigned poll = 5u; poll < 25u; poll++) {
        g_wired_script[poll] = X_; /* press at 5, release at 25: two events */
    }
    g_wired_mutate = true;
    g_wired_hook_calls = 0u;
    g_wired_armed = false;

    xinput_source_reset();
    button_dump_host_config host;
    memset(&host, 0, sizeof host);
    host.set = &g_wired_set;
    host.dir = "bd_wired";
    host.after_count = 2u;
    host.after[0] = 2u;
    host.after[1] = 4u;
    host.threshold = 60u;
    host.coalesce = 3u;
    host.max_pending = 8u;
    host.max_dumps = 100u;
    host.max_bytes = 0x10000000u;
    char error[200] = "";
    CHECK(!button_dump_host_start(&(button_dump_host_config){.set = &g_wired_set, .dir = "bd_wired", .after_count = 1u,
                                                             .after = {1u}, .after_replay = true, .max_pending = 1u},
                                  error, sizeof error)); /* after-replay without an armed query is refused */
    CHECK(error[0] != '\0');
    CHECK(button_dump_host_start(&host, error, sizeof error));
    CHECK(!button_dump_host_start(&host, error, sizeof error)); /* only once */
    xinput_source_install(wired_source, NULL);
    /* the wiring writes nothing before the first edge: the manifest only has its start line */
    fflush(stderr);
    const int saved_stderr = dup(2);
    const int quiet = open("/dev/null", O_WRONLY);
    dup2(quiet, 2); /* xinput_hle complains once per poll that no synthetic pad is attached, the hook runs regardless */
    for (unsigned poll = 0u; poll < 40u; poll++) {
        (void)xinput_source_poll();
        if (poll == 5u) {
            write_expected("bd_wired_exp_before1", "0001_press_X_before", 1u, "before", 5u, 0u);
        }
        if (poll == 7u) {
            write_expected("bd_wired_exp_after1_2", "0001_press_X_after2", 1u, "after", 7u, 2u);
        }
        if (poll == 9u) {
            write_expected("bd_wired_exp_after1_4", "0001_press_X_after4", 1u, "after", 9u, 4u);
        }
    }
    CHECK_EQ(g_wired_hook_calls, 40u);
    button_dump_host_stop("exit");
    fflush(stderr);
    /* after stop the hook is gone: further polls do not reach the dump */
    const unsigned calls_at_stop = g_wired_hook_calls;
    (void)xinput_source_poll();
    CHECK_EQ(g_wired_hook_calls, calls_at_stop + 1u);
    dup2(saved_stderr, 2);
    close(saved_stderr);
    close(quiet);
    xinput_source_install(NULL, NULL);

    run view;
    memset(&view, 0, sizeof view);
    snprintf(view.dir, sizeof view.dir, "bd_wired");
    char *text = read_in(&view, "buttons.jsonl");
    CHECK(text != NULL);
    CHECK_EQ(count_of(text, "\"type\":\"event\""), 2u);
    CHECK(strstr(text, "\"after_frames\":[2,4],") != NULL);
    CHECK(strstr(line_with(text, "\"index\":2,"), "\"edge\":\"release\",\"button\":\"X\",\"buttons\":[\"X\"],\"poll\":25,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"events\":2,\"idle_events\":0,\"dumps\":6,") != NULL);
    /* event 1: before at poll 5 (memory before the game reacts), after2 at poll 7 (the reaction at poll 7 happens in the
     * source, before the hook, so the marker is visible), after4 at poll 9 */
    const char *labels[3] = {"0001_press_X_before", "0001_press_X_after2", "0001_press_X_after4"};
    const char *expected[3] = {"bd_wired_exp_before1", "bd_wired_exp_after1_2", "bd_wired_exp_after1_4"};
    char *files[3];
    for (unsigned index = 0u; index < 3u; index++) {
        char relative[160];
        snprintf(relative, sizeof relative, "buttons/guestdump.%s", labels[index]);
        files[index] = read_in(&view, relative);
        char *want = slurp(expected[index]);
        CHECK(files[index] != NULL && want != NULL);
        if (files[index] != NULL && want != NULL) {
            CHECK(strlen(want) > 3000u);
            CHECK_STR(files[index], want);
        }
        free(want);
    }
    if (files[0] != NULL && files[1] != NULL) {
        CHECK(strstr(files[1], "de ad be ef") != NULL);   /* the reaction is in the after dump ... */
        CHECK(strstr(files[0], "de ad be ef") == NULL);   /* ... and not in the before dump */
        CHECK(strstr(files[0], "unreadable 0x03F00000") != NULL);
    }
    for (unsigned index = 0u; index < 3u; index++) {
        free(files[index]);
    }
    unsigned temporaries = 0u;
    CHECK_EQ(files_in_buttons(&view, &temporaries), 6u);
    CHECK_EQ(temporaries, 0u);
    /* every file size in the manifest is the size on disk, and the estimate used for the caps bounds the real size */
    long first_size = size_of(&view, "buttons/guestdump.0001_press_X_before");
    CHECK(first_size > 3000);
    char needle[64];
    snprintf(needle, sizeof needle, "\"bytes\":%ld,\"ok\":false}", first_size);
    CHECK(strstr(text, needle) != NULL); /* the 0x3F00000 range is unreadable, so ok is false but the dump exists */
    const unsigned long long estimate = guest_dump_estimate_text_bytes(&g_wired_set) + 200u;
    CHECK(estimate >= (unsigned long long)first_size);
    snprintf(needle, sizeof needle, "\"bytes_per_dump\":%llu,", estimate);
    CHECK(strstr(text, needle) != NULL);
    free(text);
    const int saved2 = dup(2);
    const int quiet2 = open("/dev/null", O_WRONLY);
    /* --dump-button-after-replay through the real wiring: no event until the armed query turns true, none for a held button */
    g_wired_mutate = false;
    remove_tree("bd_wired");
    host.after_replay = true;
    host.forced_state = true; /* the host runs with --forced-state: the manifest says so */
    host.armed = wired_armed;
    host.after_count = 1u;
    host.after[0] = 1u;
    host.after[1] = 0u;
    host.coalesce = 0u;
    memset(g_wired_script, 0, sizeof g_wired_script);
    for (unsigned poll = 2u; poll < 14u; poll++) {
        g_wired_script[poll] = A_; /* pressed during the "replay", still held at the handover (poll 6), released at 14 */
    }
    for (unsigned poll = 16u; poll < 20u; poll++) {
        g_wired_script[poll] = B_;
    }
    xinput_source_reset(); /* the poll index restarts at 0 */
    CHECK(button_dump_host_start(&host, error, sizeof error));
    xinput_source_install(wired_source, NULL);
    fflush(stderr);
    dup2(quiet2, 2);
    for (unsigned poll = 0u; poll < 30u; poll++) {
        g_wired_armed = poll >= 6u;
        (void)xinput_source_poll();
    }
    CHECK(button_dump_host_stop_bounded("signal", 5000u)); /* the exit paths with guest threads still running use this form */
    CHECK(button_dump_host_stop_bounded("signal", 5000u)); /* nothing left to stop: harmless */
    fflush(stderr);
    dup2(saved2, 2);
    close(saved2);
    close(quiet2);
    xinput_source_install(NULL, NULL);
    char *replayed = read_in(&view, "buttons.jsonl");
    CHECK(replayed != NULL);
    CHECK_EQ(count_of(replayed, "\"type\":\"event\""), 3u); /* release A at 14, press B at 16, release B at 20 */
    CHECK(strstr(line_with(replayed, "\"index\":1,"), "\"edge\":\"release\",\"button\":\"A\",\"buttons\":[\"A\"],\"poll\":14,") != NULL);
    CHECK(strstr(line_with(replayed, "\"type\":\"start\""), "\"after_replay\":true,\"forced_state\":true}") != NULL);
    CHECK(strstr(line_with(replayed, "\"type\":\"end\""), "\"reason\":\"signal\"") != NULL);
    free(replayed);
    remove("bd_wired_exp_before1");
    remove("bd_wired_exp_after1_2");
    remove("bd_wired_exp_after1_4");
    remove_tree("bd_wired");
    g_wired_mutate = false;
    xinput_source_reset();
}


/* The bounded stop of the exit paths: a wedged device lock (held by another thread) must not hang the host. The hook removal waits
 * for the device lock in the helper thread, the caller gives up after its bound, and when the lock is released the helper finishes. */
static atomic_bool g_lock_held;
static atomic_bool g_lock_release;

static void hold_device_lock(void *user)
{
    (void)user;
    atomic_store(&g_lock_held, true);
    while (!atomic_load(&g_lock_release)) {
        struct timespec nap = {0, 1000000L};
        nanosleep(&nap, NULL);
    }
}

static void *device_lock_thread(void *user)
{
    (void)user;
    xinput_devices_run_locked(hold_device_lock, NULL);
    return NULL;
}

static double now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static void test_bounded_stop_gives_up_and_finishes_later(void)
{
    if (g_wired_base == 0u) {
        return;
    }
    remove_tree("bd_bounded");
    xinput_source_reset();
    button_dump_host_config host;
    memset(&host, 0, sizeof host);
    host.set = &g_wired_set;
    host.dir = "bd_bounded";
    host.after_count = 1u;
    host.after[0] = 1u;
    host.threshold = 60u;
    host.max_pending = 8u;
    host.max_dumps = 100u;
    host.max_bytes = 0x10000000u;
    char error[200] = "";
    CHECK(button_dump_host_stop_bounded("exit", 100u)); /* never started: nothing to do */
    CHECK(button_dump_host_start(&host, error, sizeof error));
    memset(g_wired_script, 0, sizeof g_wired_script);
    for (unsigned poll = 2u; poll < 8u; poll++) {
        g_wired_script[poll] = A_;
    }
    g_wired_count = 8u;
    g_wired_mutate = false;
    xinput_source_install(wired_source, NULL);
    fflush(stderr);
    const int saved = dup(2);
    const int quiet = open("/dev/null", O_WRONLY);
    dup2(quiet, 2);
    for (unsigned poll = 0u; poll < 6u; poll++) {
        (void)xinput_source_poll();
    }
    atomic_store(&g_lock_held, false);
    atomic_store(&g_lock_release, false);
    pthread_t holder;
    CHECK(pthread_create(&holder, NULL, device_lock_thread, NULL) == 0);
    while (!atomic_load(&g_lock_held)) {
        struct timespec nap = {0, 1000000L};
        nanosleep(&nap, NULL);
    }
    const double began = now_ms();
    const bool finished_in_time = button_dump_host_stop_bounded("signal", 300u);
    const double waited = now_ms() - began;
    CHECK(!finished_in_time);                  /* the device lock is wedged: it gave up ... */
    CHECK(waited >= 250.0 && waited < 3000.0); /* ... after its bound, not at once and not never */
    const double second_began = now_ms();
    CHECK(!button_dump_host_stop_bounded("signal", 5000u)); /* a second closer is refused at once, never a second thread */
    CHECK(now_ms() - second_began < 1000.0);
    atomic_store(&g_lock_release, true);
    pthread_join(holder, NULL);
    bool finished_later = false;
    for (unsigned attempt = 0u; attempt < 300u && !finished_later; attempt++) {
        finished_later = button_dump_host_stop_bounded("signal", 100u);
        if (!finished_later) {
            struct timespec nap = {0, 10000000L};
            nanosleep(&nap, NULL);
        }
    }
    CHECK(finished_later);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(quiet);
    xinput_source_install(NULL, NULL);
    run view;
    memset(&view, 0, sizeof view);
    snprintf(view.dir, sizeof view.dir, "bd_bounded");
    char *text = read_in(&view, "buttons.jsonl");
    CHECK(text != NULL);
    CHECK_EQ(count_of(text, "\"type\":\"event\""), 1u);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"reason\":\"signal\"") != NULL);
    free(text);
    remove_tree("bd_bounded");
    xinput_source_reset();
}

/* T1637: a guarded poke dump (guest_dump_poke_now, the SIGUSR2 path) runs outside the pad poll: it makes no edge, no event, no counter
 * move, and its file DIR/guestdump.<label> cannot collide with DIR/buttons/guestdump.<label> (another directory), even for the very
 * same label. */
static void test_poke_dump_is_independent_of_the_button_dump(void)
{
    if (g_wired_base == 0u) {
        return;
    }
    remove_tree("bd_pokes");
    xinput_source_reset();
    button_dump_host_config host;
    memset(&host, 0, sizeof host);
    host.set = &g_wired_set;
    host.dir = "bd_pokes";
    host.after_count = 1u;
    host.after[0] = 1u;
    host.threshold = 60u;
    host.max_pending = 8u;
    host.max_dumps = 100u;
    host.max_bytes = 0x10000000u;
    host.forced_state = true;
    char error[200] = "";
    CHECK(guest_dump_start(&g_wired_set, 0x100000u, "bd_pokes")); /* the same DIR, as in the host */
    CHECK(button_dump_host_start(&host, error, sizeof error));
    memset(g_wired_script, 0, sizeof g_wired_script);
    for (unsigned poll = 3u; poll < 40u; poll++) {
        g_wired_script[poll] = A_;
    }
    g_wired_count = 40u;
    g_wired_mutate = false;
    xinput_source_install(wired_source, NULL);
    fflush(stderr);
    const int saved = dup(2);
    const int quiet = open("/dev/null", O_WRONLY);
    dup2(quiet, 2);
    for (unsigned poll = 0u; poll < 6u; poll++) {
        (void)xinput_source_poll();
    }
    run view;
    memset(&view, 0, sizeof view);
    snprintf(view.dir, sizeof view.dir, "bd_pokes");
    unsigned temporaries = 0u;
    /* pokes (no request file: nothing is applied, the dump file is still written) between polls, with the label of a button dump */
    CHECK(guest_dump_poke_now("0001_press_A_before"));
    CHECK(guest_dump_poke_now("p2"));
    for (unsigned poll = 6u; poll < 12u; poll++) {
        (void)xinput_source_poll();
    }
    button_dump_host_stop_bounded("exit", 5000u);
    guest_dump_stop();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(quiet);
    xinput_source_install(NULL, NULL);
    char *text = read_in(&view, "buttons.jsonl");
    CHECK(text != NULL);
    /* one edge (A pressed at poll 3), one event with its 2 dumps: the two pokes added nothing */
    CHECK_EQ(count_of(text, "\"type\":\"event\""), 1u);
    CHECK_EQ(count_of(text, "\"type\":\"dropped\""), 0u);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"events\":1,\"idle_events\":0,\"dumps\":2,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"end\""), "\"dropped\":0,\"coalesced_edges\":0,") != NULL);
    CHECK(strstr(line_with(text, "\"type\":\"start\""), "\"forced_state\":true}") != NULL);
    CHECK_EQ(files_in_buttons(&view, &temporaries), 2u);
    /* the poke dumps are in DIR itself, the same label in both places are two different files with different headers */
    CHECK(exists_in(&view, "guestdump.0001_press_A_before"));
    CHECK(exists_in(&view, "guestdump.p2"));
    CHECK(exists_in(&view, "buttons/guestdump.0001_press_A_before"));
    char *poke_file = read_in(&view, "guestdump.0001_press_A_before");
    char *button_file = read_in(&view, "buttons/guestdump.0001_press_A_before");
    CHECK(poke_file != NULL && button_file != NULL);
    if (poke_file != NULL && button_file != NULL) {
        CHECK(strstr(poke_file, "# button-dump") == NULL);
        CHECK(strstr(button_file, "# button-dump event=1 kind=before") != NULL);
    }
    free(poke_file);
    free(button_file);
    free(text);
    remove_tree("bd_pokes");
    xinput_source_reset();
}

int main(void)
{
    test_mask_names_and_threshold();
    test_create_refuses_bad_configuration();
    test_manifest_lines_are_exact();
    test_start_line_says_forced_state();
    test_edge_detection_and_labels();
    test_hold_at_arming_is_not_an_edge();
    test_coalescing_boundary();
    test_after_offsets_and_skipped_polls();
    test_caps();
    test_idle_controls();
    test_shutdown_and_failures();
    test_event_closed_before_any_after_dump();
    test_drop_lines_are_capped();
    test_real_wiring_round_trip();
    test_bounded_stop_gives_up_and_finishes_later();
    test_poke_dump_is_independent_of_the_button_dump();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
