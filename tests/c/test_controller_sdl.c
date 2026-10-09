/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real SDL virtual devices + production guest adapter. No physical/title run claimed. */
#include "test_d3d8_support.h"
#include "controller_sdl.h"
#include "xinput_devices.h"
#include "xinput_source.h"
#ifdef TSFP_HAVE_SDL3
#include <SDL3/SDL.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define FEEDBACK (SCRATCH_DATA + 0x200u)
typedef struct { unsigned calls; uint16_t left, right; } motors;
static bool SDLCALL rumble(void *user, Uint16 left, Uint16 right)
{
    motors *m = user; ++m->calls; m->left = left; m->right = right; return true;
}
static SDL_JoystickID attach(const char *name, unsigned product, motors *m)
{
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT; d.name = name;
    d.vendor_id = 0x1209; d.product_id = (Uint16)product;
    d.userdata = m; d.Rumble = m ? rumble : NULL;
    return SDL_AttachVirtualJoystick(&d);
}
static void read_port(unsigned p, uint32_t handle)
{
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT + p * 32u), 0u);
}
static uint8_t analog(unsigned p, unsigned b) { return *(uint8_t *)kernel_guest_at(OUT + p * 32u + 6u + b, 1); }
static atomic_bool polling_stop;
static atomic_uint polling_count;
static void *polling_thread(void *user)
{
    (void)user;
    while (!atomic_load(&polling_stop)) {
        (void)xinput_source_poll_port(0); atomic_fetch_add(&polling_count, 1);
    }
    return NULL;
}
static void test_real_virtual_ports(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); xinput_hle_init(); xinput_devices_reset(); xinput_source_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u); map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, sizeof(declarations));
    xinput_devices_enable_synthetic_pad(true); xinput_devices_enable_multiport(true);
    const char *error = NULL;
    present_video_sink *sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "T942 virtual controllers", &error);
    CHECK(sink != NULL); if (!sink) return;
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    motors m[4] = {{0}}; SDL_JoystickID ids[4]; SDL_Joystick *joysticks[4];
    for (unsigned p = 0; p < 4; ++p) {
        ids[p] = attach("T942 virtual controller", p + 1u, &m[p]); CHECK(ids[p] != 0);
        joysticks[p] = SDL_OpenJoystick(ids[p]); CHECK(joysticks[p] != NULL);
    }
    controller_config config; controller_config_defaults(&config);
    /* Reverse explicit identities: assignment is independent of SDL enumeration order. */
    for (unsigned p = 0; p < 4; ++p) {
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[3u-p]), config.selectors[p].guid, 33);
        config.selectors[p].occurrence = 0;
    }
    char text[1024];
    controller_sdl *c = controller_sdl_open(sink, &config, NULL, text, sizeof(text)); CHECK(c != NULL);
    if (!c) { printf("backend: %s\n", text); return; }
    controller_sdl_install(c);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 15u);
    uint32_t handles[4];
    for (unsigned p = 0; p < 4; ++p) { handles[p] = xinput_pad_open(TYPE, p, 0, 0); CHECK(handles[p] != 0); }
    for (unsigned physical = 0; physical < 4; ++physical) {
        CHECK(SDL_SetJoystickVirtualButton(joysticks[physical], SDL_GAMEPAD_BUTTON_SOUTH, true));
        for (unsigned p = 0; p < 4; ++p) { read_port(p, handles[p]); CHECK_EQ_U32(analog(p, 0), p == 3u-physical ? 255u : 0u); }
        CHECK(SDL_SetJoystickVirtualButton(joysticks[physical], SDL_GAMEPAD_BUTTON_SOUTH, false));
    }
    /* Each independent port has its own six axes, not a merged event stream. */
    for (unsigned physical = 0; physical < 4; ++physical) {
        for (unsigned axis = 0; axis < 6; ++axis) {
            int32_t value = (int32_t)(2000u + 3000u * physical + 100u * axis);
            /* Virtual joystick triggers span signed16; SDL's gamepad mapping normalizes them to 0..32767. */
            if (axis >= 4) value = value * 2 - 32768;
            CHECK(SDL_SetJoystickVirtualAxis(joysticks[physical], (int)axis, (Sint16)value));
        }
    }
    for (unsigned p = 0; p < 4; ++p) {
        read_port(p, handles[p]); unsigned physical = 3u - p;
        int16_t thumbs[4]; memcpy(thumbs, kernel_guest_at(OUT + p * 32u + 14u, 8), 8);
        for (unsigned a = 0; a < 4; ++a) {
            int16_t expected = (int16_t)(2000u + 3000u * physical + 100u * a);
            if ((a & 1u) != 0) expected = (int16_t)-expected;
            CHECK_EQ_U32((uint16_t)thumbs[a], (uint16_t)expected);
        }
        for (unsigned t = 0; t < 2; ++t) {
            unsigned value = 2000u + 3000u * physical + 100u * (t + 4u);
            CHECK_EQ_U32(analog(p, 6u+t), (value >> 7) < 32u ? 0u : (value >> 7));
        }
    }
    CHECK(SDL_SetJoystickVirtualButton(joysticks[3], SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true));
    CHECK(SDL_SetJoystickVirtualButton(joysticks[3], SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true));
    CHECK(SDL_SetJoystickVirtualAxis(joysticks[3], SDL_GAMEPAD_AXIS_LEFTY, -32768));
    CHECK(SDL_SetJoystickVirtualAxis(joysticks[3], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    read_port(0, handles[0]); CHECK_EQ_U32(analog(0, 4), 255); CHECK_EQ_U32(analog(0, 5), 255); CHECK_EQ_U32(analog(0, 6), 255);
    int16_t y; memcpy(&y, kernel_guest_at(OUT + 16u, 2), 2); CHECK_EQ_U32((uint16_t)y, 32767u);
    CHECK(controller_sdl_describe(c, text, sizeof(text)) != 0); CHECK(strstr(text, "T942 virtual controller") != NULL);
    /* Actual guest feedback goes through the backend to the virtual joystick rumble callback. */
    memset(kernel_guest_at(FEEDBACK, 0x46), 0, 0x46);
    uint16_t left = 0x1234, right = 0x5678;
    memcpy(kernel_guest_at(FEEDBACK + 0x42u, 2), &left, 2); memcpy(kernel_guest_at(FEEDBACK + 0x44u, 2), &right, 2);
    CHECK_EQ_U32(xinput_pad_feedback(handles[0], FEEDBACK), 997u); controller_sdl_pump(c);
    CHECK_EQ_U32(m[3].left, left); CHECK_EQ_U32(m[3].right, right);
    xinput_pad_close(handles[0]); controller_sdl_pump(c); CHECK_EQ_U32(m[3].left, 0); CHECK_EQ_U32(m[3].right, 0);
    handles[0] = xinput_pad_open(TYPE, 0, 0, 0); CHECK(handles[0] != 0);
    /* Removal clears state; replacement gets a fresh handle without reviving the old one. */
    SDL_CloseJoystick(joysticks[3]); CHECK(SDL_DetachVirtualJoystick(ids[3])); controller_sdl_pump(c);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 14u);
    CHECK_EQ_U32(xinput_pad_state_read(handles[0], OUT), 1167u);
    xinput_pad_state state = xinput_hle_pad_state(0); CHECK_EQ_U32(state.analog[4], 0); CHECK_EQ_U32((uint16_t)state.thumb_left_y, 0);
    ids[3] = attach("T942 virtual controller", 4, &m[3]); joysticks[3] = SDL_OpenJoystick(ids[3]); controller_sdl_pump(c);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 15u); CHECK_EQ_U32(xinput_pad_state_read(handles[0], OUT), 1167u);
    xinput_pad_close(handles[0]); handles[0] = xinput_pad_open(TYPE, 0, 0, 0); CHECK(handles[0] != 0);
    read_port(0, handles[0]); CHECK_EQ_U32(analog(0, 4), 0);
    /* Unsupported rumble is truthful, and disabled assignment removes only that guest port. */
    controller_selector disabled = {false, {0}, -1};
    CHECK(controller_sdl_assign(c, 2, &disabled)); CHECK_EQ_U32(xinput_devices_get(TYPE), 11u);
    read_port(1, handles[1]); read_port(3, handles[3]);
    atomic_store(&polling_stop, false); atomic_store(&polling_count, 0);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, polling_thread, NULL) == 0);
    while (atomic_load(&polling_count) == 0) sched_yield();
    controller_sdl_close(c); /* waits in-flight source callback, then uninstalls before free */
    atomic_store(&polling_stop, true); CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!xinput_source_poll_port(0));
    for (unsigned p = 0; p < 4; ++p) { SDL_CloseJoystick(joysticks[p]); CHECK(SDL_DetachVirtualJoystick(ids[p])); }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD); present_video_sink_close(sink); environment_end();
}
static void test_unknown_mapping(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); xinput_hle_init(); xinput_devices_reset(); xinput_source_reset();
    xinput_devices_set_fatal(catching_fatal); xinput_devices_enable_synthetic_pad(true); xinput_devices_enable_multiport(true);
    map_fixed(0x46C000u, 0x1000u); map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, sizeof(declarations));
    const char *error = NULL; present_video_sink *sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "T942 mappings", &error);
    CHECK(sink != NULL); if (!sink) return;
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_FLIGHT_STICK; desc.naxes = 6; desc.nbuttons = 15;
    desc.vendor_id = 0x1209; desc.product_id = 0x777; desc.name = "T942 unknown compatible joystick";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc); CHECK(id != 0); CHECK(!SDL_IsGamepad(id));
    SDL_Joystick *joy = SDL_OpenJoystick(id); CHECK(joy != NULL);
    controller_config config; controller_config_defaults(&config); char text[2048];
    controller_sdl *c = controller_sdl_open(sink, &config, NULL, text, sizeof(text)); CHECK(c != NULL);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0); CHECK_EQ_U32(xinput_devices_get(TYPE), 0);
    controller_sdl_describe(c, text, sizeof(text)); CHECK(strstr(text, "unmapped joystick") != NULL); CHECK(strstr(text, "local SDL mapping") != NULL);
    controller_sdl_close(c);
    CHECK(controller_sdl_open(sink, &config, "/no/such/t942-mappings.txt", text, sizeof(text)) == NULL);
    CHECK(strstr(text, "mapping file") != NULL);
    char path[] = "/tmp/t942-mapping-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0);
    FILE *f = fdopen(fd, "w"); CHECK(f != NULL);
    char guid[33]; SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    fprintf(f, "%s,T942 custom mapping,a:b2,b:b0,x:b3,y:b1,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:%s,\n", guid, SDL_GetPlatform());
    CHECK(fclose(f) == 0);
    c = controller_sdl_open(sink, &config, path, text, sizeof(text)); CHECK(c != NULL); CHECK(SDL_IsGamepad(id));
    controller_sdl_install(c); CHECK_EQ_U32(xinput_devices_get(TYPE), 1);
    uint32_t handle = xinput_pad_open(TYPE, 0, 0, 0); CHECK(handle != 0);
    CHECK(SDL_SetJoystickVirtualButton(joy, 2, true)); read_port(0, handle); CHECK_EQ_U32(analog(0, 0), 255); CHECK_EQ_U32(analog(0, 1), 0);
    CHECK(!xinput_feedback_send(0, 1, 1)); /* virtual descriptor deliberately has no rumble capability */
    controller_sdl_close(c);
    /* A subsequent local mapping overrides the same GUID, preserving SDL's documented replacement flow. */
    f = fopen(path, "w"); CHECK(f != NULL); fprintf(f, "%s,T942 override,a:b0,b:b2,platform:%s,\n", guid, SDL_GetPlatform()); CHECK(fclose(f) == 0);
    c = controller_sdl_open(sink, &config, path, text, sizeof(text)); CHECK(c != NULL); controller_sdl_install(c);
    xinput_pad_close(handle); handle = xinput_pad_open(TYPE, 0, 0, 0); CHECK(handle != 0);
    read_port(0, handle); CHECK_EQ_U32(analog(0, 0), 0); CHECK_EQ_U32(analog(0, 1), 255);
    controller_sdl_cancel(c); controller_sdl_close(c); CHECK(unlink(path) == 0);
    SDL_CloseJoystick(joy); CHECK(SDL_DetachVirtualJoystick(id)); SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    present_video_sink_close(sink); environment_end();
}

#endif
int main(void)
{
#ifndef TSFP_HAVE_SDL3
    puts("SKIP controller SDL: SDL3 not built; no virtual/physical claim"); return 77;
#else
    test_real_virtual_ports(); test_unknown_mapping(); printf("T942 SDL virtual controllers: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
#endif
}
