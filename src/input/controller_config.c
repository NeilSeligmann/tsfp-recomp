/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "controller_config.h"
#include "xinput_source.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_LIMIT 65536u
static bool fail(char *error, size_t size, const char *format, ...)
{
    if (error != NULL && size != 0u) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return false;
}

void controller_config_defaults(controller_config *config)
{
    memset(config, 0, sizeof(*config));
    config->version = 1u;
    for (unsigned p = 0; p < CONTROLLER_PORTS; p++) {
        config->selectors[p].enabled = true;
        config->selectors[p].occurrence = -1;
        for (unsigned b = 0; b < CONTROLLER_BUTTONS; b++)
            config->profiles[p].button_map[b] = b == 14u ? -1 : (int)b;
        for (unsigned a = 0; a < CONTROLLER_AXES; a++) {
            config->profiles[p].axis_map[a] = (int)a;
            config->profiles[p].calibration[a] = (controller_axis_calibration){
                .min = a < 4u ? -32768 : 0, .center = 0, .max = 32767,
                .deadzone = 0u, .invert = a == 1u || a == 3u};
        }
    }
}

static bool profile_valid(const controller_profile *profile, char *error, size_t size)
{
    for (unsigned b = 0; b < CONTROLLER_BUTTONS; b++)
        if (profile->button_map[b] < -1 || profile->button_map[b] >= (int)CONTROLLER_BUTTONS)
            return fail(error, size, "button %u source must be -1..14", b);
    if (profile->button_map[14] != -1)
        return fail(error, size, "GUIDE has no Xbox target; button 14 must be -1");
    for (unsigned a = 0; a < CONTROLLER_AXES; a++) {
        int source = profile->axis_map[a];
        if (source < 0 || source >= (int)CONTROLLER_AXES || (a < 4u) != (source < 4))
            return fail(error, size, "axis %u source must stay in its stick/trigger class", a);
        const controller_axis_calibration *c = &profile->calibration[a];
        if (c->min < (a < 4u ? -32768 : 0) || c->max > 32767 || c->min >= c->max ||
            (a < 4u ? c->center <= c->min || c->center >= c->max : c->center != c->min) ||
            c->deadzone >= 32767u)
            return fail(error, size, "axis %u invalid min/center/max/deadzone", a);
    }
    return true;
}

bool controller_config_validate(const controller_config *config, char *error, size_t size)
{
    if (error != NULL && size != 0u) error[0] = '\0';
    if (config == NULL || config->version != 1u) return fail(error, size, "config version must be 1");
    const char *end = memchr(config->mapping_file, '\0', sizeof(config->mapping_file));
    if (end == NULL) return fail(error, size, "mapping_file is too long");
    for (const char *s = config->mapping_file; s < end; s++)
        if ((unsigned char)*s < 32u || (unsigned char)*s == 127u)
            return fail(error, size, "mapping_file contains control characters");
    for (unsigned p = 0; p < CONTROLLER_PORTS; p++) {
        const controller_selector *s = &config->selectors[p];
        const char *e = memchr(s->guid, '\0', sizeof(s->guid));
        if (e == NULL || (e != s->guid && e - s->guid != 32))
            return fail(error, size, "port %u GUID must be 32 lowercase hex digits or auto", p);
        for (const char *g = s->guid; g < e; g++)
            if (!(*g >= '0' && *g <= '9') && !(*g >= 'a' && *g <= 'f'))
                return fail(error, size, "port %u GUID must be lowercase hexadecimal", p);
        if (s->occurrence < -1 || s->occurrence > 7 ||
            (s->guid[0] == '\0' && s->occurrence != -1) ||
            (!s->enabled && (s->guid[0] != '\0' || s->occurrence != -1)))
            return fail(error, size, "port %u occurrence must be -1..7; auto/disabled require -1", p);
        if (!profile_valid(&config->profiles[p], error, size)) return false;
        for (unsigned q = 0; q < p; q++) {
            const controller_selector *t = &config->selectors[q];
            if (s->enabled && t->enabled && s->guid[0] != '\0' && strcmp(s->guid, t->guid) == 0 &&
                (s->occurrence == t->occurrence || s->occurrence == -1 || t->occurrence == -1))
                return fail(error, size, "ports %u and %u select overlapping GUID occurrences", q, p);
        }
    }
    return true;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n != 0u && isspace((unsigned char)text[n - 1u])) text[--n] = '\0';
    return text;
}
static bool numbers(char *text, int *out, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        char *end;
        errno = 0;
        if (*text == '\0') return false;
        long value = strtol(text, &end, 10);
        if (end == text || errno != 0 || value < INT_MIN || value > INT_MAX) return false;
        out[i] = (int)value;
        while (isspace((unsigned char)*end)) end++;
        if (i + 1u == count) return *end == '\0';
        if (*end != ',') return false;
        text = end + 1;
    }
    return false;
}
static bool quoted_path(const char *value, char *out)
{
    if (*value++ != '"') return false;
    unsigned n = 0;
    while (*value != '\0' && *value != '"') {
        char c = *value++;
        if (c == '\\') {
            c = *value++;
            if (c != '\\' && c != '"') return false;
        }
        if ((unsigned char)c < 32u || (unsigned char)c == 127u || n + 1u >= CONTROLLER_MAPPING_PATH)
            return false;
        out[n++] = c;
    }
    if (*value != '"' || value[1] != '\0') return false;
    out[n] = '\0';
    return true;
}

bool controller_config_parse(const char *text, size_t len, controller_config *out, char *error, size_t size)
{
    if (out == NULL || text == NULL || len > CONFIG_LIMIT || memchr(text, '\0', len) != NULL)
        return fail(error, size, "config must be non-NUL text of at most 65536 bytes");
    char *copy = malloc(len + 1u);
    if (copy == NULL) return fail(error, size, "out of memory reading config");
    memcpy(copy, text, len);
    copy[len] = '\0';
    controller_config config;
    controller_config_defaults(&config);
    bool seen[42] = {false};
    unsigned line_number = 0u;
    bool ok = true;
    for (char *line = copy; line != NULL;) {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        line_number++;
        if (strlen(line) > 4095u) { ok = fail(error, size, "line %u too long", line_number); break; }
        char *key = trim(line);
        if (*key == '\0' || *key == '#') { line = next; continue; }
        char *value = strchr(key, '=');
        if (value == NULL) { ok = false; break; }
        *value++ = '\0';
        key = trim(key);
        value = trim(value);
        unsigned field = 42u;
        if (strcmp(key, "version") == 0) {
            int number;
            field = 0u;
            ok = numbers(value, &number, 1u) && number == 1;
        } else if (strcmp(key, "mapping_file") == 0) {
            field = 1u;
            ok = quoted_path(value, config.mapping_file);
        } else if (strncmp(key, "port.", 5) == 0 && key[5] >= '0' && key[5] <= '3' && key[6] == '.') {
            unsigned p = (unsigned)(key[5] - '0');
            const char *name = key + 7;
            if (strcmp(name, "guid") == 0) {
                field = 2u + p * 10u;
                config.selectors[p].enabled = strcmp(value, "disabled") != 0;
                if (strcmp(value, "auto") == 0 || !config.selectors[p].enabled) config.selectors[p].guid[0] = '\0';
                else if (strlen(value) == 32u) {
                    for (unsigned i = 0; i <= 32u; i++) config.selectors[p].guid[i] = (char)tolower((unsigned char)value[i]);
                } else ok = false;
            } else if (strcmp(name, "occurrence") == 0) {
                field = 3u + p * 10u;
                ok = numbers(value, &config.selectors[p].occurrence, 1u);
            } else if (strcmp(name, "buttons") == 0) {
                field = 4u + p * 10u;
                ok = numbers(value, config.profiles[p].button_map, CONTROLLER_BUTTONS);
            } else if (strcmp(name, "axes") == 0) {
                field = 5u + p * 10u;
                ok = numbers(value, config.profiles[p].axis_map, CONTROLLER_AXES);
            } else if (strncmp(name, "axis.", 5) == 0 && name[5] >= '0' && name[5] <= '5' && name[6] == '\0') {
                unsigned a = (unsigned)(name[5] - '0');
                int n[5];
                field = 6u + p * 10u + a;
                ok = numbers(value, n, 5u) && n[3] >= 0 && (n[4] == 0 || n[4] == 1);
                if (ok) config.profiles[p].calibration[a] = (controller_axis_calibration){n[0], n[1], n[2], (unsigned)n[3], n[4] != 0};
            }
        }
        if (field == 42u || !ok || seen[field]) { ok = false; break; }
        seen[field] = true;
        line = next;
    }
    free(copy);
    if (!ok) return fail(error, size, "line %u malformed, unknown or duplicate setting", line_number);
    if (!seen[0]) return fail(error, size, "required version=1 is missing");
    if (!controller_config_validate(&config, error, size)) return false;
    *out = config;
    return true;
}

bool controller_config_load(const char *path, controller_config *out, char *error, size_t size)
{
    if (path == NULL) return fail(error, size, "config path is missing");
    FILE *file = fopen(path, "rb");
    if (file == NULL) return fail(error, size, "cannot read controller config: %s", strerror(errno));
    char *text = malloc(CONFIG_LIMIT + 1u);
    if (text == NULL) { fclose(file); return fail(error, size, "out of memory reading config"); }
    size_t len = fread(text, 1u, CONFIG_LIMIT + 1u, file);
    bool ok = !ferror(file);
    fclose(file);
    if (ok) ok = controller_config_parse(text, len, out, error, size);
    else fail(error, size, "controller config read failed");
    free(text);
    return ok;
}

bool controller_config_save(const char *path, const controller_config *config, char *error, size_t size)
{
    if (!controller_config_validate(config, error, size)) return false;
    if (path == NULL || *path == '\0') return fail(error, size, "config path is missing");
    size_t len = strlen(path);
    if (len > 4096u) return fail(error, size, "config path is too long");
    char *temp = malloc(len + 12u);
    if (temp == NULL) return fail(error, size, "out of memory saving config");
    snprintf(temp, len + 12u, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd < 0) { free(temp); return fail(error, size, "cannot create config temporary file: %s", strerror(errno)); }
    FILE *file = fdopen(fd, "w");
    bool ok = file != NULL;
    if (file == NULL) close(fd);
    if (ok) {
        fprintf(file, "version=1\nmapping_file=\"");
        for (const char *s = config->mapping_file; *s != '\0'; s++) {
            if (*s == '\\' || *s == '"') fputc('\\', file);
            fputc(*s, file);
        }
        fprintf(file, "\"\n");
        for (unsigned p = 0; p < CONTROLLER_PORTS; p++) {
            const controller_selector *s = &config->selectors[p];
            const controller_profile *profile = &config->profiles[p];
            fprintf(file, "port.%u.guid=%s\nport.%u.occurrence=%d\nport.%u.buttons=", p,
                    !s->enabled ? "disabled" : s->guid[0] == '\0' ? "auto" : s->guid, p, s->occurrence, p);
            for (unsigned b = 0; b < CONTROLLER_BUTTONS; b++) fprintf(file, "%s%d", b == 0u ? "" : ",", profile->button_map[b]);
            fprintf(file, "\nport.%u.axes=", p);
            for (unsigned a = 0; a < CONTROLLER_AXES; a++) fprintf(file, "%s%d", a == 0u ? "" : ",", profile->axis_map[a]);
            fputc('\n', file);
            for (unsigned a = 0; a < CONTROLLER_AXES; a++) {
                const controller_axis_calibration *c = &profile->calibration[a];
                fprintf(file, "port.%u.axis.%u=%d,%d,%d,%u,%u\n", p, a, c->min, c->center, c->max, c->deadzone, c->invert ? 1u : 0u);
            }
        }
        ok = !ferror(file) && fflush(file) == 0 && fsync(fd) == 0;
        if (fclose(file) != 0) ok = false;
    }
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    free(temp);
    return ok ? true : fail(error, size, "could not persist controller config: %s", strerror(errno));
}

static int16_t normalized(const controller_axis_calibration *c, int16_t input, bool trigger)
{
    int32_t value = input;
    if (value < c->min) value = c->min;
    if (value > c->max) value = c->max;
    int32_t n;
    if (trigger) n = (int32_t)((int64_t)(value - c->min) * 32767 / (c->max - c->min));
    else if (value >= c->center) n = (int32_t)((int64_t)(value - c->center) * 32767 / (c->max - c->center));
    else n = -(int32_t)((int64_t)(c->center - value) * 32768 / (c->center - c->min));
    if (c->invert) n = trigger ? 32767 - n : n == -32768 ? 32767 : -n;
    int32_t magnitude = n < 0 ? -n : n;
    if (magnitude <= (int32_t)c->deadzone) return 0;
    int32_t maximum = n < 0 ? 32768 : 32767;
    n = (int32_t)((int64_t)(magnitude - (int32_t)c->deadzone) * maximum / (maximum - (int32_t)c->deadzone)) * (n < 0 ? -1 : 1);
    return (int16_t)n;
}

void controller_profile_apply(const controller_profile *profile, const bool buttons[CONTROLLER_BUTTONS],
                              const int16_t axes[CONTROLLER_AXES], xinput_pad_state *out)
{
    memset(out, 0, sizeof(*out));
    if (profile == NULL || buttons == NULL || axes == NULL || !profile_valid(profile, NULL, 0u)) return;
    static const uint8_t face[6] = {0, 1, 2, 3, 4, 5};
    static const uint16_t digital[8] = {XINPUT_BUTTON_START, XINPUT_BUTTON_BACK, XINPUT_BUTTON_LEFT_THUMB,
        XINPUT_BUTTON_RIGHT_THUMB, XINPUT_BUTTON_DPAD_UP, XINPUT_BUTTON_DPAD_DOWN,
        XINPUT_BUTTON_DPAD_LEFT, XINPUT_BUTTON_DPAD_RIGHT};
    for (unsigned b = 0; b < 14u; b++) {
        int source = profile->button_map[b];
        if (source < 0 || !buttons[source]) continue;
        if (b < 6u) out->analog[face[b]] = 255u;
        else out->digital_buttons |= digital[b - 6u];
    }
    int16_t mapped[CONTROLLER_AXES];
    for (unsigned a = 0; a < CONTROLLER_AXES; a++) {
        int source = profile->axis_map[a];
        mapped[a] = normalized(&profile->calibration[source], axes[source], a >= 4u);
    }
    out->thumb_left_x = mapped[0]; out->thumb_left_y = mapped[1];
    out->thumb_right_x = mapped[2]; out->thumb_right_y = mapped[3];
    out->analog[XINPUT_ANALOG_LEFT_TRIGGER] = (uint8_t)(mapped[4] >> 7);
    out->analog[XINPUT_ANALOG_RIGHT_TRIGGER] = (uint8_t)(mapped[5] >> 7);
}
