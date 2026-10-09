/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_PAD_SDL_FEED_H
#define TSFP_HOST_PAD_SDL_FEED_H
#include <stddef.h>
#include "present_sink.h"
#include "xinput_hotkey.h"
#include "xinput_host_source.h"

/* T751: the `sdl-window` provider of the host keyboard and gamepad pad sources. Adapts the window sink's input
 * queue (present_video_sink_input_next, pumped under the sink mutex, SDL-free events) to an xinput_event_feed.
 * Events carry no clock, each is due at the poll that pops it (poll index = the guest's XInputGetState count),
 * so the title reads whatever the window pump latched since its previous poll, one event per raw report (T731).
 * Unnamed host keys and buttons become a code outside the table and are counted as unknown by the source.
 * `device` is XINPUT_HOST_KEYBOARD or XINPUT_HOST_GAMEPAD. The sink must outlive the feed (it is not closed
 * with it). False with `error` set when the sink has no window or this build has no SDL3. */
bool pad_sdl_feed_open(present_video_sink *sink, xinput_host_device device, xinput_event_feed *feed, char *error,
                       size_t error_size);

/* T1627: the same feed with host HOTKEYS (xinput_hotkey.h). Every named raw key and button event of BOTH devices (the
 * sink is asked for both when a hotkey needs the other one) first goes through xinput_hotkeys_event: a swallowed chord
 * key never reaches the pad source, so the game does not see it. The feed ticks the hotkeys once per poll so a held pad
 * chord fires after its hold time with no further event. `hotkeys` (may be NULL = no hotkeys) must outlive the feed. */
bool pad_sdl_feed_open_hotkeys(present_video_sink *sink, xinput_host_device device, xinput_hotkeys *hotkeys,
                               xinput_event_feed *feed, char *error, size_t error_size);

#endif
