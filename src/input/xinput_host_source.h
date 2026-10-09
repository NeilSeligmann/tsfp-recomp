/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_HOST_SOURCE_H
#define TSFP_INPUT_XINPUT_HOST_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_source.h"

/* T751: host keyboard and host gamepad sources for the synthetic pad (FABRICATED, opt-in, never default).
 *
 * Both implement `xinput_source_fn` (xinput_source.h) over an abstract event feed. The feed is what a future
 * window (T744 interface, SDL3 chosen by the owner on 2026-10-04 and queued as T760, not built) or a device layer fills. Today the only
 * provider is `fake-feed`, a text file, so the mapping is exercised end to end. With no feed the sources refuse,
 * naming the missing provider. Nothing here reads hardware or a clock: events carry a POLL INDEX (the title's
 * XInputGetState count, one per frame) and an event with poll N is applied before sample N. A host timestamp to
 * poll index conversion belongs to the provider (and needs the virtual clock, T738 f).
 *
 * THE MAPPING TABLES (one table per device, in xinput_host_source.c, `keyboard_keys`, `gamepad_buttons`,
 * `gamepad_axes`). Every row is a FABRICATED convention (nothing measured says which key or button a player
 * uses), the digital bit and analog index names inherit the INFERRED XDK convention of xinput_source.h:
 *
 *   keyboard   arrows UP DOWN LEFT RIGHT -> dpad, ENTER -> START, BACKSPACE -> BACK, E -> LTHUMB, R -> RTHUMB,
 *              Z A  X B  A X  S Y  Q BLACK  W WHITE (analog 255), 1 -> LT, 2 -> RT (analog 255),
 *              I K J L -> left stick up down left right, T G F H -> right stick (full deflection +32767 or
 *              -32768, opposite keys held together cancel to 0). ESCAPE SPACE TAB F1 are known and UNMAPPED.
 *   gamepad    buttons A B X Y -> analog A B X Y (255), LB -> BLACK, RB -> WHITE (analog 255), START BACK, LSTICK
 *              RSTICK, DPAD_UP/DOWN/LEFT/RIGHT. Axes LX LY RX RY pass through unchanged (clamped to int16),
 *              LT RT (host 0..32767) scale as value >> 7 (255 at full, 0 at or below 0).
 *
 * ANALOG HANDLING: a pressure byte below 0x20 reaches the title as 0 (MEASURED against the original, T717,
 * applied by the pad adapter when the title reads, not here, so a source may report 0x1F and the title sees 0).
 * Thumb values are never thresholded (measured: passed unchanged). NO stick dead zone and NO trigger dead zone
 * is invented. Trigger scaling and keyboard stick full deflection are FABRICATED.
 *
 * PACKET NUMBER (T731): the original counts every raw report change. This source applies each event as its own
 * report through xinput_hle_set_synthetic_pad_state, so two changes between polls count two, and an event that
 * leaves the state unchanged (repeat key-down, unmapped key) counts none. */

typedef enum { XINPUT_HOST_KEYBOARD = 1, XINPUT_HOST_GAMEPAD = 2 } xinput_host_device;
typedef enum { XINPUT_HOST_DOWN = 1, XINPUT_HOST_UP = 2, XINPUT_HOST_AXIS = 3 } xinput_host_action;

/* `code` is an index into the device's name table (xinput_host_code_of), `value` only for AXIS. */
typedef struct {
    uint64_t poll;
    xinput_host_device device;
    xinput_host_action action;
    uint32_t code;
    int32_t value;
} xinput_host_event;

/* The provider seam. `next_due` returns true and fills `out` for the next event whose poll is at or before
 * `poll_index` (in order), false when none is due. `close` may be NULL. */
typedef struct {
    bool (*next_due)(void *user, uint64_t poll_index, xinput_host_event *out);
    void (*close)(void *user);
    void *user;
} xinput_event_feed;

typedef struct xinput_host_source xinput_host_source;

/* Source over a caller supplied feed. `device` selects which events apply, the other device's events are
 * counted and ignored. Takes ownership of the feed (closed by xinput_host_source_free). */
xinput_host_source *xinput_host_source_create(xinput_host_device device, xinput_event_feed feed);
void xinput_host_source_free(xinput_host_source *source);
/* Install as the pad source. The source must outlive the installation. */
void xinput_host_source_install(xinput_host_source *source);
/* The source function itself, for tests. */
bool xinput_host_source_fn(uint64_t poll_index, xinput_pad_state *out, void *user);

uint64_t xinput_host_source_applied(const xinput_host_source *source);
/* Events naming a code outside the device's table: ignored, counted, logged once each. */
uint64_t xinput_host_source_unknown_ignored(const xinput_host_source *source);
/* Events for the other device, and known keys or buttons with no mapping: ignored and counted. */
uint64_t xinput_host_source_other_device_ignored(const xinput_host_source *source);
uint64_t xinput_host_source_unmapped_ignored(const xinput_host_source *source);

/* Name tables: code <-> name (case insensitive, NULL or UINT32_MAX when unknown). */
const char *xinput_host_code_name(xinput_host_device device, uint32_t code);
uint32_t xinput_host_code_of(xinput_host_device device, const char *name);
size_t xinput_host_code_count(xinput_host_device device);
/* Axis names for gamepad AXIS events: LX LY RX RY LT RT (code = index). */
uint32_t xinput_host_axis_of(const char *name);

/* ---- providers ----
 * `fake-feed` text file, one event per line, `#` comments, polls non-decreasing:
 *     <poll> keyboard down|up <KEY>          <poll> gamepad down|up <BUTTON>
 *     <poll> gamepad axis <AXIS> <value>
 * <KEY>/<BUTTON> is a table name or `CODE:<n>` (a raw code, used to exercise the unknown-code path). Malformed
 * input is refused with a line number, an unknown device and an unknown name included. */
bool xinput_fake_feed_parse(const char *text, size_t len, xinput_event_feed *feed, char *error, size_t error_size);
bool xinput_fake_feed_load(const char *path, xinput_event_feed *feed, char *error, size_t error_size);

typedef enum { XINPUT_PAD_SOURCE_SCRIPT = 0, XINPUT_PAD_SOURCE_KEYBOARD, XINPUT_PAD_SOURCE_GAMEPAD } xinput_pad_source_kind;
/* "script" "keyboard" "gamepad"; false for anything else. */
bool xinput_pad_source_kind_parse(const char *text, xinput_pad_source_kind *kind);
/* Open the keyboard or gamepad source on the `fake-feed` file `feed_path`. With feed_path NULL there is no
 * provider: returns NULL and the error names the missing provider (window library for the keyboard, device layer
 * for the gamepad, both OWNER DECISIONS) and the fake-feed alternative. */
xinput_host_source *xinput_pad_source_open(xinput_pad_source_kind kind, const char *feed_path, char *error,
                                           size_t error_size);

/* Same, over a feed the caller built (the SDL window provider, src/host/pad_sdl_feed.h). Takes ownership of
 * the feed, also when it returns NULL. */
xinput_host_source *xinput_pad_source_open_feed(xinput_pad_source_kind kind, xinput_event_feed feed, char *error,
                                                size_t error_size);

/* T1632: what one key or button sets in the pad state, for the recorder to take a hotkey chord key out of a route. False when the
 * name is unknown or the key is UNMAPPED (it never reaches the game, so there is nothing to take out). Otherwise `digital` is the
 * bit mask, `analog` the index into xinput_pad_state.analog (-1 none), `stick` the thumb axis 0 LX 1 LY 2 RX 3 RY (-1 none). */
bool xinput_host_key_effect(xinput_host_device device, const char *name, uint16_t *digital, int *analog, int *stick);

#endif
