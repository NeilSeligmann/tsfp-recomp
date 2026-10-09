/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_hle.h"
#include "xvoice_media.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define XVOICE_CREATE_EX 0x004754FDu
#define MIC_TABLE 0x0046C894u
#define HEADPHONE_TABLE 0x0046C8A0u
#define HIGH_FIDELITY_TABLE 0x0046C8ACu
#define TITLE_CALLBACK 0x00387674u
#define SCRATCH 0x50000000u
#define FORMAT (SCRATCH + 0x200u)
#define OUTPUT (SCRATCH + 0x300u)
#define CALL_STACK (SCRATCH + 0x1000u)

static const uint32_t rates[] = {8000u, 11025u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u};
static jmp_buf fatal_jump;
static bool fatal_armed;
static bool fatal_seen;
static unsigned checks;
static unsigned failures;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(condition)) {                                                              \
            failures++;                                                                  \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);                 \
        }                                                                                \
    } while (0)

static void check_u32(uint32_t actual, uint32_t expected)
{
    checks++;
    if (actual != expected) {
        failures++;
        printf("FAIL %s:%d got %#x expected %#x\n", __FILE__, __LINE__, actual, expected);
    }
}

static void fixed_region(uint32_t base)
{
    const guest_region_request request = {
        .bytes = 0x10000u,
        .alignment = GUEST_ALLOCATION_GRANULARITY,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .fixed_base = base,
    };
    nt_status status = STATUS_SUCCESS;
    CHECK(guest_region_alloc(&request, &status) == base);
    check_u32((uint32_t)status, (uint32_t)STATUS_SUCCESS);
}

static void fatal_capture(unsigned address, const char *message)
{
    (void)address;
    (void)message;
    fatal_seen = true;
    if (fatal_armed) {
        fatal_armed = false;
        longjmp(fatal_jump, 1);
    }
    abort();
}

static int quiet_log(const char *format, ...)
{
    (void)format;
    return 0;
}

static void prepare(void)
{
    guest_mem_reset();
    kernel_hle_init();
    kernel_hle_set_log(quiet_log);
    kernel_hle_set_fatal(fatal_capture);
    fixed_region(0x00460000u);
    fixed_region(SCRATCH);
    xinput_hle_init();
    CHECK(xvoice_media_register() == 1u);
    CHECK(xinput_hle_implemented_count() == 1u);
    memset(kernel_guest_at(SCRATCH + 0x2FCu, 12u), 0xA5, 12u);
}

static uint32_t call_create(uint32_t table, uint32_t port, uint32_t rate,
                            uint32_t callback, uint32_t context, uint32_t output)
{
    const uint32_t args[7] = {table, port, 2u, FORMAT, callback, context, output};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, CALL_STACK, 0x100u, args, 7u));
    frame.stack_limit = CALL_STACK + 0x100u;
    kernel_frame_set_registers(&frame, 0u, 0u);
    CHECK(kernel_guest_write_u32(FORMAT + 4u, rate));
    return xinput_hle_call(XVOICE_CREATE_EX, &frame);
}

static void expect_refusal(uint32_t table, uint32_t callback, uint32_t output)
{
    fatal_seen = false;
    fatal_armed = true;
    if (setjmp(fatal_jump) == 0) {
        (void)call_create(table, 0u, rates[0], callback, SCRATCH + 0x500u, output);
        fatal_armed = false;
        CHECK(false);
    } else {
        CHECK(fatal_seen);
    }
    fatal_armed = false;
}

static void test_measured_disconnected_calls(void)
{
    prepare();
    unsigned calls = 0u;
    for (unsigned table_index = 0u; table_index < 2u; table_index++) {
        const uint32_t table = table_index == 0u ? MIC_TABLE : HEADPHONE_TABLE;
        for (uint32_t port = 0u; port < 4u; port++) {
            for (size_t rate_index = 0u; rate_index < sizeof(rates) / sizeof(rates[0]); rate_index++) {
                CHECK(kernel_guest_write_u32(OUTPUT, 0xDEADBEEFu));
                check_u32(call_create(table, port, rates[rate_index], TITLE_CALLBACK,
                                      SCRATCH + 0x500u, OUTPUT), 0x8007048Fu);
                uint32_t word;
                CHECK(kernel_guest_read_u32(OUTPUT, &word));
                check_u32(word, 0u);
                CHECK(kernel_guest_read_u32(OUTPUT - 4u, &word));
                check_u32(word, 0xA5A5A5A5u);
                CHECK(kernel_guest_read_u32(OUTPUT + 4u, &word));
                check_u32(word, 0xA5A5A5A5u);
                calls++;
            }
        }
    }
    check_u32(calls, 64u);
    guest_mem_reset();
}

static void test_unmeasured_shapes_refuse_before_output_write(void)
{
    prepare();
    CHECK(kernel_guest_write_u32(OUTPUT, 0xDEADBEEFu));
    expect_refusal(HIGH_FIDELITY_TABLE, TITLE_CALLBACK, OUTPUT);
    uint32_t word;
    CHECK(kernel_guest_read_u32(OUTPUT, &word));
    check_u32(word, 0xDEADBEEFu);

    expect_refusal(MIC_TABLE, TITLE_CALLBACK + 1u, OUTPUT);
    CHECK(kernel_guest_read_u32(OUTPUT, &word));
    check_u32(word, 0xDEADBEEFu);

    expect_refusal(MIC_TABLE, TITLE_CALLBACK, 0x00610000u);
    CHECK(kernel_guest_read_u32(OUTPUT, &word));
    check_u32(word, 0xDEADBEEFu);
    guest_mem_reset();
}

int main(void)
{
    test_measured_disconnected_calls();
    test_unmeasured_shapes_refuse_before_output_write();
    kernel_hle_set_fatal(NULL);
    kernel_hle_set_log(NULL);
    printf("T1086 voice media checks: %u, failures: %u\n", checks, failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
