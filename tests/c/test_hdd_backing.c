/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The writable hard-disk backing store: `--hdd`, and NtCreateFile actually creating.
 *
 * WHY THERE IS A SUITE FOR THIS AT ALL. Until this existed the title REBOOTED ITSELF.
 * MEASURED at guest 0x00381321: the XDK startup creates `TDATA\45410066` with
 * disposition 3 (FILE_OPEN_IF) and CreateOptions 0x4021 (FILE_DIRECTORY_FILE | ...), got
 * 0xC0000034 because nothing behind this host could create, and the guest tests only for
 * STATUS_DISK_FULL -- so every other status jumps to XLaunchNewImage. That reboot path
 * then trapped on a NULL function pointer, which read for two investigations as a lifter
 * gap. The bug was a storage layer that could not store.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. WRITING THE USER'S DISC. Their image is their property. A writable volume must be
 *      a DIFFERENT volume, and "we would never do that" is not a mechanism.
 *      `test_a_create_on_a_disc_volume_...` mounts a synthetic XDVDFS image, asks for a
 *      create on it, and FINGERPRINTS THE FILE before and after.
 *   2. ESCAPING THE BACKING DIRECTORY. The guest is untrusted input. A path with `..` in
 *      it, or one crossing a host symbolic link, must not reach outside the directory the
 *      operator named. Two tests, and the symbolic-link one is the easy half to forget.
 *   3. BECOMING WRITABLE BY DEFAULT. Without `--hdd` the honest refusal has to be
 *      bit-for-bit what it was, status included. A title that silently got a working hard
 *      disk it was never given is how a wrong answer surfaces far from its cause.
 *   4. FILE_OPEN_IF TREATED AS FILE_CREATE. The measured sequence creates `TDATA` and
 *      then opens it again on the next pass. If the second open failed, the boot would
 *      take the same reboot path for a new reason and the fix would look like no fix.
 *   5. SAYING "OPENED" AFTER A CREATE. The guest reads
 *      IO_STATUS_BLOCK.information -- one measured site, 0x0037D394 -- so the field has
 *      to carry which of the two happened rather than a constant.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY REAL DISC. Guest structures are
 * built in scratch guest memory at the offsets `guest_structs.h` derives; the host side is
 * a temporary directory this suite makes and removes; the one disc is 70 KB of bytes this
 * file writes itself. Nothing here skips, so a green verdict means every case ran.
 *
 * NO HARDCODED HOST PATHS. The scratch directory is made beside this binary (the build
 * directory), found at run time from /proc/self/exe, so running the suite from the repo root
 * or anywhere else cannot drop `tsfp-hdd-test-*` into the working tree. It is removed by the
 * per-test teardown AND by an atexit handler, so a FATAL exit (which skips teardown) cannot
 * leak it either. Only a SIGKILL or a timeout kill can, which no in-process code can catch.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it. The mutations live
 * in `tools/mutate/c_suites.py` with the `hdd-` prefix.
 */

#include "kernel_file.h"

#include "guest_mem.h"
#include "kernel_io.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
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
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                         \
            failures++;                                                                    \
        }                                                                                 \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                         \
        uint32_t a_ = (uint32_t)(actual);                                                 \
        uint32_t e_ = (uint32_t)(expected);                                               \
        if (a_ != e_) {                                                                    \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,   \
                   (unsigned)a_, (unsigned)e_);                                            \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

/* The 64-bit companion. The byte counters a write moves are 64-bit, and comparing one
 * through CHECK_EQ_U32 would truncate -- which is the one way a count could be wrong and
 * still look right. */
#define CHECK_EQ_U64(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                         \
        uint64_t a_ = (uint64_t)(actual);                                                 \
        uint64_t e_ = (uint64_t)(expected);                                               \
        if (a_ != e_) {                                                                    \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__, #actual, \
                   (unsigned long long)a_, (unsigned long long)e_);                        \
            failures++;                                                                    \
        }                                                                                  \
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

#define ORD_NT_OPEN_FILE 202u
#define ORD_NT_CREATE_FILE 190u
#define ORD_NT_WRITE_FILE 236u
#define ORD_NT_SET_INFORMATION_FILE 226u
#define ORD_NT_READ_FILE 219u
/* MEASURED (class, Length) pair: FileEndOfFileInformation, a single 64-bit value. */
#define FILE_CLASS_END_OF_FILE 0x14u

/*
 * The literals the measured sites pass, used throughout so the suite exercises the shape
 * the guest actually sends rather than round numbers.
 *
 * 0x00381321 is the create that was failing: disposition 3, options 0x4021. 0x00380127
 * supplies the rest of the argument shape: access 0x100001, FileAttributes 0x80, share 3,
 * and a NULL AllocationSize.
 */
#define MEASURED_ACCESS 0x00100001u
/*
 * The access mask the ONE write this boot sets up actually uses.
 *
 * MEASURED at 0x00381027, the NtCreateFile that makes
 * `\Device\Harddisk0\partition1\UDATA\45410066\TitleMeta.xbx`: DesiredAccess 0x40100000,
 * i.e. GENERIC_WRITE | SYNCHRONIZE, with ShareAccess 1 and CreateOptions 0x22. Used
 * verbatim below so the write tests exercise the shape the guest sends rather than a
 * convenient one -- the whole reason a descriptor's mode is decided by this field is that
 * this field says the title intends to write.
 */
#define MEASURED_WRITE_ACCESS 0x40100000u
#define MEASURED_WRITE_SHARE 1u
#define MEASURED_WRITE_OPTIONS 0x00000022u
#define MEASURED_WRITE_ATTRIBUTES 4u
#define MEASURED_SHARE 3u
#define MEASURED_FILE_ATTRIBUTES 0x80u
#define MEASURED_CREATE_OPTIONS 0x00004021u
#define MEASURED_OPEN_OPTIONS 0x00800021u
#define MEASURED_ATTRIBUTES GUEST_OBJ_ATTRIBUTES_OBSERVED

/* NT CreateDisposition values. */
#define DISPOSITION_FILE_CREATE 2u
#define DISPOSITION_FILE_OPEN_IF 3u
#define DISPOSITION_FILE_OPEN 1u

/* NT CreateOptions bits. */
#define OPTION_DIRECTORY_FILE 0x00000001u
#define OPTION_NON_DIRECTORY_FILE 0x00000040u

/* NT disposition-result codes, which land in IO_STATUS_BLOCK.information. */
#define INFORMATION_OPENED 1u
#define INFORMATION_CREATED 2u
#define INFORMATION_EXISTS 4u

/* The partition the measured sites use, with NO trailing separator: the mount table needs
 * the character after a matched prefix to be a separator or the end of the name. */
#define HDD_DEVICE "\\Device\\Harddisk0\\partition1"
#define DISC_DEVICE "\\Device\\CdRom0"
/* The title ID is 0x45410066, which the XDK spells in hex as a directory name. */
#define GUEST_TDATA HDD_DEVICE "\\TDATA"
#define GUEST_TDATA_TITLE HDD_DEVICE "\\TDATA\\45410066"
/* The exact name the boot creates and would write to, read out of the trace rather than
 * invented: the attempt log records it at access 0x40100000, share 0x1, options 0x22. */
#define GUEST_UDATA HDD_DEVICE "\\UDATA"
#define GUEST_UDATA_TITLE HDD_DEVICE "\\UDATA\\45410066"

/*
 * The virtual raw device, and the literals the one measured site passes.
 *
 * MEASURED at 0x0037D887 (`XapiSelectCachePartition`) and 0x00433F87 (`XNetOpenConfigVolume`):
 * NtOpenFile of `\Device\Harddisk0\partition0` with access 0xC0100000 (GENERIC_READ |
 * GENERIC_WRITE | SYNCHRONIZE), share 3, options 0x10. The first then reads 0x200 bytes at
 * offset 0x800 and writes the same 0x200 back. The capacity is 512 KiB, which is INFERRED
 * from the console's config-area size rather than measured.
 */
#define DEVICE_PREFIX "\\Device\\Harddisk0\\partition0"
#define DEVICE_FILE "partition0.bin"
#define DEVICE_CAPACITY 0x80000ull
#define MEASURED_DEVICE_ACCESS 0xC0100000u
#define MEASURED_DEVICE_OPTIONS 0x10u
#define MEASURED_DEVICE_OFFSET 0x800u
#define MEASURED_DEVICE_LENGTH 0x200u
/* Outside every region the helpers above use, and wholly inside the scratch span. */
#define DEVICE_BUFFER_OFFSET 0x2000u

#define SCRATCH_BYTES 0x4000u
#define FRAME_OFFSET 0x000u
#define FRAME_BYTES 0x100u
#define OA_OFFSET 0x200u
#define STRING_OFFSET 0x220u
#define CHARS_OFFSET 0x280u
#define IOSB_OFFSET 0x400u
#define HANDLE_OFFSET 0x440u
#define READ_BUFFER_OFFSET 0x480u
/* Outside the poisoned span on purpose: these two hold things the TEST writes and the
 * handler READS, so a poison sweep over them would be overwriting the input. */
#define BYTE_OFFSET_OFFSET 0x700u
#define WRITE_BUFFER_OFFSET 0x800u
#define POISON_FROM 0x200u
#define POISON_BYTES 0x400u
#define SENTINEL_BYTE 0x5Au

static kernel_guest_ptr scratch;
static uint32_t object_root;

/* --- the host side: a temporary directory, made and removed by this suite ------- */

/*
 * Beside the test binary, which is the build directory, rather than in the working
 * directory: the first version used a bare relative template, so a run from the repo root
 * (or any exit that skipped teardown) left `tsfp-hdd-test-*` in the source tree.
 * `mkdtemp` rewrites the template in place, so it has to be a mutable array.
 */
#define HOST_ROOT_TEMPLATE "tsfp-hdd-test-XXXXXX"
#define HOST_ROOT_PREFIX "tsfp-hdd-test-"
/* The helpers below build paths into 512-byte buffers, so the root must leave room. */
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
    const int written = snprintf(host_root, sizeof(host_root), "%s/%s", binary_directory,
                                 HOST_ROOT_TEMPLATE);
    if (written <= 0 || (size_t)written >= sizeof(host_root)) {
        printf("FATAL the scratch directory path does not fit\n");
        exit(EXIT_FAILURE);
    }
    if (mkdtemp(host_root) == NULL) {
        printf("FATAL could not create a scratch directory: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    host_root_ready = true;
    /* Registered AFTER the directory exists, and once. Every FATAL path below calls
     * exit(), which skips the per-test teardown; this is what removes the tree then. */
    if (!exit_handler_registered) {
        exit_handler_registered = true;
        (void)atexit(drop_host_root);
    }
}

/*
 * Remove a directory tree, depth first.
 *
 * Bounded by `depth` rather than trusted to terminate: this runs `unlinkat` over a tree
 * the tests have been planting symbolic links in, and a recursion with no bound is one
 * typo away from walking out of the scratch directory. AT_SYMLINK_NOFOLLOW semantics come
 * free from `unlinkat` without AT_REMOVEDIR, which unlinks a link rather than its target.
 */
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

/* Does `relative`, under the scratch directory, exist as a directory on the host? Asked
 * with the host's own syscalls rather than through the module under test, because a test
 * that verifies a create by asking the creator is not evidence. */
static bool host_dir_exists(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool host_file_exists(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool host_anything_exists(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    struct stat info;
    return lstat(path, &info) == 0;
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

/*
 * Read a file back with the HOST's own syscalls, not through the module under test.
 *
 * The whole point of the write tests: a write verified by asking the writer whether it
 * wrote is not evidence. Returns the byte count, or SIZE_MAX when the file is absent.
 */
static size_t host_read_file(const char *relative, void *out, size_t max)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    FILE *file = fopen(path, "rb");
    if (!file) {
        return (size_t)-1;
    }
    const size_t got = fread(out, 1u, max, file);
    (void)fclose(file);
    return got;
}

/*
 * Count the process's OWN descriptors that point at `relative`, and how many of them could
 * write to it.
 *
 * WHY THIS AND NOT A FINGERPRINT. A fingerprint proves nothing was written on THIS run. This
 * proves nothing CAN be: it walks `/proc/self/fd`, finds every descriptor the whole process
 * holds on the image, and asks the kernel each one's access mode. A later change that opened
 * the image `O_RDWR` and then happened not to write through it would pass every fingerprint
 * test in this file and fail this one.
 *
 * `*out_found` is handed back so the caller can assert the scan SAW the image first. A
 * "zero writable descriptors" result is also what a scan that found nothing at all returns,
 * and the two must not be confusable.
 */
static unsigned writable_descriptors_on(const char *relative, unsigned *out_found)
{
    *out_found = 0u;
    char relative_path[512];
    (void)snprintf(relative_path, sizeof(relative_path), "%s/%s", host_root, relative);
    char absolute[PATH_MAX];
    if (!realpath(relative_path, absolute)) {
        printf("FATAL could not resolve \"%s\": %s\n", relative_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        printf("FATAL could not read /proc/self/fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    unsigned writable = 0u;
    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            break;
        }
        if (entry->d_name[0] == '.') {
            continue;
        }
        char link[64];
        (void)snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name);
        char target[PATH_MAX];
        const ssize_t length = readlink(link, target, sizeof(target) - 1u);
        if (length <= 0) {
            continue;
        }
        target[length] = '\0';
        if (strcmp(target, absolute) != 0) {
            continue;
        }
        (*out_found)++;
        const int flags = fcntl((int)strtol(entry->d_name, NULL, 10), F_GETFL);
        if (flags >= 0 && (flags & O_ACCMODE) != O_RDONLY) {
            writable++;
        }
    }
    (void)closedir(dir);
    return writable;
}

static void host_make_fifo(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    if (mkfifo(path, 0600) != 0) {
        printf("FATAL could not make a FIFO at \"%s\": %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void host_remove_file(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    (void)unlink(path);
}

static void host_make_symlink(const char *target, const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    if (symlink(target, path) != 0) {
        printf("FATAL could not link \"%s\" -> \"%s\": %s\n", path, target,
               strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* --- the guest side: scratch memory and synthesised frames ---------------------- */

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

/* Everything the handler might write starts non-zero, so "untouched" is never confusable
 * with "written a zero". */
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
    object_root = 0u;
    kernel_hle_init();
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    guest_mem_reset();
    /* FOUR bindings. The exact count is deliberate: a dropped binding leaves a stub
     * returning 0, which every caller reads as STATUS_SUCCESS, so this assertion is what
     * stops the suite passing against nothing. */
    CHECK_EQ_U32(kernel_file_register(), 7u);
    /* kernel_io too, because the write tests drive ordinal 236 through the real dispatcher
     * rather than calling `kernel_file_write_backing` directly: the handler is where the
     * position, the staging copy and the transferred-versus-requested count live, and a
     * test that bypassed it would prove none of them. FIVE, not four -- 236 is the new
     * one, and if it were dropped every write below would hit a stub returning 0, which
     * reads as STATUS_SUCCESS. */
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
    poison();
    make_host_root();
}

static void teardown(void)
{
    /* Resetting the module BEFORE removing the tree, because the reset is what closes the
     * descriptors the module holds on things inside it. */
    kernel_file_reset();
    kernel_io_reset();
    kernel_object_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
    drop_host_root();
    /* Asked of the filesystem, not of `host_root_ready`: the flag is what drop_host_root
     * sets whether or not the removal worked. */
    CHECK(access(host_root, F_OK) != 0);
}

/*
 * Lay out an OBJECT_STRING for `path` at the offsets guest_structs.h derives: length at
 * +0x00 (16-bit), maximum_length at +0x02, buffer at +0x04. The characters are written
 * WITHOUT a trailing NUL, so a handler that stopped at one would read the poison byte.
 */
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

/* OBJECT_ATTRIBUTES: 12 bytes, no Length field. */
static kernel_guest_ptr build_attributes(kernel_guest_ptr name_string)
{
    const kernel_guest_ptr oa = at(OA_OFFSET);
    write_u32_at(oa + 0u, object_root);
    write_u32_at(oa + 4u, name_string);
    write_u32_at(oa + 8u, MEASURED_ATTRIBUTES);
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
    /* Clamped to exactly the slots built, so a read past the last argument FAILS rather
     * than returning whatever follows. */
    frame.stack_limit = at(FRAME_OFFSET) + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

/* The 9-argument NtCreateFile shape, in the order measured at 0x00380127. */
static uint32_t create_path(const char *path, uint32_t disposition, uint32_t options)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(name);
    const uint32_t args[9] = {at(HANDLE_OFFSET),
                              MEASURED_ACCESS,
                              oa,
                              at(IOSB_OFFSET),
                              0u,
                              MEASURED_FILE_ATTRIBUTES,
                              MEASURED_SHARE,
                              disposition,
                              options};
    return call_ordinal(ORD_NT_CREATE_FILE, args, 9u);
}

/*
 * The same 9-argument shape with the access mask, share and options the measured
 * TitleMeta.xbx create uses. A separate helper rather than a parameter on `create_path`
 * because the argument that matters is the ACCESS MASK and a boolean would hide which one
 * is in play.
 */
static uint32_t create_path_for_writing(const char *path, uint32_t disposition,
                                        uint32_t options)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(name);
    const uint32_t args[9] = {at(HANDLE_OFFSET),
                              MEASURED_WRITE_ACCESS,
                              oa,
                              at(IOSB_OFFSET),
                              0u,
                              MEASURED_WRITE_ATTRIBUTES,
                              MEASURED_WRITE_SHARE,
                              disposition,
                              options};
    return call_ordinal(ORD_NT_CREATE_FILE, args, 9u);
}

/* The 6-argument NtOpenFile shape, in the order measured at 0x00380D43. */
static uint32_t open_path(const char *path)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(name);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_ACCESS, oa, at(IOSB_OFFSET),
                              MEASURED_SHARE, MEASURED_OPEN_OPTIONS};
    return call_ordinal(ORD_NT_OPEN_FILE, args, 6u);
}

/*
 * The 8-argument NtWriteFile shape, in the order measured at 0x003810BF.
 *
 * `byte_offset` NULL reproduces that site exactly -- its arg7 is the `push ebx` at
 * 0x003810AA, which survives the intervening `ret 4` -- and a non-NULL one reproduces the
 * Win32 overlapped form at 0x0037CD44, which builds a 64-bit pair and passes its ADDRESS.
 * Both forms occur in this image, so both are driven here.
 */
static uint32_t write_file(uint32_t handle, const void *bytes, uint32_t length,
                           const uint64_t *byte_offset)
{
    const uint8_t *in = (const uint8_t *)bytes;
    for (uint32_t i = 0u; i < length; i++) {
        if (!kernel_guest_write_u8(at(WRITE_BUFFER_OFFSET) + i, in[i])) {
            printf("FATAL could not stage the write payload\n");
            exit(EXIT_FAILURE);
        }
    }
    uint32_t offset_argument = 0u;
    if (byte_offset) {
        write_u32_at(at(BYTE_OFFSET_OFFSET), (uint32_t)(*byte_offset & 0xFFFFFFFFu));
        write_u32_at(at(BYTE_OFFSET_OFFSET) + 4u, (uint32_t)(*byte_offset >> 32));
        offset_argument = at(BYTE_OFFSET_OFFSET);
    }
    const uint32_t args[8] = {handle,
                              0u, /* Event -- NULL at the measured site */
                              0u, /* ApcRoutine */
                              0u, /* ApcContext */
                              at(IOSB_OFFSET),
                              at(WRITE_BUFFER_OFFSET),
                              length,
                              offset_argument};
    return call_ordinal(ORD_NT_WRITE_FILE, args, 8u);
}

/* The 5-argument NtSetInformationFile shape, carrying a single 64-bit end of file. */
static uint32_t set_end_of_file(uint32_t handle, uint64_t end_of_file)
{
    write_u32_at(at(WRITE_BUFFER_OFFSET), (uint32_t)(end_of_file & 0xFFFFFFFFu));
    write_u32_at(at(WRITE_BUFFER_OFFSET) + 4u, (uint32_t)(end_of_file >> 32));
    const uint32_t args[5] = {handle, at(IOSB_OFFSET), at(WRITE_BUFFER_OFFSET), 8u,
                              FILE_CLASS_END_OF_FILE};
    return call_ordinal(ORD_NT_SET_INFORMATION_FILE, args, 5u);
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

static uint32_t issued_handle(void)
{
    return read_u32_at(at(HANDLE_OFFSET));
}

static bool handle_slot_untouched(void)
{
    const uint32_t expected = ((uint32_t)SENTINEL_BYTE) * 0x01010101u;
    return read_u32_at(at(HANDLE_OFFSET)) == expected;
}

static void mount_the_hdd(void)
{
    CHECK(kernel_file_mount_host_dir(HDD_DEVICE, host_root));
    CHECK_EQ_U32(kernel_file_volume_count(), 1u);
}

/* --- the virtual raw device (partition0) ------------------------------------- */

static void mount_the_device(void)
{
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE,
                                        DEVICE_CAPACITY));
}

/* NtOpenFile with the literals measured at 0x0037D887. */
static uint32_t open_device(const char *path)
{
    const kernel_guest_ptr name = build_name(path);
    const kernel_guest_ptr oa = build_attributes(name);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_DEVICE_ACCESS, oa,
                              at(IOSB_OFFSET),   MEASURED_SHARE,         MEASURED_DEVICE_OPTIONS};
    return call_ordinal(ORD_NT_OPEN_FILE, args, 6u);
}

static void fill_device_buffer(uint8_t value, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        if (!kernel_guest_write_u8(at(DEVICE_BUFFER_OFFSET) + i, value)) {
            printf("FATAL could not fill the device buffer\n");
            exit(EXIT_FAILURE);
        }
    }
}

static uint8_t device_buffer_byte(uint32_t index)
{
    uint8_t value = 0u;
    CHECK(kernel_guest_read_u8(at(DEVICE_BUFFER_OFFSET) + index, &value));
    return value;
}

/* NtReadFile with an explicit 64-bit ByteOffset, the form measured at 0x0037D8B6. */
static uint32_t read_device(uint32_t handle, uint64_t offset, uint32_t length)
{
    write_u32_at(at(BYTE_OFFSET_OFFSET), (uint32_t)(offset & 0xFFFFFFFFu));
    write_u32_at(at(BYTE_OFFSET_OFFSET) + 4u, (uint32_t)(offset >> 32));
    const uint32_t args[8] = {handle, 0u, 0u, 0u, at(IOSB_OFFSET), at(DEVICE_BUFFER_OFFSET),
                              length, at(BYTE_OFFSET_OFFSET)};
    return call_ordinal(ORD_NT_READ_FILE, args, 8u);
}

static int64_t host_file_size(const char *relative)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    struct stat info;
    return stat(path, &info) == 0 ? (int64_t)info.st_size : -1;
}

/* --- a synthetic XDVDFS image, so the read-only claim can be tested with no disc -- */

#define SYNTH_IMAGE_SECTORS 36u
#define SYNTH_IMAGE_BYTES (SYNTH_IMAGE_SECTORS * XDVDFS_SECTOR_SIZE)
#define SYNTH_DESCRIPTOR_SECTOR 32u
#define SYNTH_ROOT_SECTOR 33u
#define SYNTH_FILE_SECTOR 34u

static void store_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/*
 * Write a minimal but genuinely valid Xbox disc image into the scratch directory.
 *
 * 70 KB of bytes this file lays out itself, which is what lets the "the disc is never
 * written" claim be tested on a clean checkout with no image of the user's anywhere. Both
 * copies of the magic, because `xdvdfs_open` checks both; one real root entry with nil
 * links encoded as 0, which is the encoding this format actually uses.
 */
static void write_synthetic_disc(const char *relative)
{
    static uint8_t image[SYNTH_IMAGE_BYTES];
    memset(image, 0, sizeof(image));

    uint8_t *descriptor = &image[SYNTH_DESCRIPTOR_SECTOR * XDVDFS_SECTOR_SIZE];
    memcpy(&descriptor[0], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    memcpy(&descriptor[0x7EC], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    store_u32(&descriptor[0x14], SYNTH_ROOT_SECTOR);
    store_u32(&descriptor[0x18], 20u);

    uint8_t *root = &image[SYNTH_ROOT_SECTOR * XDVDFS_SECTOR_SIZE];
    memset(root, 0xFF, 20u);
    root[0] = 0u; /* left link, nil as 0 */
    root[1] = 0u;
    root[2] = 0u; /* right link, nil as 0 */
    root[3] = 0u;
    store_u32(&root[4], SYNTH_FILE_SECTOR);
    store_u32(&root[8], 5u);
    root[0x0C] = 0x20u; /* a file, which is what this image's files carry */
    root[0x0D] = 5u;
    memcpy(&root[0x0E], "HELLO", 5u);

    memcpy(&image[SYNTH_FILE_SECTOR * XDVDFS_SECTOR_SIZE], "WORLD", 5u);
    host_write_file(relative, image, sizeof(image));
}

/* A cheap content fingerprint. Not a cryptographic hash and does not need to be: it has
 * to notice a change, not resist one being engineered. */
static uint64_t fingerprint(const char *relative, uint64_t *out_bytes)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, relative);
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("FATAL could not fingerprint \"%s\"\n", path);
        exit(EXIT_FAILURE);
    }
    uint64_t hash = 1469598103934665603u;
    uint64_t total = 0u;
    int byte = 0;
    while ((byte = fgetc(file)) != EOF) {
        hash ^= (uint64_t)(unsigned char)byte;
        hash *= 1099511628211u;
        total++;
    }
    (void)fclose(file);
    *out_bytes = total;
    return hash;
}

/* ------------------------------------------------------------------------- */

/*
 * WITHOUT `--hdd` NOTHING CHANGES. The create fails with exactly the status it always
 * failed with, writes no handle, consumes no object-table entry, and creates nothing.
 *
 * This is the test that stops the whole feature from becoming a silent default. It is
 * first in the file for that reason.
 *
 * MUTATION: drop the `volume != NULL` guard on the create path in `hle_nt_create_file`,
 * or make `kernel_file_mount_host_dir` run without being asked, and this fails.
 */
static void test_without_a_mount_the_create_fails_exactly_as_before(void)
{
    setup();
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    const uint32_t status =
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS);
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);
    CHECK_EQ_U32(kernel_file_host_opened_count(), 0u);
    /* And nothing landed in the scratch directory, which is the host-side half of the
     * same claim: a refusal that quietly created something somewhere would satisfy every
     * assertion above. */
    CHECK(!host_anything_exists("TDATA"));
    CHECK(captured_contains("NOTHING WAS CREATED"));
    teardown();
}

/*
 * The mount itself refuses everything it should, and each refusal names its reason.
 *
 * The non-directory case is the STRUCTURAL reason an operator cannot hand their disc image
 * to this function: it fails at the `O_DIRECTORY` open before anything is recorded.
 *
 * MUTATION: drop `O_DIRECTORY` from the mount's open, or accept a prefix ending in a
 * separator, and this fails.
 */
static void test_the_mount_refuses_what_it_cannot_back(void)
{
    setup();
    CHECK(!kernel_file_mount_host_dir(NULL, host_root));
    CHECK(!kernel_file_mount_host_dir(HDD_DEVICE, NULL));
    CHECK(!kernel_file_mount_host_dir(HDD_DEVICE, "tsfp-no-such-directory-here"));

    /* A regular file. This is the disc-image shape. */
    host_write_file("not-a-directory.iso", "x", 1u);
    char file_path[512];
    (void)snprintf(file_path, sizeof(file_path), "%s/not-a-directory.iso", host_root);
    CHECK(!kernel_file_mount_host_dir(HDD_DEVICE, file_path));
    CHECK(captured_contains("must already exist and be a DIRECTORY"));

    /* A prefix ending in a separator would match the bare device and nothing under it,
     * which is a half-working mount. */
    CHECK(!kernel_file_mount_host_dir(HDD_DEVICE "\\", host_root));
    CHECK(captured_contains("must NOT end in a separator"));

    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    teardown();
}

/*
 * THE DISC MOUNT REFUSES IT TOO, and until this test it did not.
 *
 * The trailing-separator rule was written for the writable mount while
 * `kernel_file_mount_disc` had the identical latent trap sitting unguarded -- a
 * `--disc-device` ending in a separator would mount the bare device and resolve nothing
 * beneath it, so the title's reads would fail while the mount reported success. The
 * check is now one shared helper, and this asserts BOTH callers reach it: a rule with
 * two copies is a rule that drifts, and a rule with one copy and one caller tested is
 * the same thing with better odds.
 *
 * MUTATION: remove the call from either mount function and the corresponding half here
 * fails.
 */
static void test_both_mounts_refuse_a_trailing_separator(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);

    /* The same image mounts fine WITHOUT the trailing separator, so the refusal below is
     * about the prefix and not about the image. Asserting the positive first matters:
     * without it, a mount that failed for any reason at all would satisfy the negative. */
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(kernel_file_volume_count(), 1u);
    kernel_file_unmount_all();

    CHECK(!kernel_file_mount_disc(DISC_DEVICE "\\", image_path));
    CHECK(captured_contains("must NOT end in a separator"));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);

    /* And a forward slash, because the guest mixes both separators and so may an
     * operator. */
    CHECK(!kernel_file_mount_disc(DISC_DEVICE "/", image_path));
    CHECK(!kernel_file_mount_host_dir(HDD_DEVICE "/", host_root));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    teardown();
}

/*
 * FILE_OPEN_IF | FILE_DIRECTORY_FILE CREATES THE DIRECTORY -- the exact call that was
 * failing at guest 0x00381321, now succeeding, with the directory really on the host.
 *
 * Checked against the HOST's own `stat` and not against this module's accessors, because a
 * create verified by asking the creator is not evidence.
 *
 * MUTATION: make the create a no-op that reports success, or report
 * FILE_INFORMATION_OPENED instead of CREATED, and this fails.
 */
static void test_open_if_with_the_directory_bit_creates_a_real_directory(void)
{
    setup();
    mount_the_hdd();
    CHECK(!host_anything_exists("TDATA"));

    const uint32_t status =
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_CREATED);
    CHECK(issued_handle() != 0u);
    CHECK(issued_handle() != ((uint32_t)SENTINEL_BYTE) * 0x01010101u);
    CHECK_EQ_U32(kernel_file_created_count(), 1u);
    CHECK(host_dir_exists("TDATA"));
    CHECK(captured_contains("CREATED directory"));
    teardown();
}

/*
 * A SECOND FILE_OPEN_IF OPENS THE EXISTING DIRECTORY rather than failing, and says
 * "opened" rather than "created".
 *
 * This is the half that makes the fix a fix. The measured boot creates `TDATA`, relinks,
 * and comes back; if the second pass failed, the title would take the same
 * XLaunchNewImage reboot for a new reason and the symptom would be unchanged.
 *
 * MUTATION: treat FILE_OPEN_IF as FILE_CREATE -- the `disposition == FILE_CREATE`
 * collision test widened to include FILE_OPEN_IF -- and this fails.
 */
static void test_a_second_open_if_opens_the_existing_directory(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_CREATED);
    CHECK_EQ_U32(kernel_file_created_count(), 1u);

    const uint32_t again =
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS);
    CHECK_EQ_U32(again, STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_OPENED);
    /* Still ONE creation, not two. A counter that counted the open would make a
     * repeatedly-probing title look like it was filling the disk. */
    CHECK_EQ_U32(kernel_file_created_count(), 1u);
    CHECK(host_dir_exists("TDATA"));
    teardown();
}

/*
 * FILE_CREATE on a name that already exists is a COLLISION, not a silent replacement.
 *
 * On a volume holding save data, reporting success here would mean overwriting a player's
 * file and telling them it was new.
 *
 * MUTATION: let FILE_CREATE fall through to the ordinary open, and this fails.
 */
static void test_file_create_on_an_existing_name_is_a_collision(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("TDATA");

    const uint32_t status =
        create_path(GUEST_TDATA, DISPOSITION_FILE_CREATE, MEASURED_CREATE_OPTIONS);
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION);
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION);
    CHECK_EQ_U32(iosb_information(), INFORMATION_EXISTS);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);
    CHECK(host_dir_exists("TDATA"));
    teardown();
}

/*
 * The measured TWO-STEP sequence: `TDATA`, then `TDATA\45410066`. And only the leaf is
 * created -- a nested path whose parent is absent is refused, which is what NT does.
 *
 * MUTATION: create intermediate directories as well, and the second half of this fails.
 */
static void test_the_measured_two_step_create_works_and_parents_are_not_invented(void)
{
    setup();
    mount_the_hdd();

    /* The nested name BEFORE its parent exists. Refused, and nothing is created. */
    CHECK_EQ_U32(
        create_path(GUEST_TDATA_TITLE, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(!host_anything_exists("TDATA"));
    CHECK_EQ_U32(kernel_file_created_count(), 0u);

    /* Now in the order the title does it. */
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    CHECK_EQ_U32(
        create_path(GUEST_TDATA_TITLE, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_CREATED);
    CHECK_EQ_U32(kernel_file_created_count(), 2u);
    CHECK(host_dir_exists("TDATA"));
    CHECK(host_dir_exists("TDATA/45410066"));

    /* And UDATA, the other half of the pair the XDK sets up. */
    CHECK_EQ_U32(create_path(HDD_DEVICE "\\UDATA", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(create_path(HDD_DEVICE "\\UDATA\\45410066", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_created_count(), 4u);
    CHECK(host_dir_exists("UDATA/45410066"));
    teardown();
}

/*
 * A GUEST PATH CANNOT CLIMB OUT, and the proof is on the host side as well as in the
 * status: nothing appears next to the scratch directory.
 *
 * Several shapes, because one of them passing is not the same as the rule holding: `..` in
 * the middle, `..` as the leaf, a `.` component, and a run of them. The guest is untrusted
 * input.
 *
 * MUTATION: remove the `component_is_refused` check, or make it return false, and this
 * fails.
 */
static void test_a_path_that_tries_to_climb_out_is_refused(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("TDATA");

    static const char *const escapes[] = {
        HDD_DEVICE "\\..\\escaped",
        HDD_DEVICE "\\TDATA\\..\\..\\escaped",
        HDD_DEVICE "\\..",
        HDD_DEVICE "\\.\\escaped",
        HDD_DEVICE "/../escaped",
        HDD_DEVICE "\\TDATA\\..",
    };
    unsigned refusals = 0u;
    for (size_t i = 0u; i < sizeof(escapes) / sizeof(escapes[0]); i++) {
        const uint32_t created =
            create_path(escapes[i], DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS);
        CHECK_EQ_U32(created, KERNEL_FILE_STATUS_ACCESS_DENIED);
        const uint32_t opened = open_path(escapes[i]);
        CHECK_EQ_U32(opened, KERNEL_FILE_STATUS_ACCESS_DENIED);
        refusals += 2u;
    }
    CHECK_EQ_U32(kernel_file_escape_refused_count(), refusals);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("REFUSED. A guest path may not leave the directory"));

    /* The host-side half. `escaped` must not exist beside the scratch directory, where a
     * single successful `..` would have put it. That is the binary's directory, NOT the
     * working directory: the old check looked in the cwd and so also failed whenever a stale
     * `escaped` sat in the repo root. Removed afterwards so a failing run (a mutant that lets
     * the climb through) cannot leave it behind either. */
    char escaped_path[sizeof(binary_directory) + 16];
    (void)snprintf(escaped_path, sizeof(escaped_path), "%s/escaped", binary_directory);
    struct stat info;
    CHECK(lstat(escaped_path, &info) != 0);
    (void)rmdir(escaped_path);
    (void)unlink(escaped_path);
    teardown();
}

/*
 * A HOST SYMBOLIC LINK PLANTED INSIDE THE BACKING DIRECTORY IS REFUSED, NOT FOLLOWED.
 *
 * THE EASY HALF TO FORGET, and the one a `realpath` check would get wrong: refusing `..`
 * alone leaves a link as a perfectly ordinary-looking component that resolves anywhere on
 * the host. Both positions are covered -- a link traversed on the way through, and a link
 * as the final component -- because `openat` and `fstatat` refuse them in different places.
 *
 * MUTATION: drop `O_NOFOLLOW` from the walk's `openat`, or drop `AT_SYMLINK_NOFOLLOW` from
 * the leaf's `fstatat`, and this fails.
 */
static void test_a_host_symbolic_link_is_refused_rather_than_followed(void)
{
    setup();
    mount_the_hdd();
    /* A link whose target is the scratch directory's own parent. Following it would put
     * every path under it outside the backing directory. */
    host_make_symlink("..", "way-out");
    host_write_file("real.bin", "abc", 3u);
    host_make_symlink("real.bin", "alias.bin");

    /* Traversed on the way through. */
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\way-out\\anything"),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(create_path(HDD_DEVICE "\\way-out\\anything", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(captured_contains("host SYMBOLIC LINK"));

    /* As the final component. A link to a file that really is inside the directory, so
     * the only thing being tested is that the link itself is refused. */
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\alias.bin"), KERNEL_FILE_STATUS_ACCESS_DENIED);

    /* EXACTLY three, not "at least": one per refused call. An inexact bound would let a
     * missing refusal hide behind a double count, and both are defects. */
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 3u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/*
 * THE DISC IS NEVER WRITTEN, and this is the test that makes that structural rather than
 * asserted. A real XDVDFS volume is mounted, a creating disposition is aimed at it, and
 * the image file is FINGERPRINTED before and after.
 *
 * The status matters as much as the fingerprint: 0xC0000034 is the ordinary "not on this
 * disc" answer, and anything else would mean the create path was entered and then bailed
 * out, which is a guard rather than a design.
 *
 * MUTATION: drop the `backing == KERNEL_FILE_BACKING_HOST_DIR` test from the create path
 * in `hle_nt_create_file`, and this fails.
 */
static void test_a_create_on_a_disc_volume_changes_not_one_byte(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    uint64_t before_bytes = 0u;
    const uint64_t before = fingerprint("synthetic.iso", &before_bytes);

    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(kernel_file_volume_count(), 1u);

    /* The disc really is readable, so this is a volume that works and not one that failed
     * to mount. HELLO is the one entry the synthetic root carries. */
    CHECK_EQ_U32(open_path(DISC_DEVICE "\\HELLO"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_disc_opened_count(), 1u);

    CHECK_EQ_U32(create_path(DISC_DEVICE "\\TDATA", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(create_path(DISC_DEVICE "\\TDATA", DISPOSITION_FILE_CREATE,
                             MEASURED_CREATE_OPTIONS),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(create_path(DISC_DEVICE "\\save.dat", DISPOSITION_FILE_OPEN_IF,
                             OPTION_NON_DIRECTORY_FILE),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);

    /* Unmounted first so the reader's descriptor is closed before the file is read back,
     * which keeps the comparison about content and not about buffering. */
    kernel_file_unmount_all();
    uint64_t after_bytes = 0u;
    const uint64_t after = fingerprint("synthetic.iso", &after_bytes);
    CHECK(before == after);
    CHECK(before_bytes == after_bytes);
    CHECK_EQ_U32((uint32_t)after_bytes, (uint32_t)SYNTH_IMAGE_BYTES);
    teardown();
}

/*
 * A FILE ALREADY IN THE BACKING DIRECTORY OPENS WITH ITS REAL SIZE AND SERVES ITS REAL
 * BYTES.
 *
 * Without this the writable volume would be write-only in the one sense that matters: a
 * title could create its save file and never read it back. The content is asserted, not
 * just the length, because a read that returned the right count of zeros would satisfy a
 * length check.
 *
 * MUTATION: let a HOST_DIR read fall into the fabricated-empty arm, which transfers 0
 * bytes and reports success, and this fails.
 */
static void test_a_real_file_opens_with_its_real_size_and_serves_real_bytes(void)
{
    setup();
    mount_the_hdd();
    static const char payload[] = "TSFP-SAVE-DATA";
    const size_t payload_length = sizeof(payload) - 1u;
    host_write_file("save.dat", payload, payload_length);

    CHECK_EQ_U32(open_path(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_file_host_opened_count(), 1u);

    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(!info.is_directory);
    CHECK_EQ_U32((uint32_t)info.size, (uint32_t)payload_length);

    uint8_t buffer[64];
    memset(buffer, 0, sizeof(buffer));
    uint32_t read = 0u;
    CHECK(kernel_file_read_backing(handle, 0u, buffer, (uint32_t)sizeof(buffer), &read));
    CHECK_EQ_U32(read, (uint32_t)payload_length);
    CHECK(memcmp(buffer, payload, payload_length) == 0);
    CHECK_EQ_U32((uint32_t)kernel_file_host_bytes_read(), (uint32_t)payload_length);

    /* A read at an offset inside the file, so the offset is proved to reach `pread`
     * rather than being ignored. */
    memset(buffer, 0, sizeof(buffer));
    CHECK(kernel_file_read_backing(handle, 5u, buffer, 4u, &read));
    CHECK_EQ_U32(read, 4u);
    CHECK(memcmp(buffer, payload + 5, 4u) == 0);
    teardown();
}

/*
 * READING A DIRECTORY IS REFUSED, not answered with zero bytes.
 *
 * Zero bytes reads as end of file, so a title told its save directory is an empty file
 * would conclude the data is gone. Listing one is ordinal 207 NtQueryDirectoryFile, which
 * has no handler in this tree.
 *
 * MUTATION: return true with 0 bytes for a directory, and this fails.
 */
static void test_reading_a_directory_is_refused_rather_than_answered_empty(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    uint8_t buffer[16];
    memset(buffer, 0xA5, sizeof(buffer));
    uint32_t read = 0xFFFFFFFFu;
    CHECK(!kernel_file_read_backing(handle, 0u, buffer, (uint32_t)sizeof(buffer), &read));
    CHECK_EQ_U32(read, 0u);
    CHECK(captured_contains("which is a DIRECTORY -- REFUSED"));
    teardown();
}

/*
 * MATCHING IS CASE-INSENSITIVE THROUGH TO THE HOST FILESYSTEM, which a Linux host is not.
 *
 * The Xbox's own filesystem is FATX: case-insensitive and case-preserving. This binary
 * spells its own names two ways -- both `partition1` and `Partition1` -- so a directory
 * created as `TDATA` has to be findable as `tdata`, and the name on disk has to stay as
 * the guest spelled it.
 *
 * MUTATION: delete the case-insensitive directory scan and pass the component through
 * verbatim, and this fails.
 */
static void test_a_created_name_is_found_again_under_any_casing(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    /* CASE-PRESERVING: the guest's own spelling is what landed on the host. */
    CHECK(host_dir_exists("TDATA"));
    CHECK(!host_anything_exists("tdata"));

    /* CASE-INSENSITIVE: a differently-spelled open finds it, and a second OPEN_IF does
     * not create a duplicate beside it. */
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\tdata"), STATUS_SUCCESS);
    CHECK_EQ_U32(create_path(HDD_DEVICE "\\TData", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_OPENED);
    CHECK_EQ_U32(kernel_file_created_count(), 1u);
    CHECK(!host_anything_exists("TData"));

    /* And through an intermediate component, which is a different code path from the
     * leaf. */
    CHECK_EQ_U32(create_path(GUEST_TDATA_TITLE, DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\tdata\\45410066"), STATUS_SUCCESS);
    teardown();
}

/*
 * The volume ROOT itself opens, which is the title's very first hard-disk call.
 *
 * MEASURED at 0x00380D43: NtOpenFile on `\Device\Harddisk0\partition1\` with nothing after
 * the separator. Bare, with no volume behind it, that open is what fails at call 14 and
 * sends the title to XLaunchNewImage from a second direction.
 *
 * MUTATION: treat an empty remainder as a missing name, and this fails.
 */
static void test_the_volume_root_itself_opens(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\"), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(info.is_directory);
    /* And through the bare device name with no separator at all, which `volume_for_locked`
     * also admits. */
    CHECK_EQ_U32(open_path(HDD_DEVICE), STATUS_SUCCESS);
    teardown();
}

/*
 * A NAME THE WRITABLE VOLUME DOES NOT HOLD IS AN HONEST ABSENCE, even under the
 * fabricate-empty policy.
 *
 * Same rule as a mounted disc, and for the same reason: handing back an empty file would
 * invent content on a volume whose real contents we can list. Worth its own test because
 * `resolve_locked`'s `declared` line counts ANY volume as declared, so a host-directory
 * arm that forgot to return early would fabricate silently and not even count it.
 *
 * MUTATION: let the HOST_DIR arm fall through to the policy instead of returning, and this
 * fails.
 */
static void test_a_missing_name_on_a_writable_volume_is_not_fabricated(void)
{
    setup();
    mount_the_hdd();
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);

    CHECK_EQ_U32(open_path(HDD_DEVICE "\\absent.dat"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(kernel_object_live_count(), 0u);

    /* A name with no volume behind it at all still fabricates, so the policy is proved
     * still to work rather than merely proved not to fire. */
    CHECK_EQ_U32(open_path("\\Device\\Harddisk0\\partition2\\absent.dat"),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    teardown();
}

/*
 * A non-creating disposition never creates, and contradictory options are refused.
 *
 * FILE_OPEN is the shape that must stay a pure open: if it created, every probe for an
 * absent file would leave a file behind and the next probe would find it.
 *
 * MUTATION: add FILE_OPEN to `disposition_creates`, or pick one of the two contradictory
 * option bits instead of refusing, and this fails.
 */
static void test_a_non_creating_disposition_creates_nothing(void)
{
    setup();
    mount_the_hdd();

    CHECK_EQ_U32(create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN, MEASURED_CREATE_OPTIONS),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(!host_anything_exists("TDATA"));
    CHECK_EQ_U32(kernel_file_created_count(), 0u);

    /* Both option bits at once contradict, and NT refuses it. */
    CHECK_EQ_U32(create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF,
                             OPTION_DIRECTORY_FILE | OPTION_NON_DIRECTORY_FILE),
                 STATUS_INVALID_PARAMETER);
    CHECK(!host_anything_exists("TDATA"));
    CHECK(captured_contains("those contradict"));
    teardown();
}

/*
 * A create with NEITHER option bit makes a FILE, which is NT's default, and it really is a
 * file on the host rather than a directory.
 *
 * MUTATION: default to a directory, and this fails.
 */
static void test_a_create_with_no_option_bit_makes_a_file(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path(HDD_DEVICE "\\save.dat", DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_CREATED);
    CHECK(host_file_exists("save.dat"));
    CHECK(!host_dir_exists("save.dat"));
    CHECK_EQ_U32(kernel_file_created_count(), 1u);

    /* And it is a zero-length file, announced as such rather than as a fabrication: it is
     * real, it is just empty. MEASURED_ACCESS is 0x00100001, which carries no write bit, so
     * the descriptor is read-only and the log says which -- a create for writing says the
     * opposite, and `test_a_create_for_writing_...` asserts that half. */
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(issued_handle(), &info));
    CHECK_EQ_U32((uint32_t)info.size, 0u);
    CHECK(!info.writable);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK(captured_contains("opened READ-ONLY because the create did not ask for write"));
    teardown();
}

/* ================ the write path: ordinal 236 and what it may touch ============== */

/*
 * THE MEASURED SHAPE, END TO END: the create the boot performs, then the write the boot is
 * one kernel call away from performing, then the bytes read back WITH THE HOST'S OWN
 * SYSCALLS.
 *
 * Why this is the headline test. The file this reproduces,
 * `UDATA\45410066\TitleMeta.xbx`, is created by the real boot and is ZERO BYTES, and the
 * reason it is zero is that nothing could write. Everything here is taken from the trace
 * rather than chosen: access 0x40100000, share 1, options 0x22, disposition 3, and a write
 * with ByteOffset NULL.
 *
 * MEASURED: the real boot now makes this write (call 125, from 0x003810C5) and TitleMeta.xbx
 * ends up 152 bytes. This suite stays the place the write path's REFUSALS are proved, since
 * the boot only ever exercises the success path.
 *
 * MUTATION: open the created file O_RDONLY regardless of access, and the write is refused.
 * MUTATION: report the REQUESTED count instead of the transferred one, and the host
 * readback below disagrees with the IO_STATUS_BLOCK.
 */
static void test_the_measured_create_and_write_put_real_bytes_in_a_real_file(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("UDATA");
    host_make_dir("UDATA/45410066");

    CHECK_EQ_U32(create_path_for_writing(GUEST_UDATA_TITLE "\\TitleMeta.xbx",
                                         DISPOSITION_FILE_OPEN_IF, MEASURED_WRITE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), INFORMATION_CREATED);
    const uint32_t handle = issued_handle();
    CHECK(handle != 0u);

    /* The descriptor is writable BECAUSE the create asked, and the log says which. */
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(info.writable);
    CHECK_EQ_U32((uint32_t)info.size, 0u);
    CHECK(captured_contains("opened FOR WRITING"));

    static const char payload[] = "TSFP-TITLE-META-BLOCK";
    const uint32_t payload_length = (uint32_t)(sizeof(payload) - 1u);
    CHECK_EQ_U32(write_file(handle, payload, payload_length, NULL), STATUS_SUCCESS);

    /* The transferred count, which must be the real one. */
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), payload_length);
    CHECK_EQ_U32(kernel_io_write_count(), 1u);
    CHECK_EQ_U64(kernel_io_bytes_written(), (uint64_t)payload_length);
    CHECK_EQ_U32(kernel_io_write_refused_count(), 0u);
    CHECK_EQ_U64(kernel_file_host_bytes_written(), (uint64_t)payload_length);

    /* THE FILE ON THE HOST, read with fopen rather than through the module that wrote it.
     * A length check alone would be satisfied by the right number of zeros, so the content
     * is compared too. */
    char back[64];
    memset(back, 0, sizeof(back));
    const size_t got = host_read_file("UDATA/45410066/TitleMeta.xbx", back, sizeof(back));
    CHECK_EQ_U32((uint32_t)got, payload_length);
    CHECK(memcmp(back, payload, payload_length) == 0);

    /* The recorded size grew with it, which is what the guest's own "is this file empty"
     * test at 0x00381084 reads back through NtQueryInformationFile class 0x22. A stale
     * zero there would send a second pass down the nothing-saved-yet arm. */
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32((uint32_t)info.size, payload_length);
    /* And the cursor advanced, because ByteOffset was NULL. */
    CHECK_EQ_U32((uint32_t)info.offset, payload_length);
    teardown();
}

/*
 * NO DESCRIPTOR THIS PROCESS HOLDS ON A DISC IMAGE IS WRITABLE, which is a stronger claim
 * than "the image did not change".
 *
 * THE PROPERTY THE BRIEF ASKED FOR, STATED THE WAY IT CAN FAIL. "No code path may open the
 * image for writing" is a claim about every open in the tree, and a before-and-after
 * fingerprint cannot test it: an `O_RDWR` image descriptor that nothing happened to write
 * through passes every other test in this file. This asks the kernel instead, for every
 * descriptor the process holds, while a disc volume is mounted AND a writable volume is
 * mounted beside it -- which is the configuration a real `--hdd --disc` run is in.
 *
 * MUTATION: open the image O_RDWR in `xdvdfs_open`, and this is the only test that fails.
 */
static void test_no_descriptor_on_a_disc_image_is_writable(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);

    /* Both volumes at once: a writable host directory and a read-only disc. */
    CHECK(kernel_file_mount_host_dir(HDD_DEVICE, host_root));
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(kernel_file_volume_count(), 2u);

    /* Open and read the disc so the descriptor is genuinely in use, then open and write a
     * file on the writable volume, so this is not a test of an idle process. */
    CHECK_EQ_U32(open_path(DISC_DEVICE "\\HELLO"), STATUS_SUCCESS);
    uint8_t probe[8];
    uint32_t read = 0u;
    CHECK(kernel_file_read_backing(issued_handle(), 0u, probe, 5u, &read));
    CHECK_EQ_U32(read, 5u);
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat",
                                         DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(write_file(issued_handle(), "BYTES", 5u, NULL), STATUS_SUCCESS);

    unsigned found = 0u;
    const unsigned writable = writable_descriptors_on("synthetic.iso", &found);
    /* THE SCAN MUST HAVE SEEN IT. Without this, a scan that matched nothing at all would
     * report zero writable descriptors and read as a pass. */
    CHECK(found >= 1u);
    CHECK_EQ_U32(writable, 0u);

    /* And the writable volume's own file IS writable, which proves the scan can tell the
     * difference rather than always reporting zero. */
    unsigned save_found = 0u;
    const unsigned save_writable = writable_descriptors_on("save.dat", &save_found);
    CHECK(save_found >= 1u);
    CHECK(save_writable >= 1u);
    teardown();
}

/*
 * A WRITE LARGER THAN THE STAGING BUFFER LANDS INTACT, in order, with the right count.
 *
 * The handler copies guest memory through a 16 KiB host staging buffer, and the largest
 * write this boot makes is 10240 bytes, so the REAL boot never crosses a chunk boundary.
 * That leaves `buffer + total` and `offset + total` advancing across iterations untested by
 * anything but this. 40000 bytes is three chunks with an odd tail, filled with a pattern
 * that depends on the position so a chunk written twice or in the wrong place cannot match.
 *
 * MUTATION: stop advancing the source (`buffer + total`) or the target (`offset + total`)
 * between chunks, and the host readback differs.
 */
static void test_a_write_larger_than_the_staging_buffer_lands_intact(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\big.dat", DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    enum { BIG = 40000 };
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x10000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr big = guest_region_alloc(&request, &status);
    CHECK(big != 0u);
    static uint8_t expected[BIG];
    bool staged = true;
    for (uint32_t i = 0u; i < BIG; i++) {
        expected[i] = (uint8_t)((i * 31u + (i >> 8)) & 0xFFu);
        staged = staged && kernel_guest_write_u8(big + i, expected[i]);
    }
    CHECK(staged);
    const uint32_t args[8] = {handle, 0u, 0u, 0u, at(IOSB_OFFSET), big, (uint32_t)BIG, 0u};
    CHECK_EQ_U32(call_ordinal(ORD_NT_WRITE_FILE, args, 8u), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), (uint32_t)BIG);
    CHECK_EQ_U64(kernel_io_bytes_written(), (uint64_t)BIG);

    static uint8_t back[BIG + 16];
    CHECK_EQ_U32((uint32_t)host_read_file("big.dat", back, sizeof(back)), (uint32_t)BIG);
    CHECK(memcmp(back, expected, BIG) == 0);
    teardown();
}

/*
 * A SECOND IMPLICIT WRITE CONTINUES WHERE THE FIRST STOPPED.
 *
 * THE FAILURE THIS CATCHES IS IN BOTH DIRECTIONS. A handler that ignored the position
 * entirely would put the second write back at offset 0 and destroy the first; one that
 * always appended regardless of the recorded position would be right here and wrong the
 * moment ordinal 226 or an explicit ByteOffset moves the cursor. Only reading the handle's
 * own cursor and advancing it satisfies both this test and the explicit-offset one below.
 *
 * MUTATION: pass 0 instead of the handle's offset, or stop advancing it, and this fails.
 */
static void test_two_implicit_writes_continue_rather_than_restart(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat",
                                         DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    CHECK_EQ_U32(write_file(handle, "FIRST", 5u, NULL), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 5u);
    CHECK_EQ_U32(write_file(handle, "SECOND", 6u, NULL), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 6u);

    char back[32];
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 11u);
    CHECK(memcmp(back, "FIRSTSECOND", 11u) == 0);

    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32((uint32_t)info.offset, 11u);
    CHECK_EQ_U32((uint32_t)info.size, 11u);
    teardown();
}

/*
 * AN EXPLICIT ByteOffset WRITES THERE AND LEAVES THE CURSOR ALONE.
 *
 * The mirror of the read path's rule, and the half that makes the position honoured rather
 * than merely tracked: a positioned write is not a seek.
 */
static void test_an_explicit_byte_offset_writes_there_and_does_not_move_the_cursor(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat",
                                         DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    CHECK_EQ_U32(write_file(handle, "AAAA", 4u, NULL), STATUS_SUCCESS);
    const uint64_t at_one = 1u;
    CHECK_EQ_U32(write_file(handle, "ZZ", 2u, &at_one), STATUS_SUCCESS);

    char back[32];
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 4u);
    CHECK(memcmp(back, "AZZA", 4u) == 0);

    /* Still 4: the positioned write did not advance it. */
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32((uint32_t)info.offset, 4u);
    teardown();
}

/*
 * A WRITE AIMED AT A DISC-BACKED HANDLE IS REFUSED, AND THE IMAGE CHANGES NOT ONE BYTE.
 *
 * The companion to `test_a_create_on_a_disc_volume_changes_not_one_byte`, and the brief
 * asked whether writes need the same test. They do: a create and a write reach the
 * filesystem through different functions, so one of them being safe says nothing about the
 * other. Fingerprinted before and after, like the create test.
 *
 * MUTATION: delete the backing check in `writable_entry_locked`, and this fails -- by
 * refusal count if nothing else, because a disc entry carries no host descriptor and the
 * write would fail on EBADF with the wrong diagnosis.
 */
static void test_a_write_to_a_disc_backed_handle_changes_not_one_byte(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    uint64_t before_bytes = 0u;
    const uint64_t before = fingerprint("synthetic.iso", &before_bytes);

    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(open_path(DISC_DEVICE "\\HELLO"), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK(handle != 0u);

    CHECK_EQ_U32(write_file(handle, "PWNED", 5u, NULL),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_ACCESS_DENIED);
    /* ZERO, not the requested 5. A refused write that reported 5 is exactly the shape of a
     * silent fabrication. */
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK_EQ_U32(kernel_io_write_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK_EQ_U32(kernel_io_write_refused_count(), 1u);
    CHECK_EQ_U32(kernel_file_write_refused_count(), 1u);
    CHECK_EQ_U64(kernel_file_host_bytes_written(), 0u);
    CHECK(captured_contains("READ-ONLY disc image"));

    /* An explicit end-of-file set on the same handle, which is the other way to mutate a
     * file, and it is refused by exactly the same gate. */
    CHECK_EQ_U32(set_end_of_file(handle, 0u), KERNEL_FILE_STATUS_ACCESS_DENIED);

    kernel_file_unmount_all();
    uint64_t after_bytes = 0u;
    const uint64_t after = fingerprint("synthetic.iso", &after_bytes);
    CHECK(before == after);
    CHECK(before_bytes == after_bytes);
    teardown();
}

/*
 * WITH NO WRITABLE VOLUME MOUNTED, A WRITE IS REFUSED AND NOTHING IS REPORTED AS WRITTEN.
 *
 * THE DEFAULT IS A SAFETY ARGUMENT, NOT A CONVENIENCE. A handle here is a FABRICATED empty
 * file with no host object behind it, so the only two possible answers are a refusal and a
 * success that transferred nothing -- and the second is indistinguishable, from the title's
 * side, from a disk that silently discards save data.
 *
 * MUTATION: let the fabricated-empty backing fall through to the write, or return true
 * with zero bytes, and this fails.
 */
static void test_a_write_with_no_writable_volume_is_refused_not_absorbed(void)
{
    setup();
    /* No mount at all, and the fabricate policy on, which is the most permissive state this
     * host can be in. Even here nothing becomes writable. */
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);

    CHECK_EQ_U32(write_file(handle, "DATA", 4u, NULL), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(iosb_status(), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK_EQ_U32(kernel_io_write_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK_EQ_U32(kernel_file_write_refused_count(), 1u);
    CHECK(captured_contains("FABRICATED empty file with no host object"));

    /* And nothing was created on the host while we were at it. */
    CHECK(!host_anything_exists("save.dat"));
    teardown();
}

/*
 * A WRITE THROUGH A HANDLE WHOSE OPEN DID NOT ASK FOR WRITE ACCESS IS REFUSED.
 *
 * NT's own rule rather than an invention: a handle opened without write access cannot be
 * written through. Asserted because the alternative design -- making every HOST_DIR
 * descriptor O_RDWR -- would pass every other test in this file, while handing the guest a
 * capability it never requested on a volume holding save games.
 *
 * MUTATION: drop the `writable` test from `writable_entry_locked`, or open every HOST_DIR
 * file O_RDWR, and this fails.
 */
static void test_a_write_through_a_read_only_handle_is_refused(void)
{
    setup();
    mount_the_hdd();
    static const char existing[] = "ORIGINAL";
    host_write_file("save.dat", existing, sizeof(existing) - 1u);

    /* MEASURED_ACCESS is 0x00100001: SYNCHRONIZE | FILE_READ_DATA, no write bit. */
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\save.dat"), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(!info.writable);

    CHECK_EQ_U32(write_file(handle, "XX", 2u, NULL), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK(captured_contains("opened WITHOUT write access"));

    /* The file on the host is untouched, byte for byte. */
    char back[32];
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)),
                 (uint32_t)(sizeof(existing) - 1u));
    CHECK(memcmp(back, existing, sizeof(existing) - 1u) == 0);
    teardown();
}

/*
 * A WRITE TO A DIRECTORY HANDLE IS REFUSED, which mirrors the read path's refusal.
 *
 * A directory has no byte stream, and a success here would be this host inventing one.
 */
static void test_a_write_to_a_directory_handle_is_refused(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF,
                                         OPTION_DIRECTORY_FILE),
                 STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK(host_dir_exists("TDATA"));

    CHECK_EQ_U32(write_file(handle, "X", 1u, NULL), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK(captured_contains("which is a DIRECTORY -- REFUSED"));
    CHECK_EQ_U32(set_end_of_file(handle, 0u), KERNEL_FILE_STATUS_ACCESS_DENIED);
    teardown();
}

/*
 * A ZERO-LENGTH WRITE STILL PASSES THE ACCESS AND BACKING CHECKS.
 *
 * THE TRAP THIS CLOSES. The natural loop shape, `while (total < length)`, never runs its
 * body for a zero-length request, so the handler would fall straight through to the success
 * report having consulted neither the backing nor the access mask -- a path on which a
 * write aimed at the user's disc comes back STATUS_SUCCESS. The count is zero either way,
 * so nothing else in this file would notice.
 */
static void test_a_zero_length_write_still_goes_through_the_gate(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    CHECK_EQ_U32(open_path(DISC_DEVICE "\\HELLO"), STATUS_SUCCESS);
    CHECK_EQ_U32(write_file(issued_handle(), "", 0u, NULL),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(kernel_file_write_refused_count(), 1u);
    teardown();

    /* And on a volume that CAN be written, it succeeds as a no-op, which is what NT does. */
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat",
                                         DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(write_file(issued_handle(), "", 0u, NULL), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK_EQ_U32(kernel_file_write_refused_count(), 0u);
    teardown();
}

/*
 * AN OVERWRITING DISPOSITION TRUNCATES, AND SAYS SO IN IO_STATUS_BLOCK.information.
 *
 * WHAT THIS REPLACED WAS SILENTLY WRONG. FILE_OVERWRITE_IF on an existing name used to open
 * it untouched and report FILE_OPENED, so a title writing shorter content than last time
 * left the old tail in place, indistinguishable from data it had written itself.
 *
 * AND IT EXPLAINS A SITE THIS TREE RECORDED AS NOT UNDERSTOOD. `guest_structs.h` notes that
 * 0x0037D394 compares this field against 3 and that the branch was unexplained because 3
 * was not a value this module produced. 3 is NT's FILE_OVERWRITTEN, Win32 `CreateFile`
 * turns it into ERROR_ALREADY_EXISTS for CREATE_ALWAYS, and it is produced here.
 *
 * NEITHER DISPOSITION IS MEASURED IN THIS IMAGE -- only 2 and 3 occur -- which is said in
 * the test rather than left to be discovered.
 */
static void test_an_overwriting_disposition_truncates_and_reports_it(void)
{
    setup();
    mount_the_hdd();
    static const char old_content[] = "A-MUCH-LONGER-PREVIOUS-SAVE";
    host_write_file("save.dat", old_content, sizeof(old_content) - 1u);

    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat", 5u /* OVERWRITE_IF */,
                                         0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 3u /* FILE_OVERWRITTEN */);
    const uint32_t handle = issued_handle();

    /* Truncated for real, on the host, before anything was written. */
    char back[64];
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 0u);
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32((uint32_t)info.size, 0u);

    CHECK_EQ_U32(write_file(handle, "NEW", 3u, NULL), STATUS_SUCCESS);
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 3u);
    CHECK(memcmp(back, "NEW", 3u) == 0);
    CHECK(captured_contains("TRUNCATED TO ZERO"));

    /* FILE_SUPERSEDE reports 0, which looks like "nothing to report" and is not. */
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat", 0u /* SUPERSEDE */, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0u /* FILE_SUPERSEDED */);
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 0u);
    teardown();
}

/*
 * AN OVERWRITING DISPOSITION ON A VOLUME THIS HOST CANNOT TRUNCATE IS REFUSED, not quietly
 * opened.
 *
 * Reporting success would tell the title it had just blanked a file on the user's disc. The
 * disc is unharmed either way -- that is what the fingerprint in the test above proves --
 * but the LIE is the thing a later investigation would have no way to see, because the
 * title would then write and read back content that does not match what it believes it
 * stored.
 */
static void test_an_overwriting_disposition_on_a_disc_is_refused(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    uint64_t before_bytes = 0u;
    const uint64_t before = fingerprint("synthetic.iso", &before_bytes);
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));

    CHECK_EQ_U32(create_path_for_writing(DISC_DEVICE "\\HELLO", 5u /* OVERWRITE_IF */, 0u),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(captured_contains("would OVERWRITE an existing name"));
    CHECK_EQ_U32(kernel_file_write_refused_count(), 1u);
    /* FILE_OPEN on the same name still works, so this is a refusal of the DISPOSITION and
     * not of the volume. */
    CHECK_EQ_U32(create_path_for_writing(DISC_DEVICE "\\HELLO", DISPOSITION_FILE_OPEN, 0u),
                 STATUS_SUCCESS);

    kernel_file_unmount_all();
    uint64_t after_bytes = 0u;
    CHECK(fingerprint("synthetic.iso", &after_bytes) == before);
    CHECK(before_bytes == after_bytes);
    teardown();
}

/*
 * NtSetInformationFile's END-OF-FILE CLASS TRULY TRUNCATES, and is refused where a write
 * would be.
 *
 * MEASURED: ordinal 226 is reached ZERO times in the 145-call boot, so this is unexercised
 * by starting the title and is said so here. It is honoured because the refusal that stood
 * in its place claimed the only volume behind this host was a read-only disc, which stopped
 * being true the day `--hdd` landed, and because a write path that can grow a file but not
 * shrink one cannot honour an overwriting disposition.
 */
static void test_setting_the_end_of_file_truncates_for_real(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(create_path_for_writing(HDD_DEVICE "\\save.dat",
                                         DISPOSITION_FILE_OPEN_IF, 0u),
                 STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK_EQ_U32(write_file(handle, "ABCDEFGH", 8u, NULL), STATUS_SUCCESS);

    CHECK_EQ_U32(set_end_of_file(handle, 3u), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    char back[32];
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 3u);
    CHECK(memcmp(back, "ABC", 3u) == 0);

    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32((uint32_t)info.size, 3u);

    /* And it extends, which is the other half of what NT's class does. */
    CHECK_EQ_U32(set_end_of_file(handle, 10u), STATUS_SUCCESS);
    memset(back, 0, sizeof(back));
    CHECK_EQ_U32((uint32_t)host_read_file("save.dat", back, sizeof(back)), 10u);
    CHECK(memcmp(back, "ABC", 3u) == 0);
    teardown();
}

/*
 * reset() gives back everything, including the writable volume and its counters.
 *
 * A reset that left a volume mounted would make one test's hard disk the next test's
 * silent default, which is the whole failure this feature is built to avoid, arriving via
 * the test suite instead of the command line.
 *
 * MUTATION: skip the volume table or any of the four new counters in
 * `kernel_file_reset`, and this fails.
 */
static void test_reset_gives_back_the_writable_volume_and_its_counters(void)
{
    setup();
    mount_the_hdd();
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        STATUS_SUCCESS);
    CHECK_EQ_U32(open_path(HDD_DEVICE "\\.."), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(kernel_file_created_count(), 1u);
    CHECK_EQ_U32(kernel_file_host_opened_count(), 1u);
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 1u);

    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);
    CHECK_EQ_U32(kernel_file_host_opened_count(), 0u);
    CHECK_EQ_U32(kernel_file_escape_refused_count(), 0u);
    CHECK_EQ_U32((uint32_t)kernel_file_host_bytes_read(), 0u);

    /* The directory on the host SURVIVES the reset, which is the point of a real backing
     * store, and the create refuses again because there is no longer a volume. */
    CHECK(host_dir_exists("TDATA"));
    CHECK_EQ_U32(
        create_path(GUEST_TDATA, DISPOSITION_FILE_OPEN_IF, MEASURED_CREATE_OPTIONS),
        KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/*
 * The title's own `T:` link resolves onto the writable volume.
 *
 * MEASURED: the guest creates `\??\T:` with IoCreateSymbolicLink, the same mechanism that
 * makes `D:` mean the disc. Worth asserting because it is the hop that makes the mount
 * useful to the title rather than only to this suite: nothing here hard-codes `T:`, and if
 * anything ever does, this test still passes while the host has acquired a second source of
 * truth for what `T:` means.
 *
 * MUTATION: resolve symbolic links after locating the volume instead of before, and this
 * fails.
 */
static void test_the_titles_own_drive_letter_resolves_onto_the_volume(void)
{
    setup();
    mount_the_hdd();
    /* Before the link exists, `T:` means nothing. */
    CHECK_EQ_U32(create_path("\\??\\T:\\TDATA", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);

    CHECK(kernel_file_add_symlink("\\??\\T:", HDD_DEVICE));
    CHECK_EQ_U32(create_path("\\??\\T:\\TDATA", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_created_count(), 1u);
    CHECK(host_dir_exists("TDATA"));

    /* And an escape through the link is refused just the same, because the escape check
     * runs on the post-link name. */
    CHECK_EQ_U32(open_path("\\??\\T:\\..\\escaped"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    teardown();
}


/* --- the virtual raw device: partition0 -------------------------------------------- */

/*
 * WITHOUT THE DEVICE MOUNT, partition0 IS REFUSED EXACTLY AS BEFORE, even with `--hdd`.
 *
 * This is the property the previous task protected: backing partition1 with a directory must
 * not make the raw device name resolve. The two are separate mounts and only the operator's
 * explicit call creates the second.
 *
 * MUTATION: let the longest-prefix match treat partition1's mount as a parent of partition0
 * (or make the device resolve with no mount), and this fails.
 */
static void test_partition0_is_refused_unless_the_device_was_mounted(void)
{
    setup();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(handle_slot_untouched());
    mount_the_hdd();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(kernel_file_device_opened_count(), 0u);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE) == (uint32_t)-1, 1u);
    teardown();
}

/*
 * THE DEVICE OPENS BY ITS EXACT NAME AND ONLY THAT, and it announces itself.
 *
 * MUTATION: drop the `rest[0] != '\0'` refusal and `partition0\foo` resolves to the device
 * (and `partition0\` to a "root directory" that is the device), so this fails.
 */
static void test_the_device_opens_by_its_exact_name_and_nothing_inside_it(void)
{
    setup();
    mount_the_device();
    CHECK(captured_contains("VIRTUAL DEVICE"));
    CHECK(captured_contains("FABRICATED"));
    /* The file exists on the host now, empty, and is a regular file. */
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), 0u);

    CHECK_EQ_U32(open_device(DEVICE_PREFIX "\\"), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK_EQ_U32(open_device(DEVICE_PREFIX "\\inside"), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(kernel_file_device_opened_count(), 0u);

    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_file_device_opened_count(), 1u);
    CHECK(captured_contains("VIRTUAL raw device"));

    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(info.writable);
    CHECK(!info.is_directory);
    /* The device's size, not the (zero) length of the file behind it. */
    CHECK_EQ_U64(info.size, DEVICE_CAPACITY);
    /* Resolved case-insensitively like every other name here. */
    CHECK_EQ_U32(open_device("\\DEVICE\\HARDDISK0\\PARTITION0"), STATUS_SUCCESS);
    teardown();
}

/*
 * THE MEASURED READ OF A FRESH DEVICE RETURNS ZEROS, AND SAYS THEY ARE OURS.
 *
 * 0x200 bytes at offset 0x800, into a buffer that starts non-zero so "wrote zeros" cannot be
 * confused with "wrote nothing". The byte just past the read is checked too: a read that
 * overran by one is the cheapest bug to ship.
 *
 * MUTATION: skip the zero-fill and the buffer keeps its sentinel (the guest would then test
 * a magic dword against stack garbage). Count the fabricated bytes as real ones and the
 * host_bytes_read assertion fails. Drop the log line and the announcement check fails.
 */
static void test_a_fresh_device_reads_as_announced_zeros(void)
{
    setup();
    mount_the_device();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    fill_device_buffer(SENTINEL_BYTE, MEASURED_DEVICE_LENGTH + 1u);
    CHECK_EQ_U32(read_device(handle, MEASURED_DEVICE_OFFSET, MEASURED_DEVICE_LENGTH),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), MEASURED_DEVICE_LENGTH);
    unsigned nonzero = 0u;
    for (uint32_t i = 0u; i < MEASURED_DEVICE_LENGTH; i++) {
        nonzero += device_buffer_byte(i) != 0u ? 1u : 0u;
    }
    CHECK_EQ_U32(nonzero, 0u);
    CHECK_EQ_U32(device_buffer_byte(MEASURED_DEVICE_LENGTH), SENTINEL_BYTE);

    CHECK_EQ_U64(kernel_file_device_zero_bytes(), MEASURED_DEVICE_LENGTH);
    CHECK_EQ_U64(kernel_file_host_bytes_read(), 0u);
    CHECK(captured_contains("FABRICATED zeros"));
    /* Reading did not grow the backing file: zeros are served, not stored. */
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), 0u);
    teardown();
}

/*
 * THE MEASURED WRITE PERSISTS IN THE BACKING FILE AND READS BACK, WITH ZEROS AROUND IT.
 *
 * 0x200 bytes at 0x800, as XapiSelectCachePartition writes its table. Verified through the
 * host's own fopen, not through the module that wrote it, and read back across the written
 * span plus 16 bytes either side so a transposed offset or a length of 0x1FF is visible.
 *
 * MUTATION: write at offset 0 instead of the requested one, or fabricate zeros OVER the real
 * bytes (zero-fill from the start instead of from `done`), and this fails.
 */
static void test_a_write_persists_in_the_backing_file_and_reads_back(void)
{
    setup();
    mount_the_device();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    uint8_t pattern[MEASURED_DEVICE_LENGTH];
    for (uint32_t i = 0u; i < MEASURED_DEVICE_LENGTH; i++) {
        pattern[i] = (uint8_t)(0xA0u + (i % 0x3Bu));
    }
    const uint64_t offset = MEASURED_DEVICE_OFFSET;
    CHECK_EQ_U32(write_file(handle, pattern, MEASURED_DEVICE_LENGTH, &offset),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), MEASURED_DEVICE_LENGTH);
    CHECK_EQ_U64(kernel_file_host_bytes_written(), MEASURED_DEVICE_LENGTH);

    uint8_t on_host[0x1000];
    memset(on_host, 0xEE, sizeof(on_host));
    const size_t got = host_read_file(DEVICE_FILE, on_host, sizeof(on_host));
    CHECK_EQ_U32((uint32_t)got, MEASURED_DEVICE_OFFSET + MEASURED_DEVICE_LENGTH);
    CHECK(memcmp(on_host + MEASURED_DEVICE_OFFSET, pattern, MEASURED_DEVICE_LENGTH) == 0);
    /* The hole before it is a real hole, which reads as zeros on the host too. */
    unsigned hole = 0u;
    for (uint32_t i = 0u; i < MEASURED_DEVICE_OFFSET; i++) {
        hole += on_host[i] != 0u ? 1u : 0u;
    }
    CHECK_EQ_U32(hole, 0u);

    fill_device_buffer(SENTINEL_BYTE, MEASURED_DEVICE_LENGTH + 0x20u);
    CHECK_EQ_U32(read_device(handle, MEASURED_DEVICE_OFFSET - 0x10u,
                             MEASURED_DEVICE_LENGTH + 0x20u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), MEASURED_DEVICE_LENGTH + 0x20u);
    unsigned bad = 0u;
    for (uint32_t i = 0u; i < 0x10u; i++) {
        bad += device_buffer_byte(i) != 0u ? 1u : 0u;
        bad += device_buffer_byte(0x10u + MEASURED_DEVICE_LENGTH + i) != 0u ? 1u : 0u;
    }
    for (uint32_t i = 0u; i < MEASURED_DEVICE_LENGTH; i++) {
        bad += device_buffer_byte(0x10u + i) != pattern[i] ? 1u : 0u;
    }
    CHECK_EQ_U32(bad, 0u);
    /* Only the 0x10 bytes past the file's end were fabricated. The 0x10 before the written
     * span are a hole INSIDE the file, which the host serves as zeros itself and which is
     * counted as a host read, so the two counters sum to the whole transfer. */
    CHECK_EQ_U64(kernel_file_device_zero_bytes(), 0x10u);
    CHECK_EQ_U64(kernel_file_host_bytes_read(), MEASURED_DEVICE_LENGTH + 0x10u);
    teardown();
}

/*
 * WHAT THE TITLE WROTE SURVIVES THE RUN, which is why the device is a file.
 *
 * Mount, write, tear the module down completely, mount the same directory again, and the
 * bytes are there. The second mount must ADOPT the existing file rather than truncate it.
 *
 * MUTATION: open the backing file with O_TRUNC, and this fails.
 */
static void test_a_second_mount_sees_what_the_first_run_wrote(void)
{
    setup();
    mount_the_device();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    /* A fresh read first, so both counters are non-zero when the reset has to clear them. */
    CHECK_EQ_U32(read_device(issued_handle(), 0u, 0x10u), STATUS_SUCCESS);
    CHECK_EQ_U64(kernel_file_device_zero_bytes(), 0x10u);
    CHECK_EQ_U32(kernel_file_device_opened_count(), 1u);
    static const char payload[] = "CONFIG-AREA-TABLE";
    const uint64_t offset = MEASURED_DEVICE_OFFSET;
    CHECK_EQ_U32(write_file(issued_handle(), payload, sizeof(payload), &offset),
                 STATUS_SUCCESS);

    kernel_file_reset();
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    CHECK_EQ_U32(kernel_file_device_opened_count(), 0u);
    CHECK_EQ_U64(kernel_file_device_zero_bytes(), 0u);
    /* After the reset the name is refused again: nothing stays mounted. */
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);

    mount_the_device();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    fill_device_buffer(SENTINEL_BYTE, sizeof(payload));
    CHECK_EQ_U32(read_device(issued_handle(), MEASURED_DEVICE_OFFSET, sizeof(payload)),
                 STATUS_SUCCESS);
    unsigned wrong = 0u;
    for (uint32_t i = 0u; i < sizeof(payload); i++) {
        wrong += device_buffer_byte(i) != (uint8_t)payload[i] ? 1u : 0u;
    }
    CHECK_EQ_U32(wrong, 0u);
    CHECK_EQ_U64(kernel_file_device_zero_bytes(), 0u);
    teardown();
}

/*
 * A PARTITION ENDS. Reads stop at the capacity, and a write that would run past it is
 * refused whole and does not grow the backing file.
 *
 * The capacity here is 0x1000 so the boundary is reachable. The three read cases are one
 * straddling the end, one starting exactly at it, and one far beyond.
 *
 * MUTATION: let a read run past the capacity, or clip instead of refusing a write, or drop
 * the `offset > capacity` half of the write guard (an offset past the end with length 1 then
 * wraps the subtraction), and this fails.
 */
static void test_reads_and_writes_stop_at_the_capacity(void)
{
    setup();
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE, 0x1000u));
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();

    fill_device_buffer(SENTINEL_BYTE, 0x200u);
    CHECK_EQ_U32(read_device(handle, 0xF00u, 0x200u), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0x100u);
    CHECK_EQ_U32(device_buffer_byte(0xFFu), 0u);
    CHECK_EQ_U32(device_buffer_byte(0x100u), SENTINEL_BYTE);
    /* T1007/T1011/T1012 xemu EOF contract: leave previous IOSB unchanged. */
    CHECK_EQ_U32(read_device(handle, 0x1000u, 0x200u), STATUS_END_OF_FILE);
    CHECK_EQ_U32(iosb_information(), 0x100u);
    CHECK_EQ_U32(read_device(handle, 0x7FFFFFFFFull, 0x10u), STATUS_END_OF_FILE);
    CHECK_EQ_U32(iosb_information(), 0x100u);

    static const uint8_t block[0x200] = {1};
    uint64_t offset = 0xF00u;
    CHECK_EQ_U32(write_file(handle, block, 0x200u, &offset), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_io_write_refused_count(), 1u);
    CHECK(captured_contains("past its"));
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), 0u);
    offset = 0x2000u;
    CHECK_EQ_U32(write_file(handle, block, 1u, &offset), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), 0u);

    /* Exactly to the end is allowed. */
    offset = 0xE00u;
    CHECK_EQ_U32(write_file(handle, block, 0x200u, &offset), STATUS_SUCCESS);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), 0x1000u);
    teardown();
}

/*
 * A DEVICE HAS NO LENGTH TO CHANGE. Setting the end of file and the overwriting dispositions
 * are refused, and what the title wrote is still there afterwards.
 *
 * MUTATION: drop either refusal and the backing file is truncated (the size check fails).
 */
static void test_end_of_file_and_overwrite_cannot_truncate_the_device(void)
{
    setup();
    mount_the_device();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    static const char payload[] = "KEEP-ME";
    const uint64_t offset = 0x10u;
    CHECK_EQ_U32(write_file(handle, payload, sizeof(payload), &offset), STATUS_SUCCESS);
    const uint32_t expected_size = 0x10u + (uint32_t)sizeof(payload);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), expected_size);

    CHECK_EQ_U32(set_end_of_file(handle, 0u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), expected_size);
    CHECK(captured_contains("fixed length"));

    /* FILE_SUPERSEDE (0) on the device name asks to replace it. */
    CHECK_EQ_U32(create_path_for_writing(DEVICE_PREFIX, 0u, MEASURED_DEVICE_OPTIONS),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32((uint32_t)host_file_size(DEVICE_FILE), expected_size);
    /* And nothing can be created inside it. */
    CHECK_EQ_U32(create_path_for_writing(DEVICE_PREFIX "\\inside", DISPOSITION_FILE_OPEN_IF,
                                         MEASURED_WRITE_OPTIONS),
                 KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    teardown();
}

/*
 * NOTHING CAN BE CREATED ON A DEVICE, even when its backing file has vanished.
 *
 * The one way to reach the create path with the device's own name: delete the backing file
 * under the mount, so the open reports not-found and a FILE_OPEN_IF falls through to the
 * create. It must be REFUSED, not turned into a creation or a "collision" with a name that
 * has no leaf. And with the file back, the same call is a plain open.
 *
 * MUTATION: drop the device test from `hostdir_create_locked`, and the status becomes the
 * leaf-less collision.
 */
static void test_a_vanished_backing_file_does_not_become_a_creation(void)
{
    setup();
    mount_the_device();
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, DEVICE_FILE);
    CHECK(unlink(path) == 0);
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ_U32(create_path_for_writing(DEVICE_PREFIX, DISPOSITION_FILE_OPEN_IF,
                                         MEASURED_DEVICE_OPTIONS),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(kernel_file_created_count(), 0u);
    CHECK(!host_file_exists(DEVICE_FILE));
    teardown();
}

/*
 * THE MOUNT REFUSES ANYTHING THAT IS NOT A REGULAR FILE, and says WHY in the pre-open check.
 *
 * A symbolic link, a FIFO and a directory under the backing name, each in a fresh directory
 * state. The refusal must come from the type check BEFORE any open (a FIFO opened O_RDWR is
 * already an action), so the assertion is on the log's wording and not only on the result:
 * the later `fstat` would refuse the same cases with a different message.
 *
 * Then the argument validation: a name with a separator, "..", an empty name, a zero
 * capacity, one past the maximum, an existing file larger than the capacity, and a trailing
 * separator on the prefix.
 *
 * MUTATION: delete the `fstatat` pre-check (message changes), accept a file larger than the
 * capacity, or lift the capacity ceiling, and this fails.
 */
static void test_the_device_mount_refuses_what_is_not_a_regular_file(void)
{
    setup();
    char path[512];

    host_make_symlink("elsewhere", "linked.bin");
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "linked.bin",
                                         DEVICE_CAPACITY));
    CHECK(captured_contains("not a REGULAR file"));

    captured[0] = '\0';
    captured_len = 0u;
    (void)snprintf(path, sizeof(path), "%s/%s", host_root, "pipe.bin");
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "pipe.bin",
                                         DEVICE_CAPACITY));
    CHECK(captured_contains("not a REGULAR file"));

    captured[0] = '\0';
    captured_len = 0u;
    host_make_dir("dir.bin");
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "dir.bin",
                                         DEVICE_CAPACITY));
    CHECK(captured_contains("not a REGULAR file"));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);

    /* An existing regular file larger than the capacity is not adopted. */
    uint8_t big[0x2000];
    memset(big, 0x11, sizeof(big));
    host_write_file("big.bin", big, sizeof(big));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "big.bin", 0x1000u));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    /* And one that fits is adopted untouched. */
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "big.bin", 0x2000u));
    CHECK_EQ_U32((uint32_t)host_file_size("big.bin"), 0x2000u);
    kernel_file_unmount_all();

    /* A real subdirectory, so a separator that slipped through would SUCCEED in creating
     * `sub/dev.bin` instead of failing on a missing parent. */
    host_make_dir("sub");
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "sub/dev.bin",
                                         DEVICE_CAPACITY));
    CHECK(!host_file_exists("sub/dev.bin"));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "a/b", DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "a\\b", DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "..", DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, ".", DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, "", DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE, 0u));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE,
                                         KERNEL_FILE_DEVICE_CAPACITY_MAX + 1u));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX "\\", host_root, DEVICE_FILE,
                                         DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(NULL, host_root, DEVICE_FILE, DEVICE_CAPACITY));
    CHECK(!kernel_file_mount_host_device(DEVICE_PREFIX, "tsfp-no-such-dir", DEVICE_FILE,
                                         DEVICE_CAPACITY));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    /* The ceiling itself is accepted. */
    CHECK(kernel_file_mount_host_device(DEVICE_PREFIX, host_root, DEVICE_FILE,
                                        KERNEL_FILE_DEVICE_CAPACITY_MAX));
    teardown();
}

/* --- a formattable cache partition: a raw view and a directory view, one name ------ */

/*
 * MEASURED in the title's own boot (docs/disc-io.md section 2e): XMountUtilityDrive picks
 * Partition5 (the highest free slot of three), formats it with XapiFormatFATVolumeEx, then
 * validates it. The literals below are read out of that function's disassembly.
 *
 *   raw open      0x003813F4  access 0x100003, share 0, options 0x18, name with NO separator
 *   geometry      0x0038143D  NtDeviceIoControlFile 0x70000, 0x18 bytes out, reads +0x14
 *   partition     0x00381479  NtDeviceIoControlFile 0x74004, 0x20 bytes out, reads +0x08/+0x0C
 *   FATX header   0x00381574  the first dword it writes at offset 0 is 0x58544146
 *   dismount      0x003816D4  NtFsControlFile 0x90020, no buffers
 *   directory     0x00380D43  NtOpenFile of the SAME name plus `\`, access 0x100001, options
 *                              0x800021, then NtQueryVolumeInformationFile class 3
 */
#define CACHE_PREFIX "\\Device\\Harddisk0\\Partition5"
#define CACHE_IMAGE ".tsfp-cache5.bin"
#define CACHE_DIR ".tsfp-cache5"
#define CACHE_NUMBER 5u
#define CACHE_CAPACITY 0x1000000ull
#define MEASURED_CACHE_RAW_ACCESS 0x00100003u
#define MEASURED_CACHE_RAW_SHARE 0u
#define MEASURED_CACHE_RAW_OPTIONS 0x18u
#define ORD_NT_DEVICE_IO_CONTROL_FILE 196u
#define ORD_NT_FS_CONTROL_FILE 200u
#define ORD_NT_QUERY_VOLUME_INFORMATION_FILE 218u
#define MEASURED_IOCTL_GEOMETRY 0x70000u
#define MEASURED_IOCTL_PARTITION_INFO 0x74004u
#define MEASURED_FSCTL_DISMOUNT 0x90020u
#define STATUS_UNRECOGNIZED_VOLUME_CODE 0xC000014Fu
#define STATUS_INVALID_DEVICE_REQUEST_CODE 0xC0000010u
#define STATUS_BUFFER_TOO_SMALL_CODE 0xC0000023u
/* The title writes this dword first, which is the bytes "FATX" on disk. */
#define FATX_MAGIC_DWORD 0x58544146u

static void mount_the_cache_partition(void)
{
    CHECK(kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER,
                                            CACHE_IMAGE, CACHE_DIR, CACHE_CAPACITY));
}

/* NtOpenFile with the literals measured at 0x003813F4. */
static uint32_t open_cache_raw(void)
{
    const kernel_guest_ptr name = build_name(CACHE_PREFIX);
    const kernel_guest_ptr oa = build_attributes(name);
    const uint32_t args[6] = {at(HANDLE_OFFSET), MEASURED_CACHE_RAW_ACCESS, oa,
                              at(IOSB_OFFSET),   MEASURED_CACHE_RAW_SHARE,
                              MEASURED_CACHE_RAW_OPTIONS};
    return call_ordinal(ORD_NT_OPEN_FILE, args, 6u);
}

/*
 * The 10-argument shape shared by ordinals 196 and 200, in the order measured at 0x0038143D:
 * (handle, event, apc, apc context, iosb, code, in buffer, in length, out buffer, out length).
 * `out_length` 0 passes a NULL buffer, as the title does for the dismount.
 */
static uint32_t control_call(unsigned ordinal, uint32_t handle, uint32_t code,
                             uint32_t out_length)
{
    const uint32_t args[10] = {handle,
                               0u,
                               0u,
                               0u,
                               at(IOSB_OFFSET),
                               code,
                               0u,
                               0u,
                               out_length != 0u ? at(DEVICE_BUFFER_OFFSET) : 0u,
                               out_length};
    return call_ordinal(ordinal, args, 10u);
}

static uint32_t device_buffer_u32(uint32_t offset)
{
    return read_u32_at(at(DEVICE_BUFFER_OFFSET) + offset);
}

/* The title's first format write: 4096 bytes at offset 0 starting with the FATX dword. */
static void write_the_fatx_header(uint32_t handle)
{
    static uint8_t block[4096];
    memset(block, 0xFF, sizeof(block));
    block[0] = 'F';
    block[1] = 'A';
    block[2] = 'T';
    block[3] = 'X';
    const uint64_t offset = 0u;
    CHECK_EQ_U32(write_file(handle, block, sizeof(block), &offset), STATUS_SUCCESS);
}

/* NtQueryVolumeInformationFile class 3 into the device buffer, as 0x00380D58 does. */
static uint32_t query_volume_size(uint32_t handle)
{
    const uint32_t args[5] = {handle, at(IOSB_OFFSET), at(DEVICE_BUFFER_OFFSET), 0x18u, 3u};
    return call_ordinal(ORD_NT_QUERY_VOLUME_INFORMATION_FILE, args, 5u);
}

/*
 * THE NAME IS TWO THINGS, AND THE DIRECTORY ONE IS NOT THERE UNTIL THE TITLE FORMATS.
 *
 * Exact name: a raw device that knows it is a cache partition. Name plus a separator, or
 * anything under it: UNRECOGNIZED_VOLUME while the image is blank, which is what makes the
 * title format, and NOTHING is created on the host for it.
 *
 * MUTATION: drop the formatted gate and `Partition5\` opens at once, so the title would never
 * format and would validate an empty directory. Create the directory at mount time and the
 * host_anything_exists check fails. Treat a separator as part of the raw name and the first
 * two opens succeed.
 */
static void test_a_cache_partition_is_a_raw_device_by_name_and_not_a_directory_until_formatted(void)
{
    setup();
    mount_the_cache_partition();
    CHECK(captured_contains("CACHE PARTITION 5"));
    CHECK(captured_contains("FABRICATED"));
    CHECK_EQ_U32((uint32_t)host_file_size(CACHE_IMAGE), 0u);
    CHECK(!host_anything_exists(CACHE_DIR));

    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    CHECK(handle_slot_untouched());
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\inside"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    CHECK(handle_slot_untouched());
    CHECK(captured_contains("NOT FORMATTED"));
    /* Creating inside an unformatted partition is refused for the same reason, and creates
     * nothing: the gate sits in front of the create as well as the open. */
    CHECK_EQ_U32(create_path_for_writing(CACHE_PREFIX "\\early.bin", DISPOSITION_FILE_CREATE,
                                         MEASURED_WRITE_OPTIONS),
                 STATUS_UNRECOGNIZED_VOLUME_CODE);
    CHECK(handle_slot_untouched());
    CHECK(!host_anything_exists(CACHE_DIR));
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 0u);

    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t handle = issued_handle();
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(handle, &info));
    CHECK(info.device);
    CHECK_EQ_U32(info.cache_partition, CACHE_NUMBER);
    CHECK(info.writable);
    CHECK(!info.is_directory);
    CHECK_EQ_U64(info.size, CACHE_CAPACITY);
    CHECK_EQ_U32(kernel_file_device_opened_count(), 1u);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 0u);
    /* Opening the raw view is not formatting, so still no directory. */
    CHECK(!host_anything_exists(CACHE_DIR));
    teardown();
}

/*
 * THE TITLE'S FORMAT SEQUENCE IS ANSWERED, AND ONLY THEN DOES THE DIRECTORY VIEW EXIST.
 *
 * Drives 196 twice, 236, 200, then the validation, in the measured order, and checks the
 * structures the guest READS BACK: BytesPerSector at +0x14 (a power of two, 512), the
 * partition length at +0x08/+0x0C (what the title sizes its FAT from), and finally the class-3
 * product `[+0x10] * [+0x14] == 0x4000` that XapiValidateDiskPartitionEx compares. Every byte of
 * both structures is written, proven by pre-filling the buffer with a sentinel.
 *
 * MUTATION: write BytesPerSector at +0x10, return the capacity in the wrong dword, skip the
 * `Z:` link, or return an error from the dismount, and the title would fail its format.
 */
static void test_the_titles_format_sequence_is_answered_and_unlocks_the_directory_view(void)
{
    setup();
    mount_the_cache_partition();
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t raw = issued_handle();

    fill_device_buffer(SENTINEL_BYTE, 0x40u);
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw, MEASURED_IOCTL_GEOMETRY,
                              0x18u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0x18u);
    CHECK_EQ_U32(device_buffer_u32(0x14u), 512u);
    /* A power of two, because the title takes its lowest set bit as a shift. */
    CHECK((device_buffer_u32(0x14u) & (device_buffer_u32(0x14u) - 1u)) == 0u);
    CHECK_EQ_U32(device_buffer_u32(0x08u), 12u); /* FixedMedia */
    /* Written through +0x17 and not a byte beyond. */
    CHECK_EQ_U32(device_buffer_u32(0x18u), SENTINEL_BYTE * 0x01010101u);

    fill_device_buffer(SENTINEL_BYTE, 0x40u);
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw,
                              MEASURED_IOCTL_PARTITION_INFO, 0x20u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0x20u);
    CHECK_EQ_U32(device_buffer_u32(0x08u), (uint32_t)(CACHE_CAPACITY & 0xFFFFFFFFu));
    CHECK_EQ_U32(device_buffer_u32(0x0Cu), (uint32_t)(CACHE_CAPACITY >> 32));
    CHECK_EQ_U32(device_buffer_u32(0x14u), CACHE_NUMBER);
    /* All 0x20 bytes written: the tail is zero where the sentinel was. */
    CHECK_EQ_U32(device_buffer_u32(0x18u), 0u);
    CHECK_EQ_U32(device_buffer_u32(0x1Cu), 0u);
    CHECK_EQ_U32(device_buffer_u32(0x20u), SENTINEL_BYTE * 0x01010101u);

    write_the_fatx_header(raw);
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, raw, MEASURED_FSCTL_DISMOUNT, 0u),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_io_control_count(), 3u);
    CHECK_EQ_U32(kernel_io_control_refused_count(), 0u);

    uint8_t on_host[8];
    CHECK_EQ_U32((uint32_t)host_read_file(CACHE_IMAGE, on_host, sizeof(on_host)), 8u);
    CHECK(memcmp(on_host, "FATX", 4u) == 0);

    /* The validation: the same name plus a separator, then the class-3 product. */
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    const uint32_t view = issued_handle();
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(view, &info));
    CHECK(info.is_directory);
    CHECK(!info.device);
    CHECK_EQ_U32(info.cache_partition, 0u);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 1u);
    CHECK(host_dir_exists(CACHE_DIR));
    CHECK(captured_contains("is FORMATTED"));
    fill_device_buffer(SENTINEL_BYTE, 0x20u);
    CHECK_EQ_U32(query_volume_size(view), STATUS_SUCCESS);
    CHECK_EQ_U32(device_buffer_u32(0x10u) * device_buffer_u32(0x14u), 0x4000u);

    /* The title binds Z: to the name WITHOUT the separator, and Z:\ must land in the view. */
    CHECK(kernel_file_add_symlink("\\??\\Z:", CACHE_PREFIX));
    CHECK_EQ_U32(open_path("\\??\\Z:\\"), STATUS_SUCCESS);
    CHECK(kernel_file_open_info(issued_handle(), &info));
    CHECK(info.is_directory);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 2u);
    teardown();
}

/*
 * THE DIRECTORY VIEW IS A REAL DIRECTORY AND THE IMAGE IS NEVER TOUCHED THROUGH IT.
 *
 * A file the title creates under `Partition5\` lands in the host directory, can be written
 * far past the raw image's capacity (it is a file, not the device), and leaves the image at
 * exactly the four bytes the format put there. The mount capacity here is 0x1000 so the
 * boundary is reachable.
 *
 * MUTATION: decide "device" from the VOLUME rather than the handle (the old test) and the write
 * past 0x1000 is refused as running off the partition. Route a create into the raw image and
 * the image grows.
 */
static void test_files_in_the_directory_view_are_real_files_and_not_the_image(void)
{
    setup();
    CHECK(kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER,
                                            CACHE_IMAGE, CACHE_DIR, 0x1000u));
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t raw = issued_handle();
    const uint8_t magic[4] = {'F', 'A', 'T', 'X'};
    const uint64_t zero = 0u;
    CHECK_EQ_U32(write_file(raw, magic, sizeof(magic), &zero), STATUS_SUCCESS);
    CHECK_EQ_U32((uint32_t)host_file_size(CACHE_IMAGE), 4u);

    CHECK_EQ_U32(create_path_for_writing(CACHE_PREFIX "\\scratch.bin", DISPOSITION_FILE_CREATE,
                                         MEASURED_WRITE_OPTIONS),
                 STATUS_SUCCESS);
    const uint32_t file = issued_handle();
    kernel_file_open info;
    memset(&info, 0, sizeof(info));
    CHECK(kernel_file_open_info(file, &info));
    CHECK(!info.device);
    CHECK(info.writable);
    static const char payload[] = "cache-contents";
    const uint64_t far = 0x2000u;
    CHECK_EQ_U32(write_file(file, payload, sizeof(payload), &far), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), sizeof(payload));

    char on_host[0x2100];
    const size_t got = host_read_file(CACHE_DIR "/scratch.bin", on_host, sizeof(on_host));
    CHECK_EQ_U32((uint32_t)got, 0x2000u + sizeof(payload));
    CHECK(memcmp(on_host + 0x2000u, payload, sizeof(payload)) == 0);
    /* Read back through the guest path too: past the image's capacity, which a device
     * handle could not serve, and a truncation of a file in the view is a real one. */
    fill_device_buffer(SENTINEL_BYTE, sizeof(payload) + 1u);
    CHECK_EQ_U32(read_device(file, 0x2000u, sizeof(payload)), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), sizeof(payload));
    CHECK(device_buffer_byte(0u) == (uint8_t)payload[0]);
    CHECK(device_buffer_byte(sizeof(payload) - 1u) == (uint8_t)payload[sizeof(payload) - 1u]);
    CHECK_EQ_U64(kernel_file_device_zero_bytes(), 0u);
    CHECK_EQ_U32(set_end_of_file(file, 0x10u), STATUS_SUCCESS);
    CHECK_EQ_U32((uint32_t)host_file_size(CACHE_DIR "/scratch.bin"), 0x10u);
    /* And the image did not move. */
    CHECK_EQ_U32((uint32_t)host_file_size(CACHE_IMAGE), 4u);
    /* The new file is found again through the volume, by its real name. */
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\scratch.bin"), STATUS_SUCCESS);
    teardown();
}

/*
 * THE FORMATTED GATE IS ASKED ON EVERY OPEN, NOT LATCHED.
 *
 * Once the view has opened, blanking the image makes it unrecognised again, so a title that is
 * about to re-format is told the truth. The directory the view created stays: nothing here
 * deletes the title's cache. Writing the magic back restores the view.
 *
 * MUTATION: skip the check once the directory descriptor exists (latch) and the second open
 * succeeds against a blank image.
 */
static void test_the_formatted_gate_is_not_latched(void)
{
    setup();
    mount_the_cache_partition();
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t raw = issued_handle();
    write_the_fatx_header(raw);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    CHECK(host_dir_exists(CACHE_DIR));

    host_write_file(CACHE_IMAGE, "", 0u);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    CHECK(host_dir_exists(CACHE_DIR));

    /* One byte short of the magic is still blank. */
    host_write_file(CACHE_IMAGE, "FAT", 3u);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    /* All four bytes are compared, not a prefix of them. */
    host_write_file(CACHE_IMAGE, "FATY", 4u);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    host_write_file(CACHE_IMAGE, "XFAT", 4u);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    host_write_file(CACHE_IMAGE, "FATX", 4u);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);

    /* A FIFO swapped in for the image after the mount must not block the gate's read-open
     * forever with the module lock held. It is simply not a formatted image. */
    host_remove_file(CACHE_IMAGE);
    host_make_fifo(CACHE_IMAGE);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    teardown();
}

/*
 * A SECOND RUN FINDS THE PARTITION FORMATTED AND NEVER ASKS THE DEVICE ANYTHING.
 *
 * MEASURED: with the partition0 table already holding an entry the title opens the directory
 * view first and skips the format. Here that means the image survives the module being torn
 * down and re-mounted, and the view opens with no control request issued in between.
 *
 * MUTATION: open the backing image with O_TRUNC at mount, and the second run's view is blank.
 */
static void test_a_second_mount_finds_the_partition_already_formatted(void)
{
    setup();
    mount_the_cache_partition();
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    write_the_fatx_header(issued_handle());
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 1u);
    /* The volume holds a descriptor on the view's directory, and the scan sees it first so
     * "none after the reset" cannot be a scan that finds nothing. */
    unsigned found = 0u;
    (void)writable_descriptors_on(CACHE_DIR, &found);
    CHECK(found >= 1u);

    kernel_file_reset();
    (void)writable_descriptors_on(CACHE_DIR, &found);
    CHECK_EQ_U32(found, 0u);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 0u);
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    mount_the_cache_partition();
    const unsigned controls_before = kernel_io_control_count();
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_io_control_count(), controls_before);
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 1u);
    CHECK(captured_contains("already existed"));
    teardown();
}

/*
 * ONLY THE RAW VIEW OF A CACHE PARTITION ANSWERS A CONTROL REQUEST, AND EVERYTHING ELSE IS
 * REFUSED OUT LOUD.
 *
 * A plain virtual device (partition0), a directory-view handle, an unknown code, the right code
 * through the wrong ordinal, a short output buffer and a handle that was never issued: each is
 * refused with its own status, writes nothing into the output buffer, and is counted.
 *
 * MUTATION: answer on any device handle, accept the code on either ordinal, or drop the length
 * check, and the matching line below fails.
 */
static void test_control_requests_are_refused_unless_they_are_derived(void)
{
    setup();
    mount_the_device();
    mount_the_cache_partition();
    CHECK_EQ_U32(open_device(DEVICE_PREFIX), STATUS_SUCCESS);
    const uint32_t plain = issued_handle();
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t raw = issued_handle();
    unsigned refused = 0u;
    const uint32_t untouched = SENTINEL_BYTE * 0x01010101u;

    /* partition0 is a device but not a cache partition. */
    fill_device_buffer(SENTINEL_BYTE, 0x40u);
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, plain, MEASURED_IOCTL_GEOMETRY,
                              0x18u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(iosb_status(), STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(device_buffer_u32(0x14u), untouched);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);

    /* an unknown code on the right handle */
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw, 0x70001u, 0x18u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(device_buffer_u32(0x14u), untouched);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);

    /* the right code through the wrong ordinal, in both directions */
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw, MEASURED_FSCTL_DISMOUNT, 0u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, raw, MEASURED_IOCTL_GEOMETRY, 0x18u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(device_buffer_u32(0x14u), untouched);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);

    /* the other FSCTL the image issues (0x0037DC8F) is not derived */
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, raw, 0x9411Cu, 0u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, plain, MEASURED_FSCTL_DISMOUNT, 0u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);

    /* output buffers too short, or absent, leave the buffer alone */
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw, MEASURED_IOCTL_GEOMETRY,
                              0x17u),
                 STATUS_BUFFER_TOO_SMALL_CODE);
    CHECK_EQ_U32(iosb_status(), STATUS_BUFFER_TOO_SMALL_CODE);
    CHECK_EQ_U32(device_buffer_u32(0x14u), untouched);
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw,
                              MEASURED_IOCTL_PARTITION_INFO, 0x1Fu),
                 STATUS_BUFFER_TOO_SMALL_CODE);
    CHECK_EQ_U32(device_buffer_u32(0x08u), untouched);
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, raw, MEASURED_IOCTL_GEOMETRY, 0u),
                 STATUS_BUFFER_TOO_SMALL_CODE);
    /* a short buffer is not a refusal of the CODE, so the refused count did not move */
    CHECK_EQ_U32(kernel_io_control_refused_count(), refused);
    CHECK_EQ_U32(kernel_io_control_count(), 0u);

    /* a handle nobody issued */
    CHECK_EQ_U32(control_call(ORD_NT_DEVICE_IO_CONTROL_FILE, 0x7777u, MEASURED_IOCTL_GEOMETRY,
                              0x18u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, 0x7777u, MEASURED_FSCTL_DISMOUNT, 0u),
                 STATUS_INVALID_HANDLE);

    /* the directory view is a directory, not a device */
    write_the_fatx_header(raw);
    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(control_call(ORD_NT_FS_CONTROL_FILE, issued_handle(), MEASURED_FSCTL_DISMOUNT,
                              0u),
                 STATUS_INVALID_DEVICE_REQUEST_CODE);
    CHECK_EQ_U32(kernel_io_control_refused_count(), ++refused);
    CHECK_EQ_U32(kernel_io_control_count(), 0u);
    teardown();
}

/*
 * THE MOUNT REFUSES WHAT IT CANNOT BACK SAFELY.
 *
 * A partition number outside 3..10, a directory name that is the image's name or has a
 * separator or is `..`, a capacity of zero or over the ceiling, a trailing separator, and an
 * image name that already exists as a symbolic link. Each refusal leaves nothing mounted.
 *
 * MUTATION: drop the number range, the name-collision test or the separator test, and the
 * matching call returns true.
 */
static void test_the_cache_partition_mount_refuses_what_it_cannot_back(void)
{
    setup();
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, 0u, CACHE_IMAGE,
                                             CACHE_DIR, CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, 2u, CACHE_IMAGE,
                                             CACHE_DIR, CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, 11u, CACHE_IMAGE,
                                             CACHE_DIR, CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             CACHE_IMAGE, CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             "a/b", CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             "a\\b", CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             "..", CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             "", CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             NULL, CACHE_CAPACITY));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             CACHE_DIR, 0u));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             CACHE_DIR, KERNEL_FILE_DEVICE_CAPACITY_MAX + 1u));
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX "\\", host_root, CACHE_NUMBER,
                                             CACHE_IMAGE, CACHE_DIR, CACHE_CAPACITY));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    CHECK(!host_anything_exists(CACHE_IMAGE));

    host_make_dir("elsewhere");
    host_make_symlink("elsewhere", CACHE_IMAGE);
    CHECK(!kernel_file_mount_cache_partition(CACHE_PREFIX, host_root, CACHE_NUMBER, CACHE_IMAGE,
                                             CACHE_DIR, CACHE_CAPACITY));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);

    /* Both ends of the accepted range mount. */
    CHECK(kernel_file_mount_cache_partition("\\Device\\Harddisk0\\Partition3", host_root, 3u,
                                            ".tsfp-cache3.bin", ".tsfp-cache3",
                                            CACHE_CAPACITY));
    CHECK(kernel_file_mount_cache_partition("\\Device\\Harddisk0\\Partition10", host_root, 10u,
                                            ".tsfp-cache10.bin", ".tsfp-cache10",
                                            CACHE_CAPACITY));
    CHECK_EQ_U32(kernel_file_volume_count(), 2u);
    teardown();
}

/*
 * A SYMBOLIC LINK PLANTED WHERE THE DIRECTORY VIEW LIVES IS REFUSED, NOT FOLLOWED.
 *
 * The directory is opened O_NOFOLLOW | O_DIRECTORY relative to the mount's own descriptor, so a
 * link at that name cannot lead the title's cache out of the backing directory.
 *
 * MUTATION: drop O_NOFOLLOW from that open and the view resolves into `elsewhere`.
 */
static void test_a_symbolic_link_in_place_of_the_directory_view_is_refused(void)
{
    setup();
    mount_the_cache_partition();
    host_make_dir("elsewhere");
    host_make_symlink("elsewhere", CACHE_DIR);
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    const uint32_t raw = issued_handle();
    write_the_fatx_header(raw);

    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    /* The refused open issued no handle: the slot still holds the raw one. */
    CHECK_EQ_U32(issued_handle(), raw);
    CHECK_EQ_U32(create_path_for_writing(CACHE_PREFIX "\\leak.bin", DISPOSITION_FILE_CREATE,
                                         MEASURED_WRITE_OPTIONS),
                 KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK(!host_anything_exists("elsewhere/leak.bin"));
    CHECK_EQ_U32(kernel_file_cache_view_opened_count(), 0u);
    teardown();
}

/*
 * PARTITIONS ARE INDEPENDENT: FORMATTING ONE LEAVES ITS SIBLINGS BLANK.
 *
 * MUTATION: let the gate read the wrong volume's image (the first mounted one), and
 * `Partition3\` opens because `Partition5` was formatted.
 */
static void test_formatting_one_cache_partition_does_not_format_its_siblings(void)
{
    setup();
    CHECK(kernel_file_mount_cache_partition("\\Device\\Harddisk0\\Partition3", host_root, 3u,
                                            ".tsfp-cache3.bin", ".tsfp-cache3",
                                            CACHE_CAPACITY));
    mount_the_cache_partition();
    CHECK_EQ_U32(open_cache_raw(), STATUS_SUCCESS);
    write_the_fatx_header(issued_handle());

    CHECK_EQ_U32(open_path(CACHE_PREFIX "\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(open_path("\\Device\\Harddisk0\\Partition3\\"), STATUS_UNRECOGNIZED_VOLUME_CODE);
    CHECK(host_dir_exists(CACHE_DIR));
    CHECK(!host_anything_exists(".tsfp-cache3"));
    /* And a prefix of a longer number is not the shorter partition. */
    CHECK_EQ_U32(open_path("\\Device\\Harddisk0\\Partition50\\"),
                 KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
}

/* How many `tsfp-hdd-test-*` entries does `directory` hold? */
static unsigned count_scratch_entries(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir) {
        return 0u;
    }
    unsigned count = 0u;
    for (const struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (strncmp(entry->d_name, HOST_ROOT_PREFIX, sizeof(HOST_ROOT_PREFIX) - 1u) == 0) {
            count++;
        }
    }
    (void)closedir(dir);
    return count;
}

/*
 * The leak this guards: every FATAL path calls exit(), which skips teardown, so the scratch
 * directory stayed behind (two `tsfp-hdd-test-*` were found in the repo root). A child makes
 * the directory, plants a file in a subdirectory, and dies the way a FATAL does. Its path
 * comes back over a pipe so the PARENT asks the filesystem whether it survived.
 * Breaks if: the atexit registration is removed, or remove_tree stops recursing.
 */
static void test_a_fatal_exit_still_removes_the_scratch_directory(void)
{
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    (void)fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(pipe_fds[0]);
        make_host_root();
        host_make_dir("nested");
        host_write_file("nested/payload.bin", "x", 1u);
        (void)write(pipe_fds[1], host_root, strlen(host_root) + 1u);
        (void)close(pipe_fds[1]);
        exit(EXIT_FAILURE);
    }
    (void)close(pipe_fds[1]);
    char reported[sizeof(host_root)];
    memset(reported, 0, sizeof(reported));
    const ssize_t got = read(pipe_fds[0], reported, sizeof(reported) - 1u);
    (void)close(pipe_fds[0]);
    int wait_status = 0;
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == EXIT_FAILURE);
    /* Non-empty first: a child that died before reporting would leave "" and `access` of
     * an empty path fails, which would read as "removed". */
    CHECK(got > (ssize_t)sizeof(HOST_ROOT_PREFIX));
    CHECK(strstr(reported, HOST_ROOT_PREFIX) != NULL);
    CHECK(access(reported, F_OK) != 0);
}

/*
 * The scratch directory lives beside the binary, so a run from the repo root leaves the
 * repo root alone. Breaks if the template goes back to a bare relative name, because then
 * the root is not under the binary's directory.
 */
static void test_the_scratch_directory_is_beside_the_binary(void)
{
    setup();
    CHECK(binary_directory[0] == '/');
    CHECK(strncmp(host_root, binary_directory, strlen(binary_directory)) == 0);
    CHECK(host_root[strlen(binary_directory)] == '/');
    CHECK(strstr(host_root + strlen(binary_directory), HOST_ROOT_PREFIX) != NULL);
    CHECK(access(host_root, F_OK) == 0);
    teardown();
}

static void test_inferred_drive_root_policy(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("TDATA");
    host_make_dir("UDATA");
    CHECK(kernel_file_add_symlink("\\??\\T:", HDD_DEVICE "\\TDATA"));
    CHECK(kernel_file_add_symlink("\\??\\U:", HDD_DEVICE "\\UDATA"));
    object_root = UINT32_C(0xFFFFFFFD);
    CHECK_EQ_U32(open_path("U:\\"), STATUS_SUCCESS);
    CHECK_EQ_U32(open_path("t:/"), STATUS_SUCCESS);
    CHECK_EQ_U32(create_path_for_writing("T:\\real.bin", DISPOSITION_FILE_OPEN_IF,
                                       0u), STATUS_SUCCESS);
    const uint32_t file = issued_handle();
    CHECK_EQ_U32(write_file(file, "real", 4u, NULL), STATUS_SUCCESS);
    char bytes[8] = {0};
    CHECK_EQ_U32(host_read_file("TDATA/real.bin", bytes, sizeof(bytes)), 4u);
    CHECK(memcmp(bytes, "real", 4u) == 0);
    CHECK_EQ_U32(open_path("t:/REAL.bin"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 0u);
    const char *announcement = strstr(captured, "INFERRED namespace policy");
    CHECK(announcement != NULL);
    CHECK(announcement && strstr(announcement + 1, "INFERRED namespace policy") == NULL);
    CHECK_EQ_U32(open_path("T:"), STATUS_SUCCESS); /* Original0037D4F9 root. */
    const char *refused[] = {"T:real.bin", "real.bin", "\\Device\\Harddisk0\\partition1",
                            "T:\\..\\escape", "T:/./real.bin", "T:/a/../escape", "1:/file", "", "\xC0:/file"};
    for (size_t i = 0u; i < sizeof(refused)/sizeof(refused[0]); i++) {
        poison();
        CHECK_EQ_U32(open_path(refused[i]), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
        CHECK(handle_slot_untouched());
        CHECK_EQ_U32(iosb_information(), 0u);
    }
    CHECK_EQ_U32(kernel_file_relative_refused_count(), 9u);
    poison();
    CHECK_EQ_U32(open_path("Z:/missing"), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(handle_slot_untouched());
    CHECK(kernel_file_add_symlink("\\??\\C:", "\\??\\C:"));
    poison();
    CHECK_EQ_U32(open_path("C:/missing"), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(handle_slot_untouched());
    CHECK(kernel_file_add_symlink("\\??\\E:", HDD_DEVICE "\\.."));
    CHECK_EQ_U32(open_path("E:/escaped"), KERNEL_FILE_STATUS_ACCESS_DENIED);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    const uint32_t roots[] = {UINT32_C(0xFFFFFFFC), 1u, UINT32_MAX};
    for (size_t i = 0u; i < 3u; i++) {
        object_root = roots[i]; poison();
        CHECK_EQ_U32(create_path("T:/never", DISPOSITION_FILE_OPEN_IF, 0u),
                     KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
        CHECK(handle_slot_untouched());
        CHECK(!host_anything_exists("TDATA/never"));
    }
    object_root = UINT32_C(0xFFFFFFFD);
    char boundary[258];
    memcpy(boundary, "Z:/", 3u); memset(boundary + 3u, 'a', 254u); boundary[257] = '\0';
    poison();
    CHECK_EQ_U32(open_path(boundary), KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(handle_slot_untouched());
    boundary[256] = '\0';
    CHECK_EQ_U32(open_path(boundary), KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    teardown();
    setup(); mount_the_hdd();
    CHECK(kernel_file_add_symlink("\\??\\T:", HDD_DEVICE));
    object_root = UINT32_C(0xFFFFFFFD);
    CHECK_EQ_U32(open_path("T:/"), STATUS_SUCCESS);
    CHECK(strstr(captured, "INFERRED namespace policy") != NULL);
    teardown();
}

/* ---- ordinal 207 NtQueryDirectoryFile ------------------------------------------- */

#define ORD_NT_QUERY_DIRECTORY_FILE 207u
#define DIR_MASK_OFFSET 0x1000u
#define DIR_MASK_CHARS_OFFSET 0x1010u
#define DIR_BUFFER_OFFSET 0x1100u
#define DIR_BUFFER_BYTES 0x148u
#define STATUS_NO_MORE_FILES_VALUE 0x80000006u
#define STATUS_NO_SUCH_FILE_VALUE 0xC000000Fu

/* An ANSI_STRING mask in scratch (length u16, maximum u16, buffer u32). */
static kernel_guest_ptr build_mask(const char *text)
{
    const size_t length = strlen(text);
    for (size_t i = 0u; i < length; i++) {
        (void)kernel_guest_write_u8(at(DIR_MASK_CHARS_OFFSET) + (uint32_t)i, (uint8_t)text[i]);
    }
    (void)kernel_guest_write_u8(at(DIR_MASK_OFFSET), (uint8_t)length);
    (void)kernel_guest_write_u8(at(DIR_MASK_OFFSET) + 1u, 0u);
    (void)kernel_guest_write_u8(at(DIR_MASK_OFFSET) + 2u, (uint8_t)(length + 1u));
    (void)kernel_guest_write_u8(at(DIR_MASK_OFFSET) + 3u, 0u);
    write_u32_at(at(DIR_MASK_OFFSET) + 4u, at(DIR_MASK_CHARS_OFFSET));
    return at(DIR_MASK_OFFSET);
}

static void poison_dir_buffer(void)
{
    for (uint32_t i = 0u; i < DIR_BUFFER_BYTES + 8u; i++) {
        (void)kernel_guest_write_u8(at(DIR_BUFFER_OFFSET) + i, SENTINEL_BYTE);
    }
}

/* The ten-argument shape measured at 0x00381AC8 / 0x003817CC / 0x00381B2F. */
static uint32_t query_directory_full(uint32_t handle, uint32_t event, uint32_t info_class,
                                     uint32_t length, kernel_guest_ptr mask, uint32_t restart)
{
    poison_dir_buffer();
    const uint32_t args[10] = {handle, event, 0u, 0u, at(IOSB_OFFSET), at(DIR_BUFFER_OFFSET),
                               length, info_class, mask, restart};
    return call_ordinal(ORD_NT_QUERY_DIRECTORY_FILE, args, 10u);
}

static uint32_t query_directory(uint32_t handle, kernel_guest_ptr mask, uint32_t restart)
{
    return query_directory_full(handle, 0u, 1u, DIR_BUFFER_BYTES, mask, restart);
}

static uint32_t dir_u32(uint32_t offset)
{
    return read_u32_at(at(DIR_BUFFER_OFFSET) + offset);
}

static bool dir_name_is(const char *expected)
{
    const size_t length = strlen(expected);
    if (dir_u32(0x3Cu) != (uint32_t)length) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        uint8_t byte = 0u;
        (void)kernel_guest_read_u8(at(DIR_BUFFER_OFFSET) + 0x40u + (uint32_t)i, &byte);
        if (byte != (uint8_t)expected[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t open_hdd_directory(const char *guest_path)
{
    if (open_path(guest_path) != STATUS_SUCCESS) {
        return 0u;
    }
    return issued_handle();
}

/*
 * The measured shape end to end on a real temp directory: class 1 FILE_DIRECTORY_INFORMATION,
 * one entry per call, ordered case-insensitively, real size/attributes/name, NextEntryOffset 0,
 * the name NOT NUL-terminated by us (the guest writes its own), IoStatusBlock information
 * 0x40+name, then NO_MORE_FILES, and a restart replays from the top.
 *
 * MUTATIONS: byte-order sort, skip the restart rewind, write the name length at another offset,
 * swap attributes, NUL-terminate the name, or return an index cursor and a check fails.
 */
static void test_query_directory_lists_real_entries_one_per_call(void)
{
    setup();
    mount_the_hdd();
    host_write_file("b.txt", "12345", 5u);
    host_write_file("A.xbx", "abc", 3u);
    host_make_dir("Dir");
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK(handle != 0u);

    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("A.xbx"));
    CHECK_EQ_U32(dir_u32(0x00u), 0u);
    CHECK_EQ_U32(dir_u32(0x28u), 3u);
    CHECK_EQ_U32(dir_u32(0x2Cu), 0u);
    CHECK_EQ_U32(dir_u32(0x38u), 0x20u);
    CHECK_EQ_U32(read_u32_at(at(DIR_BUFFER_OFFSET) + 0x45u), ((uint32_t)SENTINEL_BYTE) * 0x01010101u);
    CHECK_EQ_U32(iosb_status(), STATUS_SUCCESS);
    CHECK_EQ_U32(iosb_information(), 0x40u + 5u);

    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("b.txt"));
    CHECK_EQ_U32(dir_u32(0x28u), 5u);

    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("Dir"));
    CHECK_EQ_U32(dir_u32(0x38u), 0x10u);
    CHECK_EQ_U32(dir_u32(0x28u), 0u);

    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);
    CHECK_EQ_U32(iosb_status(), STATUS_NO_MORE_FILES_VALUE);
    CHECK_EQ_U32(kernel_io_directory_entry_count(), 3u);

    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("A.xbx"));
    teardown();
}

/*
 * The mask is fixed by the first call (site 2 passes the file component, sites 1 and 3 pass
 * none), a zero-length mask means everything (the guest turns "*.*" into one), and `*`/`?`
 * match case-insensitively. Empty first match is NO_SUCH_FILE.
 *
 * MUTATIONS: honour a mask on later calls, treat length 0 as match-nothing, or compare
 * case-sensitively and a check fails.
 */
static void test_query_directory_mask_semantics(void)
{
    setup();
    mount_the_hdd();
    host_write_file("one.XBX", "1", 1u);
    host_write_file("two.xbx", "22", 2u);
    host_write_file("three.txt", "333", 3u);
    uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("*.xbx"), 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("one.XBX"));
    /* A different mask on a later call is ignored: the first one stands. */
    CHECK_EQ_U32(query_directory(handle, build_mask("*.txt"), 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("two.xbx"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);

    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask(""), 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("one.XBX"));
    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("T?O.*"), 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("two.xbx"));
    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("nothing*"), 0u), STATUS_NO_SUCH_FILE_VALUE);
    teardown();
}

/*
 * The XapiNukeDirectory loop deletes each entry it was handed and then asks again with
 * restart 0 (site 1: the byte at [ebp-8] is 1 once, then 0). A cursor that is a position would
 * skip the survivor after each delete; ours is the last name returned.
 *
 * MUTATION: keep an index instead of the last name and "c" is never returned.
 */
static void test_query_directory_survives_deleting_the_returned_entry(void)
{
    setup();
    mount_the_hdd();
    host_write_file("a", "", 0u);
    host_write_file("b", "", 0u);
    host_write_file("c", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    const char *expected[3] = {"a", "b", "c"};
    for (unsigned i = 0u; i < 3u; i++) {
        CHECK_EQ_U32(query_directory(handle, 0u, i == 0u ? 1u : 0u), STATUS_SUCCESS);
        CHECK(dir_name_is(expected[i]));
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%s", host_root, expected[i]);
        CHECK(unlink(path) == 0);
    }
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);
    teardown();
}

/* An empty directory answers NO_SUCH_FILE first (INFERRED NT), also for a created directory. */
static void test_query_directory_on_an_empty_and_a_created_directory(void)
{
    setup();
    mount_the_hdd();
    host_make_dir("Empty");
    uint32_t handle = open_hdd_directory(HDD_DEVICE "\\Empty");
    CHECK(handle != 0u);
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_NO_SUCH_FILE_VALUE);

    CHECK_EQ_U32(create_path(HDD_DEVICE "\\Made", DISPOSITION_FILE_OPEN_IF,
                             MEASURED_CREATE_OPTIONS),
                 STATUS_SUCCESS);
    handle = issued_handle();
    host_write_file("Made/x.bin", "xy", 2u);
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("x.bin"));
    CHECK_EQ_U32(dir_u32(0x28u), 2u);
    teardown();
}

/*
 * Times are NT FILETIMEs from the host (100ns since 1601). A file stamped at the Unix epoch
 * plus 1000 s must read back as (1000 + 11644473600) * 10^7 in last-write.
 * MUTATION: drop the 1601 offset or the 10^7 scale and this fails.
 */
static void test_query_directory_reports_host_times_as_filetime(void)
{
    setup();
    mount_the_hdd();
    host_write_file("t.bin", "", 0u);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/t.bin", host_root);
    const struct timespec stamp[2] = {{2000, 0}, {1000, 0}};
    CHECK(utimensat(AT_FDCWD, path, stamp, 0) == 0);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    const uint64_t write_time = ((uint64_t)1000 + 11644473600ull) * 10000000ull;
    const uint64_t access_time = ((uint64_t)2000 + 11644473600ull) * 10000000ull;
    CHECK_EQ_U64(((uint64_t)dir_u32(0x1Cu) << 32) | dir_u32(0x18u), write_time);
    CHECK_EQ_U64(((uint64_t)dir_u32(0x14u) << 32) | dir_u32(0x10u), access_time);
    CHECK_EQ_U64(((uint64_t)dir_u32(0x0Cu) << 32) | dir_u32(0x08u), write_time);
    teardown();
}

/*
 * Everything outside the measured shape is refused loudly and writes nothing: Event or APC
 * set, a class other than 1, a regular-file handle, a bad handle, a Length below the header,
 * and a name that does not fit (cursor kept so a larger buffer then succeeds). Dot entries and
 * host symbolic links never appear.
 *
 * MUTATIONS: ignore Event, accept class 2, list a file handle, advance the cursor on a
 * too-small buffer, or show symlinks/dots and a check fails.
 */
static void test_query_directory_refuses_what_is_not_measured(void)
{
    setup();
    mount_the_hdd();
    host_write_file("longer-name.bin", "", 0u);
    char link_path[512];
    (void)snprintf(link_path, sizeof(link_path), "%s/zz-link", host_root);
    CHECK(symlink("/", link_path) == 0);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");

    CHECK_EQ_U32(query_directory_full(handle, 0x1234u, 1u, DIR_BUFFER_BYTES, 0u, 1u),
                 STATUS_NOT_IMPLEMENTED);
    CHECK(captured_contains("REFUSED"));
    CHECK_EQ_U32(query_directory_full(handle, 0u, 2u, DIR_BUFFER_BYTES, 0u, 1u), 0xC0000003u);
    CHECK_EQ_U32(dir_u32(0x3Cu), ((uint32_t)SENTINEL_BYTE) * 0x01010101u);
    CHECK_EQ_U32(query_directory_full(0xDEADBEEFu, 0u, 1u, DIR_BUFFER_BYTES, 0u, 1u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(query_directory_full(handle, 0u, 1u, 0x20u, 0u, 1u), 0xC0000004u);
    CHECK_EQ_U32(query_directory_full(handle, 0u, 1u, 0x40u + 3u, 0u, 1u), 0xC0000004u);
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("longer-name.bin"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);

    CHECK_EQ_U32(open_path(HDD_DEVICE "\\longer-name.bin"), STATUS_SUCCESS);
    CHECK_EQ_U32(query_directory(issued_handle(), 0u, 1u), STATUS_INVALID_PARAMETER);
    teardown();
}

/* The full ten-argument call with every argument chosen by the caller. */
static uint32_t query_directory_args(const uint32_t args[10])
{
    poison_dir_buffer();
    return call_ordinal(ORD_NT_QUERY_DIRECTORY_FILE, args, 10u);
}

static uint64_t dir_u64(uint32_t offset)
{
    return ((uint64_t)dir_u32(offset + 4u) << 32) | dir_u32(offset);
}

static uint64_t expected_filetime(const struct timespec *stamp)
{
    return ((uint64_t)stamp->tv_sec + 11644473600ull) * 10000000ull +
           (uint64_t)(stamp->tv_nsec / 100);
}

/*
 * Every argument the measured sites pass as zero is refused ON ITS OWN: ApcRoutine alone,
 * ApcContext alone (the Event-only case is in the test above), and a null FileInformation
 * buffer is a length mismatch that must not consume a directory entry.
 *
 * MUTATIONS: ignore ApcRoutine, ignore ApcContext, or drop the null-buffer test (the entry is
 * consumed, the write to address 0 fails, and the answer is INVALID_PARAMETER instead).
 */
static void test_query_directory_refuses_each_unmeasured_argument_alone(void)
{
    setup();
    mount_the_hdd();
    host_write_file("only.bin", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    uint32_t args[10] = {handle, 0u, 0u, 0u, at(IOSB_OFFSET), at(DIR_BUFFER_OFFSET),
                         DIR_BUFFER_BYTES, 1u, 0u, 1u};
    args[2] = 0x1234u;
    CHECK_EQ_U32(query_directory_args(args), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(iosb_status(), STATUS_NOT_IMPLEMENTED);
    args[2] = 0u;
    args[3] = 0x5678u;
    CHECK_EQ_U32(query_directory_args(args), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(iosb_status(), STATUS_NOT_IMPLEMENTED);
    args[3] = 0u;
    args[5] = 0u;
    CHECK_EQ_U32(query_directory_args(args), 0xC0000004u);
    CHECK_EQ_U32(iosb_status(), 0xC0000004u);
    CHECK_EQ_U32(kernel_io_directory_entry_count(), 0u);
    /* Nothing above consumed the entry: the first real call still returns it. */
    args[5] = at(DIR_BUFFER_OFFSET);
    CHECK_EQ_U32(query_directory_args(args), STATUS_SUCCESS);
    CHECK(dir_name_is("only.bin"));
    teardown();
}

/*
 * A FileName mask the guest cannot read is a parameter error, not "match everything".
 * MUTATION: ignore the failed mask read and the unreadable mask lists the whole directory.
 */
static void test_query_directory_refuses_an_unreadable_mask(void)
{
    setup();
    mount_the_hdd();
    host_write_file("only.bin", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0x2000u, 1u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(iosb_status(), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_io_directory_entry_count(), 0u);
    teardown();
}

/*
 * RestartScan is a BOOLEAN: only its low byte counts (the guest pushes a byte-sized flag, so
 * stale upper bits of the dword mean nothing). 0x100 continues the scan, 0x101 restarts it.
 * MUTATION: read the whole dword and 0x100 rewinds the scan to the first entry.
 */
static void test_query_directory_restart_is_the_low_byte_only(void)
{
    setup();
    mount_the_hdd();
    host_write_file("a", "", 0u);
    host_write_file("b", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("a"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0x100u), STATUS_SUCCESS);
    CHECK(dir_name_is("b"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0x101u), STATUS_SUCCESS);
    CHECK(dir_name_is("a"));
    teardown();
}

/*
 * The fixed fields of FILE_DIRECTORY_INFORMATION: NextEntryOffset and FileIndex are both 0,
 * change time follows last-write (not last-access), the FILETIME keeps the sub-second 100 ns
 * ticks, and allocation is the size rounded UP to a 4096 multiple (0 stays 0).
 *
 * MUTATIONS: a nonzero FileIndex, change time from the access time, a sub-second divisor other
 * than 100, allocation rounded down or by a whole extra page, and a check fails.
 */
static void test_query_directory_fixed_fields_times_and_allocation(void)
{
    setup();
    mount_the_hdd();
    static const struct {
        const char *name;
        size_t size;
        uint64_t allocation;
    } files[] = {
        {"f0", 0u, 0u}, {"f1", 1u, 4096u}, {"f4096", 4096u, 4096u}, {"f4097", 4097u, 8192u},
    };
    char *payload = calloc(1u, 4097u);
    CHECK(payload != NULL);
    for (size_t i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        host_write_file(files[i].name, payload, files[i].size);
    }
    free(payload);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/f1", host_root);
    const struct timespec stamp[2] = {{2000, 987654300L}, {1000, 123456700L}};
    CHECK(utimensat(AT_FDCWD, path, stamp, 0) == 0);
    struct stat applied;
    CHECK(stat(path, &applied) == 0);

    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    for (size_t i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        CHECK_EQ_U32(query_directory(handle, 0u, i == 0u ? 1u : 0u), STATUS_SUCCESS);
        CHECK(dir_name_is(files[i].name));
        CHECK_EQ_U32(dir_u32(0x00u), 0u);
        CHECK_EQ_U32(dir_u32(0x04u), 0u);
        CHECK_EQ_U64(dir_u64(0x28u), files[i].size);
        CHECK_EQ_U64(dir_u64(0x30u), files[i].allocation);
        if (i == 1u) {
            const uint64_t write_time = expected_filetime(&applied.st_mtim);
            CHECK(write_time != expected_filetime(&applied.st_atim));
            CHECK_EQ_U64(dir_u64(0x08u), write_time);
            CHECK_EQ_U64(dir_u64(0x10u), expected_filetime(&applied.st_atim));
            CHECK_EQ_U64(dir_u64(0x18u), write_time);
            CHECK_EQ_U64(dir_u64(0x20u), write_time);
        }
    }
    teardown();
}

/*
 * A name that EXACTLY fills the caller's buffer fits: Length = 0x40 + the name's byte count is
 * accepted, one byte less is a length mismatch that keeps the cursor.
 * MUTATION: refuse when the name equals the room left (`>` as `>=`).
 */
static void test_query_directory_a_name_that_exactly_fits_is_returned(void)
{
    setup();
    mount_the_hdd();
    host_write_file("exact", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory_full(handle, 0u, 1u, 0x40u + 4u, 0u, 1u), 0xC0000004u);
    CHECK_EQ_U32(query_directory_full(handle, 0u, 1u, 0x40u + 5u, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("exact"));
    CHECK_EQ_U32(iosb_information(), 0x40u + 5u);
    teardown();
}

/*
 * After the listing ran out and every entry was then deleted, a RESTART begins a fresh scan,
 * so its empty answer is NO_SUCH_FILE (first call, nothing matches), not NO_MORE_FILES.
 * MUTATION: keep the "something was already returned" flag across a restart.
 */
static void test_query_directory_restart_forgets_the_cursor_flag(void)
{
    setup();
    mount_the_hdd();
    host_write_file("gone", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/gone", host_root);
    CHECK(unlink(path) == 0);
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_NO_SUCH_FILE_VALUE);
    teardown();
}

/*
 * Wildcard corner cases of the host mask matcher: the DOS "*.*" means EVERYTHING (a name with
 * no dot included), a trailing `*` matches the empty remainder ("abc*" finds "abc"), and the
 * ASCII fold covers the whole alphabet including 'Z' (mask "zebra" finds "Zebra", and "apple"
 * sorts before "Zebra").
 *
 * MUTATIONS: treat "*.*" as a plain wildcard, stop skipping a trailing `*`, or end the fold
 * range at 'Y'.
 */
static void test_query_directory_wildcard_corners(void)
{
    setup();
    mount_the_hdd();
    host_write_file("noext", "", 0u);
    host_write_file("abc", "", 0u);
    host_write_file("apple", "", 0u);
    host_write_file("Zebra", "", 0u);
    uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("*.*"), 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("abc"));
    unsigned listed = 1u;
    while (query_directory(handle, 0u, 0u) == STATUS_SUCCESS) {
        listed++;
    }
    CHECK_EQ_U32(listed, 4u);

    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("abc*"), 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("abc"));

    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("zebra"), 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("Zebra"));

    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, build_mask("ap*"), 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("apple"));
    handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("abc"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("apple"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("noext"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("Zebra"));
    teardown();
}

/*
 * Two names that differ ONLY in case are two entries, ordered by byte value after the fold ties
 * ("NAME.txt" before "name.txt"), and both are listed.
 * MUTATION: compare them equal and the cursor skips the second one.
 */
static void test_query_directory_case_only_different_names_are_both_listed(void)
{
    setup();
    mount_the_hdd();
    host_write_file("name.txt", "", 0u);
    host_write_file("NAME.txt", "", 0u);
    const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
    CHECK(dir_name_is("NAME.txt"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_SUCCESS);
    CHECK(dir_name_is("name.txt"));
    CHECK_EQ_U32(query_directory(handle, 0u, 0u), STATUS_NO_MORE_FILES_VALUE);
    teardown();
}

/*
 * A listed DIRECTORY reports an end-of-file of 0, never the host's own st_size for the directory
 * inode. A directory inode has a nonzero st_size only on some filesystems (ext4 4096, tmpfs
 * 40, overlayfs and several others 0), so the shared scratch directory cannot prove this on every
 * host. This test therefore lists a directory on /dev/shm (tmpfs), and says so and skips when
 * the host has no tmpfs there or its directories report size 0 anyway.
 *
 * MUTATION: report best_info.st_size for a directory.
 */
static void test_query_directory_reports_a_directory_size_of_zero_on_a_sized_filesystem(void)
{
    setup();
    char shm_root[] = "/dev/shm/tsfp-dirsize-XXXXXX";
    if (mkdtemp(shm_root) == NULL) {
        printf("  SKIP directory st_size check: no writable /dev/shm (%s)\n", strerror(errno));
        teardown();
        return;
    }
    char sub[256];
    (void)snprintf(sub, sizeof(sub), "%s/Sub", shm_root);
    struct stat inode;
    const bool sized = mkdir(sub, 0777) == 0 && stat(sub, &inode) == 0 && inode.st_size != 0;
    if (!sized) {
        printf("  SKIP directory st_size check: /dev/shm directories report size 0\n");
    } else {
        CHECK(kernel_file_mount_host_dir(HDD_DEVICE, shm_root));
        const uint32_t handle = open_hdd_directory(HDD_DEVICE "\\");
        CHECK(handle != 0u);
        CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_SUCCESS);
        CHECK(dir_name_is("Sub"));
        CHECK_EQ_U32(dir_u32(0x38u), 0x10u);
        CHECK_EQ_U64(dir_u64(0x28u), 0u);
        CHECK_EQ_U64(dir_u64(0x30u), 0u);
    }
    (void)rmdir(sub);
    (void)rmdir(shm_root);
    teardown();
}

/*
 * A DISC directory is NOT listed yet (only --hdd directories are measured and tested): the
 * answer is NOT_IMPLEMENTED, loudly, never an empty listing a title would read as "no files".
 * MUTATION: drop the HOST_DIR backing guard in kernel_file_dir_next and this fails.
 */
static void test_query_directory_on_a_disc_is_refused_not_answered_empty(void)
{
    setup();
    write_synthetic_disc("synthetic.iso");
    char image_path[512];
    (void)snprintf(image_path, sizeof(image_path), "%s/synthetic.iso", host_root);
    CHECK(kernel_file_mount_disc(DISC_DEVICE, image_path));
    const uint32_t handle = open_hdd_directory(DISC_DEVICE "\\");
    CHECK(handle != 0u);
    CHECK_EQ_U32(query_directory(handle, 0u, 1u), STATUS_NOT_IMPLEMENTED);
    CHECK(captured_contains("NOT IMPLEMENTED"));
    CHECK_EQ_U32(kernel_io_directory_entry_count(), 0u);
    teardown();
}

int main(void)
{
    test_inferred_drive_root_policy();
    printf("writable hard-disk backing store (--hdd) tests\n");

    char working_directory[PATH_MAX];
    CHECK(getcwd(working_directory, sizeof(working_directory)) != NULL);
    const unsigned strays_before = count_scratch_entries(working_directory);
    CHECK(find_binary_directory());
    const unsigned strays_before_binary = count_scratch_entries(binary_directory);

    test_without_a_mount_the_create_fails_exactly_as_before();
    test_the_mount_refuses_what_it_cannot_back();
    test_both_mounts_refuse_a_trailing_separator();
    test_open_if_with_the_directory_bit_creates_a_real_directory();
    test_a_second_open_if_opens_the_existing_directory();
    test_file_create_on_an_existing_name_is_a_collision();
    test_the_measured_two_step_create_works_and_parents_are_not_invented();
    test_a_path_that_tries_to_climb_out_is_refused();
    test_a_host_symbolic_link_is_refused_rather_than_followed();
    test_a_create_on_a_disc_volume_changes_not_one_byte();
    test_a_real_file_opens_with_its_real_size_and_serves_real_bytes();
    test_reading_a_directory_is_refused_rather_than_answered_empty();
    test_a_created_name_is_found_again_under_any_casing();
    test_the_volume_root_itself_opens();
    test_a_missing_name_on_a_writable_volume_is_not_fabricated();
    test_a_non_creating_disposition_creates_nothing();
    test_a_create_with_no_option_bit_makes_a_file();

    test_query_directory_lists_real_entries_one_per_call();
    test_query_directory_mask_semantics();
    test_query_directory_survives_deleting_the_returned_entry();
    test_query_directory_on_an_empty_and_a_created_directory();
    test_query_directory_reports_host_times_as_filetime();
    test_query_directory_refuses_what_is_not_measured();
    test_query_directory_refuses_each_unmeasured_argument_alone();
    test_query_directory_refuses_an_unreadable_mask();
    test_query_directory_restart_is_the_low_byte_only();
    test_query_directory_fixed_fields_times_and_allocation();
    test_query_directory_a_name_that_exactly_fits_is_returned();
    test_query_directory_restart_forgets_the_cursor_flag();
    test_query_directory_wildcard_corners();
    test_query_directory_case_only_different_names_are_both_listed();
    test_query_directory_reports_a_directory_size_of_zero_on_a_sized_filesystem();
    test_query_directory_on_a_disc_is_refused_not_answered_empty();

    test_the_measured_create_and_write_put_real_bytes_in_a_real_file();
    test_no_descriptor_on_a_disc_image_is_writable();
    test_a_write_larger_than_the_staging_buffer_lands_intact();
    test_two_implicit_writes_continue_rather_than_restart();
    test_an_explicit_byte_offset_writes_there_and_does_not_move_the_cursor();
    test_a_write_to_a_disc_backed_handle_changes_not_one_byte();
    test_a_write_with_no_writable_volume_is_refused_not_absorbed();
    test_a_write_through_a_read_only_handle_is_refused();
    test_a_write_to_a_directory_handle_is_refused();
    test_a_zero_length_write_still_goes_through_the_gate();
    test_an_overwriting_disposition_truncates_and_reports_it();
    test_an_overwriting_disposition_on_a_disc_is_refused();
    test_setting_the_end_of_file_truncates_for_real();

    test_reset_gives_back_the_writable_volume_and_its_counters();
    test_the_titles_own_drive_letter_resolves_onto_the_volume();

    test_partition0_is_refused_unless_the_device_was_mounted();
    test_the_device_opens_by_its_exact_name_and_nothing_inside_it();
    test_a_fresh_device_reads_as_announced_zeros();
    test_a_write_persists_in_the_backing_file_and_reads_back();
    test_a_second_mount_sees_what_the_first_run_wrote();
    test_reads_and_writes_stop_at_the_capacity();
    test_end_of_file_and_overwrite_cannot_truncate_the_device();
    test_a_vanished_backing_file_does_not_become_a_creation();
    test_the_device_mount_refuses_what_is_not_a_regular_file();

    test_a_cache_partition_is_a_raw_device_by_name_and_not_a_directory_until_formatted();
    test_the_titles_format_sequence_is_answered_and_unlocks_the_directory_view();
    test_files_in_the_directory_view_are_real_files_and_not_the_image();
    test_the_formatted_gate_is_not_latched();
    test_a_second_mount_finds_the_partition_already_formatted();
    test_control_requests_are_refused_unless_they_are_derived();
    test_the_cache_partition_mount_refuses_what_it_cannot_back();
    test_a_symbolic_link_in_place_of_the_directory_view_is_refused();
    test_formatting_one_cache_partition_does_not_format_its_siblings();

    test_a_fatal_exit_still_removes_the_scratch_directory();
    test_the_scratch_directory_is_beside_the_binary();
    /* Whole-run balance in the working directory, which is where the leak was. */
    CHECK_EQ_U32(count_scratch_entries(working_directory), strays_before);
    CHECK_EQ_U32(count_scratch_entries(binary_directory), strays_before_binary);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
