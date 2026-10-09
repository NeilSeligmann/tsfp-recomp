/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_RECORD_H
#define TSFP_INPUT_XINPUT_RECORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_hle.h"
#include "xinput_source.h"

/* T1074: record and replay of the pad (docs/input-replay.md). The clock is the title's XInputGetState poll
 * count of port 0 (one per present in the title's loop, a function of the guest, never of wall time).
 * A record file is a versioned header of `#` comments followed by the existing --pad-script body
 * (`<frames> [TOKEN ...]`), so it is also a valid --pad-script file:
 *   # tsfp-input v1
 *   # xbe-sha256: <64 hex>
 *   # flags-sha256: <64 hex>
 *   # flags: <canonical identity flags>
 *   <frames> TOKENS...
 *   # polls: N            (trailer, written at close)
 * FABRICATED input like the script. Port 0 legacy path only (not --controllers). */
#define XINPUT_RECORD_VERSION 1

/* Canonical identity of a command line: every argument except the ones that do not change what the title sees
 * (paths, presentation, audio, budgets, the record/replay/pad-source flags themselves), joined by single spaces.
 * argv[0] and the first positional (the XBE path) are skipped. False if `out` is too small. */
bool xinput_record_identity_flags(int argc, char *const *argv, char *out, size_t out_size);

/* Canonical form of an already recorded `# flags:` line (T1126), so records made with per-run output paths still
 * compare equal. Output-only path flags are dropped with their value, input path values become "<path>". */
bool xinput_record_canonical_line(const char *line, char *out, size_t out_size);

/* T1618: the flags that differ between two canonical identity lines, as units (a flag with its values). `only_recorded` gets the
 * units of the recording that this run lacks, `only_current` those of this run that the recording lacks, each `a b, c`, ended by
 * "..." when it does not fit. True when anything differs. The refusal of a replay prints these two lists only. */
bool xinput_record_flags_diff(const char *recorded, const char *current, char *only_recorded, size_t recorded_size,
                              char *only_current, size_t current_size);
/* Advice appended to the flag set refusal (the route and the replay must run with the same flags). */
#define XINPUT_RECORD_REFLAG_ADVICE \
    "The route and the replay must run with identical game flags. Record the route again with tmp/record_mapmaker_route.fish " \
    "(it builds the same flags as tmp/run_forced_weapons.fish) and replay that file, or replay with the flags of the recording."

/* T1126: the wait budgets and the interactive mode a recording ran with, written as one `# budgets:` header line
 * (outside the identity). A replay adopts them unless the command line gives its own. */
typedef struct {
    unsigned owner_waits, worker_blanks, thread_timeout_ms;
    bool interactive;
} xinput_record_budgets;
size_t xinput_record_format_budgets(char *out, size_t out_size, const xinput_record_budgets *budgets);
bool xinput_record_parse_budgets(const char *text, size_t len, xinput_record_budgets *budgets);
/* Call before xinput_record_open to have the line written. */
void xinput_record_set_budgets(const xinput_record_budgets *budgets);
/* After a successful xinput_replay_load: the loaded file's budgets (false for a record without the line) and the
 * recorded poll count, for the exit report of polls consumed. */
bool xinput_replay_budgets(xinput_record_budgets *budgets);
uint64_t xinput_replay_total_polls(void);

/* T1616: marks. With --record-input the host can be sent SIGUSR2 (xinput_record_enable_marks installs the handler and
 * writes `<record>.pid`): each signal writes a comment line `# mark: at=N` into the body, N = the number of polls
 * recorded so far, so the line sits between the runs before and after it. A replay reads them back in order. Marks are
 * how the owner says "the slow screen has finished here" (the route replay waits there, docs/input-replay.md). */
bool xinput_record_enable_marks(const char *record_path);
unsigned xinput_record_marks_written(void);
/* After a successful xinput_replay_load: the marks (positions, non decreasing) and the loaded script (owned here). */
size_t xinput_replay_marks(const uint64_t **marks);
const xinput_script *xinput_replay_script(void);
/* T1633: the `# wait: markK:fields` lines of the loaded record (event waits, same grammar as --route-wait-event). */
#define XINPUT_REPLAY_WAIT_LEN 256u
size_t xinput_replay_wait_count(void);
const char *xinput_replay_wait(size_t index);
/* T1640: the `# nav: at=P to=Q menu=ID select=SEL ...` lines of the loaded record (closed loop menu navigation, grammar and
 * parser in xinput_route_nav_spec.h), kept as text after the `# nav:` prefix, at most 64 of at most 512 characters. */
#define XINPUT_REPLAY_NAV_LEN 513u
size_t xinput_replay_nav_count(void);
const char *xinput_replay_nav(size_t index);
/* T1633: while recording, each mark also writes `# mark-info: markK <text>` when this callback returns true with text (what
 * the host observed since the previous mark). A comment: replay ignores it, tools.route_events reads it. */
typedef bool (*xinput_record_mark_facts_fn)(unsigned mark, char *out, size_t out_size, void *user);
void xinput_record_set_mark_facts(xinput_record_mark_facts_fn fn, void *user);

/* T1632: a mark requested without a signal (the `mark` hotkey, src/host/hotkey_actions.c): the same effect as SIGUSR2, the line
 * `# mark: at=N` is written at the next poll. False when no recording is open (nothing is queued). At most
 * XINPUT_ROUTE_MAX_MARKS marks are kept, a request beyond that is reported on stderr and dropped. */
bool xinput_record_request_mark(void);

/* T1632: take a pad effect out of the RECORD (never out of what the game sees). A hotkey chord key that reached the game before the
 * chord was recognised must not be in the route, replaying it would press that key at the same screen. The effect is the part of
 * the pad state one key sets: digital bits, one analog button (index into xinput_pad_state.analog, -1 none) and one thumb axis
 * (0 LX 1 LY 2 RX 3 RY, -1 none). suppress_begin clears the effect in the recorded polls from `from_poll` on (history is kept
 * pending for XINPUT_RECORD_TAIL_LAG polls, an older start is clipped and reported) and in every poll recorded afterwards, until
 * suppress_end. Neighbouring equal runs are merged again so the trimmed route is text identical to one never pressed. Both are
 * no-ops without an open recording. */
typedef struct {
    uint16_t digital_mask;
    int analog;
    int stick;
} xinput_record_effect;
void xinput_record_suppress_begin(const xinput_record_effect *effect, uint64_t from_poll);
void xinput_record_suppress_end(const xinput_record_effect *effect, uint64_t at_poll);
/* T1640: the recorder side of the closed loop menu navigation (src/input/xinput_nav_record.c). `observer` is called after every recorded port 0
 * poll, outside the record lock, with the state that was recorded (hotkey suppression applied). `closer` once at xinput_record_close with the poll
 * count. `logger` receives event log lines ("nav-skip ...") that are made inside the record (a suppressed line dropped later). */
typedef void (*xinput_record_nav_observer_fn)(uint64_t poll_index, const xinput_pad_state *recorded, void *user);
typedef void (*xinput_record_nav_close_fn)(uint64_t total_polls, void *user);
typedef void (*xinput_record_nav_log_fn)(const char *line, void *user);
void xinput_record_set_nav_hooks(xinput_record_nav_observer_fn observer, xinput_record_nav_close_fn closer, xinput_record_nav_log_fn logger, void *user);
/* Add the comment `# nav: at=AT to=TO BODY` ("menu=ID select=id:I activate") to the record, in the pending list at the position of AT (marks and runs
 * keep their order). NULL when it was added, else why not: "limit-64", "at-above-to", "overlap" (AT is raised to the end of the previous nav line,
 * then it is past TO), "mark-inside" (a mark strictly inside (AT,TO)), "suppressed" (the select press at EDGE_POLL is no longer in the pending
 * record), "not-recording". `select_analog` (0..5) or `select_digital` name the select button of the menu. A line is dropped later when a hotkey
 * suppression takes its select press out (logged through `logger`). */
const char *xinput_record_add_nav(uint64_t at, uint64_t to, uint64_t edge_poll, const char *body, int select_analog, uint16_t select_digital);
unsigned xinput_record_navs_written(void);
unsigned xinput_record_trims(void);         /* suppress_begin calls that changed history this recording */
unsigned xinput_record_trims_clipped(void); /* of which the start was older than what was still pending */

/* Header text for a record: "# tsfp-input v1\n# xbe-sha256: ...". */
size_t xinput_record_header(char *out, size_t out_size, const char *xbe_sha256, const char *flags_sha256,
                            const char *flags);

/* Check a record file's header against the current identity. Returns true when it matches. On false `error`
 * says exactly what differs (version, xbe, flag set) and nothing is installed. `body_offset` receives 0. */
bool xinput_record_check_header(const char *text, size_t len, const char *xbe_sha256, const char *flags_sha256,
                                const char *flags, char *error, size_t error_size);

/* Format one run as a script line, `<frames> TOKEN ...\n`. Returns the length, 0 if too small. Sets `*lossy`
 * when digital bits outside the eight named buttons were dropped. */
size_t xinput_record_format_run(char *out, size_t out_size, uint64_t frames, const xinput_pad_state *state,
                                bool *lossy);

/* Recording. open writes the header. Port 0 states seen by xinput_source_poll_port are run length coded.
 * close (also atexit) writes the last run and the trailer. */
bool xinput_record_open(const char *path, const char *xbe_sha256, const char *flags_sha256, const char *flags,
                        char *error, size_t error_size);
void xinput_record_close(void);
uint64_t xinput_record_poll_count(void);
/* Called by the source layer after a state was installed on `port`. */
/* NULL state records a declined update, retaining the preceding recorded state. */
void xinput_record_observe(uint64_t poll_index, unsigned port, const xinput_pad_state *state);

/* Replay: verify the header, parse the body with xinput_script_parse, install it. The script is owned here. */
bool xinput_replay_load(const char *path, const char *xbe_sha256, const char *flags_sha256, const char *flags,
                        char *error, size_t error_size, uint64_t *total_frames);

#endif
