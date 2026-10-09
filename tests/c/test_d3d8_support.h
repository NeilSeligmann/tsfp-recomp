/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared scaffolding for the D3D8 init suites: the check macros, a guest address space with the
 * D3D8 section's BSS and tables at their REAL addresses, the kernel AV module configured the way
 * the host configures it, a fatal catcher and a dispatcher call.
 *
 * WHY REAL ADDRESSES. D3D8's state is a static in a statically linked library, so the title and
 * the handlers meet at fixed guest addresses (see src/gpu/d3d8_guest.h). A suite that mapped the
 * device somewhere convenient would pass whatever the addresses were. Everything here is
 * therefore mapped where the retail image has it, and every expected number in the suites is a
 * literal.
 *
 * WHAT IS SEEDED, AND WHERE IT CAME FROM. Four small tables live in D3D8's initialised data and
 * are read by the code under test: the legal pitches (0x003E17C0), the per-format info bytes
 * (0x003E1828), the display-mode rows (0x003E2000) and two floats in the title's .rdata. The
 * suites seed only the entries they use. The mode rows are SYNTHETIC apart from the first, which
 * is a row of the real table (flags 0x02480104, 640x480, mode word 0x88110f01); inventing the
 * rest is the point, because each exercises one branch of a filter whose behaviour was
 * established separately against the original code (tools/d3dscan/oracle.py).
 */

#ifndef TSFP_TESTS_D3D8_SUPPORT_H
#define TSFP_TESTS_D3D8_SUPPORT_H

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_device.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_resource.h"
#include "guest_mem.h"
#include "kernel_av.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_register_all.h"
#include "nt_status.h"

/* Each suite uses a different subset of these helpers, and -Werror makes an unused static an
 * error, so they are all marked. */
#define TEST_UNUSED __attribute__((unused))

static int failures;
static int checks;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                 \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                    \
    do {                                                                                  \
        checks++;                                                                         \
        const uint32_t actual_ = (uint32_t)(actual);                                      \
        const uint32_t expected_ = (uint32_t)(expected);                                  \
        if (actual_ != expected_) {                                                       \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)actual_, (unsigned)expected_);                               \
        }                                                                                 \
    } while (0)

/* --- guest memory at the real D3D8 addresses -------------------------------------------- */

#define D3D_REGION_BASE 0x003D0000u
#define D3D_REGION_BYTES 0x00030000u
#define RDATA_REGION_BASE 0x00470000u
#define RDATA_REGION_BYTES 0x00010000u

#define GUEST_PITCH_TABLE 0x003E17C0u
#define GUEST_FORMAT_INFO 0x003E1828u
#define GUEST_MODE_TABLE 0x003E2000u
#define GUEST_FLOAT_TWO_POW_32 0x00475CCCu
#define GUEST_FLOAT_QUARTER 0x00475D4Cu

static TEST_UNUSED void map_fixed(uint32_t base, uint32_t bytes)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.fixed_base = base;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr got = guest_region_alloc(&request, &status);
    if (got != base) {
        printf("FATAL could not map guest region %#x (status %#x)\n", (unsigned)base,
               (unsigned)status);
        exit(EXIT_FAILURE);
    }
}

static TEST_UNUSED void store(uint32_t address, uint32_t value)
{
    if (!kernel_guest_write_u32(address, value)) {
        printf("FATAL test store to %#x failed\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
}

static TEST_UNUSED uint32_t load(uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_guest_read_u32(address, &value)) {
        printf("FATAL test load from %#x failed\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
    return value;
}

static TEST_UNUSED void store_byte(uint32_t address, uint8_t value)
{
    if (!kernel_guest_write_u8(address, value)) {
        printf("FATAL test byte store to %#x failed\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
}

/* The 26 legal pitches of 0x003E17C0, from the retail image. */
static TEST_UNUSED void seed_pitch_table(void)
{
    static const uint32_t pitches[26] = {
        0x200,  0x300,  0x400,  0x500,  0x600,  0x700,  0x800,  0xA00,  0xC00,
        0xE00,  0x1000, 0x1400, 0x1800, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3800,
        0x4000, 0x5000, 0x6000, 0x7000, 0x8000, 0xA000, 0xC000, 0xE000};
    for (uint32_t index = 0u; index < 26u; index++) {
        store(GUEST_PITCH_TABLE + index * 4u, pitches[index]);
    }
}

/* The per-format info bytes the suites use, from 0x003E1828 (bit 0 swizzled, 0x3C bits per
 * pixel): 0x01 0x09, 0x03 0x91, 0x0C 0x04, 0x0E and 0x0F 0x08, 0x10 0x12, 0x11 0x92, 0x12
 * 0xA2, 0x1C 0x92, 0x1E 0xA2, 0x2E and 0x2F 0x62, 0x30 and 0x31 0x52. */
static TEST_UNUSED void seed_format_table(void)
{
    static const struct {
        uint32_t format;
        uint8_t info;
    } entries[] = {{0x01u, 0x09u}, {0x03u, 0x91u}, {0x0Cu, 0x04u}, {0x0Eu, 0x08u},
                   {0x0Fu, 0x08u}, {0x10u, 0x12u}, {0x11u, 0x92u}, {0x12u, 0xA2u},
                   {0x1Cu, 0x92u}, {0x1Eu, 0xA2u}, {0x2Eu, 0x62u}, {0x2Fu, 0x62u},
                   {0x30u, 0x52u}, {0x31u, 0x52u}};
    for (size_t index = 0u; index < sizeof(entries) / sizeof(entries[0]); index++) {
        store_byte(GUEST_FORMAT_INFO + entries[index].format, entries[index].info);
    }
}

static TEST_UNUSED void seed_floats(void)
{
    store(GUEST_FLOAT_TWO_POW_32, 0x4F800000u);
    store(GUEST_FLOAT_QUARTER, 0x3E800000u);
}

typedef struct {
    uint32_t flags;
    uint32_t size;
    uint32_t mode;
} mode_row;

static TEST_UNUSED void seed_mode_rows(const mode_row *rows, size_t count)
{
    for (size_t index = 0u; index < count; index++) {
        const uint32_t base = GUEST_MODE_TABLE + (uint32_t)index * 12u;
        store(base, rows[index].flags);
        store(base + 4u, rows[index].size);
        store(base + 8u, rows[index].mode);
    }
}

/*
 * THE SYNTHETIC TABLE. Standard 1 (NTSC-M). Row 184 is the 0xFFFFFFFF terminator the real table
 * ends with, which is what stops a block walk; the rows between are zero. Every flag word is built
 * from the bit positions MEASURED in the original: pack in the low byte, standard 0x100,
 * widescreen 0x10000, 720p 0x20000, 1080i 0x40000, 480p 0x80000, 60 Hz class 0x400000, 50 Hz
 * class 0x800000, interlaced 0x200000, field 0x1000000 and 10:11 aspect 0x2000000.
 */
#define SYNTHETIC_TERMINATOR_ROW 184u

static const mode_row synthetic_rows[] = {
    /* 0 */ {0x00400105u, 0x01E00280u, 0x00000001u}, /* VGA pack: a different block */
    /* 1 */ {0x02480104u, 0x01E00280u, 0x88110F01u}, /* 640x480 480p 60 Hz, 10:11 aspect */
    /* 2 */ {0x00420104u, 0x02D00500u, 0x88010F02u}, /* 1280x720, 720p */
    /* 3 */ {0x00410104u, 0x01E002D0u, 0x88220F03u}, /* 720x480, widescreen */
    /* 4 */ {0x00800104u, 0x02400320u, 0x88330F04u}, /* 800x576, 50 Hz class only */
    /* 5 */ {0x00480104u, 0x01E002D0u, 0x88440F05u}, /* 720x480 480p 60 Hz */
    /* 6 */ {0x00480104u, 0x00780140u, 0x00000000u}, /* 320x120, ZERO mode word */
    /* 7 */ {0x00480104u, 0x00780140u, 0x88880F09u}, /* 320x120 again, usable mode word */
    /* 8 */ {0x00400103u, 0x01E00280u, 0x88550F06u}, /* pack 3: ends the pack-4 block */
    /* 9 */ {0x00400100u, 0x01E00280u, 0x88660F07u}, /* pack 0: the fallback block */
    /* 10 */ {0x00600100u, 0x01E002D0u, 0x88770F08u}, /* 720x480 interlaced 60 Hz, pack 0 */
    /* 11 */ {0x00430204u, 0x02D00500u, 0x88990F0Au}, /* standard 2, pack 4: 720p widescreen */
};

static TEST_UNUSED void seed_synthetic_table(void)
{
    seed_mode_rows(synthetic_rows, sizeof(synthetic_rows) / sizeof(synthetic_rows[0]));
    store(GUEST_MODE_TABLE + SYNTHETIC_TERMINATOR_ROW * 12u, 0xFFFFFFFFu);
}

/* The .rdata floats and the scale table the recompute 0x003D7B80 reads (T533): 2^32, 0.5, 1.0 and 8.0 at 0x475CCC,
 * 0x475CD4, 0x475C78 and 0x47D1E4, and the table at 0x003E1B48 (index trunc(2 * scale + 0.5)). The synthetic guest
 * has no .rdata, and with them zero every scale it computes is zero too. */
static TEST_UNUSED void seed_recompute_constants(void)
{
    static const uint32_t table[7] = {0u, 0xBF800000u, 0u, 0x3F15C28Fu, 0x3F800000u, 0x3FA9374Cu, 0x3FCAE148u};
    store(0x00475CCCu, 0x4F800000u);
    store(0x00475CD4u, 0x3F000000u);
    store(0x00475C78u, 0x3F800000u);
    store(0x0047D1E4u, 0x41000000u);
    for (uint32_t index = 0u; index < 7u; index++) store(0x003E1B48u + index * 4u, table[index]);
}

/* --- the fatal catcher ------------------------------------------------------------------- */

static jmp_buf fatal_jump;
static bool fatal_armed;
static char fatal_text[256];
static uint32_t fatal_address;

static TEST_UNUSED void catching_fatal(uint32_t address, const char *message)
{
    fatal_address = address;
    snprintf(fatal_text, sizeof(fatal_text), "%s", message);
    if (fatal_armed) {
        fatal_armed = false;
        longjmp(fatal_jump, 1);
    }
}

/* Run `code`; afterwards `fatal_seen` says whether it called d3d8_hle_fatal. The setjmp sits in
 * the controlling expression of an `if`, which is the form the C standard allows. */
static volatile bool fatal_seen;

#define RUN_EXPECTING_FATAL(code)                                                         \
    do {                                                                                  \
        fatal_seen = false;                                                               \
        fatal_text[0] = '\0';                                                             \
        fatal_armed = true;                                                               \
        if (setjmp(fatal_jump) == 0) {                                                    \
            code;                                                                         \
            fatal_armed = false;                                                          \
        } else {                                                                          \
            fatal_seen = true;                                                            \
        }                                                                                 \
    } while (0)

/* --- the whole environment --------------------------------------------------------------- */

static char captured[16384];
static size_t captured_length;

static TEST_UNUSED int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(captured + captured_length, sizeof(captured) - captured_length,
                                  format, args);
    va_end(args);
    if (written > 0) {
        captured_length += (size_t)written;
        if (captured_length >= sizeof(captured)) {
            captured_length = sizeof(captured) - 1u;
        }
    }
    return written;
}

static TEST_UNUSED bool captured_has(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

static TEST_UNUSED void capture_clear(void)
{
    captured[0] = '\0';
    captured_length = 0u;
}

static kernel_guest_ptr call_scratch;

/* Every address a suite dispatches. Rows for addresses the suite does not register are
 * harmless: d3d8_hle_register skips what is absent and the rest stay stubs. */
static const uint32_t suite_addresses[] = {
    0x003D9000u, 0x003D9010u, 0x003D90B0u, 0x003D9210u, 0x003D9230u, 0x003D6C90u, 0x003D3A80u,
    0x003D4EE0u, 0x003D5EB0u, 0x003D7060u, 0x003D7F70u, 0x003D8010u, 0x003D7150u, 0x003D80B0u,
    0x003D8190u, 0x003D81B0u, 0x003D7EE0u, 0x003D81F0u, 0x003D72A0u, 0x003D5AF0u, 0x003D5670u,
    0x003D3530u, 0x003D6660u, 0x003D6680u};

/* Reset everything and map the guest. `pack` is the kernel AV pack, NA certificate region, or
 * KERNEL_AV_PACK_COUNT for an UNCONFIGURED AV module (which refuses the capability query). */
static TEST_UNUSED void environment_begin(kernel_av_pack pack)
{
    guest_mem_reset();
    d3d8_guest_reset();
    d3d8_resource_reset();
    d3d8_pushbuffer_reset();
    kernel_hle_init();
    kernel_av_reset();
    capture_clear();
    kernel_hle_set_log(capture_printer);
    d3d8_hle_set_log(capture_printer);
    d3d8_hle_set_fatal(catching_fatal);
    (void)kernel_register_all();
    if (pack != KERNEL_AV_PACK_COUNT && !kernel_av_configure(pack, KERNEL_AV_REGION_NA)) {
        printf("FATAL AV configure failed\n");
        exit(EXIT_FAILURE);
    }

    map_fixed(D3D_REGION_BASE, D3D_REGION_BYTES);
    map_fixed(RDATA_REGION_BASE, RDATA_REGION_BYTES);
    seed_pitch_table();
    seed_format_table();
    seed_floats();
    seed_synthetic_table();

    d3d8_surface_entry rows[sizeof(suite_addresses) / sizeof(suite_addresses[0])];
    for (size_t index = 0u; index < sizeof(suite_addresses) / sizeof(suite_addresses[0]);
         index++) {
        rows[index].address = suite_addresses[index];
        rows[index].name = NULL;
        rows[index].sites = 1u;
    }
    if (!d3d8_hle_init(rows, sizeof(rows) / sizeof(rows[0]))) {
        printf("FATAL d3d8_hle_init failed\n");
        exit(EXIT_FAILURE);
    }

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    call_scratch = guest_region_alloc(&request, &status);
    if (call_scratch == 0u) {
        printf("FATAL no scratch\n");
        exit(EXIT_FAILURE);
    }
}

static TEST_UNUSED void environment_end(void)
{
    d3d8_hle_set_fatal(NULL);
    d3d8_hle_shutdown();
    kernel_hle_set_log(NULL);
    d3d8_hle_set_log(NULL);
    guest_mem_reset();
}

/* The guest scratch past the call frame, for arguments that are pointers. */
#define SCRATCH_DATA (call_scratch + 0x1000u)

/* Dispatch `address` through the D3D8 table with a stdcall frame, as xdk_thunk.c does. */
static TEST_UNUSED uint32_t call_stdcall(uint32_t address, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, call_scratch, 0x100u, args, count)) {
        printf("FATAL frame\n");
        exit(EXIT_FAILURE);
    }
    kernel_frame_set_registers(&frame, 0u, 0u);
    return d3d8_hle_call(address, &frame);
}

static TEST_UNUSED uint32_t call_fastcall(uint32_t address, uint32_t ecx, uint32_t edx)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, call_scratch, 0x100u, NULL, 0u)) {
        printf("FATAL frame\n");
        exit(EXIT_FAILURE);
    }
    kernel_frame_set_registers(&frame, ecx, edx);
    return d3d8_hle_call(address, &frame);
}

/* The 17-dword D3DPRESENT_PARAMETERS the title builds at 0x00023930, for a given flags word.
 * 640x480, A8R8G8B8, one back buffer, no multisampling, swap effect 3, depth on with D24S8,
 * 60 Hz, interval 1. */
static TEST_UNUSED void write_title_parameters(uint32_t address, uint32_t flags)
{
    static const uint32_t words[17] = {0x280, 0x1E0, 6, 1, 0x11, 3, 0, 0, 1,
                                       0x2A,  0,     0x3C, 1, 0, 0, 0, 0};
    for (unsigned index = 0u; index < 17u; index++) {
        store(address + index * 4u, index == 10u ? flags : words[index]);
    }
}

#endif /* TSFP_TESTS_D3D8_SUPPORT_H */
