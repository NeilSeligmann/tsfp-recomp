/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_HOTKEY_H
#define TSFP_INPUT_XINPUT_HOTKEY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T1627: host HOTKEYS, a chord on the live gamepad or keyboard that makes the host write DIR/hotkey.<n> so a script
 * (tools.action_profile --advance-file) can continue a step without the owner changing window focus. FABRICATED
 * tooling input, not a game input: nothing here reaches the guest except that the chord is kept AWAY from it.
 *
 * Grammar (--hotkey):  DEVICE=KEY+KEY[+KEY...][@HOLDMS]:LABEL
 *   DEVICE  pad | kb. pad keys: A B X Y LB RB START BACK GUIDE LSTICK RSTICK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT.
 *           kb keys: A..Z 0..9 UP DOWN LEFT RIGHT ENTER BACKSPACE SPACE TAB F1 and the modifiers CTRL SHIFT ALT (either
 *           side, or LCTRL RCTRL LSHIFT RSHIFT LALT RALT). 2..5 distinct keys, case insensitive, stored upper case.
 *   HOLDMS  how long the whole chord must be held, 0..10000. Default 500 for pad, 0 for kb (fires when the last key lands).
 *   LABEL   1..31 of [A-Za-z0-9_-], written to the file (a script may filter on it).
 * The chord fires ONCE per press and re-arms when any of its keys is released.
 *
 * SWALLOWING (what the game does not see). The first key of a chord must be forwarded: the host cannot know a chord is
 * starting before the second key lands. From the moment TWO keys of a chord are held, every further chord key DOWN is
 * swallowed (never forwarded) and so is its UP, so the game never sees the second, third, ... key. A hotkey therefore
 * leaks its FIRST pressed key for as long as that key is held, choose the lead key to be one that does nothing in the
 * menus and in the preview, and press it first. Keys of the same device that are not in a chord are never touched. */
#define XINPUT_HOTKEY_MAX 8u
#define XINPUT_HOTKEY_KEYS_MAX 5u
#define XINPUT_HOTKEY_LABEL_MAX 32u
#define XINPUT_HOTKEY_NAME_MAX 16u
#define XINPUT_HOTKEY_HOLD_MAX_MS 10000u
#define XINPUT_HOTKEY_PAD_DEFAULT_HOLD_MS 500u
#define XINPUT_HOTKEY_SWALLOWED_MAX 16u

/* T1632: the default chords (the owner scripts and tools/hotkey_defaults.py use exactly these strings, a pytest parses this
 * header to keep the two in step). All pad chords share the lead BACK: it is the only key forwarded to the game, the rest of a
 * chord is swallowed only while the lead is held (so LB+RB or LB+X played normally still reach the game). The last key picks
 * the label. STOP is irreversible (it ends the run) so it needs a longer hold. */
#define XINPUT_HOTKEY_DEFAULT_PAD_ADVANCE "pad=BACK+START+LB+RB@500:advance"
#define XINPUT_HOTKEY_DEFAULT_PAD_MARK "pad=BACK+START+LB+X@500:mark"
#define XINPUT_HOTKEY_DEFAULT_PAD_DUMP "pad=BACK+START+LB+B@500:dump"
#define XINPUT_HOTKEY_DEFAULT_PAD_STOP "pad=BACK+START+LB+Y@1500:stop"
#define XINPUT_HOTKEY_DEFAULT_PAD_SHOT "pad=BACK+START+LB+A@500:shot"
#define XINPUT_HOTKEY_DEFAULT_KB_SHOT "kb=CTRL+SHIFT+S:shot"
#define XINPUT_HOTKEY_DEFAULT_KB_ADVANCE "kb=CTRL+SHIFT+N:advance"
#define XINPUT_HOTKEY_DEFAULT_KB_MARK "kb=CTRL+SHIFT+M:mark"
#define XINPUT_HOTKEY_DEFAULT_KB_DUMP "kb=CTRL+SHIFT+D:dump"
#define XINPUT_HOTKEY_DEFAULT_KB_STOP "kb=CTRL+SHIFT+O@1000:stop"

typedef enum { XINPUT_HOTKEY_DEVICE_KEYBOARD = 1, XINPUT_HOTKEY_DEVICE_PAD = 2 } xinput_hotkey_device;

/* T1632: labels the HOST acts on (see src/host/hotkey_actions.c), case insensitive. Every other label only writes the file.
 *   mark  place a record mark exactly like SIGUSR2 (`# mark: at=N` in the --record-input file, no effect without a recording)
 *   stop  end the run cleanly through the same path as closing the window (the recording gets its completion trailer) */
typedef enum { XINPUT_HOTKEY_ACTION_NONE = 0, XINPUT_HOTKEY_ACTION_MARK, XINPUT_HOTKEY_ACTION_STOP, XINPUT_HOTKEY_ACTION_SHOT } xinput_hotkey_action;
/* T1720: `shot` writes DIR/shot-NNN.png of the presented frame and a line in DIR/shots.manifest (src/host/shot_hotkey.c) */
xinput_hotkey_action xinput_hotkey_action_of(const char *label);

typedef struct {
    xinput_hotkey_device device;
    unsigned key_count;
    char keys[XINPUT_HOTKEY_KEYS_MAX][XINPUT_HOTKEY_NAME_MAX]; /* upper case, as written (CTRL stays CTRL) */
    unsigned hold_ms;
    char label[XINPUT_HOTKEY_LABEL_MAX];
} xinput_hotkey_spec;

/* Parse one --hotkey value. false with `error` (may be NULL) on any malformed or unknown part. `out` is unchanged on failure. */
bool xinput_hotkey_parse(const char *text, xinput_hotkey_spec *out, char *error, size_t error_size);

typedef struct xinput_hotkeys xinput_hotkeys;
/* T1632: LEAKED chord keys. A chord key pressed BEFORE its lead is held reaches the game (the host cannot know a gesture started).
 * When a chord that contains such a key fires, the key is "tainted": `begin` is true with the poll it went down, and when its UP is
 * forwarded later `begin` is false with the poll of the UP (the pad state of that poll no longer has the key). The recorder uses the
 * span [begin poll, end poll) to remove the key from the route. A key whose chord never fires is a genuine game press: no call. */
typedef void (*xinput_hotkey_leak_fn)(xinput_hotkey_device device, const char *name, uint64_t poll, bool begin, void *user);
void xinput_hotkeys_set_leak_observer(xinput_hotkeys *hotkeys, xinput_hotkey_leak_fn fn, void *user);
unsigned xinput_hotkeys_leak_overflow(const xinput_hotkeys *hotkeys); /* leaks not tracked because the table was full */
/* Called when a hotkey fires, after the file was written (or failed). `number` counts all hotkeys, from 1. May be NULL. */
typedef void (*xinput_hotkey_fire_fn)(unsigned number, const char *label, uint64_t poll, void *user);

/* `dir` (may be NULL: no file is written) is created if missing, each fire writes DIR/hotkey.<n> through a temporary file
 * and a rename. NULL with `error` for no specs, more than XINPUT_HOTKEY_MAX, or a dir that cannot be made. */
xinput_hotkeys *xinput_hotkeys_create(const xinput_hotkey_spec *specs, unsigned count, const char *dir,
                                      xinput_hotkey_fire_fn fire, void *user, char *error, size_t error_size);
void xinput_hotkeys_free(xinput_hotkeys *hotkeys);

/* One key or button event from `device` (NULL name = an unnamed key, always forwarded). `now_ms` is a monotonic
 * millisecond clock, `poll` the guest poll index (only reported). Returns true when the event is to be FORWARDED to the
 * pad source, false when it is swallowed. */
bool xinput_hotkeys_event(xinput_hotkeys *hotkeys, xinput_hotkey_device device, const char *name, bool down,
                          uint64_t now_ms, uint64_t poll);
/* Time passed with no event (call at every poll): a held chord whose hold time elapsed fires here. */
void xinput_hotkeys_tick(xinput_hotkeys *hotkeys, uint64_t now_ms, uint64_t poll);

unsigned xinput_hotkeys_fired(const xinput_hotkeys *hotkeys);       /* hotkeys fired so far */
unsigned xinput_hotkeys_write_failures(const xinput_hotkeys *hotkeys);
unsigned xinput_hotkeys_swallowed_total(const xinput_hotkeys *hotkeys); /* events kept from the game */
unsigned xinput_hotkeys_swallow_overflow(const xinput_hotkeys *hotkeys); /* swallow wanted but the table was full */
bool xinput_hotkeys_needs_device(const xinput_hotkeys *hotkeys, xinput_hotkey_device device);

#endif
