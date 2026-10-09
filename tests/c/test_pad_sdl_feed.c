/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T751: the SDL3 window provider of the host keyboard and gamepad pad sources, on SDL's dummy video driver
 * (ctest sets SDL_VIDEODRIVER=dummy, no display is touched). Real SDL events are injected with SDL_PushEvent (and
 * a real virtual joystick for the gamepad), pumped by the sink's vblank under its mutex, converted by
 * pad_sdl_feed.c, mapped by xinput_host_source.c and read back through the pad adapter's XInputGetState. The
 * TITLE-SIDE word is computed with the two conversion tables measured from the retail image (T707, pinned by
 * tests/test_xinput_button_map.py), written out here independently. Built without SDL3 the test SKIPS LOUDLY (77). */
#include "test_d3d8_support.h"
#include "pad_sdl_feed.h"
#include "hotkey_actions.h"
#include "xinput_record.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_host_source.h"
#include "xinput_source.h"
#ifdef TSFP_HAVE_SDL3
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define HANDLE 0x58504430u

#ifdef TSFP_HAVE_SDL3
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumbs[4];
} guest_state;

/* Measured title tables (T707): XPP digital mask to game bit, analog index (pressure over 0x3C) to game bit. */
static const uint16_t digital_masks[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static const uint16_t digital_game[8] = {0x04, 0x08, 0x01, 0x02, 0x200, 0x100, 0x1000, 0x2000};
static const uint16_t analog_game[8] = {0x80, 0x20, 0x10, 0x40, 0x4000, 0x8000, 0x400, 0x800};
static uint32_t title_word(const guest_state *state)
{
    uint32_t word = 0u;
    for (int i = 0; i < 8; i++) {
        if ((state->buttons & digital_masks[i]) != 0u) word |= digital_game[i];
        if (state->analog[i] > 0x3Cu) word |= analog_game[i];
    }
    return word;
}

static present_video_sink *sink;
static xinput_host_source *source;

static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_source_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    xinput_devices_enable_synthetic_pad(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), HANDLE);
}
static bool start(xinput_host_device device)
{
    const char *error = NULL;
    initialise();
    sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp pad test", &error);
    CHECK(sink != NULL);
    if (sink == NULL) {
        printf("  window did not open: %s\n", error != NULL ? error : "(no text)");
        return false;
    }
    char text[200];
    xinput_event_feed feed;
    CHECK(pad_sdl_feed_open(sink, device, &feed, text, sizeof(text)));
    source = xinput_pad_source_open_feed(device == XINPUT_HOST_KEYBOARD ? XINPUT_PAD_SOURCE_KEYBOARD
                                                                        : XINPUT_PAD_SOURCE_GAMEPAD,
                                         feed, text, sizeof(text));
    CHECK(source != NULL);
    xinput_host_source_install(source);
    return source != NULL;
}
static void finish(void)
{
    xinput_source_reset();
    xinput_host_source_free(source);
    present_video_sink_close(sink);
    source = NULL;
    sink = NULL;
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static void key(SDL_Keycode code, bool down, bool repeat)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = code;
    event.key.down = down;
    event.key.repeat = repeat;
    CHECK(SDL_PushEvent(&event));
}
static void button(SDL_JoystickID which, SDL_GamepadButton code, bool down)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = which;
    event.gbutton.button = (Uint8)code;
    event.gbutton.down = down;
    CHECK(SDL_PushEvent(&event));
}
static void axis(SDL_GamepadAxis which, int16_t value)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.axis = (Uint8)which;
    event.gaxis.value = value;
    CHECK(SDL_PushEvent(&event));
}
static void other(SDL_EventType type)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    CHECK(SDL_PushEvent(&event));
}
/* Window pump (one modelled vblank), then the guest poll. */
static guest_state poll(void)
{
    guest_state state;
    present_video_sink_vblank(sink);
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 22u);
    memcpy(&state.packet, at, 4u);
    memcpy(&state.buttons, at + 4u, 2u);
    memcpy(state.analog, at + 6u, 8u);
    memcpy(state.thumbs, at + 14u, 8u);
    return state;
}
static void expect_rest(const guest_state *state)
{
    CHECK_EQ_U32(state->buttons, 0u);
    for (int a = 0; a < 8; a++) CHECK_EQ_U32(state->analog[a], 0u);
    for (int s = 0; s < 4; s++) CHECK_EQ_U32((uint32_t)(int32_t)state->thumbs[s], 0u);
    CHECK_EQ_U32(title_word(state), 0u);
}

/* One expectation per SDL key of the documented map, independent of the source's table. */
typedef struct {
    SDL_Keycode key;
    uint16_t digital;
    int analog;
    int stick;
    int16_t stick_value;
} key_row;
static void test_keyboard_map_end_to_end(void)
{
    static const key_row rows[] = {
        {SDLK_UP, XINPUT_BUTTON_DPAD_UP, -1, -1, 0},      {SDLK_DOWN, XINPUT_BUTTON_DPAD_DOWN, -1, -1, 0},
        {SDLK_LEFT, XINPUT_BUTTON_DPAD_LEFT, -1, -1, 0},  {SDLK_RIGHT, XINPUT_BUTTON_DPAD_RIGHT, -1, -1, 0},
        {SDLK_RETURN, XINPUT_BUTTON_START, -1, -1, 0},    {SDLK_KP_ENTER, XINPUT_BUTTON_START, -1, -1, 0},
        {SDLK_BACKSPACE, XINPUT_BUTTON_BACK, -1, -1, 0},  {SDLK_E, XINPUT_BUTTON_LEFT_THUMB, -1, -1, 0},
        {SDLK_R, XINPUT_BUTTON_RIGHT_THUMB, -1, -1, 0},   {SDLK_Z, 0, 0, -1, 0},
        {SDLK_X, 0, 1, -1, 0},                            {SDLK_A, 0, 2, -1, 0},
        {SDLK_S, 0, 3, -1, 0},                            {SDLK_Q, 0, 4, -1, 0},
        {SDLK_W, 0, 5, -1, 0},                            {SDLK_1, 0, 6, -1, 0},
        {SDLK_2, 0, 7, -1, 0},                            {SDLK_I, 0, -1, 1, 32767},
        {SDLK_K, 0, -1, 1, -32768},                       {SDLK_J, 0, -1, 0, -32768},
        {SDLK_L, 0, -1, 0, 32767},                        {SDLK_T, 0, -1, 3, 32767},
        {SDLK_G, 0, -1, 3, -32768},                       {SDLK_F, 0, -1, 2, -32768},
        {SDLK_H, 0, -1, 2, 32767},
    };
    /* Title word each row must produce (game bit), independent of the tables above. */
    static const uint32_t game[sizeof(rows) / sizeof(rows[0])] = {
        0x04, 0x08, 0x01, 0x02, 0x200, 0x200, 0x100, 0x1000, 0x2000, 0x80, 0x20, 0x10, 0x40, 0x4000, 0x8000,
        0x400, 0x800, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!start(XINPUT_HOST_KEYBOARD)) return;
    for (size_t i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        key(rows[i].key, true, false);
        const guest_state down = poll();
        CHECK_EQ_U32(down.buttons, rows[i].digital);
        for (int a = 0; a < 8; a++) CHECK_EQ_U32(down.analog[a], a == rows[i].analog ? 255u : 0u);
        for (int s = 0; s < 4; s++)
            CHECK_EQ_U32((uint32_t)(int32_t)down.thumbs[s],
                         s == rows[i].stick ? (uint32_t)(int32_t)rows[i].stick_value : 0u);
        if (i < 17u) CHECK(title_word(&down) != 0u); /* non empty before equality */
        CHECK_EQ_U32(title_word(&down), game[i]);
        key(rows[i].key, false, false);
        expect_rest((const guest_state[]){poll()});
    }
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), (uint32_t)(2u * (sizeof(rows) / sizeof(rows[0]))));
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 0u);
    CHECK_EQ_U32((uint32_t)present_video_sink_input_dropped(sink), 0u);
    finish();
}
static void test_keyboard_edges(void)
{
    if (!start(XINPUT_HOST_KEYBOARD)) return;
    /* key repeat is dropped: no raw report, packet unchanged */
    key(SDLK_Z, true, false);
    guest_state state = poll();
    CHECK_EQ_U32(state.packet, 2u);
    CHECK_EQ_U32(state.analog[0], 255u);
    key(SDLK_Z, true, true);
    key(SDLK_Z, true, true);
    state = poll();
    CHECK_EQ_U32(state.packet, 2u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), 1u);
    /* two changes between guest polls are two raw reports, latched whole under the sink mutex */
    key(SDLK_UP, true, false);
    key(SDLK_DOWN, true, false);
    present_video_sink_vblank(sink);
    state = poll();
    CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_DPAD_UP | XINPUT_BUTTON_DPAD_DOWN);
    CHECK_EQ_U32(state.packet, 4u);
    /* focus loss releases every held key */
    other(SDL_EVENT_WINDOW_FOCUS_LOST);
    state = poll();
    expect_rest(&state);
    CHECK_EQ_U32(state.packet, 7u);
    /* an unnamed key and a gamepad event are not applied. The unnamed key is counted unknown by the source, the
     * gamepad event is not queued at all for a keyboard source */
    key(SDLK_F5, true, false);
    button(0, SDL_GAMEPAD_BUTTON_SOUTH, true);
    state = poll();
    expect_rest(&state);
    CHECK_EQ_U32(state.packet, 7u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_other_device_ignored(source), 0u);
    /* ESCAPE closes the window path and is queued as nothing, a known unmapped key (SPACE) counts unmapped */
    key(SDLK_ESCAPE, true, false);
    key(SDLK_SPACE, true, false);
    state = poll();
    expect_rest(&state);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unmapped_ignored(source), 1u);
    finish();
}
static void test_queue_overflow_is_counted(void)
{
    if (!start(XINPUT_HOST_KEYBOARD)) return;
    enum { PUSHED = 300 };
    /* SDL's own queue is larger than ours: 300 alternating Z events overflow the 256 slot sink queue by 44. */
    for (int i = 0; i < PUSHED; i++) key(SDLK_Z, (i % 2) == 0, false);
    present_video_sink_vblank(sink);
    CHECK_EQ_U32((uint32_t)present_video_sink_input_dropped(sink), (uint32_t)(PUSHED - PRESENT_INPUT_QUEUE_SIZE));
    const guest_state state = poll();
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), (uint32_t)PRESENT_INPUT_QUEUE_SIZE);
    /* 256 alternating events starting with a press end released, and each was its own raw report */
    expect_rest(&state);
    CHECK_EQ_U32(state.packet, 1u + PRESENT_INPUT_QUEUE_SIZE);
    finish();
}

typedef struct {
    SDL_GamepadButton sdl;
    uint16_t digital;
    int analog;
    uint32_t game;
} button_row;
static void test_gamepad_map_end_to_end(void)
{
    static const button_row rows[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0, 0, 0x80},
        {SDL_GAMEPAD_BUTTON_EAST, 0, 1, 0x20},
        {SDL_GAMEPAD_BUTTON_WEST, 0, 2, 0x10},
        {SDL_GAMEPAD_BUTTON_NORTH, 0, 3, 0x40},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0, 4, 0x4000},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0, 5, 0x8000},
        {SDL_GAMEPAD_BUTTON_START, XINPUT_BUTTON_START, -1, 0x200},
        {SDL_GAMEPAD_BUTTON_BACK, XINPUT_BUTTON_BACK, -1, 0x100},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_BUTTON_LEFT_THUMB, -1, 0x1000},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_BUTTON_RIGHT_THUMB, -1, 0x2000},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_BUTTON_DPAD_UP, -1, 0x04},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_BUTTON_DPAD_DOWN, -1, 0x08},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_BUTTON_DPAD_LEFT, -1, 0x01},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_BUTTON_DPAD_RIGHT, -1, 0x02},
    };
    if (!start(XINPUT_HOST_GAMEPAD)) return;
    for (size_t i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        button(1, rows[i].sdl, true);
        const guest_state down = poll();
        CHECK_EQ_U32(down.buttons, rows[i].digital);
        for (int a = 0; a < 8; a++) CHECK_EQ_U32(down.analog[a], a == rows[i].analog ? 255u : 0u);
        CHECK(title_word(&down) != 0u);
        CHECK_EQ_U32(title_word(&down), rows[i].game);
        button(1, rows[i].sdl, false);
        expect_rest((const guest_state[]){poll()});
    }
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), (uint32_t)(2u * (sizeof(rows) / sizeof(rows[0]))));
    /* a button with no table name (MISC1) is counted unknown, GUIDE is known and unmapped */
    button(1, SDL_GAMEPAD_BUTTON_MISC1, true);
    button(1, SDL_GAMEPAD_BUTTON_GUIDE, true);
    guest_state state = poll();
    expect_rest(&state);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unmapped_ignored(source), 1u);
    /* keyboard events are not queued for a gamepad source */
    key(SDLK_Z, true, false);
    state = poll();
    expect_rest(&state);
    finish();
}
static void test_gamepad_axes(void)
{
    if (!start(XINPUT_HOST_GAMEPAD)) return;
    axis(SDL_GAMEPAD_AXIS_LEFTX, 12345);
    guest_state state = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[0], 12345u);
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[1], 0u);
    /* SDL up is negative, XInput up is positive: the Y axes are negated (INFERRED), -32768 clamps to 32767 */
    axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
    axis(SDL_GAMEPAD_AXIS_RIGHTY, 20000);
    axis(SDL_GAMEPAD_AXIS_RIGHTX, -32768);
    state = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[1], 32767u);
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[3], (uint32_t)(int32_t)-20000);
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[2], (uint32_t)(int32_t)-32768);
    axis(SDL_GAMEPAD_AXIS_LEFTY, 32767);
    state = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)state.thumbs[1], (uint32_t)(int32_t)-32767);
    /* triggers scale value >> 7: full 255, 0x1F00 is 62 (over the 0x3C title threshold), 0x0FFF is 31 (below
     * the 0x20 adapter threshold, reads 0) */
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0x1F00);
    state = poll();
    CHECK_EQ_U32(state.analog[6], 255u);
    CHECK_EQ_U32(state.analog[7], 62u);
    CHECK_EQ_U32(title_word(&state) & 0xC00u, 0xC00u);
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0x0FFF);
    state = poll();
    CHECK_EQ_U32(state.analog[7], 0u);
    /* an axis SDL has no table name for is unknown and counted, never applied (9 is outside the SDL axis enum) */
    axis((SDL_GamepadAxis)9, 1000);
    state = poll();
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 1u);
    CHECK_EQ_U32(state.analog[6], 255u);
    finish();
}
static void test_virtual_gamepad_hotplug(void)
{
    if (!start(XINPUT_HOST_GAMEPAD)) return;
    CHECK_EQ_U32((uint32_t)present_video_sink_gamepads_open(sink), 0u);
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = "tsfp virtual pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
    CHECK(id != 0u);
    if (id == 0u) {
        printf("  virtual joystick failed: %s\n", SDL_GetError());
        finish();
        return;
    }
    (void)poll(); /* the pump sees SDL_EVENT_GAMEPAD_ADDED and opens it */
    CHECK_EQ_U32((uint32_t)present_video_sink_gamepads_open(sink), 1u);
    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    CHECK(joystick != NULL);
    if (joystick != NULL) {
        CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, true));
        guest_state state = poll();
        CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_START);
        CHECK_EQ_U32(title_word(&state), 0x200u);
        CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, false));
        state = poll();
        expect_rest(&state);
        SDL_CloseJoystick(joystick);
    }
    CHECK(SDL_DetachVirtualJoystick(id));
    (void)poll(); /* SDL_EVENT_GAMEPAD_REMOVED closes it */
    CHECK_EQ_U32((uint32_t)present_video_sink_gamepads_open(sink), 0u);
    finish();
}
/* The sink's own queue, popped directly (no source): exact kinds, names and order, FIFO wrap past the ring size,
 * held key tracking for the focus loss, both devices enabled in two calls. */
static void test_sink_raw_queue(void)
{
    const char *error = NULL;
    present_video_sink *raw = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp raw", &error);
    CHECK(raw != NULL);
    if (raw == NULL) return;
    present_input_event event;
    /* nothing is queued before input is enabled, the events are discarded not stored */
    key(SDLK_Z, true, false);
    present_video_sink_vblank(raw);
    CHECK(!present_video_sink_input_next(raw, &event));
    CHECK(present_video_sink_enable_input(raw, true, false, &error));
    button(0, SDL_GAMEPAD_BUTTON_SOUTH, true);
    axis(SDL_GAMEPAD_AXIS_LEFTX, 1000);
    present_video_sink_vblank(raw);
    CHECK(!present_video_sink_input_next(raw, &event)); /* gamepad not enabled yet, button and axis */
    CHECK(present_video_sink_enable_input(raw, false, true, &error)); /* keyboard stays on */
    key(SDLK_Z, true, false);
    key(SDLK_X, true, false);
    key(SDLK_Z, true, false); /* a second non repeat press is remembered once */
    key(SDLK_Z, false, false);
    other(SDL_EVENT_WINDOW_FOCUS_LOST);
    button(0, SDL_GAMEPAD_BUTTON_SOUTH, true);
    present_video_sink_vblank(raw);
    static const struct { present_input_kind kind; const char *name; } expected[] = {
        {PRESENT_INPUT_KEY_DOWN, "Z"}, {PRESENT_INPUT_KEY_DOWN, "X"}, {PRESENT_INPUT_KEY_DOWN, "Z"},
        {PRESENT_INPUT_KEY_UP, "Z"}, {PRESENT_INPUT_KEY_UP, "X"}, {PRESENT_INPUT_PAD_DOWN, "A"},
    };
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); i++) {
        CHECK(present_video_sink_input_next(raw, &event));
        CHECK_EQ_U32(event.kind, expected[i].kind);
        CHECK(event.name != NULL && strcmp(event.name, expected[i].name) == 0);
    }
    CHECK(!present_video_sink_input_next(raw, &event));
    /* enabling one device again never turns the other off */
    CHECK(present_video_sink_enable_input(raw, true, false, &error));
    button(0, SDL_GAMEPAD_BUTTON_EAST, true);
    axis(SDL_GAMEPAD_AXIS_LEFTX, 77);
    present_video_sink_vblank(raw);
    CHECK(present_video_sink_input_next(raw, &event));
    CHECK_EQ_U32(event.kind, PRESENT_INPUT_PAD_DOWN);
    CHECK(present_video_sink_input_next(raw, &event));
    CHECK_EQ_U32(event.kind, PRESENT_INPUT_PAD_AXIS);
    CHECK_EQ_U32((uint32_t)event.value, 77u);
    CHECK(!present_video_sink_input_next(raw, &event));
    /* the ring wraps: 3 rounds of 200 axis events keep their order and values */
    int next = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 200; i++) axis(SDL_GAMEPAD_AXIS_LEFTX, (int16_t)(round * 200 + i));
        present_video_sink_vblank(raw);
        for (int i = 0; i < 200; i++) {
            CHECK(present_video_sink_input_next(raw, &event));
            CHECK_EQ_U32(event.kind, PRESENT_INPUT_PAD_AXIS);
            CHECK_EQ_U32((uint32_t)event.value, (uint32_t)next);
            next++;
        }
        CHECK(!present_video_sink_input_next(raw, &event));
    }
    CHECK_EQ_U32((uint32_t)present_video_sink_input_dropped(raw), 0u);
    present_video_sink_close(raw);
    /* a sink that is not a window refuses with a reason */
    present_video_sink *null_sink = present_video_sink_open(PRESENT_VIDEO_NULL, "tsfp null", &error);
    CHECK(null_sink != NULL);
    const char *reason = NULL;
    CHECK(!present_video_sink_enable_input(null_sink, true, false, &reason));
    CHECK(reason != NULL && strstr(reason, "--present window") != NULL);
    CHECK(!present_video_sink_input_next(null_sink, &event));
    present_video_sink_close(null_sink);
}
/* The adapter itself: the fields of the event it hands the source, the poll it is due at included. */
static void test_adapter_event_fields(void)
{
    const char *error = NULL;
    present_video_sink *window = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp feed", &error);
    CHECK(window != NULL);
    if (window == NULL) return;
    char text[120];
    xinput_event_feed keys;
    CHECK(pad_sdl_feed_open(window, XINPUT_HOST_KEYBOARD, &keys, text, sizeof(text)));
    xinput_host_event event;
    key(SDLK_Z, true, false);
    key(SDLK_Z, false, false);
    key(SDLK_F5, true, false);
    present_video_sink_vblank(window);
    CHECK(keys.next_due(keys.user, 7u, &event));
    CHECK_EQ_U32((uint32_t)event.poll, 7u);
    CHECK_EQ_U32(event.device, XINPUT_HOST_KEYBOARD);
    CHECK_EQ_U32(event.action, XINPUT_HOST_DOWN);
    CHECK_EQ_U32(event.code, xinput_host_code_of(XINPUT_HOST_KEYBOARD, "Z"));
    CHECK(event.code != 0u);
    CHECK(keys.next_due(keys.user, 8u, &event));
    CHECK_EQ_U32((uint32_t)event.poll, 8u);
    CHECK_EQ_U32(event.action, XINPUT_HOST_UP);
    CHECK(keys.next_due(keys.user, 9u, &event)); /* F5 has no name: a code far outside the table */
    CHECK(event.code >= xinput_host_code_count(XINPUT_HOST_KEYBOARD));
    CHECK(xinput_host_code_name(XINPUT_HOST_KEYBOARD, event.code) == NULL);
    CHECK(!keys.next_due(keys.user, 10u, &event));
    keys.close(keys.user);
    xinput_event_feed pad;
    CHECK(pad_sdl_feed_open(window, XINPUT_HOST_GAMEPAD, &pad, text, sizeof(text)));
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 4321);
    button(0, SDL_GAMEPAD_BUTTON_NORTH, false);
    present_video_sink_vblank(window);
    CHECK(pad.next_due(pad.user, 3u, &event));
    CHECK_EQ_U32(event.device, XINPUT_HOST_GAMEPAD);
    CHECK_EQ_U32(event.action, XINPUT_HOST_AXIS);
    CHECK_EQ_U32(event.code, xinput_host_axis_of("RT"));
    CHECK_EQ_U32((uint32_t)event.value, 4321u);
    CHECK(pad.next_due(pad.user, 4u, &event));
    CHECK_EQ_U32(event.device, XINPUT_HOST_GAMEPAD);
    CHECK_EQ_U32(event.action, XINPUT_HOST_UP);
    CHECK_EQ_U32(event.code, xinput_host_code_of(XINPUT_HOST_GAMEPAD, "Y"));
    pad.close(pad.user);
    present_video_sink_close(window);
}
static void test_no_window_refused(void)
{
    char error[200] = "";
    xinput_event_feed feed;
    CHECK(!pad_sdl_feed_open(NULL, XINPUT_HOST_KEYBOARD, &feed, error, sizeof(error)));
    CHECK(strstr(error, "--present window") != NULL);
    const char *reason = NULL;
    CHECK(!present_video_sink_enable_input(NULL, true, false, &reason));
    CHECK(reason != NULL);
}
/* T1627: host hotkeys over the real window feed. The chord's first key reaches the pad, the rest is swallowed, the file
 * appears, the other device's chord works although the source is the gamepad, and a held pad chord fires on a later poll
 * with no further event (the feed ticks the hotkeys at every poll). */
static bool file_text(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    const size_t got = fread(out, 1u, size - 1u, file);
    out[got] = '\0';
    fclose(file);
    return true;
}
static void test_hotkeys_end_to_end(void)
{
    char dir[160], path[220], text[120];
    snprintf(dir, sizeof dir, "/tmp/test_pad_sdl_feed_hotkeys_%d", (int)getpid());
    snprintf(path, sizeof path, "rm -rf %s", dir);
    CHECK(system(path) == 0);
    const char *specs[3] = {"pad=BACK+START+LB@0:padgo", "kb=CTRL+SHIFT+N@0:kbgo", "pad=DPAD_UP+DPAD_DOWN@150:slow"};
    xinput_hotkey_spec parsed[3];
    char error[160];
    for (unsigned i = 0u; i < 3u; i++) CHECK(xinput_hotkey_parse(specs[i], &parsed[i], error, sizeof error));
    xinput_hotkeys *hotkeys = xinput_hotkeys_create(parsed, 3u, dir, NULL, NULL, error, sizeof error);
    CHECK(hotkeys != NULL);
    const char *window_error = NULL;
    initialise();
    sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp hotkey test", &window_error);
    CHECK(sink != NULL);
    if (sink == NULL || hotkeys == NULL) return;
    xinput_event_feed feed;
    CHECK(pad_sdl_feed_open_hotkeys(sink, XINPUT_HOST_GAMEPAD, hotkeys, &feed, error, sizeof error));
    source = xinput_pad_source_open_feed(XINPUT_PAD_SOURCE_GAMEPAD, feed, error, sizeof error);
    CHECK(source != NULL);
    xinput_host_source_install(source);
    /* the lead BACK reaches the pad, START and LB do not */
    button(1, SDL_GAMEPAD_BUTTON_BACK, true);
    button(1, SDL_GAMEPAD_BUTTON_START, true);
    button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    guest_state state = poll();
    CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_BACK);
    CHECK_EQ_U32(state.analog[4], 0u);
    CHECK_EQ_U32(xinput_hotkeys_fired(hotkeys), 1u);
    snprintf(path, sizeof path, "%s/hotkey.1", dir);
    CHECK(file_text(path, text, sizeof text) && strncmp(text, "1 padgo poll=", 13) == 0);
    button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    button(1, SDL_GAMEPAD_BUTTON_START, false);
    button(1, SDL_GAMEPAD_BUTTON_BACK, false);
    state = poll();
    expect_rest(&state); /* nothing stuck, the swallowed releases did not leak either */
    /* a button outside every chord is untouched */
    button(1, SDL_GAMEPAD_BUTTON_SOUTH, true);
    state = poll();
    CHECK_EQ_U32(state.analog[0], 255u);
    button(1, SDL_GAMEPAD_BUTTON_SOUTH, false);
    state = poll();
    expect_rest(&state);
    /* the keyboard chord works with a GAMEPAD source (the sink was asked for keyboard events too) and never moves the pad */
    key(SDLK_LCTRL, true, false);
    key(SDLK_LSHIFT, true, false);
    key(SDLK_N, true, false);
    state = poll();
    expect_rest(&state);
    CHECK_EQ_U32(xinput_hotkeys_fired(hotkeys), 2u);
    snprintf(path, sizeof path, "%s/hotkey.2", dir);
    CHECK(file_text(path, text, sizeof text) && strncmp(text, "2 kbgo poll=", 12) == 0);
    key(SDLK_N, false, false);
    key(SDLK_LSHIFT, false, false);
    key(SDLK_LCTRL, false, false);
    state = poll();
    expect_rest(&state);
    /* a held pad chord with a hold time fires on a LATER poll without any new event */
    button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    button(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    state = poll();
    CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_DPAD_UP);
    CHECK_EQ_U32(xinput_hotkeys_fired(hotkeys), 2u);
    SDL_Delay(300);
    state = poll();
    CHECK_EQ_U32(xinput_hotkeys_fired(hotkeys), 3u);
    snprintf(path, sizeof path, "%s/hotkey.3", dir);
    CHECK(file_text(path, text, sizeof text) && strncmp(text, "3 slow poll=", 12) == 0);
    button(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    state = poll();
    expect_rest(&state);
    CHECK_EQ_U32((uint32_t)xinput_hotkeys_write_failures(hotkeys), 0u);
    finish();
    xinput_hotkeys_free(hotkeys);
    snprintf(path, sizeof path, "rm -rf %s", dir);
    CHECK(system(path) == 0);
}
/* T1632: RECORDING with the hotkey chords. The route recorded while the owner makes the mark gesture (or the stop gesture) must be
 * byte identical to the route of the same game input with no gesture, so a replay never presses BACK, START, LB or X at a menu.
 * Three gesture orders: the lead first (BACK leaks), a selector first (X and BACK leak), and the stop chord (the lead is still
 * held when the run ends). The "clean" run does the same game input and requests the mark itself at the same poll. */
typedef enum { GESTURE_NONE, GESTURE_LEAD_FIRST, GESTURE_SELECTOR_FIRST, GESTURE_STOP } gesture_kind;
static char *read_whole(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *text = calloc(1u, 1u << 16);
    if (text != NULL) { const size_t got = fread(text, 1u, (1u << 16) - 1u, file); text[got] = '\0'; }
    fclose(file);
    return text;
}
static bool record_mark_hook(void *user)
{
    (void)user;
    return xinput_record_request_mark();
}
static void stop_hook_close(void *user, const char *cause)
{
    present_video_sink_request_close(user, cause);
}
static void run_record_scenario(gesture_kind kind, const char *path, unsigned *marks, bool *closed, char *cause, size_t cause_size)
{
    char error[200];
    xinput_hotkey_spec specs[2];
    CHECK(xinput_hotkey_parse("pad=BACK+START+LB+X@0:mark", &specs[0], error, sizeof error));
    CHECK(xinput_hotkey_parse("pad=BACK+START+LB+Y@0:stop", &specs[1], error, sizeof error));
    initialise();
    const char *window_error = NULL;
    sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp record hotkey test", &window_error);
    CHECK(sink != NULL);
    if (sink == NULL) return;
    hotkey_actions actions;
    const hotkey_action_hooks hooks = {record_mark_hook, stop_hook_close, sink, NULL};
    hotkey_actions_init(&actions, XINPUT_HOST_GAMEPAD, &hooks);
    xinput_hotkeys *hotkeys = xinput_hotkeys_create(specs, 2u, NULL, hotkey_actions_fire, &actions, error, sizeof error);
    CHECK(hotkeys != NULL);
    xinput_hotkeys_set_leak_observer(hotkeys, hotkey_actions_leak, &actions);
    xinput_event_feed feed;
    CHECK(pad_sdl_feed_open_hotkeys(sink, XINPUT_HOST_GAMEPAD, hotkeys, &feed, error, sizeof error));
    source = xinput_pad_source_open_feed(XINPUT_PAD_SOURCE_GAMEPAD, feed, error, sizeof error);
    CHECK(source != NULL);
    xinput_host_source_install(source);
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof error));
    const bool clean = kind == GESTURE_NONE;
    const SDL_GamepadButton selector = kind == GESTURE_STOP ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST; /* Y, X */
    for (unsigned step = 0u; step < 16u; step++) {
        if (clean && step == 4u) CHECK(xinput_record_request_mark()); /* the mark the gesture would have placed */
        switch (step) {
        case 1: button(1, SDL_GAMEPAD_BUTTON_SOUTH, true); break;     /* genuine A press */
        case 2: button(1, SDL_GAMEPAD_BUTTON_SOUTH, false); break;
        case 3:
            if (kind == GESTURE_LEAD_FIRST || kind == GESTURE_STOP) button(1, SDL_GAMEPAD_BUTTON_BACK, true);
            if (kind == GESTURE_SELECTOR_FIRST) button(1, selector, true);
            break;
        case 4:
            if (kind == GESTURE_SELECTOR_FIRST) button(1, SDL_GAMEPAD_BUTTON_BACK, true);
            if (kind != GESTURE_NONE && kind != GESTURE_SELECTOR_FIRST) {
                button(1, SDL_GAMEPAD_BUTTON_START, true);
                button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                button(1, selector, true);
            } else if (kind == GESTURE_SELECTOR_FIRST) {
                button(1, SDL_GAMEPAD_BUTTON_START, true);
                button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
            }
            break;
        case 7:
            if (kind == GESTURE_LEAD_FIRST) {
                button(1, selector, false);
                button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                button(1, SDL_GAMEPAD_BUTTON_START, false);
            }
            if (kind == GESTURE_SELECTOR_FIRST) {
                button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                button(1, SDL_GAMEPAD_BUTTON_START, false);
                button(1, selector, false);
            }
            break;
        case 8:
            if (kind == GESTURE_LEAD_FIRST || kind == GESTURE_SELECTOR_FIRST) button(1, SDL_GAMEPAD_BUTTON_BACK, false);
            break;
        case 10: if (kind != GESTURE_STOP) button(1, SDL_GAMEPAD_BUTTON_BACK, true); break;  /* a genuine BACK tap afterwards */
        case 11: if (kind != GESTURE_STOP) button(1, SDL_GAMEPAD_BUTTON_BACK, false); break;
        default: break;
        }
        const guest_state state = poll();
        if (kind == GESTURE_LEAD_FIRST && step >= 3u && step <= 7u) {
            /* live: the game sees the lead only, never START, LB or X (the swallowing of T1627/T1632 is unchanged) */
            CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_BACK);
            CHECK_EQ_U32(state.analog[2], 0u);
            CHECK_EQ_U32(state.analog[4], 0u);
        }
        if (kind == GESTURE_STOP && step >= 3u) CHECK_EQ_U32(state.buttons, XINPUT_BUTTON_BACK);
    }
    xinput_record_close();
    *marks = xinput_record_marks_written();
    *closed = present_video_sink_closed(sink);
    const char *why = present_video_sink_close_cause(sink);
    snprintf(cause, cause_size, "%s", why != NULL ? why : "");
    xinput_hotkeys_free(hotkeys);
    finish();
}
static void test_record_trims_the_gesture(void)
{
    const char *clean_path = "/tmp/t1632_e2e_clean.rec", *dirty_path = "/tmp/t1632_e2e_dirty.rec";
    unsigned marks_clean = 0u, marks_dirty = 0u;
    bool closed_clean = false, closed_dirty = false;
    char cause[120];
    run_record_scenario(GESTURE_NONE, clean_path, &marks_clean, &closed_clean, cause, sizeof cause);
    char *clean = read_whole(clean_path);
    CHECK(clean != NULL && marks_clean == 1u && !closed_clean);
    CHECK(clean != NULL && strstr(clean, "# mark: at=4\n") != NULL && strstr(clean, "# polls: 16") != NULL);
    CHECK(clean != NULL && strstr(clean, "BACK") != NULL && strstr(clean, "A=255") != NULL); /* the genuine BACK tap and A press */
    const gesture_kind kinds[2] = {GESTURE_LEAD_FIRST, GESTURE_SELECTOR_FIRST};
    for (unsigned i = 0u; i < 2u; i++) {
        run_record_scenario(kinds[i], dirty_path, &marks_dirty, &closed_dirty, cause, sizeof cause);
        char *dirty = read_whole(dirty_path);
        CHECK(dirty != NULL && clean != NULL && strcmp(dirty, clean) == 0); /* the gesture is not in the route */
        CHECK(marks_dirty == 1u && !closed_dirty);
        free(dirty);
        /* and the dirty route replays like the clean one, poll by poll */
        initialise();
        char error[300];
        uint64_t total = 0u;
        CHECK(xinput_replay_load(dirty_path, "aa11", "bb22", "--x 1", error, sizeof error, &total));
        CHECK(total == 16u);
        const xinput_script *script = xinput_replay_script();
        CHECK(script != NULL);
        if (script != NULL) {
            for (uint64_t poll_index = 3u; poll_index <= 9u; poll_index++) {
                const xinput_pad_state at = xinput_script_state_at(script, poll_index);
                CHECK(at.digital_buttons == 0u && at.analog[2] == 0u && at.analog[4] == 0u);
            }
            CHECK(xinput_script_state_at(script, 10u).digital_buttons == XINPUT_BUTTON_BACK);
            CHECK(xinput_script_state_at(script, 1u).analog[0] == 255u);
        }
        xinput_source_reset();
        xinput_devices_enable_synthetic_pad(false);
        environment_end();
    }
    /* the stop chord: the host asks for the clean shutdown like a window close, the lead is still held, still no BACK */
    run_record_scenario(GESTURE_STOP, dirty_path, &marks_dirty, &closed_dirty, cause, sizeof cause);
    char *stopped = read_whole(dirty_path);
    CHECK(closed_dirty && strstr(cause, "stop hotkey") != NULL);
    CHECK(stopped != NULL && strstr(stopped, "BACK") == NULL && strstr(stopped, "# polls: 16") != NULL);
    CHECK(stopped != NULL && strstr(stopped, "A=255") != NULL);
    free(stopped);
    free(clean);
    remove(clean_path);
    remove(dirty_path);
}
static int run_all(void)
{
    if (!present_window_available()) {
        printf("SKIPPED pad_sdl_feed: no window sink\n");
        return 77;
    }
    test_no_window_refused();
    test_sink_raw_queue();
    test_adapter_event_fields();
    test_keyboard_map_end_to_end();
    test_keyboard_edges();
    test_queue_overflow_is_counted();
    test_gamepad_map_end_to_end();
    test_gamepad_axes();
    test_virtual_gamepad_hotplug();
    test_hotkeys_end_to_end();
    test_record_trims_the_gesture();
    printf("test_pad_sdl_feed: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
#endif

int main(void)
{
#ifndef TSFP_HAVE_SDL3
    printf("SKIPPED pad_sdl_feed: this build has no SDL3 (headless default build). Install libsdl3-dev and "
           "reconfigure to run it.\n");
    return 77;
#else
    return run_all();
#endif
}
