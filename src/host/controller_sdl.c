/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller_sdl.h"
#include "xinput_devices.h"
#include "xinput_source.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TSFP_HAVE_SDL3
#include <SDL3/SDL.h>
#endif
struct controller_sdl {
    present_video_sink *sink;
    controller_config config;
    pthread_mutex_t lock;
    xinput_pad_state states[4];
    bool connected[4], rumble[4];
    uint32_t delivered_ids[4];
    char names[4][128], guids[4][33];
    atomic_uint feedback[4];
    atomic_bool feedback_dirty[4];
    bool installed, initialized;
    atomic_bool stopping;
    char error[256];
    char unmapped[1024];
    const char *mapping_file;
#ifdef TSFP_HAVE_SDL3
    SDL_Gamepad *pads[4];
    SDL_JoystickID ids[4];
#endif
};
#ifdef TSFP_HAVE_SDL3
static void drop_port(controller_sdl *c, unsigned p)
{
    if (c->pads[p] == NULL) return;
    (void)SDL_RumbleGamepad(c->pads[p], 0, 0, 0);
    SDL_CloseGamepad(c->pads[p]); c->pads[p] = NULL; c->ids[p] = 0;
    atomic_store(&c->feedback[p], 0); atomic_store(&c->feedback_dirty[p], false);
    pthread_mutex_lock(&c->lock);
    memset(&c->states[p], 0, sizeof(c->states[p])); c->connected[p] = false;
    c->rumble[p] = false; c->names[p][0] = '\0'; c->guids[p][0] = '\0';
    pthread_mutex_unlock(&c->lock);

}
static bool used_id(controller_sdl *c, SDL_JoystickID id)
{
    for (unsigned p = 0; p < 4; ++p) if (c->ids[p] == id) return true;
    return false;
}
static void pump_job(void *user)
{
    controller_sdl *c = user;
    if (atomic_load(&c->stopping)) return;
    SDL_PumpEvents(); SDL_UpdateGamepads();
    for (unsigned p = 0; p < 4; ++p)
        if (c->pads[p] && !SDL_GamepadConnected(c->pads[p])) drop_port(c, p);
    int count = 0; SDL_JoystickID *ids = SDL_GetGamepads(&count);
    /* Explicit selections first: automatic ports must not steal their devices. */
    for (unsigned pass = 0; pass < 2; ++pass) for (unsigned p = 0; p < 4; ++p) {
        const controller_selector *selector = &c->config.selectors[p];
        if (!selector->enabled || c->pads[p] || (selector->guid[0] == '\0') != (pass == 1)) continue;
        int occurrence = 0;
        for (int i = 0; ids && i < count; ++i) {
            char guid[33]; SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof(guid));
            if (selector->guid[0] && strcmp(selector->guid, guid) != 0) continue;
            int n = occurrence++;
            if (selector->occurrence >= 0 && selector->occurrence != n) continue;
            if (used_id(c, ids[i])) continue;
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (!pad) { snprintf(c->error, sizeof(c->error), "cannot open gamepad: %s", SDL_GetError()); continue; }
            c->pads[p] = pad; c->ids[p] = ids[i];
            (void)SDL_SetGamepadPlayerIndex(pad, (int)p);
            const char *name = SDL_GetGamepadName(pad);
            pthread_mutex_lock(&c->lock);
            c->connected[p] = true;
            snprintf(c->names[p], sizeof(c->names[p]), "%s", name ? name : "Unnamed controller");
            snprintf(c->guids[p], sizeof(c->guids[p]), "%s", guid);
            c->rumble[p] = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            pthread_mutex_unlock(&c->lock);
            break;
        }
    }
    SDL_free(ids);
    int joystick_count = 0; SDL_JoystickID *joysticks = SDL_GetJoysticks(&joystick_count);
    pthread_mutex_lock(&c->lock); c->unmapped[0] = '\0'; size_t unknown_at = 0;
    for (int i = 0; joysticks && i < joystick_count; ++i) {
        if (SDL_IsGamepad(joysticks[i])) continue;
        char guid[33]; SDL_GUIDToString(SDL_GetJoystickGUIDForID(joysticks[i]), guid, sizeof(guid));
        const char *name = SDL_GetJoystickNameForID(joysticks[i]);
        if (unknown_at < sizeof(c->unmapped)) {
            int n = snprintf(c->unmapped + unknown_at, sizeof(c->unmapped) - unknown_at,
                             "unmapped joystick: %s [%s]; supply a local SDL mapping file\n", name ? name : "unnamed", guid);
            if (n > 0) unknown_at += (size_t)n;
        }
    }
    pthread_mutex_unlock(&c->lock); SDL_free(joysticks);
    static const SDL_GamepadButton buttons[15] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_GUIDE};
    for (unsigned p = 0; p < 4; ++p) {
        if (!c->pads[p]) continue;
        bool pressed[15]; int16_t axes[6]; xinput_pad_state state;
        for (unsigned b = 0; b < 15; ++b) pressed[b] = SDL_GetGamepadButton(c->pads[p], buttons[b]);
        for (unsigned a = 0; a < 6; ++a) axes[a] = SDL_GetGamepadAxis(c->pads[p], (SDL_GamepadAxis)a);
        controller_profile_apply(&c->config.profiles[p], pressed, axes, &state);
        pthread_mutex_lock(&c->lock); c->states[p] = state; pthread_mutex_unlock(&c->lock);
        if (atomic_exchange(&c->feedback_dirty[p], false)) {
            unsigned value = atomic_load(&c->feedback[p]);
            if (c->rumble[p] && !SDL_RumbleGamepad(c->pads[p], (Uint16)(value >> 16), (Uint16)value,
                                                value ? SDL_MAX_UINT32 : 0))
                snprintf(c->error, sizeof(c->error), "rumble failed: %s", SDL_GetError());
        }
    }
}
static void open_job(void *user)
{
    controller_sdl *c = user;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) { snprintf(c->error, sizeof(c->error), "%s", SDL_GetError()); return; }
    c->initialized = true;
    if (c->mapping_file && c->mapping_file[0] && SDL_AddGamepadMappingsFromFile(c->mapping_file) < 0) {
        snprintf(c->error, sizeof(c->error), "mapping file %s: %s", c->mapping_file, SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD); c->initialized = false;
    }
}
static void cancel_job(void *user)
{
    controller_sdl *c = user;
    for (unsigned p = 0; p < 4; ++p) {
        atomic_store(&c->feedback[p], 0); atomic_store(&c->feedback_dirty[p], false);
        if (c->pads[p]) (void)SDL_RumbleGamepad(c->pads[p], 0, 0, 0);
    }
}
static void close_job(void *user)
{
    controller_sdl *c = user;
    for (unsigned p = 0; p < 4; ++p) drop_port(c, p);
    if (c->initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
#endif
#ifdef TSFP_HAVE_SDL3
static void pump_locked(void *user)
{
    controller_sdl *c = user;
    if (!c->initialized || !present_video_sink_run(c->sink, pump_job, c)) return;
    for (unsigned p = 0; p < 4; ++p) {
        if (c->delivered_ids[p] == c->ids[p]) continue;
        if (c->delivered_ids[p]) (void)xinput_pad_disconnect(p);
        c->delivered_ids[p] = c->ids[p];
        if (c->delivered_ids[p]) (void)xinput_pad_connect(p);
    }
}
#endif
void controller_sdl_pump(controller_sdl *c)
{
#ifdef TSFP_HAVE_SDL3
    if (c) xinput_devices_run_locked(pump_locked, c);
#else
    (void)c;
#endif
}
static bool sample(uint64_t index, unsigned port, xinput_pad_state *out, void *user)
{
    (void)index; controller_sdl *c = user;
    if (port >= 4 || atomic_load(&c->stopping)) return false;
    controller_sdl_pump(c);
    pthread_mutex_lock(&c->lock); *out = c->states[port]; bool connected = c->connected[port]; pthread_mutex_unlock(&c->lock);
    return connected;
}
static bool feedback(unsigned port, uint16_t left, uint16_t right, void *user)
{
    controller_sdl *c = user; if (port >= 4 || atomic_load(&c->stopping)) return false;
    pthread_mutex_lock(&c->lock); bool capable = c->connected[port] && c->rumble[port]; pthread_mutex_unlock(&c->lock);
    if (!capable) return false;
    atomic_store(&c->feedback[port], ((unsigned)left << 16) | right);
    atomic_store(&c->feedback_dirty[port], true);
    return true;
}
void controller_sdl_install(controller_sdl *c)
{
    if (!c) return;
    xinput_source_install_ports(sample, c); xinput_feedback_install(feedback, c); c->installed = true;
}
controller_sdl *controller_sdl_open(present_video_sink *sink, const controller_config *config,
                                  const char *mapping_file, char *error, size_t size)
{
    char detail[256] = "controller support requires SDL3 and --present window";
#ifdef TSFP_HAVE_SDL3
    if (sink && controller_config_validate(config, detail, sizeof(detail))) {
        controller_sdl *c = calloc(1, sizeof(*c));
        if (!c) { snprintf(detail, sizeof(detail), "cannot allocate controller backend"); goto fail; }
        c->sink = sink; c->config = *config;
        c->mapping_file = mapping_file ? mapping_file : config->mapping_file;
        pthread_mutex_init(&c->lock, NULL);
        if (!present_video_sink_run(sink, open_job, c) || !c->initialized) {
            if (c->error[0]) snprintf(detail, sizeof(detail), "%s", c->error);
            pthread_mutex_destroy(&c->lock); free(c); goto fail;
        }
        controller_sdl_pump(c); return c;
    }
#else
    (void)sink; (void)config; (void)mapping_file;
#endif
#ifdef TSFP_HAVE_SDL3
fail:
#endif
    if (error && size) snprintf(error, size, "%s", detail);
    return NULL;
}
void controller_sdl_cancel(controller_sdl *c)
{
#ifdef TSFP_HAVE_SDL3
    if (c) { atomic_store(&c->stopping, true); (void)present_video_sink_run(c->sink, cancel_job, c); }
#else
    (void)c;
#endif
}
static void close_locked(void *user)
{
    controller_sdl *c = user;
    if (c->installed) { xinput_source_install_ports(NULL, NULL); xinput_feedback_install(NULL, NULL); }
#ifdef TSFP_HAVE_SDL3
    (void)present_video_sink_run(c->sink, close_job, c);
    for (unsigned p = 0; p < 4; ++p) if (c->delivered_ids[p]) (void)xinput_pad_disconnect(p);
#endif
}
void controller_sdl_close(controller_sdl *c)
{
    if (!c) return;
    atomic_store(&c->stopping, true);
    /* Caller stops its pumping/guest threads before close; no callback may outlive c. */
    xinput_devices_run_locked(close_locked, c);
    pthread_mutex_destroy(&c->lock); free(c);
}
#ifdef TSFP_HAVE_SDL3
typedef struct { controller_sdl *controllers; unsigned port; controller_selector selector; bool good; } assignment;
static void assign_job(void *user)
{
    assignment *a = user; controller_sdl *c = a->controllers;
    controller_config proposed = c->config; proposed.selectors[a->port] = a->selector;
    char error[256]; if (!controller_config_validate(&proposed, error, sizeof(error))) return;
    drop_port(c, a->port); c->config = proposed; pump_job(c); a->good = true;
}
#endif
#ifdef TSFP_HAVE_SDL3
static void assign_locked(void *user)
{
    assignment *request = user;
    if (!present_video_sink_run(request->controllers->sink, assign_job, request) || !request->good) return;
    pump_locked(request->controllers);
}
#endif
bool controller_sdl_assign(controller_sdl *c, unsigned port, const controller_selector *selector)
{
    if (!c || !selector || port >= 4) return false;
#ifdef TSFP_HAVE_SDL3
    assignment request = {c, port, *selector, false};
    xinput_devices_run_locked(assign_locked, &request); return request.good;
#else
    return false;
#endif
}
size_t controller_sdl_describe(controller_sdl *c, char *buffer, size_t size)
{
    if (!buffer || !size) return 0;
    if (!c) return (size_t)snprintf(buffer, size, "controller backend disabled");
    size_t at = 0; pthread_mutex_lock(&c->lock);
    for (unsigned p = 0; p < 4; ++p) {
        int n = snprintf(buffer + (at < size ? at : size - 1), at < size ? size - at : 1,
                         "port %u: %s%s%s%s%s\n", p, c->connected[p] ? c->names[p] : "no controller",
                         c->connected[p] ? " [" : "", c->guids[p], c->connected[p] ? "]" : "",
                         c->rumble[p] ? " rumble" : "");
        if (n > 0) at += (size_t)n;
    }
    bool any_connected = false;
    for (unsigned p = 0; p < 4; ++p) any_connected |= c->connected[p];
    if (!any_connected) {
        int n = snprintf(buffer + (at < size ? at : size - 1), at < size ? size - at : 1,
                         "No assigned SDL gamepad; connect a compatible device, check port GUID/occurrence selectors, or supply --controller-mappings FILE.\n");
        if (n > 0) at += (size_t)n;
    }
    if (c->unmapped[0] || c->error[0]) {
        int n = snprintf(buffer + (at < size ? at : size - 1), at < size ? size - at : 1,
                         "%s%s%s", c->unmapped, c->error[0] ? "controller error: " : "", c->error);
        if (n > 0) at += (size_t)n;
    }
    pthread_mutex_unlock(&c->lock); return at;
}
