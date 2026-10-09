/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T751: host keyboard and gamepad sources for the FABRICATED synthetic pad, driven by a fake event feed and read
 * back through the pad adapter's XInputGetState so the bytes the title would read are checked. */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_host_source.h"
#include "xinput_source.h"
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define HANDLE 0x58504430u
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumbs[4];
} guest_state;
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
static void finish(xinput_host_source *source)
{
    xinput_source_reset();
    xinput_host_source_free(source);
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static xinput_host_source *open_source(xinput_host_device device, const char *text)
{
    char error[160];
    xinput_event_feed feed;
    CHECK(xinput_fake_feed_parse(text, strlen(text), &feed, error, sizeof(error)));
    xinput_host_source *source = xinput_host_source_create(device, feed);
    CHECK(source != NULL);
    xinput_host_source_install(source);
    return source;
}
static guest_state poll(void)
{
    guest_state state;
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 22u);
    memcpy(&state.packet, at, 4u); memcpy(&state.buttons, at + 4u, 2u);
    memcpy(state.analog, at + 6u, 8u); memcpy(state.thumbs, at + 14u, 8u);
    return state;
}
static void expect_parse_error(const char *text, const char *needle)
{
    char error[160];
    xinput_event_feed feed;
    CHECK(!xinput_fake_feed_parse(text, strlen(text), &feed, error, sizeof(error)));
    CHECK(strstr(error, needle) != NULL);
    if (strstr(error, needle) == NULL) printf("  error was '%s', wanted '%s'\n", error, needle);
}
/* One expectation per row of the documented tables, written out independently of the source's table. */
typedef struct { const char *key; uint16_t digital; int analog; int stick; int16_t stick_value; } key_expect;
static void test_keyboard_table(void)
{
    static const key_expect rows[] = {
        {"UP", XINPUT_BUTTON_DPAD_UP, -1, -1, 0}, {"DOWN", XINPUT_BUTTON_DPAD_DOWN, -1, -1, 0},
        {"LEFT", XINPUT_BUTTON_DPAD_LEFT, -1, -1, 0}, {"RIGHT", XINPUT_BUTTON_DPAD_RIGHT, -1, -1, 0},
        {"ENTER", XINPUT_BUTTON_START, -1, -1, 0}, {"BACKSPACE", XINPUT_BUTTON_BACK, -1, -1, 0},
        {"E", XINPUT_BUTTON_LEFT_THUMB, -1, -1, 0}, {"R", XINPUT_BUTTON_RIGHT_THUMB, -1, -1, 0},
        {"Z", 0, 0, -1, 0}, {"X", 0, 1, -1, 0}, {"A", 0, 2, -1, 0}, {"S", 0, 3, -1, 0}, {"Q", 0, 4, -1, 0},
        {"W", 0, 5, -1, 0}, {"1", 0, 6, -1, 0}, {"2", 0, 7, -1, 0},
        {"I", 0, -1, 1, 32767}, {"K", 0, -1, 1, -32768}, {"J", 0, -1, 0, -32768}, {"L", 0, -1, 0, 32767},
        {"T", 0, -1, 3, 32767}, {"G", 0, -1, 3, -32768}, {"F", 0, -1, 2, -32768}, {"H", 0, -1, 2, 32767},
    };
    CHECK_EQ_U32((uint32_t)(sizeof(rows) / sizeof(rows[0])) + 4u, (uint32_t)xinput_host_code_count(XINPUT_HOST_KEYBOARD));
    for (size_t i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char text[96];
        snprintf(text, sizeof(text), "0 keyboard down %s\n1 keyboard up %s\n", rows[i].key, rows[i].key);
        initialise();
        xinput_host_source *source = open_source(XINPUT_HOST_KEYBOARD, text);
        const guest_state down = poll();
        CHECK_EQ_U32(down.buttons, rows[i].digital);
        for (int a = 0; a < 8; a++) CHECK_EQ_U32(down.analog[a], a == rows[i].analog ? 255u : 0u);
        for (int s = 0; s < 4; s++)
            CHECK_EQ_U32((uint32_t)(int32_t)down.thumbs[s], s == rows[i].stick ? (uint32_t)(int32_t)rows[i].stick_value : 0u);
        CHECK_EQ_U32(down.packet, 2u);
        const guest_state up = poll();
        CHECK_EQ_U32(up.buttons, 0u);
        for (int a = 0; a < 8; a++) CHECK_EQ_U32(up.analog[a], 0u);
        for (int s = 0; s < 4; s++) CHECK_EQ_U32((uint32_t)(int32_t)up.thumbs[s], 0u);
        CHECK_EQ_U32(up.packet, 3u);
        CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), 2u);
        finish(source);
    }
}
static void test_keyboard_combinations_and_cancel(void)
{
    initialise();
    xinput_host_source *source = open_source(XINPUT_HOST_KEYBOARD,
        "0 keyboard down UP\n0 keyboard down Z\n0 keyboard down I\n0 keyboard down K\n"
        "1 keyboard down l\n1 keyboard down j\n2 keyboard up K\n3 keyboard up UP\n");
    guest_state s = poll();
    CHECK_EQ_U32(s.buttons, XINPUT_BUTTON_DPAD_UP); CHECK_EQ_U32(s.analog[0], 255u);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[1], 0u);  /* I and K together cancel */
    CHECK_EQ_U32(s.packet, 5u);                        /* four raw reports at poll 0 (T731) */
    s = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[0], 0u);  /* J and L cancel, lower case names accepted */
    CHECK_EQ_U32(s.packet, 7u);                        /* J then L is two raw reports, net state unchanged */
    s = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[1], 32767u);
    CHECK_EQ_U32(s.packet, 8u);
    s = poll();
    CHECK_EQ_U32(s.buttons, 0u); CHECK_EQ_U32(s.packet, 9u);
    s = poll();
    CHECK_EQ_U32(s.packet, 9u);  /* held across polls, nothing new */
    finish(source);
}
static void test_gamepad_table_and_scaling(void)
{
    static const struct { const char *button; uint16_t digital; int analog; } rows[] = {
        {"A", 0, 0}, {"B", 0, 1}, {"X", 0, 2}, {"Y", 0, 3}, {"LB", 0, 4}, {"RB", 0, 5},
        {"START", XINPUT_BUTTON_START, -1}, {"BACK", XINPUT_BUTTON_BACK, -1},
        {"LSTICK", XINPUT_BUTTON_LEFT_THUMB, -1}, {"RSTICK", XINPUT_BUTTON_RIGHT_THUMB, -1},
        {"DPAD_UP", XINPUT_BUTTON_DPAD_UP, -1}, {"DPAD_DOWN", XINPUT_BUTTON_DPAD_DOWN, -1},
        {"DPAD_LEFT", XINPUT_BUTTON_DPAD_LEFT, -1}, {"DPAD_RIGHT", XINPUT_BUTTON_DPAD_RIGHT, -1},
    };
    CHECK_EQ_U32((uint32_t)(sizeof(rows) / sizeof(rows[0])) + 1u, (uint32_t)xinput_host_code_count(XINPUT_HOST_GAMEPAD));
    for (size_t i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char text[96];
        snprintf(text, sizeof(text), "0 gamepad down %s\n1 gamepad up %s\n", rows[i].button, rows[i].button);
        initialise();
        xinput_host_source *source = open_source(XINPUT_HOST_GAMEPAD, text);
        const guest_state down = poll();
        CHECK_EQ_U32(down.buttons, rows[i].digital);
        for (int a = 0; a < 8; a++) CHECK_EQ_U32(down.analog[a], a == rows[i].analog ? 255u : 0u);
        CHECK_EQ_U32(poll().buttons, 0u);
        finish(source);
    }
    /* Axes: sticks pass through (clamped), triggers scale value >> 7, no dead zone, below 0x20 reads 0. */
    initialise();
    xinput_host_source *source = open_source(XINPUT_HOST_GAMEPAD,
        "0 gamepad axis LX -32768\n0 gamepad axis LY 32767\n0 gamepad axis RX 100000\n0 gamepad axis RY -100000\n"
        "0 gamepad axis LT 32767\n0 gamepad axis RT 4095\n"
        "1 gamepad axis LT 128\n1 gamepad axis RT 4096\n1 gamepad axis LX 1\n"
        "2 gamepad axis LT 127\n2 gamepad axis RT -5\n");
    guest_state s = poll();
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[0], (uint32_t)-32768); CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[1], 32767u);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[2], 32767u); CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[3], (uint32_t)-32768);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_LEFT_TRIGGER], 255u);
    CHECK_EQ_U32(xinput_hle_pad_state(0u).analog[XINPUT_ANALOG_RIGHT_TRIGGER], 31u);  /* 4095 >> 7 */
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_RIGHT_TRIGGER], 0u);  /* measured threshold: 31 < 0x20 reads 0 */
    CHECK_EQ_U32(s.packet, 1u + 6u);
    s = poll();
    CHECK_EQ_U32(xinput_hle_pad_state(0u).analog[XINPUT_ANALOG_LEFT_TRIGGER], 1u);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_LEFT_TRIGGER], 0u);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_RIGHT_TRIGGER], 32u);  /* 4096 >> 7 = 32, exactly the threshold passes */
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[0], 1u);
    s = poll();
    CHECK_EQ_U32(xinput_hle_pad_state(0u).analog[XINPUT_ANALOG_LEFT_TRIGGER], 0u);  /* 127 >> 7 */
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_RIGHT_TRIGGER], 0u);  /* negative reads 0 */
    finish(source);
}
static void test_timing_and_packet_numbers(void)
{
    initialise();
    xinput_host_source *source = open_source(XINPUT_HOST_KEYBOARD,
        "2 keyboard down ENTER\n2 keyboard down ENTER\n4 keyboard up ENTER\n4 keyboard down SPACE\n");
    CHECK_EQ_U32(poll().buttons, 0u);
    guest_state s = poll();
    CHECK_EQ_U32(s.buttons, 0u); CHECK_EQ_U32(s.packet, 1u);  /* the event at poll 2 is not early */
    s = poll();
    CHECK_EQ_U32(s.buttons, XINPUT_BUTTON_START); CHECK_EQ_U32(s.packet, 2u);  /* repeated key-down: no report */
    s = poll();
    CHECK_EQ_U32(s.packet, 2u);
    s = poll();
    CHECK_EQ_U32(s.buttons, 0u); CHECK_EQ_U32(s.packet, 3u);  /* unmapped SPACE adds none */
    CHECK_EQ_U32((uint32_t)xinput_host_source_unmapped_ignored(source), 1u);
    finish(source);
}
static void test_ignored_events_are_explicit(void)
{
    initialise();
    xinput_host_source *source = open_source(XINPUT_HOST_KEYBOARD,
        "0 keyboard down CODE:9999\n0 gamepad down A\n0 gamepad axis LX 500\n0 keyboard down ESCAPE\n"
        "0 keyboard down UP\n");
    const guest_state s = poll();
    CHECK_EQ_U32(s.buttons, XINPUT_BUTTON_DPAD_UP); CHECK_EQ_U32(s.analog[0], 0u); CHECK_EQ_U32(s.packet, 2u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_other_device_ignored(source), 2u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unmapped_ignored(source), 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), 1u);
    finish(source);
    initialise();
    source = open_source(XINPUT_HOST_GAMEPAD, "0 gamepad axis CODE:6 5\n0 gamepad down CODE:15\n0 gamepad down GUIDE\n");
    CHECK_EQ_U32(poll().packet, 1u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 2u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unmapped_ignored(source), 1u);
    finish(source);
}
typedef struct { xinput_host_event events[2]; size_t next; } memory_feed;
static bool memory_next_due(void *user, uint64_t poll_index, xinput_host_event *out)
{
    memory_feed *feed = user;
    if (feed->next >= 2u || feed->events[feed->next].poll > poll_index) return false;
    *out = feed->events[feed->next++];
    return true;
}
/* A provider other than the file one (what a window layer would be): an axis event aimed at the keyboard and an
 * action no table knows are ignored and counted, never applied. */
static void test_custom_provider_bad_events_are_ignored(void)
{
    memory_feed feed = {{{0u, XINPUT_HOST_KEYBOARD, XINPUT_HOST_AXIS, 0u, 500}, {0u, XINPUT_HOST_KEYBOARD, (xinput_host_action)9, 0u, 0}}, 0u};
    initialise();
    xinput_event_feed provider = {memory_next_due, NULL, &feed};
    xinput_host_source *source = xinput_host_source_create(XINPUT_HOST_KEYBOARD, provider);
    CHECK(source != NULL);
    xinput_host_source_install(source);
    const guest_state s = poll();
    CHECK_EQ_U32(s.packet, 1u);
    for (int i = 0; i < 4; i++) CHECK_EQ_U32((uint32_t)(int32_t)s.thumbs[i], 0u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_unknown_ignored(source), 2u);
    CHECK_EQ_U32((uint32_t)xinput_host_source_applied(source), 0u);
    xinput_source_reset();
    xinput_host_source_free(source);
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static void test_feed_refusals(void)
{
    expect_parse_error("0 mouse down UP\n", "line 1: unknown device");
    expect_parse_error("0 keyboard down\n", "missing name");
    expect_parse_error("0 keyboard down NOSUCHKEY\n", "unknown key or button");
    expect_parse_error("0 keyboard down UP\n0 keyboard press UP\n", "line 2: unknown action");
    expect_parse_error("0 keyboard axis LX 5\n", "keyboard has no axis");
    expect_parse_error("0 gamepad axis LX\n", "missing axis value");
    expect_parse_error("0 gamepad axis LX five\n", "axis value");
    expect_parse_error("0 gamepad axis NOPE 5\n", "unknown axis");
    expect_parse_error("0 gamepad down A extra\n", "unexpected extra token");
    expect_parse_error("-1 keyboard down UP\n", "poll index");
    expect_parse_error("x keyboard down UP\n", "poll index");
    expect_parse_error("5 keyboard down UP\n4 keyboard up UP\n", "line 2: poll index goes backwards");
    expect_parse_error("0 keyboard down CODE:x\n", "unknown key or button");
    expect_parse_error("0\n", "missing device");
    expect_parse_error("0 keyboard\n", "missing action");
    char error[160];
    xinput_event_feed feed;
    CHECK(xinput_fake_feed_parse("# only a comment\n\n", 18u, &feed, error, sizeof(error)));
    feed.close(feed.user);
    CHECK(!xinput_fake_feed_load("tmp/definitely-missing-pad-feed.txt", &feed, error, sizeof(error)));
    CHECK(strstr(error, "cannot open") != NULL);
}
static int open_feed_closed;
static void count_close(void *user) { (void)user; open_feed_closed++; }
static bool never_due(void *user, uint64_t poll, xinput_host_event *out) { (void)user; (void)poll; (void)out; return false; }
/* T751: a caller built feed (the SDL window provider) is owned by the source, also on refusal. */
static void test_open_feed_takes_ownership(void)
{
    char error[120];
    open_feed_closed = 0;
    xinput_event_feed feed = {never_due, count_close, NULL};
    xinput_host_source *source = xinput_pad_source_open_feed(XINPUT_PAD_SOURCE_KEYBOARD, feed, error, sizeof(error));
    CHECK(source != NULL);
    CHECK_EQ_U32(open_feed_closed, 0u);
    xinput_host_source_free(source);
    CHECK_EQ_U32(open_feed_closed, 1u);
    CHECK(xinput_pad_source_open_feed(XINPUT_PAD_SOURCE_SCRIPT, feed, error, sizeof(error)) == NULL);
    CHECK_EQ_U32(open_feed_closed, 2u);
    CHECK(strstr(error, "not a host device source") != NULL);
}
static void test_no_provider_is_refused_naming_it(void)
{
    char error[240];
    CHECK(xinput_pad_source_open(XINPUT_PAD_SOURCE_KEYBOARD, NULL, error, sizeof(error)) == NULL);
    CHECK(strstr(error, "no event provider") != NULL && strstr(error, "keyboard") != NULL);
    CHECK(strstr(error, "SDL3 window keyboard provider") != NULL && strstr(error, "--present window") != NULL);
    CHECK(strstr(error, "--pad-feed") != NULL);
    CHECK(xinput_pad_source_open(XINPUT_PAD_SOURCE_GAMEPAD, NULL, error, sizeof(error)) == NULL);
    CHECK(strstr(error, "gamepad") != NULL && strstr(error, "SDL3 gamepad provider") != NULL &&
          strstr(error, "--present window") != NULL);
    FILE *file = fopen("t751-feed-script.txt", "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        fputs("0 keyboard down UP\n", file);
        fclose(file);
        /* The script source is not a host device source, even with a readable feed. */
        CHECK(xinput_pad_source_open(XINPUT_PAD_SOURCE_SCRIPT, "t751-feed-script.txt", error, sizeof(error)) == NULL);
        CHECK(strstr(error, "not a host device source") != NULL);
        remove("t751-feed-script.txt");
    }
    CHECK(xinput_pad_source_open(XINPUT_PAD_SOURCE_KEYBOARD, "tmp/definitely-missing-pad-feed.txt", error, sizeof(error)) == NULL);
    CHECK(strstr(error, "cannot open") != NULL);
    xinput_pad_source_kind kind;
    CHECK(xinput_pad_source_kind_parse("script", &kind) && kind == XINPUT_PAD_SOURCE_SCRIPT);
    CHECK(xinput_pad_source_kind_parse("keyboard", &kind) && kind == XINPUT_PAD_SOURCE_KEYBOARD);
    CHECK(xinput_pad_source_kind_parse("gamepad", &kind) && kind == XINPUT_PAD_SOURCE_GAMEPAD);
    CHECK(!xinput_pad_source_kind_parse("mouse", &kind));
    CHECK(!xinput_pad_source_kind_parse(NULL, &kind));
    /* A source cannot be created for an unknown device. */
    xinput_event_feed none = {0};
    CHECK(xinput_host_source_create((xinput_host_device)0, none) == NULL);
}
static void test_file_provider_end_to_end(void)
{
    const char *path = "t751-feed-test.txt";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file == NULL) return;
    fputs("0 keyboard down ENTER\n1 keyboard up ENTER\n", file);
    fclose(file);
    char error[160];
    xinput_host_source *source = xinput_pad_source_open(XINPUT_PAD_SOURCE_KEYBOARD, path, error, sizeof(error));
    CHECK(source != NULL);
    remove(path);
    if (source == NULL) return;
    initialise();
    xinput_host_source_install(source);
    CHECK_EQ_U32(poll().buttons, XINPUT_BUTTON_START);
    CHECK_EQ_U32(poll().buttons, 0u);
    finish(source);
}
static void test_name_tables(void)
{
    CHECK_EQ_U32(xinput_host_code_of(XINPUT_HOST_KEYBOARD, "enter"), 4u);
    CHECK_EQ_U32(xinput_host_code_of(XINPUT_HOST_KEYBOARD, "ENTERS"), UINT32_MAX);
    CHECK_EQ_U32(xinput_host_code_of(XINPUT_HOST_KEYBOARD, "ENTE"), UINT32_MAX);
    CHECK(xinput_host_code_name(XINPUT_HOST_KEYBOARD, 0u) != NULL);
    CHECK(xinput_host_code_name(XINPUT_HOST_KEYBOARD, 999u) == NULL);
    CHECK_EQ_U32(xinput_host_code_count((xinput_host_device)0), 0u);
    CHECK_EQ_U32(xinput_host_axis_of("rt"), 5u);
    CHECK_EQ_U32(xinput_host_axis_of("R"), UINT32_MAX);
}
int main(void)
{
    test_keyboard_table();
    test_keyboard_combinations_and_cancel();
    test_gamepad_table_and_scaling();
    test_timing_and_packet_numbers();
    test_ignored_events_are_explicit();
    test_custom_provider_bad_events_are_ignored();
    test_feed_refusals();
    test_no_provider_is_refused_naming_it();
    test_open_feed_takes_ownership();
    test_file_provider_end_to_end();
    test_name_tables();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
