/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T8e: ordinal 221 NtReleaseMutant and ordinal 197 NtDuplicateObject.
 *
 * Both have exactly ONE measured call site, so each suite pins what that site shows and
 * proves that everything it does not show is refused loudly with no state change. Every
 * test names the mutation it exists to kill.
 *
 *   221 NtReleaseMutant(HANDLE, PLONG PreviousCount OPTIONAL), stdcall, TWO arguments.
 *       Site 0x00380009 (the title's ReleaseMutex): push 0; push handle; call. The sign of
 *       the result is the only thing read.
 *   197 NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options), stdcall,
 *       THREE arguments. Site 0x0037CBEE (the title's DuplicateHandle, ret 0x1c); its one
 *       caller 0x0042CCE8 (XONLINE) duplicates the FILE handle CreateFileA returned, with
 *       Options 2, then runs GetFileSize/ReadFile/WriteFile on the duplicate and closes it.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "nt_status.h"

static int checks;
static int failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a, b) do { checks++; const uint32_t got_ = (uint32_t)(a); \
    const uint32_t want_ = (uint32_t)(b); if (got_ != want_) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__, #a, #b, \
            (unsigned)got_, (unsigned)want_); } } while (0)

static uint32_t scratch;
static char captured[8192];
static size_t captured_length;

static int capture(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (captured_length < sizeof(captured) - 1u) {
        const int written = vsnprintf(captured + captured_length,
                                      sizeof(captured) - captured_length, format, args);
        if (written > 0) {
            captured_length += (size_t)written;
            if (captured_length > sizeof(captured) - 1u) {
                captured_length = sizeof(captured) - 1u;
            }
        }
    }
    va_end(args);
    return 0;
}

static void clear_capture(void)
{
    captured[0] = '\0';
    captured_length = 0u;
}

static bool logged(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_object_reset();
    kernel_file_reset();
    kernel_file_unmount_all();
    kernel_io_reset();
    (void)kernel_object_register();
    (void)kernel_file_register();
    (void)kernel_io_register();
    kernel_hle_set_log(capture);
    clear_capture();
    const guest_region_request request = {.bytes = 0x4000u, .state = MEM_COMMIT,
                                          .protect = PAGE_READWRITE};
    nt_status status = 0u;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    kernel_file_unmount_all();
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    guest_mem_reset();
}

static uint32_t call(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, count));
    return kernel_hle_call(ordinal, &frame);
}

static uint32_t read32(uint32_t address)
{
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

/* The real NtCreateMutant (192) with the one measured shape (out, 0, 0). */
static uint32_t create_mutant(void)
{
    const uint32_t args[3] = {scratch + 0x800u, 0u, 0u};
    EQ(call(192u, args, 3u), STATUS_SUCCESS);
    return read32(scratch + 0x800u);
}

/* ---- 221 ------------------------------------------------------------------ */

static uint32_t release_mutant(uint32_t handle, uint32_t previous_count)
{
    const uint32_t args[2] = {handle, previous_count};
    return call(221u, args, 2u);
}

/* The ordinal is bound (it is the sixth object ordinal, 197 the seventh: NtClose, NtCreateMutant,
 * 246, 251, 250 were the five). MUTATION: drop the 221 or 197 binding and every call below
 * returns 0, which reads as STATUS_SUCCESS. */
static void test_221_and_197_are_registered(void)
{
    setup();
    kernel_hle_init();
    EQ(kernel_object_register(), 7u);
    const kernel_entry *release = kernel_hle_entry(221u);
    const kernel_entry *duplicate = kernel_hle_entry(197u);
    CHECK(release != NULL && duplicate != NULL);
    if (release != NULL && duplicate != NULL) {
        EQ(release->state, KERNEL_ENTRY_IMPLEMENTED);
        EQ(duplicate->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK(strcmp(release->name, "NtReleaseMutant") == 0);
        CHECK(strcmp(duplicate->name, "NtDuplicateObject") == 0);
    }
    teardown();
}

/* THE MEASURED MODE. Every mutant this host can hold is unowned: NtCreateMutant refuses an
 * initial owner and no wait can acquire one, so a release is a release by a non-owner and NT
 * answers STATUS_MUTANT_NOT_OWNED (0xC0000046), the value itself pinned. Nothing about the
 * mutant changes, and the report says what a successful release would need.
 * MUTATION: answer success; answer INVALID_HANDLE; skip the count; close the handle. */
static void test_221_releasing_an_unowned_mutant_is_not_owned(void)
{
    setup();
    const uint32_t mutant = create_mutant();
    CHECK(mutant != 0u);
    EQ(kernel_object_mutant_unowned_release_count(), 0u);
    EQ(release_mutant(mutant, 0u), STATUS_MUTANT_NOT_OWNED);
    EQ(STATUS_MUTANT_NOT_OWNED, 0xC0000046u);
    EQ(kernel_object_mutant_unowned_release_count(), 1u);
    CHECK(logged("NtReleaseMutant"));
    CHECK(logged("NOT OWNED"));
    CHECK(logged("wait"));
    EQ(kernel_object_live_count(), 1u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(mutant, &entry));
    EQ(entry.kind, KERNEL_OBJECT_MUTANT);
    EQ(entry.references, 1u);
    /* Repeatable, and still closable through the shared NtClose. */
    EQ(release_mutant(mutant, 0u), STATUS_MUTANT_NOT_OWNED);
    EQ(kernel_object_mutant_unowned_release_count(), 2u);
    const uint32_t close_args[1] = {mutant};
    EQ(call(187u, close_args, 1u), STATUS_SUCCESS);
    EQ(kernel_object_live_count(), 0u);
    teardown();
}

/* Two mutants stay independent: releasing one touches nothing of the other.
 * MUTATION: act on a fixed slot instead of the named handle. */
static void test_221_judges_the_named_handle(void)
{
    setup();
    const uint32_t first = create_mutant();
    const uint32_t second = create_mutant();
    CHECK(first != 0u && second != 0u && first != second);
    const uint32_t close_args[1] = {first};
    EQ(call(187u, close_args, 1u), STATUS_SUCCESS);
    EQ(release_mutant(first, 0u), STATUS_INVALID_HANDLE);
    EQ(kernel_object_mutant_unowned_release_count(), 0u);
    EQ(release_mutant(second, 0u), STATUS_MUTANT_NOT_OWNED);
    EQ(kernel_object_mutant_unowned_release_count(), 1u);
    teardown();
}

/* A handle that is not live is STATUS_INVALID_HANDLE (INFERRED NT contract), including the
 * pseudo handles, which never carry the table marker, and a stale generation. Nothing counts as
 * a non-owner release. MUTATION: fold the dead handle into NOT_OWNED or into the type mismatch. */
static void test_221_rejects_dead_handles(void)
{
    setup();
    EQ(release_mutant(0u, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(0x12345u, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(0xFFFFFFFFu, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(0xFFFFFFFEu, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(0xFFFFFFFCu, 0u), STATUS_INVALID_HANDLE);
    const uint32_t mutant = create_mutant();
    const uint32_t close_args[1] = {mutant};
    EQ(call(187u, close_args, 1u), STATUS_SUCCESS);
    EQ(release_mutant(mutant, 0u), STATUS_INVALID_HANDLE);
    const uint32_t live = create_mutant();
    EQ(release_mutant(live + 1u, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(live + 2u, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(live ^ 0x01000000u, 0u), STATUS_INVALID_HANDLE);
    EQ(release_mutant(live, 0u), STATUS_MUTANT_NOT_OWNED);
    const uint32_t close_live[1] = {live};
    EQ(call(187u, close_live, 1u), STATUS_SUCCESS);
    EQ(kernel_object_mutant_unowned_release_count(), 1u);
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        const uint32_t next = create_mutant();
        CHECK(next != mutant);
        const uint32_t close_next[1] = {next};
        EQ(call(187u, close_next, 1u), STATUS_SUCCESS);
    }
    EQ(release_mutant(mutant, 0u), STATUS_INVALID_HANDLE);
    EQ(kernel_object_mutant_unowned_release_count(), 1u);
    teardown();
}

/* A live handle of any other kind is STATUS_OBJECT_TYPE_MISMATCH (INFERRED:
 * ObReferenceObjectByHandle against ExMutantObjectType) and counts nothing.
 * MUTATION: treat any live handle as a mutant. */
static void test_221_rejects_other_kinds(void)
{
    setup();
    static const kernel_object_kind kinds[] = {
        KERNEL_OBJECT_THREAD, KERNEL_OBJECT_FILE,    KERNEL_OBJECT_EVENT,
        KERNEL_OBJECT_SEMAPHORE, KERNEL_OBJECT_SYMLINK, KERNEL_OBJECT_OTHER,
    };
    for (unsigned i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        const uint32_t handle = kernel_object_create(kinds[i], 7u);
        CHECK(handle != 0u);
        EQ(release_mutant(handle, 0u), STATUS_OBJECT_TYPE_MISMATCH);
    }
    EQ(kernel_object_mutant_unowned_release_count(), 0u);
    teardown();
}

/* A non-NULL PreviousCount is unmeasured (the only site passes the literal 0) and is refused
 * before anything is judged, whatever the handle is, with nothing written through the pointer.
 * MUTATION: write the count; drop the gate so a mutant answers NOT_OWNED; judge the handle
 * first so a dead handle answers INVALID_HANDLE. */
static void test_221_refuses_an_unmeasured_previous_count(void)
{
    setup();
    const uint32_t mutant = create_mutant();
    CHECK(kernel_guest_write_u32(scratch + 0x600u, 0xA5A5A5A5u));
    EQ(release_mutant(mutant, scratch + 0x600u), STATUS_NOT_IMPLEMENTED);
    EQ(read32(scratch + 0x600u), 0xA5A5A5A5u);
    CHECK(logged("PreviousCount"));
    CHECK(logged("REFUSED"));
    EQ(release_mutant(0x12345u, scratch + 0x600u), STATUS_NOT_IMPLEMENTED);
    EQ(release_mutant(mutant, 0x1u), STATUS_NOT_IMPLEMENTED);
    EQ(kernel_object_mutant_unowned_release_count(), 0u);
    EQ(release_mutant(mutant, 0u), STATUS_MUTANT_NOT_OWNED);
    teardown();
}

/* A missing frame, or a frame too short to hold both arguments, is a parameter error.
 * MUTATION: read one argument only, or dereference the NULL frame. */
static void test_221_rejects_a_bad_frame(void)
{
    setup();
    const uint32_t mutant = create_mutant();
    kernel_call_frame short_frame = {.stack_ptr = scratch + 0x100u,
                                     .stack_limit = scratch + 0x104u};
    EQ(kernel_hle_call(221u, &short_frame), STATUS_INVALID_PARAMETER);
    EQ(kernel_hle_call(221u, NULL), STATUS_INVALID_PARAMETER);
    /* One argument present: the handle is fine but PreviousCount cannot be read. */
    kernel_call_frame one_argument = {.stack_ptr = scratch + 0x100u,
                                      .stack_limit = scratch + 0x108u};
    CHECK(kernel_guest_write_u32(scratch + 0x104u, mutant));
    EQ(kernel_hle_call(221u, &one_argument), STATUS_INVALID_PARAMETER);
    EQ(kernel_object_mutant_unowned_release_count(), 0u);
    teardown();
}

/* ---- 197 ------------------------------------------------------------------ */

#define DUPLICATE_SAME_ACCESS 2u
#define GENERIC_READ_WRITE_ACCESS 0xC0000000u
#define OFF_IOSB 0x1000u
#define OFF_BUFFER 0x1100u
#define OFF_OPEN 0x1400u
#define OFF_DUP_OUT 0x700u

static uint32_t duplicate_object(uint32_t source, uint32_t target_out, uint32_t options)
{
    const uint32_t args[3] = {source, target_out, options};
    return call(197u, args, 3u);
}

/* The measured shape: DuplicateHandle(0, h, 0, &local, 0, 0, 2) reaches the kernel as
 * (h, &local, 2), and the result is read from the local. */
static uint32_t duplicate_same_access(uint32_t source)
{
    EQ(duplicate_object(source, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS), STATUS_SUCCESS);
    return read32(scratch + OFF_DUP_OUT);
}

static char host_dir[64];

static void build_host_dir(void)
{
    strcpy(host_dir, "/tmp/tsfp-t8e-dup-XXXXXX");
    CHECK(mkdtemp(host_dir) != NULL);
    char path[96];
    snprintf(path, sizeof(path), "%s/save.bin", host_dir);
    const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "savedata", 8u) == 8);
    (void)close(fd);
}

static void remove_host_dir(void)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/save.bin", host_dir);
    (void)unlink(path);
    (void)rmdir(host_dir);
}

static void read_host_file(char out[9])
{
    char path[96];
    snprintf(path, sizeof(path), "%s/save.bin", host_dir);
    memset(out, 0, 9u);
    const int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(read(fd, out, 8u) == 8);
        (void)close(fd);
    }
}

/* Drive the real NtOpenFile (202) on the mounted host directory. */
static uint32_t open_save(uint32_t access)
{
    const uint32_t oa = scratch + OFF_OPEN;
    const uint32_t name = oa + 0x20u;
    const uint32_t text = oa + 0x40u;
    const char *path = "\\Device\\Harddisk0\\partition1\\save.bin";
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        CHECK(kernel_guest_write_u8(text + (uint32_t)i, (uint8_t)path[i]));
    }
    CHECK(kernel_guest_write_u8(name, (uint8_t)length));
    CHECK(kernel_guest_write_u8(name + 1u, 0u));
    CHECK(kernel_guest_write_u8(name + 2u, (uint8_t)(length + 1u)));
    CHECK(kernel_guest_write_u8(name + 3u, 0u));
    CHECK(kernel_guest_write_u32(name + 4u, text));
    CHECK(kernel_guest_write_u32(oa, 0u));
    CHECK(kernel_guest_write_u32(oa + 4u, name));
    CHECK(kernel_guest_write_u32(oa + 8u, 0x40u));
    const uint32_t handle_out = oa + 0x200u;
    const uint32_t args[6] = {handle_out, access, oa, scratch + OFF_IOSB, 3u, 0x800021u};
    EQ(call(202u, args, 6u), STATUS_SUCCESS);
    return read32(handle_out);
}

static void mount_host(void)
{
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1", host_dir));
}

/* NtReadFile (219) / NtWriteFile (236), eight arguments, ByteOffset NULL: the implicit position. */
static uint32_t transfer(unsigned ordinal, uint32_t handle, uint32_t length)
{
    const uint32_t args[8] = {handle, 0u, 0u, 0u, scratch + OFF_IOSB, scratch + OFF_BUFFER,
                              length, 0u};
    return call(ordinal, args, 8u);
}

static void put_text(const char *text)
{
    for (size_t i = 0u; text[i] != '\0'; i++) {
        CHECK(kernel_guest_write_u8(scratch + OFF_BUFFER + (uint32_t)i, (uint8_t)text[i]));
    }
}

static void get_text(char *out, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        uint8_t byte = 0u;
        CHECK(kernel_guest_read_u8(scratch + OFF_BUFFER + i, &byte));
        out[i] = (char)byte;
    }
    out[length] = '\0';
}

static uint32_t references_of(uint32_t handle)
{
    kernel_object_entry entry;
    return kernel_object_get_copy(handle, &entry) ? entry.references : 0xFFFFFFFFu;
}

/* THE MEASURED MODE. A FILE handle duplicated with Options 2 yields a NEW handle value, also a
 * FILE, that names the same open file: the open-file state (path, size, backing) is the
 * original's, and reads through the duplicate reach the real bytes. The original is untouched
 * (still one reference). MUTATION: return the source handle itself; create the entry as another
 * kind; do not alias the open-file state; bump or drop the source's references. */
static void test_197_duplicates_a_file_handle_onto_the_same_open_file(void)
{
    setup();
    build_host_dir();
    mount_host();
    const uint32_t original = open_save(GENERIC_READ_WRITE_ACCESS);
    CHECK(original != 0u);
    const uint32_t live_before = kernel_object_live_count();
    const uint32_t duplicate = duplicate_same_access(original);
    CHECK(duplicate != 0u);
    CHECK(duplicate != original);
    EQ(kernel_object_live_count(), live_before + 1u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(duplicate, &entry));
    EQ(entry.kind, KERNEL_OBJECT_FILE);
    EQ(entry.owner_tag, 197u); /* minted by ordinal 197, which is what a live-handle dump shows */
    EQ(entry.file_dup_of, original);
    EQ(entry.references, 1u);
    EQ(entry.file_body, 0u);
    CHECK(kernel_object_get_copy(original, &entry));
    EQ(entry.file_dup_of, 0u);
    EQ(references_of(original), 1u);
    CHECK((duplicate & 0x00FF0000u) == 0x00E10000u);

    kernel_file_open via_original;
    kernel_file_open via_duplicate;
    CHECK(kernel_file_open_info(original, &via_original));
    CHECK(kernel_file_open_info(duplicate, &via_duplicate));
    EQ(via_duplicate.handle, original);
    CHECK(strcmp(via_duplicate.path, via_original.path) == 0);
    EQ(via_duplicate.backing, via_original.backing);
    CHECK(via_duplicate.size == 8u && via_duplicate.writable);
    uint8_t bytes[8] = {0};
    uint32_t got = 0u;
    CHECK(kernel_file_read_backing(duplicate, 0u, bytes, sizeof(bytes), &got));
    EQ(got, 8u);
    CHECK(memcmp(bytes, "savedata", 8u) == 0);
    /* The directory query resolves the duplicate too: a regular file is "not a directory"
     * (INVALID_PARAMETER) for both handles, and only an unresolved handle is INVALID_HANDLE. */
    kernel_file_dir_entry listing;
    uint32_t listing_status = 0u;
    CHECK(!kernel_file_dir_next(original, true, NULL, 255u, &listing, &listing_status));
    EQ(listing_status, STATUS_INVALID_PARAMETER);
    CHECK(!kernel_file_dir_next(duplicate, true, NULL, 255u, &listing, &listing_status));
    EQ(listing_status, STATUS_INVALID_PARAMETER);
    CHECK(logged("NtDuplicateObject"));
    teardown();
    remove_host_dir();
}

/* The file POSITION is shared, as it is for every handle of one NT file object: a read through
 * the duplicate advances the cursor the original reads from, and the reverse. This is the part
 * of "handle semantics" the XONLINE consumer depends on (ReadFile with no explicit offset).
 * MUTATION: give the duplicate its own cursor (key the open state by the duplicate). */
static void test_197_shares_the_file_position(void)
{
    setup();
    build_host_dir();
    mount_host();
    const uint32_t original = open_save(GENERIC_READ_WRITE_ACCESS);
    const uint32_t duplicate = duplicate_same_access(original);
    char text[9];
    EQ(transfer(219u, duplicate, 4u), STATUS_SUCCESS);
    get_text(text, 4u);
    CHECK(strcmp(text, "save") == 0);
    kernel_file_open info;
    CHECK(kernel_file_open_info(original, &info));
    CHECK(info.offset == 4u);
    EQ(transfer(219u, original, 4u), STATUS_SUCCESS);
    get_text(text, 4u);
    CHECK(strcmp(text, "data") == 0);
    CHECK(kernel_file_open_info(duplicate, &info));
    CHECK(info.offset == 8u);
    CHECK(kernel_file_open_set_offset(duplicate, 2u));
    CHECK(kernel_file_open_info(original, &info));
    CHECK(info.offset == 2u);
    teardown();
    remove_host_dir();
}

/* Writes through the duplicate land in the real file (the open asked for write access, the
 * duplicate shares it, "same access"), and the title's own ordering holds: after the original
 * closes, the duplicate still works, and only when the LAST handle closes is the open-file
 * slot reclaimable. MUTATION: reclaim the slot while a duplicate lives; let the duplicate
 * outlive nothing. */
static void test_197_writes_through_the_duplicate_and_outlives_the_original(void)
{
    setup();
    build_host_dir();
    mount_host();
    const uint32_t original = open_save(GENERIC_READ_WRITE_ACCESS);
    const uint32_t duplicate = duplicate_same_access(original);
    put_text("SAVE");
    EQ(transfer(236u, duplicate, 4u), STATUS_SUCCESS);
    char contents[9];
    read_host_file(contents);
    CHECK(memcmp(contents, "SAVEdata", 8u) == 0);

    const uint32_t close_original[1] = {original};
    EQ(call(187u, close_original, 1u), STATUS_SUCCESS);
    CHECK(kernel_object_find(original) == NULL);
    CHECK(kernel_object_find(duplicate) != NULL);
    /* Opening the same file again claims a slot, which sweeps slots whose handle died. The
     * duplicate must keep the shared one alive through that sweep. */
    const uint32_t reopened = open_save(GENERIC_READ_WRITE_ACCESS);
    CHECK(reopened != 0u);
    EQ(kernel_file_open_count(), 2u);
    kernel_file_open info;
    CHECK(kernel_file_open_info(duplicate, &info));
    CHECK(info.offset == 4u);
    put_text("DATA");
    EQ(transfer(236u, duplicate, 4u), STATUS_SUCCESS);
    read_host_file(contents);
    CHECK(memcmp(contents, "SAVEDATA", 8u) == 0);

    /* Last handle gone: the next claim reclaims the shared slot. */
    const uint32_t close_duplicate[1] = {duplicate};
    EQ(call(187u, close_duplicate, 1u), STATUS_SUCCESS);
    CHECK(kernel_object_find(duplicate) == NULL);
    const uint32_t third = open_save(GENERIC_READ_WRITE_ACCESS);
    CHECK(third != 0u);
    EQ(kernel_file_open_count(), 2u);
    teardown();
    remove_host_dir();
}

/* A duplicate of a duplicate names the ROOT open file, so closing the first duplicate and the
 * original in either order leaves the third working. MUTATION: record the immediate source
 * instead of the root. */
static void test_197_duplicating_a_duplicate_names_the_root(void)
{
    setup();
    build_host_dir();
    mount_host();
    const uint32_t original = open_save(GENERIC_READ_WRITE_ACCESS);
    const uint32_t first = duplicate_same_access(original);
    const uint32_t second = duplicate_same_access(first);
    CHECK(second != first && second != original);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(second, &entry));
    EQ(entry.file_dup_of, original);
    const uint32_t close_original[1] = {original};
    const uint32_t close_first[1] = {first};
    EQ(call(187u, close_original, 1u), STATUS_SUCCESS);
    EQ(call(187u, close_first, 1u), STATUS_SUCCESS);
    const uint32_t reopened = open_save(GENERIC_READ_WRITE_ACCESS);
    CHECK(reopened != 0u);
    kernel_file_open info;
    CHECK(kernel_file_open_info(second, &info));
    uint8_t bytes[8] = {0};
    uint32_t got = 0u;
    CHECK(kernel_file_read_backing(second, 0u, bytes, sizeof(bytes), &got));
    EQ(got, 8u);
    CHECK(memcmp(bytes, "savedata", 8u) == 0);
    teardown();
    remove_host_dir();
}

/* The identity accessors kernel_file keys on. A plain handle is its own identity, a duplicate
 * answers its root, an unknown value is returned unchanged (so a lookup fails exactly as before),
 * and an identity is live while the original OR any duplicate is.
 * MUTATION: answer 0 for a dead handle; compare identity liveness against the original only. */
static void test_197_identity_accessors(void)
{
    setup();
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    CHECK(original != 0u);
    EQ(kernel_object_file_identity(original), original);
    EQ(kernel_object_file_identity(0x12345u), 0x12345u);
    EQ(kernel_object_file_identity(0u), 0u);
    CHECK(kernel_object_file_identity_live(original));
    CHECK(!kernel_object_file_identity_live(0x12345u));
    CHECK(!kernel_object_file_identity_live(0u));
    const uint32_t duplicate = duplicate_same_access(original);
    EQ(kernel_object_file_identity(duplicate), original);
    const uint32_t close_original[1] = {original};
    EQ(call(187u, close_original, 1u), STATUS_SUCCESS);
    EQ(kernel_object_file_identity(duplicate), original);
    CHECK(kernel_object_find(original) == NULL);
    CHECK(kernel_object_file_identity_live(original));
    const uint32_t close_duplicate[1] = {duplicate};
    EQ(call(187u, close_duplicate, 1u), STATUS_SUCCESS);
    CHECK(!kernel_object_file_identity_live(original));
    /* A duplicate of one live handle does not keep ANOTHER identity alive. */
    const uint32_t other = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    const uint32_t other_duplicate = duplicate_same_access(other);
    CHECK(other_duplicate != 0u);
    CHECK(!kernel_object_file_identity_live(original));
    CHECK(kernel_object_file_identity_live(other));
    teardown();
}

/* A FILE handle another subsystem issued with no open-file slot still duplicates at the object
 * level (the open-file lookup then fails for both exactly as for the original).
 * MUTATION: require an open-file slot. */
static void test_197_duplicates_a_file_handle_without_an_open_slot(void)
{
    setup();
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    const uint32_t duplicate = duplicate_same_access(original);
    CHECK(duplicate != 0u);
    kernel_file_open info;
    CHECK(!kernel_file_open_info(original, &info));
    CHECK(!kernel_file_open_info(duplicate, &info));
    teardown();
}

/* Only Options == 2 is measured, and it is judged as the whole ULONG (no byte masking), so 0
 * (a plain copy), 1 (close the source), 3, 4 and a value that merely CONTAINS bit 1 are all
 * refused with nothing created and the out-parameter untouched.
 * MUTATION: accept any Options; test only bit 1; test the low byte; drop the report. */
static void test_197_refuses_unmeasured_options(void)
{
    setup();
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    static const uint32_t bad_options[] = {0u, 1u, 3u, 4u, 6u, 0x102u, 0x80000002u};
    for (unsigned i = 0u; i < sizeof(bad_options) / sizeof(bad_options[0]); i++) {
        clear_capture();
        CHECK(kernel_guest_write_u32(scratch + OFF_DUP_OUT, 0xA5A5A5A5u));
        EQ(duplicate_object(original, scratch + OFF_DUP_OUT, bad_options[i]),
           STATUS_NOT_IMPLEMENTED);
        EQ(read32(scratch + OFF_DUP_OUT), 0xA5A5A5A5u);
        EQ(kernel_object_live_count(), 1u);
        CHECK(logged("Options"));
        CHECK(logged("REFUSED"));
    }
    /* Options are judged before the handle, as in 221's PreviousCount. */
    EQ(duplicate_object(0x12345u, scratch + OFF_DUP_OUT, 1u), STATUS_NOT_IMPLEMENTED);
    teardown();
}

/* A NULL TargetHandle is a different NT mode (it is how NT closes a source with no duplicate),
 * unmeasured here, so it is refused and nothing is created. An unwritable target is a parameter
 * error and the duplicate is released again, so nothing leaks.
 * MUTATION: create anyway on NULL; leave the duplicate alive after the failed write. */
static void test_197_target_handle_cases(void)
{
    setup();
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    EQ(duplicate_object(original, 0u, DUPLICATE_SAME_ACCESS), STATUS_NOT_IMPLEMENTED);
    EQ(kernel_object_live_count(), 1u);
    CHECK(logged("TargetHandle"));
    static const uint32_t bad_targets[] = {0x00001000u, 0xFFFFFFFCu, 0xFFFFFFFEu};
    for (unsigned i = 0u; i < sizeof(bad_targets) / sizeof(bad_targets[0]); i++) {
        EQ(duplicate_object(original, bad_targets[i], DUPLICATE_SAME_ACCESS),
           STATUS_INVALID_PARAMETER);
        EQ(kernel_object_live_count(), 1u);
    }
    /* The slot freed by the failed attempt is reused, not leaked: a good call still works and
     * is the second live handle. */
    const uint32_t duplicate = duplicate_same_access(original);
    CHECK(duplicate != 0u);
    EQ(kernel_object_live_count(), 2u);
    teardown();
}

/* Only a FILE handle is measured. Every other live kind would need STATE shared between the
 * handles (an event's signal, a mutant's owner, a thread's body references) that the table
 * keeps per handle, so each is refused by name with no change.
 * MUTATION: allow EVENT or MUTANT or THREAD, or fold the kinds into one status. */
static void test_197_refuses_other_kinds(void)
{
    setup();
    static const struct {
        kernel_object_kind kind;
        const char *name;
    } kinds[] = {
        {KERNEL_OBJECT_THREAD, "THREAD"},
        {KERNEL_OBJECT_EVENT, "EVENT"},
        {KERNEL_OBJECT_SEMAPHORE, "SEMAPHORE"},
        {KERNEL_OBJECT_MUTANT, "MUTANT"},
        {KERNEL_OBJECT_SYMLINK, "SYMLINK"},
        {KERNEL_OBJECT_OTHER, "OTHER"},
    };
    for (unsigned i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        const uint32_t handle = kernel_object_create(kinds[i].kind, 7u);
        CHECK(handle != 0u);
        const unsigned live = kernel_object_live_count();
        clear_capture();
        CHECK(kernel_guest_write_u32(scratch + OFF_DUP_OUT, 0xA5A5A5A5u));
        EQ(duplicate_object(handle, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
           STATUS_NOT_IMPLEMENTED);
        EQ(read32(scratch + OFF_DUP_OUT), 0xA5A5A5A5u);
        EQ(kernel_object_live_count(), live);
        CHECK(logged(kinds[i].name));
        CHECK(logged("REFUSED"));
    }
    teardown();
}

/* A source that is not a live handle is STATUS_INVALID_HANDLE (INFERRED NT contract), the two
 * pseudo handles are a REAL NT mode this host has not measured so they are refused as such, and
 * a closed, misaligned or stale-generation value is INVALID_HANDLE. A bad frame is a parameter
 * error. MUTATION: fold pseudo handles into INVALID_HANDLE; match handle low bits. */
static void test_197_rejects_bad_sources_and_frames(void)
{
    setup();
    EQ(duplicate_object(0u, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS), STATUS_INVALID_HANDLE);
    EQ(duplicate_object(0x12345u, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_INVALID_HANDLE);
    EQ(duplicate_object(0xFFFFFFFCu, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_INVALID_HANDLE);
    EQ(duplicate_object(0xFFFFFFFFu, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_NOT_IMPLEMENTED);
    CHECK(logged("pseudo"));
    CHECK(logged("current process"));
    clear_capture();
    EQ(duplicate_object(0xFFFFFFFEu, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_NOT_IMPLEMENTED);
    CHECK(logged("pseudo"));
    CHECK(logged("current thread"));
    const uint32_t file = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    EQ(duplicate_object(file + 1u, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_INVALID_HANDLE);
    EQ(duplicate_object(file ^ 0x01000000u, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_INVALID_HANDLE);
    const uint32_t close_file[1] = {file};
    EQ(call(187u, close_file, 1u), STATUS_SUCCESS);
    EQ(duplicate_object(file, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS), STATUS_INVALID_HANDLE);
    EQ(kernel_object_live_count(), 0u);

    kernel_call_frame short_frame = {.stack_ptr = scratch + 0x100u,
                                     .stack_limit = scratch + 0x108u};
    EQ(kernel_hle_call(197u, &short_frame), STATUS_INVALID_PARAMETER);
    kernel_call_frame two_arguments = {.stack_ptr = scratch + 0x100u,
                                       .stack_limit = scratch + 0x10Cu};
    EQ(kernel_hle_call(197u, &two_arguments), STATUS_INVALID_PARAMETER);
    EQ(kernel_hle_call(197u, NULL), STATUS_INVALID_PARAMETER);
    teardown();
}

/* A full table is STATUS_INSUFFICIENT_RESOURCES with the target untouched and nothing created.
 * MUTATION: write a zero handle and report success. */
static void test_197_reports_a_full_table(void)
{
    setup();
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    CHECK(original != 0u);
    while (kernel_object_create(KERNEL_OBJECT_OTHER, 7u) != 0u) {
    }
    EQ(kernel_object_live_count(), KERNEL_OBJECT_MAX);
    CHECK(kernel_guest_write_u32(scratch + OFF_DUP_OUT, 0xA5A5A5A5u));
    EQ(duplicate_object(original, scratch + OFF_DUP_OUT, DUPLICATE_SAME_ACCESS),
       STATUS_INSUFFICIENT_RESOURCES);
    EQ(read32(scratch + OFF_DUP_OUT), 0xA5A5A5A5u);
    EQ(kernel_object_live_count(), KERNEL_OBJECT_MAX);
    teardown();
}

/* Closing the duplicate leaves the original usable, and closing it twice is the usual bad close.
 * MUTATION: close the root when a duplicate closes. */
static void test_197_closing_the_duplicate_leaves_the_original(void)
{
    setup();
    build_host_dir();
    mount_host();
    const uint32_t original = open_save(GENERIC_READ_WRITE_ACCESS);
    const uint32_t duplicate = duplicate_same_access(original);
    const uint32_t close_duplicate[1] = {duplicate};
    EQ(call(187u, close_duplicate, 1u), STATUS_SUCCESS);
    EQ(call(187u, close_duplicate, 1u), STATUS_INVALID_HANDLE);
    CHECK(kernel_object_find(original) != NULL);
    uint8_t bytes[8] = {0};
    uint32_t got = 0u;
    CHECK(kernel_file_read_backing(original, 0u, bytes, sizeof(bytes), &got));
    EQ(got, 8u);
    EQ(kernel_object_live_count(), 1u);
    teardown();
    remove_host_dir();
}

int main(void)
{
    test_221_and_197_are_registered();
    test_221_releasing_an_unowned_mutant_is_not_owned();
    test_221_judges_the_named_handle();
    test_221_rejects_dead_handles();
    test_221_rejects_other_kinds();
    test_221_refuses_an_unmeasured_previous_count();
    test_221_rejects_a_bad_frame();
    test_197_duplicates_a_file_handle_onto_the_same_open_file();
    test_197_shares_the_file_position();
    test_197_writes_through_the_duplicate_and_outlives_the_original();
    test_197_duplicating_a_duplicate_names_the_root();
    test_197_identity_accessors();
    test_197_duplicates_a_file_handle_without_an_open_slot();
    test_197_refuses_unmeasured_options();
    test_197_target_handle_cases();
    test_197_refuses_other_kinds();
    test_197_rejects_bad_sources_and_frames();
    test_197_reports_a_full_table();
    test_197_closing_the_duplicate_leaves_the_original();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
