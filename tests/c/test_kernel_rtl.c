/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the RTL group: RtlInitAnsiString (289) and RtlNtStatusToDosError (301).
 *
 * WHAT THESE TESTS ARE FOR. Two different kinds of risk, so two different kinds of
 * assertion:
 *
 *   - RtlInitAnsiString writes a GUEST-VISIBLE STRUCTURE. The danger is an offset or
 *     a width being wrong, which does not fault -- it writes the right bytes to the
 *     wrong place. So the descriptor is checked as RAW BYTES at known offsets, not
 *     through the same struct declaration the implementation used. Checking it
 *     through `guest_object_string` would pass just as happily if the struct and the
 *     handler were wrong together, which is the one failure mode that matters.
 *   - RtlNtStatusToDosError is a TABLE. The danger is a plausible wrong number. A
 *     test cannot catch an invented mapping, so what is pinned instead is the
 *     property that makes the table safe: everything unsourced takes a fallback that
 *     is nonzero, positive, counted, and logged by name.
 */

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_rtl.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            fflush(stdout);                                                              \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                        \
        uint32_t check_a = (uint32_t)(actual);                                            \
        uint32_t check_e = (uint32_t)(expected);                                          \
        if (check_a != check_e) {                                                         \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__,    \
                   #actual, #expected, check_a, check_e);                                 \
            fflush(stdout);                                                               \
        }                                                                                 \
    } while (0)

static char captured[16384];
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
            captured_len = sizeof(captured) - 1;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* Scratch guest memory. Frames at the start, structures and strings after.
 *
 * Deliberately larger than the 0xFFFE bound RtlInitAnsiString clamps at, so the
 * unterminated-string test can run the scan to its limit WITHOUT walking off the end
 * of the region. That matters: guest memory is identity-mapped, so a scan that left
 * the region would fault the host and prove nothing. */
#define SCRATCH_BYTES 0x20000u
#define SCRATCH_OUT 0x200u

static kernel_guest_ptr scratch;

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_rtl_reset();
    CHECK_EQ_U32(kernel_rtl_register(), 7u);
    kernel_hle_set_log(capture_printer);
    reset_capture();

    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch (status %#x)\n", status);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hle_set_fatal(NULL);
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

/* Where the descriptor goes, and where the source text goes. Well apart, so an
 * overrun of one lands in neither. */
#define DESCRIPTOR_AT (SCRATCH_OUT + 0x40u)
#define TEXT_AT (SCRATCH_OUT + 0x100u)

static kernel_guest_ptr descriptor_addr(void)
{
    return (kernel_guest_ptr)(scratch + DESCRIPTOR_AT);
}

static kernel_guest_ptr text_addr(void)
{
    return (kernel_guest_ptr)(scratch + TEXT_AT);
}

/* Read the descriptor as RAW BYTES, not through guest_object_string.
 *
 * This is the point of the whole file: if the handler and the struct declaration
 * were wrong in the same direction, reading it back through that same struct would
 * agree with itself. Raw offsets cannot. */
static uint16_t raw_u16(uint32_t byte_offset)
{
    const unsigned char *base =
        (const unsigned char *)kernel_guest_at(descriptor_addr(), 8u);
    if (!base) {
        printf("  FATAL: descriptor is not readable\n");
        exit(EXIT_FAILURE);
    }
    return (uint16_t)((uint16_t)base[byte_offset] | (uint16_t)((uint16_t)base[byte_offset + 1u] << 8));
}

static uint32_t raw_u32(uint32_t byte_offset)
{
    const unsigned char *base =
        (const unsigned char *)kernel_guest_at(descriptor_addr(), 8u);
    if (!base) {
        printf("  FATAL: descriptor is not readable\n");
        exit(EXIT_FAILURE);
    }
    return (uint32_t)base[byte_offset] | ((uint32_t)base[byte_offset + 1u] << 8)
           | ((uint32_t)base[byte_offset + 2u] << 16)
           | ((uint32_t)base[byte_offset + 3u] << 24);
}

static void put_text(const char *text)
{
    size_t length = strlen(text);
    char *destination = (char *)kernel_guest_at(text_addr(), (uint32_t)length + 1u);
    if (!destination) {
        printf("  FATAL: text slot is not writable\n");
        exit(EXIT_FAILURE);
    }
    memcpy(destination, text, length + 1u);
}

/* Fill the descriptor with a pattern no correct result can contain, so a field the
 * handler FAILED to write is distinguishable from one it wrote as zero. Without
 * this, "Length == 0" could mean either, and the NULL-source test would pass against
 * a handler that wrote nothing at all. */
static void poison_descriptor(void)
{
    unsigned char *base = (unsigned char *)kernel_guest_at(descriptor_addr(), 8u);
    if (!base) {
        printf("  FATAL: descriptor is not writable\n");
        exit(EXIT_FAILURE);
    }
    memset(base, 0xA5, 8u);
}

/* ---------------------------------------------------------------------------
 * RtlEqualString (ordinal 279) scaffolding.
 *
 * Two descriptors and two text slots, all well apart, placed ABOVE the single-
 * descriptor region the ordinal-289 tests use so neither group can disturb the other.
 *
 * The descriptors are built HERE rather than by calling ordinal 289, deliberately. If a
 * test built its operands with RtlInitAnsiString and then compared them, a handler that
 * agreed with that handler's bug would pass -- the same trap the raw-byte readback above
 * exists to avoid. These write Length, MaximumLength and Buffer at the measured raw
 * offsets, so the comparison is tested against the STRUCTURE and not against its builder.
 * ------------------------------------------------------------------------- */

#define PAIR_A_DESC (SCRATCH_OUT + 0x400u)
#define PAIR_B_DESC (SCRATCH_OUT + 0x410u)
#define PAIR_A_TEXT (SCRATCH_OUT + 0x500u)
#define PAIR_B_TEXT (SCRATCH_OUT + 0x600u)

static kernel_guest_ptr at(uint32_t offset)
{
    return (kernel_guest_ptr)(scratch + offset);
}

/* Write an 8-byte OBJECT_STRING at `desc` describing `length` bytes at `text`, laying
 * the fields out by RAW OFFSET. `maximum_length` is passed separately because nothing
 * in a comparison should depend on it, and a test proves that by setting it wrong. */
static void put_descriptor(uint32_t desc_offset, uint32_t text_offset, const void *bytes,
                           uint32_t length, uint16_t maximum_length)
{
    if (bytes != NULL && length > 0u) {
        unsigned char *text = (unsigned char *)kernel_guest_at(at(text_offset), length);
        if (!text) {
            printf("  FATAL: pair text slot is not writable\n");
            exit(EXIT_FAILURE);
        }
        memcpy(text, bytes, length);
    }
    unsigned char *base = (unsigned char *)kernel_guest_at(at(desc_offset), 8u);
    if (!base) {
        printf("  FATAL: pair descriptor is not writable\n");
        exit(EXIT_FAILURE);
    }
    const uint32_t buffer = (bytes == NULL) ? 0u : (uint32_t)at(text_offset);
    base[0] = (unsigned char)(length & 0xFFu);
    base[1] = (unsigned char)((length >> 8) & 0xFFu);
    base[2] = (unsigned char)(maximum_length & 0xFFu);
    base[3] = (unsigned char)((maximum_length >> 8) & 0xFFu);
    base[4] = (unsigned char)(buffer & 0xFFu);
    base[5] = (unsigned char)((buffer >> 8) & 0xFFu);
    base[6] = (unsigned char)((buffer >> 16) & 0xFFu);
    base[7] = (unsigned char)((buffer >> 24) & 0xFFu);
}

/* Build both operands from NUL-terminated C text, Length = strlen, the shape ordinal
 * 289 produces. The NUL is NOT part of the comparison, which one test below proves. */
static void put_pair(const char *left, const char *right)
{
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, left, (uint32_t)strlen(left),
                   (uint16_t)(strlen(left) + 1u));
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, right, (uint32_t)strlen(right),
                   (uint16_t)(strlen(right) + 1u));
}

/* Through the DISPATCHER, by ordinal, with a real three-slot stack frame. Going through
 * kernel_hle_call rather than the exported helper is what makes this test cover the
 * argument FETCH -- a handler reading its arguments in the wrong order, or reading three
 * where it should read two, fails here and not in the helper. */
static uint32_t equal_via_dispatch(uint32_t desc_a, uint32_t desc_b, uint32_t ci)
{
    const uint32_t args[3] = {(uint32_t)at(desc_a), (uint32_t)at(desc_b), ci};
    return call_ordinal(279u, args, 3u);
}

/* ---------------------------------------------------------------------------
 * RtlInitAnsiString.
 * ------------------------------------------------------------------------- */

static void test_ansi_string_fields_land_at_the_measured_offsets(void)
{
    setup();
    poison_descriptor();
    put_text("\\Device\\Harddisk0\\partition0");

    uint32_t args[2] = {descriptor_addr(), text_addr()};
    (void)call_ordinal(289u, args, 2u);

    /* 28 characters. The same string, and the same length, as one of the 18
     * statically initialised instances guest_structs.h found in the image at
     * 0x004A151C (len=28 max=29) -- so this is checked against the guest's OWN
     * compile-time answer for the identical input, not just against itself. */
    CHECK_EQ_U32(raw_u16(0x00u), 28u);
    CHECK_EQ_U32(raw_u16(0x02u), 29u);
    CHECK_EQ_U32(raw_u32(0x04u), text_addr());

    /* And the struct declaration agrees with the raw bytes. If this diverges from
     * the three checks above, guest_structs.h is what is wrong. */
    const guest_object_string *view =
        (const guest_object_string *)kernel_guest_at(descriptor_addr(), 8u);
    CHECK(view != NULL);
    if (view) {
        CHECK_EQ_U32(view->length, 28u);
        CHECK_EQ_U32(view->maximum_length, 29u);
        CHECK_EQ_U32(view->buffer, text_addr());
    }
    teardown();
}

static void test_maximum_length_is_always_one_more_than_length(void)
{
    setup();
    /* The joint condition that 18 static instances in the image all satisfy. */
    static const char *const cases[] = {"", "a", "ab", "\\??\\D:", "0123456789abcdef"};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        poison_descriptor();
        put_text(cases[i]);
        uint32_t args[2] = {descriptor_addr(), text_addr()};
        (void)call_ordinal(289u, args, 2u);
        CHECK_EQ_U32(raw_u16(0x00u), (uint32_t)strlen(cases[i]));
        CHECK_EQ_U32(raw_u16(0x02u), (uint32_t)strlen(cases[i]) + 1u);
    }
    teardown();
}

static void test_buffer_aliases_the_source_and_is_not_a_copy(void)
{
    setup();
    poison_descriptor();
    put_text("alias");

    uint32_t args[2] = {descriptor_addr(), text_addr()};
    (void)call_ordinal(289u, args, 2u);

    /* Buffer must BE the source pointer. The guest's own open-coded copy at
     * 0x0037C9CC stores the incoming pointer straight into +0x04, and nothing is
     * allocated -- which is why the guest never calls RtlFreeAnsiString. A handler
     * that copied the text would produce a different pointer here, and the leak
     * would be invisible. */
    CHECK_EQ_U32(raw_u32(0x04u), text_addr());

    /* Prove the aliasing behaviourally as well as by address: mutating the source
     * through the recorded Buffer changes the source. */
    char *through_buffer = (char *)kernel_guest_at(raw_u32(0x04u), 6u);
    CHECK(through_buffer != NULL);
    if (through_buffer) {
        through_buffer[0] = 'A';
        const char *source = (const char *)kernel_guest_at(text_addr(), 6u);
        CHECK(source != NULL && source[0] == 'A');
    }
    teardown();
}

static void test_an_eight_byte_descriptor_does_not_touch_a_ninth_byte(void)
{
    setup();
    /* OBJECT_STRING is 8 bytes. Lifter patch 06 found upstream declaring a
     * 16-byte IO_STATUS_BLOCK where the guest's is 8, overrunning into whatever
     * local sat next to it. This pins that this handler cannot do the same. */
    unsigned char *base = (unsigned char *)kernel_guest_at(descriptor_addr(), 16u);
    CHECK(base != NULL);
    if (!base) {
        teardown();
        return;
    }
    memset(base, 0x5Au, 16u);
    put_text("eight");

    uint32_t args[2] = {descriptor_addr(), text_addr()};
    (void)call_ordinal(289u, args, 2u);

    for (unsigned i = 8u; i < 16u; i++) {
        CHECK_EQ_U32(base[i], 0x5Au);
    }
    teardown();
}

static void test_null_source_empties_the_descriptor_and_reports(void)
{
    setup();
    poison_descriptor();
    uint32_t args[2] = {descriptor_addr(), 0u};
    (void)call_ordinal(289u, args, 2u);

    /* All three fields written, so the poison is gone -- a handler that bailed out
     * without writing would leave 0xA5A5 and fail here. */
    CHECK_EQ_U32(raw_u16(0x00u), 0u);
    CHECK_EQ_U32(raw_u16(0x02u), 0u);
    CHECK_EQ_U32(raw_u32(0x04u), 0u);
    /* Reported, because this path is NOT measured from the image. */
    CHECK(captured_contains("NOT measured"));
    teardown();
}

static void test_null_destination_is_refused_and_reported(void)
{
    setup();
    put_text("ignored");
    uint32_t args[2] = {0u, text_addr()};
    (void)call_ordinal(289u, args, 2u);
    CHECK(captured_contains("destination is not a usable guest address"));
    teardown();
}

static void test_an_unterminated_source_is_clamped_and_reported(void)
{
    setup();
    poison_descriptor();

    /* 0x10000 non-NUL bytes: more than the 0xFFFE the guest's 16-bit Length can
     * express, and comfortably inside the scratch region so the scan never leaves
     * mapped memory. */
    const uint32_t span = 0x10000u;
    unsigned char *text = (unsigned char *)kernel_guest_at(text_addr(), span);
    CHECK(text != NULL);
    if (!text) {
        teardown();
        return;
    }
    memset(text, 'A', span);

    uint32_t args[2] = {descriptor_addr(), text_addr()};
    (void)call_ordinal(289u, args, 2u);

    /* Clamped to what the field can hold, and MaximumLength still Length+1 without
     * wrapping to 0 -- which is the bug a naive (uint16_t)(length + 1) would have. */
    CHECK_EQ_U32(raw_u16(0x00u), 0xFFFEu);
    CHECK_EQ_U32(raw_u16(0x02u), 0xFFFFu);
    CHECK(captured_contains("no NUL within"));
    teardown();
}

static void test_init_ansi_string_without_a_frame_is_reported(void)
{
    setup();
    (void)kernel_hle_call(289u, NULL);
    CHECK(captured_contains("no argument frame"));
    teardown();
}

/* A frame that cannot supply the SECOND argument. This is the test that actually
 * pins the arity: a handler reading only one argument would pass it. */
static void test_a_frame_too_short_for_two_arguments_is_reported(void)
{
    setup();
    /* Room for a return address and one argument only. */
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 8u), 0u, 0u, false, 0u, false};
    (void)kernel_hle_call(289u, &narrow);
    CHECK(captured_contains("could not read its two arguments"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * RtlNtStatusToDosError.
 * ------------------------------------------------------------------------- */

static void test_the_sourced_mappings(void)
{
    setup();
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(STATUS_SUCCESS), 0u);
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(STATUS_INVALID_HANDLE), 6u);
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(STATUS_NO_MEMORY), 8u);
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(STATUS_INVALID_PARAMETER), 87u);
    /* None of those is the fallback, which is the thing that would otherwise make
     * this test vacuous. */
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 0u);
    teardown();
}

static void test_status_pending_maps_to_error_io_pending_not_the_fallback(void)
{
    setup();
    /* THE REGRESSION TEST FOR A RECORDED HANG. STATUS_PENDING (0x103) returning the
     * generic 317 left another title in its first boot state forever, because its
     * resource loader only marks an object as loading when the answer is
     * ERROR_IO_PENDING. STATUS_PENDING is measured in this image's IO_STATUS_BLOCK
     * handling (guest_structs.h, 0x0037CC49), so the trap is reachable here. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(0x00000103u), 997u);
    CHECK(kernel_rtl_status_to_dos_error(0x00000103u)
          != KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 0u);
    teardown();
}

static void test_object_name_not_found_maps_to_error_file_not_found(void)
{
    setup();
    /* A missing disc file (\pak\overlay.pak) must read as file not found, not 317. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(0xC0000034u), 2u);
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 0u);
    teardown();
}

static void test_file_layer_statuses_map_to_nt_win32_table(void)
{
    static const struct { uint32_t status, error; } rows[] = {
        {0xC0000035u, 183u}, {0x80000006u, 18u}, {0xC0000034u, 2u}, {0xC000003Au, 3u},
        {0xC0000022u, 5u}, {0xC0000043u, 32u}, {0xC000007Fu, 112u}, {0xC0000011u, 38u},
        {0xC000000Du, 87u}, {0xC0000103u, 267u}, {0xC0000101u, 145u}, {0xC00000BAu, 5u},
        {0xC000009Au, 1450u}, {0xC000000Fu, 2u}, {0xC0000004u, 24u}, {0xC0000023u, 122u},
        {0xC0000024u, 6u}, {0xC0000010u, 1u}, {0xC000014Fu, 1005u}, {0xC0000033u, 123u},
        {0x80000005u, 234u},
    };
    setup();
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CHECK_EQ_U32(kernel_rtl_status_to_dos_error(rows[i].status), rows[i].error);
    }
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 0u);
    teardown();
}

static void test_an_unmapped_status_falls_back_and_is_counted_and_named(void)
{
    setup();
    /* 0xC000009A is the most common status in the guest's own code (37
     * occurrences) and this project cannot source its Win32 number, so it must take
     * the fallback rather than a guess. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(0xC0000364u),
                 KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 1u);
    CHECK_EQ_U32(kernel_rtl_last_unmapped(), 0xC0000364u);
    CHECK(captured_contains("no mapping"));
    teardown();
}

static void test_invalid_info_class_maps_to_invalid_parameter(void)
{
    setup();
    /* INFERRED from NT: STATUS_INVALID_INFO_CLASS -> ERROR_INVALID_PARAMETER. MUTATION: drop the row. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(0xC0000003u), 87u);
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 0u);
    teardown();
}

static void test_a_deliberately_unmapped_status_is_named_in_the_diagnostic(void)
{
    setup();
    /* Our own handlers return this one, so the report must say that it is
     * deliberately unmapped rather than leaving a bare hex value. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(STATUS_ACCESS_VIOLATION),
                 KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    CHECK(captured_contains("STATUS_ACCESS_VIOLATION"));
    CHECK(captured_contains("cannot source its Win32 number"));
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 1u);
    teardown();
}

static void test_the_hard_coded_literal_this_image_passes_takes_the_fallback(void)
{
    setup();
    /* recomp_0061.c:29785 pushes this constant straight into ordinal 301. It is not
     * a valid NTSTATUS at all (HRESULT-shaped, facility 7), so the fallback path is
     * exercised by the image itself and is not theoretical. */
    CHECK_EQ_U32(kernel_rtl_status_to_dos_error(0x80072747u),
                 KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    CHECK_EQ_U32(kernel_rtl_unmapped_count(), 1u);
    teardown();
}

static void test_the_fallback_is_nonzero_and_positive(void)
{
    setup();
    /* 16 of the 32 call sites do nothing with the result but compare it against
     * zero, and 4 of those use a SIGNED `jg`. So a fallback of 0 would report
     * success for a failure, and one with the high bit set would test as "not
     * greater than zero" and take the success branch too. Both would be silent. */
    uint32_t fallback = kernel_rtl_status_to_dos_error(0xC000DEADu);
    CHECK(fallback != 0u);
    CHECK((fallback & 0x80000000u) == 0u);
    CHECK((int32_t)fallback > 0);
    teardown();
}

static void test_a_failure_status_never_translates_to_success(void)
{
    setup();
    /* The one answer this function must never give: a nonzero NTSTATUS becoming
     * ERROR_SUCCESS tells the guest a failed operation worked. Swept over every
     * failure status nt_status.h knows about, mapped or not. */
    static const nt_status failures_to_check[] = {
        STATUS_UNSUCCESSFUL,        STATUS_NOT_IMPLEMENTED,
        STATUS_ACCESS_VIOLATION,    STATUS_INVALID_HANDLE,
        STATUS_INVALID_PARAMETER,   STATUS_NO_MEMORY,
        STATUS_CONFLICTING_ADDRESSES, STATUS_INVALID_PAGE_PROTECTION,
        STATUS_FREE_VM_NOT_AT_BASE, STATUS_MEMORY_NOT_ALLOCATED,
    };
    for (size_t i = 0u; i < sizeof(failures_to_check) / sizeof(failures_to_check[0]);
         i++) {
        CHECK(kernel_rtl_status_to_dos_error(failures_to_check[i]) != 0u);
    }
    teardown();
}

static void test_status_to_dos_error_through_the_dispatcher(void)
{
    setup();
    /* Through the real call boundary, so the argument is fetched from a guest stack
     * rather than passed as a C parameter. */
    uint32_t args[1] = {STATUS_INVALID_PARAMETER};
    CHECK_EQ_U32(call_ordinal(301u, args, 1u), 87u);

    uint32_t pending[1] = {0x00000103u};
    CHECK_EQ_U32(call_ordinal(301u, pending, 1u), 997u);
    teardown();
}

static void test_a_missing_status_argument_never_reports_success(void)
{
    setup();
    /* An unreadable argument must not yield ERROR_SUCCESS: that is the most
     * damaging possible lie from this particular function. */
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 4u), 0u, 0u, false, 0u, false};
    CHECK_EQ_U32(kernel_hle_call(301u, &narrow), KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    CHECK(captured_contains("could not read its status argument"));

    (void)kernel_hle_call(301u, NULL);
    CHECK(captured_contains("no argument frame"));
    teardown();
}


/* ---------------------------------------------------------------------------
 * Ordinals 269, 304 and 305 (startup-critical additions).
 *
 * Every expected value below is a literal. The calendar literals were produced by a
 * separate implementation (Python's datetime, ticks counted from 1601-01-01 by hand
 * arithmetic) and are NOT computed with the module's constants. Weekday is Sunday 0.
 * ------------------------------------------------------------------------- */

#define STARTUP_DATA 0x1000u

static unsigned char *startup_bytes(uint32_t offset, uint32_t length)
{
    unsigned char *base = (unsigned char *)kernel_guest_at(
        (kernel_guest_ptr)(scratch + STARTUP_DATA + offset), length);
    if (!base) {
        printf("  FATAL: startup scratch is not usable\n");
        exit(EXIT_FAILURE);
    }
    return base;
}

static kernel_guest_ptr startup_addr(uint32_t offset)
{
    return (kernel_guest_ptr)(scratch + STARTUP_DATA + offset);
}

static void put_word(uint32_t offset, uint32_t value)
{
    unsigned char *base = startup_bytes(offset, 4u);
    for (unsigned i = 0u; i < 4u; i++) {
        base[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    }
}

static uint32_t compare_via_dispatch(kernel_guest_ptr source, uint32_t length, uint32_t pattern)
{
    const uint32_t args[3] = {source, length, pattern};
    return call_ordinal(269u, args, 3u);
}

static void test_compare_full_match_with_the_measured_pattern(void)
{
    setup();
    for (unsigned i = 0u; i < 6u; i++) {
        put_word(i * 4u, 0xFEEEFEEEu);
    }
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 24u, 0xFEEEFEEEu), 24u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 4u, 0xFEEEFEEEu), 4u);
    CHECK_EQ_U32(kernel_rtl_compare_refused_count(), 0u);
    teardown();
}

static void test_compare_stops_at_the_first_mismatching_word(void)
{
    setup();
    for (unsigned i = 0u; i < 6u; i++) {
        put_word(i * 4u, 0xFEEEFEEEu);
    }
    put_word(12u, 0xFEEEFEEFu);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 24u, 0xFEEEFEEEu), 12u);
    put_word(0u, 0u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 24u, 0xFEEEFEEEu), 0u);
    /* A match after the mismatch does not count: it is a prefix length. */
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 24u, 0u), 4u);
    teardown();
}

static void test_compare_rounds_length_down_to_whole_words(void)
{
    setup();
    for (unsigned i = 0u; i < 4u; i++) {
        put_word(i * 4u, 0xFEEEFEEEu);
    }
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 11u, 0xFEEEFEEEu), 8u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 3u, 0xFEEEFEEEu), 0u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 0u, 0xFEEEFEEEu), 0u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 13u, 0xFEEEFEEEu), 12u);
    teardown();
}

static void test_compare_does_not_read_past_the_rounded_length(void)
{
    setup();
    put_word(0u, 0xFEEEFEEEu);
    put_word(4u, 0xFEEEFEEEu);
    /* The third word differs but lies beyond Length 9 (two whole words). */
    put_word(8u, 0u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 9u, 0xFEEEFEEEu), 8u);
    teardown();
}

static void test_compare_has_no_alignment_rule(void)
{
    setup();
    unsigned char *base = startup_bytes(0u, 16u);
    memset(base, 0, 16u);
    /* Pattern 0x04030201 stored little-endian at byte offset 1. */
    base[1] = 0x01u;
    base[2] = 0x02u;
    base[3] = 0x03u;
    base[4] = 0x04u;
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(1u), 4u, 0x04030201u), 4u);
    CHECK_EQ_U32(compare_via_dispatch(startup_addr(0u), 4u, 0x04030201u), 0u);
    teardown();
}

static void test_compare_unreadable_source_answers_zero_and_reports(void)
{
    setup();
    reset_capture();
    CHECK_EQ_U32(compare_via_dispatch(0u, 16u, 0xFEEEFEEEu), 0u);
    CHECK_EQ_U32(kernel_rtl_compare_refused_count(), 1u);
    CHECK(captured_contains("RtlCompareMemoryUlong"));
    CHECK(captured_contains("not readable"));
    teardown();
}

static void test_compare_a_frame_too_short_is_reported_and_answers_zero(void)
{
    setup();
    for (unsigned i = 0u; i < 4u; i++) {
        put_word(i * 4u, 0xFEEEFEEEu);
    }
    /* No frame at all, and a frame whose stack_limit cuts argument 2 off. */
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(269u, NULL), 0u);
    CHECK(captured_contains("RtlCompareMemoryUlong"));
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    const uint32_t two[2] = {startup_addr(0u), 16u};
    CHECK(kernel_frame_build(&frame, scratch, SCRATCH_OUT, two, 2u));
    frame.stack_limit = (kernel_guest_ptr)(frame.stack_ptr + 12u);
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(269u, &frame), 0u);
    CHECK(captured_contains("could not read"));
    teardown();
}

typedef struct {
    uint16_t year, month, day, hour, minute, second, millisecond, weekday;
    uint32_t ticks_high;
    uint32_t ticks_low;
} calendar_case;

/* ticks as high:low words of the 64-bit count. */
static const calendar_case calendar_cases[] = {
    {1601, 1, 1, 0, 0, 0, 0, 1, 0x00000000u, 0x00000000u},
    {1601, 12, 31, 23, 59, 59, 999, 1, 0x00011ED1u, 0x78C698F0u},
    {1970, 1, 1, 0, 0, 0, 0, 4, 0x019DB1DEu, 0xD53E8000u},
    {1999, 12, 31, 23, 59, 59, 999, 5, 0x01BF53EBu, 0x256D18F0u},
    {2000, 1, 1, 0, 0, 0, 0, 6, 0x01BF53EBu, 0x256D4000u},
    {2000, 2, 28, 0, 0, 0, 0, 1, 0x01BF817Eu, 0xC162C000u},
    {2000, 2, 29, 12, 34, 56, 789, 2, 0x01BF82B1u, 0x62C9FC50u},
    {2000, 3, 1, 0, 0, 0, 1, 3, 0x01BF8311u, 0x16366710u},
    {1900, 2, 28, 23, 0, 0, 0, 3, 0x014F6590u, 0x627B1800u},
    {1900, 3, 1, 0, 0, 0, 0, 4, 0x014F6598u, 0xC43F8000u},
    {2100, 2, 28, 0, 0, 0, 0, 0, 0x022F9EF7u, 0x13598000u},
    {2100, 3, 1, 0, 0, 0, 0, 1, 0x022F9FC0u, 0x3DC34000u},
    {2004, 2, 29, 1, 2, 3, 4, 0, 0x01C3FE5Fu, 0xA46693C0u},
    {2024, 12, 31, 23, 59, 59, 999, 2, 0x01DB5BE0u, 0x19BA18F0u},
    {9999, 12, 31, 23, 59, 59, 999, 5, 0x24C85A5Eu, 0xD1C018F0u},
    {2001, 9, 9, 1, 46, 40, 0, 0, 0x01C138D1u, 0x44FF8000u},
    {1752, 9, 14, 0, 0, 0, 0, 4, 0x00AA13CBu, 0xC8440000u},
};
#define CALENDAR_CASE_COUNT (sizeof(calendar_cases) / sizeof(calendar_cases[0]))

#define TIME_AT 0x40u
#define FIELDS_AT 0x80u

static void put_fields(const uint16_t fields[8])
{
    unsigned char *base = startup_bytes(FIELDS_AT, 16u);
    for (unsigned i = 0u; i < 8u; i++) {
        base[i * 2u] = (unsigned char)(fields[i] & 0xFFu);
        base[i * 2u + 1u] = (unsigned char)(fields[i] >> 8);
    }
}

static uint16_t field_at(unsigned index)
{
    const unsigned char *base = startup_bytes(FIELDS_AT, 16u);
    return (uint16_t)((uint16_t)base[index * 2u] | (uint16_t)((uint16_t)base[index * 2u + 1u] << 8));
}

static void put_time(uint32_t high, uint32_t low)
{
    put_word(TIME_AT, low);
    put_word(TIME_AT + 4u, high);
}

static uint32_t time_low(void)
{
    const unsigned char *base = startup_bytes(TIME_AT, 8u);
    return (uint32_t)base[0] | ((uint32_t)base[1] << 8) | ((uint32_t)base[2] << 16) |
           ((uint32_t)base[3] << 24);
}

static uint32_t time_high(void)
{
    const unsigned char *base = startup_bytes(TIME_AT + 4u, 4u);
    return (uint32_t)base[0] | ((uint32_t)base[1] << 8) | ((uint32_t)base[2] << 16) |
           ((uint32_t)base[3] << 24);
}

static void to_fields_via_dispatch(void)
{
    const uint32_t args[2] = {startup_addr(TIME_AT), startup_addr(FIELDS_AT)};
    (void)call_ordinal(305u, args, 2u);
}

static uint32_t to_time_via_dispatch(void)
{
    const uint32_t args[2] = {startup_addr(FIELDS_AT), startup_addr(TIME_AT)};
    return call_ordinal(304u, args, 2u);
}

static void test_time_to_fields_matches_the_calendar_literals(void)
{
    setup();
    for (unsigned i = 0u; i < CALENDAR_CASE_COUNT; i++) {
        const calendar_case *c = &calendar_cases[i];
        memset(startup_bytes(FIELDS_AT, 16u), 0xEE, 16u);
        put_time(c->ticks_high, c->ticks_low);
        to_fields_via_dispatch();
        CHECK_EQ_U32(field_at(0), c->year);
        CHECK_EQ_U32(field_at(1), c->month);
        CHECK_EQ_U32(field_at(2), c->day);
        CHECK_EQ_U32(field_at(3), c->hour);
        CHECK_EQ_U32(field_at(4), c->minute);
        CHECK_EQ_U32(field_at(5), c->second);
        CHECK_EQ_U32(field_at(6), c->millisecond);
        CHECK_EQ_U32(field_at(7), c->weekday);
    }
    teardown();
}

static void test_fields_to_time_matches_the_calendar_literals(void)
{
    setup();
    for (unsigned i = 0u; i < CALENDAR_CASE_COUNT; i++) {
        const calendar_case *c = &calendar_cases[i];
        /* Weekday is garbage on purpose: the guest never initialises it. */
        const uint16_t fields[8] = {c->year, c->month, c->day, c->hour, c->minute,
                                    c->second, c->millisecond, 0x7777u};
        put_fields(fields);
        put_time(0xDEADBEEFu, 0xDEADBEEFu);
        CHECK_EQ_U32(to_time_via_dispatch(), 1u);
        CHECK_EQ_U32(time_high(), c->ticks_high);
        CHECK_EQ_U32(time_low(), c->ticks_low);
    }
    teardown();
}

static void test_sub_millisecond_ticks_truncate_and_do_not_carry(void)
{
    setup();
    /* 1970-01-01 plus 9999 ticks (0.9999 ms) is still millisecond 0. */
    put_time(0x019DB1DEu, 0xD53E8000u + 9999u);
    to_fields_via_dispatch();
    CHECK_EQ_U32(field_at(6), 0u);
    CHECK_EQ_U32(field_at(5), 0u);
    put_time(0x019DB1DEu, 0xD53E8000u + 10000u);
    to_fields_via_dispatch();
    CHECK_EQ_U32(field_at(6), 1u);
    /* The last tick of a day stays on that day. */
    put_time(0x019DB1DEu, 0xD53E8000u - 1u);
    to_fields_via_dispatch();
    CHECK_EQ_U32(field_at(0), 1969u);
    CHECK_EQ_U32(field_at(1), 12u);
    CHECK_EQ_U32(field_at(2), 31u);
    CHECK_EQ_U32(field_at(3), 23u);
    CHECK_EQ_U32(field_at(4), 59u);
    CHECK_EQ_U32(field_at(5), 59u);
    CHECK_EQ_U32(field_at(6), 999u);
    CHECK_EQ_U32(field_at(7), 3u);
    teardown();
}

static void test_a_negative_time_is_refused_and_leaves_the_fields_alone(void)
{
    setup();
    memset(startup_bytes(FIELDS_AT, 16u), 0xEE, 16u);
    put_time(0x80000000u, 0u);
    reset_capture();
    to_fields_via_dispatch();
    CHECK_EQ_U32(field_at(0), 0xEEEEu);
    CHECK_EQ_U32(field_at(7), 0xEEEEu);
    CHECK_EQ_U32(kernel_rtl_time_refused_count(), 1u);
    CHECK(captured_contains("RtlTimeToTimeFields"));
    CHECK(captured_contains("REFUSED"));
    teardown();
}

static void test_a_year_past_cshort_is_refused(void)
{
    setup();
    memset(startup_bytes(FIELDS_AT, 16u), 0xEE, 16u);
    /* 0x7FFFFFFFFFFFFFFF ticks is year 30828, which fits, so it converts. */
    put_time(0x7FFFFFFFu, 0xFFFFFFFFu);
    to_fields_via_dispatch();
    CHECK_EQ_U32(field_at(0), 30828u);
    CHECK_EQ_U32(kernel_rtl_time_refused_count(), 0u);
    teardown();
}

static void test_time_to_fields_without_a_frame_is_reported(void)
{
    setup();
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(305u, NULL), 0u);
    CHECK(captured_contains("RtlTimeToTimeFields"));
    CHECK_EQ_U32(kernel_rtl_time_refused_count(), 1u);
    teardown();
}

static void test_fields_round_trip_for_every_month_boundary(void)
{
    setup();
    static const uint16_t lengths_leap[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const uint16_t lengths_common[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const uint16_t years[2] = {2000, 2001};
    for (unsigned y = 0u; y < 2u; y++) {
        const uint16_t *lengths = y == 0u ? lengths_leap : lengths_common;
        for (uint16_t month = 1u; month <= 12u; month++) {
            const uint16_t last = lengths[month - 1u];
            const uint16_t fields[8] = {years[y], month, last, 23, 59, 59, 999, 0};
            put_fields(fields);
            CHECK_EQ_U32(to_time_via_dispatch(), 1u);
            to_fields_via_dispatch();
            CHECK_EQ_U32(field_at(0), years[y]);
            CHECK_EQ_U32(field_at(1), month);
            CHECK_EQ_U32(field_at(2), last);
            /* One tick of 100ns after the last millisecond rolls into the next day. */
            const uint16_t over[8] = {years[y], month, (uint16_t)(last + 1u), 0, 0, 0, 0, 0};
            put_fields(over);
            put_time(0xDEADBEEFu, 0xDEADBEEFu);
            CHECK_EQ_U32(to_time_via_dispatch(), 0u);
            CHECK_EQ_U32(time_low(), 0xDEADBEEFu);
        }
    }
    teardown();
}

static void test_each_invalid_field_is_rejected_on_its_own(void)
{
    setup();
    /* A valid base: 2000-06-15 12:30:45.500. Each row breaks exactly one field. */
    static const struct {
        unsigned index;
        uint16_t value;
    } bad[] = {
        {0, 1600}, {0, 0}, {0, 0xFFFFu}, {1, 0}, {1, 13}, {1, 0xFFFFu}, {2, 0}, {2, 31},
        {2, 0xFFFFu}, {3, 24}, {3, 0xFFFFu}, {4, 60}, {4, 0xFFFFu}, {5, 60}, {5, 0xFFFFu},
        {6, 1000}, {6, 0xFFFFu},
    };
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint16_t fields[8] = {2000, 6, 15, 12, 30, 45, 500, 3};
        fields[bad[i].index] = bad[i].value;
        put_fields(fields);
        put_time(0xDEADBEEFu, 0xCAFEF00Du);
        reset_capture();
        CHECK_EQ_U32(to_time_via_dispatch(), 0u);
        CHECK_EQ_U32(time_high(), 0xDEADBEEFu);
        CHECK_EQ_U32(time_low(), 0xCAFEF00Du);
        CHECK(captured_contains("REFUSED"));
    }
    CHECK_EQ_U32(kernel_rtl_time_refused_count(), sizeof(bad) / sizeof(bad[0]));
    /* The edges just inside are accepted: year 1601, hour 23, minute and second 59. */
    const uint16_t edge[8] = {1601, 1, 1, 23, 59, 59, 999, 0};
    put_fields(edge);
    CHECK_EQ_U32(to_time_via_dispatch(), 1u);
    CHECK_EQ_U32(time_high(), 0xC9u);
    CHECK_EQ_U32(time_low(), 0x2A6998F0u);
    teardown();
}

static void test_the_leap_rule_has_all_three_clauses(void)
{
    setup();
    /* 29 February: 2000 yes (400), 1900 no (100), 2100 no, 2004 yes (4), 2001 no. */
    static const struct {
        uint16_t year;
        uint32_t valid;
    } cases[] = {{2000, 1}, {1900, 0}, {2100, 0}, {2004, 1}, {2001, 0}, {2400, 1}, {1604, 1}};
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint16_t fields[8] = {cases[i].year, 2, 29, 0, 0, 0, 0, 0};
        put_fields(fields);
        CHECK_EQ_U32(to_time_via_dispatch(), cases[i].valid);
    }
    teardown();
}

static void test_fields_to_time_with_unreadable_arguments_answers_false(void)
{
    setup();
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(304u, NULL), 0u);
    CHECK(captured_contains("RtlTimeFieldsToTime"));
    const uint32_t args[2] = {0u, startup_addr(TIME_AT)};
    CHECK_EQ_U32(call_ordinal(304u, args, 2u), 0u);
    const uint32_t args2[2] = {startup_addr(FIELDS_AT), 0u};
    CHECK_EQ_U32(call_ordinal(304u, args2, 2u), 0u);
    CHECK_EQ_U32(kernel_rtl_time_refused_count(), 3u);
    teardown();
}

static void test_argument_order_input_is_arg0_for_304_and_arg1_is_output(void)
{
    setup();
    const uint16_t fields[8] = {1970, 1, 1, 0, 0, 0, 0, 0};
    put_fields(fields);
    put_time(0u, 0u);
    /* Swapped arguments would read the time as fields and write the fields as time. */
    const uint32_t args[2] = {startup_addr(FIELDS_AT), startup_addr(TIME_AT)};
    CHECK_EQ_U32(call_ordinal(304u, args, 2u), 1u);
    CHECK_EQ_U32(time_high(), 0x019DB1DEu);
    CHECK_EQ_U32(field_at(0), 1970u);
    teardown();
}

static void run_startup_ordinal_tests(void)
{
    test_compare_full_match_with_the_measured_pattern();
    test_compare_stops_at_the_first_mismatching_word();
    test_compare_rounds_length_down_to_whole_words();
    test_compare_does_not_read_past_the_rounded_length();
    test_compare_has_no_alignment_rule();
    test_compare_unreadable_source_answers_zero_and_reports();
    test_compare_a_frame_too_short_is_reported_and_answers_zero();
    test_time_to_fields_matches_the_calendar_literals();
    test_fields_to_time_matches_the_calendar_literals();
    test_sub_millisecond_ticks_truncate_and_do_not_carry();
    test_a_negative_time_is_refused_and_leaves_the_fields_alone();
    test_a_year_past_cshort_is_refused();
    test_time_to_fields_without_a_frame_is_reported();
    test_fields_round_trip_for_every_month_boundary();
    test_each_invalid_field_is_rejected_on_its_own();
    test_the_leap_rule_has_all_three_clauses();
    test_fields_to_time_with_unreadable_arguments_answers_false();
    test_argument_order_input_is_arg0_for_304_and_arg1_is_output();
}

static void test_registration_makes_all_seven_ordinals_implemented(void)
{
    setup();
    static const unsigned ordinals[] = {269u, 279u, 289u, 301u, 302u, 304u, 305u};
    CHECK_EQ_U32(kernel_hle_implemented_count(ordinals, 7u), 7u);
    /* Implemented AND the entry's handler is what the dispatcher runs, not a stub. */
    for (unsigned i = 0u; i < 7u; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED &&
              entry->handler != NULL);
    }
    teardown();
}

/* ---------------------------------------------------------------------------
 * RtlEqualString (ordinal 279).
 *
 * The risk here is NOT a wrong offset -- the operands are read, not written. It is a
 * wrong RULE, and a wrong rule produces a plausible answer. The single caller
 * (`_XGetSectionHandleA@4`) searches the XBE section table, so a comparison that is too
 * loose hands the title the WRONG SECTION and one that is too strict reports a section
 * that exists as missing. Both look like working code. So every assertion below pins one
 * rule that could be wrong on its own.
 * ------------------------------------------------------------------------- */

static void test_identical_strings_compare_equal(void)
{
    setup();
    put_pair("$$XTINFO", "$$XTINFO");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 1u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    /* No diagnostic at all on the ordinary path. A handler that logged here would bury
     * the non-ASCII warning that is the only line worth reading. */
    CHECK_EQ_U32(captured_len, 0u);
    teardown();
}

static void test_same_length_different_bytes_compare_unequal(void)
{
    setup();
    put_pair("$$XTINFO", "$$XSINFO");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    teardown();
}

/* A PREFIX IS NOT A MATCH, in either operand order.
 *
 * This is the assertion that kills the most dangerous wrong rule. An implementation that
 * compared `min(Length1, Length2)` bytes -- or that used `<` where it needed `!=` on the
 * length -- would call these equal, and at the real call site that means
 * `_XGetSectionHandleA@4` returning the handle of "$$XTINFO" when the title asked for
 * "$$XTINFOEXTRA". Both orders are tested because a one-sided comparison is only wrong
 * in one direction and would otherwise pass half the time. */
static void test_a_prefix_is_not_a_match_in_either_order(void)
{
    setup();
    put_pair("$$XTINFO", "$$XTINFOEXTRA");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    /* Swapped: the longer operand first. */
    CHECK_EQ_U32(equal_via_dispatch(PAIR_B_DESC, PAIR_A_DESC, 0u), 0u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_B_DESC, PAIR_A_DESC, 1u), 0u);
    teardown();
}

/* CaseInSensitive IS READ, AND IT IS READ FROM ARGUMENT 2.
 *
 * The case-SENSITIVE leg does double duty. It proves folding is conditional, and it
 * proves the flag is fetched from slot 2 rather than slot 0 or 1 -- those two hold
 * guest ADDRESSES, which are nonzero, so a handler reading the flag from either would
 * fold unconditionally and this CHECK would fail. The case-insensitive leg then proves
 * the folding works in both directions at once, since the operands differ in case on
 * letters in both. */
static void test_case_folding_happens_only_when_asked(void)
{
    setup();
    put_pair("aBcZ", "AbCz");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    /* Any nonzero is TRUE on hardware, not just 1 -- the guest's own literal is 1 but
     * a BOOLEAN is a byte and nothing restricts it. */
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0x100u), 1u);
    teardown();
}

/* The ENDS of the alphabet, because an off-by-one in the fold range is invisible in the
 * middle of it. 'a' and 'z' are the two bytes a `>` where `>=` belongs, or a `<` where
 * `<=` belongs, would get wrong. */
static void test_the_fold_range_includes_both_its_endpoints(void)
{
    setup();
    put_pair("a", "A");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    put_pair("z", "Z");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    /* And it stops there. '`' is 'a'-1 and '{' is 'z'+1; neither has a case partner, so
     * folding either would make it collide with '@' and ']'. */
    put_pair("`", "@");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    put_pair("{", "[");
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    teardown();
}

/* COUNTED, NOT NUL-TERMINATED. Two assertions, and they are opposite failures of the
 * same confusion: bytes past Length must be ignored even when they differ, and a NUL
 * INSIDE Length must be compared like any other byte. A `strcmp` or `strncmp`
 * implementation fails one or the other. */
static void test_only_length_bytes_are_compared(void)
{
    setup();
    /* Length 2 over four-byte buffers whose tails disagree. */
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "ABZZ", 2u, 5u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "ABYY", 2u, 5u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 1u);
    teardown();
}

static void test_an_embedded_nul_is_an_ordinary_byte(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "A\0B", 3u, 4u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "A\0C", 3u, 4u);
    /* A C-string comparison stops at the NUL and calls these equal. */
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "A\0B", 3u, 4u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 1u);
    teardown();
}

/* MaximumLength TAKES NO PART. It is set to deliberately absurd and unequal values here;
 * a handler that compared it, or that bounded the scan by it, would answer differently. */
static void test_maximum_length_does_not_affect_the_answer(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "SAME", 4u, 0xFFFFu);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "SAME", 4u, 1u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 1u);
    teardown();
}

/* TWO EMPTY DESCRIPTORS ARE EQUAL AND NEITHER BUFFER IS TOUCHED.
 *
 * Buffer is NULL in both, which is exactly the shape ordinal 289 writes for a NULL
 * source. An implementation that mapped the buffers before checking the length would
 * refuse this and answer FALSE, and the silence of the log is what proves it did not. */
static void test_two_empty_strings_are_equal_without_reading_a_buffer(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, NULL, 0u, 0u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, NULL, 0u, 0u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 1u);
    CHECK_EQ_U32(captured_len, 0u);
    /* And an empty string is not equal to a non-empty one. */
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "X", 1u, 2u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    teardown();
}

/* THE UNMEASURED PATH IS COUNTED AND NAMED, which is the whole point of having a counter.
 *
 * 0xE0 and 0xC0 are latin1 lower and upper 'a-grave'. On hardware with a latin1 OEM page
 * a case-insensitive comparison would call them EQUAL. We do not know that page, so we
 * answer FALSE -- and the assertion that matters is not the answer, it is that the run
 * SAYS SO rather than quietly approximating. */
static void test_a_byte_above_ascii_is_counted_and_named_not_folded(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "\xE0", 1u, 2u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "\xC0", 1u, 2u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 1u);
    CHECK_EQ_U32(kernel_rtl_last_nonascii_fold(), 0xE0u);
    CHECK(captured_contains("above 0x7F"));
    CHECK(captured_contains("NOT measured"));

    /* Case-SENSITIVE comparison of the same bytes does no folding at all, so there is
     * nothing unmeasured about it and nothing to count. A handler that counted
     * unconditionally would inflate the one number that is supposed to mean "go and
     * measure the code page". */
    reset_capture();
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 1u);
    CHECK_EQ_U32(captured_len, 0u);
    teardown();
}

/* 0x80 EXACTLY, the boundary. A `> 0x80` where `>= 0x80` belongs leaves this one byte
 * silently unflagged, and it is the byte most likely to appear first. */
static void test_byte_0x80_is_on_the_unmeasured_side_of_the_boundary(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "\x80", 1u, 2u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "\x80", 1u, 2u);
    /* Equal bytes, so the ANSWER is TRUE -- and it still has to be counted, because the
     * folding we could not perform is what makes the answer uncertain, not the result. */
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 1u);
    CHECK_EQ_U32(kernel_rtl_last_nonascii_fold(), 0x80u);
    CHECK(captured_contains("TRUE"));
    /* 0x7F is the last ASCII byte and must NOT be counted. */
    reset_capture();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "\x7F", 1u, 2u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "\x7F", 1u, 2u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 1u);
    CHECK_EQ_U32(captured_len, 0u);
    teardown();
}

/* A MISMATCH DOES NOT STOP THE SCAN, which is a behaviour and not an accident.
 *
 * The high byte sits AFTER the first difference here. An implementation that broke out of
 * the loop on the first mismatch would answer FALSE correctly and never count the
 * unmeasured byte -- so the counter would depend on where in the string the difference
 * happened to be, and the one signal it exists to give would be lost precisely in the
 * cases where the strings are mostly different. */
static void test_a_high_byte_after_the_first_difference_is_still_counted(void)
{
    setup();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, "A\xE0", 2u, 3u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "B\xE0", 2u, 3u);
    CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 0u);
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 1u);
    CHECK_EQ_U32(kernel_rtl_last_nonascii_fold(), 0xE0u);
    teardown();
}

/* A REFUSAL IS NOT A MISMATCH, and only the helper can say which.
 *
 * The ABI has one byte to answer in, so the handler must return FALSE for both. The
 * exported helper returns them separately, and a caller that cannot tell them apart
 * cannot tell a broken pointer from two different strings. */
static void test_an_unusable_descriptor_is_refused_rather_than_answered(void)
{
    setup();
    put_pair("SAME", "SAME");
    bool equal = true;
    /* Address 0 is rejected by kernel_guest_at, so this is a usable stand-in for a
     * pointer the guest could really pass. */
    CHECK(!kernel_rtl_strings_equal(0u, (uint32_t)at(PAIR_B_DESC), false, &equal));
    CHECK(captured_contains("String1"));
    reset_capture();
    CHECK(!kernel_rtl_strings_equal((uint32_t)at(PAIR_A_DESC), 0u, false, &equal));
    CHECK(captured_contains("String2"));
    reset_capture();
    CHECK(!kernel_rtl_strings_equal(0u, 0u, false, &equal));
    CHECK(captured_contains("neither operand"));

    /* Through the dispatcher the same refusal becomes FALSE, which at the real call site
     * means "not this section" and so makes the search continue. */
    reset_capture();
    const uint32_t args[3] = {0u, (uint32_t)at(PAIR_B_DESC), 0u};
    CHECK_EQ_U32(call_ordinal(279u, args, 3u), 0u);

    /* A non-empty descriptor whose Buffer is NULL is refused too, and named differently
     * -- the descriptor was readable, the text was not. */
    reset_capture();
    put_descriptor(PAIR_A_DESC, PAIR_A_TEXT, NULL, 4u, 5u);
    put_descriptor(PAIR_B_DESC, PAIR_B_TEXT, "SAME", 4u, 5u);
    equal = true;
    CHECK(!kernel_rtl_strings_equal((uint32_t)at(PAIR_A_DESC), (uint32_t)at(PAIR_B_DESC),
                                    false, &equal));
    CHECK(captured_contains("not usable guest memory"));
    teardown();
}

static void test_equal_string_without_a_frame_is_reported(void)
{
    setup();
    CHECK_EQ_U32(kernel_hle_call(279u, NULL), 0u);
    CHECK(captured_contains("no argument frame"));
    teardown();
}

static void test_a_frame_too_short_for_three_arguments_is_reported(void)
{
    setup();
    put_pair("SAME", "SAME");
    /* Two slots where three are needed. The frame's stack_limit must turn the third
     * fetch into a reported error rather than a read of whatever is above it. */
    const uint32_t args[2] = {(uint32_t)at(PAIR_A_DESC), (uint32_t)at(PAIR_B_DESC)};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, 3u * 4u, args, 2u)) {
        printf("  FATAL: could not build a short frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(279u, &frame), 0u);
    CHECK(captured_contains("three arguments"));
    teardown();
}

/* THE REAL CASE, end to end: the three names the boot actually looks up, compared
 * against a section table that holds them in a different case. The boot trace matches at
 * iterations 13, 14 and 15 of the scan, so these are the comparisons that have to come
 * out right for the title to find its sections -- and the lengths differ between the
 * three names, so a length rule that was too loose would match the wrong one. */
static void test_the_three_section_names_the_boot_looks_up(void)
{
    setup();
    static const char *const names[] = {"$$XTINFO", "$$XTIMAGE", "$$XSIMAGE"};
    static const char *const lowered[] = {"$$xtinfo", "$$xtimage", "$$xsimage"};
    for (unsigned i = 0u; i < 3u; i++) {
        put_pair(names[i], lowered[i]);
        CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), 1u);
        CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 0u), 0u);
        /* And each name matches only itself across the set. */
        for (unsigned j = 0u; j < 3u; j++) {
            put_pair(names[i], names[j]);
            CHECK_EQ_U32(equal_via_dispatch(PAIR_A_DESC, PAIR_B_DESC, 1u), i == j ? 1u : 0u);
        }
    }
    CHECK_EQ_U32(kernel_rtl_nonascii_fold_count(), 0u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * RtlRaiseException (ordinal 302).
 *
 * The handler has no dispatch model to hand the record to, so it decodes it, reports it
 * through kernel_hle_fatal and stops. The risk is a message that names the wrong field,
 * so the guest records are built here at the raw EXCEPTION_RECORD offsets and the
 * literals the guest actually raises are pinned. The capturing hook RETURNS, which makes
 * the post-hook return value (the ExceptionCode, or 0 when unreadable) observable.
 *
 * Frame refusal convention for this export: never silent. A frame that cannot supply the
 * pointer goes fatal and says so, because refusing quietly would let the guest run on.
 * ------------------------------------------------------------------------- */

static unsigned fatal_calls;
static unsigned fatal_ordinal;
static char fatal_detail_copy[256];

static void capture_fatal(unsigned ordinal, const char *detail)
{
    fatal_calls++;
    fatal_ordinal = ordinal;
    (void)snprintf(fatal_detail_copy, sizeof(fatal_detail_copy), "%s", detail);
}

static void install_fatal_capture(void)
{
    fatal_calls = 0u;
    fatal_ordinal = 0u;
    fatal_detail_copy[0] = '\0';
    kernel_hle_set_fatal(capture_fatal);
}

#define RECORD_AT 0x18000u
/* An address no test maps, nonzero and outside the one scratch region. */
#define RAISE_UNMAPPED_AT 0x7FF00010u

/* Lay out an EXCEPTION_RECORD at RAW offsets: Code +0, Flags +4, Record +8, Address +0xC,
 * NumberParameters +0x10, Information[] +0x14. Returns its guest address. */
static kernel_guest_ptr put_exception_record(uint32_t code, uint32_t flags,
                                             uint32_t address, uint32_t parameters,
                                             const uint32_t *info, unsigned info_count)
{
    const kernel_guest_ptr record = at(RECORD_AT);
    const uint32_t header[5] = {code, flags, 0u, address, parameters};
    if (!kernel_guest_write_bytes(record, header, sizeof(header)) ||
        (info_count > 0u &&
         !kernel_guest_write_bytes(record + 0x14u, info, info_count * 4u))) {
        printf("  FATAL: could not write the exception record\n");
        exit(EXIT_FAILURE);
    }
    return record;
}

static uint32_t raise_via_dispatch(uint32_t record)
{
    const uint32_t args[1] = {record};
    return call_ordinal(302u, args, 1u);
}

/* Replay of __CxxThrowException through sub_0037FD08 (site 0x0037FD53): code 0xE06D7363,
 * flags 1 (non-continuable), 3 params, Info[0] = 0x19930520, ExceptionAddress the literal
 * 0x37FD08. The literals are pinned so a handler that swaps code and flags, or prints no
 * Info, cannot pass. */
static void test_raise_replays_the_cxx_throw_shape(void)
{
    setup();
    install_fatal_capture();
    const uint32_t info[3] = {0x19930520u, 0x0012FF00u, 0x004A1000u};
    const kernel_guest_ptr record = put_exception_record(0xE06D7363u, 1u, 0x0037FD08u, 3u, info, 3u);
    CHECK_EQ_U32(raise_via_dispatch(record), 0xE06D7363u);
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK_EQ_U32(fatal_ordinal, 302u);
    CHECK(strstr(fatal_detail_copy, "code 0xE06D7363") != NULL);
    CHECK(strstr(fatal_detail_copy, "flags 0x1,") != NULL);
    CHECK(strstr(fatal_detail_copy, "address 0x0037FD08") != NULL);
    CHECK(strstr(fatal_detail_copy, "3 params: 0x19930520") != NULL);
    CHECK(strstr(fatal_detail_copy, "no dispatch model") != NULL);
    CHECK(captured_contains("FATAL ordinal 302"));
    CHECK(captured_contains("E06D7363"));
    teardown();
}

/* Replay of RtlAllocateHeap (site 0x00383D8F) under HEAP_GENERATE_EXCEPTIONS:
 * STATUS_NO_MEMORY 0xC0000017, flags 0, one param holding the rounded size. */
static void test_raise_replays_the_heap_no_memory_shape(void)
{
    setup();
    install_fatal_capture();
    const uint32_t info[1] = {0x00200000u};
    const kernel_guest_ptr record = put_exception_record(0xC0000017u, 0u, 0x00383D8Fu, 1u, info, 1u);
    CHECK_EQ_U32(raise_via_dispatch(record), 0xC0000017u);
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK_EQ_U32(fatal_ordinal, 302u);
    CHECK(strstr(fatal_detail_copy, "code 0xC0000017") != NULL);
    CHECK(strstr(fatal_detail_copy, "flags 0x0,") != NULL);
    CHECK(strstr(fatal_detail_copy, "1 params: 0x00200000") != NULL);
    teardown();
}

/* The guest caps NumberParameters at 15. A larger count must be capped in the message,
 * and only four Info dwords are shown, the rest elided. */
static void test_raise_caps_the_parameter_count_and_elides_the_tail(void)
{
    setup();
    install_fatal_capture();
    const uint32_t info[6] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                              0x55555555u, 0x66666666u};
    const kernel_guest_ptr record = put_exception_record(0xC000008Eu, 0u, 0x0037FD08u, 99u, info, 6u);
    CHECK_EQ_U32(raise_via_dispatch(record), 0xC000008Eu);
    CHECK(strstr(fatal_detail_copy, "15 params: 0x11111111 0x22222222 0x33333333 0x44444444 ...") != NULL);
    CHECK(strstr(fatal_detail_copy, "0x55555555") == NULL);
    teardown();
}

static void test_raise_with_an_unmapped_record_still_goes_fatal(void)
{
    setup();
    install_fatal_capture();
    CHECK_EQ_U32(raise_via_dispatch(RAISE_UNMAPPED_AT), 0u);
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK_EQ_U32(fatal_ordinal, 302u);
    CHECK(strstr(fatal_detail_copy, "0x7FF00010 unreadable") != NULL);

    install_fatal_capture();
    CHECK_EQ_U32(raise_via_dispatch(0u), 0u); /* a NULL record pointer */
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK(strstr(fatal_detail_copy, "unreadable") != NULL);
    teardown();
}

static void test_raise_with_an_unreadable_frame_still_goes_fatal(void)
{
    setup();
    install_fatal_capture();
    CHECK_EQ_U32(kernel_hle_call(302u, NULL), 0u);
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK_EQ_U32(fatal_ordinal, 302u);
    CHECK(strstr(fatal_detail_copy, "record pointer unreadable") != NULL);

    install_fatal_capture();
    const uint32_t none[1] = {0u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, 1u * 4u, none, 0u)) {
        printf("  FATAL: could not build a short frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(302u, &frame), 0u);
    CHECK_EQ_U32(fatal_calls, 1u);
    CHECK(strstr(fatal_detail_copy, "record pointer unreadable") != NULL);
    teardown();
}

int main(void)
{
    printf("kernel RTL HLE tests\n");

    test_ansi_string_fields_land_at_the_measured_offsets();
    test_maximum_length_is_always_one_more_than_length();
    test_buffer_aliases_the_source_and_is_not_a_copy();
    test_an_eight_byte_descriptor_does_not_touch_a_ninth_byte();
    test_null_source_empties_the_descriptor_and_reports();
    test_null_destination_is_refused_and_reported();
    test_an_unterminated_source_is_clamped_and_reported();
    test_init_ansi_string_without_a_frame_is_reported();
    test_a_frame_too_short_for_two_arguments_is_reported();

    test_the_sourced_mappings();
    test_status_pending_maps_to_error_io_pending_not_the_fallback();
    test_object_name_not_found_maps_to_error_file_not_found();
    test_file_layer_statuses_map_to_nt_win32_table();
    test_an_unmapped_status_falls_back_and_is_counted_and_named();
    test_invalid_info_class_maps_to_invalid_parameter();
    test_a_deliberately_unmapped_status_is_named_in_the_diagnostic();
    test_the_hard_coded_literal_this_image_passes_takes_the_fallback();
    test_the_fallback_is_nonzero_and_positive();
    test_a_failure_status_never_translates_to_success();
    test_status_to_dos_error_through_the_dispatcher();
    test_a_missing_status_argument_never_reports_success();

    test_identical_strings_compare_equal();
    test_same_length_different_bytes_compare_unequal();
    test_a_prefix_is_not_a_match_in_either_order();
    test_case_folding_happens_only_when_asked();
    test_the_fold_range_includes_both_its_endpoints();
    test_only_length_bytes_are_compared();
    test_an_embedded_nul_is_an_ordinary_byte();
    test_maximum_length_does_not_affect_the_answer();
    test_two_empty_strings_are_equal_without_reading_a_buffer();
    test_a_byte_above_ascii_is_counted_and_named_not_folded();
    test_byte_0x80_is_on_the_unmeasured_side_of_the_boundary();
    test_a_high_byte_after_the_first_difference_is_still_counted();
    test_an_unusable_descriptor_is_refused_rather_than_answered();
    test_equal_string_without_a_frame_is_reported();
    test_a_frame_too_short_for_three_arguments_is_reported();
    test_the_three_section_names_the_boot_looks_up();

    test_registration_makes_all_seven_ordinals_implemented();
    test_raise_replays_the_cxx_throw_shape();
    test_raise_replays_the_heap_no_memory_shape();
    test_raise_caps_the_parameter_count_and_elides_the_tail();
    test_raise_with_an_unmapped_record_still_goes_fatal();
    test_raise_with_an_unreadable_frame_still_goes_fatal();
    run_startup_ordinal_tests();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
