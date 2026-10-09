/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NtOpenFile (ordinal 202).
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. THE ARITY. 6 stack arguments. The measured table has the right number but
 *      flags it non-unanimous, so the count rests on a hand reading of the call
 *      sites; our handler is the __stdcall callee, so a wrong count desyncs the
 *      guest's esp permanently and the damage appears arbitrarily far away.
 *   2. THE ALIASED HANDLE OUT-PARAMETER. At the site the run actually reaches,
 *      argument 0 is `lea eax,[ebp+8]` and OBJECT_ATTRIBUTES.object_name is
 *      `[ebp+8]` -- the handle is written into the very slot that holds the pointer
 *      to the name. A handler that wrote the handle before reading the name would
 *      destroy the name, and the symptom would be a garbage path with nothing
 *      pointing at the cause. `test_the_handle_slot_may_alias_...` is that case.
 *   3. THE 16-BIT OBJECT_STRING LENGTH. `guest_structs.h` derives it from word-sized
 *      reads, word-sized writes and a word-sized clear. Reading a dword there picks
 *      up MaximumLength in the high half, which for a short path is a nonsense
 *      length in the hundreds of thousands.
 *   4. SILENTLY SUCCEEDING. There is no volume behind any of these names. An open
 *      that succeeds anyway must say so, or every later read is attributed to the
 *      lift.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. All three guest structures are
 * built in scratch guest memory at the offsets `guest_structs.h` derives, so the
 * suite reads and writes the same bytes the guest does with no image address
 * resolved and no generated C linked.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "kernel_file.h"

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
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
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,  \
                   (unsigned)a_, (unsigned)e_);                                           \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static char captured[32768];
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

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define ORD_NT_OPEN_FILE 202u

/* The three literals the reached call site at 0x00380D43 passes. Used throughout so
 * the suite exercises what the guest actually sends rather than round numbers. */
#define MEASURED_ACCESS 0x00100001u
#define MEASURED_SHARE 3u
#define MEASURED_OPTIONS 0x00800021u
/* The attributes value at all 31 measured inline OBJECT_ATTRIBUTES builders;
 * the shared named-event/mutex helper stores 0x80 instead. */
#define MEASURED_ATTRIBUTES GUEST_OBJ_ATTRIBUTES_OBSERVED

/* A device path shaped like the image's own static OBJECT_STRINGs. */
#define GUEST_PATH "\\Device\\Harddisk0\\partition1\\tsfp.xbe"

#define SCRATCH_BYTES 0x4000u
#define FRAME_OFFSET 0x000u
#define FRAME_BYTES 0x100u
#define OA_OFFSET 0x200u
#define STRING_OFFSET 0x220u
#define CHARS_OFFSET 0x240u
#define IOSB_OFFSET 0x400u
#define HANDLE_OFFSET 0x440u
#define POISON_FROM 0x200u
#define POISON_BYTES 0x400u

#define SENTINEL_BYTE 0x5Au

static kernel_guest_ptr scratch;

static kernel_guest_ptr at(uint32_t offset)
{
    return scratch + offset;
}

static void write_u32_at(kernel_guest_ptr address, uint32_t value)
{
    if (!kernel_guest_write_u32(address, value)) {
        printf("FATAL could not write guest memory at %#x\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
}

static uint32_t read_u32_at(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

/* Everything the handler might write starts at a known non-zero value, so "untouched"
 * is never confusable with "written a zero". */
static void poison(void)
{
    for (uint32_t i = 0u; i < POISON_BYTES; i++) {
        if (!kernel_guest_write_u8(at(POISON_FROM) + i, SENTINEL_BYTE)) {
            printf("FATAL could not poison scratch\n");
            exit(EXIT_FAILURE);
        }
    }
}

static void setup(void)
{
    kernel_hle_init();
    kernel_file_reset();
    kernel_object_reset();
    guest_mem_reset();
    /* FOUR now, not one: NtOpenFile (202) was joined by NtCreateFile (190) and the
     * symbolic-link pair IoCreateSymbolicLink (67) / IoDeleteSymbolicLink (69). The
     * exact count is deliberate -- a dropped binding leaves a stub that returns 0, which
     * every caller reads as STATUS_SUCCESS, so this assertion is what stops the suite
     * from passing against nothing. */
    CHECK_EQ_U32(kernel_file_register(), 7u);
    kernel_hle_set_log(capture_printer);
    captured[0] = '\0';
    captured_len = 0u;

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FATAL could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
    poison();
}

static void teardown(void)
{
    kernel_file_reset();
    kernel_object_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

/*
 * Lay out an OBJECT_STRING for `path`, at the offsets guest_structs.h derives:
 * length at +0x00 (16-bit), maximum_length at +0x02 (16-bit), buffer at +0x04.
 *
 * The characters are written WITHOUT a trailing NUL, and the byte after them is left
 * at the poison value. The guest always sets MaximumLength to Length+1 and so always
 * has room for one, but MaximumLength is the guest's promise about its own buffer, not
 * a promise that the kernel may stop at a NUL -- so a handler that relied on one would
 * read the poison byte and beyond.
 */
static kernel_guest_ptr build_name(const char *path)
{
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        if (!kernel_guest_write_u8(at(CHARS_OFFSET) + (uint32_t)i,
                                   (uint8_t)path[i])) {
            printf("FATAL could not write the path characters\n");
            exit(EXIT_FAILURE);
        }
    }
    const kernel_guest_ptr string = at(STRING_OFFSET);
    if (!kernel_guest_write_u8(string + 0u, (uint8_t)(length & 0xFFu)) ||
        !kernel_guest_write_u8(string + 1u, (uint8_t)((length >> 8) & 0xFFu)) ||
        !kernel_guest_write_u8(string + 2u, (uint8_t)((length + 1u) & 0xFFu)) ||
        !kernel_guest_write_u8(string + 3u, (uint8_t)(((length + 1u) >> 8) & 0xFFu))) {
        printf("FATAL could not write the OBJECT_STRING header\n");
        exit(EXIT_FAILURE);
    }
    write_u32_at(string + 4u, at(CHARS_OFFSET));
    return string;
}

/* OBJECT_ATTRIBUTES: root_directory at +0x00, object_name at +0x04, attributes at
 * +0x08. 12 bytes, no Length field. */
static kernel_guest_ptr build_attributes(uint32_t root_directory,
                                         kernel_guest_ptr name_string)
{
    const kernel_guest_ptr oa = at(OA_OFFSET);
    write_u32_at(oa + 0u, root_directory);
    write_u32_at(oa + 4u, name_string);
    write_u32_at(oa + 8u, MEASURED_ATTRIBUTES);
    return oa;
}

/* `count` is a parameter rather than a constant 6 so the arity test can hand over a
 * frame that is one slot short. */
static uint32_t call_open(const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamped to exactly the slots built, so a read past the last argument FAILS
     * rather than returning whatever follows. */
    frame.stack_limit = at(FRAME_OFFSET) + (count + 1u) * 4u;
    return kernel_hle_call(ORD_NT_OPEN_FILE, &frame);
}

/* The whole measured call shape, with the handle out-parameter in its own slot. */
static uint32_t open_path(const char *path, uint32_t root_directory)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(root_directory, name);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE, MEASURED_OPTIONS};
    return call_open(args, 6u);
}

static uint32_t iosb_status(void)
{
    return read_u32_at(at(IOSB_OFFSET) +
                       (uint32_t)offsetof(guest_io_status_block, status));
}

static uint32_t iosb_information(void)
{
    return read_u32_at(at(IOSB_OFFSET) +
                       (uint32_t)offsetof(guest_io_status_block, information));
}

static bool all_bytes_at_are(kernel_guest_ptr address, uint8_t expected, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        uint8_t actual = 0u;
        if (!kernel_guest_read_u8(address + i, &actual) || actual != expected) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */

/*
 * The ordinal stops being a stub, which is the line that moves the bring-up on.
 *
 * MUTATION: remove the binding from `kernel_file_register`'s table and this fails.
 */
static void test_registration_makes_the_ordinal_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_NT_OPEN_FILE);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK(entry->name != NULL && strcmp(entry->name, "NtOpenFile") == 0);
    }
    teardown();
}

static void test_an_implemented_ordinal_does_not_report_itself_as_a_stub(void)
{
    setup();
    (void)open_path(GUEST_PATH, 0u);
    CHECK(!captured_contains("not implemented"));
    teardown();
}

/*
 * THE DEFAULT: a name with no volume behind it is refused, the failure reaches the
 * IO_STATUS_BLOCK, and the handle out-parameter is LEFT ALONE -- which is what the
 * real kernel does on a failed open.
 *
 * MUTATION: let the refusal path write the handle, and this fails on the sentinel.
 * MUTATION: skip the IO_STATUS_BLOCK write on the failure path, and this fails too:
 * the two measured STATUS_PENDING sites read that field rather than eax.
 */
static void test_a_name_with_no_volume_is_refused_and_writes_no_handle(void)
{
    setup();
    CHECK_EQ_U32(kernel_file_openable_count(), 0u);

    CHECK_EQ_U32(open_path(GUEST_PATH, 0u),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);

    CHECK(all_bytes_at_are(at(HANDLE_OFFSET), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK_EQ_U32(kernel_file_refused_count(), 1u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("REFUSED"));
    teardown();
}

/*
 * An openable name produces a real handle, and the handle is a FILE handle in the
 * shared object table -- not merely a plausible number, which is what NtClose would
 * later choke on.
 *
 * MUTATION: have the success path return STATUS_SUCCESS without calling
 * kernel_object_create, and this fails on the live count.
 */
static void test_an_openable_name_yields_a_real_file_handle(void)
{
    setup();
    CHECK(kernel_file_add_openable(GUEST_PATH));
    CHECK_EQ_U32(kernel_file_openable_count(), 1u);

    CHECK_EQ_U32(open_path(GUEST_PATH, 0u), STATUS_SUCCESS);

    const uint32_t handle = read_u32_at(at(HANDLE_OFFSET));
    CHECK(handle != 0u);
    /* Not the sentinel pattern, i.e. something really was written. */
    CHECK(handle != 0x5A5A5A5Au);
    const kernel_object_entry *object = kernel_object_find(handle);
    CHECK(object != NULL);
    if (object) {
        CHECK_EQ_U32(object->kind, KERNEL_OBJECT_FILE);
    }
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 1u);
    CHECK_EQ_U32(kernel_file_refused_count(), 0u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);

    const kernel_file_attempt *attempt = kernel_file_attempt_at(0u);
    CHECK(attempt != NULL);
    if (attempt) {
        CHECK(attempt->opened);
    }
    teardown();
}

/*
 * THE ARITY. A frame one slot short of the sixth argument must be refused.
 *
 * MUTATION: read 5 arguments and default OpenOptions, and this is the only test here
 * that fails -- which is why it exists.
 */
static void test_six_arguments_are_required(void)
{
    setup();
    const kernel_guest_ptr name = build_name(GUEST_PATH);
    const kernel_guest_ptr oa = build_attributes(0u, name);
    const uint32_t args[5] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE};
    CHECK_EQ_U32(call_open(args, 5u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 5"));
    CHECK(all_bytes_at_are(at(HANDLE_OFFSET), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    teardown();
}

/*
 * THE ARGUMENT ORDER, pinned by the three literals the reached site passes.
 *
 * MUTATION: swap any two of DesiredAccess, ShareAccess and OpenOptions in the
 * handler, or shift the OBJECT_ATTRIBUTES and IoStatusBlock indices, and this fails.
 * The three literals are deliberately all different and none is a small number that
 * could coincide with another, so no permutation passes.
 */
static void test_the_six_arguments_are_in_the_measured_order(void)
{
    setup();
    CHECK_EQ_U32(open_path(GUEST_PATH, 0u),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);

    CHECK_EQ_U32(kernel_file_attempt_count(), 1u);
    const kernel_file_attempt *attempt = kernel_file_attempt_at(0u);
    CHECK(attempt != NULL);
    if (attempt) {
        /* The path proves OBJECT_ATTRIBUTES came from argument 2. */
        CHECK(strcmp(attempt->path, GUEST_PATH) == 0);
        CHECK_EQ_U32(attempt->desired_access, MEASURED_ACCESS);
        CHECK_EQ_U32(attempt->share_access, MEASURED_SHARE);
        CHECK_EQ_U32(attempt->open_options, MEASURED_OPTIONS);
    }
    /* And the IO_STATUS_BLOCK really was argument 3: the status landed there and
     * nowhere else in the poisoned span. */
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/*
 * THE ALIASED OUT-PARAMETER, which is the shape the run actually reaches.
 *
 * `sub_00380D0D` passes `lea eax,[ebp+8]` as the handle out-parameter while
 * OBJECT_ATTRIBUTES.object_name is `[ebp+8]` -- so the handle is written over the
 * pointer to the name.
 *
 * MUTATION: move the handle write above the name read in the handler, and this fails
 * with a garbage path. Nothing else in this suite catches that ordering, because
 * every other test puts the handle in a slot of its own.
 */
static void test_the_handle_slot_may_alias_the_slot_holding_the_name_pointer(void)
{
    setup();
    CHECK(kernel_file_add_openable(GUEST_PATH));

    const kernel_guest_ptr name = build_name(GUEST_PATH);
    /* One slot plays both parts: it holds the pointer to the OBJECT_STRING, and it is
     * where the handle goes. */
    const kernel_guest_ptr shared_slot = at(HANDLE_OFFSET);
    write_u32_at(shared_slot, name);
    const kernel_guest_ptr oa = build_attributes(0u, name);

    const uint32_t args[6] = {shared_slot, MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE, MEASURED_OPTIONS};
    CHECK_EQ_U32(call_open(args, 6u), STATUS_SUCCESS);

    /* The handle overwrote the name pointer, which is fine -- but the path recorded
     * before that happened must be the real one. */
    const uint32_t handle = read_u32_at(shared_slot);
    CHECK(handle != name);
    CHECK(kernel_object_find(handle) != NULL);
    const kernel_file_attempt *attempt = kernel_file_attempt_at(0u);
    CHECK(attempt != NULL);
    if (attempt) {
        CHECK(strcmp(attempt->path, GUEST_PATH) == 0);
    }
    teardown();
}

/*
 * OBJECT_STRING.length IS 16 BITS, with MaximumLength in the half above it.
 *
 * MUTATION: read a dword at +0x00 for the length, and this fails. For this path the
 * dword is 0x00260025 -- 2490405 -- which is not a length at all, and a handler
 * reading it would either refuse a perfectly good name or walk 2.4 MB of guest
 * memory. `guest_structs.h` derives the 16-bit width from a word-sized CLEAR at
 * 0x003805A5 that a 32-bit field could not use; this is the behavioural counterpart.
 */
static void test_the_name_length_is_sixteen_bits_not_thirty_two(void)
{
    setup();
    CHECK(kernel_file_add_openable(GUEST_PATH));

    const kernel_guest_ptr name = build_name(GUEST_PATH);
    /* Confirm the fixture really does put a nonzero MaximumLength in the high half,
     * or this test would be vacuous. */
    const uint32_t packed = read_u32_at(name);
    CHECK_EQ_U32(packed & 0xFFFFu, strlen(GUEST_PATH));
    CHECK((packed >> 16) != 0u);

    CHECK_EQ_U32(open_path(GUEST_PATH, 0u), STATUS_SUCCESS);
    const kernel_file_attempt *attempt = kernel_file_attempt_at(0u);
    CHECK(attempt != NULL);
    if (attempt) {
        CHECK(strcmp(attempt->path, GUEST_PATH) == 0);
        CHECK_EQ_U32(strlen(attempt->path), strlen(GUEST_PATH));
    }
    teardown();
}

/*
 * The characters are taken by LENGTH, not by looking for a NUL.
 *
 * `build_name` deliberately leaves the byte after the path at the poison value.
 *
 * MUTATION: read the path with a strlen-style loop over the guest buffer instead of
 * `length` bytes, and this fails -- the recorded path picks up poison bytes until it
 * happens to hit a zero.
 */
static void test_the_name_is_taken_by_length_and_not_by_a_nul(void)
{
    setup();
    const kernel_guest_ptr name = build_name(GUEST_PATH);
    uint8_t after = 0u;
    CHECK(kernel_guest_read_u8(at(CHARS_OFFSET) + (uint32_t)strlen(GUEST_PATH), &after));
    CHECK_EQ_U32(after, SENTINEL_BYTE);
    (void)name;

    (void)open_path(GUEST_PATH, 0u);
    const kernel_file_attempt *attempt = kernel_file_attempt_at(0u);
    CHECK(attempt != NULL);
    if (attempt) {
        CHECK(strcmp(attempt->path, GUEST_PATH) == 0);
    }
    teardown();
}

/*
 * A RELATIVE open is refused rather than treated as absolute.
 *
 * MUTATION: ignore `root_directory` and fall through to the normal path, and this
 * fails. The pseudo-handle 0xFFFFFFFD appears at 9 of the 31 measured
 * inline OBJECT_ATTRIBUTES sites and `guest_structs.h` records its meaning as a HYPOTHESIS;
 * acting on it here would bake the hypothesis in.
 */
static void test_a_relative_open_is_refused_and_counted(void)
{
    setup();
    /* Openable, so the refusal cannot be the name simply not resolving. */
    CHECK(kernel_file_add_openable(GUEST_PATH));

    CHECK_EQ_U32(open_path(GUEST_PATH, GUEST_OBJ_ROOT_DIRECTORY_PSEUDO),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 1u);
    CHECK_EQ_U32(kernel_file_refused_count(), 0u);
    CHECK(all_bytes_at_are(at(HANDLE_OFFSET), SENTINEL_BYTE, 4u));
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/*
 * Matching is case-insensitive, in the LENIENT direction.
 *
 * MUTATION: use strcmp in `paths_equal` and this fails. A case-sensitive match could
 * refuse a name the real kernel would have opened, and a refusal that should have
 * succeeded is the error that stops a run -- so when the evidence is a single
 * inferred flag bit, the error that keeps going is the better one to make.
 */
static void test_matching_is_case_insensitive(void)
{
    setup();
    CHECK(kernel_file_add_openable("\\Device\\Harddisk0\\Partition1\\TSFP.XBE"));
    CHECK_EQ_U32(open_path("\\device\\harddisk0\\partition1\\tsfp.xbe", 0u),
                 STATUS_SUCCESS);
    /* And a genuinely different name is still refused, or the comparison would be
     * accepting everything. */
    CHECK_EQ_U32(open_path("\\Device\\Harddisk0\\Partition1\\other.xbe", 0u),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/*
 * The EMPTY policy succeeds and ANNOUNCES it.
 *
 * MUTATION: drop the FABRICATED log line, and this fails. A fabricated open that does
 * not say so makes every later read look like a lifting problem.
 */
static void test_the_empty_policy_fabricates_and_says_so(void)
{
    setup();
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);

    CHECK_EQ_U32(open_path(GUEST_PATH, 0u), STATUS_SUCCESS);
    CHECK(read_u32_at(at(HANDLE_OFFSET)) != 0u);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 1u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    CHECK_EQ_U32(kernel_file_refused_count(), 0u);
    CHECK(captured_contains("FABRICATED"));

    /* The policy is not a one-way door: an openable name is still served without
     * being counted as fabricated. */
    CHECK(kernel_file_add_openable("\\??\\D:"));
    CHECK_EQ_U32(open_path("\\??\\D:", 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    teardown();
}

/* Repeated opens of one name are one record with a count, because the deliverable is
 * WHICH names the title wants. */
static void test_repeated_opens_of_one_name_are_one_record(void)
{
    setup();
    (void)open_path(GUEST_PATH, 0u);
    (void)open_path(GUEST_PATH, 0u);
    (void)open_path("\\??\\D:", 0u);

    CHECK_EQ_U32(kernel_file_attempt_count(), 2u);
    const kernel_file_attempt *first = kernel_file_attempt_at(0u);
    CHECK(first != NULL);
    if (first) {
        CHECK_EQ_U32(first->attempts, 2u);
        CHECK(strcmp(first->path, GUEST_PATH) == 0);
    }
    const kernel_file_attempt *second = kernel_file_attempt_at(1u);
    CHECK(second != NULL);
    if (second) {
        CHECK_EQ_U32(second->attempts, 1u);
    }
    CHECK(kernel_file_attempt_at(2u) == NULL);
    CHECK_EQ_U32(kernel_file_refused_count(), 3u);
    teardown();
}

/* Missing out-parameters and a missing frame are reported, not treated as an open of
 * nothing. */
static void test_missing_parameters_are_reported(void)
{
    setup();
    const kernel_guest_ptr name = build_name(GUEST_PATH);
    const kernel_guest_ptr oa = build_attributes(0u, name);

    const uint32_t no_handle[6] = {0u, MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                                   MEASURED_SHARE, MEASURED_OPTIONS};
    CHECK_EQ_U32(call_open(no_handle, 6u), STATUS_INVALID_PARAMETER);

    const uint32_t no_oa[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, 0u, at(IOSB_OFFSET),
                               MEASURED_SHARE, MEASURED_OPTIONS};
    CHECK_EQ_U32(call_open(no_oa, 6u), STATUS_INVALID_PARAMETER);

    CHECK_EQ_U32(kernel_hle_call(ORD_NT_OPEN_FILE, NULL), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("no argument frame"));

    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* An OBJECT_ATTRIBUTES whose object_name is null, and a name whose buffer is null,
 * are parameter errors rather than an open of the empty string. */
static void test_a_nameless_object_attributes_is_refused(void)
{
    setup();
    const kernel_guest_ptr oa = build_attributes(0u, 0u);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE, MEASURED_OPTIONS};
    CHECK_EQ_U32(call_open(args, 6u), STATUS_INVALID_PARAMETER);

    /* A string struct whose Buffer is null. */
    const kernel_guest_ptr name = build_name(GUEST_PATH);
    write_u32_at(name + 4u, 0u);
    const kernel_guest_ptr oa2 = build_attributes(0u, name);
    const uint32_t args2[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa2, at(IOSB_OFFSET),
                               MEASURED_SHARE, MEASURED_OPTIONS};
    CHECK_EQ_U32(call_open(args2, 6u), STATUS_INVALID_PARAMETER);

    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    teardown();
}

/* The openable set's own refusals, so a caller cannot install a name this module
 * could not later hold. */
static void test_the_openable_set_refuses_what_it_cannot_hold(void)
{
    setup();
    CHECK(!kernel_file_add_openable(NULL));
    CHECK(!kernel_file_add_openable(""));
    char too_long[KERNEL_FILE_PATH_MAX + 8u];
    memset(too_long, 'a', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK(!kernel_file_add_openable(too_long));
    CHECK_EQ_U32(kernel_file_openable_count(), 0u);

    /* Adding the same name twice is one entry, not two. */
    CHECK(kernel_file_add_openable(GUEST_PATH));
    CHECK(kernel_file_add_openable(GUEST_PATH));
    CHECK_EQ_U32(kernel_file_openable_count(), 1u);
    teardown();
}

/* reset() must clear, including the policy, or one test silently changes what the
 * next one is testing. */
static void test_reset_clears_the_set_the_record_and_the_policy(void)
{
    setup();
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    CHECK(kernel_file_add_openable("\\??\\D:"));
    (void)open_path(GUEST_PATH, 0u);
    CHECK_EQ_U32(kernel_file_attempt_count(), 1u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);

    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    CHECK_EQ_U32(kernel_file_openable_count(), 0u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    /* Back to FAIL. */
    CHECK_EQ_U32(open_path(GUEST_PATH, 0u),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/* ===================== NtOpenSymbolicLinkObject / NtQuerySymbolicLinkObject ============ */

/*
 * The pair the boot stops on after the title creates `\??\D:` and then reads it back
 * (0x003805A1 and 0x003805C9). Every literal below is a value the guest uses: the link
 * and target are the title's own `\??\D:` -> `\Device\CdRom0`, MaximumLength 0x208 is the
 * guest's stack buffer at that site, and the 14-byte length is the target's real length.
 *
 * Statuses are written as hex literals, never through the KERNEL_FILE_STATUS_* macros, so
 * a mutated macro cannot move the expectation with it.
 */
#define ORD_NT_OPEN_SYMLINK 203u
#define ORD_NT_QUERY_SYMLINK 215u
#define ORD_NT_CLOSE 187u

#define LINK_NAME "\\??\\D:"
#define LINK_TARGET "\\Device\\CdRom0"
#define LINK_TARGET_BYTES 14u
#define GUEST_MAXIMUM_LENGTH 0x208u

#define QUERY_STRING_OFFSET 0x800u
#define QUERY_CHARS_OFFSET 0x820u
#define RETURNED_LENGTH_OFFSET 0xA80u
#define QUERY_REGION_BYTES 0x300u
#define INCOMING_LENGTH 0x7777u

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    frame.stack_limit = at(FRAME_OFFSET) + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

static void poison_query_region(void)
{
    for (uint32_t i = 0u; i < QUERY_REGION_BYTES; i++) {
        if (!kernel_guest_write_u8(at(QUERY_STRING_OFFSET) + i, SENTINEL_BYTE)) {
            printf("FATAL could not poison the query region\n");
            exit(EXIT_FAILURE);
        }
    }
}

static uint32_t open_link(const char *name, uint32_t root_directory)
{
    const kernel_guest_ptr oa = build_attributes(root_directory, build_name(name));
    const uint32_t args[2] = {at(HANDLE_OFFSET), oa};
    return call_ordinal(ORD_NT_OPEN_SYMLINK, args, 2u);
}

/* The guest's own shape: Length is garbage on entry, MaximumLength is the buffer size. */
static void build_output_string(uint32_t maximum_length)
{
    const kernel_guest_ptr string = at(QUERY_STRING_OFFSET);
    if (!kernel_guest_write_u8(string + 0u, (uint8_t)(INCOMING_LENGTH & 0xFFu)) ||
        !kernel_guest_write_u8(string + 1u, (uint8_t)(INCOMING_LENGTH >> 8)) ||
        !kernel_guest_write_u8(string + 2u, (uint8_t)(maximum_length & 0xFFu)) ||
        !kernel_guest_write_u8(string + 3u, (uint8_t)(maximum_length >> 8))) {
        printf("FATAL could not write the output OBJECT_STRING\n");
        exit(EXIT_FAILURE);
    }
    write_u32_at(string + 4u, at(QUERY_CHARS_OFFSET));
}

static uint32_t query_link(uint32_t handle, uint32_t returned_length_ptr)
{
    const uint32_t args[3] = {handle, at(QUERY_STRING_OFFSET), returned_length_ptr};
    return call_ordinal(ORD_NT_QUERY_SYMLINK, args, 3u);
}

static uint32_t opened_handle(void)
{
    return read_u32_at(at(HANDLE_OFFSET));
}

static uint32_t output_length(void)
{
    uint8_t low = 0u;
    uint8_t high = 0u;
    CHECK(kernel_guest_read_u8(at(QUERY_STRING_OFFSET) + 0u, &low));
    CHECK(kernel_guest_read_u8(at(QUERY_STRING_OFFSET) + 1u, &high));
    return (uint32_t)low | ((uint32_t)high << 8);
}

static uint32_t output_maximum_length(void)
{
    uint8_t low = 0u;
    uint8_t high = 0u;
    CHECK(kernel_guest_read_u8(at(QUERY_STRING_OFFSET) + 2u, &low));
    CHECK(kernel_guest_read_u8(at(QUERY_STRING_OFFSET) + 3u, &high));
    return (uint32_t)low | ((uint32_t)high << 8);
}

static bool output_chars_are(const char *expected)
{
    for (size_t i = 0u; expected[i] != '\0'; i++) {
        uint8_t actual = 0u;
        if (!kernel_guest_read_u8(at(QUERY_CHARS_OFFSET) + (uint32_t)i, &actual) ||
            actual != (uint8_t)expected[i]) {
            return false;
        }
    }
    return true;
}

static uint8_t output_byte(uint32_t index)
{
    uint8_t value = 0u;
    CHECK(kernel_guest_read_u8(at(QUERY_CHARS_OFFSET) + index, &value));
    return value;
}

static void setup_links(void)
{
    setup();
    poison_query_region();
    CHECK(kernel_file_add_symlink(LINK_NAME, LINK_TARGET));
}

/*
 * MUTATION: drop either binding from kernel_file_register and this fails, as does a
 * wrong ordinal number.
 */
static void test_the_symbolic_link_ordinals_are_implemented(void)
{
    setup();
    const kernel_entry *open_entry = kernel_hle_entry(ORD_NT_OPEN_SYMLINK);
    const kernel_entry *query_entry = kernel_hle_entry(ORD_NT_QUERY_SYMLINK);
    CHECK(open_entry != NULL && query_entry != NULL);
    if (open_entry && query_entry) {
        CHECK_EQ_U32(open_entry->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK_EQ_U32(query_entry->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK(strcmp(open_entry->name, "NtOpenSymbolicLinkObject") == 0);
        CHECK(strcmp(query_entry->name, "NtQuerySymbolicLinkObject") == 0);
    }
    teardown();
}

/*
 * THE BOOT'S OWN SEQUENCE: open the link the title created, read its target back.
 *
 * MUTATION: write the Length at the wrong width or offset, write no characters, return a
 * different target, or skip the handle write, and one of these fails.
 */
static void test_the_guests_own_open_then_query_round_trips(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    const uint32_t handle = opened_handle();
    CHECK(handle != SENTINEL_BYTE * 0x01010101u);
    const kernel_object_entry *entry = kernel_object_find(handle);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->kind, KERNEL_OBJECT_SYMLINK);
    }
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 1u);

    build_output_string(GUEST_MAXIMUM_LENGTH);
    CHECK_EQ_U32(query_link(handle, 0u), 0x00000000u);
    CHECK_EQ_U32(output_length(), 14u);
    CHECK(output_chars_are("\\Device\\CdRom0"));
    /* The 16-bit Length write must not touch MaximumLength beside it. */
    CHECK_EQ_U32(output_maximum_length(), 0x208u);
    teardown();
}

/*
 * ReturnedLength is a DWORD and equals the length WITHOUT the NUL: the guest at
 * 0x0037F930 indexes Buffer[esi-9] and parses Buffer[esi-8 .. esi-1] as hex digits.
 *
 * MUTATION: write Length+1, write only a word, or write nothing, and this fails.
 */
static void test_returned_length_is_a_dword_without_the_nul(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    build_output_string(GUEST_MAXIMUM_LENGTH);
    write_u32_at(at(RETURNED_LENGTH_OFFSET), 0xFFFFFFFFu);
    CHECK_EQ_U32(query_link(opened_handle(), at(RETURNED_LENGTH_OFFSET)), 0x00000000u);
    CHECK_EQ_U32(read_u32_at(at(RETURNED_LENGTH_OFFSET)), 14u);
    CHECK_EQ_U32(output_length(), 14u);
    teardown();
}

/*
 * The incoming Length (0x7777 here) is replaced, since the guest never initialises it at
 * 0x0037F915, and nothing past the characters is written: the guest's consumer tolerates a
 * missing NUL.
 *
 * MUTATION: append a NUL, or leave Length untouched, and this fails.
 */
static void test_only_the_characters_are_written_and_the_incoming_length_is_ignored(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    build_output_string(GUEST_MAXIMUM_LENGTH);
    CHECK_EQ_U32(query_link(opened_handle(), 0u), 0x00000000u);
    CHECK_EQ_U32(output_length(), 14u);
    CHECK_EQ_U32(output_byte(LINK_TARGET_BYTES), SENTINEL_BYTE);
    teardown();
}

/*
 * Capacity boundary: a target of exactly MaximumLength bytes fits (no NUL is written, so
 * none is needed), one byte less does not.
 *
 * MUTATION: `>` to `>=` fails the first, `>` to `<` or an off-by-one fails the second.
 */
static void test_the_capacity_boundary_is_the_exact_target_length(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    const uint32_t handle = opened_handle();

    build_output_string(14u);
    CHECK_EQ_U32(query_link(handle, 0u), 0x00000000u);
    CHECK_EQ_U32(output_length(), 14u);

    poison_query_region();
    build_output_string(13u);
    write_u32_at(at(RETURNED_LENGTH_OFFSET), 0u);
    CHECK_EQ_U32(query_link(handle, at(RETURNED_LENGTH_OFFSET)), 0xC0000023u);
    /* Nothing was written to the caller's string, and the size needed is reported. */
    CHECK_EQ_U32(output_length(), INCOMING_LENGTH);
    CHECK_EQ_U32(output_byte(0u), SENTINEL_BYTE);
    CHECK_EQ_U32(read_u32_at(at(RETURNED_LENGTH_OFFSET)), 14u);
    teardown();
}

/* MUTATION: skip the NULL check on the OBJECT_STRING pointer or on its buffer. */
static void test_an_unusable_output_string_is_refused(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    const uint32_t handle = opened_handle();
    const uint32_t null_string[3] = {handle, 0u, 0u};
    CHECK_EQ_U32(call_ordinal(ORD_NT_QUERY_SYMLINK, null_string, 3u), 0xC000000Du);

    build_output_string(GUEST_MAXIMUM_LENGTH);
    write_u32_at(at(QUERY_STRING_OFFSET) + 4u, 0u);
    CHECK_EQ_U32(query_link(handle, 0u), 0xC000000Du);
    teardown();
}

/*
 * A link that does not exist is an honest miss: the guest probes `\??\W:` and takes its
 * failure path. The handle slot is left alone and nothing is issued.
 *
 * MUTATION: succeed on a miss, write the handle anyway, or return another status.
 */
static void test_a_link_the_title_never_created_is_not_found(void)
{
    setup_links();
    write_u32_at(at(HANDLE_OFFSET), 0xA5A5A5A5u);
    CHECK_EQ_U32(open_link("\\??\\W:", 0u), 0xC0000034u);
    CHECK_EQ_U32(opened_handle(), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 0u);
    CHECK(captured_contains("\\??\\W:"));
    teardown();
}

/*
 * The link is matched as a WHOLE NAME, not a prefix. Path resolution rewrites a leading
 * component, but opening the link object itself names exactly one object.
 *
 * MUTATION: resolve through resolve_symlinks_locked or prefix-match, and the longer and
 * the shorter spelling both stop being misses.
 */
static void test_the_link_name_is_matched_whole_not_as_a_prefix(void)
{
    setup_links();
    CHECK_EQ_U32(open_link("\\??\\D:\\pak", 0u), 0xC0000034u);
    CHECK_EQ_U32(open_link("\\??\\D", 0u), 0xC0000034u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/*
 * A handle slot the host cannot write is reported, not answered with success. 0xFFFFFFFE runs a
 * dword past the 4 GB guest limit, which the accessors reject.
 *
 * MUTATION: disable the failure branch of the handle write and this fails.
 */
static void test_an_unwritable_handle_slot_is_reported(void)
{
    setup_links();
    const kernel_guest_ptr oa = build_attributes(0u, build_name(LINK_NAME));
    const uint32_t args[2] = {0xFFFFFFFEu, oa};
    CHECK_EQ_U32(call_ordinal(ORD_NT_OPEN_SYMLINK, args, 2u), 0xC000000Du);
    CHECK(captured_contains("could not write the handle"));
    teardown();
}

/* The title spells device names in mixed case, so link names match ASCII-insensitively. */
static void test_the_link_name_is_case_insensitive(void)
{
    setup_links();
    CHECK_EQ_U32(open_link("\\??\\d:", 0u), 0x00000000u);
    teardown();
}

/* MUTATION: ignore a non-NULL root directory and open anyway. */
static void test_a_relative_link_open_is_refused_and_counted(void)
{
    setup_links();
    write_u32_at(at(HANDLE_OFFSET), 0xA5A5A5A5u);
    CHECK_EQ_U32(open_link(LINK_NAME, 0xFFFFFFFCu), 0xC000003Au);
    CHECK_EQ_U32(opened_handle(), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 1u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* An open with `attributes` written over the measured value. Everything else is the
 * measured call shape. */
static uint32_t open_path_with_attributes(const char *path, uint32_t attributes)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(0u, name);
    write_u32_at(oa + 8u, attributes);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE, MEASURED_OPTIONS};
    return call_open(args, 6u);
}

#define ORD_NT_CREATE_FILE_LITERAL 190u

static uint32_t create_path_with_attributes(const char *path, uint32_t attributes)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(0u, name);
    write_u32_at(oa + 8u, attributes);
    /* Handle, access, OA, IOSB, allocation size, file attributes, share, disposition
     * (FILE_OPEN = 1), options: the order measured at 0x00380127. */
    const uint32_t args[9] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET), 0u, 0u,
                              MEASURED_SHARE, 1u, 0u};
    return call_ordinal(ORD_NT_CREATE_FILE_LITERAL, args, 9u);
}

static uint32_t open_link_with_attributes(const char *name, uint32_t attributes)
{
    const kernel_guest_ptr oa = build_attributes(0u, build_name(name));
    write_u32_at(oa + 8u, attributes);
    const uint32_t args[2] = {at(HANDLE_OFFSET), oa};
    return call_ordinal(ORD_NT_OPEN_SYMLINK, args, 2u);
}

/*
 * `attributes` is honoured by no handler, so every value but the one the host's behaviour
 * agrees with (0x40, case-insensitive) is COUNTED and REPORTED, by all THREE handlers that
 * read an OBJECT_ATTRIBUTES. The image's inline builders all store 0x40 and the
 * named-object helper stores 0x80 (0x00381B99), so 0x80 is the realistic second value.
 * Literals, never the macro under test.
 *
 * MUTATION: drop the report from any one handler, or invert the 0x40 test, and this fails.
 * The call itself must still succeed: the field is ignored, not enforced.
 */
static void test_an_attributes_value_the_host_does_not_model_is_counted_and_reported(void)
{
    setup_links();
    CHECK(kernel_file_add_openable(GUEST_PATH));

    CHECK_EQ_U32(open_path_with_attributes(GUEST_PATH, 0x40u), 0x00000000u);
    CHECK_EQ_U32(create_path_with_attributes(GUEST_PATH, 0x40u), 0x00000000u);
    CHECK_EQ_U32(open_link_with_attributes(LINK_NAME, 0x40u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 0u);
    CHECK(!captured_contains("does not model"));

    CHECK_EQ_U32(open_path_with_attributes(GUEST_PATH, 0x80u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 1u);
    CHECK(captured_contains("NtOpenFile(\"" GUEST_PATH "\") OBJECT_ATTRIBUTES.attributes is 0x80"));

    CHECK_EQ_U32(create_path_with_attributes(GUEST_PATH, 0x80u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 2u);
    CHECK(captured_contains("NtCreateFile(\"" GUEST_PATH "\") OBJECT_ATTRIBUTES.attributes is 0x80"));

    CHECK_EQ_U32(open_link_with_attributes(LINK_NAME, 0x80u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 3u);
    CHECK(captured_contains("NtOpenSymbolicLinkObject(\"" LINK_NAME "\") OBJECT_ATTRIBUTES.attributes is 0x80"));

    /* 0x40 plus another bit, and no bits at all: both differ from what the host does. */
    CHECK_EQ_U32(open_path_with_attributes(GUEST_PATH, 0xC0u), 0x00000000u);
    CHECK_EQ_U32(open_path_with_attributes(GUEST_PATH, 0x00u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 5u);
    /* Reporting is not refusing: every one of those opens was satisfied. */
    CHECK_EQ_U32(kernel_file_refused_count(), 0u);
    teardown();
}

/*
 * ARITY. 203 takes two stack arguments (not desktop NT's three) and 215 takes three. A
 * frame one slot short must be refused, because a handler that read a stale slot would
 * pop and use the wrong number.
 */
static void test_each_ordinal_refuses_a_frame_that_is_one_argument_short(void)
{
    setup_links();
    const uint32_t one[1] = {at(HANDLE_OFFSET)};
    CHECK_EQ_U32(call_ordinal(ORD_NT_OPEN_SYMLINK, one, 1u), 0xC000000Du);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);

    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    const uint32_t two[2] = {opened_handle(), at(QUERY_STRING_OFFSET)};
    CHECK_EQ_U32(call_ordinal(ORD_NT_QUERY_SYMLINK, two, 2u), 0xC000000Du);
    teardown();
}

/* The handle is checked for existence first and for kind second, with different statuses. */
static void test_a_query_on_the_wrong_handle_is_refused_by_kind(void)
{
    setup_links();
    build_output_string(GUEST_MAXIMUM_LENGTH);
    CHECK_EQ_U32(query_link(0x00DEAD00u, 0u), 0xC0000008u);

    const uint32_t event = kernel_object_create(KERNEL_OBJECT_EVENT, 0u);
    CHECK(event != 0u);
    CHECK_EQ_U32(query_link(event, 0u), 0xC0000024u);
    CHECK_EQ_U32(output_length(), INCOMING_LENGTH);
    teardown();
}

/*
 * The handle names the link OBJECT. Deleting the link afterwards must not change what the
 * handle answers, and two handles must not share a target.
 *
 * MUTATION: look the target up by name at query time, or key the slot lookup wrongly.
 */
static void test_a_handle_keeps_its_own_target_after_the_link_is_deleted(void)
{
    setup_links();
    CHECK(kernel_file_add_symlink("\\??\\T:",
                                  "\\Device\\Harddisk0\\partition1\\TDATA\\45410066"));
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    const uint32_t cdrom = opened_handle();
    CHECK_EQ_U32(open_link("\\??\\T:", 0u), 0x00000000u);
    const uint32_t tdata = opened_handle();
    CHECK(cdrom != tdata);
    CHECK(kernel_file_remove_symlink(LINK_NAME));

    build_output_string(GUEST_MAXIMUM_LENGTH);
    CHECK_EQ_U32(query_link(cdrom, 0u), 0x00000000u);
    CHECK_EQ_U32(output_length(), 14u);
    CHECK(output_chars_are("\\Device\\CdRom0"));

    build_output_string(GUEST_MAXIMUM_LENGTH);
    CHECK_EQ_U32(query_link(tdata, 0u), 0x00000000u);
    CHECK_EQ_U32(output_length(), 43u);
    CHECK(output_chars_are("\\Device\\Harddisk0\\partition1\\TDATA\\45410066"));
    teardown();
}

/*
 * 16 link handles may be live at once and the 17th is refused. A closed one frees a slot,
 * because the table asks the object table which handles are alive.
 *
 * MUTATION: never reclaim, or reclaim without checking, and the final open fails.
 */
static void test_closing_a_link_handle_frees_its_slot(void)
{
    setup_links();
    CHECK(kernel_object_register() > 0u);
    uint32_t first = 0u;
    for (unsigned i = 0u; i < 16u; i++) {
        CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
        if (i == 0u) {
            first = opened_handle();
        }
    }
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 16u);
    write_u32_at(at(HANDLE_OFFSET), 0xA5A5A5A5u);
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0xC000009Au);
    CHECK_EQ_U32(opened_handle(), 0xA5A5A5A5u);

    const uint32_t close_args[1] = {first};
    CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 15u);
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 16u);
    teardown();
}

/* kernel_file_reset must forget link handles even when the object table still has them. */
static void test_reset_forgets_link_handles(void)
{
    setup_links();
    CHECK_EQ_U32(open_link(LINK_NAME, 0u), 0x00000000u);
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 1u);
    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_symlink_handle_count(), 0u);
    teardown();
}

int main(void)
{
    printf("kernel file (NtOpenFile, symbolic links) tests\n");

    test_registration_makes_the_ordinal_implemented();
    test_an_implemented_ordinal_does_not_report_itself_as_a_stub();
    test_a_name_with_no_volume_is_refused_and_writes_no_handle();
    test_an_openable_name_yields_a_real_file_handle();
    test_six_arguments_are_required();
    test_the_six_arguments_are_in_the_measured_order();
    test_the_handle_slot_may_alias_the_slot_holding_the_name_pointer();
    test_the_name_length_is_sixteen_bits_not_thirty_two();
    test_the_name_is_taken_by_length_and_not_by_a_nul();
    test_a_relative_open_is_refused_and_counted();
    test_matching_is_case_insensitive();
    test_the_empty_policy_fabricates_and_says_so();
    test_repeated_opens_of_one_name_are_one_record();
    test_missing_parameters_are_reported();
    test_a_nameless_object_attributes_is_refused();
    test_the_openable_set_refuses_what_it_cannot_hold();
    test_reset_clears_the_set_the_record_and_the_policy();

    test_the_symbolic_link_ordinals_are_implemented();
    test_the_guests_own_open_then_query_round_trips();
    test_returned_length_is_a_dword_without_the_nul();
    test_only_the_characters_are_written_and_the_incoming_length_is_ignored();
    test_the_capacity_boundary_is_the_exact_target_length();
    test_an_unusable_output_string_is_refused();
    test_a_link_the_title_never_created_is_not_found();
    test_the_link_name_is_matched_whole_not_as_a_prefix();
    test_an_unwritable_handle_slot_is_reported();
    test_the_link_name_is_case_insensitive();
    test_a_relative_link_open_is_refused_and_counted();
    test_an_attributes_value_the_host_does_not_model_is_counted_and_reported();
    test_each_ordinal_refuses_a_frame_that_is_one_argument_short();
    test_a_query_on_the_wrong_handle_is_refused_by_kind();
    test_a_handle_keeps_its_own_target_after_the_link_is_deleted();
    test_closing_a_link_handle_frees_its_slot();
    test_reset_forgets_link_handles();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
