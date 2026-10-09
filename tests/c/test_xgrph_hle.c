/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_hle.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
static unsigned failures, calls, logs;
static jmp_buf stopped;
#define CHECK(value) do { if (!(value)) { failures++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
static int capture(const char *format, ...) { (void)format; logs++; return 0; }
static uint32_t handler(void *context) { calls++; return *(uint32_t *)context; }
static void stop(uint32_t address, const char *message)
{
    CHECK(address == 0x3E66AEu);
    CHECK(strcmp(message, "bounded path") == 0);
    longjmp(stopped, 1);
}
int main(void)
{
    const xgrph_xdk_row measured[] = {
        {0x3E66AEu, "XGRPH", "XGSetCubeTextureHeader", 2u},
        {0x3D9000u, "D3D", NULL, 1u},
        {0x3E6684u, "XGRPH", "XGSetTextureHeader", 5u},
        {0x11111u, NULL, NULL, 1u},
    };
    xgrph_hle_set_log(capture);
    CHECK(xgrph_hle_adopt(measured, 4u));
    CHECK(xgrph_hle_count() == 2u);
    CHECK(xgrph_hle_implemented_count() == 0u);
    CHECK(xgrph_hle_entry(0x3D9000u) == NULL);
    CHECK(xgrph_hle_entry(0x3E66ADu) == NULL);
    CHECK(xgrph_hle_entry(0x3E66AEu)->sites == 2u);
    CHECK(!xgrph_hle_register(0x3E66AFu, handler));
    CHECK(!xgrph_hle_register(0x3E66AEu, NULL));
    CHECK(!xgrph_hle_set_default_return(0x3E66AFu, 1u));
    CHECK(xgrph_hle_set_default_return(0x3E66AEu, 123u));
    CHECK(xgrph_hle_call(0x3E66AEu, NULL) == 123u);
    CHECK(xgrph_hle_call(0x3E66AEu, NULL) == 123u);
    CHECK(logs == 1u);
    CHECK(xgrph_hle_entry(0x3E66AEu)->call_count == 2u);
    CHECK(xgrph_hle_register(0x3E66AEu, handler));
    uint32_t result = 0xABCDEu;
    CHECK(xgrph_hle_call(0x3E66AEu, &result) == result);
    CHECK(calls == 1u && xgrph_hle_implemented_count() == 1u);
    CHECK(xgrph_hle_call(0x3E66AFu, NULL) == 0u);
    CHECK(xgrph_hle_call(0x3E66AFu, NULL) == 0u);
    CHECK(xgrph_hle_unknown_count() == 2u && logs == 3u);
    const xgrph_surface_entry duplicates[] = {{1u, NULL, 1u}, {1u, NULL, 2u}};
    const xgrph_surface_entry zero[] = {{0u, NULL, 1u}};
    CHECK(!xgrph_hle_init(duplicates, 2u));
    CHECK(!xgrph_hle_init(zero, 1u));
    CHECK(!xgrph_hle_init(NULL, 1u));
    CHECK(!xgrph_hle_adopt(measured + 1u, 1u));
    CHECK(xgrph_hle_count() == 2u && xgrph_hle_implemented_count() == 1u);
    xgrph_hle_set_fatal(stop);
    if (setjmp(stopped) == 0) xgrph_hle_fatal(0x3E66AEu, "%s", "bounded path");
    CHECK(xgrph_hle_adopt(measured, 4u));
    CHECK(xgrph_hle_implemented_count() == 0u && xgrph_hle_unknown_count() == 0u);
    xgrph_hle_shutdown();
    xgrph_hle_shutdown();
    CHECK(xgrph_hle_count() == 0u && xgrph_hle_entry(0x3E66AEu) == NULL);
    xgrph_hle_set_fatal(NULL);
    printf("xgrph registry: %u failure(s)\n", failures);
    return failures == 0u ? 0 : 1;
}
