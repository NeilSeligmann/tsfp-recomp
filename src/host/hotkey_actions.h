/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_HOTKEY_ACTIONS_H
#define TSFP_HOST_HOTKEY_ACTIONS_H
#include <stdbool.h>
#include <stdint.h>
#include "xinput_host_source.h"
#include "xinput_hotkey.h"

/* T1632: what the HOST does with a hotkey, beyond writing DIR/hotkey.<n> (T1627). Tooling only, FABRICATED: nothing here is a game
 * input. Two things.
 *
 * 1. Label actions (xinput_hotkey_action_of): `mark` places a record mark exactly like SIGUSR2 (the `# mark: at=N` comment of the
 *    --record-input file), `stop` ends the run through the same path as closing the window, so a recording gets its trailer. The
 *    hotkey fires on the guest thread inside the pad poll, so both only set a flag that the normal paths consume. Every other label
 *    only writes the file. The file is written first in every case (a script can follow what happened).
 * 2. The leak observer: a chord key that reached the game before the chord was recognised (pressed with no lead held) is taken
 *    out of the RECORDED route (xinput_record_suppress_begin/end) from the poll it went down to the poll it came up, so replaying
 *    the route never presses the hotkey gesture at a menu. What the game saw live is not changed (a replay of the route
 *    differs from the live run only by that key). Keys that map to nothing (GUIDE, CTRL, SHIFT) have no effect and are skipped,
 *    and so is a key of the other device than the pad source (a keyboard key with a gamepad source never reached the game). */
typedef struct {
    bool (*mark)(void *user);                         /* true when a mark was queued (a recording is open) */
    void (*stop)(void *user, const char *cause);      /* ask for the clean shutdown, `cause` is static text */
    void *user;
    bool (*shot)(void *user, unsigned number, const char *label, uint64_t poll); /* T1720: take a shot, NULL = unavailable */
} hotkey_action_hooks;

typedef struct {
    hotkey_action_hooks hooks;
    xinput_host_device source_device; /* the pad source of the run: only its keys can have reached the game */
    unsigned marks_queued, marks_refused, stops_requested, shots_taken, shots_refused, leaks_trimmed, leaks_ignored;
} hotkey_actions;

void hotkey_actions_init(hotkey_actions *actions, xinput_host_device source_device, const hotkey_action_hooks *hooks);
/* The xinput_hotkey_fire_fn: `context` is the hotkey_actions. */
void hotkey_actions_fire(unsigned number, const char *label, uint64_t poll, void *context);
/* The xinput_hotkey_leak_fn: `context` is the hotkey_actions. Calls xinput_record_suppress_begin/end. */
void hotkey_actions_leak(xinput_hotkey_device device, const char *name, uint64_t poll, bool begin, void *context);

#endif
