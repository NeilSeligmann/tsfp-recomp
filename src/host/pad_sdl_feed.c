/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* clock_gettime for the T1627 hotkey hold time */
#endif
#include "pad_sdl_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    present_video_sink *sink;
    xinput_host_device device;
    xinput_hotkeys *hotkeys; /* T1627, may be NULL */
    bool ticked;
    uint64_t ticked_poll;
} sdl_feed;

/* Host codes without a table name land here, far outside either table, and are counted by the source. */
#define UNNAMED_CODE_BASE 0x10000000u

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* T1627: false when the hotkeys swallow the event (a chord key the game must not see). */
static bool hotkeys_pass(sdl_feed *feed, const present_input_event *raw, uint64_t poll_index)
{
    if (feed->hotkeys == NULL) return true;
    bool down;
    xinput_hotkey_device device;
    switch (raw->kind) {
    case PRESENT_INPUT_KEY_DOWN: down = true; device = XINPUT_HOTKEY_DEVICE_KEYBOARD; break;
    case PRESENT_INPUT_KEY_UP: down = false; device = XINPUT_HOTKEY_DEVICE_KEYBOARD; break;
    case PRESENT_INPUT_PAD_DOWN: down = true; device = XINPUT_HOTKEY_DEVICE_PAD; break;
    case PRESENT_INPUT_PAD_UP: down = false; device = XINPUT_HOTKEY_DEVICE_PAD; break;
    default: return true; /* axes are never part of a chord */
    }
    return xinput_hotkeys_event(feed->hotkeys, device, raw->name, down, monotonic_ms(), poll_index);
}

static bool sdl_next_due(void *user, uint64_t poll_index, xinput_host_event *out)
{
    sdl_feed *feed = user;
    present_input_event raw;
    if (feed->hotkeys != NULL && (!feed->ticked || feed->ticked_poll != poll_index)) {
        feed->ticked = true;
        feed->ticked_poll = poll_index;
        xinput_hotkeys_tick(feed->hotkeys, monotonic_ms(), poll_index);
    }
    do {
        if (!present_video_sink_input_next(feed->sink, &raw)) return false;
    } while (!hotkeys_pass(feed, &raw, poll_index));
    out->poll = poll_index;
    out->value = raw.value;
    const bool keyboard = raw.kind == PRESENT_INPUT_KEY_DOWN || raw.kind == PRESENT_INPUT_KEY_UP;
    out->device = keyboard ? XINPUT_HOST_KEYBOARD : XINPUT_HOST_GAMEPAD;
    switch (raw.kind) {
    case PRESENT_INPUT_KEY_DOWN:
    case PRESENT_INPUT_PAD_DOWN: out->action = XINPUT_HOST_DOWN; break;
    case PRESENT_INPUT_KEY_UP:
    case PRESENT_INPUT_PAD_UP: out->action = XINPUT_HOST_UP; break;
    default: out->action = XINPUT_HOST_AXIS; break;
    }
    uint32_t code = UINT32_MAX;
    if (raw.name != NULL)
        code = out->action == XINPUT_HOST_AXIS ? xinput_host_axis_of(raw.name) : xinput_host_code_of(out->device, raw.name);
    out->code = code != UINT32_MAX ? code : UNNAMED_CODE_BASE + ((uint32_t)raw.raw & 0xFFFFFFu);
    return true;
}

static void sdl_close(void *user) { free(user); }

bool pad_sdl_feed_open(present_video_sink *sink, xinput_host_device device, xinput_event_feed *feed, char *error,
                       size_t error_size)
{
    return pad_sdl_feed_open_hotkeys(sink, device, NULL, feed, error, error_size);
}

bool pad_sdl_feed_open_hotkeys(present_video_sink *sink, xinput_host_device device, xinput_hotkeys *hotkeys,
                               xinput_event_feed *feed, char *error, size_t error_size)
{
    const char *reason = NULL;
    /* a hotkey on the other device needs that device's events in the queue too (the source ignores and counts them) */
    const bool keyboard = device == XINPUT_HOST_KEYBOARD ||
                          (hotkeys != NULL && xinput_hotkeys_needs_device(hotkeys, XINPUT_HOTKEY_DEVICE_KEYBOARD));
    const bool gamepad = device == XINPUT_HOST_GAMEPAD ||
                         (hotkeys != NULL && xinput_hotkeys_needs_device(hotkeys, XINPUT_HOTKEY_DEVICE_PAD));
    if (!present_video_sink_enable_input(sink, keyboard, gamepad, &reason)) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "%s", reason != NULL ? reason : "window input failed");
        return false;
    }
    sdl_feed *state = calloc(1u, sizeof(*state));
    if (state == NULL) return false;
    state->sink = sink;
    state->device = device;
    state->hotkeys = hotkeys;
    feed->next_due = sdl_next_due;
    feed->close = sdl_close;
    feed->user = state;
    return true;
}
