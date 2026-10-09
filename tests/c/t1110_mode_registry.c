/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1110 independent finite registry/policy witness. No generated/retail inputs. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_hle.h"
#include "dsound_device.h"
#include "dsound_hrtf.h"
#include "dsound_buffer.h"
#include "dsound_stream.h"
#include "dsound_listener.h"
#include "dsound_effects_binding.h"
#include "dsound_completion.h"
#include "xinput_hle.h"
#include "xinput_devices.h"
#include "mu_device.h"
#include "xonline_hle.h"
#include "xonline_offline.h"
#include "xdk_registration_report.h"
#include "host_options.h"

static unsigned failures, checks;
static jmp_buf refusal;
static uint32_t refused_address;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); } } while (0)
static int quiet(const char *format, ...) { (void)format; return 0; }
static void fatal(uint32_t address, const char *message)
{ (void)message; refused_address = address; longjmp(refusal, 1); }
static void rejects(const xdk_registration_row *rows, size_t count)
{
    FILE *out = tmpfile();
    CHECK(out != NULL);
    if (out == NULL) return;
    CHECK(!xdk_registration_report_write(out, rows, count));
    CHECK(ftell(out) == 0L);
    fclose(out);
}
static const uint32_t online_addresses[] = {
    0x412E50u,0x412E5Bu,0x412F9Eu,0x412FA9u,0x412FBFu,0x412FCEu,0x412FD9u,
    0x412FE4u,0x412FEFu,0x412FFAu,0x413010u,0x413034u,0x41303Fu,0x41306Cu,
    0x413077u,0x413082u,0x41308Du,0x413098u,0x4130A3u,0x4130AEu,0x4130CFu,
    0x4130F0u,0x4130FBu,0x413106u,0x413111u,0x413120u,
    0x413496u,0x412FB4u,0x413593u,0x413005u
};
static const uint32_t movie_addresses[] = {
    0x444A2Du,0x444F71u,0x445055u,0x4450C2u,0x445241u,0x445252u,0x44525Du
};
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const bool pad = strcmp(argv[1], "pad") == 0 || strcmp(argv[1], "all") == 0;
    const bool audio = strcmp(argv[1], "audio") == 0 || strcmp(argv[1], "all") == 0;
    const bool offline = strcmp(argv[1], "offline") == 0 || strcmp(argv[1], "all") == 0 || strcmp(argv[1], "report") == 0;
    const bool movie = strcmp(argv[1], "all") == 0 || strcmp(argv[1], "report") == 0;
    dsound_hle_init(); dsound_hle_set_log(quiet);
    dsound_buffer_set_enabled(audio); dsound_stream_set_enabled(audio);
    dsound_listener_set_enabled(audio); dsound_effects_binding_set_enabled(audio);
    dsound_completion_set_enabled(audio);
    dsound_buffer_set_completion(audio, dsound_completion_buffer_started);
    (void)dsound_device_register(); (void)dsound_hrtf_register();
    (void)dsound_effects_binding_register(); (void)dsound_stream_register();
    (void)dsound_buffer_register(); (void)dsound_listener_register();
    (void)dsound_completion_register();
    if (movie) (void)dsound_device_register_movie();
    xinput_hle_init(); xinput_hle_set_log(quiet); xinput_devices_reset();
    (void)xinput_devices_register(); (void)mu_register();
    if (pad) CHECK(xinput_devices_register_pad() == 5u);
    xonline_surface_entry online[30];
    for (size_t i = 0u; i < 30u; i++) online[i] = (xonline_surface_entry){online_addresses[i], "finite online row", 0u};
    CHECK(xonline_hle_init(online, 30u)); xonline_hle_set_log(quiet);
    xonline_offline_reset();
    if (offline) {
        CHECK(xonline_offline_register() == 30u);
        fprintf(stderr, "T1110 host-offline-guard=%d\n", t1110_host_guard(xonline_hle_implemented_count()));
    }

    /* The registration snapshot must not invoke any handler. */
    xdk_registration_row rows[128]; size_t count = 0u, n = 0u;
    const dsound_entry *sound = dsound_hle_table(&n);
    for (size_t i = 0u; i < n; i++) rows[count++] = (xdk_registration_row){sound[i].address, "DSOUND", sound[i].name, sound[i].sites};
    const xinput_entry *input = xinput_hle_table(&n);
    for (size_t i = 0u; i < n; i++) rows[count++] = (xdk_registration_row){input[i].address, "XPP", input[i].name, input[i].sites};
    for (size_t i = 0u; i < 30u; i++) rows[count++] = (xdk_registration_row){online_addresses[i], "XONLINE", NULL, 0u};
    for (size_t i = 0u; i < 7u; i++) rows[count++] = (xdk_registration_row){movie_addresses[i], "XMV", NULL, 0u};
    CHECK(count <= 128u);
    CHECK(xdk_registration_report_write(stdout, rows, count));
    sound = dsound_hle_table(&n);
    for (size_t i = 0u; i < n; i++) CHECK(sound[i].call_count == 0u);
    input = xinput_hle_table(&n);
    for (size_t i = 0u; i < n; i++) CHECK(input[i].call_count == 0u);
    for (size_t i = 0u; i < 30u; i++) CHECK(xonline_hle_entry(online_addresses[i])->call_count == 0u);
    CHECK(xonline_offline_stats_get().references == 0u);
    CHECK(xonline_offline_stats_get().service_calls == 0u);
    const xdk_registration_row duplicate[] = {rows[0], rows[0]};
    rejects(duplicate, 2u);
    xdk_registration_row wrong = {0x412FBFu, "DSOUND", NULL, 0u}; rejects(&wrong, 1u);
    wrong.section = "unknown"; rejects(&wrong, 1u);
    wrong = (xdk_registration_row){0x413999u, "XONLINE", NULL, 0u}; rejects(&wrong, 1u);
    /* An actual registered wrapper supports no state and refuses the initialized service. */
    if (offline) {
        for (size_t i = 0u; i < 26u; i++) {
            const uint32_t expected = online_addresses[i] == 0x412FD9u || online_addresses[i] == 0x41308Du ? 0u : XONLINE_E_NOT_INITIALIZED;
            CHECK(xonline_hle_call(online_addresses[i], NULL) == expected);
        }
        uint32_t result = 99u;
        CHECK(xonline_offline_startup(0u, &result) && result == 0u);
        xonline_hle_set_fatal(fatal);
        for (size_t i = 0u; i < 26u; i++) {
            if (setjmp(refusal) == 0) { (void)xonline_hle_call(online_addresses[i], NULL); CHECK(false); }
            else CHECK(refused_address == online_addresses[i]);
        }
        if (setjmp(refusal) == 0) { (void)xonline_hle_call(0x413005u, NULL); CHECK(false); }
        else CHECK(refused_address == 0x413005u);
        CHECK(xonline_hle_entry(0x412FBFu)->handler != NULL);
        CHECK(xonline_offline_stats_get().references == 1u);
        CHECK(xonline_offline_cleanup() == 0u);
        CHECK(xonline_hle_call(0x412FBFu, NULL) == XONLINE_E_NOT_INITIALIZED);
    }
    /* Registered audio factory is still refused without its opt-in CPU model. */
    if (!audio) {
        CHECK(dsound_hle_entry(0x4093C8u)->handler != NULL);
        dsound_buffer_set_fatal(fatal);
        if (setjmp(refusal) == 0) { (void)dsound_hle_call(0x4093C8u, NULL); CHECK(false); }
        else CHECK(refused_address == 0x4093C8u);
    }
    options opts;
    char *plain[] = {"host", "image.xbe"};
    CHECK(parse_options(2, plain, &opts));
    CHECK(!opts.native_xmv && !opts.xonline_offline && !opts.controllers && !opts.synthetic_pad);
    CHECK(!opts.headless_buffers && !opts.headless_streams && !opts.passive_audio_completion);
    char *disc[] = {"host", "--disc", "disc.iso", "image.xbe"};
    CHECK(parse_options(4, disc, &opts) && opts.native_xmv && !opts.native_xmv_explicit);
    char *optout[] = {"host", "--disc", "disc.iso", "--no-native-xmv", "image.xbe"};
    CHECK(parse_options(5, optout, &opts) && !opts.native_xmv);
    char *bad[] = {"host", "--native-xmv", "image.xbe"};
    CHECK(!parse_options(3, bad, &opts));
    xonline_offline_reset(); xonline_hle_shutdown();
    fprintf(stderr, "T1110 %s: %u checks, %u failures\n", argv[1], checks, failures);
    return failures == 0u ? 0 : 1;
}
