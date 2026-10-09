/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1632: what the host does with a hotkey (src/host/hotkey_actions.c): the `mark` and `stop` label actions through fake hooks, and the
 * leak observer that takes a chord key out of the RECORD through the real recorder. No SDL, no guest, no window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hotkey_actions.h"
#include "xinput_record.h"

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static unsigned mark_calls, stop_calls;
static bool mark_result = true;
static char stop_cause[80];
static bool fake_mark(void *user)
{
    CHECK(user == &mark_calls);
    mark_calls++;
    return mark_result;
}
static void fake_stop(void *user, const char *cause)
{
    CHECK(user == &mark_calls);
    stop_calls++;
    snprintf(stop_cause, sizeof stop_cause, "%s", cause);
}

static void test_label_actions(void)
{
    hotkey_actions actions;
    const hotkey_action_hooks hooks = {fake_mark, fake_stop, &mark_calls, NULL};
    hotkey_actions_init(&actions, XINPUT_HOST_GAMEPAD, &hooks);
    hotkey_actions_fire(1u, "advance", 10u, &actions);
    hotkey_actions_fire(2u, "dump", 11u, &actions);
    hotkey_actions_fire(3u, "marker", 12u, &actions);
    CHECK(mark_calls == 0u && stop_calls == 0u); /* only the file for every other label */
    hotkey_actions_fire(4u, "mark", 13u, &actions);
    CHECK(mark_calls == 1u && actions.marks_queued == 1u && stop_calls == 0u);
    hotkey_actions_fire(5u, "MARK", 14u, &actions);
    CHECK(mark_calls == 2u && actions.marks_queued == 2u);
    mark_result = false; /* no recording open: counted as refused, never fatal */
    hotkey_actions_fire(6u, "mark", 15u, &actions);
    CHECK(mark_calls == 3u && actions.marks_queued == 2u && actions.marks_refused == 1u);
    hotkey_actions_fire(7u, "stop", 16u, &actions);
    CHECK(stop_calls == 1u && actions.stops_requested == 1u && strstr(stop_cause, "stop hotkey") != NULL);
    hotkey_actions_fire(8u, "Stop", 17u, &actions);
    CHECK(stop_calls == 2u);
    /* tolerated: no context, no hooks */
    hotkey_actions_fire(9u, "stop", 18u, NULL);
    hotkey_actions blank;
    hotkey_actions_init(&blank, XINPUT_HOST_KEYBOARD, NULL);
    hotkey_actions_fire(10u, "mark", 19u, &blank);
    hotkey_actions_fire(11u, "stop", 20u, &blank);
    CHECK(blank.marks_refused == 1u && blank.stops_requested == 1u && stop_calls == 2u);
}

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *text = calloc(1u, 1u << 16);
    if (text != NULL) { size_t got = fread(text, 1u, (1u << 16) - 1u, file); text[got] = '\0'; }
    fclose(file);
    return text;
}

/* Record 8 polls of a held key with the leak observer, return the text. */
static char *record_with_leak(xinput_host_device source, xinput_hotkey_device device, const char *name, unsigned held_analog,
                              uint16_t held_digital, int16_t held_stick_x, const char *path)
{
    char error[200];
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    hotkey_actions actions;
    hotkey_actions_init(&actions, source, NULL);
    for (unsigned poll = 0u; poll < 8u; poll++) {
        xinput_pad_state state = {0};
        state.analog[held_analog] = 255u;
        state.digital_buttons = held_digital;
        state.thumb_left_x = held_stick_x;
        if (poll == 0u) hotkey_actions_leak(device, name, 0u, true, &actions);
        if (poll == 6u) hotkey_actions_leak(device, name, 6u, false, &actions);
        xinput_record_observe(poll, 0u, &state);
    }
    xinput_record_close();
    char *text = slurp(path);
    remove(path);
    return text;
}

static void test_leak_mapping(void)
{
    /* pad LB is the BLACK analog button: cut from polls 0..5, back at 6 */
    char *text = record_with_leak(XINPUT_HOST_GAMEPAD, XINPUT_HOTKEY_DEVICE_PAD, "LB", 4u, 0u, 0, "/tmp/t1632_leak_lb.rec");
    CHECK(text != NULL && strstr(text, "\n6\n2 BLACK=255\n") != NULL);
    free(text);
    /* BACK is a digital bit */
    text = record_with_leak(XINPUT_HOST_GAMEPAD, XINPUT_HOTKEY_DEVICE_PAD, "BACK", 0u, XINPUT_BUTTON_BACK, 0, "/tmp/t1632_leak_back.rec");
    CHECK(text != NULL && strstr(text, "\n6 A=255\n2 BACK A=255\n") != NULL);
    free(text);
    /* a keyboard stick key (J = left stick X) with a keyboard source: the axis is zeroed */
    text = record_with_leak(XINPUT_HOST_KEYBOARD, XINPUT_HOTKEY_DEVICE_KEYBOARD, "J", 0u, 0u, -32768, "/tmp/t1632_leak_j.rec");
    CHECK(text != NULL && strstr(text, "\n6 A=255\n2 A=255 LX=-32768\n") != NULL);
    free(text);
    /* GUIDE and CTRL map to nothing, a keyboard key with a gamepad source never reached the game: the record is untouched */
    text = record_with_leak(XINPUT_HOST_GAMEPAD, XINPUT_HOTKEY_DEVICE_PAD, "GUIDE", 4u, 0u, 0, "/tmp/t1632_leak_guide.rec");
    CHECK(text != NULL && strstr(text, "\n8 BLACK=255\n") != NULL);
    free(text);
    text = record_with_leak(XINPUT_HOST_GAMEPAD, XINPUT_HOTKEY_DEVICE_KEYBOARD, "Z", 0u, 0u, 0, "/tmp/t1632_leak_z.rec");
    CHECK(text != NULL && strstr(text, "\n8 A=255\n") != NULL); /* keyboard Z is the A button, but the source is the gamepad */
    free(text);
    text = record_with_leak(XINPUT_HOST_KEYBOARD, XINPUT_HOTKEY_DEVICE_KEYBOARD, "LCTRL", 4u, 0u, 0, "/tmp/t1632_leak_ctrl.rec");
    CHECK(text != NULL && strstr(text, "\n8 BLACK=255\n") != NULL);
    free(text);
    /* counters */
    hotkey_actions actions;
    hotkey_actions_init(&actions, XINPUT_HOST_GAMEPAD, NULL);
    hotkey_actions_leak(XINPUT_HOTKEY_DEVICE_PAD, "GUIDE", 1u, true, &actions);
    hotkey_actions_leak(XINPUT_HOTKEY_DEVICE_PAD, "GUIDE", 2u, false, &actions);
    CHECK(actions.leaks_ignored == 1u && actions.leaks_trimmed == 0u);
    hotkey_actions_leak(XINPUT_HOTKEY_DEVICE_PAD, "BACK", 1u, true, &actions); /* no recording open: counted, a no-op */
    CHECK(actions.leaks_trimmed == 1u);
    hotkey_actions_leak(XINPUT_HOTKEY_DEVICE_PAD, "BACK", 1u, true, NULL);
}

/* T1720b: the whole gesture through the REAL chord engine, hook table and recorder. A pad chord (lead BACK) is pressed, held until it
 * fires, released, 12 polls at 100 ms each. `lead_first` false presses LB and START before BACK (they reach the game, the leak
 * observer must cut them). Forwarded events change the pad state the recorder sees, swallowed ones do not. */
static unsigned g_shot_calls;
static bool fake_shot(void *user, unsigned number, const char *label, uint64_t poll)
{
    (void)user;
    (void)number;
    (void)label;
    (void)poll;
    g_shot_calls++;
    return true;
}

typedef struct {
    xinput_pad_state state;
    xinput_hotkeys *hotkeys;
    uint64_t now_ms;
} gesture_run;

static void gesture_key(gesture_run *run, const char *name, bool down, uint64_t poll)
{
    if (run->hotkeys == NULL || !xinput_hotkeys_event(run->hotkeys, XINPUT_HOTKEY_DEVICE_PAD, name, down, run->now_ms, poll)) return;
    uint16_t digital = 0u;
    int analog = -1, stick = -1;
    if (!xinput_host_key_effect(XINPUT_HOST_GAMEPAD, name, &digital, &analog, &stick)) return;
    if (down) {
        run->state.digital_buttons = (uint16_t)(run->state.digital_buttons | digital);
        if (analog >= 0) run->state.analog[analog] = 255u;
    } else {
        run->state.digital_buttons = (uint16_t)(run->state.digital_buttons & ~digital);
        if (analog >= 0) run->state.analog[analog] = 0u;
    }
}

/* returns the record text; `chord` NULL records the no-gesture control run. */
static char *record_gesture(const char *chord, const char *last_key, bool lead_first, bool observe_leaks, const char *path,
                            hotkey_actions *actions)
{
    char error[200];
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    gesture_run run;
    memset(&run, 0, sizeof run);
    xinput_hotkey_spec spec;
    if (chord != NULL) {
        CHECK(xinput_hotkey_parse(chord, &spec, error, sizeof error));
        run.hotkeys = xinput_hotkeys_create(&spec, 1u, NULL, hotkey_actions_fire, actions, error, sizeof error);
        CHECK(run.hotkeys != NULL);
        if (observe_leaks) xinput_hotkeys_set_leak_observer(run.hotkeys, hotkey_actions_leak, actions);
    }
    const char *down_order_lead[] = {"BACK", "START", "LB", last_key};
    const char *down_order_late[] = {"LB", "START", "BACK", last_key};
    const char **down = lead_first ? down_order_lead : down_order_late;
    for (uint64_t poll = 0u; poll < 30u; poll++) {
        run.now_ms = poll * 100u;
        if (chord != NULL && poll >= 1u && poll <= 4u) gesture_key(&run, down[poll - 1u], true, poll);
        if (chord != NULL && poll >= 20u && poll <= 23u) gesture_key(&run, down[23u - poll], false, poll);
        if (run.hotkeys != NULL) xinput_hotkeys_tick(run.hotkeys, run.now_ms, poll);
        xinput_record_observe(poll, 0u, &run.state);
    }
    xinput_record_close();
    xinput_hotkeys_free(run.hotkeys);
    char *text = slurp(path);
    remove(path);
    return text;
}

static void test_shot_chord_in_recording(void)
{
    const char *path = "/tmp/t1720b_gesture.rec";
    hotkey_actions control_actions;
    hotkey_actions_init(&control_actions, XINPUT_HOST_GAMEPAD, NULL);
    char *control = record_gesture(NULL, "A", true, true, path, &control_actions);
    CHECK(control != NULL && strstr(control, "\n30\n") != NULL); /* 30 neutral polls, nothing else */
    const struct {
        const char *spec, *last;
    } chords[] = {{XINPUT_HOTKEY_DEFAULT_PAD_SHOT, "A"}, {XINPUT_HOTKEY_DEFAULT_PAD_MARK, "X"}, {XINPUT_HOTKEY_DEFAULT_PAD_STOP, "Y"}};
    for (unsigned c = 0u; c < 3u; c++) {
        for (unsigned order = 0u; order < 2u; order++) {
            const bool lead_first = order == 0u;
            unsigned marks_before = mark_calls, stops_before = stop_calls;
            g_shot_calls = 0u;
            mark_result = true;
            hotkey_actions actions;
            const hotkey_action_hooks hooks = {fake_mark, fake_stop, &mark_calls, fake_shot};
            hotkey_actions_init(&actions, XINPUT_HOST_GAMEPAD, &hooks);
            char *text = record_gesture(chords[c].spec, chords[c].last, lead_first, true, path, &actions);
            /* the gesture fired exactly once and did the action of ITS label only */
            CHECK(actions.shots_taken + actions.marks_queued + actions.stops_requested == 1u);
            CHECK(c != 0u || (g_shot_calls == 1u && actions.shots_taken == 1u && mark_calls == marks_before && stop_calls == stops_before &&
                              actions.marks_queued == 0u && actions.stops_requested == 0u));
            CHECK(c != 1u || (g_shot_calls == 0u && mark_calls == marks_before + 1u && stop_calls == stops_before));
            CHECK(c != 2u || (g_shot_calls == 0u && stop_calls == stops_before + 1u && mark_calls == marks_before));
            /* the keys that reached the game are cut: the route is byte for byte the no-gesture route */
            CHECK(text != NULL && control != NULL && strcmp(text, control) == 0);
            CHECK(order == 0u || actions.leaks_trimmed >= 2u); /* LB and START (and BACK) went down before the lead held */
            free(text);
        }
    }
    /* control of the control: without the leak observer the lead (or the early keys) stays in the record, so the comparison above
     * does have teeth */
    hotkey_actions plain;
    const hotkey_action_hooks plain_hooks = {NULL, NULL, NULL, fake_shot};
    hotkey_actions_init(&plain, XINPUT_HOST_GAMEPAD, &plain_hooks);
    char *untrimmed = record_gesture(XINPUT_HOTKEY_DEFAULT_PAD_SHOT, "A", true, false, path, &plain);
    CHECK(untrimmed != NULL && control != NULL && strcmp(untrimmed, control) != 0 && strstr(untrimmed, "BACK") != NULL);
    free(untrimmed);
    free(control);
}

int main(void)
{
    test_label_actions();
    test_leak_mapping();
    test_shot_chord_in_recording();
    printf("test_hotkey_actions: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
