/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_hotkey.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_ALIAS 2u

static const char *const pad_names[] = {"A",     "B",       "X",         "Y",         "LB",        "RB",
                                        "START", "BACK",    "GUIDE",     "LSTICK",    "RSTICK",    "DPAD_UP",
                                        "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};
static const char *const kb_names[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
                                       "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                       "UP", "DOWN", "LEFT", "RIGHT", "ENTER", "BACKSPACE", "SPACE", "TAB", "F1",
                                       "LCTRL", "RCTRL", "LSHIFT", "RSHIFT", "LALT", "RALT"};
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    xinput_hotkey_spec spec;
    char accept[XINPUT_HOTKEY_KEYS_MAX][MAX_ALIAS][XINPUT_HOTKEY_NAME_MAX];
    unsigned accept_count[XINPUT_HOTKEY_KEYS_MAX];
    uint8_t held[XINPUT_HOTKEY_KEYS_MAX]; /* bit per alias */
    bool complete, fired;
    uint64_t since_ms;
} hotkey_entry;

typedef struct {
    xinput_hotkey_device device;
    char name[XINPUT_HOTKEY_NAME_MAX];
} swallowed_key;

/* T1632: a chord key that WAS forwarded to the game (it was pressed before its chord's lead was held). If a chord it belongs to
 * later fires, the game saw a key that was only part of the gesture: the observer is told the poll it went down (BEGIN) and the
 * poll it came up (END) so the recorder can take that span out of the route. */
typedef struct {
    xinput_hotkey_device device;
    char name[XINPUT_HOTKEY_NAME_MAX];
    uint64_t down_poll;
    bool tainted;
} leak_episode;
#define LEAK_MAX 8u

struct xinput_hotkeys {
    hotkey_entry entries[XINPUT_HOTKEY_MAX];
    unsigned count;
    char dir[400];
    bool has_dir;
    xinput_hotkey_fire_fn fire;
    void *user;
    unsigned fired, write_failures, swallowed_total, swallow_overflow;
    swallowed_key swallowed[XINPUT_HOTKEY_SWALLOWED_MAX];
    unsigned swallowed_count;
    leak_episode leaks[LEAK_MAX];
    unsigned leak_count, leak_overflow;
    xinput_hotkey_leak_fn leak_fn;
    void *leak_user;
};

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

static bool names_equal(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++)
        if (upper(*a) != upper(*b)) return false;
    return *a == *b;
}

xinput_hotkey_action xinput_hotkey_action_of(const char *label)
{
    if (label == NULL) return XINPUT_HOTKEY_ACTION_NONE;
    if (names_equal(label, "mark")) return XINPUT_HOTKEY_ACTION_MARK;
    if (names_equal(label, "stop")) return XINPUT_HOTKEY_ACTION_STOP;
    if (names_equal(label, "shot")) return XINPUT_HOTKEY_ACTION_SHOT;
    return XINPUT_HOTKEY_ACTION_NONE;
}

static void set_error(char *error, size_t size, const char *format, const char *detail)
{
    if (error != NULL && size != 0u) snprintf(error, size, format, detail);
}

/* The names a key element accepts: CTRL = LCTRL or RCTRL (kb only), else the name itself. 0 = unknown name. */
static unsigned accept_of(xinput_hotkey_device device, const char *name, char out[MAX_ALIAS][XINPUT_HOTKEY_NAME_MAX])
{
    if (device == XINPUT_HOTKEY_DEVICE_KEYBOARD) {
        static const char *const modifiers[] = {"CTRL", "SHIFT", "ALT"};
        for (size_t i = 0u; i < COUNT_OF(modifiers); i++) {
            if (!names_equal(name, modifiers[i])) continue;
            snprintf(out[0], XINPUT_HOTKEY_NAME_MAX, "L%s", modifiers[i]);
            snprintf(out[1], XINPUT_HOTKEY_NAME_MAX, "R%s", modifiers[i]);
            return 2u;
        }
        for (size_t i = 0u; i < COUNT_OF(kb_names); i++) {
            if (!names_equal(name, kb_names[i])) continue;
            snprintf(out[0], XINPUT_HOTKEY_NAME_MAX, "%s", kb_names[i]);
            return 1u;
        }
        return 0u;
    }
    for (size_t i = 0u; i < COUNT_OF(pad_names); i++) {
        if (!names_equal(name, pad_names[i])) continue;
        snprintf(out[0], XINPUT_HOTKEY_NAME_MAX, "%s", pad_names[i]);
        return 1u;
    }
    return 0u;
}

static bool parse_hold(const char *text, size_t length, unsigned *out)
{
    if (length == 0u || length > 5u) return false;
    unsigned value = 0u;
    for (size_t i = 0u; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10u + (unsigned)(text[i] - '0');
    }
    if (value > XINPUT_HOTKEY_HOLD_MAX_MS) return false;
    *out = value;
    return true;
}

bool xinput_hotkey_parse(const char *text, xinput_hotkey_spec *out, char *error, size_t error_size)
{
    if (text == NULL || out == NULL) {
        set_error(error, error_size, "%s", "no hotkey");
        return false;
    }
    xinput_hotkey_spec work;
    memset(&work, 0, sizeof work);
    const char *equals = strchr(text, '=');
    if (equals == NULL) {
        set_error(error, error_size, "not DEVICE=KEY+KEY[@MS]:LABEL: '%s'", text);
        return false;
    }
    const size_t device_length = (size_t)(equals - text);
    if (device_length == 3u && upper(text[0]) == 'P' && upper(text[1]) == 'A' && upper(text[2]) == 'D') {
        work.device = XINPUT_HOTKEY_DEVICE_PAD;
    } else if (device_length == 2u && upper(text[0]) == 'K' && upper(text[1]) == 'B') {
        work.device = XINPUT_HOTKEY_DEVICE_KEYBOARD;
    } else {
        set_error(error, error_size, "device must be pad or kb: '%s'", text);
        return false;
    }
    const char *keys_begin = equals + 1;
    const char *colon = strchr(keys_begin, ':');
    if (colon == NULL || colon == keys_begin) {
        set_error(error, error_size, "missing :LABEL or keys in '%s'", text);
        return false;
    }
    const char *label = colon + 1;
    const size_t label_length = strlen(label);
    if (label_length == 0u || label_length >= XINPUT_HOTKEY_LABEL_MAX) {
        set_error(error, error_size, "label must be 1..31 characters: '%s'", text);
        return false;
    }
    for (size_t i = 0u; i < label_length; i++) {
        const char c = label[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            set_error(error, error_size, "label may only use letters, digits, _ and -: '%s'", text);
            return false;
        }
    }
    memcpy(work.label, label, label_length);
    work.hold_ms = work.device == XINPUT_HOTKEY_DEVICE_PAD ? XINPUT_HOTKEY_PAD_DEFAULT_HOLD_MS : 0u;
    const char *keys_end = colon;
    const char *at = memchr(keys_begin, '@', (size_t)(keys_end - keys_begin));
    if (at != NULL) {
        if (!parse_hold(at + 1, (size_t)(keys_end - at - 1), &work.hold_ms)) {
            set_error(error, error_size, "hold must be 0..10000 ms: '%s'", text);
            return false;
        }
        keys_end = at;
    }
    char accept_seen[XINPUT_HOTKEY_KEYS_MAX][MAX_ALIAS][XINPUT_HOTKEY_NAME_MAX];
    unsigned accept_seen_count[XINPUT_HOTKEY_KEYS_MAX];
    const char *cursor = keys_begin;
    while (cursor <= keys_end) {
        const char *plus = memchr(cursor, '+', (size_t)(keys_end - cursor));
        const char *stop = plus != NULL ? plus : keys_end;
        const size_t length = (size_t)(stop - cursor);
        if (length == 0u || length >= XINPUT_HOTKEY_NAME_MAX) {
            set_error(error, error_size, "empty or too long key name in '%s'", text);
            return false;
        }
        if (work.key_count >= XINPUT_HOTKEY_KEYS_MAX) {
            set_error(error, error_size, "at most 5 keys: '%s'", text);
            return false;
        }
        char name[XINPUT_HOTKEY_NAME_MAX];
        for (size_t i = 0u; i < length; i++) name[i] = upper(cursor[i]);
        name[length] = '\0';
        const unsigned aliases = accept_of(work.device, name, accept_seen[work.key_count]);
        if (aliases == 0u) {
            set_error(error, error_size, "unknown key '%s' for that device", name);
            return false;
        }
        accept_seen_count[work.key_count] = aliases;
        for (unsigned earlier = 0u; earlier < work.key_count; earlier++)
            for (unsigned a = 0u; a < accept_seen_count[earlier]; a++)
                for (unsigned b = 0u; b < aliases; b++)
                    if (names_equal(accept_seen[earlier][a], accept_seen[work.key_count][b])) {
                        set_error(error, error_size, "key '%s' is listed twice (or overlaps another key)", name);
                        return false;
                    }
        memcpy(work.keys[work.key_count], name, length + 1u);
        work.key_count++;
        if (plus == NULL) break;
        cursor = plus + 1;
    }
    if (work.key_count < 2u) {
        set_error(error, error_size, "a hotkey needs at least 2 keys (a single key would be taken from the game): '%s'", text);
        return false;
    }
    *out = work;
    return true;
}

/* mkdir that accepts an existing DIRECTORY only (an existing regular file is not a place for hotkey.<n>) */
static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0) return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool make_directories(const char *path)
{
    char copy[400];
    if (strlen(path) >= sizeof copy) return false;
    strcpy(copy, path);
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (!ensure_directory(copy)) return false;
            *cursor = '/';
        }
    }
    return ensure_directory(copy);
}

xinput_hotkeys *xinput_hotkeys_create(const xinput_hotkey_spec *specs, unsigned count, const char *dir,
                                      xinput_hotkey_fire_fn fire, void *user, char *error, size_t error_size)
{
    if (specs == NULL || count == 0u || count > XINPUT_HOTKEY_MAX) {
        set_error(error, error_size, "%s", "hotkeys: need 1..4 specs");
        return NULL;
    }
    xinput_hotkeys *hotkeys = calloc(1u, sizeof *hotkeys);
    if (hotkeys == NULL) {
        set_error(error, error_size, "%s", "hotkeys: out of memory");
        return NULL;
    }
    for (unsigned i = 0u; i < count; i++) {
        hotkey_entry *entry = &hotkeys->entries[i];
        entry->spec = specs[i];
        if (entry->spec.key_count < 2u || entry->spec.key_count > XINPUT_HOTKEY_KEYS_MAX) {
            set_error(error, error_size, "%s", "hotkeys: a spec needs 2..5 keys");
            free(hotkeys);
            return NULL;
        }
        for (unsigned k = 0u; k < entry->spec.key_count; k++) {
            entry->accept_count[k] = accept_of(entry->spec.device, entry->spec.keys[k], entry->accept[k]);
            if (entry->accept_count[k] == 0u) {
                set_error(error, error_size, "hotkeys: unknown key '%s'", entry->spec.keys[k]);
                free(hotkeys);
                return NULL;
            }
        }
    }
    hotkeys->count = count;
    hotkeys->fire = fire;
    hotkeys->user = user;
    if (dir != NULL) {
        if (strlen(dir) >= sizeof hotkeys->dir || !make_directories(dir)) {
            set_error(error, error_size, "hotkeys: cannot create the directory '%s'", dir);
            free(hotkeys);
            return NULL;
        }
        strcpy(hotkeys->dir, dir);
        hotkeys->has_dir = true;
    }
    return hotkeys;
}

void xinput_hotkeys_free(xinput_hotkeys *hotkeys) { free(hotkeys); }

unsigned xinput_hotkeys_fired(const xinput_hotkeys *hotkeys) { return hotkeys->fired; }
unsigned xinput_hotkeys_write_failures(const xinput_hotkeys *hotkeys) { return hotkeys->write_failures; }
unsigned xinput_hotkeys_swallowed_total(const xinput_hotkeys *hotkeys) { return hotkeys->swallowed_total; }
unsigned xinput_hotkeys_swallow_overflow(const xinput_hotkeys *hotkeys) { return hotkeys->swallow_overflow; }

void xinput_hotkeys_set_leak_observer(xinput_hotkeys *hotkeys, xinput_hotkey_leak_fn fn, void *user)
{
    if (hotkeys == NULL) return;
    hotkeys->leak_fn = fn;
    hotkeys->leak_user = user;
}
unsigned xinput_hotkeys_leak_overflow(const xinput_hotkeys *hotkeys) { return hotkeys->leak_overflow; }

bool xinput_hotkeys_needs_device(const xinput_hotkeys *hotkeys, xinput_hotkey_device device)
{
    for (unsigned i = 0u; i < hotkeys->count; i++)
        if (hotkeys->entries[i].spec.device == device) return true;
    return false;
}

static unsigned held_count(const hotkey_entry *entry)
{
    unsigned held = 0u;
    for (unsigned k = 0u; k < entry->spec.key_count; k++)
        if (entry->held[k] != 0u) held++;
    return held;
}

/* The element/alias of `entry` that `name` is. false when the key is not part of the chord. */
static bool locate(const hotkey_entry *entry, const char *name, unsigned *element, unsigned *alias)
{
    for (unsigned k = 0u; k < entry->spec.key_count; k++)
        for (unsigned a = 0u; a < entry->accept_count[k]; a++)
            if (names_equal(entry->accept[k][a], name)) {
                *element = k;
                *alias = a;
                return true;
            }
    return false;
}

static bool write_file(const xinput_hotkeys *hotkeys, unsigned number, const char *label, uint64_t poll, uint64_t now_ms)
{
    char path[sizeof hotkeys->dir + 32u], temporary[sizeof hotkeys->dir + 40u];
    snprintf(path, sizeof path, "%s/hotkey.%u", hotkeys->dir, number);
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) return false;
    fprintf(file, "%u %s poll=%llu ms=%llu\n", number, label, (unsigned long long)poll, (unsigned long long)now_ms);
    if (fclose(file) != 0 || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    return true;
}

static void fire(xinput_hotkeys *hotkeys, hotkey_entry *entry, uint64_t now_ms, uint64_t poll)
{
    entry->fired = true;
    const unsigned number = ++hotkeys->fired;
    bool written = true;
    if (hotkeys->has_dir && !write_file(hotkeys, number, entry->spec.label, poll, now_ms)) {
        hotkeys->write_failures++;
        written = false;
    }
    fprintf(stderr, "hotkey (T1627): '%s' #%u at host poll %llu%s\n", entry->spec.label, number, (unsigned long long)poll,
            written ? "" : " (WRITING THE FILE FAILED)");
    /* T1632: the chord keys the game DID see (pressed before the lead) are part of this gesture now */
    for (unsigned i = 0u; i < hotkeys->leak_count; i++) {
        leak_episode *episode = &hotkeys->leaks[i];
        unsigned element, alias;
        if (episode->tainted || episode->device != entry->spec.device || !locate(entry, episode->name, &element, &alias)) continue;
        episode->tainted = true;
        if (hotkeys->leak_fn != NULL) hotkeys->leak_fn(episode->device, episode->name, episode->down_poll, true, hotkeys->leak_user);
    }
    if (hotkeys->fire != NULL) hotkeys->fire(number, entry->spec.label, poll, hotkeys->user);
}

static void evaluate(xinput_hotkeys *hotkeys, uint64_t now_ms, uint64_t poll)
{
    for (unsigned i = 0u; i < hotkeys->count; i++) {
        hotkey_entry *entry = &hotkeys->entries[i];
        const bool all = held_count(entry) == entry->spec.key_count;
        if (!all) {
            entry->complete = false; /* re-arms: the next whole chord starts a new press */
            continue;
        }
        if (!entry->complete) {
            entry->complete = true;
            entry->fired = false;
            entry->since_ms = now_ms;
        }
        if (!entry->fired && now_ms - entry->since_ms >= entry->spec.hold_ms) fire(hotkeys, entry, now_ms, poll);
    }
}

void xinput_hotkeys_tick(xinput_hotkeys *hotkeys, uint64_t now_ms, uint64_t poll)
{
    if (hotkeys != NULL) evaluate(hotkeys, now_ms, poll);
}

static int swallowed_find(const xinput_hotkeys *hotkeys, xinput_hotkey_device device, const char *name)
{
    for (unsigned i = 0u; i < hotkeys->swallowed_count; i++)
        if (hotkeys->swallowed[i].device == device && names_equal(hotkeys->swallowed[i].name, name)) return (int)i;
    return -1;
}

static bool is_chord_member(const xinput_hotkeys *hotkeys, xinput_hotkey_device device, const char *name)
{
    for (unsigned i = 0u; i < hotkeys->count; i++) {
        unsigned element, alias;
        if (hotkeys->entries[i].spec.device == device && locate(&hotkeys->entries[i], name, &element, &alias)) return true;
    }
    return false;
}

static int leak_find(const xinput_hotkeys *hotkeys, xinput_hotkey_device device, const char *name)
{
    for (unsigned i = 0u; i < hotkeys->leak_count; i++)
        if (hotkeys->leaks[i].device == device && names_equal(hotkeys->leaks[i].name, name)) return (int)i;
    return -1;
}

bool xinput_hotkeys_event(xinput_hotkeys *hotkeys, xinput_hotkey_device device, const char *name, bool down,
                          uint64_t now_ms, uint64_t poll)
{
    if (hotkeys == NULL || name == NULL) return true;
    if (!down) {
        for (unsigned i = 0u; i < hotkeys->count; i++) {
            hotkey_entry *entry = &hotkeys->entries[i];
            unsigned element, alias;
            if (entry->spec.device == device && locate(entry, name, &element, &alias))
                entry->held[element] = (uint8_t)(entry->held[element] & ~(1u << alias));
        }
        evaluate(hotkeys, now_ms, poll);
        const int index = swallowed_find(hotkeys, device, name);
        if (index < 0) {
            const int leak = leak_find(hotkeys, device, name);
            if (leak >= 0) {
                const bool tainted = hotkeys->leaks[leak].tainted;
                hotkeys->leaks[leak] = hotkeys->leaks[--hotkeys->leak_count];
                if (tainted && hotkeys->leak_fn != NULL) hotkeys->leak_fn(device, name, poll, false, hotkeys->leak_user);
            }
            return true;
        }
        hotkeys->swallowed[index] = hotkeys->swallowed[--hotkeys->swallowed_count];
        hotkeys->swallowed_total++;
        return false;
    }
    bool already = false, chord = false;
    for (unsigned i = 0u; i < hotkeys->count; i++) {
        hotkey_entry *entry = &hotkeys->entries[i];
        unsigned element, alias;
        if (entry->spec.device != device || !locate(entry, name, &element, &alias)) continue;
        if ((entry->held[element] & (1u << alias)) != 0u) already = true;
        entry->held[element] = (uint8_t)(entry->held[element] | (1u << alias));
        /* T1632: a chord key is kept from the game only while the chord's LEAD (its first key) is held. Before, any two keys
         * of a chord swallowed the next one, so playing LB+RB or LB+X with no gesture lost the second button. */
        if (element != 0u && entry->held[0] != 0u) chord = true;
    }
    bool forward = true;
    if (already) {
        /* a repeated DOWN: it is swallowed only if its first DOWN was */
        forward = swallowed_find(hotkeys, device, name) < 0;
        if (!forward) hotkeys->swallowed_total++;
    } else if (chord) {
        if (hotkeys->swallowed_count < XINPUT_HOTKEY_SWALLOWED_MAX) {
            swallowed_key *slot = &hotkeys->swallowed[hotkeys->swallowed_count++];
            slot->device = device;
            snprintf(slot->name, sizeof slot->name, "%s", name);
            hotkeys->swallowed_total++;
            forward = false;
        } else {
            hotkeys->swallow_overflow++;
        }
    }
    if (forward && !already && is_chord_member(hotkeys, device, name) && leak_find(hotkeys, device, name) < 0) {
        if (hotkeys->leak_count < LEAK_MAX) {
            leak_episode *episode = &hotkeys->leaks[hotkeys->leak_count++];
            episode->device = device;
            snprintf(episode->name, sizeof episode->name, "%s", name);
            episode->down_poll = poll;
            episode->tainted = false;
        } else {
            hotkeys->leak_overflow++;
        }
    }
    evaluate(hotkeys, now_ms, poll);
    return forward;
}
