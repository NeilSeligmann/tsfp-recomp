/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "d3d8_hle.h"
#include "dsound_hle.h"
#include "xdk_registration_report.h"
#include "xinput_hle.h"
#include "xgrph_hle.h"
#include "xnet_hle.h"

static unsigned handler_calls;
static unsigned failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static uint32_t dummy(void *context)
{
    (void)context;
    handler_calls++;
    return 0u;
}

static void snapshot_json(const xdk_registration_row *rows, size_t count, const char *expected)
{
    FILE *out = tmpfile();
    CHECK(out != NULL);
    if (out == NULL) {
        return;
    }
    CHECK(xdk_registration_report_write(out, rows, count));
    rewind(out);
    char json[4096] = {0};
    const size_t length = fread(json, 1u, sizeof(json) - 1u, out);
    CHECK(length > 0u);
    CHECK(strstr(json, expected) != NULL);
    CHECK(strstr(json, "\"full_implementation_count\":null") != NULL);
    CHECK(strstr(json, "\"full_coverage\":\"unknown\"") != NULL);
    CHECK(strstr(json, "\"module\":\"D3D8\"") != NULL);
    CHECK(strstr(json, "\"module\":\"DSOUND\"") != NULL);
    CHECK(strstr(json, "\"module\":\"XGRPH\"") != NULL);
    CHECK(strstr(json, "\"module\":\"XAPI input\"") != NULL);
    CHECK(strstr(json, "\"section\":\"XNET\",\"module\":\"XNET\"") != NULL);
    fclose(out);
}

static void refuses_without_partial_output(const xdk_registration_row *rows, size_t count)
{
    FILE *out = tmpfile();
    CHECK(out != NULL);
    if (out == NULL) {
        return;
    }
    CHECK(!xdk_registration_report_write(out, rows, count));
    CHECK(ftell(out) == 0);
    fclose(out);
}

int main(void)
{
    dsound_hle_init();
    xinput_hle_init();
    const d3d8_surface_entry graphics[] = {{0x003D9000u, NULL, 1u}};
    CHECK(d3d8_hle_init(graphics, 1u));
    size_t audio_count = 0u;
    size_t input_count = 0u;
    const dsound_entry *audio = dsound_hle_table(&audio_count);
    const xinput_entry *input = xinput_hle_table(&input_count);
    CHECK(audio != NULL && audio_count > 0u);
    CHECK(input != NULL && input_count > 0u);
    if (audio == NULL || input == NULL) {
        return 1;
    }
    const xgrph_surface_entry utilities[] = {{0x003E66AEu, "XGSetCubeTextureHeader", 2u}};
    CHECK(xgrph_hle_init(utilities, 1u));
    const xnet_surface_entry network[] = {{0x01000000u, "ntohs", 1u}};
    CHECK(xnet_hle_init(network, 1u));
    xdk_registration_row surface[] = {
        {0x003D9000u, "D3D", NULL, 1u},
        {audio[0].address, "DSOUND", NULL, 1u},
        {input[0].address, "XPP", NULL, 1u},
        {0x01000000u, "XNET", NULL, 1u},
        {0x003E66AEu, "XGRPH", "XGSetCubeTextureHeader", 2u},
    };
    snapshot_json(surface, 5u, "\"registered_handler_count\":0");
    CHECK(d3d8_hle_register(surface[0].address, dummy));
    CHECK(dsound_hle_register(surface[1].address, dummy));
    CHECK(xinput_hle_register(surface[2].address, dummy));
    CHECK(xgrph_hle_register(surface[4].address, dummy));
    CHECK(xnet_hle_register(surface[3].address, dummy));
    snapshot_json(surface, 5u, "\"registered_handler_count\":5");
    CHECK(handler_calls == 0u);
    CHECK(d3d8_hle_entry(surface[0].address)->call_count == 0u);
    CHECK(dsound_hle_entry(surface[1].address)->call_count == 0u);
    CHECK(xinput_hle_entry(surface[2].address)->call_count == 0u);

    xdk_registration_row bad = surface[1];
    bad.section = "XNET";
    refuses_without_partial_output(&bad, 1u);
    bad = surface[0];
    bad.section = "DSOUND";
    refuses_without_partial_output(&bad, 1u);
    bad = surface[3];
    bad.section = "DSOUND";
    refuses_without_partial_output(&bad, 1u);
    bad.section = "UNKNOWN";
    refuses_without_partial_output(&bad, 1u);
    bad.address = 0u;
    bad.section = "XNET";
    refuses_without_partial_output(&bad, 1u);
    const xdk_registration_row duplicates[] = {surface[0], surface[0]};
    refuses_without_partial_output(duplicates, 2u);
    refuses_without_partial_output(NULL, 0u);
    bad = surface[4];
    bad.section = "D3D";
    refuses_without_partial_output(&bad, 1u);
    bad.section = "XNET";
    refuses_without_partial_output(&bad, 1u);
    bad = surface[3];
    bad.address = 0x01000010u; /* an XNET row absent from the adopted registry is refused */
    refuses_without_partial_output(&bad, 1u);
    bad = surface[0];
    bad.section = "XGRPH";
    refuses_without_partial_output(&bad, 1u);
    CHECK(xgrph_hle_entry(surface[4].address)->call_count == 0u);
    d3d8_hle_shutdown();
    xgrph_hle_shutdown();
    xnet_hle_shutdown();
    printf("xdk registration report: %u failure(s)\n", failures);
    return failures == 0u ? 0 : 1;
}
