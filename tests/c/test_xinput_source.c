/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T707: host controller input sources for the FABRICATED synthetic pad (scripted per-frame states and the source
 * seam), exercised through the pad adapter's XInputGetState so the bytes the title would read are checked. */
#define _DEFAULT_SOURCE 1
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_source.h"
#include <fcntl.h>
#include <unistd.h>
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define HANDLE 0x58504430u
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
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
static void finish(void)
{
    xinput_source_reset();
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static xinput_script *parse(const char *text, char *error)
{
    return xinput_script_parse(text, strlen(text), error, 160u);
}
static void expect_parse_error(const char *text, const char *needle)
{
    char error[160];
    xinput_script *script = parse(text, error);
    CHECK(script == NULL);
    CHECK(strstr(error, needle) != NULL);
    if (strstr(error, needle) == NULL) printf("  error was '%s', wanted '%s'\n", error, needle);
    xinput_script_free(script);
}
static void test_parse_tokens_and_runs(void)
{
    char error[160];
    xinput_script *script = parse(
        "# comment\n\n2\n3 UP START # trailing\n1 a LT=200 rt=31 LX=-32768 ly=32767 RX=-1 RY=5\n4 DOWN LEFT RIGHT BACK "
        "LTHUMB RTHUMB B X Y BLACK WHITE\n", error);
    CHECK(script != NULL);
    if (script == NULL) return;
    CHECK_EQ_U32((uint32_t)xinput_script_entry_count(script), 4u);
    CHECK_EQ_U32((uint32_t)xinput_script_total_frames(script), 10u);
    xinput_pad_state s = xinput_script_state_at(script, 0u);
    CHECK_EQ_U32(s.digital_buttons, 0u);
    CHECK_EQ_U32(s.analog[0], 0u);
    s = xinput_script_state_at(script, 2u);
    CHECK_EQ_U32(s.digital_buttons, XINPUT_BUTTON_DPAD_UP | XINPUT_BUTTON_START);
    CHECK_EQ_U32(s.analog[0], 0u);
    s = xinput_script_state_at(script, 5u);
    CHECK_EQ_U32(s.digital_buttons, 0u);
    CHECK_EQ_U32(s.analog[0], 255u); CHECK_EQ_U32(s.analog[1], 0u);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_LEFT_TRIGGER], 200u);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_RIGHT_TRIGGER], 31u);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumb_left_x, (uint32_t)-32768);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumb_left_y, 32767u);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumb_right_x, (uint32_t)-1);
    CHECK_EQ_U32((uint32_t)(int32_t)s.thumb_right_y, 5u);
    s = xinput_script_state_at(script, 9u);
    CHECK_EQ_U32(s.digital_buttons, XINPUT_BUTTON_DPAD_DOWN | XINPUT_BUTTON_DPAD_LEFT | XINPUT_BUTTON_DPAD_RIGHT |
        XINPUT_BUTTON_BACK | XINPUT_BUTTON_LEFT_THUMB | XINPUT_BUTTON_RIGHT_THUMB);
    for (unsigned i = 1u; i < 6u; i++) CHECK_EQ_U32(s.analog[i], 255u);
    CHECK_EQ_U32(s.analog[0], 0u);
    CHECK_EQ_U32(s.analog[XINPUT_ANALOG_LEFT_TRIGGER], 0u);
    /* Run boundaries: the last frame of one run and the first of the next differ. */
    CHECK_EQ_U32(xinput_script_state_at(script, 1u).digital_buttons, 0u);
    CHECK_EQ_U32(xinput_script_state_at(script, 5u).analog[0], 255u);
    CHECK_EQ_U32(xinput_script_state_at(script, 4u).digital_buttons, XINPUT_BUTTON_DPAD_UP | XINPUT_BUTTON_START);
    CHECK_EQ_U32(xinput_script_state_at(script, 6u).digital_buttons, XINPUT_BUTTON_DPAD_DOWN | XINPUT_BUTTON_DPAD_LEFT | XINPUT_BUTTON_DPAD_RIGHT | XINPUT_BUTTON_BACK | XINPUT_BUTTON_LEFT_THUMB | XINPUT_BUTTON_RIGHT_THUMB);
    /* After the end: rest, never a held last state. */
    s = xinput_script_state_at(script, 10u);
    CHECK_EQ_U32(s.digital_buttons, 0u); CHECK_EQ_U32(s.analog[1], 0u);
    s = xinput_script_state_at(script, 1000000u);
    CHECK_EQ_U32(s.digital_buttons, 0u);
    xinput_script_free(script);
    script = parse("# nothing\n", error);
    CHECK(script != NULL);
    CHECK_EQ_U32((uint32_t)xinput_script_entry_count(script), 0u);
    xinput_script_free(script);
}
static void test_parse_refusals(void)
{
    expect_parse_error("0 UP\n", "line 1: frame count");
    expect_parse_error("UP\n", "line 1: frame count");
    expect_parse_error("1000001\n", "frame count");
    expect_parse_error("1\n1 UPP\n", "line 2: unknown token");
    expect_parse_error("1 LT=256\n", "analog value");
    expect_parse_error("1 LT=-1\n", "analog value");
    expect_parse_error("1 LX=32768\n", "axis value");
    expect_parse_error("1 LX=-32769\n", "axis value");
    expect_parse_error("1 LX=\n", "axis value");
    expect_parse_error("1 UP=1\n", "unknown token");
    expect_parse_error("1 LX\n", "unknown token");
    expect_parse_error("1 LT\n", "unknown token");
    expect_parse_error("1 RT\n", "unknown token");
    expect_parse_error("1 LT=12x\n", "analog value");
    char error[160];
    CHECK(xinput_script_load("tmp/definitely-missing-pad-script.txt", error, sizeof(error)) == NULL);
    CHECK(strstr(error, "cannot open") != NULL);
}
static void read_state(uint32_t handle, uint32_t *packet, uint16_t *buttons, uint8_t analog[8], int16_t thumbs[4])
{
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 22u);
    memcpy(packet, at, 4u); memcpy(buttons, at + 4u, 2u); memcpy(analog, at + 6u, 8u); memcpy(thumbs, at + 14u, 8u);
}
static void test_script_reaches_the_guest_bytes(void)
{
    initialise();
    char error[160];
    xinput_script *script = parse("2\n1 START A LT=255 LX=-5 RY=300\n2 UP\n", error);
    CHECK(script != NULL);
    xinput_script_install(script);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 1u); CHECK_EQ_U32(buttons, 0u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 1u); CHECK_EQ_U32(buttons, 0u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 2u); CHECK_EQ_U32(buttons, XINPUT_BUTTON_START);
    CHECK_EQ_U32(analog[0], 255u); CHECK_EQ_U32(analog[XINPUT_ANALOG_LEFT_TRIGGER], 255u);
    CHECK_EQ_U32(analog[1], 0u);
    CHECK_EQ_U32((uint32_t)(int32_t)thumbs[0], (uint32_t)-5); CHECK_EQ_U32((uint32_t)(int32_t)thumbs[3], 300u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 3u); CHECK_EQ_U32(buttons, XINPUT_BUTTON_DPAD_UP);
    CHECK_EQ_U32(analog[0], 0u); CHECK_EQ_U32((uint32_t)(int32_t)thumbs[0], 0u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 3u); CHECK_EQ_U32(buttons, XINPUT_BUTTON_DPAD_UP);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 4u); CHECK_EQ_U32(buttons, 0u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 4u); CHECK_EQ_U32(buttons, 0u);
    CHECK_EQ_U32((uint32_t)xinput_source_poll_count(), 7u);
    CHECK(xinput_hle_synthetic_value_count() >= 7u);
    xinput_script_free(script);
    finish();
}
static void test_analog_below_threshold_is_not_scripted_away(void)
{
    initialise();
    char error[160];
    xinput_script *script = parse("1 A=31 B=32 RT=255\n", error);
    CHECK(script != NULL);
    xinput_script_install(script);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    /* The title sees what the adapter measured: pressure below 0x20 reads 0, the host state keeps 31. */
    CHECK_EQ_U32(analog[0], 0u); CHECK_EQ_U32(analog[1], 32u); CHECK_EQ_U32(analog[7], 255u);
    CHECK_EQ_U32(xinput_hle_pad_state(0u).analog[0], 31u);
    xinput_script_free(script);
    finish();
}
static bool counting_source(uint64_t index, xinput_pad_state *out, void *user)
{
    uint64_t *calls = user;
    (*calls)++;
    out->digital_buttons = (uint16_t)(index + 1u);
    return index != 1u;
}
static void test_custom_source_seam_and_declines(void)
{
    initialise();
    uint64_t calls = 0u;
    xinput_source_install(counting_source, &calls);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 1u);
    /* A source returning false leaves the previous state alone. */
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 1u); CHECK_EQ_U32(packet, 2u);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 3u); CHECK_EQ_U32(packet, 3u);
    CHECK_EQ_U32((uint32_t)calls, 3u);
    xinput_source_install(NULL, NULL);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 3u); CHECK_EQ_U32((uint32_t)calls, 3u);
    finish();
}
static void test_no_source_is_the_default_rest_pad(void)
{
    initialise();
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(packet, 1u); CHECK_EQ_U32(buttons, 0u);
    CHECK_EQ_U32(xinput_hle_synthetic_value_count(), 0u);
    CHECK(!xinput_source_poll());
    finish();
}
static void test_removed_pad_is_not_polled(void)
{
    initialise();
    char error[160];
    xinput_script *script = parse("5 UP\n", error);
    CHECK(script != NULL);
    xinput_script_install(script);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(xinput_source_poll_count(), 1u);
    xinput_pad_remove();
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE, OUT), 0x48Fu);
    CHECK_EQ_U32(xinput_source_poll_count(), 1u);
    xinput_script_free(script);
    xinput_source_reset();
    finish();
}
static void test_source_refused_without_synthetic_pad(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_source_reset();
    xinput_script *script = parse("1 UP\n", (char[160]){0});
    CHECK(script != NULL);
    xinput_script_install(script);
    CHECK(!xinput_source_poll());
    CHECK_EQ_U32(xinput_hle_synthetic_value_count(), 0u);
    CHECK_EQ_U32(xinput_hle_pad_state(0u).digital_buttons, 0u);
    xinput_script_free(script);
    xinput_source_reset();
    environment_end();
}
static void write_live(int fd, const char *text)
{
    char padded[128];
    memset(padded, ' ', sizeof(padded));
    memcpy(padded, text, strlen(text));
    padded[sizeof(padded) - 1u] = '\n';
    CHECK(pwrite(fd, padded, sizeof(padded), 0) == (ssize_t)sizeof(padded));
}
static void test_live_pad_follows_the_file_and_refuses_garbage(void)
{
    initialise();
    char path[] = "/tmp/t1222-live-pad-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    char error[160];
    xinput_live *live = xinput_live_open(path, error, sizeof(error));
    CHECK(live != NULL);
    if (live == NULL) { close(fd); unlink(path); finish(); return; }
    xinput_live_install(live);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    /* An empty file is the pad at rest. */
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 0u); CHECK_EQ_U32((uint32_t)(int32_t)thumbs[2], 0u); CHECK_EQ_U32(analog[XINPUT_ANALOG_RIGHT_TRIGGER], 0u);
    write_live(fd, "RX=-9000 RY=1234 RT=255 START # comment");
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, XINPUT_BUTTON_START);
    CHECK_EQ_U32((uint32_t)(int32_t)thumbs[2], (uint32_t)-9000); CHECK_EQ_U32((uint32_t)(int32_t)thumbs[3], 1234u);
    CHECK_EQ_U32(analog[XINPUT_ANALOG_RIGHT_TRIGGER], 255u);
    /* The next line replaces the state, nothing is latched from the line before. */
    write_live(fd, "LY=32767");
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(buttons, 0u); CHECK_EQ_U32((uint32_t)(int32_t)thumbs[1], 32767u);
    CHECK_EQ_U32((uint32_t)(int32_t)thumbs[2], 0u); CHECK_EQ_U32(analog[XINPUT_ANALOG_RIGHT_TRIGGER], 0u);
    /* A line that does not parse keeps the previous state and is counted, never half applied. */
    write_live(fd, "LY=-5 BOGUS");
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32((uint32_t)(int32_t)thumbs[1], 32767u);
    CHECK_EQ_U32((uint32_t)xinput_live_refused(live), 1u);
    write_live(fd, "LX=70000");
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32((uint32_t)(int32_t)thumbs[0], 0u);
    CHECK_EQ_U32((uint32_t)xinput_live_refused(live), 2u);
    xinput_pad_state parsed;
    CHECK(xinput_live_parse_line("A B # x\n9999", 14u, &parsed));
    CHECK_EQ_U32(parsed.analog[0], 255u); CHECK_EQ_U32(parsed.analog[1], 255u);
    CHECK(!xinput_live_parse_line("1 UP", 4u, &parsed));
    CHECK(xinput_live_open("tmp/definitely-missing-live-pad.txt", error, sizeof(error)) == NULL);
    xinput_source_install(NULL, NULL);
    xinput_live_free(live);
    close(fd);
    unlink(path);
    finish();
}
/* T1629: the pre install hook sees the state of every PORT 0 poll the source supplied, in order, BEFORE it is installed, and
 * is off (NULL, reset) at zero cost. The hook itself never calls back into the device layer. */
static unsigned hook_calls;
static uint64_t hook_index[16];
static uint16_t hook_buttons[16];
static uint8_t hook_analog_b[16];
static uint16_t hook_installed_buttons[16];
static unsigned hook_user_seen;
static void pre_hook(uint64_t index, const xinput_pad_state *state, void *user)
{
    if (user == &hook_user_seen) hook_user_seen++;
    if (hook_calls < 16u) {
        hook_index[hook_calls] = index;
        hook_buttons[hook_calls] = state->digital_buttons;
        hook_analog_b[hook_calls] = state->analog[1];
        hook_installed_buttons[hook_calls] = xinput_hle_pad_state(0u).digital_buttons;
    }
    hook_calls++;
}
static bool declining_source(uint64_t index, xinput_pad_state *out, void *user)
{
    (void)user;
    if (index == 1u) return false;
    memset(out, 0, sizeof(*out));
    out->digital_buttons = (uint16_t)(index == 0u ? XINPUT_BUTTON_START : XINPUT_BUTTON_BACK);
    out->analog[1] = (uint8_t)(10u * index);
    return true;
}
static bool port_one_source(uint64_t index, unsigned port, xinput_pad_state *out, void *user)
{
    (void)index; (void)user;
    if (port != 1u) return false;
    memset(out, 0, sizeof(*out));
    out->digital_buttons = XINPUT_BUTTON_DPAD_UP;
    return true;
}
static void test_pre_install_hook(void)
{
    initialise();
    hook_calls = 0u;
    memset(hook_index, 0xFF, sizeof(hook_index));
    char error[160];
    xinput_script *script = parse("2 START\n1 B\n", error);
    xinput_script *earlier = parse("1 BACK\n", error);
    CHECK(script != NULL && earlier != NULL);
    xinput_script_install(earlier);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    /* no hook: nothing is called, and the pad now holds BACK */
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(hook_calls, 0u);
    CHECK_EQ_U32(buttons, XINPUT_BUTTON_BACK);
    xinput_script_free(earlier);
    xinput_source_reset();
    xinput_script_install(script);
    hook_user_seen = 0u;
    xinput_source_set_pre_install_hook(pre_hook, &hook_user_seen);
    for (unsigned poll = 0u; poll < 3u; poll++) read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(hook_calls, 3u);
    CHECK_EQ_U32(hook_user_seen, 3u); /* the user pointer comes back */
    CHECK_EQ_U32((uint32_t)hook_index[0], 0u); CHECK_EQ_U32((uint32_t)hook_index[1], 1u); CHECK_EQ_U32((uint32_t)hook_index[2], 2u);
    /* the hook saw the NEW state: START, START, then B pressed */
    CHECK_EQ_U32(hook_buttons[0], XINPUT_BUTTON_START); CHECK_EQ_U32(hook_buttons[1], XINPUT_BUTTON_START);
    CHECK_EQ_U32(hook_buttons[2], 0u); CHECK_EQ_U32(hook_analog_b[2], 255u); CHECK_EQ_U32(hook_analog_b[1], 0u);
    /* ... while the installed state was still the previous poll's: BACK at poll 0 (START not installed yet), START at poll 1 and 2 */
    CHECK_EQ_U32(hook_installed_buttons[0], XINPUT_BUTTON_BACK); CHECK_EQ_U32(hook_installed_buttons[1], XINPUT_BUTTON_START);
    CHECK_EQ_U32(hook_installed_buttons[2], XINPUT_BUTTON_START);
    CHECK_EQ_U32(buttons, 0u); /* after poll 2 the guest reads the B state */
    CHECK_EQ_U32(analog[1], 255u);
    xinput_script_free(script);
    /* a source that declines supplies no state: the hook is not called for that poll, the index still counts */
    xinput_source_reset();
    hook_calls = 0u;
    xinput_source_install(declining_source, NULL);
    xinput_source_set_pre_install_hook(pre_hook, NULL);
    for (unsigned poll = 0u; poll < 3u; poll++) read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(hook_calls, 2u);
    CHECK_EQ_U32((uint32_t)hook_index[0], 0u); CHECK_EQ_U32((uint32_t)hook_index[1], 2u);
    CHECK_EQ_U32(hook_buttons[1], XINPUT_BUTTON_BACK); CHECK_EQ_U32(hook_analog_b[1], 20u);
    /* removing it stops the calls */
    xinput_source_set_pre_install_hook(NULL, NULL);
    read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32(hook_calls, 2u);
    /* port 0 only: a poll of another port never reaches it */
    xinput_source_reset();
    hook_calls = 0u;
    xinput_source_install_ports(port_one_source, NULL);
    xinput_source_set_pre_install_hook(pre_hook, NULL);
    (void)xinput_source_poll_port(1u);
    (void)xinput_source_poll_port(1u);
    CHECK_EQ_U32(hook_calls, 0u);
    CHECK_EQ_U32((uint32_t)xinput_source_port_poll_count(1u), 2u);
    xinput_source_install_ports(NULL, NULL);
    /* reset clears the hook with the sources */
    xinput_source_install(declining_source, NULL);
    (void)xinput_source_poll_port(0u);
    CHECK_EQ_U32(hook_calls, 1u);
    xinput_source_reset();
    xinput_source_install(declining_source, NULL);
    (void)xinput_source_poll_port(0u);
    CHECK_EQ_U32(hook_calls, 1u);
    finish();
}

int main(void)
{
    test_parse_tokens_and_runs();
    test_parse_refusals();
    test_script_reaches_the_guest_bytes();
    test_analog_below_threshold_is_not_scripted_away();
    test_custom_source_seam_and_declines();
    test_no_source_is_the_default_rest_pad();
    test_live_pad_follows_the_file_and_refuses_garbage();
    test_removed_pad_is_not_polled();
    test_source_refused_without_synthetic_pad();
    test_pre_install_hook();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
