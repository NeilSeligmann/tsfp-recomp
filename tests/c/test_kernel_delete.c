/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ordinal 195, NtDeleteFile(POBJECT_ATTRIBUTES), stdcall, ONE argument.
 *
 * THE ONE MEASURED SITE is 0x004228EC, inside the XONLINE local-cache closer 0x0042287C
 * (`this` in ecx, one stack argument: the slot index 2, 3 or 4, from the three callers
 * 0x0041778E, 0x0041A932 and 0x0041AA2E). It first closes the handle it keeps for that slot,
 * then formats `\Device\Harddisk0\partition1\CACHE%.4s\LocalCache%02d.bin` into a stack
 * buffer, builds an ANSI string over it (RtlInitAnsiString), an OBJECT_ATTRIBUTES
 * {root 0, name, attributes 0x40} on its stack and calls NtDeleteFile with that one pointer.
 * Only the SIGN of the result is read: a failure goes through RtlNtStatusToDosError, a success
 * returns the caller's zero. So THE ONE MEASURED MODE is a FILE named from root 0 inside a
 * CACHE.... directory of partition1 (the title's data volume, a host-directory volume here),
 * deleted after the title has closed its own handle. A cache PARTITION's directory view
 * (partition 3 to 10) goes through the same code and its FATX gate, which is INFERRED.
 *
 * Every test names the mutation it exists to kill. DELIBERATELY FREE OF LIFTED CODE AND OF ANY
 * REAL DISC: guest structures are built in scratch guest memory, the host side is a temporary
 * directory beside this binary (removed by teardown and by an atexit handler).
 */

#include "kernel_file.h"

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "nt_status.h"
#include "xdvdfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)a_, (unsigned)e_);                                          \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        uint64_t a_ = (uint64_t)(actual);                                                \
        uint64_t e_ = (uint64_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,        \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);             \
            failures++;                                                                  \
        }                                                                                \
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

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}


#define ORD_NT_DELETE_FILE 195u
#define ORD_NT_OPEN_FILE 202u
#define ORD_NT_CLOSE 187u

#define STATUS_SHARING_VIOLATION_CODE 0xC0000043u

/* The OBJECT_ATTRIBUTES.attributes the measured site stores. */
#define MEASURED_OA_ATTRIBUTES 0x40u
#define MEASURED_ROOT 0xFFFFFFFDu

#define SCRATCH_BYTES 0x4000u
#define FRAME_OFFSET 0x000u
#define FRAME_BYTES 0x100u
#define OA_OFFSET 0x200u
#define STRING_OFFSET 0x220u
#define CHARS_OFFSET 0x280u
#define HANDLE_OFFSET 0x400u
#define IOSB_OFFSET 0x410u

#define HDD_DEVICE "\\Device\\Harddisk0\\partition1"
#define DISC_DEVICE "\\Device\\CdRom0"
#define DEVICE_PREFIX "\\Device\\Harddisk0\\partition0"
#define DEVICE_FILE "partition0.bin"
#define DEVICE_CAPACITY 0x80000ull
#define CACHE_PREFIX "\\Device\\Harddisk0\\Partition5"
#define CACHE_IMAGE ".tsfp-cache5.bin"
#define CACHE_DIR ".tsfp-cache5"
#define CACHE_CAPACITY 0x1000000ull

static kernel_guest_ptr scratch;
static char host_root[256];
static char binary_directory[200];
static bool host_root_ready;

static void drop_host_root(void);

static bool find_binary_directory(void)
{
    char exe[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
    if (length <= 0) {
        return false;
    }
    exe[length] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash == NULL || (size_t)(slash - exe) >= sizeof(binary_directory)) {
        return false;
    }
    *slash = '\0';
    memcpy(binary_directory, exe, strlen(exe) + 1u);
    return true;
}

static void make_host_root(void)
{
    static bool exit_handler_registered;
    if (binary_directory[0] == '\0' && !find_binary_directory()) {
        printf("FATAL could not locate the test binary's directory\n");
        exit(EXIT_FAILURE);
    }
    const int written = snprintf(host_root, sizeof(host_root), "%s/tsfp-attr-test-XXXXXX",
                                 binary_directory);
    if (written <= 0 || (size_t)written >= sizeof(host_root) || mkdtemp(host_root) == NULL) {
        printf("FATAL could not create a scratch directory: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    host_root_ready = true;
    if (!exit_handler_registered) {
        exit_handler_registered = true;
        (void)atexit(drop_host_root);
    }
}

static void remove_tree(int parent_fd, const char *name, unsigned depth)
{
    if (depth == 0u) {
        printf("FATAL scratch tree is deeper than this remover will go\n");
        exit(EXIT_FAILURE);
    }
    const int dir_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) {
        return;
    }
    const int scan_fd = dup(dir_fd);
    DIR *dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!dir) {
        if (scan_fd >= 0) {
            (void)close(scan_fd);
        }
        (void)close(dir_fd);
        return;
    }
    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        struct stat info;
        if (fstatat(dir_fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(info.st_mode)) {
            remove_tree(dir_fd, entry->d_name, depth - 1u);
        } else {
            (void)unlinkat(dir_fd, entry->d_name, 0);
        }
    }
    (void)closedir(dir);
    (void)close(dir_fd);
    (void)unlinkat(parent_fd, name, AT_REMOVEDIR);
}

static void drop_host_root(void)
{
    if (!host_root_ready) {
        return;
    }
    remove_tree(AT_FDCWD, host_root, 16u);
    host_root_ready = false;
}

static void host_make_dir(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    if (mkdir(path, 0777) != 0) {
        printf("FATAL could not create \"%s\": %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void host_write_file(const char *relative, const void *bytes, size_t length)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    FILE *file = fopen(path, "wb");
    if (!file || (length > 0u && fwrite(bytes, 1u, length, file) != length)) {
        printf("FATAL could not write \"%s\"\n", path);
        exit(EXIT_FAILURE);
    }
    (void)fclose(file);
}

static void host_make_symlink(const char *target, const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    if (symlink(target, path) != 0) {
        printf("FATAL could not link \"%s\": %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* How many descriptors this process holds, from /proc, so a leaked one is a failed check. */
static unsigned open_descriptor_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    unsigned count = 0u;
    if (!dir) {
        return 0u;
    }
    while (readdir(dir) != NULL) {
        count++;
    }
    (void)closedir(dir);
    return count;
}
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

static void setup(void)
{
    kernel_hle_init();
    kernel_file_reset();
    kernel_file_unmount_all();
    kernel_io_reset();
    kernel_object_reset();
    guest_mem_reset();
    (void)kernel_object_register();
    (void)kernel_io_register();
    /* SEVEN: 195 is the seventh. A dropped binding leaves a stub returning 0, which reads as
     * STATUS_SUCCESS with nothing deleted, so the exact count is what stops this passing
     * against nothing. */
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
    make_host_root();
}

static void teardown(void)
{
    kernel_file_unmount_all();
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
    drop_host_root();
    CHECK(access(host_root, F_OK) != 0);
}


/* OBJECT_STRING: length at +0, maximum_length at +2, buffer at +4. No NUL is written, so a
 * handler that stopped at one would read the sentinel. */
static kernel_guest_ptr build_name(const char *path)
{
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        if (!kernel_guest_write_u8(at(CHARS_OFFSET) + (uint32_t)i, (uint8_t)path[i])) {
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

static kernel_guest_ptr build_attributes(uint32_t root, kernel_guest_ptr name,
                                         uint32_t attributes)
{
    const kernel_guest_ptr oa = at(OA_OFFSET);
    write_u32_at(oa + 0u, root);
    write_u32_at(oa + 4u, name);
    write_u32_at(oa + 8u, attributes);
    return oa;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamped to exactly the arguments built, so a read of one more FAILS. */
    frame.stack_limit = at(FRAME_OFFSET) + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

/* NtDeleteFile with the measured shape: root 0, attributes 0x40, ONE argument. */
static uint32_t delete_rooted(uint32_t root, const char *path)
{
    const uint32_t args[1] = {build_attributes(root, build_name(path), MEASURED_OA_ATTRIBUTES)};
    return call_ordinal(ORD_NT_DELETE_FILE, args, 1u);
}

static uint32_t delete_name(const char *path)
{
    return delete_rooted(0u, path);
}

static bool host_exists(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    struct stat info;
    return lstat(path, &info) == 0;
}

static void mount_the_hdd(void)
{
    CHECK(kernel_file_mount_host_dir(HDD_DEVICE, host_root));
}

/* A cache partition whose raw image already starts with the title's FATX magic, so its
 * directory view is served. */
static void mount_a_formatted_cache_partition(void)
{
    host_write_file(CACHE_IMAGE, "FATX", 4u);
    CHECK(kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, 5u, CACHE_IMAGE,
                                            CACHE_DIR, CACHE_CAPACITY));
}

/* NtOpenFile with read access of a name, returning the handle (0 on failure). */
static uint32_t open_name(const char *path)
{
    const uint32_t args[6] = {at(HANDLE_OFFSET), 0x80100000u,
                              build_attributes(0u, build_name(path), MEASURED_OA_ATTRIBUTES),
                              at(IOSB_OFFSET), 3u, 0x60u};
    if (call_ordinal(ORD_NT_OPEN_FILE, args, 6u) != STATUS_SUCCESS) {
        return 0u;
    }
    return read_u32_at(at(HANDLE_OFFSET));
}

static uint32_t close_handle(uint32_t handle)
{
    const uint32_t args[1] = {handle};
    return call_ordinal(ORD_NT_CLOSE, args, 1u);
}

static void store_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

#define SYNTH_IMAGE_SECTORS 36u
#define SYNTH_DESCRIPTOR_SECTOR 32u
#define SYNTH_ROOT_SECTOR 33u
#define SYNTH_FILE_SECTOR 34u

/* A minimal valid XDVDFS image with one 5-byte file, HELLO, in its root. */
static void write_synthetic_disc(const char *relative)
{
    static uint8_t image[SYNTH_IMAGE_SECTORS * XDVDFS_SECTOR_SIZE];
    memset(image, 0, sizeof(image));
    uint8_t *descriptor = &image[SYNTH_DESCRIPTOR_SECTOR * XDVDFS_SECTOR_SIZE];
    memcpy(&descriptor[0], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    memcpy(&descriptor[0x7EC], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    store_u32(&descriptor[0x14], SYNTH_ROOT_SECTOR);
    store_u32(&descriptor[0x18], 20u);
    uint8_t *root = &image[SYNTH_ROOT_SECTOR * XDVDFS_SECTOR_SIZE];
    memset(root, 0xFF, 20u);
    root[0] = 0u;
    root[1] = 0u;
    root[2] = 0u;
    root[3] = 0u;
    store_u32(&root[4], SYNTH_FILE_SECTOR);
    store_u32(&root[8], 5u);
    root[0x0C] = 0x20u;
    root[0x0D] = 5u;
    memcpy(&root[0x0E], "HELLO", 5u);
    memcpy(&image[SYNTH_FILE_SECTOR * XDVDFS_SECTOR_SIZE], "WORLD", 5u);
    host_write_file(relative, image, sizeof(image));
}

/* ---------------------------------------------------------------------------------- */

/* 210 is registered, by name, and 211 (the class-0x22 twin) is still there beside it. */

/* ---------------------------------------------------------------------------------- */

/* 195 is registered, by name, as implemented. MUTATION: drop the binding. */
static void test_registered_by_name(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_NT_DELETE_FILE);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->name != NULL && strcmp(entry->name, "NtDeleteFile") == 0);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    teardown();
}

/*
 * THE MEASURED MODE: a FILE named from root 0 inside a CACHE.... directory of partition1,
 * attributes 0x40. The real host file goes, its siblings stay, and the status is the plain
 * success the title's `return ebx(0)` path relies on. A second delete of the same name is an
 * honest absence (OBJECT_NAME_NOT_FOUND), counted as no refusal.
 *
 * MUTATION: delete nothing and report success, delete every file in the directory, delete in
 * the wrong directory, or answer success for an absent name, and this fails.
 */
static void test_the_measured_cache_file_is_deleted_and_its_siblings_stay(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("CACHEGSRV");
    host_write_file("CACHEGSRV/LocalCache02.bin", "two", 3u);
    host_write_file("CACHEGSRV/LocalCache03.bin", "three", 5u);
    host_write_file("LocalCache02.bin", "decoy", 5u);
    const unsigned descriptors = open_descriptor_count();

    CHECK_EQ_U32(delete_rooted(0u, HDD_DEVICE "\\CACHEGSRV\\LocalCache02.bin"), STATUS_SUCCESS);
    CHECK(!host_exists("CACHEGSRV/LocalCache02.bin"));
    CHECK(host_exists("CACHEGSRV/LocalCache03.bin"));
    CHECK(host_exists("LocalCache02.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 0u);
    CHECK(captured_contains("NtDeleteFile"));
    CHECK(captured_contains("DELETED"));

    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\CACHEGSRV\\LocalCache02.bin"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 0u);
    CHECK(host_exists("CACHEGSRV/LocalCache03.bin"));
    CHECK_EQ_U32(open_descriptor_count(), descriptors);
    teardown();
}

/* A formatted cache partition's directory view deletes through the same code (INFERRED, not a
 * measured mode). MUTATION: send the cache view to the wrong root descriptor (the raw-image
 * directory) and the file stays, or delete the image. */
static void test_a_formatted_cache_partition_view_deletes_a_file(void)
{
    setup();
    mount_a_formatted_cache_partition();
    host_make_dir(CACHE_DIR);
    host_write_file(CACHE_DIR "/LocalCache02.bin", "x", 1u);
    CHECK_EQ_U32(delete_name(CACHE_PREFIX "\\LocalCache02.bin"), STATUS_SUCCESS);
    CHECK(!host_exists(CACHE_DIR "/LocalCache02.bin"));
    CHECK(host_exists(CACHE_IMAGE));
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    teardown();
}

/* Names are matched case-insensitively, as in every open (attributes 0x40), and the host's
 * real spelling is what goes. MUTATION: match case-sensitively and this fails. */
static void test_the_name_is_matched_case_insensitively(void)
{
    setup();
    mount_the_hdd();
    host_write_file("Save.Dat", "x", 1u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\sAVE.dAT"), STATUS_SUCCESS);
    CHECK(!host_exists("Save.Dat"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    teardown();
}

/* A plain HDD volume and a nested name work the same, a missing parent is a missing PATH and
 * not a missing name. MUTATION: report NAME_NOT_FOUND for the missing parent, or walk the
 * wrong component. */
static void test_a_nested_host_file_is_deleted_and_a_missing_parent_is_a_missing_path(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("TDATA");
    host_write_file("TDATA/a.bin", "a", 1u);
    host_write_file("a.bin", "root", 4u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\TDATA\\a.bin"), STATUS_SUCCESS);
    CHECK(!host_exists("TDATA/a.bin"));
    CHECK(host_exists("a.bin"));
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\NOPE\\a.bin"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 0u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\absent.bin"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    teardown();
}

/*
 * THE CACHE DIRECTORY VIEW IS GATED ON THE FORMAT, exactly as an open is: an image without
 * the FATX magic answers UNRECOGNIZED_VOLUME and touches nothing, and no directory view is
 * created on the host for a delete.
 *
 * MUTATION: skip the cache_view_prepare gate in the delete and the unformatted partition
 * answers NAME_NOT_FOUND (or deletes a stray file) and this fails.
 */
static void test_an_unformatted_cache_partition_deletes_nothing(void)
{
    setup();
    host_write_file(CACHE_IMAGE, "\0\0\0\0", 4u);
    CHECK(kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, 5u, CACHE_IMAGE,
                                            CACHE_DIR, CACHE_CAPACITY));
    host_make_dir(CACHE_DIR);
    host_write_file(CACHE_DIR "/LocalCache02.bin", "x", 1u);
    CHECK_EQ_U32(delete_name(CACHE_PREFIX "\\LocalCache02.bin"),
                 KERNEL_FILE_STATUS_UNRECOGNIZED_VOLUME);
    CHECK(host_exists(CACHE_DIR "/LocalCache02.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    teardown();
}

/*
 * THE TITLE CLOSES FIRST. The measured closer calls its own CloseHandle before the delete, so
 * a delete after the close succeeds. A delete while a LIVE handle still holds the file is
 * STATUS_SHARING_VIOLATION (INFERRED, NT's answer to a delete of a file open without
 * FILE_SHARE_DELETE) and the file stays. The closed handle's slot is reclaimed lazily and
 * must NOT block the delete.
 *
 * MUTATION: ignore live handles (the delete succeeds), or treat a closed handle's stale slot
 * as live (the second delete fails), and this fails.
 */
static void test_a_live_handle_blocks_the_delete_and_a_closed_one_does_not(void)
{
    setup();
    mount_the_hdd();
    host_write_file("LocalCache02.bin", "held", 4u);
    const uint32_t handle = open_name(HDD_DEVICE "\\LocalCache02.bin");
    CHECK(handle != 0u);

    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\LocalCache02.bin"), STATUS_SHARING_VIOLATION_CODE);
    CHECK(host_exists("LocalCache02.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 1u);
    CHECK(captured_contains("SHARING"));

    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\LocalCache02.bin"), STATUS_SUCCESS);
    CHECK(!host_exists("LocalCache02.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);

    /* A different open file does not block a delete of this one. */
    host_write_file("other.bin", "o", 1u);
    host_write_file("mine.bin", "m", 1u);
    const uint32_t other = open_name(HDD_DEVICE "\\other.bin");
    CHECK(other != 0u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\mine.bin"), STATUS_SUCCESS);
    CHECK(host_exists("other.bin"));
    teardown();
}

/*
 * A DIRECTORY IS NOT A MEASURED MODE (the title deletes a file named LocalCache%02d.bin) and
 * is REFUSED loudly with no change, empty or not, rather than rounded to NT's remove-empty-
 * directory behaviour.
 *
 * MUTATION: rmdir an empty directory, or route a directory to unlink, and this fails.
 */
static void test_a_directory_is_refused_not_removed(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("empty");
    host_make_dir("full");
    host_write_file("full/f.bin", "f", 1u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\empty"), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\full"), STATUS_NOT_IMPLEMENTED);
    CHECK(host_exists("empty"));
    CHECK(host_exists("full/f.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 2u);
    CHECK(captured_contains("directory"));
    teardown();
}

/* The volume root and a virtual raw device are not files to delete. Refused with
 * ACCESS_DENIED, image untouched. MUTATION: unlink the device image, or fall through on an
 * empty leaf. */
static void test_the_volume_root_and_a_raw_device_are_refused(void)
{
    setup();
    mount_the_hdd();
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE, DEVICE_CAPACITY));
    host_write_file(DEVICE_FILE, "raw", 3u);
    CHECK_EQ_U32(delete_name(DEVICE_PREFIX), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists(DEVICE_FILE));
    /* Anything named INSIDE a plain device is the device's, not a missing file. */
    CHECK_EQ_U32(delete_name(DEVICE_PREFIX "\\x"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(delete_name(DEVICE_PREFIX "\\" DEVICE_FILE), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists(DEVICE_FILE));
    CHECK_EQ_U32(delete_name(HDD_DEVICE), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 5u);
    teardown();
}

/* A mounted disc is never written. MUTATION: let a disc volume reach the host unlink, report
 * success for it, or answer NAME_NOT_FOUND for a file that is there. */
static void test_a_disc_file_is_refused(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(delete_name(DISC_DEVICE "\\HELLO"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists("synthetic.iso"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 1u);
    CHECK(captured_contains("disc"));
    teardown();
}

/* A name no volume backs has no host file. Undeclared under FAIL is an honest absence. Declared,
 * or fabricated under EMPTY (what an open would hand back as a file), is refused loudly with
 * nothing invented. MUTATION: report success (as if a fabricated file were deleted), or answer
 * NOT_IMPLEMENTED for the plain miss. */
static void test_a_name_with_no_volume_behind_it_follows_the_open_policy(void)
{
    setup();
    CHECK_EQ_U32(delete_name("\\Device\\Harddisk0\\partition9\\x.dat"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 0u);
    CHECK(kernel_file_add_openable("\\Device\\Harddisk0\\partition9\\declared.dat"));
    CHECK_EQ_U32(delete_name("\\Device\\Harddisk0\\partition9\\declared.dat"),
                 STATUS_NOT_IMPLEMENTED);
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    CHECK_EQ_U32(delete_name("\\Device\\Harddisk0\\partition9\\x.dat"),
                 STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 2u);
    CHECK(captured_contains("no volume"));
    teardown();
}

/* Escapes: a ".." component, a symbolic link as the leaf, and a link as a parent are refused,
 * and nothing outside (or the link itself) is touched. MUTATION: follow the link, unlink the
 * link, or accept "..". */
static void test_escapes_and_symbolic_links_are_refused(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("inner");
    host_write_file("outside.bin", "o", 1u);
    host_write_file("inner/real.bin", "r", 1u);
    host_make_symlink("../outside.bin", "inner/link.bin");
    host_make_symlink("inner", "linkdir");

    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\inner\\..\\outside.bin"),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists("outside.bin"));
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 1u);

    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\inner\\link.bin"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists("inner/link.bin"));
    CHECK(host_exists("outside.bin"));
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 2u);

    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\linkdir\\real.bin"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists("inner/real.bin"));
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 3u);
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 3u);
    teardown();
}

/* A FIFO planted in the backing directory is neither a link nor a directory nor a regular
 * file. It is refused ACCESS_DENIED, counted, and stays. MUTATION: drop the regular-file test
 * and the FIFO is unlinked and the delete reports success. */
static void test_a_non_regular_host_object_is_refused_not_removed(void)
{
    setup();
    mount_the_hdd();
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, "pipe.bin");
    if (mkfifo(path, 0600) != 0) {
        printf("FATAL could not make a FIFO at \"%s\": %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\pipe.bin"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(host_exists("pipe.bin"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 1u);
    teardown();
}

/* Roots: the measured 0 and the drive-qualified 0xFFFFFFFD are the same policy NtOpenFile
 * applies, any other root is refused PATH_NOT_FOUND with the file intact. MUTATION: ignore
 * the root, or accept 0xFFFFFFFC / a handle value. */
static void test_the_root_policy_is_the_opens(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "12345", 5u);
    CHECK(kernel_file_add_symlink("\\??\\T:", HDD_DEVICE));
    CHECK_EQ_U32(delete_rooted(0xFFFFFFFCu, "T:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(delete_rooted(0x1234u, "T:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(host_exists("save.dat"));
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 2u);
    CHECK_EQ_U32(delete_rooted(MEASURED_ROOT, "Q:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(delete_rooted(MEASURED_ROOT, "T:\\save.dat"), STATUS_SUCCESS);
    CHECK(!host_exists("save.dat"));
    teardown();
}

/* Argument faults: nothing is deleted and the status is INVALID_PARAMETER. MUTATION: skip the
 * null check or ignore an unreadable name. */
static void test_argument_faults_delete_nothing(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "12345", 5u);
    uint32_t args[1] = {0u};
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 1u), STATUS_INVALID_PARAMETER);
    args[0] = 0x10u;
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 1u), STATUS_INVALID_PARAMETER);
    args[0] = build_attributes(0u, 0x10u, MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 1u), STATUS_INVALID_PARAMETER);
    /* No argument frame at all, and a frame with no arguments. */
    CHECK_EQ_U32(kernel_hle_call(ORD_NT_DELETE_FILE, NULL), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 0u), STATUS_INVALID_PARAMETER);
    CHECK(host_exists("save.dat"));
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    teardown();
}

/* OBJECT_ATTRIBUTES.attributes other than 0x40 is reported and counted, and the delete still
 * runs (as every open does). MUTATION: drop the report or refuse the delete. */
static void test_unmodelled_object_attributes_are_counted(void)
{
    setup();
    mount_the_hdd();
    host_write_file("a.dat", "1", 1u);
    host_write_file("b.dat", "2", 1u);
    uint32_t args[1] = {build_attributes(0u, build_name(HDD_DEVICE "\\a.dat"),
                                         MEASURED_OA_ATTRIBUTES)};
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 0u);
    args[0] = build_attributes(0u, build_name(HDD_DEVICE "\\b.dat"), 0x80u);
    CHECK_EQ_U32(call_ordinal(ORD_NT_DELETE_FILE, args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 1u);
    CHECK(captured_contains("NtDeleteFile"));
    CHECK(!host_exists("b.dat"));
    teardown();
}

/* A host file the process may not remove is a failure with a status the title can read,
 * not a lie. (Skipped for a superuser, who can remove it.) MUTATION: report success on a
 * failed unlink. */
static void test_a_host_failure_is_reported_not_swallowed(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("locked");
    host_write_file("locked/f.bin", "f", 1u);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/locked", host_root);
    if (chmod(path, 0555) == 0 && access(path, W_OK) != 0) {
        CHECK_EQ_U32(delete_name(HDD_DEVICE "\\locked\\f.bin"), KERNEL_FILE_STATUS_ACCESS_DENIED);
        CHECK(host_exists("locked/f.bin"));
        CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
        CHECK_EQ_U32(kernel_file_delete_refused_count(), 1u);
        (void)chmod(path, 0755);
    }
    teardown();
}

/* reset gives both counters back. MUTATION: leave either out of kernel_file_reset. */
static void test_reset_clears_the_counters(void)
{
    setup();
    mount_the_hdd();
    host_write_file("a.dat", "1", 1u);
    host_make_dir("d");
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\a.dat"), STATUS_SUCCESS);
    CHECK_EQ_U32(delete_name(HDD_DEVICE "\\d"), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_file_deleted_count(), 1u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 1u);
    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_deleted_count(), 0u);
    CHECK_EQ_U32(kernel_file_delete_refused_count(), 0u);
    teardown();
}

int main(void)
{
    printf("NtDeleteFile (195) tests\n");
    test_registered_by_name();
    test_the_measured_cache_file_is_deleted_and_its_siblings_stay();
    test_a_formatted_cache_partition_view_deletes_a_file();
    test_the_name_is_matched_case_insensitively();
    test_a_nested_host_file_is_deleted_and_a_missing_parent_is_a_missing_path();
    test_an_unformatted_cache_partition_deletes_nothing();
    test_a_live_handle_blocks_the_delete_and_a_closed_one_does_not();
    test_a_directory_is_refused_not_removed();
    test_the_volume_root_and_a_raw_device_are_refused();
    test_a_disc_file_is_refused();
    test_a_name_with_no_volume_behind_it_follows_the_open_policy();
    test_escapes_and_symbolic_links_are_refused();
    test_a_non_regular_host_object_is_refused_not_removed();
    test_the_root_policy_is_the_opens();
    test_argument_faults_delete_nothing();
    test_unmodelled_object_attributes_are_counted();
    test_a_host_failure_is_reported_not_swallowed();
    test_reset_clears_the_counters();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
