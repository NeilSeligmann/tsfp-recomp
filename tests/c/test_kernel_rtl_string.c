/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RtlAnsiStringToUnicodeString (260) and RtlUnicodeStringToAnsiString (308), exactly as the
 * XAPI MultiByteToWideChar / WideCharToMultiByte wrappers drive them: descriptors built in
 * guest memory, AllocateDestinationString 0. Every refusal in kernel_rtl_string.h has a case.
 */

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_rtl_string.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static int failures;
static int checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
            fflush(stdout);                                                          \
        }                                                                            \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                               \
    do {                                                                             \
        checks++;                                                                    \
        const uint32_t check_a = (uint32_t)(actual);                                 \
        const uint32_t check_e = (uint32_t)(expected);                               \
        if (check_a != check_e) {                                                    \
            failures++;                                                              \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__,         \
                   __LINE__, #actual, #expected, check_a, check_e);                  \
            fflush(stdout);                                                          \
        }                                                                            \
    } while (0)

static char captured[8192];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len, format,
                            args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static void clear_capture(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* Layout of the scratch region, all inside one writable region of REGION_BYTES. */
#define REGION_BYTES 0x30000u
#define FRAME_AT 0x100u
#define SOURCE_DESC 0x200u
#define DEST_DESC 0x210u
#define SOURCE_TEXT 0x1000u
#define DEST_TEXT 0x9000u
#define SENTINEL 0xA5u

static kernel_guest_ptr base;

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_rtl_string_register(), 2u);
    kernel_hle_set_log(capture_printer);
    clear_capture();
    guest_region_request request = {
        .bytes = REGION_BYTES,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
    };
    nt_status status = STATUS_SUCCESS;
    base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        printf("  FATAL: no scratch memory\n");
        exit(EXIT_FAILURE);
    }
    memset((void *)(uintptr_t)(base + DEST_TEXT), SENTINEL, 0x1000u);
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    base = 0u;
}

static void put_descriptor(uint32_t at, uint16_t length, uint16_t maximum, uint32_t buffer)
{
    guest_object_string descriptor = {length, maximum, buffer};
    CHECK(kernel_guest_write_bytes(base + at, &descriptor, sizeof(descriptor)));
}

static guest_object_string get_descriptor(uint32_t at)
{
    guest_object_string descriptor;
    CHECK(kernel_guest_read_bytes(base + at, &descriptor, sizeof(descriptor)));
    return descriptor;
}

static uint8_t byte_at(uint32_t offset)
{
    uint8_t value = 0u;
    CHECK(kernel_guest_read_u8(base + offset, &value));
    return value;
}

/* The wrapper's call: push 0 (Allocate), push &source, push &destination. */
static uint32_t convert(unsigned ordinal, uint32_t destination, uint32_t source,
                        uint32_t allocate)
{
    const uint32_t args[3] = {destination, source, allocate};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, base, FRAME_AT, args, 3u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static void put_ansi_source(const char *text, uint16_t length)
{
    CHECK(kernel_guest_write_bytes(base + SOURCE_TEXT, text, length));
    put_descriptor(SOURCE_DESC, length, (uint16_t)(length + 1u), base + SOURCE_TEXT);
}

static void put_wide_source(const uint16_t *text, uint16_t characters)
{
    CHECK(kernel_guest_write_bytes(base + SOURCE_TEXT, text, (size_t)characters * 2u));
    put_descriptor(SOURCE_DESC, (uint16_t)(characters * 2u),
                   (uint16_t)(characters * 2u + 2u), base + SOURCE_TEXT);
}

/* ------------------------------------------------------------------ 260 */

static void test_260_widens_and_terminates(void)
{
    setup();
    put_ansi_source("Z:\\a.xbe", 8u);
    /* Exactly the wrapper's destination: Length is a placeholder the callee overwrites. */
    put_descriptor(DEST_DESC, 16u, 18u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    const guest_object_string dest = get_descriptor(DEST_DESC);
    CHECK_EQ_U32(dest.length, 16u);
    CHECK_EQ_U32(dest.maximum_length, 18u);
    CHECK_EQ_U32(dest.buffer, base + DEST_TEXT);
    const char *expected = "Z:\\a.xbe";
    for (unsigned i = 0u; i < 8u; i++) {
        CHECK_EQ_U32(byte_at(DEST_TEXT + 2u * i), (uint8_t)expected[i]);
        CHECK_EQ_U32(byte_at(DEST_TEXT + 2u * i + 1u), 0u);
    }
    /* The WCHAR NUL after the text, and nothing past it. */
    CHECK_EQ_U32(byte_at(DEST_TEXT + 16u), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 17u), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 18u), SENTINEL);
    CHECK(captured[0] == '\0');
    CHECK_EQ_U32(kernel_rtl_string_nonascii_count(), 0u);
    teardown();
}

static void test_260_empty_string_and_roomier_buffer(void)
{
    setup();
    put_ansi_source("", 0u);
    put_descriptor(DEST_DESC, 99u, 64u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 0u);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).maximum_length, 64u);
    CHECK_EQ_U32(byte_at(DEST_TEXT), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 1u), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 2u), SENTINEL);
    /* An empty source with no buffer at all is still empty. */
    put_descriptor(SOURCE_DESC, 0u, 0u, 0u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    teardown();
}

static void test_260_exact_fit_and_one_byte_short(void)
{
    setup();
    put_ansi_source("abc", 3u);
    put_descriptor(DEST_DESC, 0u, 8u, base + DEST_TEXT); /* needs 2*3 + 2 = 8 */
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 6u);
    memset((void *)(uintptr_t)(base + DEST_TEXT), SENTINEL, 16u);
    put_descriptor(DEST_DESC, 77u, 7u, base + DEST_TEXT);
    clear_capture();
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_BUFFER_OVERFLOW);
    CHECK_EQ_U32(STATUS_BUFFER_OVERFLOW, 0x80000005u);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u); /* untouched */
    for (unsigned i = 0u; i < 16u; i++) {
        CHECK_EQ_U32(byte_at(DEST_TEXT + i), SENTINEL); /* nothing written */
    }
    CHECK(captured_contains("STATUS_BUFFER_OVERFLOW"));
    teardown();
}

static void test_260_high_bytes_are_reported(void)
{
    setup();
    put_ansi_source("a\xE9z\x80", 4u);
    put_descriptor(DEST_DESC, 0u, 10u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 2u), 0xE9u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 3u), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 6u), 0x80u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 7u), 0u);
    CHECK_EQ_U32(kernel_rtl_string_nonascii_count(), 2u);
    CHECK(captured_contains("INFERRED"));
    teardown();
}

static void test_260_embedded_nul_is_an_ordinary_byte(void)
{
    setup();
    put_ansi_source("a\0b", 3u);
    put_descriptor(DEST_DESC, 0u, 8u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 6u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 4u), 'b');
    teardown();
}

static void test_260_refuses_allocation_and_oversize(void)
{
    setup();
    put_ansi_source("abc", 3u);
    put_descriptor(DEST_DESC, 77u, 64u, base + DEST_TEXT);
    clear_capture();
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 1u), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0x100u),
                 STATUS_SUCCESS); /* BOOLEAN: only the low byte counts */
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0xFFu), STATUS_NOT_IMPLEMENTED);
    CHECK(captured_contains("AllocateDestinationString is unmeasured"));
    /* 32767 bytes need 2*32767 + 2 = 65536 > 0xFFFF: a counted string cannot hold it. */
    put_descriptor(SOURCE_DESC, 32767u, 32768u, base + SOURCE_TEXT);
    put_descriptor(DEST_DESC, 77u, 0xFFFFu, base + DEST_TEXT);
    clear_capture();
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u),
                 STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("more than a counted string can describe"));
    /* 32766 bytes need 65534, which fits. */
    put_descriptor(SOURCE_DESC, 32766u, 32767u, base + SOURCE_TEXT);
    put_descriptor(DEST_DESC, 77u, 0xFFFFu, base + 0x10000u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 65532u);
    teardown();
}

static void test_bad_pointers_fault_and_write_nothing(void)
{
    setup();
    put_ansi_source("abc", 3u);
    put_descriptor(DEST_DESC, 77u, 64u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, 0u, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, 0u, 0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(convert(260u, 0x2000u, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    /* Source text pointing nowhere. */
    put_descriptor(SOURCE_DESC, 3u, 4u, 0x2000u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    put_descriptor(SOURCE_DESC, 3u, 4u, 0u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    /* Destination text pointing nowhere or at nothing: Length must stay as it was. */
    put_ansi_source("abc", 3u);
    put_descriptor(DEST_DESC, 77u, 64u, 0x2000u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    put_descriptor(DEST_DESC, 77u, 64u, 0u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u);
    /* A read-only destination buffer is refused, again with Length untouched. */
    put_descriptor(DEST_DESC, 77u, 64u, base + 0x20000u);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x20000u), 0x1000u, PROT_READ) == 0);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x20000u), 0x1000u, PROT_READ | PROT_WRITE) == 0);
    /* A destination DESCRIPTOR that is readable but not writable: the text is converted into
     * its (writable) buffer, then the Length write faults, and that is reported as a fault in
     * BOTH directions, never as success with a stale Length. */
    const uint32_t read_only_descriptor = 0x20000u + 0x10u;
    put_ansi_source("abc", 3u);
    put_descriptor(read_only_descriptor, 77u, 64u, base + DEST_TEXT);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x20000u), 0x1000u, PROT_READ) == 0);
    CHECK_EQ_U32(convert(260u, base + read_only_descriptor, base + SOURCE_DESC, 0u),
                 STATUS_ACCESS_VIOLATION);
    CHECK(captured_contains("destination descriptor is not writable"));
    CHECK_EQ_U32(get_descriptor(read_only_descriptor).length, 77u);
    const uint16_t wide_abc[] = {'a', 'b', 'c'};
    put_wide_source(wide_abc, 3u);
    clear_capture();
    CHECK_EQ_U32(convert(308u, base + read_only_descriptor, base + SOURCE_DESC, 0u),
                 STATUS_ACCESS_VIOLATION);
    CHECK(captured_contains("destination descriptor is not writable"));
    CHECK_EQ_U32(get_descriptor(read_only_descriptor).length, 77u);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x20000u), 0x1000u, PROT_READ | PROT_WRITE) == 0);
    /* Missing frame and short frame. */
    CHECK_EQ_U32(kernel_hle_call(260u, NULL), STATUS_INVALID_PARAMETER);
    kernel_call_frame short_frame = {.stack_ptr = base + FRAME_AT, .stack_limit = base + FRAME_AT + 8u};
    CHECK_EQ_U32(kernel_hle_call(308u, &short_frame), STATUS_INVALID_PARAMETER);
    teardown();
}

static void test_260_overlapping_buffers(void)
{
    setup();
    /* Destination text overlaps the source text one byte further on. */
    put_ansi_source("abcd", 4u);
    put_descriptor(DEST_DESC, 0u, 16u, base + SOURCE_TEXT + 1u);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(byte_at(SOURCE_TEXT + 1u), 'a');
    CHECK_EQ_U32(byte_at(SOURCE_TEXT + 3u), 'b');
    CHECK_EQ_U32(byte_at(SOURCE_TEXT + 5u), 'c');
    CHECK_EQ_U32(byte_at(SOURCE_TEXT + 7u), 'd');
    teardown();
}

/* ------------------------------------------------------------------ 308 */

static void test_308_narrows_and_terminates(void)
{
    setup();
    const uint16_t text[3] = {'x', 'y', 0x00FFu};
    put_wide_source(text, 3u);
    /* The wrapper's destination: Length 0, MaximumLength = cbMultiByte. */
    put_descriptor(DEST_DESC, 0u, 4u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    const guest_object_string dest = get_descriptor(DEST_DESC);
    CHECK_EQ_U32(dest.length, 3u);
    CHECK_EQ_U32(dest.maximum_length, 4u);
    CHECK_EQ_U32(dest.buffer, base + DEST_TEXT);
    CHECK_EQ_U32(byte_at(DEST_TEXT), 'x');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 1u), 'y');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 2u), 0xFFu);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 3u), 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 4u), SENTINEL);
    /* 0xFF is within the byte range, so it is converted, but it is not ASCII. */
    CHECK_EQ_U32(kernel_rtl_string_nonascii_count(), 0u);
    teardown();
}

static void test_308_replaces_unmappable_characters(void)
{
    setup();
    const uint16_t text[4] = {'a', 0x0100u, 0x20ACu, 'b'};
    put_wide_source(text, 4u);
    put_descriptor(DEST_DESC, 0u, 5u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 4u);
    CHECK_EQ_U32(byte_at(DEST_TEXT), 'a');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 1u), '?');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 2u), '?');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 3u), 'b');
    CHECK_EQ_U32(byte_at(DEST_TEXT + 4u), 0u);
    CHECK_EQ_U32(kernel_rtl_string_nonascii_count(), 2u);
    CHECK(captured_contains("character(s) above 0xFF"));
    /* 0x0100 is the first unmappable value and 0x00FF the last mappable: pin the edge. */
    const uint16_t edge[2] = {0x00FFu, 0x0100u};
    put_wide_source(edge, 2u);
    put_descriptor(DEST_DESC, 0u, 3u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(byte_at(DEST_TEXT), 0xFFu);
    CHECK_EQ_U32(byte_at(DEST_TEXT + 1u), '?');
    teardown();
}

static void test_308_buffer_limits(void)
{
    setup();
    const uint16_t text[3] = {'a', 'b', 'c'};
    put_wide_source(text, 3u);
    put_descriptor(DEST_DESC, 77u, 3u, base + DEST_TEXT); /* needs 3 + 1 */
    clear_capture();
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_BUFFER_OVERFLOW);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u);
    CHECK_EQ_U32(byte_at(DEST_TEXT), SENTINEL);
    put_descriptor(DEST_DESC, 77u, 4u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 3u);
    /* Allocation is unmeasured. */
    put_descriptor(DEST_DESC, 77u, 64u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 1u), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u);
    /* An odd Unicode Length is unmeasured and refused, not truncated. */
    put_descriptor(SOURCE_DESC, 5u, 8u, base + SOURCE_TEXT);
    clear_capture();
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("is odd"));
    /* 65534 bytes of Unicode are 32767 characters, needing 32768 bytes: fine. 65535 is odd. */
    put_descriptor(SOURCE_DESC, 0xFFFEu, 0xFFFEu, base + SOURCE_TEXT);
    put_descriptor(DEST_DESC, 77u, 0xFFFFu, base + 0x10000u);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 32767u);
    teardown();
}

static void test_308_empty_and_faults(void)
{
    setup();
    put_descriptor(SOURCE_DESC, 0u, 0u, 0u);
    put_descriptor(DEST_DESC, 77u, 1u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 0u);
    CHECK_EQ_U32(byte_at(DEST_TEXT), 0u);
    put_descriptor(SOURCE_DESC, 4u, 6u, 0x2000u);
    put_descriptor(DEST_DESC, 77u, 8u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 77u);
    teardown();
}

static void test_round_trip(void)
{
    setup();
    put_ansi_source("\\Device\\Harddisk0", 17u);
    put_descriptor(DEST_DESC, 0u, 36u, base + DEST_TEXT);
    CHECK_EQ_U32(convert(260u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    /* Feed the Unicode result back through 308 into a second buffer. */
    put_descriptor(SOURCE_DESC, get_descriptor(DEST_DESC).length, 36u, base + DEST_TEXT);
    put_descriptor(DEST_DESC, 0u, 18u, base + 0x10000u);
    CHECK_EQ_U32(convert(308u, base + DEST_DESC, base + SOURCE_DESC, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(get_descriptor(DEST_DESC).length, 17u);
    CHECK(memcmp((void *)(uintptr_t)(base + 0x10000u), "\\Device\\Harddisk0", 18u) == 0);
    teardown();
}

int main(void)
{
    test_260_widens_and_terminates();
    test_260_empty_string_and_roomier_buffer();
    test_260_exact_fit_and_one_byte_short();
    test_260_high_bytes_are_reported();
    test_260_embedded_nul_is_an_ordinary_byte();
    test_260_refuses_allocation_and_oversize();
    test_bad_pointers_fault_and_write_nothing();
    test_260_overlapping_buffers();
    test_308_narrows_and_terminates();
    test_308_replaces_unmappable_characters();
    test_308_buffer_limits();
    test_308_empty_and_faults();
    test_round_trip();
    printf("kernel_rtl_string: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
