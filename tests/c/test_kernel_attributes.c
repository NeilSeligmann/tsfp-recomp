/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ordinal 210, NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION).
 *
 * THE ONE MEASURED SITE is the XAPI `GetFileAttributesExA` body at 0x00381BD2 (reached from
 * one XONLINE caller, 0x0042C789, which sums nFileSizeLow rounded to 16 KiB). It builds an
 * OBJECT_ATTRIBUTES {root 0xFFFFFFFD, name, attributes 0x40} on its stack, pushes
 * `&info` then `&attributes`, and copies the 0x38-byte result field by field into a
 * WIN32_FILE_ATTRIBUTE_DATA. So the layout this suite pins is the one the guest reads:
 * +0x00 creation, +0x08 access, +0x10 write, +0x20 allocation (never read), +0x28 end of
 * file, +0x30 attributes. +0x18 (change time) is not read by the guest and is INFERRED.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF ANY REAL DISC. Guest structures are built in
 * scratch guest memory, the host side is a temporary directory beside this binary (removed
 * by teardown and by an atexit handler), and the one disc is bytes this file writes itself.
 * Nothing here skips.
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

#define ORD_NT_QUERY_FULL_ATTRIBUTES_FILE 210u

/* The layout the guest reads, written out here as literals so a wrong constant in the
 * handler cannot also be a wrong constant in the test. */
#define INFO_CREATION 0x00u
#define INFO_ACCESS 0x08u
#define INFO_WRITE 0x10u
#define INFO_CHANGE 0x18u
#define INFO_ALLOCATION 0x20u
#define INFO_END_OF_FILE 0x28u
#define INFO_ATTRIBUTES 0x30u
#define INFO_TAIL 0x34u
#define INFO_BYTES 0x38u

#define ATTRIBUTE_DIRECTORY 0x10u
#define ATTRIBUTE_ARCHIVE 0x20u

/* The root the measured site stores, and the OBJECT_ATTRIBUTES.attributes it stores. */
#define MEASURED_ROOT 0xFFFFFFFDu
#define MEASURED_OA_ATTRIBUTES 0x40u

/* FILETIME of Unix second 1700000000 plus 500 ns, derived independently:
 * (1700000000 + 11644473600) * 10^7 + 5. The access time is 100 seconds later. */
#define FILETIME_WRITE 133444736000000005ull
#define FILETIME_ACCESS 133444737000000000ull

#define SCRATCH_BYTES 0x4000u
#define FRAME_OFFSET 0x000u
#define FRAME_BYTES 0x100u
#define OA_OFFSET 0x200u
#define STRING_OFFSET 0x220u
#define CHARS_OFFSET 0x280u
#define INFO_OFFSET 0x400u
#define SENTINEL_BYTE 0x5Au

#define HDD_DEVICE "\\Device\\Harddisk0\\partition1"
#define DISC_DEVICE "\\Device\\CdRom0"
#define DEVICE_PREFIX "\\Device\\Harddisk0\\partition0"
#define DEVICE_FILE "partition0.bin"
#define DEVICE_CAPACITY 0x80000ull

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

static void host_set_times(const char *relative, time_t access_seconds, long access_ns,
                           time_t write_seconds, long write_ns)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    const struct timespec times[2] = {{access_seconds, access_ns}, {write_seconds, write_ns}};
    if (utimensat(AT_FDCWD, path, times, 0) != 0) {
        printf("FATAL could not set times on \"%s\": %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
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

static uint64_t read_u64_at(kernel_guest_ptr address)
{
    return (uint64_t)read_u32_at(address) | ((uint64_t)read_u32_at(address + 4u) << 32);
}

static uint64_t info_u64(uint32_t offset)
{
    return read_u64_at(at(INFO_OFFSET) + offset);
}

static uint32_t info_u32(uint32_t offset)
{
    return read_u32_at(at(INFO_OFFSET) + offset);
}

/* The result buffer starts as sentinel bytes, so "untouched" is never confusable with
 * "written zero". Wider than the structure, so a write past 0x38 is seen too. */
static void poison_info(void)
{
    for (uint32_t i = 0u; i < INFO_BYTES + 0x40u; i++) {
        if (!kernel_guest_write_u8(at(INFO_OFFSET) + i, SENTINEL_BYTE)) {
            printf("FATAL could not poison the result buffer\n");
            exit(EXIT_FAILURE);
        }
    }
}

static bool info_untouched(void)
{
    for (uint32_t i = 0u; i < INFO_BYTES + 0x40u; i++) {
        uint8_t value = 0u;
        if (!kernel_guest_read_u8(at(INFO_OFFSET) + i, &value) || value != SENTINEL_BYTE) {
            return false;
        }
    }
    return true;
}

static void setup(void)
{
    kernel_hle_init();
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_file_register(), 7u);
    /* TEN: 210 is the tenth. A dropped binding leaves a stub returning 0, which reads as
     * STATUS_SUCCESS with nothing written, so the exact count is what stops this passing
     * against nothing. */
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
        printf("FATAL could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
    poison_info();
    make_host_root();
}

static void teardown(void)
{
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

static uint32_t call_with(kernel_guest_ptr oa, kernel_guest_ptr info)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    const uint32_t args[2] = {oa, info};
    if (!kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, args, 2u)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamped to exactly the two arguments built, so a read of a third FAILS. */
    frame.stack_limit = at(FRAME_OFFSET) + 3u * 4u;
    return kernel_hle_call(ORD_NT_QUERY_FULL_ATTRIBUTES_FILE, &frame);
}

/* The measured shape: root 0, or 0xFFFFFFFD for a drive-qualified name. */
static uint32_t query_rooted(uint32_t root, const char *path)
{
    return call_with(build_attributes(root, build_name(path), MEASURED_OA_ATTRIBUTES),
                     at(INFO_OFFSET));
}

static uint32_t query(const char *path)
{
    return query_rooted(0u, path);
}

static void mount_the_hdd(void)
{
    CHECK(kernel_file_mount_host_dir(HDD_DEVICE, host_root));
    CHECK_EQ_U32(kernel_file_volume_count(), 1u);
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
static void test_registered_by_name(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_NT_QUERY_FULL_ATTRIBUTES_FILE);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->name != NULL &&
          strcmp(entry->name, "NtQueryFullAttributesFile") == 0);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    teardown();
}

/*
 * A REAL HOST FILE: size, attributes and all three times come from the host, in the layout
 * the guest reads, and the whole 0x38 bytes are written (tail included).
 *
 * MUTATION: swap any two of the 64-bit stores, write the size at +0x20, drop the tail
 * store, or report the access time as the write time, and this fails.
 */
static void test_a_host_file_reports_real_size_attributes_and_times(void)
{
    setup();
    mount_the_hdd();
    static const char payload[] = "TSFP-SAVE-DATA";
    host_write_file("save.dat", payload, sizeof(payload) - 1u);
    host_set_times("save.dat", 1700000100, 0, 1700000000, 500);

    CHECK_EQ_U32(query(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), sizeof(payload) - 1u);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_ARCHIVE);
    CHECK_EQ_U64(info_u64(INFO_WRITE), FILETIME_WRITE);
    CHECK_EQ_U64(info_u64(INFO_ACCESS), FILETIME_ACCESS);
    /* INFERRED, announced policy: creation and change follow the last write. */
    CHECK_EQ_U64(info_u64(INFO_CREATION), FILETIME_WRITE);
    CHECK_EQ_U64(info_u64(INFO_CHANGE), FILETIME_WRITE);
    /* T951 xemu reference: HDD NetworkOpenInformation allocation equals EOF. */
    CHECK_EQ_U64(info_u64(INFO_ALLOCATION), 14u);
    CHECK_EQ_U32(info_u32(INFO_TAIL), 0u);
    /* Real host times are not fabricated ones, in the log or in the counter. */
    CHECK(!captured_contains("FABRICATED"));
    CHECK_EQ_U32(kernel_io_fabricated_timestamp_count(), 0u);
    /* Not one byte past the 0x38-byte structure was written. */
    for (uint32_t i = INFO_BYTES; i < INFO_BYTES + 0x40u; i++) {
        uint8_t value = 0u;
        CHECK(kernel_guest_read_u8(at(INFO_OFFSET) + i, &value) && value == SENTINEL_BYTE);
    }
    teardown();
}

/* An empty file and a file whose size does not fit 32 bits: the end of file is 64-bit. The
 * allocation is computed in 64 bits too, so it neither wraps nor truncates.
 *
 * MUTATION: store only the low half of the end of file, or compute the sector rounding in
 * 32 bits, and this fails. */
static void test_size_is_sixty_four_bit_and_zero_is_zero(void)
{
    setup();
    mount_the_hdd();
    host_write_file("empty.dat", "", 0u);
    CHECK_EQ_U32(query(HDD_DEVICE "\\empty.dat"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 0u);
    CHECK_EQ_U64(info_u64(INFO_ALLOCATION), 0u);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_ARCHIVE);

    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, "huge.dat");
    const int descriptor = open(path, O_CREAT | O_WRONLY, 0666);
    CHECK(descriptor >= 0);
    const uint64_t huge = 0x100000005ull;
    const bool sparse = descriptor >= 0 && ftruncate(descriptor, (off_t)huge) == 0;
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    if (sparse) {
        poison_info();
        CHECK_EQ_U32(query(HDD_DEVICE "\\huge.dat"), STATUS_SUCCESS);
        CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), huge);
        /* T951 replaces the older INFERRED HDD sector rounding. */
        CHECK_EQ_U64(info_u64(INFO_ALLOCATION), 0x100000005ull);
    } else {
        printf("FATAL the scratch filesystem has no sparse files\n");
        failures++;
    }
    teardown();
}

/* A directory, and the volume root: attribute 0x10, size 0, real times. */
static void test_directories_and_the_volume_root(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("TDATA");
    host_set_times("TDATA", 1700000100, 0, 1700000000, 500);

    CHECK_EQ_U32(query(HDD_DEVICE "\\TDATA"), STATUS_SUCCESS);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_DIRECTORY);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 0u);
    CHECK_EQ_U64(info_u64(INFO_WRITE), FILETIME_WRITE);

    poison_info();
    CHECK_EQ_U32(query(HDD_DEVICE "\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_DIRECTORY);
    CHECK(info_u64(INFO_WRITE) != 0u && info_u32(INFO_TAIL) == 0u);
    teardown();
}

/* Name matching is case-insensitive, as the open path's is. */
static void test_the_name_is_matched_case_insensitively(void)
{
    setup();
    mount_the_hdd();
    host_write_file("Save.Dat", "abc", 3u);
    CHECK_EQ_U32(query(HDD_DEVICE "\\SAVE.DAT"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 3u);
    teardown();
}

/*
 * A DISC FILE: real size and the disc's own attribute, allocation rounded to its sector,
 * and the four times are FABRICATED zeros that are announced (XDVDFS has none per entry).
 *
 * MUTATION: report host times for a disc entry, drop the announcement, or round the
 * allocation to anything but the sector, and this fails.
 */
static void test_a_disc_file_has_size_and_no_times(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));

    CHECK_EQ_U32(query(DISC_DEVICE "\\HELLO"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 5u);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_ARCHIVE);
    CHECK_EQ_U64(info_u64(INFO_ALLOCATION), XDVDFS_SECTOR_SIZE);
    CHECK_EQ_U64(info_u64(INFO_CREATION), 0u);
    CHECK_EQ_U64(info_u64(INFO_ACCESS), 0u);
    CHECK_EQ_U64(info_u64(INFO_WRITE), 0u);
    CHECK_EQ_U64(info_u64(INFO_CHANGE), 0u);
    CHECK(captured_contains("FABRICATED zero timestamps"));
    CHECK_EQ_U32(kernel_io_fabricated_timestamp_count(), 1u);
    CHECK_EQ_U32(kernel_file_disc_opened_count(), 0u);

    poison_info();
    CHECK_EQ_U32(query(DISC_DEVICE "\\ABSENT"), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(info_untouched());
    teardown();
}

/*
 * A REFUSED QUERY CONSUMES NOTHING AND WRITES NOTHING: no handle, no open slot, no leaked
 * descriptor, the result buffer untouched, and the title's open-attempt log unchanged
 * (that log is what the title tried to OPEN).
 *
 * MUTATION: write the structure before resolving, create a handle, or skip the descriptor
 * release in the query path, and this fails.
 */
static void test_a_refused_query_leaves_no_trace(void)
{
    setup();
    mount_the_hdd();
    host_write_file("real.dat", "x", 1u);
    const unsigned descriptors = open_descriptor_count();

    CHECK_EQ_U32(query(HDD_DEVICE "\\absent.dat"), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(info_untouched());
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_file_open_count(), 0u);
    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    CHECK_EQ_U32(kernel_file_attribute_query_count(), 1u);

    /* A name that DOES resolve leaves no handle, slot or descriptor either. */
    CHECK_EQ_U32(query(HDD_DEVICE "\\real.dat"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_file_open_count(), 0u);
    CHECK_EQ_U32(kernel_file_attempt_count(), 0u);
    CHECK_EQ_U32(kernel_file_host_opened_count(), 0u);
    CHECK_EQ_U32(kernel_file_attribute_query_count(), 2u);

    for (unsigned i = 0u; i < 70u; i++) { /* more than the 64 open slots */
        CHECK_EQ_U32(query(HDD_DEVICE "\\real.dat"), STATUS_SUCCESS);
    }
    CHECK_EQ_U32(open_descriptor_count(), descriptors);
    teardown();
}

/* A missing parent is a missing PATH, not a missing name, exactly as the open path says. */
static void test_a_missing_parent_is_not_a_missing_name(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(query(HDD_DEVICE "\\nodir\\file.dat"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(info_untouched());
    teardown();
}

/* The escape refusals of the open path apply to a query too: `..` and a host symbolic link
 * answer ACCESS_DENIED and report nothing about what is outside the backing directory. */
static void test_escapes_and_symbolic_links_are_refused(void)
{
    setup();
    mount_the_hdd();
    host_write_file("inside.dat", "x", 1u);
    host_make_symlink("inside.dat", "link.dat");

    CHECK_EQ_U32(query(HDD_DEVICE "\\..\\escaped"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(info_untouched());
    CHECK_EQ_U32(query(HDD_DEVICE "\\link.dat"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(info_untouched());
    CHECK(kernel_file_escape_refused_count() >= 1u);
    teardown();
}

/*
 * THE MEASURED ROOT. The site stores 0xFFFFFFFD and a drive-qualified name, which resolves
 * through the title's `\??\X:` link under the bounded INFERRED policy. Any other root is
 * REFUSED as the open path refuses it, and counted.
 *
 * MUTATION: skip the root policy (treat any root as absolute), or drop the "\??\" prefix,
 * and this fails.
 */
static void test_the_measured_drive_root_resolves_and_other_roots_are_refused(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "12345", 5u);
    CHECK(kernel_file_add_symlink("\\??\\T:", HDD_DEVICE));

    CHECK_EQ_U32(query_rooted(MEASURED_ROOT, "T:\\save.dat"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 5u);
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 0u);

    poison_info();
    CHECK_EQ_U32(query_rooted(0xFFFFFFFCu, "T:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(query_rooted(0x1234u, "T:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(info_untouched());
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 2u);

    /* No link for that letter: the same absence the open path reports. */
    CHECK_EQ_U32(query_rooted(MEASURED_ROOT, "Q:\\save.dat"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/*
 * THE MISSING-NAME POLICY IS THE OPEN PATH'S. Under FAIL an undeclared name is absent. Under
 * EMPTY it is a FABRICATED zero-length file, announced and counted, exactly what an open of
 * it would hand back. A mounted volume never fabricates.
 *
 * MUTATION: ignore the policy in the query (always fabricate, or never), or let a mounted
 * volume's miss fall through to it, and this fails.
 */
static void test_the_missing_policy_matches_the_open_path(void)
{
    setup();
    CHECK_EQ_U32(query("\\Device\\Harddisk0\\partition9\\x.dat"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK(info_untouched());

    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    CHECK_EQ_U32(query("\\Device\\Harddisk0\\partition9\\x.dat"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    CHECK(captured_contains("EMPTY, FABRICATED"));
    CHECK_EQ_U32(kernel_io_fabricated_timestamp_count(), 1u);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 0u);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_ARCHIVE);
    CHECK_EQ_U64(info_u64(INFO_WRITE), 0u);

    mount_the_hdd();
    poison_info();
    CHECK_EQ_U32(query(HDD_DEVICE "\\absent.dat"), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    CHECK(info_untouched());
    teardown();
}

/* A declared openable name answers its declared (zero-length) file, not a fabrication. */
static void test_a_declared_openable_name_is_an_empty_file_not_a_fabrication(void)
{
    setup();
    CHECK(kernel_file_add_openable("\\Device\\Foo\\bar.dat"));
    CHECK_EQ_U32(query("\\Device\\Foo\\bar.dat"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 0u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    teardown();
}

/* The virtual raw device reports the capacity it was mounted with (a partition does not
 * grow as it is written), and its backing file's host times. */
static void test_the_virtual_device_reports_its_capacity(void)
{
    setup();
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE,
                                        DEVICE_CAPACITY));
    CHECK_EQ_U32(query(DEVICE_PREFIX), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), DEVICE_CAPACITY);
    CHECK_EQ_U32(info_u32(INFO_ATTRIBUTES), ATTRIBUTE_ARCHIVE);
    teardown();
}

/*
 * ARGUMENT FAULTS write nothing. NULL or unreadable OBJECT_ATTRIBUTES, a NULL or
 * unwritable result, and a result whose last bytes fall off the mapped region (the write
 * is one probed range, so none of the structure lands).
 *
 * MUTATION: write field by field instead of one range (the first fields land), or accept a
 * NULL result as a success, and this fails.
 */
static void test_argument_faults_write_nothing(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "12345", 5u);
    const kernel_guest_ptr oa =
        build_attributes(0u, build_name(HDD_DEVICE "\\save.dat"), MEASURED_OA_ATTRIBUTES);

    CHECK_EQ_U32(call_with(0u, at(INFO_OFFSET)), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(call_with(0x10u, at(INFO_OFFSET)), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(call_with(oa, 0u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(call_with(oa, 0x10u), STATUS_INVALID_PARAMETER);
    CHECK(info_untouched());

    /* The last 4 bytes of the 0x38 structure fall past the end of the scratch region. */
    const kernel_guest_ptr edge = at(SCRATCH_BYTES) - (INFO_BYTES - 4u);
    for (uint32_t i = 0u; i < INFO_BYTES - 4u; i++) {
        CHECK(kernel_guest_write_u8(edge + i, SENTINEL_BYTE));
    }
    CHECK_EQ_U32(call_with(oa, edge), STATUS_INVALID_PARAMETER);
    uint32_t first = 0u;
    CHECK(kernel_guest_read_u32(edge, &first));
    CHECK_EQ_U32(first, 0x5A5A5A5Au);

    /* A name object that cannot be read. */
    const kernel_guest_ptr bad = build_attributes(0u, 0x10u, MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(call_with(bad, at(INFO_OFFSET)), STATUS_INVALID_PARAMETER);
    CHECK(info_untouched());
    /* And the good call still works, so the refusals above were about the arguments. (The
     * OBJECT_ATTRIBUTES lives in one slot, which `bad` overwrote, so it is rebuilt.) */
    const kernel_guest_ptr good =
        build_attributes(0u, build_name(HDD_DEVICE "\\save.dat"), MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(call_with(good, at(INFO_OFFSET)), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 5u);
    teardown();
}

/* OBJECT_ATTRIBUTES.attributes other than the measured 0x40 is reported and counted, and
 * the query still answers (the bits are ignored, as in every open). */
static void test_unmodelled_object_attributes_are_counted(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "12345", 5u);
    const kernel_guest_ptr oa = build_attributes(
        0u, build_name(HDD_DEVICE "\\save.dat"), MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(call_with(oa, at(INFO_OFFSET)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 0u);

    const kernel_guest_ptr other = build_attributes(
        0u, build_name(HDD_DEVICE "\\save.dat"), 0x80u);
    CHECK_EQ_U32(call_with(other, at(INFO_OFFSET)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_unmodelled_attributes_count(), 1u);
    CHECK(captured_contains("NtQueryFullAttributesFile"));
    teardown();
}

/* A read-only host file is queried through a READ-ONLY descriptor: the resolution asks for
 * no access, so the host never refuses a write the title did not ask for. (A superuser can
 * open any file read-write, so the log line below only proves this for a normal user.) */
static void test_a_read_only_host_file_is_queried_without_asking_for_write(void)
{
    setup();
    mount_the_hdd();
    host_write_file("locked.dat", "abc", 3u);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, "locked.dat");
    CHECK(chmod(path, 0444) == 0);
    CHECK_EQ_U32(query(HDD_DEVICE "\\locked.dat"), STATUS_SUCCESS);
    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 3u);
    CHECK(!captured_contains("asked for write access"));
    teardown();
}

/* The library call zeroes its result on every failure, so a caller that ignores the status
 * cannot read a stale answer. */
static void test_the_library_call_zeroes_its_result_on_failure(void)
{
    setup();
    mount_the_hdd();
    kernel_file_attributes found;
    memset(&found, 0xAB, sizeof(found));
    const kernel_guest_ptr oa = build_attributes(
        0u, build_name(HDD_DEVICE "\\absent.dat"), MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(kernel_file_query_attributes(oa, &found),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U64(found.size, 0u);
    CHECK(!found.is_directory && !found.times_known);
    CHECK_EQ_U64(found.last_write_time, 0u);
    CHECK_EQ_U32(kernel_file_query_attributes(0u, &found), STATUS_INVALID_PARAMETER);
    teardown();
}

/* The library call reports the BACKING the name resolved to, for a file and for a directory,
 * which no guest-visible structure carries (it is the host-object marker callers branch on).
 * BREAKS THIS: not copying the resolution's backing into the result (it stays EMPTY). */
static void test_the_library_call_reports_the_backing_it_resolved(void)
{
    setup();
    mount_the_hdd();
    host_write_file("backed.dat", "abc", 3u);
    host_make_dir("BACKEDDIR");
    kernel_file_attributes found;
    memset(&found, 0, sizeof(found));
    kernel_guest_ptr oa = build_attributes(
        0u, build_name(HDD_DEVICE "\\backed.dat"), MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(kernel_file_query_attributes(oa, &found), STATUS_SUCCESS);
    CHECK(found.backing == KERNEL_FILE_BACKING_HOST_DIR);
    CHECK(!found.is_directory);
    memset(&found, 0, sizeof(found));
    oa = build_attributes(0u, build_name(HDD_DEVICE "\\BACKEDDIR"), MEASURED_OA_ATTRIBUTES);
    CHECK_EQ_U32(kernel_file_query_attributes(oa, &found), STATUS_SUCCESS);
    CHECK(found.backing == KERNEL_FILE_BACKING_HOST_DIR);
    CHECK(found.is_directory);
    teardown();
}

/* One file, one answer: the class-0x22 answer of NtQueryInformationFile (211) for an open
 * handle and ordinal 210 for the same name write the same size, allocation, attribute and
 * tail. Only the times differ, because 211 knows no times (it answers zeros). This also pins
 * the shared writer against a non-empty file, which the class-0x22 suite does not.
 *
 * MUTATION: let either path store a different size or allocation, and this fails. */
static void test_class_0x22_and_ordinal_210_agree_on_one_file(void)
{
    setup();
    mount_the_hdd();
    host_write_file("both.dat", "0123456789", 10u);
    CHECK_EQ_U32(query(HDD_DEVICE "\\both.dat"), STATUS_SUCCESS);
    uint8_t from_210[INFO_BYTES];
    CHECK(kernel_guest_read_bytes(at(INFO_OFFSET), from_210, sizeof(from_210)));

    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    const kernel_guest_ptr oa = build_attributes(
        0u, build_name(HDD_DEVICE "\\both.dat"), MEASURED_OA_ATTRIBUTES);
    const uint32_t open_args[6] = {at(0x700u), 0x00100001u, oa, at(0x720u), 3u, 0x21u};
    CHECK(kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, open_args, 6u));
    frame.stack_limit = at(FRAME_OFFSET) + 7u * 4u;
    CHECK_EQ_U32(kernel_hle_call(202u, &frame), STATUS_SUCCESS);
    const uint32_t handle = read_u32_at(at(0x700u));
    CHECK(handle != 0u);

    poison_info();
    const uint32_t info_args[5] = {handle, at(0x720u), at(INFO_OFFSET), INFO_BYTES, 0x22u};
    CHECK(kernel_frame_build(&frame, at(FRAME_OFFSET), FRAME_BYTES, info_args, 5u));
    frame.stack_limit = at(FRAME_OFFSET) + 6u * 4u;
    CHECK_EQ_U32(kernel_hle_call(211u, &frame), STATUS_SUCCESS);
    uint8_t from_211[INFO_BYTES];
    CHECK(kernel_guest_read_bytes(at(INFO_OFFSET), from_211, sizeof(from_211)));

    CHECK_EQ_U64(info_u64(INFO_END_OF_FILE), 10u);
    /* Bytes 0x20..0x37 (allocation, end of file, attributes, tail) are identical. */
    CHECK(memcmp(&from_210[INFO_ALLOCATION], &from_211[INFO_ALLOCATION],
                 INFO_BYTES - INFO_ALLOCATION) == 0);
    teardown();
}

/* reset gives the query counter back. */
static void test_reset_clears_the_query_count(void)
{
    setup();
    mount_the_hdd();
    host_write_file("save.dat", "1", 1u);
    CHECK_EQ_U32(query(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_attribute_query_count(), 1u);
    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_attribute_query_count(), 0u);
    teardown();
}

int main(void)
{
    printf("NtQueryFullAttributesFile (210) tests\n");
    test_registered_by_name();
    test_a_host_file_reports_real_size_attributes_and_times();
    test_size_is_sixty_four_bit_and_zero_is_zero();
    test_directories_and_the_volume_root();
    test_the_name_is_matched_case_insensitively();
    test_a_disc_file_has_size_and_no_times();
    test_a_refused_query_leaves_no_trace();
    test_a_missing_parent_is_not_a_missing_name();
    test_escapes_and_symbolic_links_are_refused();
    test_the_measured_drive_root_resolves_and_other_roots_are_refused();
    test_the_missing_policy_matches_the_open_path();
    test_a_declared_openable_name_is_an_empty_file_not_a_fabrication();
    test_the_virtual_device_reports_its_capacity();
    test_argument_faults_write_nothing();
    test_unmodelled_object_attributes_are_counted();
    test_a_read_only_host_file_is_queried_without_asking_for_write();
    test_the_library_call_zeroes_its_result_on_failure();
    test_the_library_call_reports_the_backing_it_resolved();
    test_class_0x22_and_ordinal_210_agree_on_one_file();
    test_reset_clears_the_query_count();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
