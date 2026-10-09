/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_source.h"
#include "xinput_devices.h"
#include "xinput_record.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

static pthread_mutex_t source_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Provider lifetimes must span in-flight sampling; callbacks run without this
 * lock so hotplug can safely reenter the serialized device adapter. */
static xinput_source_fn source_fn;
static xinput_ports_source_fn ports_source_fn;
static void *source_user;
static void *ports_source_user;
static uint64_t polls;
static uint64_t port_polls[XINPUT_PORT_COUNT];
static xinput_feedback_fn feedback_fn;
static void *feedback_user;
static xinput_pre_install_hook_fn pre_install_hook;
static void *pre_install_user;

struct xinput_script {
    xinput_script_entry *entries;
    size_t count;
    uint64_t total;
};

static void inner_xinput_source_install(xinput_source_fn source, void *user)
{
    (void)pthread_mutex_lock(&source_mutex);
    source_fn = source; source_user = user;
    (void)pthread_mutex_unlock(&source_mutex);
}
static void inner_xinput_source_install_ports(xinput_ports_source_fn source, void *user)
{
    (void)pthread_mutex_lock(&source_mutex);
    ports_source_fn = source; ports_source_user = user;
    (void)pthread_mutex_unlock(&source_mutex);
}
static bool inner_xinput_feedback_send(unsigned port, uint16_t left, uint16_t right)
{
    if (port >= XINPUT_PORT_COUNT) return false;
    (void)pthread_mutex_lock(&source_mutex);
    xinput_feedback_fn fn = feedback_fn; void *user = feedback_user;
    (void)pthread_mutex_unlock(&source_mutex);
    return fn != NULL && fn(port, left, right, user);
}
static void inner_xinput_set_pre_install_hook(xinput_pre_install_hook_fn hook, void *user)
{
    (void)pthread_mutex_lock(&source_mutex);
    pre_install_hook = hook; pre_install_user = user;
    (void)pthread_mutex_unlock(&source_mutex);
}
static void inner_xinput_feedback_install(xinput_feedback_fn feedback, void *user)
{
    (void)pthread_mutex_lock(&source_mutex);
    xinput_feedback_fn previous = feedback_fn; void *previous_user = feedback_user;
    feedback_fn = feedback; feedback_user = user;
    (void)pthread_mutex_unlock(&source_mutex);
    if (previous != NULL)
        for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++) (void)previous(port, 0u, 0u, previous_user);
}
static void inner_xinput_source_reset(void)
{
    xinput_feedback_install(NULL, NULL);
    (void)pthread_mutex_lock(&source_mutex);
    source_fn = NULL; ports_source_fn = NULL; source_user = NULL; ports_source_user = NULL; polls = 0u;
    pre_install_hook = NULL; pre_install_user = NULL;
    memset(port_polls, 0, sizeof(port_polls));
    (void)pthread_mutex_unlock(&source_mutex);
}
uint64_t xinput_source_poll_count(void)
{
    (void)pthread_mutex_lock(&source_mutex); uint64_t result = polls; (void)pthread_mutex_unlock(&source_mutex); return result;
}
uint64_t xinput_source_port_poll_count(unsigned port)
{
    (void)pthread_mutex_lock(&source_mutex); uint64_t result = port < XINPUT_PORT_COUNT ? port_polls[port] : 0u;
    (void)pthread_mutex_unlock(&source_mutex); return result;
}
static bool inner_xinput_source_poll_port(unsigned port)
{
    if (port >= XINPUT_PORT_COUNT) return false;
    (void)pthread_mutex_lock(&source_mutex);
    const uint64_t index = port_polls[port]++; polls++;
    xinput_ports_source_fn port_fn = ports_source_fn; void *port_user = ports_source_user;
    xinput_source_fn legacy_fn = source_fn; void *legacy_user = source_user;
    xinput_pre_install_hook_fn hook = port == 0u ? pre_install_hook : NULL; void *hook_user = pre_install_user;
    (void)pthread_mutex_unlock(&source_mutex);
    xinput_pad_state state = {0};
    bool supplied = port_fn != NULL && port_fn(index, port, &state, port_user);
    if (!supplied) {
        memset(&state, 0, sizeof(state));
        if (port != 0u || legacy_fn == NULL || !legacy_fn(index, &state, legacy_user)) {
            xinput_record_observe(index, port, NULL);
            return false;
        }
    }
    if (hook != NULL) hook(index, &state, hook_user); /* T1629: observer, see the header for the rules */
    bool installed = xinput_hle_set_synthetic_pad_state(port, state);
    xinput_record_observe(index, port, installed ? &state : NULL);
    return installed;
}
bool xinput_source_poll(void) { return xinput_source_poll_port(0u); }

/* Share device serialization for provider installation, cancellation and calls.
 * Once uninstall returns, no earlier sample/feedback callback remains in flight. */
typedef struct {
    unsigned operation, port;
    xinput_source_fn legacy;
    xinput_ports_source_fn ports;
    xinput_feedback_fn feedback;
    xinput_pre_install_hook_fn hook;
    void *user;
    uint16_t left, right;
    bool result;
} source_operation;
static void source_job(void *user)
{
    source_operation *op = user;
    switch (op->operation) {
    case 0u: inner_xinput_source_install(op->legacy, op->user); break;
    case 1u: inner_xinput_source_install_ports(op->ports, op->user); break;
    case 2u: inner_xinput_feedback_install(op->feedback, op->user); break;
    case 3u: inner_xinput_source_reset(); break;
    case 4u: op->result = inner_xinput_source_poll_port(op->port); break;
    case 6u: inner_xinput_set_pre_install_hook(op->hook, op->user); break;
    default: op->result = inner_xinput_feedback_send(op->port, op->left, op->right); break;
    }
}
void xinput_source_install(xinput_source_fn source, void *user)
{
    source_operation op = {.operation = 0u, .legacy = source, .user = user};
    xinput_devices_run_locked(source_job, &op);
}
void xinput_source_install_ports(xinput_ports_source_fn source, void *user)
{
    source_operation op = {.operation = 1u, .ports = source, .user = user};
    xinput_devices_run_locked(source_job, &op);
}
void xinput_source_set_pre_install_hook(xinput_pre_install_hook_fn hook, void *user)
{
    source_operation op = {.operation = 6u, .hook = hook, .user = user};
    xinput_devices_run_locked(source_job, &op);
}
void xinput_feedback_install(xinput_feedback_fn feedback, void *user)
{
    source_operation op = {.operation = 2u, .feedback = feedback, .user = user};
    xinput_devices_run_locked(source_job, &op);
}
void xinput_source_reset(void)
{
    source_operation op = {.operation = 3u}; xinput_devices_run_locked(source_job, &op);
}
static xinput_poll_observer_fn g_poll_observer;
static void *g_poll_observer_user;
void xinput_source_set_poll_observer(xinput_poll_observer_fn observer, void *user)
{
    g_poll_observer_user = user; g_poll_observer = observer;
}
bool xinput_source_poll_port(unsigned port)
{
    source_operation op = {.operation = 4u, .port = port}; xinput_devices_run_locked(source_job, &op);
    xinput_poll_observer_fn observer = g_poll_observer;
    if (observer != NULL && port < XINPUT_PORT_COUNT) observer(port, xinput_source_port_poll_count(port), g_poll_observer_user);
    return op.result;
}
bool xinput_feedback_send(unsigned port, uint16_t left, uint16_t right)
{
    source_operation op = {.operation = 5u, .port = port, .left = left, .right = right};
    xinput_devices_run_locked(source_job, &op); return op.result;
}

static void fail(char *error, size_t size, unsigned line, const char *what, const char *token)
{
    if (error != NULL && size != 0u) snprintf(error, size, "line %u: %s '%s'", line, what, token);
}
static bool parse_number(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) return false;
    *out = value;
    return true;
}
static bool apply_token(xinput_pad_state *state, char *token, unsigned line, char *error, size_t size)
{
    static const struct { const char *name; uint16_t bit; } digital[] = {
        {"UP", XINPUT_BUTTON_DPAD_UP}, {"DOWN", XINPUT_BUTTON_DPAD_DOWN}, {"LEFT", XINPUT_BUTTON_DPAD_LEFT},
        {"RIGHT", XINPUT_BUTTON_DPAD_RIGHT}, {"START", XINPUT_BUTTON_START}, {"BACK", XINPUT_BUTTON_BACK},
        {"LTHUMB", XINPUT_BUTTON_LEFT_THUMB}, {"RTHUMB", XINPUT_BUTTON_RIGHT_THUMB},
    };
    static const char *const analog[XINPUT_ANALOG_COUNT] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
    static const char *const axes[4] = {"LX", "LY", "RX", "RY"};
    char *equals = strchr(token, '=');
    const char *value_text = NULL;
    if (equals != NULL) { *equals = '\0'; value_text = equals + 1; }
    for (char *c = token; *c != '\0'; c++) *c = (char)toupper((unsigned char)*c);
    if (value_text == NULL) {
        for (size_t i = 0u; i < sizeof(digital) / sizeof(digital[0]); i++)
            if (strcmp(token, digital[i].name) == 0) { state->digital_buttons |= digital[i].bit; return true; }
        for (unsigned i = 0u; i < 6u; i++)
            if (strcmp(token, analog[i]) == 0) { state->analog[i] = 255u; return true; }
        fail(error, size, line, "unknown token", token);
        return false;
    }
    long value = 0;
    for (unsigned i = 0u; i < XINPUT_ANALOG_COUNT; i++)
        if (strcmp(token, analog[i]) == 0) {
            if (!parse_number(value_text, 0, 255, &value)) { fail(error, size, line, "analog value not 0..255", value_text); return false; }
            state->analog[i] = (uint8_t)value;
            return true;
        }
    int16_t *const targets[4] = {&state->thumb_left_x, &state->thumb_left_y, &state->thumb_right_x, &state->thumb_right_y};
    for (unsigned i = 0u; i < 4u; i++)
        if (strcmp(token, axes[i]) == 0) {
            if (!parse_number(value_text, -32768, 32767, &value)) { fail(error, size, line, "axis value not -32768..32767", value_text); return false; }
            *targets[i] = (int16_t)value;
            return true;
        }
    fail(error, size, line, "unknown token", token);
    return false;
}

xinput_script *xinput_script_parse(const char *text, size_t len, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0u) error[0] = '\0';
    xinput_script *script = calloc(1u, sizeof(*script));
    if (script == NULL) return NULL;
    unsigned line_number = 0u;
    size_t at = 0u;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        char line[512];
        const size_t n = end - at;
        line_number++;
        if (n >= sizeof(line)) { fail(error, error_size, line_number, "line too long", ""); goto bad; }
        memcpy(line, text + at, n);
        line[n] = '\0';
        at = end + 1u;
        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        char *save = NULL;
        char *first = strtok_r(line, " \t\r", &save);
        if (first == NULL) continue;
        long frames = 0;
        if (!parse_number(first, 1, 1000000, &frames)) { fail(error, error_size, line_number, "frame count not 1..1000000", first); goto bad; }
        xinput_pad_state state;
        memset(&state, 0, sizeof(state));
        for (char *token = strtok_r(NULL, " \t\r", &save); token != NULL; token = strtok_r(NULL, " \t\r", &save))
            if (!apply_token(&state, token, line_number, error, error_size)) goto bad;
        xinput_script_entry *grown = realloc(script->entries, (script->count + 1u) * sizeof(*grown));
        if (grown == NULL) goto bad;
        script->entries = grown;
        script->entries[script->count].frames = (uint32_t)frames;
        script->entries[script->count].state = state;
        script->count++;
        script->total += (uint64_t)frames;
    }
    return script;
bad:
    xinput_script_free(script);
    return NULL;
}

xinput_script *xinput_script_load(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }
    char *buffer = NULL;
    size_t size = 0u, used = 0u;
    for (;;) {
        if (used == size) {
            size = size ? size * 2u : 4096u;
            char *grown = realloc(buffer, size);
            if (grown == NULL) { free(buffer); fclose(file); return NULL; }
            buffer = grown;
        }
        const size_t got = fread(buffer + used, 1u, size - used, file);
        used += got;
        if (got == 0u) break;
    }
    fclose(file);
    xinput_script *script = xinput_script_parse(buffer, used, error, error_size);
    free(buffer);
    return script;
}

void xinput_script_free(xinput_script *script)
{
    if (script == NULL) return;
    free(script->entries);
    free(script);
}
size_t xinput_script_entry_count(const xinput_script *script) { return script != NULL ? script->count : 0u; }
uint64_t xinput_script_total_frames(const xinput_script *script) { return script != NULL ? script->total : 0u; }
xinput_pad_state xinput_script_state_at(const xinput_script *script, uint64_t poll_index)
{
    xinput_pad_state rest;
    memset(&rest, 0, sizeof(rest));
    if (script == NULL) return rest;
    uint64_t base = 0u;
    for (size_t i = 0u; i < script->count; i++) {
        if (poll_index < base + script->entries[i].frames) return script->entries[i].state;
        base += script->entries[i].frames;
    }
    return rest;
}
static bool script_source(uint64_t poll_index, xinput_pad_state *out, void *user)
{
    *out = xinput_script_state_at((const xinput_script *)user, poll_index);
    return true;
}
void xinput_script_install(const xinput_script *script) { xinput_source_install(script_source, (void *)script); }

/* T1222: live pad. The file holds ONE line of tokens (the script syntax without the frame count), re-read at every poll,
 * so a controller process can change the pad while the title runs. A line that does not parse leaves the previous state
 * (counted in `refused`), an empty file is the pad at rest. FABRICATED input like the script. */
struct xinput_live {
    int fd;
    xinput_pad_state last;
    uint64_t refused;
};
bool xinput_live_parse_line(const char *text, size_t len, xinput_pad_state *out)
{
    char line[512];
    size_t n = 0u;
    while (n < len && n < sizeof(line) - 1u && text[n] != '\n' && text[n] != '\0') n++;
    memcpy(line, text, n);
    line[n] = '\0';
    char *hash = strchr(line, '#');
    if (hash != NULL) *hash = '\0';
    xinput_pad_state state;
    memset(&state, 0, sizeof(state));
    char *save = NULL;
    for (char *token = strtok_r(line, " \t\r", &save); token != NULL; token = strtok_r(NULL, " \t\r", &save))
        if (!apply_token(&state, token, 1u, NULL, 0u)) return false;
    *out = state;
    return true;
}
xinput_live *xinput_live_open(const char *path, char *error, size_t error_size)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }
    xinput_live *live = calloc(1u, sizeof(*live));
    if (live == NULL) { close(fd); return NULL; }
    live->fd = fd;
    return live;
}
void xinput_live_free(xinput_live *live)
{
    if (live == NULL) return;
    close(live->fd);
    free(live);
}
uint64_t xinput_live_refused(const xinput_live *live) { return live != NULL ? live->refused : 0u; }
static bool live_source(uint64_t poll_index, xinput_pad_state *out, void *user)
{
    (void)poll_index;
    xinput_live *live = user;
    char buffer[512];
    const ssize_t got = pread(live->fd, buffer, sizeof(buffer), 0);
    if (got < 0) { live->refused++; *out = live->last; return true; }
    xinput_pad_state state;
    if (xinput_live_parse_line(buffer, (size_t)got, &state)) live->last = state;
    else live->refused++;
    *out = live->last;
    return true;
}
void xinput_live_install(xinput_live *live) { xinput_source_install(live_source, live); }
