/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_SOURCE_H
#define TSFP_INPUT_XINPUT_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_hle.h"

/* T707: host-side controller input sources for the opt-in synthetic pad (FABRICATED, nothing is read from
 * hardware). A source answers "what is the pad doing at poll N", where N counts the title's XInputGetState calls
 * (the title polls once per frame, so N is a frame index). The source is the seam for later keyboard or host
 * gamepad mappings: they implement `xinput_source_fn` and install it, nothing else changes. Default: no source.
 *
 * BUTTON NAMES ARE INFERRED. Measured from the retail image: the digital word is at state+4, analog entries 6
 * and 7 are the triggers, thumbs are signed. NOT measured: which digital bit and which of analog 0..5 is which
 * face button. The mapping below is the XDK convention (bits UP 1, DOWN 2, LEFT 4, RIGHT 8, START 0x10, BACK
 * 0x20, LTHUMB 0x40, RTHUMB 0x80; analog A B X Y BLACK WHITE) and must be confirmed against the title's tests
 * of these bits at 0x0018FFE2 once a menu consumes them. */
#define XINPUT_BUTTON_DPAD_UP 0x0001u
#define XINPUT_BUTTON_DPAD_DOWN 0x0002u
#define XINPUT_BUTTON_DPAD_LEFT 0x0004u
#define XINPUT_BUTTON_DPAD_RIGHT 0x0008u
#define XINPUT_BUTTON_START 0x0010u
#define XINPUT_BUTTON_BACK 0x0020u
#define XINPUT_BUTTON_LEFT_THUMB 0x0040u
#define XINPUT_BUTTON_RIGHT_THUMB 0x0080u

/* Fill `out` for poll `poll_index` (0 based). Return false to leave the pad state unchanged. */
typedef bool (*xinput_source_fn)(uint64_t poll_index, xinput_pad_state *out, void *user);

/* Install (or with NULL remove) the source consulted by xinput_pad_state_read before each sample. */
void xinput_source_install(xinput_source_fn source, void *user);

/* Called by the pad adapter at each XInputGetState. Returns true if a source supplied a state and it was
 * installed on port 0 (xinput_hle_set_synthetic_pad_state). Counts every call. */
bool xinput_source_poll(void);
uint64_t xinput_source_poll_count(void);
void xinput_source_reset(void);

/* Explicit multiport provider. A callback only supplies the requested port; presence is
 * managed separately by xinput_pad_connect/disconnect. Legacy providers remain port0 and coexist: the ports provider takes precedence;
 * a false result falls back to the legacy callback on port0 only. Each has its own user. */
/* Install/uninstall and callbacks share device serialization. Uninstall completes
 * earlier callbacks before returning; then user state may be freed. A callback
 * must not free its own user state while still executing. */
typedef bool (*xinput_ports_source_fn)(uint64_t poll_index, unsigned port, xinput_pad_state *out, void *user);
void xinput_source_install_ports(xinput_ports_source_fn source, void *user);
bool xinput_source_poll_port(unsigned port);
/* T1153: observer called after every poll, outside the device lock, with the port and the number of polls that
 * port has had so far (including this one). Install before the guest threads start, NULL removes it. */
typedef void (*xinput_poll_observer_fn)(unsigned port, uint64_t port_polls, void *user);
void xinput_source_set_poll_observer(xinput_poll_observer_fn observer, void *user);
uint64_t xinput_source_port_poll_count(unsigned port);
/* T1629: read-only observer of the state a PORT 0 poll is about to install, called with the port 0 poll index (0 based)
 * and the new state AFTER the source supplied it and BEFORE xinput_hle_set_synthetic_pad_state installs it. It runs
 * INSIDE the device lock on the polling (guest) thread, so it must be short and must never call back into the xinput
 * device layer (xinput_*, including this setter), take a lock that code under the device lock can wait for, do file IO or
 * block. NULL removes it (zero cost when off: the pointer is read in a critical section the poll already holds).
 * Install/uninstall are serialized with the poll like the sources: once the call returns no hook call is in flight. */
typedef void (*xinput_pre_install_hook_fn)(uint64_t port_poll_index, const xinput_pad_state *state, void *user);
void xinput_source_set_pre_install_hook(xinput_pre_install_hook_fn hook, void *user);
/* Capability-gated host output. False means no supported motor output; guest USB
 * completion remains modeled separately. Zero/zero cancels on close/disconnect/reset. */
typedef bool (*xinput_feedback_fn)(unsigned port, uint16_t left, uint16_t right, void *user);
void xinput_feedback_install(xinput_feedback_fn feedback, void *user);
bool xinput_feedback_send(unsigned port, uint16_t left, uint16_t right);

/* ---- scripted source ----
 * Text file, one line per run of frames: `<frames> [TOKEN ...]`, `#` starts a comment, blank lines ignored.
 * Tokens: digital UP DOWN LEFT RIGHT START BACK LTHUMB RTHUMB, face A B X Y BLACK WHITE (analog 255),
 * `NAME=value` for A B X Y BLACK WHITE LT RT (0..255) and LX LY RX RY (-32768..32767). A line with no tokens is
 * the pad at rest. After the last line the pad returns to rest. Malformed input is refused with a line number. */
typedef struct {
    uint32_t frames;
    xinput_pad_state state;
} xinput_script_entry;

typedef struct xinput_script xinput_script;

/* Parse `text` (length `len`, not necessarily NUL terminated). NULL on error with `error` filled. */
xinput_script *xinput_script_parse(const char *text, size_t len, char *error, size_t error_size);
xinput_script *xinput_script_load(const char *path, char *error, size_t error_size);
void xinput_script_free(xinput_script *script);
size_t xinput_script_entry_count(const xinput_script *script);
uint64_t xinput_script_total_frames(const xinput_script *script);
/* State at poll `poll_index`; rest after the end. */
xinput_pad_state xinput_script_state_at(const xinput_script *script, uint64_t poll_index);
/* Install the script as the source. The script must outlive the source. */
void xinput_script_install(const xinput_script *script);

/* T1222: live pad, one line of tokens (script syntax, no frame count) in a file that is re-read at every poll. The
 * writer should replace the line with ONE pwrite of a fixed length padded with spaces. A bad line keeps the previous
 * state and is counted. The live pad must outlive the source. */
typedef struct xinput_live xinput_live;
bool xinput_live_parse_line(const char *text, size_t len, xinput_pad_state *out);
xinput_live *xinput_live_open(const char *path, char *error, size_t error_size);
void xinput_live_free(xinput_live *live);
uint64_t xinput_live_refused(const xinput_live *live);
void xinput_live_install(xinput_live *live);

#endif
