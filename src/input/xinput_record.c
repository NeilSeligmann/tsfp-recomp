/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_record.h"
#include "xinput_route_nav_spec.h"
#include "xinput_route_spec.h"
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Flags that do not change what the title sees, with how many values follow. */
static const struct { const char *name; int values; } ignored[] = {
    {"--record-input", 1}, {"--replay-input", 1}, {"--pad-source", 1}, {"--pad-feed", 1}, {"--pad-script", 1}, {"--pad-script-live", 1},
    {"--disc", 1}, {"--hdd", 1}, {"--present", 1}, {"--present-capture", 1}, {"--present-timeline", 1},
    {"--present-no-pace", 0}, {"--present-hold-ms", 1}, {"--audio-sink", 1}, {"--audio-output", 1},
    {"--audio-mute", 0}, {"--audio-latency-ms", 1}, {"--audio-min-rate", 1}, {"--audio-stretch", 1}, {"--audio-stretch-legacy", 0}, {"--av-sync-offset-ms", 1}, {"--audio-hold-ms", 1}, {"--no-audio-pump", 0}, {"--live-pipeline", 1}, {"--live-present-sync", 0}, {"--live-readback", 0}, {"--live-blit-verify", 1}, {"--gpu-live-blit", 0}, {"--live-frame-hash", 1}, {"--live-pipeline-cache", 1}, {"--cpu-profile", 1}, {"--cpu-profile-wall", 1}, {"--interactive", 0}, {"--thread-timeout", 1}, {"--vblank-owner-waits", 1},
    {"--vblank-worker-blanks", 1}, {"--stop-after-calls", 1}, {"--trace", 1}, {"--profile-calls", 0}, {"--profile-calls-top", 1},
    {"--profile-watch", 1}, {"--trace-xmv", 0}, {"--synthetic-pad", 0}, {"--controllers", 0},
    {"--controller-config", 1}, {"--controller-mappings", 1}, {"--controller-save", 1},
    /* T1126: output-only paths (a dump or capture the title never reads back) differ per run dir. */
    {"--dump-overlay", 1}, {"--dump-xmv-frames", 1}, {"--capture-xmv-entries", 1}, {"--gpu-replay-dump", 1},
    {"--gpu-replay-draw-dump", 1},
    /* T1153: where a snapshot is taken does not change what the title sees. */
    {"--snapshot-at-poll", 1}, {"--stop-at-poll", 1},
    /* T1616: the guarded poke gate, the route replay and the dumps are observers or a later pad handover, not what the
     * title sees before the poke; a route recorded by one tool run must replay under the other. */
    {"--forced-state", 0}, {"--dump-guest-range", 1}, {"--dump-guest-dir", 1}, {"--dump-guest-max-bytes", 1},
    {"--poke-at-poll", 1}, {"--route-wait", 1}, {"--replay-handover", 0},
    /* T1633: event waits and the event log only observe the guest (file I/O counters, calls, memory reads) and pace the pad. */
    {"--route-wait-event", 1}, {"--route-event-log", 1}, {"--route-log-mem", 1},
    /* T1618: the indirect call census is a read-only observer (src/host/function_census.c: it reads the pushed return address
     * from the guest stack, counts under its own mutex and, for --census-phases, dumps files from a helper thread on SIGUSR1).
     * It writes no guest state and decides nothing the title sees, so a route recorded without it replays under
     * tools.action_profile, which always adds it. --cpu-profile and --cpu-profile-wall (the SIGPROF sampler) are ignored above. */
    {"--voice-log", 1}, /* T1793 read-only entries and private resident receipts. */
    {"--census-icalls", 0}, {"--census-phases", 1}, {"--census-window", 1},
    /* T1627: host hotkeys are tooling, they only write files and keep a chord away from the game, they do not change the run
     * a record was made on. */
    {"--hotkey", 1}, {"--hotkey-dir", 1},
    /* T1720b: where the `shot` label writes its pictures and how many it may take. Pure output, so a route recorded or replayed
     * with them is the same run as one without (without these three a replay under --shot-dir was refused on the flag set). */
    {"--shot-dir", 1}, {"--shot-max", 1}, {"--shot-max-bytes", 1},
    /* T1629: the button dump is a passive read-only observer (src/host/button_dump.c: it reads guest memory at port 0 pad edges and
     * writes files from its own thread), it decides nothing the title sees, so a recorded route replays with or without it. */
    {"--dump-on-button", 0}, {"--dump-after-frames", 1}, {"--dump-button-threshold", 1}, {"--dump-button-coalesce", 1},
    {"--dump-button-max-pending", 1}, {"--dump-button-max-dumps", 1}, {"--dump-button-max-bytes", 1},
    {"--dump-button-start-poll", 1}, {"--dump-button-after-replay", 0}, {"--dump-button-idle-every", 1},
    {"--dump-button-max-idle", 1},
    /* T1640: the nav flags choose how the replay drives the menus (closed loop, from guest memory reads) and where the menu table
     * lives. They never change what the title sees before a nav step, and a route recorded without them must replay with them. */
    {"--route-nav", 1}, {"--route-nav-menus", 1}, {"--route-nav-timing", 1}, {"--route-nav-record", 1},
    /* T1741: the write watch only protects pages and single steps one store at a time (src/host/guest_watch.c), it writes no
     * guest byte and changes nothing the title sees, so a recorded route replays with or without it. */
    {"--watch-write", 1}, {"--watch-write-log", 1}, {"--watch-write-max", 1},
};
/* Flags whose value is an input path. The flag still changes the title run, the path does not, so the line
 * keeps the flag and writes XINPUT_RECORD_PATH_TOKEN for the value. */
static const char *const path_valued[] = {
    "--gpu-replay", "--gpu-replay-undo-viewport", "--gpu-replay-window-to-clip", "--mount", "--seed-xmv-result",
};
#define XINPUT_RECORD_PATH_TOKEN "<path>"

/* Append one canonical argument. */
static bool put_token(char *out, size_t out_size, size_t *used, const char *token, size_t length)
{
    if (length >= out_size - *used || out_size - *used - length < (*used ? 2u : 1u)) return false;
    if (*used) out[(*used)++] = ' ';
    memcpy(out + *used, token, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

/* Canonicalise argv[first..argc) into out. The same rules read a recorded `# flags:` line (split on spaces),
 * so a record made before T1126, which carries raw paths, still compares equal to a canonical line. */
static bool canonicalise(int first, int argc, char *const *argv, char *out, size_t out_size)
{
    size_t used = 0u;
    if (out_size == 0u) return false;
    out[0] = '\0';
    for (int i = first; i < argc; i++) {
        int skip = -1;
        for (size_t k = 0u; k < sizeof(ignored) / sizeof(ignored[0]); k++)
            if (strcmp(argv[i], ignored[k].name) == 0) { skip = ignored[k].values; break; }
        if (skip >= 0) { i += skip; continue; }
        if (!put_token(out, out_size, &used, argv[i], strlen(argv[i]))) return false;
        for (size_t k = 0u; k < sizeof(path_valued) / sizeof(path_valued[0]); k++)
            if (strcmp(argv[i], path_valued[k]) == 0 && i + 1 < argc) {
                i++;
                if (!put_token(out, out_size, &used, XINPUT_RECORD_PATH_TOKEN, strlen(XINPUT_RECORD_PATH_TOKEN))) return false;
                break;
            }
    }
    return true;
}

bool xinput_record_identity_flags(int argc, char *const *argv, char *out, size_t out_size)
{
    return canonicalise(2, argc, argv, out, out_size);
}

bool xinput_record_canonical_line(const char *line, char *out, size_t out_size)
{
    char copy[2048];
    char *tokens[512];
    int count = 0;
    if (strlen(line) >= sizeof(copy)) return false;
    strcpy(copy, line);
    for (char *at = strtok(copy, " "); at != NULL; at = strtok(NULL, " ")) {
        if (count == 512) return false;
        tokens[count++] = at;
    }
    return canonicalise(0, count, tokens, out, out_size);
}

/* T1618: a unit is one flag with its values (a token starting with "--" opens a unit, the tokens after it that do not are its
 * values), so a changed value reads as `--vblank-owner-waits 5` against `--vblank-owner-waits 6`, not as two stray numbers. */
typedef struct { int first, count; } flag_unit;
#define DIFF_MAX_TOKENS 512

static int split_line(const char *line, char *copy, size_t copy_size, char **tokens)
{
    int count = 0;
    char *save = NULL;
    if (strlen(line) >= copy_size) return -1;
    strcpy(copy, line);
    for (char *at = strtok_r(copy, " ", &save); at != NULL; at = strtok_r(NULL, " ", &save)) {
        if (count == DIFF_MAX_TOKENS) return -1;
        tokens[count++] = at;
    }
    return count;
}

static int group_units(char *const *tokens, int count, flag_unit *units)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n == 0 || strncmp(tokens[i], "--", 2) == 0) { units[n].first = i; units[n].count = 1; n++; }
        else units[n - 1].count++;
    }
    return n;
}

static bool units_equal(char *const *a, const flag_unit *ua, char *const *b, const flag_unit *ub)
{
    if (ua->count != ub->count) return false;
    for (int k = 0; k < ua->count; k++)
        if (strcmp(a[ua->first + k], b[ub->first + k]) != 0) return false;
    return true;
}

/* Append one unit to out (units separated by ", ", tokens by a space). When it does not fit the list ends with "...". */
static void append_unit(char *out, size_t out_size, char *const *tokens, const flag_unit *unit)
{
    size_t used = strlen(out);
    if (used >= 3u && strcmp(out + used - 3u, "...") == 0) return;
    size_t need = used ? 2u : 0u;
    for (int k = 0; k < unit->count; k++) need += strlen(tokens[unit->first + k]) + (k ? 1u : 0u);
    if (used + need + 4u > out_size) {
        if (used + 4u <= out_size) strcpy(out + used, "...");
        return;
    }
    if (used) { out[used++] = ','; out[used++] = ' '; }
    for (int k = 0; k < unit->count; k++) {
        const size_t length = strlen(tokens[unit->first + k]);
        if (k) out[used++] = ' ';
        memcpy(out + used, tokens[unit->first + k], length);
        used += length;
    }
    out[used] = '\0';
}

bool xinput_record_flags_diff(const char *recorded, const char *current, char *only_recorded, size_t recorded_size,
                              char *only_current, size_t current_size)
{
    char copy_a[2048], copy_b[2048];
    char *tokens_a[DIFF_MAX_TOKENS], *tokens_b[DIFF_MAX_TOKENS];
    flag_unit units_a[DIFF_MAX_TOKENS], units_b[DIFF_MAX_TOKENS];
    bool matched_b[DIFF_MAX_TOKENS] = {false};
    if (recorded_size > 0u) only_recorded[0] = '\0';
    if (current_size > 0u) only_current[0] = '\0';
    const int count_a = split_line(recorded, copy_a, sizeof(copy_a), tokens_a);
    const int count_b = split_line(current, copy_b, sizeof(copy_b), tokens_b);
    if (count_a < 0 || count_b < 0 || recorded_size < 8u || current_size < 8u) return false;
    const int n_a = group_units(tokens_a, count_a, units_a);
    const int n_b = group_units(tokens_b, count_b, units_b);
    bool differs = false;
    for (int i = 0; i < n_a; i++) {
        bool found = false;
        for (int j = 0; j < n_b && !found; j++)
            if (!matched_b[j] && units_equal(tokens_a, &units_a[i], tokens_b, &units_b[j])) { matched_b[j] = true; found = true; }
        if (!found) { append_unit(only_recorded, recorded_size, tokens_a, &units_a[i]); differs = true; }
    }
    for (int j = 0; j < n_b; j++)
        if (!matched_b[j]) { append_unit(only_current, current_size, tokens_b, &units_b[j]); differs = true; }
    return differs;
}

size_t xinput_record_header(char *out, size_t out_size, const char *xbe_sha256, const char *flags_sha256,
                            const char *flags)
{
    const int n = snprintf(out, out_size, "# tsfp-input v%d\n# xbe-sha256: %s\n# flags-sha256: %s\n# flags: %s\n",
                           XINPUT_RECORD_VERSION, xbe_sha256, flags_sha256, flags);
    return n < 0 || (size_t)n >= out_size ? 0u : (size_t)n;
}

/* The value of `# key: value` among the leading comment lines, copied into out. */
static bool header_value(const char *text, size_t len, const char *key, char *out, size_t out_size)
{
    const size_t key_len = strlen(key);
    size_t at = 0u;
    while (at < len && text[at] == '#') {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end - at > 2u + key_len + 1u && strncmp(text + at, "# ", 2) == 0 &&
            strncmp(text + at + 2u, key, key_len) == 0 && text[at + 2u + key_len] == ':') {
            size_t start = at + 2u + key_len + 1u;
            while (start < end && text[start] == ' ') start++;
            size_t n = end - start;
            if (n >= out_size) return false;
            memcpy(out, text + start, n);
            out[n] = '\0';
            return true;
        }
        at = end + 1u;
    }
    return false;
}

bool xinput_record_check_header(const char *text, size_t len, const char *xbe_sha256, const char *flags_sha256,
                                const char *flags, char *error, size_t error_size)
{
    char value[2048] = "(missing)";
    char want[32];
    snprintf(want, sizeof(want), "tsfp-input v%d", XINPUT_RECORD_VERSION);
    if (len < 2u || strncmp(text, "# ", 2) != 0) {
        snprintf(error, error_size, "not an input record (no '# tsfp-input vN' header)");
        return false;
    }
    size_t first = 2u;
    while (first < len && text[first] != '\n') first++;
    if (first - 2u != strlen(want) || strncmp(text + 2, want, first - 2u) != 0) {
        snprintf(error, error_size, "unsupported record version (first line is not '# %s')", want);
        return false;
    }
    if (!header_value(text, len, "xbe-sha256", value, sizeof(value)) || strcmp(value, xbe_sha256) != 0) {
        snprintf(error, error_size, "recorded against a different XBE: record has %.64s, this run has %s",
                 value, xbe_sha256);
        return false;
    }
    char recorded[1500] = "(missing)";
    const bool have_flags_line = header_value(text, len, "flags", recorded, sizeof(recorded));
    char recorded_canonical[2048] = "", current_canonical[2048] = "";
    const bool canonical_ok = have_flags_line &&
        xinput_record_canonical_line(recorded, recorded_canonical, sizeof(recorded_canonical)) &&
        xinput_record_canonical_line(flags, current_canonical, sizeof(current_canonical));
    const bool flags_match =
        (header_value(text, len, "flags-sha256", value, sizeof(value)) && strcmp(value, flags_sha256) == 0) ||
        (canonical_ok && strcmp(recorded_canonical, current_canonical) == 0);
    if (!flags_match) {
        /* T1618: print ONLY the flags that differ, never the two full lines (about 700 characters each). */
        char only_recorded[700] = "", only_current[700] = "";
        if (!canonical_ok) {
            snprintf(error, error_size, "recorded against a different flag set: the record has no usable '# flags:' line.\n  %s",
                     XINPUT_RECORD_REFLAG_ADVICE);
        } else if (xinput_record_flags_diff(recorded_canonical, current_canonical, only_recorded, sizeof(only_recorded),
                                            only_current, sizeof(only_current))) {
            snprintf(error, error_size,
                     "recorded against a different flag set.\n  only in the recording: %s\n  only in this run:      %s\n  %s",
                     only_recorded[0] ? only_recorded : "(nothing)", only_current[0] ? only_current : "(nothing)",
                     XINPUT_RECORD_REFLAG_ADVICE);
        } else {
            snprintf(error, error_size,
                     "recorded against a different flag set: the same flags in a different order.\n  recording: %.500s\n  this run:  %.500s\n  %s",
                     recorded_canonical, current_canonical, XINPUT_RECORD_REFLAG_ADVICE);
        }
        return false;
    }
    return true;
}

size_t xinput_record_format_run(char *out, size_t out_size, uint64_t frames, const xinput_pad_state *state,
                                bool *lossy)
{
    static const struct { const char *name; uint16_t bit; } digital[] = {
        {"UP", XINPUT_BUTTON_DPAD_UP}, {"DOWN", XINPUT_BUTTON_DPAD_DOWN}, {"LEFT", XINPUT_BUTTON_DPAD_LEFT},
        {"RIGHT", XINPUT_BUTTON_DPAD_RIGHT}, {"START", XINPUT_BUTTON_START}, {"BACK", XINPUT_BUTTON_BACK},
        {"LTHUMB", XINPUT_BUTTON_LEFT_THUMB}, {"RTHUMB", XINPUT_BUTTON_RIGHT_THUMB},
    };
    static const char *const analog[XINPUT_ANALOG_COUNT] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
    size_t used = 0u;
    uint16_t known = 0u;
#define PUT(...) do { int n_ = snprintf(out + used, out_size - used, __VA_ARGS__); \
    if (n_ < 0 || (size_t)n_ >= out_size - used) return 0u; used += (size_t)n_; } while (0)
    PUT("%llu", (unsigned long long)frames);
    for (size_t i = 0u; i < sizeof(digital) / sizeof(digital[0]); i++) {
        known |= digital[i].bit;
        if (state->digital_buttons & digital[i].bit) PUT(" %s", digital[i].name);
    }
    for (unsigned i = 0u; i < XINPUT_ANALOG_COUNT; i++)
        if (state->analog[i] != 0u) PUT(" %s=%u", analog[i], (unsigned)state->analog[i]);
    if (state->thumb_left_x) PUT(" LX=%d", state->thumb_left_x);
    if (state->thumb_left_y) PUT(" LY=%d", state->thumb_left_y);
    if (state->thumb_right_x) PUT(" RX=%d", state->thumb_right_x);
    if (state->thumb_right_y) PUT(" RY=%d", state->thumb_right_y);
    PUT("\n");
#undef PUT
    if (lossy != NULL && (state->digital_buttons & (uint16_t)~known)) *lossy = true;
    return used;
}

/* T1126: the budgets a recording ran with. They stay out of the identity (a prefix replay is allowed) but a replay
 * must run under the recorded ones by default, or an interactive recording (which waives the owner and worker
 * wait budget stops) stops early when replayed non-interactively. */
size_t xinput_record_format_budgets(char *out, size_t out_size, const xinput_record_budgets *budgets)
{
    const int n = snprintf(out, out_size, "# budgets: owner-waits=%u worker-blanks=%u thread-timeout=%u interactive=%u\n",
                           budgets->owner_waits, budgets->worker_blanks, budgets->thread_timeout_ms,
                           budgets->interactive ? 1u : 0u);
    return n < 0 || (size_t)n >= out_size ? 0u : (size_t)n;
}

bool xinput_record_parse_budgets(const char *text, size_t len, xinput_record_budgets *budgets)
{
    char value[256];
    unsigned owner, worker, timeout, interactive;
    if (!header_value(text, len, "budgets", value, sizeof(value))) return false;
    if (sscanf(value, "owner-waits=%u worker-blanks=%u thread-timeout=%u interactive=%u", &owner, &worker, &timeout,
               &interactive) != 4 || interactive > 1u) return false;
    budgets->owner_waits = owner; budgets->worker_blanks = worker; budgets->thread_timeout_ms = timeout;
    budgets->interactive = interactive != 0u;
    return true;
}

static xinput_record_budgets record_budgets, replay_budgets;
static bool record_have_budgets, replay_have_budgets;
static uint64_t replay_total_polls;
static uint64_t replay_marks[XINPUT_ROUTE_MAX_MARKS];
static size_t replay_mark_count;
static const xinput_script *replay_script;
/* T1633: `# wait: markK:fields` lines of the record (event waits written by tools.route_events or by hand). */
static char replay_waits[XINPUT_ROUTE_MAX_WAITS][XINPUT_REPLAY_WAIT_LEN];
static size_t replay_wait_count;
/* T1640: `# nav:` lines of the record (closed loop menu navigation), kept as text for the route. */
static char replay_navs[ROUTE_NAV_MAX_STEPS][XINPUT_REPLAY_NAV_LEN];
static size_t replay_nav_count;
static xinput_record_mark_facts_fn mark_facts_fn;
static void *mark_facts_user;
void xinput_record_set_mark_facts(xinput_record_mark_facts_fn fn, void *user)
{
    mark_facts_fn = fn;
    mark_facts_user = user;
}
size_t xinput_replay_wait_count(void) { return replay_wait_count; }
const char *xinput_replay_wait(size_t index) { return index < replay_wait_count ? replay_waits[index] : NULL; }
size_t xinput_replay_nav_count(void) { return replay_nav_count; }
const char *xinput_replay_nav(size_t index) { return index < replay_nav_count ? replay_navs[index] : NULL; }

void xinput_record_set_budgets(const xinput_record_budgets *budgets)
{
    record_budgets = *budgets; record_have_budgets = true;
}
bool xinput_replay_budgets(xinput_record_budgets *budgets)
{
    if (replay_have_budgets) *budgets = replay_budgets;
    return replay_have_budgets;
}
uint64_t xinput_replay_total_polls(void) { return replay_total_polls; }
size_t xinput_replay_marks(const uint64_t **marks) { *marks = replay_marks; return replay_mark_count; }
const xinput_script *xinput_replay_script(void) { return replay_script; }

/* T1633: `# wait: markK:...` lines, at most XINPUT_ROUTE_MAX_WAITS, kept as text (the route parses them, so a record with a
 * grammar this host does not know is refused when the route is built, with the line number, not silently skipped). */
static bool parse_waits(const char *text, size_t len, char (*waits)[XINPUT_REPLAY_WAIT_LEN], size_t *count, char *error,
                        size_t error_size)
{
    size_t at = 0u, line = 1u;
    *count = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end - at >= 7u && memcmp(text + at, "# wait:", 7u) == 0) {
            size_t p = at + 7u, q = end;
            while (p < q && text[p] == ' ') p++;
            while (q > p && (text[q - 1u] == ' ' || text[q - 1u] == '\r')) q--;
            if (q - p < 6u || memcmp(text + p, "mark", 4u) != 0 || q - p >= XINPUT_REPLAY_WAIT_LEN || *count == XINPUT_ROUTE_MAX_WAITS) {
                snprintf(error, error_size, "bad '# wait:' line %zu (want 'mark<K>:fields', at most %u waits, %u characters)", line,
                         XINPUT_ROUTE_MAX_WAITS, XINPUT_REPLAY_WAIT_LEN - 1u);
                return false;
            }
            memcpy(waits[*count], text + p, q - p);
            waits[*count][q - p] = '\0';
            (*count)++;
        }
        at = end + 1u;
        line++;
    }
    return true;
}

/* T1640: `# nav:` lines, at most ROUTE_NAV_MAX_STEPS, kept as text (the host parses and checks them against the marks). */
static bool parse_navs(const char *text, size_t len, char (*navs)[XINPUT_REPLAY_NAV_LEN], size_t *count, char *error, size_t error_size)
{
    size_t at = 0u, line = 1u;
    *count = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end - at >= 6u && memcmp(text + at, "# nav:", 6u) == 0) {
            size_t p = at + 6u, q = end;
            while (p < q && text[p] == ' ') p++;
            while (q > p && (text[q - 1u] == ' ' || text[q - 1u] == '\r')) q--;
            if (*count == ROUTE_NAV_MAX_STEPS || q == p || q - p >= XINPUT_REPLAY_NAV_LEN) {
                snprintf(error, error_size, "bad '# nav:' line %zu (empty, over %u characters, or more than %u nav lines)", line,
                         XINPUT_REPLAY_NAV_LEN - 1u, ROUTE_NAV_MAX_STEPS);
                return false;
            }
            memcpy(navs[*count], text + p, q - p);
            navs[*count][q - p] = '\0';
            (*count)++;
        }
        at = end + 1u;
        line++;
    }
    return true;
}

/* `# mark: at=N` lines of the body: non decreasing, at most XINPUT_ROUTE_MAX_MARKS. False with `error` when malformed. */
static bool parse_marks(const char *text, size_t len, uint64_t total, uint64_t *marks, size_t *count, char *error,
                        size_t error_size)
{
    size_t at = 0u;
    *count = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end - at >= 8u && memcmp(text + at, "# mark:", 7u) == 0) {
            size_t p = at + 7u;
            while (p < end && text[p] == ' ') p++;
            if (end - p < 3u || memcmp(text + p, "at=", 3u) != 0) { snprintf(error, error_size, "malformed mark line"); return false; }
            p += 3u;
            uint64_t value = 0u;
            const size_t digits = p;
            while (p < end && text[p] >= '0' && text[p] <= '9') {
                const unsigned digit = (unsigned)(text[p++] - '0');
                if (value > (UINT64_MAX - digit) / 10u) { snprintf(error, error_size, "malformed mark line"); return false; }
                value = value * 10u + digit;
            }
            while (p < end && (text[p] == ' ' || text[p] == '\r')) p++;
            if (p == digits || p != end || value > total || *count == XINPUT_ROUTE_MAX_MARKS ||
                (*count != 0u && value < marks[*count - 1u])) {
                snprintf(error, error_size, "bad mark %zu (not a count, past the record, out of order or more than %u)", *count + 1u,
                         XINPUT_ROUTE_MAX_MARKS);
                return false;
            }
            marks[(*count)++] = value;
        }
        at = end + 1u;
    }
    return true;
}

static pthread_mutex_t record_mutex = PTHREAD_MUTEX_INITIALIZER;
/* T1616: SIGUSR2 while recording asks for a mark at the next poll (async-signal-safe counter only). */
static atomic_uint mark_requests;
static unsigned marks_written, marks_dropped;
static struct sigaction mark_previous;
static bool mark_installed;
static char mark_pid_path[1100];
static void on_mark_signal(int signal_number)
{
    atomic_fetch_add(&mark_requests, 1u);
    if (mark_previous.sa_handler != SIG_DFL && mark_previous.sa_handler != SIG_IGN && mark_previous.sa_handler != NULL &&
        (mark_previous.sa_flags & SA_SIGINFO) == 0)
        mark_previous.sa_handler(signal_number);
}
bool xinput_record_enable_marks(const char *record_path)
{
    if (mark_installed) return true;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_mark_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, &mark_previous) != 0) return false;
    mark_installed = true;
    snprintf(mark_pid_path, sizeof mark_pid_path, "%s.pid", record_path);
    FILE *pid = fopen(mark_pid_path, "w");
    if (pid != NULL) { fprintf(pid, "%d\n", (int)getpid()); fclose(pid); }
    return true;
}
unsigned xinput_record_marks_written(void) { return marks_written; }
static FILE *record_file;
static bool record_have_run, record_lossy, record_atexit, record_failed;
static uint64_t record_run_frames, record_run_start, record_polls;
static xinput_pad_state record_run_state, record_last_state;

static bool same_state(const xinput_pad_state *a, const xinput_pad_state *b)
{
    return a->digital_buttons == b->digital_buttons && memcmp(a->analog, b->analog, sizeof(a->analog)) == 0 &&
           a->thumb_left_x == b->thumb_left_x && a->thumb_left_y == b->thumb_left_y &&
           a->thumb_right_x == b->thumb_right_x && a->thumb_right_y == b->thumb_right_y;
}

/* T1632: the recorded runs are kept in a pending tail and written to the file only when they are older than
 * RECORD_TAIL_LAG polls (or at close). While they are pending, xinput_record_suppress_begin can take a chord key that
 * leaked to the game out of them, so the route never holds the hotkey gesture. The lag is in POLLS (never wall time), so what is
 * written is a function of the poll sequence only. A comment mark is an item of the same list so order is kept. */
#define RECORD_TAIL_LAG 4096u
#define RECORD_PENDING_MAX 8192u
#define RECORD_RUN_LIMIT 1000000u
typedef struct {
    bool mark;
    uint64_t start, frames; /* a mark: start is its position, frames 0 */
    xinput_pad_state state;
    unsigned number; /* T1633: a mark's number (1 based), its `# mark-info:` text is mark_info[number - 1] */
    bool nav;        /* T1640: a `# nav:` comment (mark is true too: a barrier no run merges across), number indexes nav_text, frames = to - at */
} pending_item;
/* T1633: what the host observed since the previous mark, captured when the mark is requested and written after its `# mark:` line
 * (which can be written later, the runs are pending for a while). */
#define MARK_INFO_LEN 400u
static char mark_info[XINPUT_ROUTE_MAX_MARKS][MARK_INFO_LEN];
static pending_item pending[RECORD_PENDING_MAX];
static size_t pending_count;
static uint64_t record_written_polls; /* polls that reached the file: a trim cannot reach back before it */
static unsigned record_trim_clipped, record_trims;

#define RECORD_SUPPRESS_MAX 8u
static xinput_record_effect suppressed[RECORD_SUPPRESS_MAX];
static unsigned suppressed_refs[RECORD_SUPPRESS_MAX]; /* two keys can set the same effect */
static unsigned suppressed_count;

static bool effect_equal(const xinput_record_effect *a, const xinput_record_effect *b)
{
    return a->digital_mask == b->digital_mask && a->analog == b->analog && a->stick == b->stick;
}
static void apply_effect(xinput_pad_state *state, const xinput_record_effect *effect)
{
    state->digital_buttons = (uint16_t)(state->digital_buttons & (uint16_t)~effect->digital_mask);
    if (effect->analog >= 0 && (unsigned)effect->analog < XINPUT_ANALOG_COUNT) state->analog[effect->analog] = 0u;
    switch (effect->stick) {
    case 0: state->thumb_left_x = 0; break;
    case 1: state->thumb_left_y = 0; break;
    case 2: state->thumb_right_x = 0; break;
    case 3: state->thumb_right_y = 0; break;
    default: break;
    }
}

/* T1640: the `# nav:` lines made by the recorder side (xinput_nav_record.c), waiting in the pending list like marks. */
#define NAV_SLOTS 128u
static char nav_text[NAV_SLOTS][320];
static int nav_select_analog[NAV_SLOTS];
static uint16_t nav_select_digital[NAV_SLOTS];
static uint64_t nav_edge[NAV_SLOTS];
static unsigned nav_slots, nav_live, nav_written;
static uint64_t last_nav_to;
static uint64_t mark_positions[XINPUT_ROUTE_MAX_MARKS];
static xinput_record_nav_observer_fn nav_observer;
static xinput_record_nav_close_fn nav_closer;
static xinput_record_nav_log_fn nav_logger;
static void *nav_hook_user;

static void write_item(const pending_item *item)
{
    if (record_file == NULL || record_failed) return;
    if (item->nav) {
        if (fprintf(record_file, "%s\n", nav_text[item->number]) < 0) record_failed = true;
        nav_written++;
        return;
    }
    if (item->mark) {
        if (fprintf(record_file, "# mark: at=%llu\n", (unsigned long long)item->start) < 0) record_failed = true;
        if (item->number >= 1u && item->number <= XINPUT_ROUTE_MAX_MARKS && mark_info[item->number - 1u][0] != '\0' &&
            fprintf(record_file, "# mark-info: mark%u %s\n", item->number, mark_info[item->number - 1u]) < 0)
            record_failed = true;
        return;
    }
    char line[256];
    const size_t n = xinput_record_format_run(line, sizeof(line), item->frames, &item->state, &record_lossy);
    if (n == 0u || fwrite(line, 1u, n, record_file) != n) record_failed = true;
    record_written_polls = item->start + item->frames;
}
/* Write the first `count` pending items and drop them. */
static void drain_pending(size_t count)
{
    if (count > pending_count) count = pending_count;
    for (size_t i = 0u; i < count; i++) write_item(&pending[i]);
    memmove(pending, pending + count, (pending_count - count) * sizeof pending[0]);
    pending_count -= count;
}
static void drain_old(void)
{
    size_t count = 0u;
    while (count < pending_count && pending[count].start + pending[count].frames + RECORD_TAIL_LAG <= record_polls) count++;
    if (pending_count - count > RECORD_PENDING_MAX - 8u) count = pending_count - (RECORD_PENDING_MAX - 8u); /* never overflow */
    drain_pending(count);
}
static void pending_push(const pending_item *item)
{
    if (pending_count >= RECORD_PENDING_MAX - 2u) drain_pending(RECORD_PENDING_MAX / 4u);
    /* equal states next to each other are one run, so a trimmed route is the same text as a never pressed one */
    if (!item->mark && pending_count != 0u) {
        pending_item *last = &pending[pending_count - 1u];
        if (!last->mark && same_state(&last->state, &item->state) && last->frames + item->frames <= RECORD_RUN_LIMIT) {
            last->frames += item->frames;
            return;
        }
    }
    pending[pending_count++] = *item;
}
/* The run being counted becomes a pending item (a mark or a trim needs the history as items). */
static void commit_open_run(void)
{
    if (!record_have_run) return;
    const pending_item item = {false, record_run_start, record_run_frames, record_run_state, 0u, false};
    pending_push(&item);
    record_have_run = false;
}
/* The script grammar bounds each run to one million polls. */
static void append_state(const xinput_pad_state *state_in)
{
    xinput_pad_state state = *state_in;
    for (unsigned i = 0u; i < suppressed_count; i++) apply_effect(&state, &suppressed[i]);
    record_last_state = state;
    if (record_have_run && (!same_state(&record_run_state, &state) || record_run_frames == RECORD_RUN_LIMIT)) commit_open_run();
    if (record_have_run) record_run_frames++;
    else {
        record_run_state = state;
        record_run_frames = 1u;
        record_run_start = record_polls;
        record_have_run = true;
    }
}

bool xinput_record_open(const char *path, const char *xbe_sha256, const char *flags_sha256, const char *flags,
                        char *error, size_t error_size)
{
    char header[4096];
    const size_t n = xinput_record_header(header, sizeof(header), xbe_sha256, flags_sha256, flags);
    if (n == 0u) { snprintf(error, error_size, "flag identity too long for the header"); return false; }
    (void)pthread_mutex_lock(&record_mutex);
    if (record_file != NULL) {
        (void)pthread_mutex_unlock(&record_mutex);
        snprintf(error, error_size, "an input record is already open"); return false;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) { (void)pthread_mutex_unlock(&record_mutex); snprintf(error, error_size, "cannot create %s", path); return false; }
    char budget_line[256];
    const size_t budget_n = record_have_budgets ? xinput_record_format_budgets(budget_line, sizeof(budget_line), &record_budgets) : 0u;
    if (fwrite(header, 1u, n, file) != n || (budget_n != 0u && fwrite(budget_line, 1u, budget_n, file) != budget_n) ||
        fflush(file) != 0) {
        fclose(file); (void)pthread_mutex_unlock(&record_mutex); snprintf(error, error_size, "cannot write record header"); return false;
    }
    record_file = file; record_have_run = false; record_lossy = false; record_failed = false; record_polls = 0u;
    record_run_state = (xinput_pad_state){0};
    record_last_state = (xinput_pad_state){0};
    pending_count = 0u; record_written_polls = 0u; suppressed_count = 0u; record_trim_clipped = 0u; record_trims = 0u;
    nav_slots = 0u; nav_live = 0u; nav_written = 0u; last_nav_to = 0u;
    atomic_store(&mark_requests, 0u); marks_written = 0u; marks_dropped = 0u; /* T1632: the marks and the cap are per recording */
    if (!record_atexit) { record_atexit = true; atexit(xinput_record_close); }
    (void)pthread_mutex_unlock(&record_mutex);
    return true;
}

/* Marks requested by SIGUSR2 or the `mark` hotkey are written as comment lines at the next poll position (record_polls), after
 * the run so far is closed. A comment line keeps the file a valid --pad-script. A replay parses at most
 * XINPUT_ROUTE_MAX_MARKS, so a request beyond that is refused loudly here instead of making the whole route unreplayable. */
static void write_pending_marks(void)
{
    const unsigned requested = atomic_load(&mark_requests);
    while (marks_written + marks_dropped < requested && record_file != NULL && !record_failed) {
        if (marks_written >= XINPUT_ROUTE_MAX_MARKS) {
            marks_dropped++;
            fprintf(stderr, "input recording: mark ignored at poll %llu, a route holds at most %u marks (T1632)\n",
                    (unsigned long long)record_polls, (unsigned)XINPUT_ROUTE_MAX_MARKS);
            continue;
        }
        commit_open_run();
        marks_written++;
        mark_info[marks_written - 1u][0] = '\0';
        if (mark_facts_fn != NULL) { /* T1633: what the host observed since the previous mark, a comment for tools.route_events */
            if (!mark_facts_fn(marks_written, mark_info[marks_written - 1u], MARK_INFO_LEN, mark_facts_user))
                mark_info[marks_written - 1u][0] = '\0';
        }
        mark_positions[marks_written - 1u] = record_polls;
        const pending_item item = {true, record_polls, 0u, {0}, marks_written, false};
        pending_push(&item);
        fprintf(stderr, "input recording: mark %u at poll %llu (T1616)\n", marks_written, (unsigned long long)record_polls);
    }
}

bool xinput_record_request_mark(void)
{
    (void)pthread_mutex_lock(&record_mutex);
    const bool recording = record_file != NULL && !record_failed;
    if (recording) atomic_fetch_add(&mark_requests, 1u);
    (void)pthread_mutex_unlock(&record_mutex);
    return recording;
}

/* The recorded state of poll `poll` while it is still pending (or in the open run). False when it is already in the file. */
static bool state_at_poll(uint64_t poll, xinput_pad_state *out)
{
    for (size_t i = 0u; i < pending_count; i++) {
        const pending_item *item = &pending[i];
        if (!item->mark && poll >= item->start && poll < item->start + item->frames) { *out = item->state; return true; }
    }
    if (record_have_run && poll >= record_run_start && poll < record_run_start + record_run_frames) { *out = record_run_state; return true; }
    return false;
}
static bool select_is_down(const xinput_pad_state *state, int analog, uint16_t digital)
{
    if (analog >= 0 && (unsigned)analog < XINPUT_ANALOG_COUNT) return state->analog[analog] >= ROUTE_NAV_SELECT_DOWN;
    return (state->digital_buttons & digital) != 0u;
}
static void nav_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void nav_log(const char *format, ...)
{
    if (nav_logger == NULL) return;
    char line[300];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    nav_logger(line, nav_hook_user);
}

void xinput_record_set_nav_hooks(xinput_record_nav_observer_fn observer, xinput_record_nav_close_fn closer, xinput_record_nav_log_fn logger, void *user)
{
    (void)pthread_mutex_lock(&record_mutex);
    nav_observer = observer;
    nav_closer = closer;
    nav_logger = logger;
    nav_hook_user = user;
    (void)pthread_mutex_unlock(&record_mutex);
}

const char *xinput_record_add_nav(uint64_t at, uint64_t to, uint64_t edge_poll, const char *body, int select_analog, uint16_t select_digital)
{
    const char *reason = NULL;
    (void)pthread_mutex_lock(&record_mutex);
    if (record_file == NULL || record_failed) reason = "not-recording";
    else if (nav_live >= ROUTE_NAV_MAX_STEPS || nav_slots >= NAV_SLOTS) reason = "limit-64";
    else if (at > to) reason = "at-above-to";
    if (reason == NULL && at < last_nav_to) at = last_nav_to; /* the previous step ends where this one may begin */
    if (reason == NULL && at > to) reason = "overlap";
    for (unsigned i = 0u; reason == NULL && i < marks_written && i < XINPUT_ROUTE_MAX_MARKS; i++)
        if (mark_positions[i] > at && mark_positions[i] < to) reason = "mark-inside";
    xinput_pad_state edge_state;
    if (reason == NULL && state_at_poll(edge_poll, &edge_state) && !select_is_down(&edge_state, select_analog, select_digital)) reason = "suppressed";
    if (reason == NULL) {
        const unsigned slot = nav_slots++;
        snprintf(nav_text[slot], sizeof nav_text[slot], "# nav: at=%llu to=%llu %s", (unsigned long long)at, (unsigned long long)to, body);
        nav_select_analog[slot] = select_analog;
        nav_select_digital[slot] = select_digital;
        nav_edge[slot] = edge_poll;
        if (pending_count >= RECORD_PENDING_MAX - 2u) drain_pending(RECORD_PENDING_MAX / 4u);
        size_t index = pending_count;
        for (size_t i = 0u; i < pending_count; i++) {
            if (!pending[i].mark && pending[i].start + pending[i].frames > at) { index = i; break; }
        }
        memmove(&pending[index + 1u], &pending[index], (pending_count - index) * sizeof pending[0]);
        const pending_item item = {true, at, to - at, {0}, slot, true};
        pending[index] = item;
        pending_count++;
        nav_live++;
        last_nav_to = to;
    }
    (void)pthread_mutex_unlock(&record_mutex);
    return reason;
}

unsigned xinput_record_navs_written(void) { return nav_written; }

/* After the history changed (a hotkey chord taken out): a nav line whose select press is gone from the record is dropped. */
static void revalidate_navs(void)
{
    size_t out = 0u;
    for (size_t i = 0u; i < pending_count; i++) {
        const pending_item *item = &pending[i];
        xinput_pad_state state;
        if (item->nav && state_at_poll(nav_edge[item->number], &state) &&
            !select_is_down(&state, nav_select_analog[item->number], nav_select_digital[item->number])) {
            nav_live--;
            nav_log("nav-skip reason=suppressed %s", nav_text[item->number] + 7);
            continue;
        }
        pending[out++] = pending[i];
    }
    pending_count = out;
}

void xinput_record_suppress_begin(const xinput_record_effect *effect, uint64_t from_poll)
{
    (void)pthread_mutex_lock(&record_mutex);
    if (record_file != NULL && !record_failed && effect != NULL) {
        commit_open_run();
        if (from_poll < record_written_polls) {
            /* the key went down so long ago that its runs are already in the file: only the rest can be taken out */
            record_trim_clipped++;
            fprintf(stderr, "input recording: the hotkey gesture started at poll %llu but polls before %llu are already written, "
                            "that part stays in the route (T1632)\n",
                    (unsigned long long)from_poll, (unsigned long long)record_written_polls);
            from_poll = record_written_polls;
        }
        record_trims++;
        /* clear the effect in every pending run from `from_poll` on, splitting the run that straddles it */
        for (size_t i = 0u; i < pending_count; i++) {
            pending_item *item = &pending[i];
            if (item->mark || item->start + item->frames <= from_poll) continue;
            if (item->start < from_poll) {
                if (pending_count >= RECORD_PENDING_MAX) break;
                memmove(&pending[i + 1u], &pending[i], (pending_count - i) * sizeof pending[0]);
                pending_count++;
                pending[i].frames = from_poll - pending[i].start;
                pending[i + 1u].start = from_poll;
                pending[i + 1u].frames -= pending[i].frames;
                continue; /* the next iteration clears the second half */
            }
            apply_effect(&item->state, effect);
        }
        /* equal neighbours become one run again */
        size_t out = 0u;
        for (size_t i = 0u; i < pending_count; i++) {
            if (out != 0u && !pending[i].mark && !pending[out - 1u].mark &&
                same_state(&pending[out - 1u].state, &pending[i].state) &&
                pending[out - 1u].frames + pending[i].frames <= RECORD_RUN_LIMIT) {
                pending[out - 1u].frames += pending[i].frames;
                continue;
            }
            pending[out++] = pending[i];
        }
        pending_count = out;
        revalidate_navs();
        if (pending_count != 0u) {
            const pending_item *last = &pending[pending_count - 1u];
            if (!last->mark) record_last_state = last->state;
        }
        bool known = false;
        for (unsigned i = 0u; i < suppressed_count; i++)
            if (effect_equal(&suppressed[i], effect)) { suppressed_refs[i]++; known = true; }
        if (!known && suppressed_count < RECORD_SUPPRESS_MAX) {
            suppressed[suppressed_count] = *effect;
            suppressed_refs[suppressed_count++] = 1u;
        }
    }
    (void)pthread_mutex_unlock(&record_mutex);
}

void xinput_record_suppress_end(const xinput_record_effect *effect, uint64_t at_poll)
{
    (void)at_poll; /* the poll is where the key is no longer in the state; later polls are simply recorded as they come */
    (void)pthread_mutex_lock(&record_mutex);
    if (effect != NULL) {
        for (unsigned i = 0u; i < suppressed_count; i++) {
            if (!effect_equal(&suppressed[i], effect)) continue;
            if (--suppressed_refs[i] != 0u) break;
            suppressed[i] = suppressed[--suppressed_count];
            suppressed_refs[i] = suppressed_refs[suppressed_count];
            break;
        }
    }
    (void)pthread_mutex_unlock(&record_mutex);
}

unsigned xinput_record_trims(void) { return record_trims; }
unsigned xinput_record_trims_clipped(void) { return record_trim_clipped; }

void xinput_record_observe(uint64_t poll_index, unsigned port, const xinput_pad_state *state)
{
    if (port != 0u) return;
    xinput_record_nav_observer_fn observer = NULL;
    void *observer_user = NULL;
    xinput_pad_state recorded = {0};
    (void)pthread_mutex_lock(&record_mutex);
    if (record_file != NULL && !record_failed) {
        if (poll_index < record_polls || poll_index == UINT64_MAX) record_failed = true;
        else {
            /* NULL means a declined/failed source update: retain the preceding state. */
            xinput_pad_state held = record_last_state;
            while (record_polls < poll_index && !record_failed) {
                append_state(&held); record_polls++;
            }
            write_pending_marks();
            if (!record_failed) { append_state(state != NULL ? state : &held); record_polls++; }
            drain_old();
            if (!record_failed && nav_observer != NULL) { /* T1640: the recorder side sees the state that was recorded (suppression applied) */
                observer = nav_observer;
                observer_user = nav_hook_user;
                recorded = record_last_state;
            }
        }
    }
    (void)pthread_mutex_unlock(&record_mutex);
    if (observer != NULL) observer(poll_index, &recorded, observer_user); /* outside the lock: it may add `# nav:` lines */
}

void xinput_record_close(void)
{
    (void)pthread_mutex_lock(&record_mutex);
    xinput_record_nav_close_fn closer = record_file != NULL ? nav_closer : NULL;
    void *closer_user = nav_hook_user;
    const uint64_t total = record_polls;
    nav_closer = NULL;
    nav_observer = NULL;
    (void)pthread_mutex_unlock(&record_mutex);
    if (closer != NULL) closer(total, closer_user); /* an activation still held at the end is closed at the last poll (T1640) */
    (void)pthread_mutex_lock(&record_mutex);
    if (record_file != NULL) {
        write_pending_marks();
        commit_open_run();
        drain_pending(pending_count);
        /* Flush the complete body before asserting completion. Failed writes have no trailer. */
        if (fflush(record_file) != 0) record_failed = true;
        if (!record_failed) {
            if (record_lossy && fprintf(record_file, "# lossy: digital bits outside the eight named buttons were dropped\n") < 0) record_failed = true;
            if (!record_failed && fprintf(record_file, "# polls: %llu\n", (unsigned long long)record_polls) < 0) record_failed = true;
        }
        if (fclose(record_file) != 0) record_failed = true;
        if (record_failed) fprintf(stderr, "input recording failed; completion is not guaranteed\n");
        record_file = NULL;
        suppressed_count = 0u;
    }
    (void)pthread_mutex_unlock(&record_mutex);
}

uint64_t xinput_record_poll_count(void)
{
    (void)pthread_mutex_lock(&record_mutex);
    const uint64_t polls = record_polls;
    (void)pthread_mutex_unlock(&record_mutex);
    return polls;
}

/* Require one terminal completion declaration, with strict decimal syntax and no
 * body after it. Existing v1 lossy/comment annotations remain compatible. */
static bool completion_polls(const char *text, size_t len, uint64_t *polls)
{
    bool found = false;
    size_t at = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        size_t start = at;
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) start++;
        if (end - start >= 8u && memcmp(text + start, "# polls:", 8u) == 0) {
            if (found || end == len) return false;
            size_t p = start + 8u;
            while (p < end && (text[p] == ' ' || text[p] == '\t')) p++;
            uint64_t value = 0u;
            size_t digits = p;
            while (p < end && text[p] >= '0' && text[p] <= '9') {
                unsigned digit = (unsigned)(text[p++] - '0');
                if (value > (UINT64_MAX - digit) / 10u) return false;
                value = value * 10u + digit;
            }
            if (p == digits) return false;
            while (p < end && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r')) p++;
            if (p != end) return false;
            *polls = value; found = true;
        } else if (found && start < end && text[start] != '#') return false;
        at = end + 1u;
    }
    return found;
}

bool xinput_replay_load(const char *path, const char *xbe_sha256, const char *flags_sha256, const char *flags,
                        char *error, size_t error_size, uint64_t *total_frames)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) { snprintf(error, error_size, "cannot open %s", path); return false; }
    size_t size = 0u, used = 0u;
    char *buffer = NULL;
    for (;;) {
        if (used == size) {
            if (size > SIZE_MAX / 2u) { free(buffer); fclose(file); snprintf(error, error_size, "record too large"); return false; }
            size = size ? size * 2u : 4096u;
            char *grown = realloc(buffer, size);
            if (grown == NULL) { free(buffer); fclose(file); snprintf(error, error_size, "out of memory"); return false; }
            buffer = grown;
        }
        const size_t got = fread(buffer + used, 1u, size - used, file);
        used += got;
        if (got == 0u) break;
    }
    bool read_failed = ferror(file) != 0;
    if (fclose(file) != 0) read_failed = true;
    uint64_t completed = 0u;
    if (read_failed || memchr(buffer, '\0', used) != NULL || !completion_polls(buffer, used, &completed)) {
        free(buffer); snprintf(error, error_size, "incomplete or invalid input record trailer"); return false;
    }
    if (!xinput_record_check_header(buffer, used, xbe_sha256, flags_sha256, flags, error, error_size)) {
        free(buffer);
        return false;
    }
    xinput_record_budgets budgets;
    const bool have_budgets = xinput_record_parse_budgets(buffer, used, &budgets);
    xinput_script *script = xinput_script_parse(buffer, used, error, error_size);
    char *buffer_copy = buffer;
    const size_t used_copy = used;
    if (script == NULL) { free(buffer_copy); return false; }
    if (completed != xinput_script_total_frames(script)) {
        free(buffer_copy); xinput_script_free(script); snprintf(error, error_size, "record completion count differs from body"); return false;
    }
    if (!parse_marks(buffer_copy, used_copy, completed, replay_marks, &replay_mark_count, error, error_size)) {
        free(buffer_copy); xinput_script_free(script); return false;
    }
    if (!parse_waits(buffer_copy, used_copy, replay_waits, &replay_wait_count, error, error_size)) {
        free(buffer_copy); xinput_script_free(script); return false;
    }
    if (!parse_navs(buffer_copy, used_copy, replay_navs, &replay_nav_count, error, error_size)) {
        free(buffer_copy); xinput_script_free(script); return false;
    }
    free(buffer_copy);
    if (total_frames != NULL) *total_frames = xinput_script_total_frames(script);
    replay_have_budgets = have_budgets;
    if (have_budgets) replay_budgets = budgets;
    replay_total_polls = xinput_script_total_frames(script);
    replay_script = script;
    xinput_script_install(script); /* lives for the process, like --pad-script's */
    return true;
}
