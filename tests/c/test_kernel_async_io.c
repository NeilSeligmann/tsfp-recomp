/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T743: asynchronous (overlapped) NtReadFile completion on the virtual clock, `--async-file-io`.
 *
 * The contract under test (src/xbox/kernel_async_io.h):
 *   - with the flag off every read is synchronous; a successful read signals its Event after
 *     the buffer and IoStatusBlock, matching the T763 xemu reference;
 *   - with it on, a read that supplies an Event on a handle opened WITHOUT FILE_SYNCHRONOUS_IO
 *     returns STATUS_PENDING and touches neither the guest buffer, the IoStatusBlock nor the
 *     Event; the request completes on a serial drive when the virtual clock reaches
 *     access + bytes / rate, serviced at a virtual vblank (the clock frame hook) or at the next
 *     NtReadFile, and then the buffer, IoStatusBlock {STATUS_SUCCESS, bytes} and Event are set;
 *   - a synchronous handle, or a read with no Event, stays synchronous.
 *
 * T764 adds the blocking waits (NtWaitForSingleObject(Ex) on the Event of a pending read, or on the
 * file object of an Event-less one with `--async-file-io-file-object`) that advance the virtual
 * clock to the due time of the awaited request and never past it, the file object signal, and the
 * by-name refusal of an ApcRoutine.
 *
 * Every guest structure is built in scratch guest memory, the file is a real host file behind a
 * host-directory volume, the frames are synthesised. EVERY CHECK is mutation-tested
 * (tools/mutate/sets/kernel_async_io.py), each test says what breaks it.
 */

#include "kernel_async_io.h"

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "nt_status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                               \
        uint32_t e_ = (uint32_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)a_, (unsigned)e_);                                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint64_t a_ = (uint64_t)(actual);                                               \
        uint64_t e_ = (uint64_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,        \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);             \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

static char captured[65536];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len, format, args);
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
#define ORD_NT_READ_FILE 219u
#define HDD_DEVICE "\\Device\\Harddisk0\\partition1"
#define GUEST_FILE HDD_DEVICE "\\music.bin"
#define FILE_BYTES 300000u
#define IMPLICIT_OFFSET UINT64_MAX
#define OPEN_ASYNC 0x48u
#define OPEN_SYNC 0x68u

#define SCRATCH_BYTES 0x80000u
#define OFF_FRAME 0x0000u
#define OFF_OA 0x0200u
#define OFF_STRING 0x0220u
#define OFF_CHARS 0x0280u
#define OFF_IOSB 0x0400u
#define OFF_HANDLE 0x0440u
#define OFF_EVENT 0x0460u
#define OFF_IOSB2 0x0480u
#define OFF_BYTE_OFFSET 0x0500u
#define OFF_TIMEOUT 0x0600u
#define OFF_EVENT2 0x0620u
#define OFF_DUP 0x0640u
#define OFF_BUFFER 0x1000u
#define OFF_BUFFER2 0x20000u

static kernel_guest_ptr scratch;
static char host_root[256];
static bool host_root_ready;
static uint8_t file_bytes[FILE_BYTES];

static kernel_guest_ptr at(uint32_t offset)
{
    return scratch + offset;
}

static uint32_t read32(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

static void write32(kernel_guest_ptr address, uint32_t value)
{
    CHECK(kernel_guest_write_u32(address, value));
}

static void remove_host_root(void)
{
    if (!host_root_ready) {
        return;
    }
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/music.bin", host_root);
    (void)unlink(path);
    (void)rmdir(host_root);
    host_root_ready = false;
}

static void make_host_root(void)
{
    char exe[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
    if (length <= 0) {
        printf("FATAL could not locate the test binary\n");
        exit(EXIT_FAILURE);
    }
    exe[length] = '\0';
    char *slash = strrchr(exe, '/');
    *slash = '\0';
    (void)snprintf(host_root, sizeof(host_root), "%s/tsfp-async-io-XXXXXX", exe);
    if (mkdtemp(host_root) == NULL) {
        printf("FATAL could not create a scratch directory: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    host_root_ready = true;
    for (uint32_t i = 0u; i < FILE_BYTES; i++) {
        file_bytes[i] = (uint8_t)((i * 7u + (i >> 8)) & 0xFFu);
    }
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/music.bin", host_root);
    FILE *file = fopen(path, "wb");
    if (file == NULL || fwrite(file_bytes, 1u, FILE_BYTES, file) != FILE_BYTES) {
        printf("FATAL could not write the host file\n");
        exit(EXIT_FAILURE);
    }
    (void)fclose(file);
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(OFF_FRAME), 0x100u, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    frame.stack_limit = at(OFF_FRAME) + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

static void setup(void)
{
    kernel_hle_init();
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    kernel_clock_reset();
    kernel_async_io_set_enabled(false);
    kernel_async_io_set_file_object_enabled(false);
    kernel_clock_set_frame_hook(NULL);
    guest_mem_reset();
    CHECK_EQ_U32(kernel_file_register(), 7u);
    CHECK_EQ_U32(kernel_io_register(), 10u);
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
        printf("FATAL could not allocate scratch\n");
        exit(EXIT_FAILURE);
    }
    make_host_root();
    CHECK(kernel_file_mount_host_dir(HDD_DEVICE, host_root));
    /* An EVENT handle the way NtCreateEvent makes one: Type 1, not signalled. */
    CHECK_EQ_U32(kernel_object_create_event(1u, 0u, at(OFF_EVENT)), STATUS_SUCCESS);
}

static void teardown(void)
{
    kernel_clock_set_frame_hook(NULL);
    kernel_async_io_set_enabled(false);
    kernel_async_io_set_file_object_enabled(false);
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
    remove_host_root();
}

static uint32_t event_handle(void)
{
    return read32(at(OFF_EVENT));
}

static bool event_signalled(void)
{
    bool signalled = false;
    CHECK(kernel_object_event_signaled(event_handle(), &signalled));
    return signalled;
}

/* NtOpenFile the file with `options`, the handle in OFF_HANDLE. */
static uint32_t open_file(uint32_t options)
{
    const char *path = GUEST_FILE;
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        CHECK(kernel_guest_write_u8(at(OFF_CHARS) + (uint32_t)i, (uint8_t)path[i]));
    }
    CHECK(kernel_guest_write_u8(at(OFF_STRING) + 0u, (uint8_t)(length & 0xFFu)));
    CHECK(kernel_guest_write_u8(at(OFF_STRING) + 1u, (uint8_t)(length >> 8)));
    CHECK(kernel_guest_write_u8(at(OFF_STRING) + 2u, (uint8_t)((length + 1u) & 0xFFu)));
    CHECK(kernel_guest_write_u8(at(OFF_STRING) + 3u, (uint8_t)((length + 1u) >> 8)));
    write32(at(OFF_STRING) + 4u, at(OFF_CHARS));
    write32(at(OFF_OA) + 0u, 0u);
    write32(at(OFF_OA) + 4u, at(OFF_STRING));
    write32(at(OFF_OA) + 8u, GUEST_OBJ_ATTRIBUTES_OBSERVED);
    const uint32_t args[6] = {at(OFF_HANDLE), 0x80100080u, at(OFF_OA), at(OFF_IOSB2), 1u, options};
    return call_ordinal(ORD_NT_OPEN_FILE, args, 6u);
}

static void poison(uint32_t offset, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes; i += 4u) {
        write32(at(offset) + i, 0xA5A5A5A5u);
    }
}

/* The way the title's ReadFile wrapper 0x37CC08 calls it: IoStatusBlock pre-set to PENDING, the
 * OVERLAPPED's own event, an absolute 64-bit offset. */
static uint32_t read_file(uint32_t handle, uint32_t event, kernel_guest_ptr iosb,
                          kernel_guest_ptr buffer, uint32_t length, uint64_t offset)
{
    write32(iosb, STATUS_PENDING);
    write32(iosb + 4u, 0xA5A5A5A5u);
    write32(at(OFF_BYTE_OFFSET), (uint32_t)(offset & 0xFFFFFFFFu));
    write32(at(OFF_BYTE_OFFSET) + 4u, (uint32_t)(offset >> 32));
    /* IMPLICIT_OFFSET is a NULL ByteOffset: read at the handle's position and advance it. */
    const uint32_t args[8] = {handle,
                              event,
                              0u,
                              0u,
                              iosb,
                              buffer,
                              length,
                              offset == IMPLICIT_OFFSET ? 0u : at(OFF_BYTE_OFFSET)};
    return call_ordinal(ORD_NT_READ_FILE, args, 8u);
}

static bool buffer_matches(kernel_guest_ptr buffer, uint32_t from, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        uint8_t value = 0u;
        if (!kernel_guest_read_u8(buffer + i, &value) || value != file_bytes[from + i]) {
            return false;
        }
    }
    return true;
}

static bool buffer_untouched(kernel_guest_ptr buffer, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i += 4u) {
        if (read32(buffer + i) != 0xA5A5A5A5u) {
            return false;
        }
    }
    return true;
}

/*
 * Flag off: the read is synchronous. The data and successful IoStatusBlock are written before
 * the supplied Event HANDLE is signalled, and nothing is queued.
 *
 * MUTATION: make eligible() ignore the flag and the call returns PENDING here.
 */
static void test_flag_off_is_the_synchronous_read(void)
{
    setup();
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x2000u);
    CHECK(!kernel_async_io_enabled());
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0x1000u, 0x800u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0x1000u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0x800u, 0x1000u));
    CHECK(event_signalled());
    CHECK(!captured_contains("which is NOT signalled"));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    teardown();
}

/*
 * T785 buffered asynchronous handle (FILE_NON_DIRECTORY_FILE but no FILE_NO_INTERMEDIATE_BUFFERING):
 * despite async policy being enabled, NtReadFile completes successfully in the call and sets the
 * Event HANDLE after its buffer and IOSB. A second handle remains clear; the handle value is not
 * confused with a guest address. A failure before the file read is accepted leaves its Event clear.
 *
 * MUTATIONS: omit success signaling, signal by guest address, signal every Event, or signal an
 * invalid-handle failure.
 */
static void test_buffered_async_read_signals_only_its_event_after_sync_success(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(0x40u), STATUS_SUCCESS);
    const uint32_t selected_event = event_handle();
    CHECK(selected_event != at(OFF_EVENT));
    CHECK_EQ_U32(kernel_object_create_event(1u, 0u, at(OFF_EVENT2)), STATUS_SUCCESS);
    const uint32_t neighbor_event = read32(at(OFF_EVENT2));
    CHECK(neighbor_event != selected_event);
    CHECK(!kernel_async_io_eligible(0x40u, selected_event));
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), selected_event, at(OFF_IOSB), at(OFF_BUFFER),
                           0x1000u, 0x800u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0x1000u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0x800u, 0x1000u));
    CHECK(event_signalled());
    bool acquired = false;
    CHECK_EQ_U32(kernel_object_event_try_wait(selected_event, &acquired), STATUS_SUCCESS);
    CHECK(acquired);
    CHECK(!event_signalled());
    bool neighbor_signalled = true;
    CHECK(kernel_object_event_signaled(neighbor_event, &neighbor_signalled));
    CHECK(!neighbor_signalled);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    CHECK(!captured_contains("NOT signalled"));

    poison(OFF_BUFFER2, 0x1000u);
    CHECK_EQ_U32(read_file(0xDEADu, neighbor_event, at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER2), 0x1000u));
    neighbor_signalled = true;
    CHECK(kernel_object_event_signaled(neighbor_event, &neighbor_signalled));
    CHECK(!neighbor_signalled);
    teardown();
}

/* T1011: the published original probe distinguishes immediate EOF from validation
 * rejection. Positive-length EOF returns END_OF_FILE, clears only its selected Event,
 * and leaves IOSB and destination bytes alone. Invalid file/Event and wrong-kind Event
 * reject without publishing IOSB or changing a pre-signaled Event. Zero-length EOF and
 * a positive short read remain successful completions. */
static void test_eof_and_validation_preserve_original_event_iosb_contract(void)
{
    setup();
    CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_event_set(event_handle(), NULL), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_create_event(1u, 0u, at(OFF_EVENT2)), STATUS_SUCCESS);
    const uint32_t neighbor = read32(at(OFF_EVENT2));
    CHECK_EQ_U32(kernel_object_event_set(neighbor, NULL), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x100u);

    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           16u, FILE_BYTES + 0x100u),
                 STATUS_END_OF_FILE);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 16u));
    CHECK(!event_signalled());
    bool neighbor_signaled = false;
    CHECK(kernel_object_event_signaled(neighbor, &neighbor_signaled));
    CHECK(neighbor_signaled);

    /* Validation failures preserve a pre-signaled Event and both IOSB words. */
    CHECK_EQ_U32(kernel_object_event_set(event_handle(), NULL), STATUS_SUCCESS);
    CHECK_EQ_U32(read_file(0xDEADu, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 0xA5A5A5A5u);
    CHECK(event_signalled());
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), 0xDEADu, at(OFF_IOSB2), at(OFF_BUFFER), 16u,
                           0u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 0xA5A5A5A5u);
    CHECK(event_signalled());
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), read32(at(OFF_HANDLE)), at(OFF_IOSB2),
                           at(OFF_BUFFER), 16u, 0u),
                 STATUS_OBJECT_TYPE_MISMATCH);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 0xA5A5A5A5u);
    CHECK(event_signalled());

    /* Zero-byte requests past EOF are still successful and signal the supplied Event. */
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0u, FILE_BYTES + 0x100u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0u);
    CHECK(event_signalled());

    /* A positive short read at EOF remains a successful 8-byte transfer. */
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           16u, FILE_BYTES - 8u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 8u);
    CHECK(buffer_matches(at(OFF_BUFFER), FILE_BYTES - 8u, 8u));
    CHECK_EQ_U32(read32(at(OFF_BUFFER) + 8u), 0xA5A5A5A5u);
    CHECK(event_signalled());
    teardown();
}

/* Unbuffered async EOF is an immediate measured error, not an empty queued completion. */
static void test_async_eof_is_not_reported_as_pending(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_event_set(event_handle(), NULL), STATUS_SUCCESS);
    poison(OFF_BUFFER, 16u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           16u, FILE_BYTES),
                 STATUS_END_OF_FILE);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 16u));
    CHECK(!event_signalled());
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);

    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0u, FILE_BYTES + 0x100u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0u);
    CHECK(event_signalled());
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    teardown();
}

/* A closed original is not a live handle merely because its FILE duplicate keeps the
 * shared open identity alive. The closed value must retain the T1007 invalid-handle
 * boundary while the duplicate remains usable. */
static void test_closed_file_handle_rejected_while_duplicate_remains_live(void)
{
    setup();
    (void)kernel_object_register();
    CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
    const uint32_t original = read32(at(OFF_HANDLE));
    const uint32_t duplicate_args[3] = {original, at(OFF_DUP), 2u};
    CHECK_EQ_U32(call_ordinal(197u, duplicate_args, 3u), STATUS_SUCCESS);
    const uint32_t duplicate = read32(at(OFF_DUP));
    CHECK(duplicate != 0u && duplicate != original);

    const uint32_t close_args[1] = {original};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(read_file(original, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 0xA5A5A5A5u);
    CHECK(!event_signalled());

    CHECK_EQ_U32(read_file(duplicate, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 16u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 16u));
    CHECK(event_signalled());
    teardown();
}

/*
 * Flag on, asynchronous handle, Event supplied: PENDING, and the guest buffer, the IoStatusBlock
 * and the Event are all untouched until the drive finishes. Then ONE virtual vblank (60 Hz, 16.7 ms
 * against 2 ms + 4096 bytes at 6.9 MB/s) completes it through the clock hook: buffer, IoStatusBlock
 * {SUCCESS, bytes} and the Event, in one step.
 *
 * MUTATIONS: write the buffer or the IoStatusBlock at issue, complete without setting the Event,
 * return SUCCESS, never call the hook, report the requested length instead of the transferred one.
 */
static void test_an_overlapped_read_is_pending_then_completes_on_a_frame(void)
{
    setup();
    CHECK(kernel_async_io_idle()); /* T1289: nothing queued after a reset */
    kernel_async_io_set_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0x1000u, 0x800u),
                 STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 0x1000u));
    CHECK(!event_signalled());
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK(!kernel_async_io_idle()); /* T1289: the safepoint's lock free test follows the queue */
    CHECK_EQ_U32(kernel_async_io_service(), 0u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK(kernel_clock_frame(60u));
    CHECK(kernel_async_io_idle());
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0x1000u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0x800u, 0x1000u));
    CHECK(event_signalled());
    CHECK_EQ_U32(kernel_async_io_get_stats().completed, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    CHECK_EQ_U64(kernel_async_io_get_stats().bytes, 0x1000u);
    teardown();
}

/*
 * A request is not complete before its time: the 299,200 byte read takes the HDD volume's access
 * plus transfer time (xemu-level, T763), the service finds it pending 20 us before and complete
 * 20 us after, and the due time is the documented rounded-up formula, so it never completes early.
 * The clock is advanced with stalls so the check does not depend on the frame rate.
 *
 * MUTATION: drop the transfer term, drop the access term, round the transfer down.
 */
static void test_completion_waits_for_access_plus_transfer_time(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x20u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER2),
                           FILE_BYTES - 0x800u, 0x800u),
                 STATUS_PENDING);
    const uint64_t ticks = kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, FILE_BYTES - 0x800u);
    const uint32_t due_us = (uint32_t)(ticks * 1000000u / KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK(due_us > 100u);
    (void)kernel_clock_stall_us(due_us - 20u);
    CHECK_EQ_U32(kernel_async_io_service(), 0u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK(!event_signalled());
    (void)kernel_clock_stall_us(40u);
    CHECK_EQ_U32(kernel_async_io_service(), 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), FILE_BYTES - 0x800u);
    CHECK(buffer_matches(at(OFF_BUFFER2), 0x800u, FILE_BYTES - 0x800u));
    CHECK(event_signalled());
    /* The size term is monotonic and never zero (the access time is always paid), rounded up. */
    const uint64_t access_hdd = ((uint64_t)KERNEL_ASYNC_IO_HDD_ACCESS_US * KERNEL_CLOCK_FREQUENCY_HZ + 999999u) / 1000000u;
    const uint64_t access_disc = ((uint64_t)KERNEL_ASYNC_IO_DISC_ACCESS_US * KERNEL_CLOCK_FREQUENCY_HZ + 999999u) / 1000000u;
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0u), access_hdd);
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, 0u), access_disc);
    CHECK(access_hdd > 0u);
    CHECK(access_disc > 0u);
    /* One byte is a fraction of a tick per byte of transfer, rounded UP to a whole tick, never down. */
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 1u) -
                     kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0u),
                 (KERNEL_CLOCK_FREQUENCY_HZ + KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND - 1u) /
                     KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND);
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, 1u) -
                     kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, 0u),
                 (KERNEL_CLOCK_FREQUENCY_HZ + KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND - 1u) /
                     KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND);
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND) -
                     access_hdd,
                 KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK_EQ_U64(kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND) -
                     access_disc,
                 KERNEL_CLOCK_FREQUENCY_HZ);
    teardown();
}

/*
 * One drive: a second request starts when the first finishes, so it completes later by its own
 * service time even though it was issued at the same instant. Two 100,000 byte reads take one
 * service time each: 20 us after the first is due only the first is done, 20 us after the second
 * is due both.
 *
 * MUTATION: start every request at "now" (parallel drive), complete newest first.
 */
static void test_the_drive_is_serial_and_in_issue_order(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x20u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER2), 100000u, 0u),
                 STATUS_PENDING);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER2) + 100000u, 100000u,
                           100000u),
                 STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 2u);
    const uint64_t ticks = kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 100000u);
    const uint32_t each_us = (uint32_t)(ticks * 1000000u / KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK(each_us > 100u);
    (void)kernel_clock_stall_us(each_us + 20u);
    CHECK_EQ_U32(kernel_async_io_service(), 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    (void)kernel_clock_stall_us(each_us);
    CHECK_EQ_U32(kernel_async_io_service(), 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB2) + 4u), 100000u);
    CHECK(buffer_matches(at(OFF_BUFFER2) + 100000u, 100000u, 100000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().completed, 2u);
    teardown();
}

/*
 * Each volume has its own timing and the volume comes from the file's path: the HDD for the
 * \Device\Harddisk and C: E: F: X: Y: Z: spellings, the disc for D:, \Device\CdRom and anything else
 * (T763). A request submitted directly with the disc volume is due at the disc's own time, which
 * is not the HDD's: a mutant that ignores the volume completes it at the other drive's time.
 *
 * MUTATION: always use the HDD constants, always use the disc constants, classify every path as
 * the disc, drop the case folding, drop the drive-letter forms.
 */
static void test_each_volume_has_its_own_timing_and_paths_pick_the_volume(void)
{
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\Device\\Harddisk0\\Partition1\\music.bin"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\DEVICE\\HARDDISK0\\partition2\\a"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\??\\E:\\save.dat"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\??\\z:\\cache"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\DosDevices\\C:\\x"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("F:\\x"), KERNEL_ASYNC_IO_VOLUME_HDD);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\??\\D:\\pak\\musicts.pak"), KERNEL_ASYNC_IO_VOLUME_DISC);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\Device\\CdRom0\\default.xbe"), KERNEL_ASYNC_IO_VOLUME_DISC);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("\\pak\\musicts.pak"), KERNEL_ASYNC_IO_VOLUME_DISC);
    CHECK_EQ_U32(kernel_async_io_volume_of_path("E"), KERNEL_ASYNC_IO_VOLUME_DISC);
    CHECK_EQ_U32(kernel_async_io_volume_of_path(""), KERNEL_ASYNC_IO_VOLUME_DISC);
    CHECK_EQ_U32(kernel_async_io_volume_of_path(NULL), KERNEL_ASYNC_IO_VOLUME_DISC);

    /* The two volumes differ at the sizes the title reads (0x12000), by the measured amounts. */
    const uint64_t disc = kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, 0x12000u);
    const uint64_t hdd = kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x12000u);
    CHECK(disc != hdd);

    setup();
    kernel_async_io_set_enabled(true);
    uint8_t *copy = malloc(0x12000u);
    CHECK(copy != NULL);
    if (copy != NULL) {
        memset(copy, 0x5A, 0x12000u);
        kernel_async_read request = {
            .file_handle = 0u,
            .event_handle = 0u,
            .io_status = at(OFF_IOSB),
            .buffer = at(OFF_BUFFER),
            .data = copy,
            .length = 0x12000u,
            .requested = 0x12000u,
            .volume = KERNEL_ASYNC_IO_VOLUME_DISC,
        };
        write32(at(OFF_IOSB), STATUS_PENDING);
        CHECK(kernel_async_io_submit(&request));
        const uint32_t disc_us = (uint32_t)(disc * 1000000u / KERNEL_CLOCK_FREQUENCY_HZ);
        const uint32_t hdd_us = (uint32_t)(hdd * 1000000u / KERNEL_CLOCK_FREQUENCY_HZ);
        const uint32_t gap = disc_us > hdd_us ? disc_us - hdd_us : hdd_us - disc_us;
        CHECK(gap > 40u);
        /* Halfway between the two volumes' times: the request is done only if the DISC is the faster one. */
        const uint32_t middle = (disc_us + hdd_us) / 2u;
        (void)kernel_clock_stall_us(middle);
        const bool disc_is_faster = disc_us < hdd_us;
        CHECK_EQ_U32(kernel_async_io_service(), disc_is_faster ? 1u : 0u);
        CHECK_EQ_U32(read32(at(OFF_IOSB)), disc_is_faster ? STATUS_SUCCESS : STATUS_PENDING);
        /* Past both times it is done whichever volume is used. */
        (void)kernel_clock_stall_us(gap + 20u);
        (void)kernel_async_io_service();
        CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    }
    teardown();
}

/*
 * Only an asynchronous handle with an Event queues. A synchronous handle (FILE_SYNCHRONOUS_IO_
 * NONALERT 0x20 in the options) and a read with no Event stay synchronous and say so, nothing is
 * submitted.
 *
 * MUTATION: ignore the options bits, or the Event test.
 */
static void test_synchronous_handles_and_eventless_reads_stay_synchronous(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0x1000u, 0u),
                 STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), 0u, at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u),
                 STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    teardown();
}

/*
 * End of file: the transferred count is the short one (information 1,000 of 4,096 asked), the
 * request completes with STATUS_SUCCESS, and an implicit-offset read advances the file position at
 * issue by what was read, so the next implicit read starts there.
 *
 * MUTATION: report the requested length, or advance by the requested length.
 */
static void test_a_short_read_reports_the_transferred_bytes(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           0x1000u, FILE_BYTES - 1000u),
                 STATUS_PENDING);
    CHECK(kernel_clock_frame(60u));
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 1000u);
    CHECK(buffer_matches(at(OFF_BUFFER), FILE_BYTES - 1000u, 1000u));
    /* The bytes past the transferred count were not written. */
    CHECK_EQ_U32(read32(at(OFF_BUFFER) + 1004u), 0xA5A5A5A5u);
    teardown();
}

/*
 * The queue is bounded: the 33rd pending request is refused with STATUS_INSUFFICIENT_RESOURCES, the
 * IoStatusBlock says so, and the refusal is counted and logged by name, never dropped.
 *
 * MUTATION: raise the bound, or drop the refusal.
 */
static void test_a_full_queue_is_refused_by_name(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    for (unsigned i = 0u; i < KERNEL_ASYNC_IO_QUEUE; i++) {
        CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u),
                     STATUS_PENDING);
    }
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u),
                 STATUS_INSUFFICIENT_RESOURCES);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_INSUFFICIENT_RESOURCES);
    CHECK(captured_contains("asynchronous queue is full"));
    CHECK_EQ_U32(kernel_async_io_get_stats().refused, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, KERNEL_ASYNC_IO_QUEUE);
    kernel_io_reset();
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    teardown();
}

/*
 * The service also runs at the next NtReadFile, so a title that never sees a vblank still makes
 * progress when it reads again: a clock that moved past the due time completes the first request
 * before the second is queued.
 *
 * MUTATION: drop the service call at the top of the read.
 */
static void test_the_next_read_services_what_is_due(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u),
                 STATUS_PENDING);
    CHECK(kernel_clock_frame(60u));
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0x1000u),
                 STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_async_io_get_stats().completed, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    teardown();
}


/*
 * A NULL ByteOffset reads at the handle's position and ADVANCES it at issue by what was read, so
 * two implicit reads in a row return consecutive data even though neither has completed.
 *
 * MUTATION: do not advance, advance by the requested length.
 */
static void test_an_implicit_offset_read_advances_the_position_at_issue(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x400u, IMPLICIT_OFFSET),
                 STATUS_PENDING);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB2), at(OFF_BUFFER) + 0x400u, 0x400u,
                           IMPLICIT_OFFSET),
                 STATUS_PENDING);
    CHECK(kernel_clock_frame(60u));
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x800u));
    teardown();
}

/*
 * A read larger than the queue's bound (16 MiB) stays synchronous and signals its Event on
 * success; a destination the guest does not map is refused at issue with the IoStatusBlock
 * written, nothing queued.
 *
 * MUTATION: drop the size bound, drop the mapped-destination probe.
 */
static void test_oversize_and_unmapped_reads_do_not_queue(void)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER),
                           KERNEL_ASYNC_IO_MAX_BYTES + 1u, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    CHECK(event_signalled());
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB2), at(0u) + SCRATCH_BYTES + 0x100000u,
                           0x1000u, 0u),
                 STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    teardown();
}

/* ---- T764: blocking waits, the file object, the ApcRoutine refusal ---- */

#define ORD_NT_CLOSE 187u
#define ORD_NT_DUPLICATE_OBJECT 197u
#define ORD_NT_WAIT_FOR_SINGLE_OBJECT_EX 234u
#define STATUS_WAIT_TIMEOUT_VALUE 0x102u

static jmp_buf wait_jump;
static unsigned refusals;
static uint32_t refused_handle;
static kernel_thread_wait_refusal refused_reason;

static void refuse_wait(uint32_t handle, kernel_thread_wait_refusal reason)
{
    refusals++;
    refused_handle = handle;
    refused_reason = reason;
    longjmp(wait_jump, 1);
}

static const kernel_thread_host_ops refusing_ops = {.wait_refused = refuse_wait};

static void setup_waits(void)
{
    setup();
    kernel_sync_reset();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(&refusing_ops));
    (void)kernel_object_register();
    (void)kernel_thread_register();
    refusals = 0u;
    refused_handle = 0u;
    refused_reason = (kernel_thread_wait_refusal)99;
    CHECK_EQ_U32(kernel_object_create_event(1u, 0u, at(OFF_EVENT2)), STATUS_SUCCESS);
}

static void teardown_waits(void)
{
    CHECK(kernel_thread_set_host_ops(NULL));
    teardown();
}

static uint32_t event2_handle(void)
{
    return read32(at(OFF_EVENT2));
}

static bool event_state(uint32_t handle)
{
    bool signalled = false;
    CHECK(kernel_object_event_signaled(handle, &signalled));
    return signalled;
}

/* The wait the retail wrapper 0x380029 makes: handle, WaitMode 1, Alertable 0, `timeout` pointer.
 * `*refused` is set when the host would have stopped the run (the real host aborts there). */
static uint32_t wait_on(uint32_t handle, kernel_guest_ptr timeout, bool *refused)
{
    uint32_t status = 0xDEADBEEFu;
    const unsigned before = refusals;
    if (setjmp(wait_jump) == 0) {
        const uint32_t args[4] = {handle, 1u, 0u, timeout};
        status = call_ordinal(ORD_NT_WAIT_FOR_SINGLE_OBJECT_EX, args, 4u);
    }
    if (refused != NULL) {
        *refused = refusals != before;
    }
    return status;
}

static uint32_t wait_ok(uint32_t handle, kernel_guest_ptr timeout)
{
    bool refused = false;
    const uint32_t status = wait_on(handle, timeout, &refused);
    CHECK(!refused);
    return status;
}

/* A relative timeout in 100 ns units (the guest passes a negative LARGE_INTEGER). */
static kernel_guest_ptr relative_timeout(int64_t units_100ns)
{
    const int64_t value = -units_100ns;
    CHECK(kernel_guest_write_bytes(at(OFF_TIMEOUT), &value, sizeof(value)));
    return at(OFF_TIMEOUT);
}

/*
 * A blocking wait (NULL timeout) on the Event of a pending read advances the virtual clock to the due
 * time of THAT request, exactly, completes it (buffer, IoStatusBlock, Event) and returns
 * STATUS_SUCCESS, consuming the auto-reset Event. The drive is serial, so with a second request queued
 * behind it the clock stops at the first one's due time and the second stays pending. Waiting on the
 * second then advances to its own due time.
 *
 * MUTATIONS: advance to the due time plus a tick or to the second request's, complete without
 * advancing the clock, do not service after the advance, leave the Event signalled, return a timeout.
 */
static void test_a_blocking_wait_advances_to_the_due_time_and_not_past_it(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    poison(OFF_BUFFER2, 0x2000u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0x800u),
                 STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    CHECK_EQ_U32(read_file(handle, event2_handle(), at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0x1800u),
                 STATUS_PENDING);
    const uint64_t first_due = issued + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    const uint64_t second_due = first_due + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    CHECK(second_due > first_due && first_due > issued);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    CHECK_EQ_U32(wait_ok(event_handle(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), first_due);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0x1000u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0x800u, 0x1000u));
    CHECK(!event_state(event_handle()));
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK(!event_state(event2_handle()));
    CHECK(buffer_untouched(at(OFF_BUFFER2), 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().completed, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 1u);
    CHECK_EQ_U32(wait_ok(event2_handle(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), second_due);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER2), 0x1800u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 2u);
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * A wait whose finite relative timeout ends before the due time advances the clock to the DEADLINE
 * only (50 us is 36,667 ticks, rounded up), times out with STATUS_TIMEOUT and leaves the request
 * pending and the Event clear. A longer timeout then completes at the due time, not at the deadline.
 *
 * MUTATIONS: advance to the due time anyway, round the deadline down, skip the timeout branch, count
 * a timeout as a wait.
 */
static void test_a_finite_timeout_ends_the_wait_at_the_deadline(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u),
                 STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    const uint64_t due = issued + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    /* 50 us is 36,666.7 ticks, rounded UP to 36,667, and a 4 KiB read takes 85 us plus 3.4 us. */
    CHECK_EQ_U32(wait_ok(event_handle(), relative_timeout(500)), STATUS_WAIT_TIMEOUT_VALUE);
    CHECK_EQ_U64(kernel_clock_peek(), issued + 36667u);
    CHECK(kernel_clock_peek() < due);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK(!event_state(event_handle()));
    CHECK_EQ_U32(kernel_async_io_get_stats().wait_timeouts, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 0u);
    CHECK_EQ_U32(wait_ok(event_handle(), relative_timeout(200000)), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), due);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 1u);
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * While a timed out wait passes the clock forward, a request that was due BEFORE the deadline completes
 * on the way (here a first request on another Event, due at 88 us of a 100 us timeout on the second, due at 177 us): its
 * Event is set and its IoStatusBlock written, the awaited second request is still pending.
 *
 * MUTATION: skip the service after advancing to the deadline.
 */
static void test_a_timeout_completes_the_requests_due_before_the_deadline(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    poison(OFF_BUFFER2, 0x2000u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u), STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    CHECK_EQ_U32(read_file(handle, event2_handle(), at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0x1000u),
                 STATUS_PENDING);
    const uint64_t first_due = issued + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    /* 100 us is 73,333.3 ticks, rounded up to 73,334: after the first request, before the second. */
    CHECK(first_due < issued + 73334u);
    CHECK(first_due + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u) > issued + 73334u);
    CHECK_EQ_U32(wait_ok(event2_handle(), relative_timeout(1000)), STATUS_WAIT_TIMEOUT_VALUE);
    CHECK_EQ_U64(kernel_clock_peek(), issued + 73334u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK(event_state(event_handle()));
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_PENDING);
    CHECK(!event_state(event2_handle()));
    CHECK_EQ_U32(kernel_async_io_get_stats().completed, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * A poll (a timeout pointer to zero) never advances the clock: a request not yet due is a
 * STATUS_TIMEOUT with the clock where it was. Once the clock is past the due time (a frame raised the
 * floor, no frame hook installed) the same poll completes the request itself and succeeds.
 *
 * MUTATIONS: a poll that advances to the due time, a poll that does not service what is due.
 */
static void test_a_poll_completes_only_what_is_already_due(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u),
                 STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    CHECK_EQ_U32(wait_ok(event_handle(), relative_timeout(0)), STATUS_WAIT_TIMEOUT_VALUE);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK(kernel_clock_frame(60u));
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(wait_ok(event_handle(), relative_timeout(0)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK(!event_state(event_handle()));
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * A wait nothing pending would end is NOT this model's: an Event no request signals (another Event's
 * request is pending) is refused by name as a wait that would block, the clock does not move and the
 * other request is untouched. Off, a wait on a pending request's event is the same named refusal. An
 * Event already signalled is satisfied at once with no clock change.
 *
 * MUTATIONS: match every pending request, ignore the flag, advance on a refusal.
 */
static void test_a_wait_nothing_pending_would_end_is_refused_by_name(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u),
                 STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    bool refused = false;
    (void)wait_on(event2_handle(), 0u, &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK_EQ_U32(refused_handle, event2_handle());
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK(kernel_async_io_wait_event(event2_handle(), false, 0u) == KERNEL_ASYNC_WAIT_NONE);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    /* An Event read never signals a file object, not even the (impossible) handle 0 that an Event read's
     * empty file identity would equal. */
    CHECK(kernel_async_io_wait_file(0u, false, 0u) == KERNEL_ASYNC_WAIT_NONE);
    CHECK(kernel_async_io_wait_file(read32(at(OFF_HANDLE)), false, 0u) == KERNEL_ASYNC_WAIT_NONE);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    /* Flag off: the API is inert even for an Event a request would signal. */
    kernel_async_io_set_enabled(false);
    CHECK(kernel_async_io_wait_event(event_handle(), false, 0u) == KERNEL_ASYNC_WAIT_NONE);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    (void)wait_on(event_handle(), 0u, &refused);
    CHECK(refused);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    kernel_async_io_set_enabled(true);
    /* Flag off: a wait on a FILE handle keeps its original named scope refusal, nothing is served. */
    kernel_async_io_set_enabled(false);
    (void)wait_on(read32(at(OFF_HANDLE)), 0u, &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    kernel_async_io_set_enabled(true);
    /* Absolute timeouts are host wall time: refused by name, for an Event and for a file. */
    const int64_t absolute = 1000;
    CHECK(kernel_guest_write_bytes(at(OFF_TIMEOUT), &absolute, sizeof(absolute)));
    (void)wait_on(read32(at(OFF_HANDLE)), at(OFF_TIMEOUT), &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_reason, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    (void)wait_on(event_handle(), at(OFF_TIMEOUT), &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    /* Already signalled: satisfied at once, no advance. */
    bool previous = false;
    CHECK_EQ_U32(kernel_object_event_set(event2_handle(), &previous), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_ok(event2_handle(), 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    teardown_waits();
}

/*
 * The Event-less read (`--async-file-io-file-object`): queued as pending, and the FILE OBJECT is what a
 * waiter sees. A wait on the file handle advances to the request's due time and succeeds. A file
 * object stays signalled afterwards (a notification object: a second wait returns at once with the
 * clock where it was), a new Event-less read CLEARS it at issue (the next wait advances again), and a
 * duplicate handle is the same file object.
 *
 * MUTATIONS: never set the file object, set it at issue, never clear it at issue, key it by the raw
 * handle instead of the file identity, consume it on a wait.
 */
static void test_an_eventless_read_signals_the_file_object(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    kernel_async_io_set_file_object_enabled(true);
    CHECK(kernel_async_io_file_object_enabled());
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    const uint32_t duplicate_args[3] = {handle, at(OFF_DUP), 2u};
    CHECK_EQ_U32(call_ordinal(ORD_NT_DUPLICATE_OBJECT, duplicate_args, 3u), STATUS_SUCCESS);
    const uint32_t duplicate = read32(at(OFF_DUP));
    CHECK(duplicate != 0u && duplicate != handle);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(read_file(handle, 0u, at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0x800u), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK(buffer_untouched(at(OFF_BUFFER), 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().file_object_reads, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 1u);
    const uint64_t issued = kernel_clock_peek();
    const uint64_t due = issued + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    CHECK_EQ_U32(wait_ok(duplicate, 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), due);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0x800u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 1u);
    /* Still signalled: another wait returns at once, on either handle. */
    CHECK_EQ_U32(wait_ok(handle, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_ok(duplicate, 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), due);
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 1u);
    /* A new Event-less read clears it at issue, so the next wait waits for the new request. It is issued
     * THROUGH THE DUPLICATE and waited on through the original: one file object. A third request queued
     * behind it signals the same object later, the wait ends at the FIRST one. */
    poison(OFF_BUFFER2, 0x2000u);
    CHECK_EQ_U32(read_file(duplicate, 0u, at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0x1800u), STATUS_PENDING);
    const uint64_t second_due = due + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    CHECK_EQ_U32(read_file(duplicate, 0u, at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0x2800u), STATUS_PENDING);
    const uint64_t third_due = second_due + kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_HDD, 0x1000u);
    CHECK(third_due > second_due);
    CHECK_EQ_U32(wait_ok(handle, 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), second_due);
    CHECK(buffer_matches(at(OFF_BUFFER2), 0x1800u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().waits, 2u);
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * The file object belongs to the Event-less requests only. A pending read that supplies an Event
 * never signals it, so a wait on that file handle is refused by name (it would block forever on a real
 * console too), and a wait on ANOTHER file's handle while this file has an Event-less request pending
 * is refused as well. A poll of an unsignalled file object is STATUS_TIMEOUT, never a refusal.
 *
 * MUTATIONS: signal the file object for an Event read, match any file, refuse a poll.
 */
static void test_the_file_object_is_only_signalled_by_eventless_reads_of_that_file(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    kernel_async_io_set_file_object_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t first = read32(at(OFF_HANDLE));
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t second = read32(at(OFF_HANDLE));
    CHECK(first != second);
    CHECK_EQ_U32(read_file(first, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 0x1000u, 0u), STATUS_PENDING);
    const uint64_t issued = kernel_clock_peek();
    bool refused = false;
    (void)wait_on(first, 0u, &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    CHECK_EQ_U64(kernel_clock_peek(), issued);
    CHECK_EQ_U32(wait_ok(first, relative_timeout(0)), STATUS_WAIT_TIMEOUT_VALUE);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read_file(first, 0u, at(OFF_IOSB2), at(OFF_BUFFER2), 0x1000u, 0u), STATUS_PENDING);
    (void)wait_on(second, 0u, &refused);
    CHECK(refused);
    CHECK_EQ_U32(refused_handle, second);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 2u);
    /* The other file's wait would see only its own object: the first file's wait ends. */
    CHECK_EQ_U32(wait_ok(first, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB2)), STATUS_SUCCESS);
    teardown_waits();
}

/*
 * The file object table holds 64 identities, as many as kernel_file has open slots. A slot is reused
 * only when its file has no live handle AND no request pending on it: with 63 files live and one
 * closed while its Event-less read is still pending, the next new file has no slot and its read is
 * refused by name and counted, then succeeds once the pending read completed. A closed file's entry
 * is then reused WITHOUT disturbing the signal state of the live ones (they are still signalled).
 *
 * MUTATIONS: shrink the table, never reuse a dead entry, reuse a live entry, reuse an entry with a
 * pending request.
 */
static void test_the_file_object_table_reuses_only_dead_idle_files(void)
{
    setup_waits();
    kernel_async_io_set_enabled(true);
    kernel_async_io_set_file_object_enabled(true);
    kernel_clock_set_frame_hook(kernel_async_io_service_hook);
    uint32_t handles[64];
    for (unsigned i = 0u; i < 64u; i++) {
        CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
        handles[i] = read32(at(OFF_HANDLE));
        CHECK_EQ_U32(read_file(handles[i], 0u, at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u), STATUS_PENDING);
        if (i != 63u) {
            CHECK(kernel_clock_frame(60u));
        }
    }
    CHECK_EQ_U32(kernel_async_io_get_stats().file_object_reads, 64u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    const uint32_t close_pending[1] = {handles[63]};
    CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_pending, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t fresh = read32(at(OFF_HANDLE));
    CHECK_EQ_U32(read_file(fresh, 0u, at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u),
                 STATUS_INSUFFICIENT_RESOURCES);
    CHECK_EQ_U32(kernel_async_io_get_stats().refused, 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 64u);
    CHECK(captured_contains("no file object slot"));
    CHECK(kernel_clock_frame(60u));
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    CHECK_EQ_U32(read_file(fresh, 0u, at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u), STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 65u);
    CHECK(kernel_clock_frame(60u));
    /* Another closed idle file frees a slot again, the live files keep their signal. */
    const uint32_t close_idle[1] = {handles[7]};
    CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_idle, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t reused = read32(at(OFF_HANDLE));
    CHECK_EQ_U32(read_file(reused, 0u, at(OFF_IOSB2), at(OFF_BUFFER), 16u, 0u), STATUS_PENDING);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 66u);
    const uint64_t before = kernel_clock_peek();
    CHECK_EQ_U32(wait_ok(handles[0], 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_ok(handles[8], 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_ok(handles[62], 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(wait_ok(fresh, 0u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_clock_peek(), before);
    CHECK_EQ_U32(refusals, 0u);
    teardown_waits();
}

/*
 * An ApcRoutine on an asynchronous handle is REFUSED BY NAME with the flag on: STATUS_NOT_IMPLEMENTED,
 * the IoStatusBlock says so, nothing is queued, the buffer is untouched, the refusal is counted and
 * logged. A synchronous handle with an ApcRoutine and the flag off keep the old behaviour (reported,
 * not called, the read completes synchronously).
 *
 * MUTATIONS: ignore the flag, ignore the handle kind, complete the read anyway, do not count.
 */
static void test_an_apc_routine_on_an_asynchronous_handle_is_refused_by_name(void)
{
    setup_waits();
    CHECK(!kernel_async_io_apc_refused(OPEN_ASYNC, 0x1234u));
    CHECK_EQ_U32(kernel_async_io_get_stats().apc_refused, 0u);
    kernel_async_io_set_enabled(true);
    CHECK(!kernel_async_io_apc_refused(OPEN_ASYNC, 0u));
    CHECK(!kernel_async_io_apc_refused(OPEN_SYNC, 0x1234u));
    CHECK_EQ_U32(kernel_async_io_get_stats().apc_refused, 0u);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    const uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 0x2000u);
    write32(at(OFF_IOSB), STATUS_PENDING);
    write32(at(OFF_BYTE_OFFSET), 0u);
    write32(at(OFF_BYTE_OFFSET) + 4u, 0u);
    const uint32_t args[8] = {handle, event_handle(), 0x00401234u, 0u, at(OFF_IOSB), at(OFF_BUFFER), 0x1000u,
                              at(OFF_BYTE_OFFSET)};
    CHECK_EQ_U32(call_ordinal(ORD_NT_READ_FILE, args, 8u), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_NOT_IMPLEMENTED);
    CHECK(buffer_untouched(at(OFF_BUFFER), 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    CHECK_EQ_U32(kernel_async_io_get_stats().apc_refused, 1u);
    CHECK(captured_contains("ApcRoutine 0x401234"));
    CHECK(captured_contains("REFUSED"));
    /* A synchronous handle keeps the reported-not-called completion. */
    CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
    const uint32_t sync_args[8] = {read32(at(OFF_HANDLE)), 0u, 0x00401234u, 0u, at(OFF_IOSB), at(OFF_BUFFER),
                                   0x1000u, at(OFF_BYTE_OFFSET)};
    CHECK_EQ_U32(call_ordinal(ORD_NT_READ_FILE, sync_args, 8u), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    CHECK_EQ_U32(kernel_async_io_get_stats().apc_refused, 1u);
    /* Flag off: the old completion, no refusal. */
    kernel_async_io_set_enabled(false);
    poison(OFF_BUFFER, 0x2000u);
    CHECK_EQ_U32(call_ordinal(ORD_NT_READ_FILE, args, 8u), STATUS_SUCCESS);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 0x1000u));
    teardown_waits();
}

/*
 * Eligibility with the file object model: an Event is still required unless it is on, the
 * synchronous options still refuse, and it needs the base flag.
 *
 * MUTATIONS: let the file object flag stand without the base flag, ignore it.
 */
static void test_eligibility_with_the_file_object_flag(void)
{
    setup();
    CHECK(!kernel_async_io_eligible(OPEN_ASYNC, 0u));
    CHECK(!kernel_async_io_eligible(OPEN_ASYNC, 5u));
    kernel_async_io_set_file_object_enabled(true);
    CHECK(!kernel_async_io_eligible(OPEN_ASYNC, 0u));
    CHECK(!kernel_async_io_eligible(OPEN_ASYNC, 5u));
    kernel_async_io_set_enabled(true);
    CHECK(kernel_async_io_eligible(OPEN_ASYNC, 0u));
    CHECK(kernel_async_io_eligible(OPEN_ASYNC, 5u));
    CHECK(!kernel_async_io_eligible(OPEN_SYNC, 0u));
    CHECK(!kernel_async_io_eligible(OPEN_SYNC, 5u));
    CHECK(!kernel_async_io_eligible(0x10u, 0u));
    CHECK(!kernel_async_io_eligible(0x20u, 0u));
    /* T785: a buffered asynchronous handle (no 0x8) completes inside the call, as on xemu (T763). */
    CHECK(!kernel_async_io_eligible(0x40u, 5u));
    CHECK(!kernel_async_io_eligible(0x40u, 0u));
    CHECK(kernel_async_io_eligible(0x08u, 5u));
    kernel_async_io_set_file_object_enabled(false);
    CHECK(!kernel_async_io_eligible(OPEN_ASYNC, 0u));
    CHECK(kernel_async_io_eligible(OPEN_ASYNC, 5u));
    teardown();
}

/* The clock primitive the waits use: advance_to is monotone, never creeps, and never goes backwards. */
static void test_the_clock_advances_to_a_time_and_never_back(void)
{
    kernel_clock_reset();
    CHECK_EQ_U64(kernel_clock_advance_to(5000u), 5000u);
    CHECK_EQ_U64(kernel_clock_peek(), 5000u);
    CHECK_EQ_U64(kernel_clock_advance_to(100u), 5000u);
    CHECK_EQ_U64(kernel_clock_peek(), 5000u);
    CHECK(kernel_clock_frame(60u));
    const uint64_t floor_now = kernel_clock_peek();
    CHECK(floor_now > 5000u);
    CHECK_EQ_U64(kernel_clock_advance_to(6000u), floor_now);
    CHECK_EQ_U64(kernel_clock_advance_to(floor_now + 7u), floor_now + 7u);
    CHECK_EQ_U64(kernel_clock_peek(), floor_now + 7u);
    kernel_clock_reset();
}

int main(void)
{
    test_flag_off_is_the_synchronous_read();
    test_buffered_async_read_signals_only_its_event_after_sync_success();
    test_eof_and_validation_preserve_original_event_iosb_contract();
    test_async_eof_is_not_reported_as_pending();
    test_closed_file_handle_rejected_while_duplicate_remains_live();
    test_an_overlapped_read_is_pending_then_completes_on_a_frame();
    test_completion_waits_for_access_plus_transfer_time();
    test_the_drive_is_serial_and_in_issue_order();
    test_each_volume_has_its_own_timing_and_paths_pick_the_volume();
    test_synchronous_handles_and_eventless_reads_stay_synchronous();
    test_a_short_read_reports_the_transferred_bytes();
    test_a_full_queue_is_refused_by_name();
    test_the_next_read_services_what_is_due();
    test_an_implicit_offset_read_advances_the_position_at_issue();
    test_oversize_and_unmapped_reads_do_not_queue();
    test_a_blocking_wait_advances_to_the_due_time_and_not_past_it();
    test_a_finite_timeout_ends_the_wait_at_the_deadline();
    test_a_timeout_completes_the_requests_due_before_the_deadline();
    test_a_poll_completes_only_what_is_already_due();
    test_a_wait_nothing_pending_would_end_is_refused_by_name();
    test_an_eventless_read_signals_the_file_object();
    test_the_file_object_is_only_signalled_by_eventless_reads_of_that_file();
    test_the_file_object_table_reuses_only_dead_idle_files();
    test_an_apc_routine_on_an_asynchronous_handle_is_refused_by_name();
    test_eligibility_with_the_file_object_flag();
    test_the_clock_advances_to_a_time_and_never_back();
    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
