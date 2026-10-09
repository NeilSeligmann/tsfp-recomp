/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hotkey_actions.h"
#include <stdio.h>
#include <string.h>
#include "xinput_record.h"

void hotkey_actions_init(hotkey_actions *actions, xinput_host_device source_device, const hotkey_action_hooks *hooks)
{
    memset(actions, 0, sizeof *actions);
    actions->source_device = source_device;
    if (hooks != NULL) actions->hooks = *hooks;
}

void hotkey_actions_fire(unsigned number, const char *label, uint64_t poll, void *context)
{
    hotkey_actions *actions = context;
    if (actions == NULL) return;
    switch (xinput_hotkey_action_of(label)) {
    case XINPUT_HOTKEY_ACTION_MARK:
        if (actions->hooks.mark != NULL && actions->hooks.mark(actions->hooks.user)) {
            actions->marks_queued++;
        } else {
            actions->marks_refused++;
            fprintf(stderr, "hotkey (T1632): 'mark' #%u at host poll %llu ignored, no --record-input recording is open\n", number,
                    (unsigned long long)poll);
        }
        break;
    case XINPUT_HOTKEY_ACTION_STOP:
        actions->stops_requested++;
        fprintf(stderr, "hotkey (T1632): 'stop' #%u at host poll %llu, ending the run cleanly\n", number, (unsigned long long)poll);
        if (actions->hooks.stop != NULL) actions->hooks.stop(actions->hooks.user, "stop hotkey (T1632)");
        break;
    case XINPUT_HOTKEY_ACTION_SHOT:
        if (actions->hooks.shot != NULL && actions->hooks.shot(actions->hooks.user, number, label, poll)) {
            actions->shots_taken++;
        } else {
            actions->shots_refused++;
        }
        break;
    default:
        break;
    }
}

void hotkey_actions_leak(xinput_hotkey_device device, const char *name, uint64_t poll, bool begin, void *context)
{
    hotkey_actions *actions = context;
    if (actions == NULL) return;
    const xinput_host_device key_device = device == XINPUT_HOTKEY_DEVICE_PAD ? XINPUT_HOST_GAMEPAD : XINPUT_HOST_KEYBOARD;
    xinput_record_effect effect;
    uint16_t digital = 0u;
    int analog = -1, stick = -1;
    if (key_device != actions->source_device || !xinput_host_key_effect(key_device, name, &digital, &analog, &stick)) {
        if (begin) actions->leaks_ignored++;
        return;
    }
    effect.digital_mask = digital;
    effect.analog = analog;
    effect.stick = stick;
    if (begin) {
        actions->leaks_trimmed++;
        xinput_record_suppress_begin(&effect, poll);
    } else {
        xinput_record_suppress_end(&effect, poll);
    }
}
