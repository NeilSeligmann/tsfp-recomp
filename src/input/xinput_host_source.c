/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_host_source.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { TARGET_NONE, TARGET_DIGITAL, TARGET_ANALOG, TARGET_STICK } target_kind;
typedef struct {
    const char *name;
    target_kind kind;
    uint16_t digital;  /* TARGET_DIGITAL: XINPUT_BUTTON_* bit */
    uint8_t analog;    /* TARGET_ANALOG: index into the analog run (6 and 7 are the triggers) */
    uint8_t axis;      /* TARGET_STICK: 0 LX 1 LY 2 RX 3 RY */
    int8_t direction;  /* TARGET_STICK: +1 or -1 */
} mapping_row;

#define DIG(n, bit) {n, TARGET_DIGITAL, bit, 0u, 0u, 0}
#define ANA(n, idx) {n, TARGET_ANALOG, 0u, idx, 0u, 0}
#define STK(n, ax, dir) {n, TARGET_STICK, 0u, 0u, ax, dir}
#define NONE(n) {n, TARGET_NONE, 0u, 0u, 0u, 0}
#define FACE_A 0u
#define FACE_B 1u
#define FACE_X 2u
#define FACE_Y 3u
#define FACE_BLACK 4u
#define FACE_WHITE 5u

/* THE keyboard table (FABRICATED, see the header). Row order is the code. */
static const mapping_row keyboard_keys[] = {
    DIG("UP", XINPUT_BUTTON_DPAD_UP), DIG("DOWN", XINPUT_BUTTON_DPAD_DOWN),
    DIG("LEFT", XINPUT_BUTTON_DPAD_LEFT), DIG("RIGHT", XINPUT_BUTTON_DPAD_RIGHT),
    DIG("ENTER", XINPUT_BUTTON_START), DIG("BACKSPACE", XINPUT_BUTTON_BACK),
    DIG("E", XINPUT_BUTTON_LEFT_THUMB), DIG("R", XINPUT_BUTTON_RIGHT_THUMB),
    ANA("Z", FACE_A), ANA("X", FACE_B), ANA("A", FACE_X), ANA("S", FACE_Y), ANA("Q", FACE_BLACK),
    ANA("W", FACE_WHITE), ANA("1", XINPUT_ANALOG_LEFT_TRIGGER), ANA("2", XINPUT_ANALOG_RIGHT_TRIGGER),
    STK("I", 1u, 1), STK("K", 1u, -1), STK("J", 0u, -1), STK("L", 0u, 1),
    STK("T", 3u, 1), STK("G", 3u, -1), STK("F", 2u, -1), STK("H", 2u, 1),
    NONE("ESCAPE"), NONE("SPACE"), NONE("TAB"), NONE("F1"),
};
/* THE gamepad button table (FABRICATED). Face buttons report as analog 255 like the retail pad's pressure
 * buttons. */
static const mapping_row gamepad_buttons[] = {
    ANA("A", FACE_A), ANA("B", FACE_B), ANA("X", FACE_X), ANA("Y", FACE_Y), ANA("LB", FACE_BLACK),
    ANA("RB", FACE_WHITE), DIG("START", XINPUT_BUTTON_START), DIG("BACK", XINPUT_BUTTON_BACK),
    DIG("LSTICK", XINPUT_BUTTON_LEFT_THUMB), DIG("RSTICK", XINPUT_BUTTON_RIGHT_THUMB),
    DIG("DPAD_UP", XINPUT_BUTTON_DPAD_UP), DIG("DPAD_DOWN", XINPUT_BUTTON_DPAD_DOWN),
    DIG("DPAD_LEFT", XINPUT_BUTTON_DPAD_LEFT), DIG("DPAD_RIGHT", XINPUT_BUTTON_DPAD_RIGHT),
    NONE("GUIDE"),
};
/* THE gamepad axis table: LX LY RX RY pass through, LT RT are scaled (FABRICATED). */
static const char *const gamepad_axes[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
#define AXIS_COUNT 6u
#define AXIS_LT 4u

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define KEYBOARD_ROWS COUNT_OF(keyboard_keys)
#define GAMEPAD_ROWS COUNT_OF(gamepad_buttons)

struct xinput_host_source {
    xinput_host_device device;
    xinput_event_feed feed;
    uint64_t held;         /* bit per table row, keyboard keys or gamepad buttons */
    int16_t axes[AXIS_COUNT];
    uint64_t applied, unknown, other_device, unmapped;
};

static const mapping_row *table_of(xinput_host_device device, size_t *count)
{
    if (device == XINPUT_HOST_KEYBOARD) { *count = KEYBOARD_ROWS; return keyboard_keys; }
    if (device == XINPUT_HOST_GAMEPAD) { *count = GAMEPAD_ROWS; return gamepad_buttons; }
    *count = 0u;
    return NULL;
}

static bool names_equal(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}
size_t xinput_host_code_count(xinput_host_device device)
{
    size_t count;
    (void)table_of(device, &count);
    return count;
}
const char *xinput_host_code_name(xinput_host_device device, uint32_t code)
{
    size_t count;
    const mapping_row *table = table_of(device, &count);
    return code < count ? table[code].name : NULL;
}
uint32_t xinput_host_code_of(xinput_host_device device, const char *name)
{
    size_t count;
    const mapping_row *table = table_of(device, &count);
    for (size_t i = 0u; i < count; i++)
        if (names_equal(table[i].name, name)) return (uint32_t)i;
    return UINT32_MAX;
}
uint32_t xinput_host_axis_of(const char *name)
{
    for (uint32_t i = 0u; i < AXIS_COUNT; i++)
        if (names_equal(gamepad_axes[i], name)) return i;
    return UINT32_MAX;
}

xinput_host_source *xinput_host_source_create(xinput_host_device device, xinput_event_feed feed)
{
    if (device != XINPUT_HOST_KEYBOARD && device != XINPUT_HOST_GAMEPAD) return NULL;
    xinput_host_source *source = calloc(1u, sizeof(*source));
    if (source == NULL) return NULL;
    source->device = device;
    source->feed = feed;
    return source;
}
void xinput_host_source_free(xinput_host_source *source)
{
    if (source == NULL) return;
    if (source->feed.close != NULL) source->feed.close(source->feed.user);
    free(source);
}
void xinput_host_source_install(xinput_host_source *source) { xinput_source_install(xinput_host_source_fn, source); }
uint64_t xinput_host_source_applied(const xinput_host_source *source) { return source->applied; }
uint64_t xinput_host_source_unknown_ignored(const xinput_host_source *source) { return source->unknown; }
uint64_t xinput_host_source_other_device_ignored(const xinput_host_source *source) { return source->other_device; }
uint64_t xinput_host_source_unmapped_ignored(const xinput_host_source *source) { return source->unmapped; }

static int16_t clamp16(int32_t value)
{
    return (int16_t)(value < -32768 ? -32768 : (value > 32767 ? 32767 : value));
}
/* Opposite keys held together cancel (FABRICATED). */
static int16_t stick_from_keys(const xinput_host_source *source, uint8_t axis)
{
    bool positive = false, negative = false;
    for (size_t i = 0u; i < KEYBOARD_ROWS; i++) {
        if ((source->held & ((uint64_t)1u << i)) == 0u || keyboard_keys[i].kind != TARGET_STICK ||
            keyboard_keys[i].axis != axis)
            continue;
        if (keyboard_keys[i].direction > 0) positive = true; else negative = true;
    }
    if (positive == negative) return 0;
    return positive ? 32767 : -32768;
}
/* The pad state the held buttons and axes describe. */
static xinput_pad_state compose(const xinput_host_source *source)
{
    xinput_pad_state state;
    memset(&state, 0, sizeof(state));
    size_t count;
    const mapping_row *table = table_of(source->device, &count);
    for (size_t i = 0u; i < count; i++) {
        if ((source->held & ((uint64_t)1u << i)) == 0u) continue;
        if (table[i].kind == TARGET_DIGITAL) state.digital_buttons |= table[i].digital;
        else if (table[i].kind == TARGET_ANALOG) state.analog[table[i].analog] = 255u;
    }
    if (source->device == XINPUT_HOST_KEYBOARD) {
        state.thumb_left_x = stick_from_keys(source, 0u);
        state.thumb_left_y = stick_from_keys(source, 1u);
        state.thumb_right_x = stick_from_keys(source, 2u);
        state.thumb_right_y = stick_from_keys(source, 3u);
    } else {
        state.thumb_left_x = source->axes[0];
        state.thumb_left_y = source->axes[1];
        state.thumb_right_x = source->axes[2];
        state.thumb_right_y = source->axes[3];
        /* Host triggers 0..32767 scale to 0..255 (FABRICATED); no dead zone. */
        for (unsigned t = 0u; t < 2u; t++) {
            const int16_t host = source->axes[AXIS_LT + t];
            state.analog[XINPUT_ANALOG_LEFT_TRIGGER + t] = host <= 0 ? 0u : (uint8_t)(host >> 7);
        }
    }
    return state;
}

/* Apply one event to the held set. Returns true when it was an applicable known event. */
static bool apply_event(xinput_host_source *source, const xinput_host_event *event)
{
    if (event->device != source->device) {
        source->other_device++;
        return false;
    }
    size_t count;
    const mapping_row *table = table_of(source->device, &count);
    if (event->action == XINPUT_HOST_AXIS) {
        if (source->device != XINPUT_HOST_GAMEPAD || event->code >= AXIS_COUNT) {
            source->unknown++;
            fprintf(stderr, "xinput host source: axis event code %u is not a %s axis, IGNORED\n", event->code,
                    source->device == XINPUT_HOST_GAMEPAD ? "gamepad" : "keyboard (it has none)");
            return false;
        }
        source->axes[event->code] = clamp16(event->value);
        return true;
    }
    if (event->action != XINPUT_HOST_DOWN && event->action != XINPUT_HOST_UP) {
        source->unknown++;
        fprintf(stderr, "xinput host source: unknown action %d, IGNORED\n", (int)event->action);
        return false;
    }
    if (event->code >= count) {
        source->unknown++;
        fprintf(stderr, "xinput host source: unknown %s code %u, IGNORED\n",
                source->device == XINPUT_HOST_KEYBOARD ? "key" : "button", event->code);
        return false;
    }
    if (table[event->code].kind == TARGET_NONE) {
        source->unmapped++;
        return false;
    }
    const uint64_t bit = (uint64_t)1u << event->code;
    if (event->action == XINPUT_HOST_DOWN) source->held |= bit; else source->held &= ~bit;
    return true;
}

bool xinput_host_source_fn(uint64_t poll_index, xinput_pad_state *out, void *user)
{
    xinput_host_source *source = user;
    xinput_host_event event;
    while (source->feed.next_due(source->feed.user, poll_index, &event)) {
        if (!apply_event(source, &event)) continue;
        source->applied++;
        /* T731: one raw report per applied event, an unchanged state counts none (the adapter compares). */
        (void)xinput_hle_set_synthetic_pad_state(0u, compose(source));
    }
    *out = compose(source);
    return true;
}

/* ---- fake-feed provider ---- */
typedef struct {
    xinput_host_event *events;
    size_t count, next;
} fake_feed;

static bool fake_next_due(void *user, uint64_t poll_index, xinput_host_event *out)
{
    fake_feed *feed = user;
    if (feed->next >= feed->count || feed->events[feed->next].poll > poll_index) return false;
    *out = feed->events[feed->next++];
    return true;
}
static void fake_close(void *user)
{
    fake_feed *feed = user;
    free(feed->events);
    free(feed);
}
static void fail(char *error, size_t size, unsigned line, const char *what, const char *token)
{
    if (error != NULL && size != 0u) snprintf(error, size, "line %u: %s '%s'", line, what, token);
}
static bool parse_long(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) return false;
    *out = value;
    return true;
}
static bool parse_code(xinput_host_device device, const char *name, uint32_t *code)
{
    if (strncmp(name, "CODE:", 5u) == 0 || strncmp(name, "code:", 5u) == 0) {
        long raw;
        if (!parse_long(name + 5, 0, 1000000, &raw)) return false;
        *code = (uint32_t)raw;
        return true;
    }
    *code = xinput_host_code_of(device, name);
    return *code != UINT32_MAX;
}
static bool parse_line(char *line, unsigned number, xinput_host_event *event, char *error, size_t size)
{
    char *save = NULL;
    char *poll = strtok_r(line, " \t\r", &save);
    char *device = strtok_r(NULL, " \t\r", &save);
    char *action = strtok_r(NULL, " \t\r", &save);
    char *name = strtok_r(NULL, " \t\r", &save);
    char *value = strtok_r(NULL, " \t\r", &save);
    long number_value = 0;
    if (!parse_long(poll, 0, 1000000000L, &number_value)) { fail(error, size, number, "poll index not 0..1000000000", poll); return false; }
    event->poll = (uint64_t)number_value;
    if (device == NULL) { fail(error, size, number, "missing device", ""); return false; }
    if (names_equal(device, "keyboard")) event->device = XINPUT_HOST_KEYBOARD;
    else if (names_equal(device, "gamepad")) event->device = XINPUT_HOST_GAMEPAD;
    else { fail(error, size, number, "unknown device", device); return false; }
    if (action == NULL) { fail(error, size, number, "missing action", ""); return false; }
    if (names_equal(action, "down")) event->action = XINPUT_HOST_DOWN;
    else if (names_equal(action, "up")) event->action = XINPUT_HOST_UP;
    else if (names_equal(action, "axis")) event->action = XINPUT_HOST_AXIS;
    else { fail(error, size, number, "unknown action", action); return false; }
    if (name == NULL) { fail(error, size, number, "missing name", ""); return false; }
    event->value = 0;
    if (event->action == XINPUT_HOST_AXIS) {
        if (event->device != XINPUT_HOST_GAMEPAD) { fail(error, size, number, "the keyboard has no axis", name); return false; }
        if (value == NULL) { fail(error, size, number, "missing axis value", name); return false; }
        if (!parse_long(value, -2147483647L, 2147483647L, &number_value)) { fail(error, size, number, "axis value not an integer", value); return false; }
        event->value = (int32_t)number_value;
        event->code = xinput_host_axis_of(name);
        if (event->code == UINT32_MAX) {
            if (!parse_code(event->device, name, &event->code)) { fail(error, size, number, "unknown axis", name); return false; }
        }
        return true;
    }
    if (value != NULL) { fail(error, size, number, "unexpected extra token", value); return false; }
    if (!parse_code(event->device, name, &event->code)) { fail(error, size, number, "unknown key or button", name); return false; }
    return true;
}

bool xinput_fake_feed_parse(const char *text, size_t len, xinput_event_feed *feed, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0u) error[0] = '\0';
    fake_feed *state = calloc(1u, sizeof(*state));
    if (state == NULL) return false;
    unsigned number = 0u;
    size_t at = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        char line[256];
        const size_t n = end - at;
        number++;
        if (n >= sizeof(line)) { fail(error, error_size, number, "line too long", ""); goto bad; }
        memcpy(line, text + at, n);
        line[n] = '\0';
        at = end + 1u;
        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        if (line[strspn(line, " \t\r")] == '\0') continue;
        xinput_host_event event;
        if (!parse_line(line, number, &event, error, error_size)) goto bad;
        if (state->count != 0u && event.poll < state->events[state->count - 1u].poll) {
            fail(error, error_size, number, "poll index goes backwards", "");
            goto bad;
        }
        xinput_host_event *grown = realloc(state->events, (state->count + 1u) * sizeof(*grown));
        if (grown == NULL) goto bad;
        state->events = grown;
        state->events[state->count++] = event;
    }
    feed->next_due = fake_next_due;
    feed->close = fake_close;
    feed->user = state;
    return true;
bad:
    free(state->events);
    free(state);
    return false;
}
bool xinput_fake_feed_load(const char *path, xinput_event_feed *feed, char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "cannot open '%s'", path);
        return false;
    }
    char *text = NULL;
    size_t len = 0u, cap = 0u;
    for (;;) {
        if (len + 4096u > cap) {
            cap = cap == 0u ? 8192u : cap * 2u;
            char *grown = realloc(text, cap);
            if (grown == NULL) { free(text); fclose(file); return false; }
            text = grown;
        }
        const size_t got = fread(text + len, 1u, 4096u, file);
        len += got;
        if (got == 0u) break;
    }
    fclose(file);
    const bool ok = xinput_fake_feed_parse(text == NULL ? "" : text, len, feed, error, error_size);
    free(text);
    return ok;
}

bool xinput_host_key_effect(xinput_host_device device, const char *name, uint16_t *digital, int *analog, int *stick)
{
    const uint32_t code = xinput_host_code_of(device, name);
    size_t count;
    const mapping_row *table = table_of(device, &count);
    if (code == UINT32_MAX || table == NULL || code >= count) return false;
    *digital = 0u;
    *analog = -1;
    *stick = -1;
    switch (table[code].kind) {
    case TARGET_DIGITAL: *digital = table[code].digital; return true;
    case TARGET_ANALOG: *analog = (int)table[code].analog; return true;
    case TARGET_STICK: *stick = (int)table[code].axis; return true;
    default: return false;
    }
}

bool xinput_pad_source_kind_parse(const char *text, xinput_pad_source_kind *kind)
{
    if (text == NULL) return false;
    if (strcmp(text, "script") == 0) *kind = XINPUT_PAD_SOURCE_SCRIPT;
    else if (strcmp(text, "keyboard") == 0) *kind = XINPUT_PAD_SOURCE_KEYBOARD;
    else if (strcmp(text, "gamepad") == 0) *kind = XINPUT_PAD_SOURCE_GAMEPAD;
    else return false;
    return true;
}
xinput_host_source *xinput_pad_source_open(xinput_pad_source_kind kind, const char *feed_path, char *error,
                                           size_t error_size)
{
    if (kind != XINPUT_PAD_SOURCE_KEYBOARD && kind != XINPUT_PAD_SOURCE_GAMEPAD) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "not a host device source");
        return NULL;
    }
    const bool keyboard = kind == XINPUT_PAD_SOURCE_KEYBOARD;
    if (feed_path == NULL) {
        if (error != NULL && error_size != 0u)
            snprintf(error, error_size,
                     "no event provider for the %s source: pass --present window (the SDL3 %s provider, needs a build "
                     "with SDL3) or --pad-feed FILE (the fake-feed provider)",
                     keyboard ? "keyboard" : "gamepad", keyboard ? "window keyboard" : "gamepad");
        return NULL;
    }
    xinput_event_feed feed;
    if (!xinput_fake_feed_load(feed_path, &feed, error, error_size)) return NULL;
    return xinput_pad_source_open_feed(kind, feed, error, error_size);
}
xinput_host_source *xinput_pad_source_open_feed(xinput_pad_source_kind kind, xinput_event_feed feed, char *error,
                                                size_t error_size)
{
    if (kind != XINPUT_PAD_SOURCE_KEYBOARD && kind != XINPUT_PAD_SOURCE_GAMEPAD) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "not a host device source");
        if (feed.close != NULL) feed.close(feed.user);
        return NULL;
    }
    xinput_host_source *source = xinput_host_source_create(
        kind == XINPUT_PAD_SOURCE_KEYBOARD ? XINPUT_HOST_KEYBOARD : XINPUT_HOST_GAMEPAD, feed);
    if (source == NULL && feed.close != NULL) feed.close(feed.user);
    return source;
}
