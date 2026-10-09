/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1629: --dump-on-button. A guest memory dump just BEFORE and just AFTER each digital button press or release of pad
 * port 0, with a JSONL manifest, so the effect of a weapon switch, fire, reload or pickup on the player record can be
 * diffed. PASSIVE and READ-ONLY: nothing is ever written into the guest. Evidence class: xemu-level MEASURED observation of
 * the unmodified game.
 *
 * This file is the PURE core: the button mask, the edge detector, the event scheduler (coalescing, caps, after offsets,
 * idle controls), one bounded writer thread and the manifest. It has NO guest dependency: the snapshot capture, the file
 * write of a snapshot, the present counter and the arming condition are injected (button_dump_ops), so
 * tests/c/test_button_dump.c drives it with fakes. The real wiring (guest_dump snapshots, the xinput pre install hook,
 * the route handover) is button_dump_host.c.
 *
 * THREADS. button_dump_poll runs on ONE thread (the guest's pad poll, under the device lock): it only pays the 16 bit
 * compare per poll and, at an edge or a due after offset, one capture (ops.capture, reads only) plus a queue push. It never
 * formats text, never does file IO and never blocks. The single writer thread formats and writes the dump files (ops.write
 * must be atomic: tmp file + rename), then the manifest lines, flushing each.
 */
#ifndef TSFP_HOST_BUTTON_DUMP_H
#define TSFP_HOST_BUTTON_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xinput_hle.h"

#define BUTTON_DUMP_BUTTONS 16u
#define BUTTON_DUMP_MAX_AFTER 4u
#define BUTTON_DUMP_MAX_EDGES 64u /* edges LISTED per event, more are counted in "coalesced" only */
#define BUTTON_DUMP_MAX_PENDING_LIMIT 64u
#define BUTTON_DUMP_QUEUE_BYTES (64ull << 20) /* snapshots queued for the writer, beyond that an edge is dropped */
#define BUTTON_DUMP_LABEL_MAX 200u
/* At most this many "dropped" manifest lines per run. Further drops only move the counters (the end line "dropped" is the TOTAL), after
 * the last line ONE marker {"type":"dropped","poll":P,"reason":"suppressed","edges":[],"mono_ms":M} is written. */
#define BUTTON_DUMP_MAX_DROP_LINES 200u

/* Bit i of the 16 bit button mask: 0..7 are the digital word bits (UP DOWN LEFT RIGHT START BACK LTHUMB RTHUMB, the
 * XINPUT_BUTTON_* bits 0..7), 8..15 are analog[0..7] (A B X Y BLACK WHITE LT RT), down when the pressure byte is over the
 * threshold: the title itself turns every analog button into a game bit with `> 0x3C` (T707, 0x47DF40 table). */
extern const char *const button_dump_names[BUTTON_DUMP_BUTTONS];
/* The title's own game bit per button (informative, recorded in the manifest header). */
extern const uint16_t button_dump_game_bits[BUTTON_DUMP_BUTTONS];
uint16_t button_dump_mask(const xinput_pad_state *state, uint8_t threshold);

typedef struct {
    unsigned after_count;
    uint32_t after[BUTTON_DUMP_MAX_AFTER]; /* strictly increasing polls after the edge poll */
    uint8_t threshold;                     /* analog button down when > threshold */
    unsigned coalesce;                     /* an edge within this many polls of the newest event's first edge merges */
    unsigned max_pending;                  /* 1..BUTTON_DUMP_MAX_PENDING_LIMIT events in flight */
    unsigned max_dumps;                    /* total dump files */
    uint64_t max_bytes;                    /* total dump bytes, estimated with bytes_per_dump */
    uint64_t start_poll;                   /* no events before this port 0 poll */
    bool after_replay;                     /* events only while ops.armed says the replay handed over */
    bool forced_state;                     /* the host runs with --forced-state (guarded pokes): said in the start line, evidence class */
    unsigned idle_every;                   /* 0 = off */
    unsigned max_idle;
    uint64_t bytes_per_dump;               /* text bytes of one dump file (estimate) */
    uint64_t snapshot_bytes;               /* memory one queued snapshot holds */
    uint64_t queue_limit;                  /* 0 = BUTTON_DUMP_QUEUE_BYTES (raised to fit one whole event) */
    unsigned ranges;                       /* manifest header only */
    const char *dump_dir;                  /* manifest header, files go to dump_dir/buttons/ */
} button_dump_config;

typedef struct {
    /* Guest thread. Read only snapshot of the dump set, NULL when it failed (recorded as an unreadable dump). */
    void *(*capture)(void *user);
    /* Writer thread. Format `snapshot` into `path`, ATOMICALLY (tmp + rename), `header` holds '#' lines. True when every
     * range was read. */
    bool (*write)(void *user, void *snapshot, const char *path, const char *header);
    /* Writer thread. Free the snapshot (after write, or unwritten at shutdown). */
    void (*discard)(void *user, void *snapshot);
    /* Guest thread. The present index recorded with each capture (may be NULL: 0). */
    uint64_t (*present)(void *user);
    /* Guest thread. NULL: always armed. Used with config.after_replay. */
    bool (*armed)(void *user);
    /* Optional clock (tests): monotonic and unix wall milliseconds. NULL: the real clocks. */
    void (*clock)(void *user, uint64_t *monotonic_ms, uint64_t *wall_unix_ms);
    /* Optional line sink for the live counters (writer thread or the finisher). NULL: stderr. */
    void (*log)(void *user, const char *line);
    void *user;
} button_dump_ops;

typedef struct button_dump button_dump;

typedef struct {
    unsigned events;          /* real events opened */
    unsigned idle_events;
    unsigned dropped;         /* every dropped edge group; the manifest has at most BUTTON_DUMP_MAX_DROP_LINES lines + one marker */
    unsigned coalesced_edges;
    unsigned dumps_committed; /* files admitted (the max-dumps count) */
    unsigned dumps_written;   /* files that exist */
    uint64_t bytes_written;
    /* Cost, measured with the real monotonic clock: the capture runs on the guest thread, the write on the writer. */
    unsigned captures;
    uint64_t capture_ns_total;
    uint64_t capture_ns_max;
    unsigned writes;
    uint64_t write_ns_total;
    uint64_t write_ns_max;
} button_dump_stats;

/* Creates dump_dir/buttons, truncates and starts dump_dir/buttons.jsonl with the start line, starts the writer. NULL
 * with `error` on a bad config or IO failure. The ops and config are copied (the dump_dir string too). */
button_dump *button_dump_create(const button_dump_config *config, const button_dump_ops *ops, char *error, size_t error_size);

/* One pad poll of port 0: `poll` is the port 0 poll index, `state` the state about to be installed. Single thread. */
void button_dump_poll(button_dump *dump, uint64_t poll, const xinput_pad_state *state);

/* Wait until the writer has written everything queued so far (tests, and before reading files). */
void button_dump_flush(button_dump *dump);

void button_dump_stats_get(button_dump *dump, button_dump_stats *out);

/* Name and close every open event (complete:false listing what exists), drain and stop the writer, write the end line
 * with `reason` ("exit" or "signal") and the closing counters line. Idempotent. The dump object stays valid for
 * stats until button_dump_destroy. */
void button_dump_finish(button_dump *dump, const char *reason);
void button_dump_destroy(button_dump *dump);

#endif /* TSFP_HOST_BUTTON_DUMP_H */
