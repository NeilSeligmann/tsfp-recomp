/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The T51 dispatch probe in src/host/xdk_thunk.c (TSFP_XDK_PROBE).
 *
 * The probe is how a boot reports which program, shader key and draw the title reached at
 * an XDK boundary, so a probe that lied would put a wrong key into docs/shader-inputs.md.
 * What is proved here:
 *   1. a probed address logs one line with the caller, the stack arguments, the sampled
 *      guest dword and the FNV-1a 64 hash of the named buffer, and an unprobed address
 *      logs nothing (both directions);
 *   2. the line is written BEFORE the stop checks, so a call that stops the run (an
 *      unimplemented XDK address) is still logged;
 *   3. the probe changes nothing the guest can see: eax and esp after the call are exactly
 *      what the dispatcher alone leaves;
 *   4. an over-long hash length is reported as unreadable rather than read;
 *   5. a malformed spec ends the process with status 2, because a silently disabled probe
 *      reads as "the game never made that call" (checked in a forked child, before the
 *      once-only parse in this process).
 *
 * Free of lifted code, the XBE and any disc, like tests/c/test_xdk_dispatch.c. Each check
 * is mutation-tested by tools/mutate/sets/shaderprobe.py.
 */

#include "xdk_thunk.h"

#include "d3d8_hle.h"
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "thunk_trace.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

static char captured[65536];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                            format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static void capture_clear(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static size_t count_of(const char *needle)
{
    size_t found = 0;
    for (const char *at = captured; (at = strstr(at, needle)) != NULL; at += strlen(needle)) {
        found++;
    }
    return found;
}

#define SCRATCH_BYTES 0x10000u
#define OFF_STACK 0x0800u
#define OFF_WORD 0x0100u
#define OFF_BUFFER 0x0200u
static kernel_guest_ptr scratch;

static bool scratch_init(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    return scratch != 0u;
}

/* Real retail addresses: the D3D module refuses an address absent from its table. */
#define ADDR_PROBED 0x003D57D0u   /* implemented, probed */
#define ADDR_UNPROBED 0x003D9230u /* not probed */
#define ADDR_STOPPING 0x003D4FB0u /* probed, no handler: stops the run */
#define RETURN_ADDRESS 0x0002A8E4u
#define HANDLER_RESULT 0x0D3D0001u

static const xdk_dispatch_entry SURFACE[] = {
    {ADDR_PROBED, NULL, XDK_MODULE_D3D8},
    {ADDR_UNPROBED, NULL, XDK_MODULE_D3D8},
    {ADDR_STOPPING, NULL, XDK_MODULE_D3D8},
};
static const d3d8_surface_entry D3D_SURFACE[] = {
    {ADDR_PROBED, NULL, 93u},
    {ADDR_UNPROBED, NULL, 1u},
    {ADDR_STOPPING, NULL, 1u},
};

static uint32_t handler(void *context)
{
    (void)context;
    return HANDLER_RESULT;
}

static void reset_all(void)
{
    capture_clear();
    thunk_trace_reset();
    CHECK(d3d8_hle_init(D3D_SURFACE, 3u));
    CHECK(xdk_thunk_init(SURFACE, 3u));
    xdk_thunk_set_stop_on_missing(true);
    xdk_thunk_set_log(capture_printer);
    d3d8_hle_set_log(capture_printer);
    CHECK(d3d8_hle_register(ADDR_PROBED, handler));
    CHECK(d3d8_hle_register(ADDR_UNPROBED, handler));
    CHECK(xdk_thunk_declare_abi(ADDR_PROBED, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_UNPROBED, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_STOPPING, XDK_CC_STDCALL, 3u, 0u));
}

static uint32_t esp_at_entry;

static void prepare_frame(uint32_t arg0, uint32_t arg1)
{
    const kernel_guest_ptr base = scratch + OFF_STACK;
    g_esp = base;
    CHECK(kernel_guest_write_u32(base, RETURN_ADDRESS));
    CHECK(kernel_guest_write_u32(base + 4u, arg0));
    CHECK(kernel_guest_write_u32(base + 8u, arg1));
    esp_at_entry = g_esp;
    g_eax = 0xFEEDFACEu;
}

static bool stopped;

static void dispatch(uint32_t address)
{
    recomp_func_t fn = recomp_lookup_manual(address);
    CHECK(fn != NULL);
    stopped = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        fn();
    } else {
        stopped = true;
    }
    host_run_disarm();
}

/* FNV-1a 64, written here independently of xdk_thunk.c. */
static uint64_t fnv(const uint8_t *bytes, size_t length)
{
    uint64_t value = 14695981039346656037ull;
    for (size_t i = 0; i < length; i++) {
        value = (value ^ bytes[i]) * 1099511628211ull;
    }
    return value;
}

static void test_a_probed_address_logs_the_call(void)
{
    reset_all();
    const uint8_t text[4] = {'a', 'b', 'c', 'd'};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(kernel_guest_write_u8(scratch + OFF_BUFFER + i, text[i]));
    }
    CHECK(kernel_guest_write_u32(scratch + OFF_WORD, 0x3Du));
    prepare_frame(scratch + OFF_BUFFER, 4u);
    dispatch(ADDR_PROBED);
    CHECK(!stopped);
    CHECK(count_of("probe: ") == 1u);
    CHECK(strstr(captured, "addr=0x003D57D0 caller=0x0002A8E4") != NULL);
    char expected_args[64];
    snprintf(expected_args, sizeof(expected_args), "args=0x%X,0x4,", (unsigned)(scratch + OFF_BUFFER));
    CHECK(strstr(captured, expected_args) != NULL);
    char expected_word[64];
    snprintf(expected_word, sizeof(expected_word), "words=0x%X:0x3D", (unsigned)(scratch + OFF_WORD));
    CHECK(strstr(captured, expected_word) != NULL);
    char expected_hash[64];
    snprintf(expected_hash, sizeof(expected_hash), "hash=%016llx/4", (unsigned long long)fnv(text, 4));
    CHECK(strstr(captured, expected_hash) != NULL);
    /* The probe is invisible to the guest. */
    CHECK(g_eax == HANDLER_RESULT);
    CHECK(g_esp == esp_at_entry + 4u + 8u);
}

static void test_an_unprobed_address_logs_nothing(void)
{
    reset_all();
    prepare_frame(0u, 0u);
    dispatch(ADDR_UNPROBED);
    CHECK(!stopped);
    CHECK(count_of("probe: ") == 0u);
    CHECK(g_eax == HANDLER_RESULT);
}

static void test_the_line_is_written_before_a_stop(void)
{
    reset_all();
    prepare_frame(7u, 8u);
    dispatch(ADDR_STOPPING);
    CHECK(stopped);
    CHECK(count_of("probe: ") == 1u);
    CHECK(strstr(captured, "addr=0x003D4FB0") != NULL);
}

static void test_an_overlong_hash_is_not_read(void)
{
    reset_all();
    prepare_frame(scratch + OFF_BUFFER, 0x100000u);
    dispatch(ADDR_PROBED);
    CHECK(strstr(captured, "hash=unreadable:") != NULL);
    CHECK(strstr(captured, "/1048576") != NULL);
}

/* One forked child per spec: the parse is once-only and the bad spec ends the process. */
static void expect_spec_refused(const char *spec)
{
    fflush(stdout);
    const pid_t child = fork();
    if (child == 0) {
        setenv("TSFP_XDK_PROBE", spec, 1);
        reset_all();
        prepare_frame(0u, 0u);
        dispatch(ADDR_PROBED);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 2)) {
        printf("FAIL spec %s was not refused with status 2\n", spec);
        failures++;
    }
    checks++;
}

static void test_a_malformed_spec_ends_the_process(void)
{
    expect_spec_refused("at=0x3D57D0;bogus=1");
    expect_spec_refused("at=zzz");
    expect_spec_refused("at=0x1FFFFFFFF");
    expect_spec_refused("hash=0x3D57D0:0");
    expect_spec_refused("word=1,2,3,4,5,6,7,8,9");
}

int main(void)
{
    if (!scratch_init()) {
        printf("FAIL could not allocate scratch guest memory\n");
        return 1;
    }
    /* The spec is parsed once, at the first dispatch, so it is set before any. The
     * malformed-spec child runs first and parses its own copy of the process. */
    test_a_malformed_spec_ends_the_process();
    char spec[256];
    snprintf(spec, sizeof(spec), "at=0x%X,0x%X;word=0x%X;hash=0x%X:0:1", ADDR_PROBED,
             ADDR_STOPPING, (unsigned)(scratch + OFF_WORD), ADDR_PROBED);
    setenv("TSFP_XDK_PROBE", spec, 1);
    test_a_probed_address_logs_the_call();
    test_an_unprobed_address_logs_nothing();
    test_the_line_is_written_before_a_stop();
    test_an_overlong_hash_is_not_read();
    CHECK(checks > 20);

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
