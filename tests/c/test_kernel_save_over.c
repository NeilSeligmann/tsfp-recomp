/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1214 / T1090: SAVING OVER AN EXISTING FILE on a host-directory --hdd volume, and the
 * status the title then asks RtlNtStatusToDosError about.
 *
 * Owner evidence 2026-10-06: Map Maker NtCreateFile("\\??\\U:\\3E67F296FD7B") with FILE_CREATE on
 * a name that already exists. The kernel reports STATUS_OBJECT_NAME_COLLISION (0xC0000035,
 * correct for FILE_CREATE, MEASURED against kernel_file.c semantics), and the title asks
 * RtlNtStatusToDosError, which returned 317 (unmapped) instead of ERROR_ALREADY_EXISTS (183,
 * INFERRED from the standard NT table). This test pins the whole chain and the replace paths a
 * save-over can use (FILE_OVERWRITE_IF truncates, FILE_SUPERSEDE, a collision creates nothing
 * and leaves the old bytes), plus a two-process write / save over / read back round trip.
 * Free of lifted code and discs; the scratch host directory is beside this binary.
 */
#include "kernel_file.h"

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "kernel_rtl.h"
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
#include <sys/wait.h>
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
    (void)kernel_rtl_register();
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

static uint32_t close_handle(uint32_t handle)
{
    const uint32_t args[1] = {handle};
    return call_ordinal(ORD_NT_CLOSE, args, 1u);
}

#define ORD_NT_CREATE_FILE 190u
#define ORD_NT_WRITE_FILE 236u
#define ORD_NT_READ_FILE 219u
#define ORD_RTL_NT_STATUS_TO_DOS_ERROR 301u
#define FILE_SUPERSEDE_D 0u
#define FILE_OPEN_D 1u
#define FILE_CREATE_D 2u
#define FILE_OPEN_IF_D 3u
#define FILE_OVERWRITE_IF_D 5u
#define OPT_DIRECTORY 0x1u
#define OPT_SYNC_NONALERT 0x20u
#define OPT_NON_DIRECTORY 0x40u
#define ACCESS_RW 0xC0100000u
#define PATH_A HDD_DEVICE "\\save.bin"
#define COLLISION 0xC0000035u

static uint32_t create_file(const char *path, uint32_t disposition, uint32_t options,
                            uint32_t *handle_out, uint32_t *information_out)
{
    write_u32_at(at(HANDLE_OFFSET), 0u);
    const uint32_t args[9] = {at(HANDLE_OFFSET), ACCESS_RW,
                              build_attributes(0u, build_name(path), MEASURED_OA_ATTRIBUTES),
                              at(IOSB_OFFSET), 0u, 0x80u, 3u, disposition, options};
    const uint32_t status = call_ordinal(ORD_NT_CREATE_FILE, args, 9u);
    *handle_out = read_u32_at(at(HANDLE_OFFSET));
    *information_out = read_u32_at(at(IOSB_OFFSET) + 4u);
    return status;
}

static uint32_t write_bytes(uint32_t handle, uint8_t fill, uint32_t length, uint32_t offset)
{
    for (uint32_t i = 0u; i < length; i++) {
        (void)kernel_guest_write_u8(at(0x1000u) + i, fill);
    }
    write_u32_at(at(0x500u), offset);
    write_u32_at(at(0x504u), 0u);
    const uint32_t args[8] = {handle, 0u, 0u, 0u, at(IOSB_OFFSET), at(0x1000u), length,
                              at(0x500u)};
    return call_ordinal(ORD_NT_WRITE_FILE, args, 8u);
}

static uint32_t read_bytes(uint32_t handle, uint32_t length, uint8_t *out)
{
    write_u32_at(at(0x500u), 0u);
    write_u32_at(at(0x504u), 0u);
    const uint32_t args[8] = {handle, 0u, 0u, 0u, at(IOSB_OFFSET), at(0x2000u), length,
                              at(0x500u)};
    const uint32_t status = call_ordinal(ORD_NT_READ_FILE, args, 8u);
    for (uint32_t i = 0u; i < length; i++) {
        (void)kernel_guest_read_u8(at(0x2000u) + i, &out[i]);
    }
    return status;
}

static uint32_t dos_error_of(uint32_t status)
{
    const uint32_t args[1] = {status};
    return call_ordinal(ORD_RTL_NT_STATUS_TO_DOS_ERROR, args, 1u);
}

static size_t host_size(const char *relative, uint8_t *content, size_t capacity)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    FILE *file = fopen(path, "rb");
    if (!file) {
        return (size_t)-1;
    }
    const size_t got = fread(content, 1u, capacity, file);
    (void)fclose(file);
    return got;
}

static bool all_equal(const uint8_t *bytes, size_t length, uint8_t value)
{
    for (size_t i = 0u; i < length; i++) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static void test_file_create_on_an_existing_name_collides_and_maps_to_183(void)
{
    setup();
    mount_the_hdd();
    uint32_t handle = 0u, information = 0u;
    CHECK_EQ_U32(create_file(PATH_A, FILE_CREATE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                             &handle, &information), STATUS_SUCCESS);
    CHECK(handle != 0u);
    CHECK_EQ_U32(information, 2u); /* FILE_CREATED */
    CHECK_EQ_U32(write_bytes(handle, 'A', 100u, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);

    handle = 0u;
    const uint32_t status = create_file(PATH_A, FILE_CREATE_D,
                                        OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle,
                                        &information);
    CHECK_EQ_U32(status, COLLISION);
    CHECK_EQ_U32(handle, 0u);
    CHECK_EQ_U32(information, 4u); /* FILE_EXISTS */
    CHECK(captured_contains("collision"));
    uint8_t content[256];
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 100u);
    CHECK(all_equal(content, 100u, 'A')); /* the collision destroyed nothing */
    /* The title asks for the Win32 error: must be ERROR_ALREADY_EXISTS, not 317. */
    CHECK_EQ_U32(dos_error_of(status), 183u);
    CHECK(!captured_contains("has no mapping"));

    /* A directory create over an existing directory collides the same way. */
    host_make_dir("3E67F296FD7B");
    CHECK_EQ_U32(create_file(HDD_DEVICE "\\3E67F296FD7B", FILE_CREATE_D, OPT_DIRECTORY,
                             &handle, &information), COLLISION);
    teardown();
}

static void test_overwrite_if_truncates_so_no_old_tail_survives(void)
{
    setup();
    mount_the_hdd();
    uint32_t handle = 0u, information = 0u;
    host_write_file("save.bin", "OLDOLDOLDOLDOLDOLD", 18u);
    CHECK_EQ_U32(create_file(PATH_A, FILE_OVERWRITE_IF_D,
                             OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle, &information),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(information, 3u); /* FILE_OVERWRITTEN */
    CHECK_EQ_U32(write_bytes(handle, 'N', 5u, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    uint8_t content[64];
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 5u);
    CHECK(all_equal(content, 5u, 'N'));

    /* FILE_SUPERSEDE replaces too. */
    CHECK_EQ_U32(create_file(PATH_A, FILE_SUPERSEDE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                             &handle, &information), STATUS_SUCCESS);
    CHECK_EQ_U32(write_bytes(handle, 'S', 3u, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 3u);
    CHECK(all_equal(content, 3u, 'S'));

    /* FILE_OPEN_IF on an existing file opens it untouched. */
    CHECK_EQ_U32(create_file(PATH_A, FILE_OPEN_IF_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                             &handle, &information), STATUS_SUCCESS);
    CHECK_EQ_U32(information, 1u);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 3u);
    teardown();
}

static void test_the_collision_then_replace_sequence_the_title_runs(void)
{
    setup();
    mount_the_hdd();
    uint32_t handle = 0u, information = 0u;
    host_write_file("save.bin", "0123456789", 10u);
    /* FILE_CREATE collides, the title reads 183, deletes and creates again. */
    const uint32_t status = create_file(PATH_A, FILE_CREATE_D,
                                        OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle,
                                        &information);
    CHECK_EQ_U32(dos_error_of(status), 183u);
    CHECK_EQ_U32(delete_name(PATH_A), STATUS_SUCCESS);
    CHECK(!host_exists("save.bin"));
    CHECK_EQ_U32(create_file(PATH_A, FILE_CREATE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                             &handle, &information), STATUS_SUCCESS);
    CHECK_EQ_U32(write_bytes(handle, 'Z', 4u, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    uint8_t content[32];
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 4u);
    CHECK(all_equal(content, 4u, 'Z'));
    teardown();
}

/* Two real processes over one --hdd tree: the first writes then saves over, the second reads. */
static void test_two_process_write_save_over_read_back(void)
{
    setup();
    mount_the_hdd();
    const pid_t child = fork();
    if (child == 0) {
        uint32_t handle = 0u, information = 0u;
        int bad = 0;
        bad |= create_file(PATH_A, FILE_CREATE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                           &handle, &information) != STATUS_SUCCESS;
        bad |= write_bytes(handle, 'A', 300u, 0u) != STATUS_SUCCESS;
        bad |= close_handle(handle) != STATUS_SUCCESS;
        bad |= create_file(PATH_A, FILE_CREATE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                           &handle, &information) != COLLISION;
        bad |= create_file(PATH_A, FILE_OVERWRITE_IF_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                           &handle, &information) != STATUS_SUCCESS;
        bad |= write_bytes(handle, 'B', 120u, 0u) != STATUS_SUCCESS;
        bad |= close_handle(handle) != STATUS_SUCCESS;
        _exit(bad ? 1 : 0);
    }
    CHECK(child > 0);
    int wait_status = 0;
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
    /* "Second process": fresh kernel state, same tree. */
    kernel_file_unmount_all();
    kernel_file_reset();
    mount_the_hdd();
    uint32_t handle = 0u, information = 0u;
    CHECK_EQ_U32(create_file(PATH_A, FILE_OPEN_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY,
                             &handle, &information), STATUS_SUCCESS);
    uint8_t content[300];
    CHECK_EQ_U32(read_bytes(handle, 120u, content), STATUS_SUCCESS);
    CHECK(all_equal(content, 120u, 'B'));
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    CHECK_EQ_U64(host_size("save.bin", content, sizeof(content)), 120u);
    teardown();
}

/* T1633: the file observer behind the event driven route sees opens, reads and writes with the guest's path. */
static struct { int opens, reads, writes; uint64_t read_bytes, write_bytes; char open_path[260]; } observed;
static void observe_file(kernel_file_event_kind kind, const char *path, uint64_t bytes)
{
    if (kind == KERNEL_FILE_EVENT_OPEN) {
        observed.opens++;
        snprintf(observed.open_path, sizeof(observed.open_path), "%s", path);
    } else if (kind == KERNEL_FILE_EVENT_READ) {
        observed.reads++;
        observed.read_bytes += bytes;
    } else if (kind == KERNEL_FILE_EVENT_WRITE) {
        observed.writes++;
        observed.write_bytes += bytes;
    }
}

static void test_t1633_file_observer(void)
{
    setup();
    mount_the_hdd();
    uint32_t handle = 0u, information = 0u;
    host_write_file("save.bin", "OLDOLDOLDOLDOLDOLD", 18u);
    memset(&observed, 0, sizeof(observed));
    kernel_file_set_observer(observe_file);
    CHECK_EQ_U32(create_file(PATH_A, FILE_OPEN_IF_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle, &information), STATUS_SUCCESS);
    CHECK_EQ_U32(observed.opens, 1u);
    CHECK(strstr(observed.open_path, "save.bin") != NULL);
    uint8_t content[32];
    CHECK_EQ_U32(read_bytes(handle, 7u, content), STATUS_SUCCESS);
    CHECK_EQ_U32(observed.reads, 1u);
    CHECK_EQ_U64(observed.read_bytes, 7u);
    CHECK_EQ_U32(write_bytes(handle, 'N', 5u, 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(observed.writes, 1u);
    CHECK_EQ_U64(observed.write_bytes, 5u);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    /* a refused open (FILE_CREATE on an existing name) is not an open event */
    CHECK(create_file(PATH_A, FILE_CREATE_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle, &information) != STATUS_SUCCESS);
    CHECK_EQ_U32(observed.opens, 1u);
    /* removing the observer stops the events */
    kernel_file_set_observer(NULL);
    CHECK_EQ_U32(create_file(PATH_A, FILE_OPEN_IF_D, OPT_SYNC_NONALERT | OPT_NON_DIRECTORY, &handle, &information), STATUS_SUCCESS);
    CHECK_EQ_U32(observed.opens, 1u);
    CHECK_EQ_U32(close_handle(handle), STATUS_SUCCESS);
    teardown();
}

int main(void)
{
    printf("kernel_file save-over tests (T1214)\n");
    test_t1633_file_observer();
    test_file_create_on_an_existing_name_collides_and_maps_to_183();
    test_overwrite_if_truncates_so_no_old_tail_survives();
    test_the_collision_then_replace_sequence_the_title_runs();
    test_two_process_write_save_over_read_back();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
