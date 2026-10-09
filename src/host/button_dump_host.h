/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1629: the host wiring of --dump-on-button (button_dump.h is the pure core). Guest snapshots come from
 * guest_dump_capture (read only, kernel_guest_read_bytes), files from guest_dump_snapshot_write (the exact text format
 * of the SIGUSR1 dumps, atomic), the pad poll from the xinput pre install hook (port 0), the present index from the
 * caller. Nothing here writes into the guest.
 */
#ifndef TSFP_HOST_BUTTON_DUMP_HOST_H
#define TSFP_HOST_BUTTON_DUMP_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest_dump.h"

typedef struct {
    const guest_dump_set *set;
    const char *dir; /* the --dump-guest-dir, files go to dir/buttons/, the manifest is dir/buttons.jsonl */
    unsigned after_count;
    uint32_t after[4];
    uint8_t threshold;
    unsigned coalesce;
    unsigned max_pending;
    unsigned max_dumps;
    uint64_t max_bytes;
    uint64_t start_poll;
    bool after_replay;
    bool forced_state; /* --forced-state: recorded in the manifest start line (the dump itself never pokes) */
    unsigned idle_every;
    unsigned max_idle;
    uint64_t (*present)(void); /* may be NULL */
    bool (*armed)(void);       /* route handover query, needed with after_replay */
} button_dump_host_config;

/* Create the dump, install the pre install hook on port 0. False with `error` on failure (nothing installed). */
bool button_dump_host_start(const button_dump_host_config *config, char *error, size_t error_size);
/* Remove the hook, flush every pending event as incomplete, write the end line. Safe when never started. */
void button_dump_host_stop(const char *reason);

/* The same for exit paths with guest threads still running (the interactive safepoint miss, the controllers watchdog, the unjoined
 * thread exit): a helper thread does the hook removal and the flush, this waits at most `timeout_ms`. True when it finished. The dump
 * is left open (end line missing, readers tolerate that) when the hook could not be removed in time. Safe when never started. */
bool button_dump_host_stop_bounded(const char *reason, unsigned timeout_ms);

#endif
