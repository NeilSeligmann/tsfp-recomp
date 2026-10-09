/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_file.h for the resolved ordinal number, the hand-verified arity, the
 * argument order read off the reached call site, and why there is no file system
 * behind this yet.
 */

#include "kernel_file.h"
#include <sys/statvfs.h>

/* No feature-test macro here, for the reason xdvdfs.c gives: CMakeLists.txt already
 * compiles `tsfp_xbox` with `_DEFAULT_SOURCE`, which on glibc implies
 * _POSIX_C_SOURCE 200809L and so declares `openat`, `mkdirat`, `fstatat` and
 * `O_NOFOLLOW`. Defining one here too would be a second place to keep in step. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/falloc.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"
#include "xdvdfs.h"
#include "../input/mu_fatx.h"

#define ORD_NtOpenFile 202u
#define ORD_NtCreateFile 190u
#define ORD_NtDeleteFile 195u
#define ORD_IoCreateSymbolicLink 67u
#define ORD_IoDeleteSymbolicLink 69u
#define ORD_NtOpenSymbolicLinkObject 203u
#define ORD_NtQuerySymbolicLinkObject 215u

/*
 * IO_STATUS_BLOCK.information after a successful open.
 *
 * INFERRED, NOT MEASURED, and labelled as such. The real kernel reports which of
 * create/open/overwrite happened here, and `guest_structs.h` records one guest site
 * (0x0037D394) comparing the field against 3 -- but that is on the NtCreateFile path,
 * not this one, and no measured NtOpenFile site in this image reads the field at all.
 * 1 is the conventional "the file was opened" value. If a site is ever found that
 * branches on it after an NtOpenFile, that site is the evidence and this is the thing
 * to replace.
 */
#define FILE_INFORMATION_OPENED 1u

/*
 * The other two IO_STATUS_BLOCK.information values this module can now produce.
 *
 * NT's disposition-result codes. FILE_CREATED is 2 and FILE_EXISTS is 4, and both are
 * now reachable because NtCreateFile can genuinely create. Reporting "opened" after a
 * create would tell the title its data was already there, which is the one lie that
 * matters on a volume holding a save game.
 *
 * NOT MEASURED IN THIS IMAGE, and flagged as such. `guest_structs.h` records exactly one
 * guest site reading this field, 0x0037D394, comparing it against 3. 3 is NT's
 * FILE_OVERWRITTEN and is not a value this module produces, so that site's branch is not
 * yet understood; what IS clear is that it reads the field, so the field has to carry the
 * right answer rather than a constant.
 */
#define FILE_INFORMATION_CREATED 2u
#define FILE_INFORMATION_EXISTS 4u

/*
 * The two an OVERWRITING disposition produces.
 *
 * FILE_SUPERSEDED is 0, which collides numerically with "nothing to report" -- said out
 * loud because it looks like a missing value and is not. NT uses 0 for it.
 *
 * FILE_OVERWRITTEN being 3 CLOSES A GAP THIS MODULE RECORDED AS OPEN. The comment above
 * says guest site 0x0037D394 compares information against 3 and that "that site's branch
 * is not yet understood". It is understood now: Win32 `CreateFile` sets
 * ERROR_ALREADY_EXISTS when an NtCreateFile reports FILE_OVERWRITTEN or FILE_SUPERSEDED,
 * which is documented behaviour for CREATE_ALWAYS. So 3 is reachable, and it is reachable
 * only from the truncating path below.
 */
#define FILE_INFORMATION_SUPERSEDED 0u
#define FILE_INFORMATION_OVERWRITTEN 3u

/*
 * ACCESS_MASK bits that mean "this handle will be written through".
 *
 * MEASURED IN THIS IMAGE: the only create that asks for write on a file is 0x00381027
 * (and its twin 0x00380DBE), both pushing DesiredAccess 0x40100000 -- GENERIC_WRITE
 * (0x40000000) | SYNCHRONIZE (0x00100000). So GENERIC_WRITE is the bit this boot actually
 * uses, and the rest are included because NT grants write through them too and a handle
 * that was granted write and then refused it would stop a run for no reason.
 *
 * MAXIMUM_ALLOWED is in the set deliberately: it means "whatever you can give me", and on
 * a volume the operator explicitly made writable the honest answer to that is write.
 */
#define FILE_WRITE_DATA 0x00000002u
#define FILE_APPEND_DATA 0x00000004u
#define GENERIC_WRITE_BIT 0x40000000u
#define GENERIC_ALL_BIT 0x10000000u
#define MAXIMUM_ALLOWED_BIT 0x02000000u

/*
 * NtCreateFile's CreateDisposition values, and the two CreateOptions bits read here.
 *
 * MEASURED IN THIS IMAGE: disposition 2 at 0x00380127 and disposition 3 at 0x00381321,
 * both with CreateOptions 0x4021. 0x4021 has bit 0 set, which is FILE_DIRECTORY_FILE,
 * and that bit is what decides between `mkdirat` and `openat(O_CREAT)` below. The other
 * four dispositions are named because the switch has to be total: a disposition this
 * module did not recognise must be refused with a diagnosis, not treated as the nearest
 * one it knows.
 */
#define FILE_SUPERSEDE 0u
#define FILE_OPEN 1u
#define FILE_CREATE 2u
#define FILE_OPEN_IF 3u
#define FILE_OVERWRITE 4u
#define FILE_OVERWRITE_IF 5u
#define FILE_DIRECTORY_FILE 0x00000001u
#define FILE_NON_DIRECTORY_FILE 0x00000040u

/*
 * Does this ACCESS_MASK ask to write?
 *
 * ONE PLACE, because the answer decides both the descriptor's mode at open time and
 * whether an overwriting disposition may truncate, and two copies of it could disagree --
 * which would produce a handle that was opened read-only and then truncated, or the
 * reverse.
 */
static bool access_wants_write(uint32_t desired_access)
{
    return (desired_access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE_BIT |
                              GENERIC_ALL_BIT | MAXIMUM_ALLOWED_BIT)) != 0u;
}

/* How many names can be declared openable, and how many distinct attempts recorded.
 * 13 call sites cannot produce an unbounded number of either in a startup path, and
 * both bounds are REPORTED when reached rather than silently dropping work. */
#define KERNEL_FILE_OPENABLE_MAX 32u
#define KERNEL_FILE_ATTEMPT_MAX 64u

/*
 * THE LOCK. Two guest threads run and either can open a file. Claiming a slot in
 * either table is a read-modify-write, so without it two threads can claim the same
 * slot and one attempt vanishes from the record -- and the record is this module's
 * entire output. RECURSIVE for the reason kernel_object.c, kernel_pool.c,
 * kernel_hal.c and kernel_config.c give: the critical sections call
 * kernel_hle_log(), whose sink is caller-supplied.
 */
static pthread_mutex_t file_lock;
static bool file_lock_ready;
static pthread_once_t file_lock_once = PTHREAD_ONCE_INIT;

static void file_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&file_lock, &attr) == 0) {
        file_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&file_lock_once, file_lock_init);
    if (file_lock_ready) {
        (void)pthread_mutex_lock(&file_lock);
    }
}

static void unlock(void)
{
    if (file_lock_ready) {
        (void)pthread_mutex_unlock(&file_lock);
    }
}

typedef struct {
    char path[KERNEL_FILE_PATH_MAX];
    bool in_use;
} openable_entry;

/* Declared up here, with the other module state, rather than beside the functions that
 * use them: `kernel_file_reset` sits near the top of this file and has to clear all of
 * them, and a reset that silently missed a table would leak one test case's mounts into
 * the next. */
/*
 * EVERY DESCRIPTOR IN THIS MODULE IS STORED BIASED BY ONE, so that 0 means "none".
 *
 * Not a style choice. Slots here are created, reclaimed and reset with `memset(0)` in
 * six different places, and 0 is a perfectly valid file descriptor -- it is stdin. A
 * -1 sentinel would have to be written by hand at each of those six sites, and the one
 * that was missed would `close(0)` and then `pread` the terminal. Biasing makes a zeroed
 * slot mean exactly what every memset here intends it to mean.
 */
typedef struct {
    char prefix[KERNEL_FILE_PATH_MAX];
    kernel_file_backing backing;
    /* Valid only when backing is DISC. One reader per volume: the handle owns the file
     * descriptor, so unmounting is what closes it. */
    xdvdfs_reader disc;
    /* Valid only when backing is HOST_DIR. The host directory's own path, for
     * DIAGNOSTICS ONLY -- no guest path is ever joined onto it. */
    char host_root[KERNEL_FILE_HOST_PATH_MAX];
    /* Valid only when backing is HOST_DIR. An open descriptor on the backing directory,
     * biased by one. THIS, not `host_root`, is what a path walk starts from: the walk
     * can then only ever reach things reachable from the directory that was validated,
     * even if something replaces that directory's name underneath us. */
    int root_fd_plus_one;
    /* Non-empty only for a VIRTUAL RAW DEVICE (see kernel_file_mount_host_device): the
     * name of the one regular file under the root descriptor that IS the device, and the
     * byte length the device claims. Empty and 0 for every directory volume, which is what
     * a zeroed slot means. */
    char device_file[KERNEL_FILE_DEVICE_NAME_MAX + 1u];
    uint64_t device_capacity;
    /* Non-zero only for a FORMATTABLE CACHE PARTITION (see kernel_file_mount_cache_partition):
     * the N of `PartitionN`. Such a volume is a raw device for its exact name and a host
     * directory for anything under it, so `device_file` alone no longer says which view a
     * path wants. */
    unsigned cache_partition;
    /* Cache partition only. The directory view's name under `root_fd`, and its descriptor
     * (biased by one, 0 until the first open after the title formats the image). */
    char content_dir[KERNEL_FILE_DEVICE_NAME_MAX + 1u];
    int content_fd_plus_one;
    fatx_volume *fatx;
    uint64_t generation;
    bool (*flush_image)(void *);
    void *flush_context;
    bool in_use;
} volume_entry;
static uint64_t fatx_generation;

typedef struct {
    char name[KERNEL_FILE_PATH_MAX];
    char target[KERNEL_FILE_PATH_MAX];
    bool in_use;
} symlink_entry;

/* One live NtOpenSymbolicLinkObject handle. The TARGET IS COPIED at open time, because a
 * handle names the link object and not its name: IoDeleteSymbolicLink after the open must
 * not change what a query through the handle returns. */
typedef struct {
    uint32_t handle;
    char target[KERNEL_FILE_PATH_MAX];
    bool in_use;
} symlink_handle_entry;

typedef struct {
    kernel_file_open state;
    uint64_t generation;
    char fatx_path[KERNEL_FILE_PATH_MAX];
    /* Which volume backs it, so a read does not have to re-resolve the path. */
    unsigned volume_index;
    /* The host descriptor for a HOST_DIR-backed regular file, biased by one. Opened at
     * open time and carried, for the same reason the disc's sector is carried: a read
     * must not be able to disagree with the open that produced it. 0 for a directory and
     * for every other backing. */
    int host_fd_plus_one;
    /* A HOST_DIR directory's descriptor, biased by one, opened with O_NOFOLLOW at open
     * time so NtQueryDirectoryFile lists the very directory the open resolved. 0 for every
     * other handle. */
    int dir_fd_plus_one;
    /* NtQueryDirectoryFile enumeration state. The cursor is the LAST NAME returned, not an
     * index, so entries the guest deletes mid-listing (the XapiNukeDirectory loop does)
     * cannot make the next call skip a survivor. The mask is fixed by the first call. */
    bool dir_started;
    bool dir_has_last;
    char dir_last[KERNEL_FILE_DIR_NAME_MAX + 1u];
    char dir_mask[KERNEL_FILE_DIR_NAME_MAX + 1u];
    bool in_use;
} open_entry;

static volume_entry volumes[KERNEL_FILE_VOLUME_MAX];
static symlink_entry symlinks[KERNEL_FILE_SYMLINK_MAX];
static symlink_handle_entry symlink_handles[KERNEL_FILE_SYMLINK_HANDLE_MAX];
static open_entry open_files[KERNEL_FILE_OPEN_MAX];
static uint64_t disc_bytes_read;
static unsigned disc_opened_count;
static uint64_t host_bytes_read;
static uint64_t host_bytes_written;
static unsigned host_opened_count;
static unsigned created_count;
static unsigned escape_refused_count;
static unsigned write_refused_count;
static unsigned device_opened_count;
static unsigned cache_view_opened_count;
static uint64_t device_zero_bytes;

static openable_entry openable[KERNEL_FILE_OPENABLE_MAX];
static kernel_file_attempt attempts[KERNEL_FILE_ATTEMPT_MAX];
static unsigned attempt_count;
static kernel_file_missing_policy missing_policy = KERNEL_FILE_MISSING_FAIL;
static unsigned refused_count;
static unsigned fabricated_count;
static unsigned relative_refused_count;
static bool inferred_drive_root_announced;
static unsigned unmodelled_attributes_count;
static unsigned attribute_query_count;
static unsigned deleted_count;
static unsigned delete_refused_count;

/* Case-insensitive compare over ASCII only. Deliberately not `strcasecmp`: that is
 * locale-dependent, and a guest device path must not start matching differently
 * because the host's locale changed. */
static bool paths_equal(const char *a, const char *b)
{
    size_t i = 0u;
    for (;; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca + ('a' - 'A'));
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb + ('a' - 'A'));
        }
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
}

/* Close whatever this slot owns, then clear it. Callers hold the lock. The ONLY way a
 * slot is released, so there is one place a descriptor can leak from rather than six. */
static void release_open_slot_locked(open_entry *slot)
{
    if (slot->host_fd_plus_one != 0) {
        (void)close(slot->host_fd_plus_one - 1);
    }
    if (slot->dir_fd_plus_one != 0) {
        (void)close(slot->dir_fd_plus_one - 1);
    }
    memset(slot, 0, sizeof(*slot));
}

void kernel_file_reset(void)
{
    /* Unmounts first, and that ORDER MATTERS: it closes the disc image's file
     * descriptor. Zeroing the volume table without closing would leak one descriptor per
     * reset, which a test suite that resets between cases would hit quickly. */
    kernel_file_unmount_all();
    lock();
    memset(openable, 0, sizeof(openable));
    memset(attempts, 0, sizeof(attempts));
    memset(symlinks, 0, sizeof(symlinks));
    memset(symlink_handles, 0, sizeof(symlink_handles));
    /* Per slot rather than one memset over the table: an open file may own a host
     * descriptor, and a table-wide memset would forget it was ever opened. */
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        release_open_slot_locked(&open_files[i]);
    }
    attempt_count = 0u;
    missing_policy = KERNEL_FILE_MISSING_FAIL;
    refused_count = 0u;
    fabricated_count = 0u;
    relative_refused_count = 0u;
    inferred_drive_root_announced = false;
    unmodelled_attributes_count = 0u;
    attribute_query_count = 0u;
    deleted_count = 0u;
    delete_refused_count = 0u;
    disc_bytes_read = 0u;
    disc_opened_count = 0u;
    host_bytes_read = 0u;
    host_bytes_written = 0u;
    host_opened_count = 0u;
    created_count = 0u;
    escape_refused_count = 0u;
    write_refused_count = 0u;
    cache_view_opened_count = 0u;
    device_opened_count = 0u;
    device_zero_bytes = 0u;
    unlock();
}

void kernel_file_set_missing_policy(kernel_file_missing_policy policy)
{
    lock();
    missing_policy = policy;
    unlock();
}

bool kernel_file_add_openable(const char *path)
{
    if (!path) {
        return false;
    }
    const size_t length = strlen(path);
    if (length == 0u || length >= KERNEL_FILE_PATH_MAX) {
        return false;
    }
    lock();
    openable_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_OPENABLE_MAX; i++) {
        if (openable[i].in_use && paths_equal(openable[i].path, path)) {
            slot = &openable[i];
            break;
        }
        if (!openable[i].in_use && !slot) {
            slot = &openable[i];
        }
    }
    if (!slot) {
        unlock();
        return false;
    }
    memcpy(slot->path, path, length + 1u);
    slot->in_use = true;
    unlock();
    return true;
}

unsigned kernel_file_openable_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_OPENABLE_MAX; i++) {
        if (openable[i].in_use) {
            count++;
        }
    }
    unlock();
    return count;
}

unsigned kernel_file_attempt_count(void)
{
    lock();
    const unsigned count = attempt_count;
    unlock();
    return count;
}

const kernel_file_attempt *kernel_file_attempt_at(unsigned index)
{
    if (index >= KERNEL_FILE_ATTEMPT_MAX) {
        return NULL;
    }
    lock();
    const bool valid = index < attempt_count;
    unlock();
    /* The array is write-once per slot and never moves, so returning a pointer into
     * it is safe without holding the lock. Returning a copy would be safer still, but
     * the caller is a reporter that wants the whole record, and an out-parameter
     * struct copy per entry buys nothing here. */
    return valid ? &attempts[index] : NULL;
}

unsigned kernel_file_refused_count(void)
{
    lock();
    const unsigned count = refused_count;
    unlock();
    return count;
}

unsigned kernel_file_fabricated_count(void)
{
    lock();
    const unsigned count = fabricated_count;
    unlock();
    return count;
}

unsigned kernel_file_relative_refused_count(void)
{
    lock();
    const unsigned count = relative_refused_count;
    unlock();
    return count;
}

unsigned kernel_file_attribute_query_count(void)
{
    lock();
    const unsigned count = attribute_query_count;
    unlock();
    return count;
}

unsigned kernel_file_deleted_count(void)
{
    lock();
    const unsigned count = deleted_count;
    unlock();
    return count;
}

unsigned kernel_file_delete_refused_count(void)
{
    lock();
    const unsigned count = delete_refused_count;
    unlock();
    return count;
}

unsigned kernel_file_unmodelled_attributes_count(void)
{
    lock();
    const unsigned count = unmodelled_attributes_count;
    unlock();
    return count;
}

/* Callers hold the lock. Returns the recorded attempt for `path`, creating it on
 * first sight. NULL when the record is full, which is reported by the caller. */
static kernel_file_attempt *record_attempt_locked(const char *path, uint32_t access,
                                                  uint32_t share, uint32_t options)
{
    for (unsigned i = 0u; i < attempt_count && i < KERNEL_FILE_ATTEMPT_MAX; i++) {
        if (paths_equal(attempts[i].path, path)) {
            attempts[i].attempts++;
            return &attempts[i];
        }
    }
    if (attempt_count >= KERNEL_FILE_ATTEMPT_MAX) {
        return NULL;
    }
    kernel_file_attempt *entry = &attempts[attempt_count];
    const size_t length = strlen(path);
    memcpy(entry->path, path, length + 1u);
    entry->desired_access = access;
    entry->share_access = share;
    entry->open_options = options;
    entry->attempts = 1u;
    entry->opened = false;
    attempt_count++;
    return entry;
}

/* Callers hold the lock. */
static bool is_openable_locked(const char *path)
{
    for (unsigned i = 0u; i < KERNEL_FILE_OPENABLE_MAX; i++) {
        if (openable[i].in_use && paths_equal(openable[i].path, path)) {
            return true;
        }
    }
    return false;
}

/* ===================== volumes, symlinks and open files =================== */

/* True when `text` starts with `prefix`, compared case-insensitively over ASCII. The
 * guest spells its own device names inconsistently -- `\Device\Cdrom0` at 0x004A13F0
 * and `\Device\CdRom0` at 0x004A19E4 are the same device -- so a case-sensitive prefix
 * match would miss names this very executable uses. Returns the matched length. */
static size_t prefix_match_length(const char *text, const char *prefix)
{
    size_t i = 0u;
    for (;; i++) {
        unsigned char cp = (unsigned char)prefix[i];
        if (cp == '\0') {
            return i;
        }
        unsigned char ct = (unsigned char)text[i];
        if (ct == '\0') {
            return 0u;
        }
        if (cp >= 'A' && cp <= 'Z') {
            cp = (unsigned char)(cp + ('a' - 'A'));
        }
        if (ct >= 'A' && ct <= 'Z') {
            ct = (unsigned char)(ct + ('a' - 'A'));
        }
        /* `\` and `/` are the same separator. The guest MIXES THEM IN ONE STRING: it
         * builds `<letter>:\` with a `%c:\` format and then joins a forward-slash
         * relative asset path onto it, so the kernel is handed names shaped like
         * `d:\pak/arcade/l_103.pak`. Treating them as distinct would fail to match a
         * prefix the title genuinely used. */
        if (cp == '\\') {
            cp = '/';
        }
        if (ct == '\\') {
            ct = '/';
        }
        if (cp != ct) {
            return 0u;
        }
    }
}

/* Callers hold the lock. */
static const char *symlink_target_locked(const char *name)
{
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_MAX; i++) {
        if (symlinks[i].in_use && paths_equal(symlinks[i].name, name)) {
            return symlinks[i].target;
        }
    }
    return NULL;
}

/*
 * Rewrite a leading symbolic link, repeatedly, into `out`.
 *
 * The guest's own links are what make `D:` mean the disc, so this is the step that
 * turns a title-visible name into a device name. A link matches only as a WHOLE
 * leading component -- `\??\D:` rewrites the head of `\??\D:\pak\chr.pak` and leaves
 * the remainder alone -- because a substring match would corrupt any name that merely
 * contained a drive letter.
 *
 * Bounded by KERNEL_FILE_SYMLINK_DEPTH_MAX and REPORTS exhaustion: the guest can point
 * a link at itself and an unbounded loop would hang rather than diagnose.
 * Callers hold the lock.
 */
static bool resolve_symlinks_locked(const char *path, char *out, size_t out_bytes)
{
    if (strlen(path) >= out_bytes) {
        return false;
    }
    memcpy(out, path, strlen(path) + 1u);

    for (unsigned depth = 0u; depth < KERNEL_FILE_SYMLINK_DEPTH_MAX; depth++) {
        bool rewrote = false;
        for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_MAX; i++) {
            if (!symlinks[i].in_use) {
                continue;
            }
            const size_t matched = prefix_match_length(out, symlinks[i].name);
            if (matched == 0u) {
                continue;
            }
            /* The link must cover a whole component: the next character has to be a
             * separator or the end of the string. Without this, a link named `\??\D`
             * would match inside `\??\DATA`. */
            const char after = out[matched];
            if (after != '\0' && after != '\\' && after != '/') {
                continue;
            }
            const size_t target_length = strlen(symlinks[i].target);
            const size_t tail_length = strlen(out + matched);
            if (target_length + tail_length >= out_bytes) {
                return false;
            }
            char rebuilt[KERNEL_FILE_PATH_MAX];
            memcpy(rebuilt, symlinks[i].target, target_length);
            memcpy(rebuilt + target_length, out + matched, tail_length + 1u);
            memcpy(out, rebuilt, target_length + tail_length + 1u);
            rewrote = true;
            break;
        }
        if (!rewrote) {
            return true;
        }
    }
    kernel_hle_log()("kernel: resolving \"%s\" followed %u symbolic links without "
                     "settling -- REFUSED, the title's own link table may be cyclic\n",
                     path, KERNEL_FILE_SYMLINK_DEPTH_MAX);
    return false;
}

/* Callers hold the lock. Longest matching mount prefix, or NULL. `*out_rest` points at
 * the remainder of `path` inside the volume. Longest rather than first so that mounting
 * both a device and a subdirectory of it resolves to the more specific one. */
static volume_entry *volume_for_locked(const char *path, const char **out_rest)
{
    volume_entry *best = NULL;
    size_t best_length = 0u;
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (!volumes[i].in_use) {
            continue;
        }
        const size_t matched = prefix_match_length(path, volumes[i].prefix);
        if (matched == 0u || matched < best_length) {
            continue;
        }
        const char after = path[matched];
        if (after != '\0' && after != '\\' && after != '/') {
            continue;
        }
        best = &volumes[i];
        best_length = matched;
    }
    if (best && out_rest) {
        *out_rest = path + best_length;
    }
    return best;
}

/* ================= the writable host-directory backing store ============== */

/*
 * Is this one guest path component refused outright?
 *
 * `..` is the escape, and `.` is refused with it rather than silently skipped: skipping
 * it would be this module quietly rewriting a name the guest chose, and a rewrite that
 * is right 100 times is impossible to notice the 101st time it is not. Neither spelling
 * appears in any measured path in this image, so refusing both costs nothing real.
 *
 * `length` is passed rather than relying on a terminator because the caller has a slice
 * of a longer string and has not copied it out yet.
 */
static bool component_is_refused(const char *component, size_t length)
{
    if (length == 1u && component[0] == '.') {
        return true;
    }
    return length == 2u && component[0] == '.' && component[1] == '.';
}

/*
 * Find the spelling `wanted` actually has inside `dir_fd`, comparing case-insensitively.
 *
 * WHY A SCAN IS NEEDED AT ALL. The Xbox's own filesystem is FATX, which is
 * case-INSENSITIVE and case-PRESERVING; a Linux host filesystem is neither. This module
 * already matches guest paths case-insensitively everywhere, for the measured reason that
 * the binary spells its own names two ways -- both `partition1` and `Partition1`, both
 * `\Device\Cdrom0` and `\Device\CdRom0`. If a directory this host created as `TDATA` were
 * only findable as `TDATA`, a later open spelled `tdata` would come back absent, and
 * "that file is not there" is a wrong answer that looks exactly like a right one.
 *
 * The scan is the SLOW PATH only. The caller probes the exact spelling first, so a run
 * where the title is consistent pays nothing for this.
 *
 * Callers hold the lock.
 */
static bool hostdir_match_case_locked(int dir_fd, const char *wanted, char *out,
                                      size_t out_bytes)
{
    /*
     * A FRESH DESCRIPTOR, NOT A `dup`, and this cost a real defect before it was
     * understood. `fdopendir` takes ownership of what it is given so the caller's
     * descriptor cannot be handed over directly -- but `dup` and `F_DUPFD` SHARE THE FILE
     * OFFSET with the original. The first scan therefore left the shared offset at the end
     * of the directory, and every later scan through another duplicate of the same
     * descriptor read zero entries and reported "no such name". MEASURED as a created
     * `TData` sitting beside an existing `TDATA`, which is exactly the duplicate this
     * function exists to prevent.
     *
     * `openat(dir_fd, ".")` reopens the same directory with its own independent offset.
     * `"."` here is ours, not the guest's: no guest component reaches this call, and the
     * walk refuses `.` and `..` from guest input before it gets here.
     */
    const int scan_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan_fd < 0) {
        return false;
    }
    DIR *dir = fdopendir(scan_fd);
    if (!dir) {
        (void)close(scan_fd);
        return false;
    }
    bool found = false;
    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            break;
        }
        if (!paths_equal(entry->d_name, wanted)) {
            continue;
        }
        const size_t length = strlen(entry->d_name);
        if (length < out_bytes) {
            memcpy(out, entry->d_name, length + 1u);
            found = true;
        }
        break;
    }
    (void)closedir(dir);
    return found;
}

/*
 * Resolve one guest component to the name it has on the host, and stat it, or report it
 * absent.
 *
 * Absent is NOT an error here: the create path needs to know a name is free, and when it
 * is, the name the guest chose is the one that gets created. That is what keeps this
 * case-preserving as well as case-insensitive.
 *
 * THE STAT COMES BACK WITH THE NAME, rather than being left to the caller, because what
 * the caller has to know first is whether this component is a SYMBOLIC LINK -- and the
 * answer cannot be inferred from a failed open. MEASURED: `openat` with
 * O_NOFOLLOW | O_DIRECTORY on a link fails ENOTDIR on Linux, not ELOOP, because
 * O_NOFOLLOW opens the link itself and a link is not a directory. An errno-based test read
 * that as an ordinary missing directory and reported the wrong status. `lstat` answers the
 * question directly instead of inferring it.
 *
 * Callers hold the lock.
 */
static bool hostdir_real_name_locked(int dir_fd, const char *wanted, char *out,
                                     size_t out_bytes, struct stat *out_info)
{
    /* AT_SYMLINK_NOFOLLOW so a symbolic link is SEEN rather than resolved. */
    if (fstatat(dir_fd, wanted, out_info, AT_SYMLINK_NOFOLLOW) == 0) {
        const size_t length = strlen(wanted);
        if (length >= out_bytes) {
            return false;
        }
        memcpy(out, wanted, length + 1u);
        return true;
    }
    if (!hostdir_match_case_locked(dir_fd, wanted, out, out_bytes)) {
        return false;
    }
    return fstatat(dir_fd, out, out_info, AT_SYMLINK_NOFOLLOW) == 0;
}

/*
 * Walk `rest` down from a host-directory volume's root, and hand back the directory that
 * would HOLD its last component plus that component's name.
 *
 * THIS FUNCTION IS THE ESCAPE GUARANTEE, so what it does not do matters as much as what
 * it does. It never assembles a host path. It starts from a duplicate of the volume's
 * root descriptor and opens one component at a time with `openat`, so the only reachable
 * objects are those reachable from that directory. Every intermediate open carries
 * O_NOFOLLOW, so a host symbolic link planted inside the backing directory FAILS the walk
 * rather than being followed -- which is also why there is no `realpath` check anywhere
 * here: a check on a name, followed by a use of that name, is a window, and not opening
 * links at all closes it instead of narrowing it.
 *
 * Returns a descriptor the caller must close, or -1 with `*out_status` set. `leaf` is
 * empty when `rest` named the volume root itself, which is a real case: the title's first
 * open is `\Device\Harddisk0\partition1\` with nothing after it.
 *
 * Callers hold the lock.
 */
static int hostdir_walk_locked(const volume_entry *volume, const char *guest_path,
                               const char *rest, char *leaf, size_t leaf_bytes,
                               uint32_t *out_status)
{
    *out_status = STATUS_SUCCESS;
    leaf[0] = '\0';
    /* A cache partition's directory view walks from its own directory, not from the
     * `--hdd` root that holds the raw image. */
    const int start_plus_one = (volume->cache_partition != 0u && rest[0] != '\0')
                                   ? volume->content_fd_plus_one
                                   : volume->root_fd_plus_one;
    if (start_plus_one == 0) {
        /* Not reachable from the two callers, which both check the backing first. Kept
         * because the alternative is walking from descriptor -1, which `openat` would
         * interpret relative to the process's current directory -- an escape by
         * accident. */
        *out_status = STATUS_UNSUCCESSFUL;
        return -1;
    }

    int dir_fd = fcntl(start_plus_one - 1, F_DUPFD_CLOEXEC, 0);
    if (dir_fd < 0) {
        kernel_hle_log()("kernel: cannot walk \"%s\": no descriptor available (%s)\n",
                         guest_path, strerror(errno));
        *out_status = KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
        return -1;
    }

    /*
     * One component behind. The last component of the path is the LEAF and must not be
     * opened as a directory here, because the caller may be about to create it -- but
     * which component is last is only known once the next separator search comes up
     * empty, so the loop opens the component it saw on the previous turn.
     */
    char pending[KERNEL_FILE_PATH_MAX];
    pending[0] = '\0';
    const char *cursor = rest;
    for (;;) {
        /* `\` and `/` are both separators and the guest mixes them inside one string,
         * which is the same finding `prefix_match_length` rests on. Runs of them collapse
         * rather than producing an empty component. */
        while (*cursor == '\\' || *cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '\\' && *cursor != '/') {
            cursor++;
        }
        const size_t length = (size_t)(cursor - start);
        if (length >= sizeof(pending) || length >= leaf_bytes) {
            (void)close(dir_fd);
            kernel_hle_log()("kernel: \"%s\" has a path component of %zu bytes, which "
                             "this host will not hold -- REFUSED\n",
                             guest_path, length);
            *out_status = KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
            return -1;
        }
        if (component_is_refused(start, length)) {
            (void)close(dir_fd);
            escape_refused_count++;
            kernel_hle_log()("kernel: \"%s\" contains a \".\" or \"..\" component -- "
                             "REFUSED. A guest path may not leave the directory backing "
                             "its volume, and the guest is untrusted input\n",
                             guest_path);
            *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
            return -1;
        }
        if (pending[0] != '\0') {
            /* An intermediate component must already exist: this host creates the LEAF of
             * a path and never its parents. NT does not create intermediate directories
             * either, so a title that needs `a\b` creates `a` first -- which is exactly
             * what the measured sequence does, `TDATA` then `TDATA\45410066`. */
            char real[KERNEL_FILE_PATH_MAX];
            struct stat info;
            if (!hostdir_real_name_locked(dir_fd, pending, real, sizeof(real), &info)) {
                (void)close(dir_fd);
                *out_status = KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
                return -1;
            }
            if (S_ISLNK(info.st_mode)) {
                /* Refused BY NAME rather than by a failed open, for the reason
                 * `hostdir_real_name_locked` gives: the open's errno does not distinguish
                 * a link from a missing directory. A link planted in the backing directory
                 * is the simplest way to make a guest path reach outside it. */
                (void)close(dir_fd);
                escape_refused_count++;
                kernel_hle_log()("kernel: \"%s\" traverses \"%s\", which is a host "
                                 "SYMBOLIC LINK -- REFUSED rather than followed, because "
                                 "following it could leave the backing directory\n",
                                 guest_path, real);
                *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
                return -1;
            }
            /* O_NOFOLLOW stays, as defence in depth: the check above is a separate syscall
             * from this open, and something could become a link in between. */
            const int next =
                openat(dir_fd, real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            (void)close(dir_fd);
            if (next < 0) {
                *out_status = KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
                return -1;
            }
            dir_fd = next;
        }
        memcpy(pending, start, length);
        pending[length] = '\0';
    }

    memcpy(leaf, pending, strlen(pending) + 1u);
    return dir_fd;
}

/*
 * A volume prefix must not end in a separator. Shared by BOTH mount functions, because
 * it was written for the writable one while the disc one had the identical latent trap
 * sitting unguarded -- which is exactly how two copies of a rule drift apart.
 *
 * REFUSED rather than quietly trimmed. `volume_for_locked` requires the character after
 * a matched prefix to be a separator or the end of the string, so a prefix that already
 * ends in one matches the bare device and NOTHING UNDER IT. The volume appears to work
 * for the bare device and then refuses everything beneath it: a half-working mount,
 * which is worse than no mount. Trimming it here would be this layer silently changing
 * what the operator asked for.
 */
static bool prefix_has_no_trailing_separator(const char *prefix, size_t length)
{
    if (length == 0u || (prefix[length - 1u] != '\\' && prefix[length - 1u] != '/')) {
        return true;
    }
    kernel_hle_log()("kernel: cannot mount \"%s\": a volume prefix must NOT end in a "
                     "separator, or paths under it will not resolve. Pass \"%.*s\" "
                     "instead\n",
                     prefix, (int)(length - 1u), prefix);
    return false;
}

bool kernel_file_mount_disc(const char *prefix, const char *image_path)
{
    if (!prefix || !image_path) {
        return false;
    }
    const size_t length = strlen(prefix);
    if (length == 0u || length >= KERNEL_FILE_PATH_MAX) {
        return false;
    }
    if (!prefix_has_no_trailing_separator(prefix, length)) {
        return false;
    }

    /* Open and VALIDATE here, not at the title's first read. A wrong path or a
     * non-Xbox image is an operator mistake, and diagnosing it at mount time names the
     * cause; diagnosing it later would surface as the title failing to find its
     * assets, which looks like a lifting problem. */
    xdvdfs_reader reader;
    const xdvdfs_result result = xdvdfs_open(&reader, image_path);
    if (result != XDVDFS_OK) {
        kernel_hle_log()("kernel: cannot mount \"%s\" on \"%s\": %s\n", image_path,
                         prefix, xdvdfs_result_str(result));
        return false;
    }

    lock();
    volume_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (!volumes[i].in_use) {
            slot = &volumes[i];
            break;
        }
    }
    if (!slot) {
        unlock();
        xdvdfs_close(&reader);
        kernel_hle_log()("kernel: cannot mount \"%s\": all %u volume slots are in use\n",
                         prefix, KERNEL_FILE_VOLUME_MAX);
        return false;
    }
    memcpy(slot->prefix, prefix, length + 1u);
    slot->backing = KERNEL_FILE_BACKING_DISC;
    slot->disc = reader;
    slot->in_use = true;
    unlock();
    return true;
}

/* The caller owns the byte image and must unmount under this lock before freeing it.
 * This is an explicit host FATX model, not a measured Xbox kernel implementation. */
void kernel_file_run_locked(void (*job)(void *), void *context)
{
    lock(); job(context); unlock();
}
bool kernel_file_mount_fatx(const char *prefix, fatx_volume *image,
                            bool (*flush_image)(void *), void *context)
{
    if (prefix == NULL || image == NULL || strlen(prefix) >= KERNEL_FILE_PATH_MAX) return false;
    lock();
    volume_entry *slot = NULL;
    for (unsigned i = 0; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (volumes[i].in_use && strcmp(volumes[i].prefix, prefix) == 0) { unlock(); return false; }
        if (!volumes[i].in_use && slot == NULL) slot = &volumes[i];
    }
    if (slot == NULL || fatx_generation == UINT64_MAX) { unlock(); return false; }
    memset(slot, 0, sizeof(*slot)); strcpy(slot->prefix, prefix);
    slot->backing = KERNEL_FILE_BACKING_FATX; slot->fatx = image;
    slot->generation = ++fatx_generation; slot->flush_image = flush_image;
    slot->flush_context = context; slot->in_use = true;
    unlock(); return true;
}
bool kernel_file_unmount_fatx(const char *prefix)
{
    lock();
    for (unsigned i = 0; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (volumes[i].in_use && volumes[i].backing == KERNEL_FILE_BACKING_FATX &&
            strcmp(volumes[i].prefix, prefix) == 0) {
            memset(&volumes[i], 0, sizeof(volumes[i])); unlock(); return true;
        }
    }
    unlock(); return false;
}
static bool live_fatx(const open_entry *entry)
{
    const volume_entry *volume = &volumes[entry->volume_index];
    return volume->in_use && volume->backing == KERNEL_FILE_BACKING_FATX &&
           volume->generation == entry->generation && volume->fatx != NULL;
}
static uint32_t fatx_status_code(fatx_status status)
{
    /* Host mapping only: genuine original kernel FATX status measurements remain pending. */
    switch (status) {
    case FATX_OK: return STATUS_SUCCESS;
    case FATX_E_NOT_FOUND: return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
    case FATX_E_PATH_NOT_FOUND: return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    case FATX_E_EXISTS: return KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION;
    case FATX_E_NO_SPACE: case FATX_E_DIR_FULL: return KERNEL_FILE_STATUS_DISK_FULL;
    case FATX_E_NOT_DIR: case FATX_E_IS_DIR: return KERNEL_FILE_STATUS_ACCESS_DENIED;
    case FATX_E_NAME: case FATX_E_ARGUMENT: return STATUS_INVALID_PARAMETER;
    default: return STATUS_UNSUCCESSFUL;
    }
}
bool kernel_file_mount_host_dir(const char *prefix, const char *host_dir)
{
    if (!prefix || !host_dir) {
        return false;
    }
    const size_t length = strlen(prefix);
    const size_t root_length = strlen(host_dir);
    if (length == 0u || length >= KERNEL_FILE_PATH_MAX || root_length == 0u ||
        root_length >= KERNEL_FILE_HOST_PATH_MAX) {
        kernel_hle_log()("kernel: cannot mount a writable volume: the guest prefix or "
                         "the host directory is empty or too long\n");
        return false;
    }
    if (!prefix_has_no_trailing_separator(prefix, length)) {
        return false;
    }

    /*
     * OPENED AND VALIDATED HERE, not at the title's first create. The mount is pinned to
     * this descriptor from now on, which is what makes the escape guarantee structural:
     * every later walk is relative to the directory that was checked, and no name is
     * re-resolved against the filesystem afterwards.
     *
     * O_DIRECTORY is the check, not a convenience. It is why an operator cannot hand
     * their disc image to this function: a regular file fails the open with ENOTDIR
     * before anything is recorded, so there is no code path on which an image becomes a
     * writable volume.
     */
    const int root_fd = open(host_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        kernel_hle_log()("kernel: cannot mount \"%s\" as writable storage on \"%s\": %s "
                         "(it must already exist and be a DIRECTORY -- nothing is created "
                         "here, because a typo would otherwise produce an empty hard disk "
                         "somewhere you did not mean)\n",
                         host_dir, prefix, strerror(errno));
        return false;
    }

    lock();
    volume_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (!volumes[i].in_use) {
            slot = &volumes[i];
            break;
        }
    }
    if (!slot) {
        unlock();
        (void)close(root_fd);
        kernel_hle_log()("kernel: cannot mount \"%s\": all %u volume slots are in use\n",
                         prefix, KERNEL_FILE_VOLUME_MAX);
        return false;
    }
    memcpy(slot->prefix, prefix, length + 1u);
    slot->backing = KERNEL_FILE_BACKING_HOST_DIR;
    memcpy(slot->host_root, host_dir, root_length + 1u);
    slot->root_fd_plus_one = root_fd + 1;
    slot->in_use = true;
    unlock();
    return true;
}

/* One body for both public mounts: a plain virtual device (`cache_number` 0) and a
 * formattable cache partition, which adds a directory view named `content_dir`. */
static bool mount_device(const char *prefix, const char *host_dir, const char *file_name,
                         uint64_t capacity_bytes, unsigned cache_number,
                         const char *content_dir)
{
    if (!prefix || !host_dir || !file_name) {
        return false;
    }
    const size_t length = strlen(prefix);
    const size_t root_length = strlen(host_dir);
    const size_t name_length = strlen(file_name);
    if (length == 0u || length >= KERNEL_FILE_PATH_MAX || root_length == 0u ||
        root_length >= KERNEL_FILE_HOST_PATH_MAX) {
        kernel_hle_log()("kernel: cannot mount a virtual device: the guest prefix or the "
                         "host directory is empty or too long\n");
        return false;
    }
    if (!prefix_has_no_trailing_separator(prefix, length)) {
        return false;
    }
    /* A NAME, never a path: a separator or a dot component here would let the caller aim
     * the device at a file outside the directory it was given. */
    if (name_length == 0u || name_length > KERNEL_FILE_DEVICE_NAME_MAX ||
        strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL ||
        component_is_refused(file_name, name_length)) {
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device: the backing "
                         "file name must be ONE component of 1..%u bytes with no "
                         "separator and not \".\" or \"..\"\n",
                         prefix, (unsigned)KERNEL_FILE_DEVICE_NAME_MAX);
        return false;
    }
    size_t content_length = 0u;
    if (cache_number != 0u) {
        content_length = content_dir ? strlen(content_dir) : 0u;
        if (cache_number < KERNEL_FILE_CACHE_PARTITION_FIRST ||
            cache_number > KERNEL_FILE_CACHE_PARTITION_LAST || content_length == 0u ||
            content_length > KERNEL_FILE_DEVICE_NAME_MAX ||
            strchr(content_dir, '/') != NULL || strchr(content_dir, '\\') != NULL ||
            component_is_refused(content_dir, content_length) ||
            strcmp(content_dir, file_name) == 0) {
            kernel_hle_log()("kernel: cannot mount \"%s\" as cache partition %u: the "
                             "number must be %u..%u and the directory name ONE component "
                             "that differs from the image name\n",
                             prefix, cache_number,
                             (unsigned)KERNEL_FILE_CACHE_PARTITION_FIRST,
                             (unsigned)KERNEL_FILE_CACHE_PARTITION_LAST);
            return false;
        }
    }
    if (capacity_bytes == 0u || capacity_bytes > KERNEL_FILE_DEVICE_CAPACITY_MAX) {
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device: capacity %llu "
                         "is outside 1..%llu bytes\n",
                         prefix, (unsigned long long)capacity_bytes,
                         (unsigned long long)KERNEL_FILE_DEVICE_CAPACITY_MAX);
        return false;
    }

    const int root_fd = open(host_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device in \"%s\": %s "
                         "(it must already exist and be a DIRECTORY)\n",
                         prefix, host_dir, strerror(errno));
        return false;
    }

    /*
     * LOOKED AT BEFORE IT IS OPENED, and that order is the whole safety argument. Opening
     * a device node or a FIFO read/write is already an action, so the type is read with
     * AT_SYMLINK_NOFOLLOW first and anything that exists and is not a regular file ends
     * the mount. The `fstat` after the open re-checks the descriptor itself, because the
     * name could have been swapped between the two syscalls.
     */
    struct stat existing;
    if (fstatat(root_fd, file_name, &existing, AT_SYMLINK_NOFOLLOW) == 0 &&
        !S_ISREG(existing.st_mode)) {
        (void)close(root_fd);
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device: \"%s\" in "
                         "\"%s\" exists and is not a REGULAR file (a device node, FIFO, "
                         "symbolic link or directory) -- REFUSED, a raw host object is "
                         "never handed to the guest\n",
                         prefix, file_name, host_dir);
        return false;
    }
    const int file_fd = openat(root_fd, file_name,
                               O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file_fd < 0) {
        const int failure = errno;
        (void)close(root_fd);
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device: opening \"%s\" "
                         "in \"%s\" failed: %s\n",
                         prefix, file_name, host_dir, strerror(failure));
        return false;
    }
    struct stat opened;
    const bool usable = fstat(file_fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
                        (uint64_t)opened.st_size <= capacity_bytes;
    (void)close(file_fd);
    if (!usable) {
        (void)close(root_fd);
        kernel_hle_log()("kernel: cannot mount \"%s\" as a virtual device: \"%s\" is not "
                         "a regular file of at most %llu bytes -- REFUSED rather than "
                         "adopted\n",
                         prefix, file_name, (unsigned long long)capacity_bytes);
        return false;
    }

    lock();
    volume_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (!volumes[i].in_use) {
            slot = &volumes[i];
            break;
        }
    }
    if (!slot) {
        unlock();
        (void)close(root_fd);
        kernel_hle_log()("kernel: cannot mount \"%s\": all %u volume slots are in use\n",
                         prefix, KERNEL_FILE_VOLUME_MAX);
        return false;
    }
    memcpy(slot->prefix, prefix, length + 1u);
    slot->backing = KERNEL_FILE_BACKING_HOST_DIR;
    memcpy(slot->host_root, host_dir, root_length + 1u);
    slot->root_fd_plus_one = root_fd + 1;
    memcpy(slot->device_file, file_name, name_length + 1u);
    slot->device_capacity = capacity_bytes;
    if (cache_number != 0u) {
        slot->cache_partition = cache_number;
        memcpy(slot->content_dir, content_dir, content_length + 1u);
    }
    slot->in_use = true;
    unlock();
    if (cache_number != 0u) {
        kernel_hle_log()("kernel: CACHE PARTITION %u \"%s\" mounted, FABRICATED: the raw "
                         "view is the regular file \"%s\" in \"%s\" (%llu bytes of "
                         "capacity, %llu on the host now) and the directory view is "
                         "\"%s\" there, created only once the title has formatted the "
                         "image. No host block device is involved, and no FAT is parsed\n",
                         cache_number, prefix, file_name, host_dir,
                         (unsigned long long)capacity_bytes,
                         (unsigned long long)opened.st_size, content_dir);
        return true;
    }
    kernel_hle_log()("kernel: VIRTUAL DEVICE \"%s\" mounted, FABRICATED: it is the regular "
                     "file \"%s\" in \"%s\" (%llu bytes of capacity, %llu on the host "
                     "now). What the title writes there persists in that file, and every "
                     "byte it has not written reads back as ZEROS that are ours, not a "
                     "console's. No host block device is involved\n",
                     prefix, file_name, host_dir, (unsigned long long)capacity_bytes,
                     (unsigned long long)opened.st_size);
    return true;
}

bool kernel_file_mount_host_device(const char *prefix, const char *host_dir,
                                   const char *file_name, uint64_t capacity_bytes)
{
    return mount_device(prefix, host_dir, file_name, capacity_bytes, 0u, NULL);
}


bool kernel_file_device_backing_acquire(const char *prefix, kernel_file_device_backing *out)
{
    if (!prefix || !out || strnlen(prefix, KERNEL_FILE_PATH_MAX) == 0u ||
        strnlen(prefix, KERNEL_FILE_PATH_MAX) >= KERNEL_FILE_PATH_MAX) {
        return false;
    }
    bool acquired = false;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        const volume_entry *volume = &volumes[i];
        if (!volume->in_use || !paths_equal(prefix, volume->prefix) ||
            volume->backing != KERNEL_FILE_BACKING_HOST_DIR ||
            volume->root_fd_plus_one == 0 || volume->device_file[0] == '\0' ||
            volume->device_capacity == 0u) {
            continue;
        }
        const int root_fd = fcntl(volume->root_fd_plus_one - 1, F_DUPFD_CLOEXEC, 3);
        if (root_fd >= 0) {
            kernel_file_device_backing candidate = {0};
            candidate.root_fd = root_fd;
            memcpy(candidate.filename, volume->device_file, sizeof(candidate.filename));
            candidate.capacity = volume->device_capacity;
            memcpy(out, &candidate, sizeof(candidate));
            acquired = true;
        }
        break;
    }
    unlock();
    return acquired;
}

bool kernel_file_mount_cache_partition(const char *prefix, const char *host_dir,
                                       unsigned number, const char *image_name,
                                       const char *content_dir, uint64_t capacity_bytes)
{
    if (number == 0u) {
        return false;
    }
    return mount_device(prefix, host_dir, image_name, capacity_bytes, number, content_dir);
}

unsigned kernel_file_volume_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (volumes[i].in_use) {
            count++;
        }
    }
    unlock();
    return count;
}

void kernel_file_unmount_all(void)
{
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_VOLUME_MAX; i++) {
        if (!volumes[i].in_use) {
            continue;
        }
        if (volumes[i].backing == KERNEL_FILE_BACKING_DISC) {
            xdvdfs_close(&volumes[i].disc);
        }
        /* Safe to close with walks outstanding for two independent reasons: every walk
         * runs under this same lock, and a walk duplicates the descriptor rather than
         * using it, so the copy it holds survives regardless. */
        if (volumes[i].root_fd_plus_one != 0) {
            (void)close(volumes[i].root_fd_plus_one - 1);
        }
        if (volumes[i].content_fd_plus_one != 0) {
            (void)close(volumes[i].content_fd_plus_one - 1);
        }
    }
    memset(volumes, 0, sizeof(volumes));
    unlock();
}

bool kernel_file_add_symlink(const char *name, const char *target)
{
    if (!name || !target) {
        return false;
    }
    const size_t name_length = strlen(name);
    const size_t target_length = strlen(target);
    if (name_length == 0u || name_length >= KERNEL_FILE_PATH_MAX ||
        target_length == 0u || target_length >= KERNEL_FILE_PATH_MAX) {
        return false;
    }
    lock();
    symlink_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_MAX; i++) {
        if (symlinks[i].in_use && paths_equal(symlinks[i].name, name)) {
            slot = &symlinks[i];
            break;
        }
        if (!symlinks[i].in_use && !slot) {
            slot = &symlinks[i];
        }
    }
    if (!slot) {
        unlock();
        return false;
    }
    memcpy(slot->name, name, name_length + 1u);
    memcpy(slot->target, target, target_length + 1u);
    slot->in_use = true;
    unlock();
    return true;
}

bool kernel_file_remove_symlink(const char *name)
{
    if (!name) {
        return false;
    }
    bool removed = false;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_MAX; i++) {
        if (symlinks[i].in_use && paths_equal(symlinks[i].name, name)) {
            memset(&symlinks[i], 0, sizeof(symlinks[i]));
            removed = true;
            break;
        }
    }
    unlock();
    return removed;
}

unsigned kernel_file_symlink_handle_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_HANDLE_MAX; i++) {
        /* Asks the object table, as claim_open_locked does: a slot whose handle the guest
         * has closed is not live, and this module is never told about a close. */
        if (symlink_handles[i].in_use &&
            kernel_object_find(symlink_handles[i].handle) != NULL) {
            count++;
        }
    }
    unlock();
    return count;
}

unsigned kernel_file_symlink_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_MAX; i++) {
        if (symlinks[i].in_use) {
            count++;
        }
    }
    unlock();
    return count;
}

const char *kernel_file_symlink_target(const char *name)
{
    if (!name) {
        return NULL;
    }
    lock();
    const char *target = symlink_target_locked(name);
    unlock();
    /* The table is only rewritten by ordinals 67 and 69, and a target string never
     * moves while its slot is in use, so returning the pointer is safe for a reporter.
     * A caller that holds it across a `IoDeleteSymbolicLink` would be reading a freed
     * slot -- which is why this is documented as a reporting accessor. */
    return target;
}

bool kernel_file_open_info(uint32_t handle, kernel_file_open *out)
{
    if (!out) {
        return false;
    }
    bool found = false;
    lock();
    const uint32_t fatx_identity = kernel_object_file_identity(handle);
    for (unsigned index = 0u; index < KERNEL_FILE_OPEN_MAX; index++) {
        open_entry *entry = &open_files[index];
        if (entry->in_use && entry->state.handle == fatx_identity && entry->state.backing == KERNEL_FILE_BACKING_FATX) {
            bool directory; uint32_t size;
            if (!live_fatx(entry) || fatx_stat_path(volumes[entry->volume_index].fatx,
                entry->fatx_path, &directory, &size) != FATX_OK) { unlock(); return false; }
            entry->state.size = size;
        }
    }
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            *out = open_files[i].state;
            found = true;
            break;
        }
    }
    unlock();
    return found;
}

bool kernel_file_flush_fatx(uint32_t handle, uint32_t *status)
{
    lock(); const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i=0; i<KERNEL_FILE_OPEN_MAX; i++) {
        open_entry *entry = &open_files[i];
        if (entry->in_use && entry->state.handle == identity && entry->state.backing == KERNEL_FILE_BACKING_FATX) {
            if (!live_fatx(entry)) *status = STATUS_INVALID_HANDLE;
            else {
                volume_entry *v = &volumes[entry->volume_index];
                *status = v->flush_image != NULL && v->flush_image(v->flush_context) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
            }
            unlock(); return true;
        }
    }
    unlock(); return false;
}
bool kernel_file_open_set_offset(uint32_t handle, uint64_t offset)
{
    bool found = false;
    lock();
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            open_files[i].state.offset = offset;
            found = true;
            break;
        }
    }
    unlock();
    return found;
}

unsigned kernel_file_open_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use) {
            count++;
        }
    }
    unlock();
    return count;
}

uint64_t kernel_file_disc_bytes_read(void)
{
    lock();
    const uint64_t total = disc_bytes_read;
    unlock();
    return total;
}

unsigned kernel_file_disc_opened_count(void)
{
    lock();
    const unsigned count = disc_opened_count;
    unlock();
    return count;
}

unsigned kernel_file_host_opened_count(void)
{
    lock();
    const unsigned count = host_opened_count;
    unlock();
    return count;
}

uint64_t kernel_file_host_bytes_read(void)
{
    lock();
    const uint64_t total = host_bytes_read;
    unlock();
    return total;
}

uint64_t kernel_file_host_bytes_written(void)
{
    lock();
    const uint64_t total = host_bytes_written;
    unlock();
    return total;
}

unsigned kernel_file_write_refused_count(void)
{
    lock();
    const unsigned count = write_refused_count;
    unlock();
    return count;
}

unsigned kernel_file_cache_view_opened_count(void)
{
    lock();
    const unsigned count = cache_view_opened_count;
    unlock();
    return count;
}

unsigned kernel_file_device_opened_count(void)
{
    lock();
    const unsigned count = device_opened_count;
    unlock();
    return count;
}

uint64_t kernel_file_device_zero_bytes(void)
{
    lock();
    const uint64_t count = device_zero_bytes;
    unlock();
    return count;
}

unsigned kernel_file_created_count(void)
{
    lock();
    const unsigned count = created_count;
    unlock();
    return count;
}

unsigned kernel_file_escape_refused_count(void)
{
    lock();
    const unsigned count = escape_refused_count;
    unlock();
    return count;
}

bool kernel_file_volume_space(uint32_t handle, uint64_t *total_units,
    uint64_t *available_units, uint32_t *sectors_per_unit, uint32_t *bytes_per_sector)
{
    if (!total_units || !available_units || !sectors_per_unit || !bytes_per_sector)
        return false;
    lock();
    const uint32_t identity = kernel_object_file_identity(handle);
    open_entry *entry = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; ++i) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            entry = &open_files[i];
            break;
        }
    }
    bool available = false;
    uint64_t total = 0u, free_units = 0u;
    uint32_t sectors = 32u;
    if (identity && entry && entry->volume_index < KERNEL_FILE_VOLUME_MAX) {
        const volume_entry *volume = &volumes[entry->volume_index];
        if (volume->in_use && volume->backing == KERNEL_FILE_BACKING_HOST_DIR &&
            volume->root_fd_plus_one && !volume->device_file[0]) {
            struct statvfs space;
            if (fstatvfs(volume->root_fd_plus_one - 1, &space) == 0 && space.f_frsize &&
                (uint64_t)space.f_blocks <= UINT64_MAX / space.f_frsize &&
                (uint64_t)space.f_bavail <= UINT64_MAX / space.f_frsize) {
                total = (uint64_t)space.f_blocks * space.f_frsize / 16384u;
                free_units = (uint64_t)space.f_bavail * space.f_frsize / 16384u;
                available = true;
            }
        } else if (live_fatx(entry)) {
            const uint32_t free_clusters = fatx_free_clusters(volume->fatx);
            if (free_clusters != UINT32_MAX) {
                total = volume->fatx->cluster_count;
                free_units = free_clusters;
                sectors = volume->fatx->sectors_per_cluster;
                available = true;
            }
        }
    }
    unlock();
    if (!available) return false;
    *total_units = total;
    *available_units = free_units;
    *sectors_per_unit = sectors;
    *bytes_per_sector = 512u;
    return true;
}

static bool read_backing(uint32_t handle, uint64_t offset, void *buffer,
                         uint32_t length, uint32_t *out_read, bool stored_only)
{
    if (!out_read) {
        return false;
    }
    *out_read = 0u;

    /*
     * THE WHOLE READ HAPPENS UNDER THE LOCK, and that is a deliberate trade. The
     * alternative -- copy the volume index out, drop the lock, then pread -- would let
     * another thread unmount the volume and close the descriptor between the two, which
     * is a use-after-close. Holding it means two guest threads reading the disc
     * serialise, which costs throughput this host does not yet have and buys an
     * invariant it cannot otherwise get. The lock is recursive and nothing in here
     * calls back into the guest, so it cannot deadlock.
     */
    lock();
    open_entry *entry = NULL;
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            entry = &open_files[i];
            break;
        }
    }
    if (!entry) {
        unlock();
        return false;
    }

    if (entry->state.backing == KERNEL_FILE_BACKING_HOST_DIR) {
        if (entry->state.is_directory || entry->host_fd_plus_one == 0) {
            /* A directory has no byte stream. REFUSED rather than answered with zero
             * bytes: a short read reads as end-of-file, and a title told its directory is
             * an empty file would conclude its save data is gone. Listing a directory is
             * ordinal 207 NtQueryDirectoryFile (kernel_file_dir_next). */
            unlock();
            kernel_hle_log()("kernel: read on \"%s\", which is a DIRECTORY -- REFUSED. "
                             "Listing one is ordinal 207 NtQueryDirectoryFile, not a "
                             "read\n",
                             entry->state.path);
            return false;
        }
        /* `pread` for the reason xdvdfs.c gives: the offset is an argument, so the only
         * shared state is the descriptor and two guest threads need not serialise around a
         * seek. Loops because a short count is permitted, and retries EINTR. */
        uint8_t *out = (uint8_t *)buffer;
        uint32_t done = 0u;
        /* A VIRTUAL DEVICE ENDS AT ITS CAPACITY, not at however much has been written. */
        const volume_entry *device = &volumes[entry->volume_index];
        const bool is_device = device->in_use && entry->state.device;
        uint32_t wanted = length;
        if (is_device) {
            const uint64_t left =
                offset >= device->device_capacity ? 0u : device->device_capacity - offset;
            wanted = (uint64_t)length > left ? (uint32_t)left : length;
        }
        while (done < wanted) {
            const ssize_t got = pread(entry->host_fd_plus_one - 1, out + done,
                                      wanted - done, (off_t)(offset + done));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                unlock();
                kernel_hle_log()("kernel: reading \"%s\" at offset %llu failed: %s\n",
                                 entry->state.path, (unsigned long long)offset,
                                 strerror(errno));
                return false;
            }
            if (got == 0) {
                /* End of file. A short transfer is not an error and the caller needs the
                 * count either way. */
                break;
            }
            done += (uint32_t)got;
        }
        host_bytes_read += (uint64_t)done;
        if (stored_only && is_device && done < wanted) {
            unlock();
            *out_read = done;
            return true;
        }
        if (is_device && done < wanted) {
            /* Bytes inside the device that the title never wrote. Zeros, counted apart
             * from real bytes and said out loud: this is our answer, not a console's. */
            const uint32_t fabricated = wanted - done;
            memset(out + done, 0, fabricated);
            device_zero_bytes += (uint64_t)fabricated;
            done = wanted;
            kernel_hle_log()("kernel: read of virtual device \"%s\" at offset %llu: %u "
                             "byte(s) were never written and are FABRICATED zeros\n",
                             entry->state.path, (unsigned long long)offset,
                             (unsigned)fabricated);
        }
        unlock();
        *out_read = done;
        return true;
    }

    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        const bool ok = live_fatx(entry) && !entry->state.is_directory &&
            fatx_read_range(volumes[entry->volume_index].fatx, entry->fatx_path, offset,
                            buffer, length, out_read) == FATX_OK;
        unlock(); return ok;
    }
    if (entry->state.backing != KERNEL_FILE_BACKING_DISC) {
        /* A fabricated empty file transfers nothing, and deliberately does NOT
         * zero-fill: a zero-filled buffer is indistinguishable from a real read of
         * zeros, and the point of the empty backing is that the difference stays
         * visible. */
        unlock();
        return !stored_only;
    }

    volume_entry *volume = &volumes[entry->volume_index];
    if (!volume->in_use || volume->backing != KERNEL_FILE_BACKING_DISC) {
        unlock();
        kernel_hle_log()("kernel: read on handle %#x whose volume is no longer "
                         "mounted\n",
                         (unsigned)handle);
        return false;
    }

    xdvdfs_entry target;
    memset(&target, 0, sizeof(target));
    target.start_sector = entry->state.disc_sector;
    /* The disc entry's size is what bounds the read. It came from the directory entry
     * at open time and is carried rather than re-resolved, so a read cannot disagree
     * with the open that produced it. */
    target.size = (uint32_t)entry->state.size;
    target.is_directory = entry->state.is_directory;

    uint32_t transferred = 0u;
    const xdvdfs_result result =
        xdvdfs_read(&volume->disc, &target, offset, buffer, length, &transferred);
    if (result != XDVDFS_OK) {
        unlock();
        kernel_hle_log()("kernel: reading \"%s\" at offset %llu failed: %s\n",
                         entry->state.path, (unsigned long long)offset,
                         xdvdfs_result_str(result));
        return false;
    }
    disc_bytes_read += (uint64_t)transferred;
    unlock();
    *out_read = transferred;
    return true;
}

/* T1633: observer of the guest's file activity (route event waits, docs/input-replay.md). Set once at startup before any guest
 * thread runs, NULL (the default) costs one pointer test. The observer runs under the file lock and must not call back here. */
static kernel_file_observer file_observer;
void kernel_file_set_observer(kernel_file_observer observer) { file_observer = observer; }
static void observe_handle(kernel_file_event_kind kind, uint32_t handle, uint64_t bytes)
{
    if (file_observer == NULL) {
        return;
    }
    lock();
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            file_observer(kind, open_files[i].state.path, bytes);
            break;
        }
    }
    unlock();
}

static bool write_backing_inner(uint32_t handle, uint64_t offset, const void *buffer,
                                uint32_t length, uint32_t *out_written, uint32_t *out_status);
bool kernel_file_write_backing(uint32_t handle, uint64_t offset, const void *buffer,
                               uint32_t length, uint32_t *out_written,
                               uint32_t *out_status)
{
    const bool ok = write_backing_inner(handle, offset, buffer, length, out_written, out_status);
    if (ok && out_written != NULL) {
        observe_handle(KERNEL_FILE_EVENT_WRITE, handle, *out_written);
    }
    return ok;
}

bool kernel_file_read_backing(uint32_t handle, uint64_t offset, void *buffer,
                              uint32_t length, uint32_t *out_read)
{
    const bool ok = read_backing(handle, offset, buffer, length, out_read, false);
    if (ok && out_read != NULL) {
        observe_handle(KERNEL_FILE_EVENT_READ, handle, *out_read);
    }
    return ok;
}
bool kernel_file_read_backing_stored(uint32_t handle, uint64_t offset, void *buffer,
                                     uint32_t length, uint32_t *out_read)
{
    const bool ok = read_backing(handle, offset, buffer, length, out_read, true);
    if (ok && out_read != NULL) {
        observe_handle(KERNEL_FILE_EVENT_READ, handle, *out_read);
    }
    return ok;
}


/*
 * THE SINGLE GATE EVERY MUTATING OPERATION PASSES THROUGH.
 *
 * Both `kernel_file_write_backing` and `kernel_file_set_end_of_file` reach the host
 * filesystem only through this, which is what makes "the disc is never written" and
 * "nothing is writable without --hdd" structural rather than a pair of checks that could
 * drift apart. Written as ONE function for exactly that reason: a second copy of these
 * four refusals is a second place for one of them to be dropped.
 *
 * Returns the entry, or NULL with `*out_status` set and the refusal counted and logged.
 * Callers hold the lock, and must keep holding it while they use the descriptor -- see
 * `kernel_file_read_backing` for why the whole operation stays inside the lock.
 */
static open_entry *writable_entry_locked(uint32_t handle, const char *what,
                                         uint32_t *out_status)
{
    open_entry *entry = NULL;
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            entry = &open_files[i];
            break;
        }
    }
    if (!entry) {
        write_refused_count++;
        *out_status = STATUS_INVALID_HANDLE;
        kernel_hle_log()("kernel: %s on handle %#x, which is not an open file -- "
                         "REFUSED\n",
                         what, (unsigned)handle);
        return NULL;
    }

    /*
     * THE BACKING CHECK IS THE READ-ONLY GUARANTEE, and it is first on purpose. A
     * KERNEL_FILE_BACKING_DISC entry carries no host descriptor of its own -- the image's
     * descriptor lives inside the volume's `xdvdfs_reader`, which nothing below this line
     * names -- so there is no pointer for a later edit to accidentally hand to `pwrite`.
     * KERNEL_FILE_BACKING_EMPTY is refused by the same test, which is what keeps a
     * fabricated empty file from absorbing a write and reporting success.
     */
    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        if (!live_fatx(entry)) { *out_status = STATUS_INVALID_HANDLE; return NULL; }
        if (entry->state.is_directory || !entry->state.writable) {
            *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED; return NULL;
        }
        return entry;
    }
    if (entry->state.backing != KERNEL_FILE_BACKING_HOST_DIR) {
        write_refused_count++;
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        kernel_hle_log()(
            "kernel: %s on \"%s\" REFUSED -- its backing is %s, and the ONLY writable "
            "backing is a host directory an operator mounted with --hdd. Nothing was "
            "written, and this is reported rather than answered with a success that "
            "transferred nothing\n",
            what, entry->state.path,
            entry->state.backing == KERNEL_FILE_BACKING_DISC
                ? "the user's READ-ONLY disc image"
                : "a FABRICATED empty file with no host object behind it");
        return NULL;
    }
    if (entry->state.is_directory || entry->host_fd_plus_one == 0) {
        write_refused_count++;
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        kernel_hle_log()("kernel: %s on \"%s\", which is a DIRECTORY -- REFUSED. A "
                         "directory has no byte stream to write into\n",
                         what, entry->state.path);
        return NULL;
    }
    if (!entry->state.writable) {
        /* NT's own answer, not an extra restriction: a handle opened without write access
         * cannot be written through. Refusing names the real cause, where succeeding
         * would make the open's ACCESS_MASK look irrelevant. */
        write_refused_count++;
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        kernel_hle_log()("kernel: %s on \"%s\" REFUSED -- that handle was opened WITHOUT "
                         "write access, so its host descriptor is read-only\n",
                         what, entry->state.path);
        return NULL;
    }

    /* The volume could have been unmounted between the open and now, which would leave
     * `volume_index` pointing at a reused slot. Checked rather than assumed, because the
     * descriptor being valid says nothing about the volume still being the one that
     * produced it. */
    const volume_entry *volume = &volumes[entry->volume_index];
    if (!volume->in_use || volume->backing != KERNEL_FILE_BACKING_HOST_DIR) {
        write_refused_count++;
        *out_status = STATUS_UNSUCCESSFUL;
        kernel_hle_log()("kernel: %s on handle %#x whose writable volume is no longer "
                         "mounted -- REFUSED\n",
                         what, (unsigned)handle);
        return NULL;
    }
    *out_status = STATUS_SUCCESS;
    return entry;
}

/*
 * Turn an `errno` from a mutating syscall into the status to hand the guest.
 *
 * DISK_FULL IS THE ONE THE TITLE HANDLES RATHER THAN REBOOTING ON, measured after the
 * create at 0x00381321, so it is reported when it is the truth and never as a convenient
 * way to keep a run alive. A fabricated out-of-space would send the title down its own
 * out-of-space path on a host with terabytes free, and that wrong turn would surface
 * nowhere near here.
 */
static uint32_t status_for_write_errno(int failure)
{
    return (failure == ENOSPC || failure == EDQUOT) ? KERNEL_FILE_STATUS_DISK_FULL
                                                    : STATUS_UNSUCCESSFUL;
}

static bool write_backing_inner(uint32_t handle, uint64_t offset, const void *buffer,
                                uint32_t length, uint32_t *out_written,
                                uint32_t *out_status)
{
    if (!out_written || !out_status) {
        return false;
    }
    *out_written = 0u;
    *out_status = STATUS_UNSUCCESSFUL;
    if (!buffer && length > 0u) {
        return false;
    }

    /* The whole write happens under the lock, for the reason `kernel_file_read_backing`
     * gives: dropping it between finding the entry and using its descriptor would let
     * another thread unmount the volume and close that descriptor. */
    lock();
    open_entry *entry = writable_entry_locked(handle, "write", out_status);
    if (!entry) {
        unlock();
        return false;
    }
    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        bool directory; uint32_t size;
        fatx_status result = fatx_stat_path(volumes[entry->volume_index].fatx, entry->fatx_path, &directory, &size);
        if (result == FATX_OK && length != 0u) {
            if (offset > UINT32_MAX || length > UINT32_MAX - offset) result = FATX_E_ARGUMENT;
            else result = fatx_resize_path(volumes[entry->volume_index].fatx, entry->fatx_path,
                offset + length > size ? offset + length : size, offset, buffer, length);
        }
        *out_status = fatx_status_code(result);
        if (result == FATX_OK) { *out_written = length; entry->state.size = length != 0u && offset + length > size ? offset + length : size; }
        unlock(); return result == FATX_OK;
    }

    if (length == 0u) {
        /* A no-op that succeeds, which is what NT does. Deliberately NOT counted as a
         * write: a run report saying "1 write, 0 bytes" would read exactly like the
         * refusal this module exists to make visible. */
        unlock();
        *out_status = STATUS_SUCCESS;
        return true;
    }

    const volume_entry *device = &volumes[entry->volume_index];
    if (entry->state.device &&
        (offset > device->device_capacity ||
         (uint64_t)length > device->device_capacity - offset)) {
        /* A partition ends. Writing past the end would grow the backing file without
         * bound, so it is refused whole rather than clipped: a clipped write is a short
         * transfer the guest may not check. */
        write_refused_count++;
        *out_status = STATUS_INVALID_PARAMETER;
        kernel_hle_log()("kernel: write of %u byte(s) at offset %llu on virtual device "
                         "\"%s\" runs past its %llu-byte capacity -- REFUSED\n",
                         (unsigned)length, (unsigned long long)offset, entry->state.path,
                         (unsigned long long)device->device_capacity);
        unlock();
        return false;
    }

    const uint8_t *in = (const uint8_t *)buffer;
    const int fd = entry->host_fd_plus_one - 1;
    uint32_t done = 0u;
    bool complete = true;
    uint32_t failure_status = STATUS_SUCCESS;
    while (done < length) {
        /* `pwrite` rather than `lseek` + `write` for the reason the read path gives: the
         * offset is an argument, so two guest threads need not serialise around a shared
         * seek position. Retries EINTR. */
        const ssize_t put = pwrite(fd, in + done, (size_t)(length - done),
                                   (off_t)(offset + (uint64_t)done));
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int failure = errno;
            kernel_hle_log()("kernel: writing \"%s\" at offset %llu failed after %u of %u "
                             "byte(s): %s\n",
                             entry->state.path, (unsigned long long)offset,
                             (unsigned)done, (unsigned)length, strerror(failure));
            failure_status = status_for_write_errno(failure);
            complete = false;
            break;
        }
        if (put == 0) {
            /*
             * THERE IS NO END-OF-FILE FOR A WRITE, so a zero return on a non-zero request
             * is not a short transfer to accept -- it is a condition that would spin this
             * loop forever. Reported and failed. The symmetric arm in the read path breaks
             * out and calls it end of file, and copying that shape here would be the
             * single easiest way to turn a stuck write into a silent success.
             */
            kernel_hle_log()("kernel: writing \"%s\" at offset %llu made no progress "
                             "after %u of %u byte(s) -- REPORTED rather than looped\n",
                             entry->state.path, (unsigned long long)offset,
                             (unsigned)done, (unsigned)length);
            failure_status = STATUS_UNSUCCESSFUL;
            complete = false;
            break;
        }
        done += (uint32_t)put;
    }

    /*
     * THE RECORDED SIZE GROWS WITH THE FILE, and that is not bookkeeping. The size is what
     * NtQueryInformationFile class 0x22 reports at +0x28, and the guest TESTS THAT FIELD
     * AGAINST ZERO -- measured at 0x00381084 and 0x0038108D -- to decide whether the file
     * it just opened is empty. A stale zero there would send a second pass down the
     * "nothing saved yet" arm over content that is on the disk.
     */
    const uint64_t end = offset + (uint64_t)done;
    if (end > entry->state.size) {
        entry->state.size = end;
    }
    host_bytes_written += (uint64_t)done;
    unlock();

    *out_written = done;
    if (!complete) {
        *out_status = failure_status;
        return false;
    }
    /* True ONLY on a complete transfer, which is the contract in the header: a caller that
     * reports `*out_written` cannot then claim the requested count. */
    *out_status = STATUS_SUCCESS;
    return true;
}

bool kernel_file_set_end_of_file(uint32_t handle, uint64_t length, uint32_t *out_status)
{
    if (!out_status) {
        return false;
    }
    *out_status = STATUS_UNSUCCESSFUL;

    lock();
    open_entry *entry = writable_entry_locked(handle, "set end of file", out_status);
    if (!entry) {
        unlock();
        return false;
    }
    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        const fatx_status result = fatx_resize_path(volumes[entry->volume_index].fatx,
            entry->fatx_path, length, 0u, NULL, 0u);
        *out_status = fatx_status_code(result);
        if (result == FATX_OK) entry->state.size = length;
        unlock(); return result == FATX_OK;
    }
    if (entry->state.device) {
        /* A device has no length to change, and truncating its backing file would
         * silently erase what the title wrote. */
        write_refused_count++;
        *out_status = STATUS_INVALID_PARAMETER;
        kernel_hle_log()("kernel: setting the end of virtual device \"%s\" is REFUSED: "
                         "a raw device has a fixed length\n",
                         entry->state.path);
        unlock();
        return false;
    }
    if (ftruncate(entry->host_fd_plus_one - 1, (off_t)length) != 0) {
        const int failure = errno;
        *out_status = status_for_write_errno(failure);
        write_refused_count++;
        kernel_hle_log()("kernel: setting \"%s\" to %llu byte(s) failed: %s\n",
                         entry->state.path, (unsigned long long)length,
                         strerror(failure));
        unlock();
        return false;
    }
    /* Logged INSIDE the lock, which reads as over-holding and is not: `entry` points into
     * module state that another thread may reclaim the moment the lock is dropped, and the
     * log line names the path. The lock is recursive and the sink cannot reach back into
     * this module, so re-entry is safe. */
    kernel_hle_log()("kernel: \"%s\" end of file set to %llu byte(s) (was %llu) -- a REAL "
                     "truncation on a real filesystem, not a recorded intention\n",
                     entry->state.path, (unsigned long long)length,
                     (unsigned long long)entry->state.size);
    entry->state.size = length;
    unlock();
    *out_status = STATUS_SUCCESS;
    return true;
}

/* Xbox FileAllocationInformation reserves space without changing readable EOF,
 * including a request smaller than EOF (T951 xemu reference). Host extents are
 * real; no synthetic allocation counter stands in for filesystem reservation. */
bool kernel_file_set_allocation(uint32_t handle, uint64_t length, uint32_t *out_status)
{
    if (!out_status) {
        return false;
    }
    *out_status = STATUS_UNSUCCESSFUL;
    lock();
    open_entry *entry = writable_entry_locked(handle, "set allocation", out_status);
    if (!entry) {
        unlock();
        return false;
    }
    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        *out_status = STATUS_NOT_IMPLEMENTED; unlock(); return false;
    }
    struct stat metadata;
    const int fd = entry->host_fd_plus_one - 1;
    if (entry->state.device || length > INT64_MAX) {
        *out_status = STATUS_INVALID_PARAMETER;
        write_refused_count++;
        unlock();
        return false;
    }
    if (fstat(fd, &metadata) != 0 || metadata.st_size < 0 || metadata.st_blksize <= 0) {
        *out_status = STATUS_UNSUCCESSFUL;
        write_refused_count++;
        unlock();
        return false;
    }
    /* Reserve at least EOF: allocation shrink never truncates Xbox file data.
     * Release only whole host blocks strictly outside that retained range. */
    const uint64_t eof = (uint64_t)metadata.st_size;
    const uint64_t retained = length > eof ? length : eof;
    const uint64_t block = (uint64_t)metadata.st_blksize;
    if (retained > INT64_MAX - (block - 1u)) {
        *out_status = STATUS_INVALID_PARAMETER;
        write_refused_count++;
        unlock();
        return false;
    }
    const uint64_t release_at = ((retained + block - 1u) / block) * block;
    int failure = 0;
    if (retained != 0u &&
        syscall(SYS_fallocate, fd, FALLOC_FL_KEEP_SIZE, (off_t)0, (off_t)retained) != 0) {
        failure = errno;
    } else if (release_at < INT64_MAX &&
               syscall(SYS_fallocate, fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                       (off_t)release_at, (off_t)(INT64_MAX - release_at)) != 0) {
        failure = errno;
    }
    if (failure) {
        *out_status = status_for_write_errno(failure);
        write_refused_count++;
        kernel_hle_log()("kernel: real allocation for \"%s\" failed: %s\n",
                         entry->state.path, strerror(failure));
        unlock();
        return false;
    }
    entry->state.size = eof;
    kernel_hle_log()("kernel: \"%s\" allocation request %llu reserved on host; EOF %llu preserved\n",
                     entry->state.path, (unsigned long long)length, (unsigned long long)eof);
    unlock();
    *out_status = STATUS_SUCCESS;
    return true;
}

static unsigned char dir_fold(char character)
{
    const unsigned char value = (unsigned char)character;
    return (value >= 'A' && value <= 'Z') ? (unsigned char)(value + 32u) : value;
}

/* ASCII case-insensitive wildcard match: `*` any run, `?` one character. */
static bool dir_mask_matches(const char *mask, const char *name)
{
    if (mask[0] == '\0' || strcmp(mask, "*.*") == 0) {
        /* "*.*" is the DOS spelling of "everything", names without a dot included. The guest
         * itself maps that exact mask to a zero-length one (0x00381A6B..0x00381A82). */
        return true;
    }
    const char *star = NULL;
    const char *resume = NULL;
    while (*name != '\0') {
        if (*mask == '*') {
            star = mask++;
            resume = name;
        } else if (*mask == '?' || (*mask != '\0' && dir_fold(*mask) == dir_fold(*name))) {
            mask++;
            name++;
        } else if (star) {
            mask = star + 1;
            name = ++resume;
        } else {
            return false;
        }
    }
    while (*mask == '*') {
        mask++;
    }
    return *mask == '\0';
}

/* Cursor order: ASCII case-folded, byte order breaking ties. */
static int dir_name_compare(const char *a, const char *b)
{
    for (size_t i = 0u;; i++) {
        const unsigned char fa = dir_fold(a[i]);
        const unsigned char fb = dir_fold(b[i]);
        if (fa != fb) {
            return fa < fb ? -1 : 1;
        }
        if (a[i] == '\0') {
            return strcmp(a, b);
        }
    }
}

static uint64_t dir_filetime(time_t seconds, long nanoseconds)
{
    return ((uint64_t)seconds + 11644473600ull) * 10000000ull +
           (uint64_t)(nanoseconds / 100);
}

/* Same bounded host ordering/mask contract, now over actual FATX directory bytes.
 * Ordering and end-status distinctions remain INFERRED, not xemu observations. */
static bool fatx_dir_next_locked(open_entry *entry, bool restart, const char *mask,
                                 uint32_t max_name, kernel_file_dir_entry *out, uint32_t *status)
{
    if (!live_fatx(entry)) { *status = STATUS_INVALID_HANDLE; return false; }
    if (restart) entry->dir_started = false;
    if (!entry->dir_started) {
        const size_t length = mask == NULL ? 0u : strlen(mask);
        if (length > KERNEL_FILE_DIR_NAME_MAX) { *status = STATUS_INVALID_PARAMETER; return false; }
        strcpy(entry->dir_mask, mask == NULL ? "" : mask);
        entry->dir_started = true; entry->dir_has_last = false;
    }
    bool found = false; fatx_directory_entry best;
    for (unsigned index = 0u; index < FATX_CLUSTER_BYTES / FATX_ENTRY_BYTES; index++) {
        fatx_directory_entry candidate; bool end;
        const fatx_status result = fatx_directory_entry_at(volumes[entry->volume_index].fatx,
            entry->fatx_path, index, &candidate, &end);
        if (result != FATX_OK) { *status = fatx_status_code(result); return false; }
        if (end) break;
        if (candidate.name[0] == '\0' || !dir_mask_matches(entry->dir_mask, candidate.name) ||
            (entry->dir_has_last && dir_name_compare(candidate.name, entry->dir_last) <= 0)) continue;
        if (!found || dir_name_compare(candidate.name, best.name) < 0) { best = candidate; found = true; }
    }
    if (!found) { *status = entry->dir_has_last ? KERNEL_FILE_STATUS_NO_MORE_FILES : KERNEL_FILE_STATUS_NO_SUCH_FILE; return false; }
    if (strlen(best.name) > max_name) { *status = KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH; return false; }
    memset(out, 0, sizeof(*out)); strcpy(out->name, best.name);
    out->is_directory = best.directory; out->size = best.size;
    out->image_backed = true; out->attributes = best.attributes; out->allocation = best.allocation;
    out->creation_time = best.creation_time; out->last_access_time = best.access_time; out->last_write_time = best.write_time;
    strcpy(entry->dir_last, best.name); entry->dir_has_last = true; *status = STATUS_SUCCESS; return true;
}

bool kernel_file_dir_next(uint32_t handle, bool restart, const char *mask,
                          uint32_t max_name_bytes, kernel_file_dir_entry *out,
                          uint32_t *out_status)
{
    lock();
    open_entry *entry = NULL;
    const uint32_t identity = kernel_object_file_identity(handle);
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use && open_files[i].state.handle == identity) {
            entry = &open_files[i];
            break;
        }
    }
    if (!entry) {
        *out_status = STATUS_INVALID_HANDLE;
        unlock();
        return false;
    }
    if (!entry->state.is_directory) {
        *out_status = STATUS_INVALID_PARAMETER;
        kernel_hle_log()("kernel: directory query on \"%s\", which is not a directory -- "
                         "REFUSED\n",
                         entry->state.path);
        unlock();
        return false;
    }
    if (entry->state.backing == KERNEL_FILE_BACKING_FATX) {
        const bool ok = fatx_dir_next_locked(entry, restart, mask, max_name_bytes, out, out_status);
        unlock(); return ok;
    }
    if (entry->state.backing != KERNEL_FILE_BACKING_HOST_DIR ||
        entry->dir_fd_plus_one == 0) {
        *out_status = STATUS_NOT_IMPLEMENTED;
        kernel_hle_log()("kernel: listing \"%s\" is NOT IMPLEMENTED: only a directory on an "
                         "--hdd host-directory volume can be enumerated (backing %d)\n",
                         entry->state.path, (int)entry->state.backing);
        unlock();
        return false;
    }

    if (restart) {
        entry->dir_started = false;
    }
    if (!entry->dir_started) {
        entry->dir_started = true;
        entry->dir_has_last = false;
        entry->dir_last[0] = '\0';
        /* The mask is fixed by the first call (or the first after a restart); later calls
         * pass none (MEASURED: only the first call at 0x00381AC8 carries one). */
        const size_t mask_length = mask ? strlen(mask) : 0u;
        if (mask_length > KERNEL_FILE_DIR_NAME_MAX) {
            entry->dir_started = false;
            *out_status = STATUS_INVALID_PARAMETER;
            unlock();
            return false;
        }
        memcpy(entry->dir_mask, mask ? mask : "", mask_length + 1u);
    }

    const int scan_fd = dup(entry->dir_fd_plus_one - 1);
    DIR *dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!dir) {
        if (scan_fd >= 0) {
            (void)close(scan_fd);
        }
        *out_status = STATUS_UNSUCCESSFUL;
        kernel_hle_log()("kernel: listing \"%s\" failed: %s\n", entry->state.path,
                         strerror(errno));
        unlock();
        return false;
    }
    /* dup shares the directory offset with the stored descriptor, so always rewind. */
    rewinddir(dir);

    bool found = false;
    char best[KERNEL_FILE_DIR_NAME_MAX + 1u];
    struct stat best_info;
    memset(&best_info, 0, sizeof(best_info));
    const int dir_fd = dirfd(dir);
    for (;;) {
        const struct dirent *item = readdir(dir);
        if (!item) {
            break;
        }
        const char *name = item->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            /* FATX has no dot entries (INFERRED), and the guest would recurse into them. */
            continue;
        }
        if (strlen(name) > KERNEL_FILE_DIR_NAME_MAX ||
            !dir_mask_matches(entry->dir_mask, name)) {
            continue;
        }
        if (entry->dir_has_last && dir_name_compare(name, entry->dir_last) <= 0) {
            continue;
        }
        if (found && dir_name_compare(name, best) >= 0) {
            continue;
        }
        struct stat info;
        if (fstatat(dir_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
            /* A planted link or device node is hidden, matching the open path refusing it. */
            continue;
        }
        memcpy(best, name, strlen(name) + 1u);
        best_info = info;
        found = true;
    }
    (void)closedir(dir);

    if (!found) {
        /* NT answers NO_SUCH_FILE when the first call matches nothing and NO_MORE_FILES once
         * the listing ran out (INFERRED from desktop NT, the measured site 0x003818C0 treats
         * both as the end). */
        *out_status = entry->dir_has_last ? KERNEL_FILE_STATUS_NO_MORE_FILES
                                          : KERNEL_FILE_STATUS_NO_SUCH_FILE;
        unlock();
        return false;
    }
    if (strlen(best) > max_name_bytes) {
        *out_status = KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH;
        kernel_hle_log()("kernel: listing \"%s\": entry \"%s\" does not fit the caller's "
                         "buffer -- REFUSED, cursor not advanced\n",
                         entry->state.path, best);
        unlock();
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, best, strlen(best) + 1u);
    out->is_directory = S_ISDIR(best_info.st_mode);
    out->size = out->is_directory ? 0u : (uint64_t)best_info.st_size;
    out->last_access_time = dir_filetime(best_info.st_atim.tv_sec, best_info.st_atim.tv_nsec);
    out->last_write_time = dir_filetime(best_info.st_mtim.tv_sec, best_info.st_mtim.tv_nsec);
    /* Linux keeps no birth time through this interface, so creation takes the last-write
     * time (INFERRED, the one consumer copies it and only stores it). */
    out->creation_time = out->last_write_time;
    memcpy(entry->dir_last, best, strlen(best) + 1u);
    entry->dir_has_last = true;
    *out_status = STATUS_SUCCESS;
    unlock();
    return true;
}

/* What a name resolved to, before any handle exists. */
typedef struct {
    kernel_file_backing backing;
    uint64_t generation;
    char fatx_path[KERNEL_FILE_PATH_MAX];
    bool is_directory;
    uint64_t size;
    uint32_t disc_sector;
    unsigned volume_index;
    /* A host descriptor on the resolved regular file, biased by one. See the comment on
     * `volume_entry`: zero means none, which is what this struct's `memset` produces.
     * OWNED BY THE CALLER once a resolution succeeds -- either it reaches an open slot or
     * it has to be closed. */
    int host_fd_plus_one;
    /* A descriptor on the resolved HOST_DIR directory, biased by one, for listing. Owned by
     * the caller exactly like `host_fd_plus_one`. */
    int dir_fd_plus_one;
    /* Whether that descriptor was opened O_RDWR. Carried out of the resolution rather than
     * re-derived at claim time so the recorded capability cannot disagree with the mode the
     * descriptor was actually opened with. */
    bool writable;
    /* The name resolved to a virtual raw device. Carried so an overwriting disposition can
     * be refused: a device has no length to truncate. */
    bool device;
    /* The N of `PartitionN` when the name resolved to a cache partition's RAW view, and
     * whether it resolved to its DIRECTORY view. */
    unsigned cache_partition;
    bool cache_view;
} resolution;

/*
 * Close whatever a resolution opened, for the paths between a successful resolution and
 * the open slot that would have taken ownership of it.
 *
 * Needed because resolving and claiming are deliberately separate steps -- see
 * `resolve_locked` for why -- which leaves a window in which a descriptor exists and no
 * slot owns it. Callers hold the lock.
 */
static void release_resolution_locked(resolution *from)
{
    if (from->host_fd_plus_one != 0) {
        (void)close(from->host_fd_plus_one - 1);
        from->host_fd_plus_one = 0;
    }
    if (from->dir_fd_plus_one != 0) {
        (void)close(from->dir_fd_plus_one - 1);
        from->dir_fd_plus_one = 0;
    }
}

/*
 * Which CreateDispositions may bring a new object into existence.
 *
 * AN ALLOWLIST, not a check for the ones that cannot. That matters: a disposition this
 * module has never seen falls through as non-creating and is refused exactly as it was
 * before any of this existed, rather than being rounded to the nearest value it knows.
 * MEASURED in this image: only 2 (FILE_CREATE) at 0x00380127 and 3 (FILE_OPEN_IF) at
 * 0x00381321. SUPERSEDE and OVERWRITE_IF are listed because NT defines them as creating,
 * not because this title uses them.
 */
static bool disposition_creates(uint32_t disposition)
{
    return disposition == FILE_SUPERSEDE || disposition == FILE_CREATE ||
           disposition == FILE_OPEN_IF || disposition == FILE_OVERWRITE_IF;
}

/*
 * Which CreateDispositions TRUNCATE a name that already exists.
 *
 * ALSO AN ALLOWLIST, and the overlap with `disposition_creates` is the point: SUPERSEDE
 * and OVERWRITE_IF both create when absent AND truncate when present, while OVERWRITE
 * only truncates and FILE_OPEN_IF only opens. Separating the two questions means neither
 * answer has to be inferred from the other.
 *
 * NONE OF THESE IS MEASURED IN THIS IMAGE -- the only dispositions the binary passes are 2
 * at 0x00380127 and 3 at 0x00381321. They are handled anyway because the alternative,
 * which is what this module did until now, is to OPEN the existing file and report
 * FILE_OPENED: the title's new content then sits in front of the old file's tail and
 * nothing anywhere says so. A refusal is recoverable and a silent wrong answer is not.
 */
static bool disposition_overwrites(uint32_t disposition)
{
    return disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE ||
           disposition == FILE_OVERWRITE_IF;
}

/*
 * Resolve what a guest path names inside a host-directory volume.
 *
 * Callers hold the lock, and must have checked that `volume` is a HOST_DIR volume. False
 * with `*out_status` set for a name that is not there, which is an HONEST ABSENCE and not
 * an error -- the same answer a mounted disc gives, and for the same reason. The
 * fabricate-empty policy is bypassed on both: a volume whose real contents are visible
 * must not have contents invented for it.
 */
/*
 * Does `rest` (what follows a matched volume prefix) name this volume's RAW view?
 *
 * A plain virtual device is raw for every `rest`, and answers "not found" inside itself. A
 * cache partition is raw only for its EXACT name: `PartitionN` is the device the title
 * formats and `PartitionN\` is the filesystem it then validates, so one trailing separator
 * is the whole difference between the two views. MEASURED at 0x003813F4 (raw open, no
 * separator, options 0x18) against 0x00380D3F (directory open, `\`, options 0x800021).
 */
static bool names_raw_view(const volume_entry *volume, const char *rest)
{
    if (volume->device_file[0] == '\0') {
        return false;
    }
    return volume->cache_partition == 0u || rest[0] == '\0';
}

/*
 * Does the cache partition's raw image start with the title's own FATX magic?
 *
 * THE ONLY READ-BACK OF A CACHE IMAGE IN THIS MODULE, and it is four bytes at offset 0.
 * MEASURED: the title writes "FATX" there as the first dword of its first format write
 * (0x00381574) and never reads the image again. Opened O_NONBLOCK and re-checked with
 * `fstat` because the name was validated at mount and a FIFO swapped in since would block
 * an open for read forever, with the module lock held.
 */
static bool cache_image_formatted_locked(const volume_entry *volume)
{
    if (volume->root_fd_plus_one == 0) {
        return false;
    }
    const int fd = openat(volume->root_fd_plus_one - 1, volume->device_file,
                          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat info;
    uint8_t magic[4] = {0u, 0u, 0u, 0u};
    bool formatted = false;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        const ssize_t got = pread(fd, magic, sizeof(magic), 0);
        formatted = got == (ssize_t)sizeof(magic) && memcmp(magic, "FATX", 4u) == 0;
    }
    (void)close(fd);
    return formatted;
}

/*
 * Make a cache partition's DIRECTORY view reachable, or say why not.
 *
 * Gated on `cache_image_formatted_locked`, evaluated on EVERY open and not once: the
 * answer is a property of the image, and a gate that latched would keep answering "yes"
 * after something zeroed the image the title is about to re-format. The directory itself
 * is created lazily on the first open that passes, so a mount for a partition the title
 * never selects leaves nothing on the host but its empty image file.
 *
 * Callers hold the lock.
 */
static bool cache_view_prepare_locked(volume_entry *volume, const char *guest_path,
                                      uint32_t *out_status)
{
    if (!cache_image_formatted_locked(volume)) {
        kernel_hle_log()("kernel: \"%s\" is the directory view of cache partition %u, "
                         "whose raw image \"%s\" does not start with the FATX magic -- "
                         "NOT FORMATTED, answered UNRECOGNIZED_VOLUME so the title "
                         "formats it\n",
                         guest_path, volume->cache_partition, volume->device_file);
        *out_status = KERNEL_FILE_STATUS_UNRECOGNIZED_VOLUME;
        return false;
    }
    if (volume->content_fd_plus_one != 0) {
        return true;
    }
    bool created = true;
    if (mkdirat(volume->root_fd_plus_one - 1, volume->content_dir, 0777) != 0) {
        if (errno != EEXIST) {
            kernel_hle_log()("kernel: creating the directory view \"%s\" of cache "
                             "partition %u in \"%s\" failed: %s\n",
                             volume->content_dir, volume->cache_partition,
                             volume->host_root, strerror(errno));
            *out_status = STATUS_UNSUCCESSFUL;
            return false;
        }
        created = false;
    }
    /* O_NOFOLLOW | O_DIRECTORY: a symbolic link or a file planted under that name is
     * refused here rather than followed out of the backing directory. */
    const int fd = openat(volume->root_fd_plus_one - 1, volume->content_dir,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        kernel_hle_log()("kernel: the directory view \"%s\" of cache partition %u in "
                         "\"%s\" is not a plain directory (%s) -- REFUSED\n",
                         volume->content_dir, volume->cache_partition, volume->host_root,
                         strerror(errno));
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        return false;
    }
    volume->content_fd_plus_one = fd + 1;
    kernel_hle_log()("kernel: cache partition %u is FORMATTED (its image starts with "
                     "FATX), so its directory view \"%s\" in \"%s\" is now served -- "
                     "%s. A REAL host directory: what the title stores in it is NOT "
                     "written into the raw image\n",
                     volume->cache_partition, volume->content_dir, volume->host_root,
                     created ? "CREATED just now" : "it already existed");
    return true;
}

static bool hostdir_resolve_locked(volume_entry *volume, const char *guest_path,
                                   const char *rest, uint32_t desired_access,
                                   resolution *out, uint32_t *out_status)
{
    const bool is_device = names_raw_view(volume, rest);
    if (volume->cache_partition != 0u && !is_device) {
        if (!cache_view_prepare_locked(volume, guest_path, out_status)) {
            return false;
        }
        out->cache_view = true;
    }
    if (is_device && rest[0] != '\0') {
        /* A raw device has no namespace inside it, and a trailing separator would mean
         * its ROOT DIRECTORY, which is a filesystem's. Not found rather than quietly
         * treated as the device. */
        kernel_hle_log()("kernel: \"%s\" names something INSIDE a virtual raw device, "
                         "which has no namespace -- not found\n",
                         guest_path);
        *out_status = KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
        return false;
    }

    char leaf[KERNEL_FILE_PATH_MAX];
    int dir_fd =
        hostdir_walk_locked(volume, guest_path, rest, leaf, sizeof(leaf), out_status);
    if (dir_fd < 0) {
        return false;
    }
    if (is_device) {
        /* The device IS its backing file: the exact name resolves to it, through the same
         * lstat, type and O_NOFOLLOW checks as any other file in this volume. */
        memcpy(leaf, volume->device_file, strlen(volume->device_file) + 1u);
    }

    out->backing = KERNEL_FILE_BACKING_HOST_DIR;
    out->volume_index = (unsigned)(volume - volumes);
    out->size = 0u;

    if (leaf[0] == '\0') {
        /* The volume root itself, which the mount already proved exists and is a
         * directory. A real case, not a corner: the title's first open of the hard disk
         * is `\Device\Harddisk0\partition1\` with nothing after it. */
        /* The walk's descriptor IS the root directory here, so it is kept for listing. */
        out->dir_fd_plus_one = dir_fd + 1;
        out->is_directory = true;
        *out_status = STATUS_SUCCESS;
        return true;
    }

    char real[KERNEL_FILE_PATH_MAX];
    struct stat info;
    if (!hostdir_real_name_locked(dir_fd, leaf, real, sizeof(real), &info)) {
        (void)close(dir_fd);
        *out_status = KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
        return false;
    }
    if (S_ISDIR(info.st_mode)) {
        /* Best effort: a directory the host will not let us open still opens, and a listing
         * of it is then refused rather than answered empty. */
        const int listing_fd =
            openat(dir_fd, real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        (void)close(dir_fd);
        out->dir_fd_plus_one = listing_fd >= 0 ? listing_fd + 1 : 0;
        out->is_directory = true;
        *out_status = STATUS_SUCCESS;
        return true;
    }
    if (S_ISLNK(info.st_mode)) {
        /* Visible here BECAUSE of AT_SYMLINK_NOFOLLOW, and refused rather than followed:
         * a link planted in the backing directory is the simplest way to make a guest
         * path name a host file outside it. */
        (void)close(dir_fd);
        escape_refused_count++;
        kernel_hle_log()("kernel: \"%s\" names \"%s\", a host SYMBOLIC LINK -- REFUSED, "
                         "because following it could leave the backing directory\n",
                         guest_path, real);
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        /* A device node, a socket, a FIFO. Opening one would block or would hand the
         * title something that is not a file at all. */
        (void)close(dir_fd);
        kernel_hle_log()("kernel: \"%s\" names \"%s\", which is neither a regular file "
                         "nor a directory -- REFUSED\n",
                         guest_path, real);
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        return false;
    }

    /*
     * O_RDWR ONLY WHEN THE GUEST ASKED TO WRITE, O_RDONLY OTHERWISE. This line used to be
     * unconditionally O_RDONLY because nothing in this tree could write; it is now the
     * narrowest mode that satisfies the open, which is NT's own rule rather than a
     * restriction invented here. O_NOFOLLOW stays either way -- the mode decides what may
     * be done to the object, the walk decides WHICH object, and conflating the two is how
     * a write path acquires an escape.
     */
    const bool wants_write = access_wants_write(desired_access);
    int file_fd = openat(dir_fd, real,
                         (wants_write ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
    bool writable = wants_write;
    if (file_fd < 0 && wants_write &&
        (errno == EACCES || errno == EPERM || errno == EROFS)) {
        /*
         * THE HOST REFUSED WRITE, so fall back to read-only and RECORD THAT IT IS NOT
         * WRITABLE. Falling back silently with `writable` left true would produce a handle
         * that claims a capability its descriptor does not have, and the first write
         * through it would fail with EBADF -- a diagnosis that names the wrong layer. A
         * read-only file inside a writable directory is an ordinary thing on a host, so
         * refusing the whole open would stop a run over something the title may only
         * intend to read.
         */
        const int first_failure = errno;
        file_fd = openat(dir_fd, real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        writable = false;
        if (file_fd >= 0) {
            kernel_hle_log()("kernel: \"%s\" -> \"%s\" asked for write access (%#x) and "
                             "the host refused it (%s); opened READ-ONLY, and a write "
                             "through this handle will be REFUSED rather than silently "
                             "dropped\n",
                             guest_path, real, (unsigned)desired_access,
                             strerror(first_failure));
        }
    }
    (void)close(dir_fd);
    if (file_fd < 0) {
        kernel_hle_log()("kernel: \"%s\" -> \"%s\" could not be opened: %s\n", guest_path,
                         real, strerror(errno));
        *out_status = STATUS_UNSUCCESSFUL;
        return false;
    }
    out->is_directory = false;
    out->size = (uint64_t)info.st_size;
    out->host_fd_plus_one = file_fd + 1;
    out->writable = writable;
    if (is_device) {
        /* The size a title sees is the device's, not however much of it has been written:
         * a partition does not grow as it is used. */
        out->size = volume->device_capacity;
        out->device = true;
        out->cache_partition = volume->cache_partition;
        kernel_hle_log()("kernel: \"%s\" -> VIRTUAL raw device, FABRICATED: host file "
                         "\"%s\", %llu bytes of capacity, %s. Unwritten bytes read as "
                         "ZEROS that are ours\n",
                         guest_path, volume->device_file,
                         (unsigned long long)volume->device_capacity,
                         writable ? "opened FOR WRITING" : "READ-ONLY");
    }
    *out_status = STATUS_SUCCESS;
    return true;
}

/*
 * Genuinely create the last component of a guest path on a host-directory volume.
 *
 * THE ONLY FUNCTION IN THIS MODULE THAT CREATES ANYTHING, and the only caller is
 * NtCreateFile with a creating disposition. It refuses any volume that is not HOST_DIR
 * before touching the filesystem, which is what makes "the disc is never written"
 * structural rather than conventional: there is no argument an operator or a guest can
 * supply that routes a disc volume here.
 *
 * Only the LEAF is created. NT does not create intermediate directories either, and the
 * measured sequence relies on that: the title creates `TDATA`, then `TDATA\45410066`.
 *
 * Callers hold the lock.
 */
static bool hostdir_create_locked(const volume_entry *volume, const char *guest_path,
                                  const char *rest, bool as_directory,
                                  uint32_t desired_access, resolution *out,
                                  uint32_t *out_status)
{
    if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR || names_raw_view(volume, rest)) {
        /* A raw device has nothing to create inside it. */
        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;
        return false;
    }
    /* A cache partition's directory view is already prepared and gated: the one caller only
     * gets here after `hostdir_resolve_locked` answered OBJECT_NAME_NOT_FOUND, which it does
     * for a missing name only AFTER `cache_view_prepare_locked` passed. Unprepared, the walk
     * below finds no descriptor and refuses, so there is no path around the gate. */

    char leaf[KERNEL_FILE_PATH_MAX];
    const int dir_fd =
        hostdir_walk_locked(volume, guest_path, rest, leaf, sizeof(leaf), out_status);
    if (dir_fd < 0) {
        return false;
    }
    /* Not re-checked for an existing name here: the one caller only reaches this after a
     * resolution came back OBJECT_NAME_NOT_FOUND, and `mkdirat` and `O_EXCL` below both
     * fail rather than clobber if that answer has gone stale underneath us. */
    if (leaf[0] == '\0') {
        /* Not reachable from the one caller, which only gets here after a resolution
         * failed and the volume root always resolves. Refused rather than assumed
         * unreachable, because the alternative is creating something with no name. */
        (void)close(dir_fd);
        *out_status = KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION;
        return false;
    }

    out->backing = KERNEL_FILE_BACKING_HOST_DIR;
    out->volume_index = (unsigned)(volume - volumes);
    out->size = 0u;
    out->is_directory = as_directory;

    if (as_directory) {
        /* 0777 and let the process umask decide, which is what `mkdir` does. Forcing a
         * mode here would override a deliberate umask on the operator's own directory. */
        if (mkdirat(dir_fd, leaf, 0777) != 0) {
            const int failure = errno;
            (void)close(dir_fd);
            kernel_hle_log()("kernel: creating directory \"%s\" on \"%s\" failed: %s\n",
                             guest_path, volume->host_root, strerror(failure));
            /* DISK_FULL is the one failure the guest handles rather than rebooting on, so
             * it is reported when it is the truth and never when it is not. */
            *out_status = (failure == ENOSPC || failure == EDQUOT)
                              ? KERNEL_FILE_STATUS_DISK_FULL
                              : STATUS_UNSUCCESSFUL;
            return false;
        }
        const int listing_fd =
            openat(dir_fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        out->dir_fd_plus_one = listing_fd >= 0 ? listing_fd + 1 : 0;
        (void)close(dir_fd);
        created_count++;
        kernel_hle_log()("kernel: CREATED directory \"%s\" under \"%s\" -- a real "
                         "directory on a real filesystem, not a fabrication\n",
                         guest_path, volume->host_root);
        *out_status = STATUS_SUCCESS;
        return true;
    }

    /*
     * O_EXCL: this is only called once the name was found absent, so an existing file
     * here means something changed underneath us and must be reported, not silently
     * truncated. O_NOFOLLOW so a planted link is not written through.
     *
     * O_RDWR when the create asked for write access, which the measured one does: at
     * 0x00381027 the title creates TitleMeta.xbx with DesiredAccess 0x40100000. A file
     * created for writing and then handed back on a read-only descriptor would leave the
     * title's very next NtWriteFile refused, on a file it had just made.
     */
    const bool wants_write = access_wants_write(desired_access);
    const int file_fd = openat(dir_fd, leaf,
                               (wants_write ? O_RDWR : O_RDONLY) | O_CREAT | O_EXCL |
                                   O_NOFOLLOW | O_CLOEXEC,
                               0666);
    const int failure = errno;
    (void)close(dir_fd);
    if (file_fd < 0) {
        kernel_hle_log()("kernel: creating file \"%s\" on \"%s\" failed: %s\n", guest_path,
                         volume->host_root, strerror(failure));
        *out_status = (failure == ENOSPC || failure == EDQUOT)
                          ? KERNEL_FILE_STATUS_DISK_FULL
                          : STATUS_UNSUCCESSFUL;
        return false;
    }
    out->host_fd_plus_one = file_fd + 1;
    out->writable = wants_write;
    created_count++;
    kernel_hle_log()("kernel: CREATED file \"%s\" under \"%s\", empty -- a real file on a "
                     "real filesystem, %s\n",
                     guest_path, volume->host_root,
                     wants_write ? "opened FOR WRITING: NtWriteFile through this handle "
                                   "puts real bytes in it"
                                 : "opened READ-ONLY because the create did not ask for "
                                   "write access");
    *out_status = STATUS_SUCCESS;
    return true;
}

/*
 * Resolve a guest path to content, WITHOUT issuing a handle or claiming a slot.
 *
 * Callers hold the lock. This is the single place a name becomes content, so it is the
 * single place the three outcomes are decided: a real disc file, a fabricated empty one,
 * or a refusal.
 *
 * SEPARATED FROM CLAIMING ON PURPOSE, and the project's own test suite is why. An earlier
 * version issued the handle first and resolved second, because the slot needed a handle to
 * key on. That made a REFUSED open consume an object-table entry, and
 * `test_kernel_file.c` caught it with `kernel_object_live_count() == 0` after a failed
 * open. The test was right and the design was wrong: the real kernel leaves the handle
 * out-parameter untouched on failure and consumes nothing, and a title that probes for a
 * dozen absent files -- which this one does, it has a whole `host0:` tree that cannot
 * resolve -- would have drained the table. Resolving first means nothing is allocated
 * until the answer is known.
 */
/*
 * Rewrite the title's own symbolic links, then find the volume that owns the result.
 *
 * SPLIT OUT because NtCreateFile has to ask this question twice for two different reasons:
 * once to learn whether the name already resolves, and again -- when it does not -- to
 * learn whether the volume behind it is one this host may create on. A second copy of the
 * walk would be a second place for the separator, case and longest-prefix rules to drift.
 *
 * False ONLY when the link walk did not settle, which is the one outcome that is not a
 * question about volumes. `*out_volume` NULL with a true return means the name resolved
 * fine and nothing is mounted behind it. Callers hold the lock, and `*out_rest` points
 * into `resolved`, so it lives exactly as long as that buffer.
 */
static bool locate_volume_locked(const char *path, char *resolved, size_t resolved_bytes,
                                 volume_entry **out_volume, const char **out_rest,
                                 uint32_t *out_status)
{
    *out_volume = NULL;
    *out_rest = NULL;
    if (!resolve_symlinks_locked(path, resolved, resolved_bytes)) {
        *out_status = KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
        return false;
    }
    *out_status = STATUS_SUCCESS;
    *out_volume = volume_for_locked(resolved, out_rest);
    return true;
}

static bool resolve_backing_locked(const char *path, uint32_t desired_access, resolution *out,
                                   uint32_t *out_status, bool *out_fabricated, bool allow_empty)
{
    *out_fabricated = false;
    memset(out, 0, sizeof(*out));
    char resolved[KERNEL_FILE_PATH_MAX];
    volume_entry *volume = NULL;
    const char *rest = NULL;
    if (!locate_volume_locked(path, resolved, sizeof(resolved), &volume, &rest,
                              out_status)) {
        return false;
    }

    if (volume && volume->backing == KERNEL_FILE_BACKING_FATX) {
        bool directory; uint32_t size;
        const fatx_status result = fatx_stat_path(volume->fatx, rest, &directory, &size);
        *out_status = fatx_status_code(result);
        if (result != FATX_OK) return false;
        out->backing = KERNEL_FILE_BACKING_FATX; out->is_directory = directory;
        out->size = size; out->writable = access_wants_write(desired_access);
        out->volume_index = (unsigned)(volume - volumes); out->generation = volume->generation;
        if (strlen(rest) >= sizeof(out->fatx_path)) { *out_status = STATUS_INVALID_PARAMETER; return false; }
        strcpy(out->fatx_path, rest); return true;
    }
    if (volume && volume->backing == KERNEL_FILE_BACKING_HOST_DIR) {
        /* A real writable volume answers for what it actually holds and refuses the rest,
         * exactly as a mounted disc does. Falling through to the fabricate policy would
         * invent content on a volume whose real content we can list. */
        return hostdir_resolve_locked(volume, path, rest, desired_access, out,
                                      out_status);
    }

    if (volume && volume->backing == KERNEL_FILE_BACKING_DISC) {
        xdvdfs_entry found;
        const xdvdfs_result result = xdvdfs_lookup(&volume->disc, rest, &found);
        if (result != XDVDFS_OK) {
            /* NOT FOUND ON A MOUNTED DISC IS AN HONEST FAILURE, and it is reported as
             * one rather than falling through to the fabricated-empty policy. If the
             * title asks a mounted disc for a file the disc does not have, the right
             * answer is that the file is absent -- handing back an empty one would
             * invent content on a volume whose real content we can see. */
            kernel_hle_log()("kernel: \"%s\" -> disc path \"%s\": %s\n", path, rest,
                             xdvdfs_result_str(result));
            *out_status = (result == XDVDFS_ERR_NOT_FOUND)
                              ? KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND
                              : STATUS_UNSUCCESSFUL;
            return false;
        }
        out->backing = KERNEL_FILE_BACKING_DISC;
        out->is_directory = found.is_directory;
        out->size = (uint64_t)found.size;
        out->disc_sector = found.start_sector;
        out->volume_index = (unsigned)(volume - volumes);
        kernel_hle_log()("kernel: \"%s\" -> REAL disc file \"%s\", %llu bytes at sector "
                         "%u\n",
                         path, rest, (unsigned long long)found.size,
                         (unsigned)found.start_sector);
        *out_status = STATUS_SUCCESS;
        return true;
    }

    /*
     * Nothing with content resolved the name. The remaining question is the one this
     * module has always asked: is this name DECLARED openable, and if not, what is the
     * policy? Unchanged in meaning by the arrival of real volumes -- a mounted disc
     * answers for the paths it contains and for nothing else.
     */
    if (!allow_empty) {
        refused_count++;
        *out_status = KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
        return false;
    }
    const bool declared = is_openable_locked(path) || volume != NULL;
    if (!declared) {
        if (missing_policy == KERNEL_FILE_MISSING_FAIL) {
            refused_count++;
            *out_status = KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
            return false;
        }
        fabricated_count++;
        *out_fabricated = true;
    }

    out->backing = KERNEL_FILE_BACKING_EMPTY;
    out->size = 0u;
    *out_status = STATUS_SUCCESS;
    return true;
}

static bool resolve_locked(const char *path, uint32_t desired_access, resolution *out,
                           uint32_t *out_status, bool *out_fabricated)
{
    return resolve_backing_locked(path, desired_access, out, out_status, out_fabricated, true);
}

/*
 * Record an already-resolved name against a handle. Callers hold the lock.
 *
 * Called only AFTER both the resolution and the handle have succeeded, so it cannot
 * fail in a way that wastes either. Returns false only when the open-file table is
 * full, which is reported: a file the guest believes is open but which this module is
 * not tracking would read as zero bytes forever.
 */
static bool claim_open_locked(const char *path, uint32_t handle,
                              const resolution *from, uint32_t open_options)
{
    /*
     * RECLAIM SLOTS WHOSE HANDLE IS DEAD, which is how this table survives a long run
     * without a close notification. `NtClose` lives in kernel_object.c and offers no
     * hook for an owning subsystem to learn that one of its handles went away -- so
     * rather than reach into a module this task does not own, the authoritative object
     * table is consulted: a slot whose handle it no longer knows is a file the guest has
     * closed, and its slot is free. Self-healing, and it cannot disagree with the
     * object table because the object table is the thing being asked. A handle made by
     * NtDuplicateObject (197) shares its original's slot, so the question is "is the original
     * or any duplicate of it still live", which is what kernel_object_file_identity_live
     * answers.
     */
    open_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        if (open_files[i].in_use &&
            !kernel_object_file_identity_live(open_files[i].state.handle)) {
            /* Through the release helper, not a bare memset: this is where a host-backed
             * file's descriptor is closed, and it is the only notification this module
             * gets that the guest let go of the handle. */
            release_open_slot_locked(&open_files[i]);
        }
        if (!open_files[i].in_use && !slot) {
            slot = &open_files[i];
        }
    }
    if (!slot) {
        kernel_hle_log()("kernel: \"%s\" resolved but all %u open-file slots are in "
                         "use\n",
                         path, KERNEL_FILE_OPEN_MAX);
        return false;
    }

    release_open_slot_locked(slot);
    slot->state.handle = handle;
    const size_t path_length = strlen(path);
    memcpy(slot->state.path, path, path_length + 1u);
    slot->state.backing = from->backing;
    slot->state.is_directory = from->is_directory;
    slot->state.size = from->size;
    slot->state.disc_sector = from->disc_sector;
    /* Copied from the resolution rather than recomputed, so the recorded capability and the
     * descriptor's actual mode are the same fact rather than two derivations of it. */
    slot->state.writable = from->writable;
    slot->state.device = from->device;
    slot->state.cache_partition = from->cache_partition;
    slot->state.open_options = open_options;
    slot->volume_index = from->volume_index;
    slot->generation = from->generation;
    strcpy(slot->fatx_path, from->fatx_path);
    /* OWNERSHIP MOVES HERE. The resolution opened this descriptor; from now on the slot
     * owns it and `release_open_slot_locked` is what closes it. Every path that fails
     * between the resolution and this call has to close it instead, which is why both
     * handlers do so explicitly rather than relying on a later sweep. */
    slot->host_fd_plus_one = from->host_fd_plus_one;
    slot->dir_fd_plus_one = from->dir_fd_plus_one;
    slot->in_use = true;
    if (from->backing == KERNEL_FILE_BACKING_DISC) {
        disc_opened_count++;
    } else if (from->backing == KERNEL_FILE_BACKING_HOST_DIR) {
        host_opened_count++;
        if (from->device) {
            device_opened_count++;
        }
        if (from->cache_view) {
            cache_view_opened_count++;
        }
    }
    if (file_observer != NULL) {
        file_observer(KERNEL_FILE_EVENT_OPEN, slot->state.path, 0u);
    }
    return true;
}

/*
 * There is deliberately NO release_open_locked() here.
 *
 * The obvious design is for NtClose to tell this module to drop the slot, but NtClose
 * lives in kernel_object.c and offers no hook for an owning subsystem -- and that file is
 * outside this task's ownership. An explicit release function with no caller was written
 * first and then deleted, because dead code that looks like the lifetime story is worse
 * than no code: a reader would assume slots are released on close and they are not.
 *
 * Slots are instead reclaimed lazily in claim_open_locked(), which asks the object table
 * whether each recorded handle is still live. That cannot disagree with the object table,
 * because the object table is the thing being asked.
 */

/* Address of a guest struct member, 0 on 32-bit wrap (which every accessor refuses). */
#define GUEST_FIELD(base, type, member) \
    kernel_guest_add((base), (uint32_t)offsetof(type, member))
#define GUEST_FIELD_BYTE(base, type, member, byte) \
    kernel_guest_add((base), (uint32_t)offsetof(type, member) + (byte))

/* Read the 12-byte OBJECT_ATTRIBUTES. Field by field through the guest accessors, which
 * refuse an unmapped page (kernel_call.c probes it), and with kernel_guest_add so a
 * base near 4 GB cannot wrap into a low address. Offsets are the ones
 * guest_structs.h derives rather than by casting a host struct over guest memory --
 * the host's padding rules are not the guest's. */
static bool read_object_attributes(kernel_guest_ptr address, uint32_t *root_directory,
                                   uint32_t *object_name, uint32_t *attributes)
{
    return kernel_guest_read_u32(GUEST_FIELD(address, guest_object_attributes, root_directory),
                                 root_directory) &&
           kernel_guest_read_u32(GUEST_FIELD(address, guest_object_attributes, object_name),
                                 object_name) &&
           kernel_guest_read_u32(GUEST_FIELD(address, guest_object_attributes, attributes),
                                 attributes);
}

/* Bounded inferred policy from the live ANSI shims: FD + drive-absolute
 * names refer to the title's existing \\??\\ links. This does not establish the
 * console's internal sentinel meaning or support real directory handles/FC.
 *
 * T142 MEASURED the two still-refused root families, so the refusals below are
 * now known to guard paths this title cannot reach rather than pending work:
 *   - 0xFFFFFFFC is stored only by the named-event/mutex helper 0x381B7C, whose
 *     two call sites sit on statically dead branches (see kernel_object.c and
 *     GUEST_OBJ_ROOT_NAMED_OBJECT_PSEUDO in guest_structs.h). It can never
 *     arrive at a file ordinal at all.
 *   - A LIVE directory handle at +0x00 is built only inside XAPI's recursive
 *     delete: `_XapiNukeDirectoryFromHandle@8` 0x3817A4 (relative NtOpenFile at
 *     0x38183D) and `_XapiNukeDirectory@8` 0x381921 (NtOpenFile at 0x381963).
 *     Their only reachable entry points are XDeleteSaveGame 0x37E37D (game
 *     callers 0x256F3, 0x26722) and XRemoveContent 0x37FBA9 (game caller
 *     0x26AE1), and the enumeration both run FIRST is ordinal 207
 *     NtQueryDirectoryFile, which has no handler here and stops the run loudly
 *     before any relative open is attempted. No boot reaches them (the current
 *     stop is in DSOUND effects init). So real directory-handle roots stay
 *     REFUSED with this message until a run measurably arrives here. */
static bool read_object_path(const char *ordinal, uint32_t root, char *path, size_t capacity)
{
    if (root == 0u) return true;
    const size_t length = strlen(path);
    /* Retail0037D4F9 passes the bare drive root T: with this sentinel. */
    bool accepted = root == UINT32_C(0xFFFFFFFD) && length >= 2u &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (length == 2u || path[2] == '\\' || path[2] == '/') &&
        length + 4u < capacity;
    for (const char *component = path + (length >= 3u ? 3u : length);
         accepted && *component != '\0';) {
        const char *end = component;
        while (*end != '\0' && *end != '\\' && *end != '/') end++;
        if (component_is_refused(component, (size_t)(end - component))) accepted = false;
        component = *end == '\0' ? end : end + 1;
    }
    lock();
    if (!accepted) relative_refused_count++;
    const bool announce = accepted && !inferred_drive_root_announced;
    if (announce) inferred_drive_root_announced = true;
    unlock();
    if (!accepted) {
        kernel_hle_log()("kernel: %s(\"%s\") root %#x -- REFUSED, outside bounded drive-root policy\n",
                         ordinal, path, (unsigned)root);
        return false;
    }
    if (announce)
        kernel_hle_log()("kernel: INFERRED namespace policy: root 0xfffffffd + absolute drive names "
                         "resolve through the title's existing \\??\\ links; other roots remain refused\n");
    memmove(path + 4u, path, length + 1u);
    memcpy(path, "\\??\\", 4u);
    return true;
}

/*
 * OBJECT_ATTRIBUTES.attributes is read by every handler and honoured by none, so any value
 * other than the one this host's behaviour agrees with is COUNTED and REPORTED, not dropped.
 *
 * The only value that agrees is 0x40: names are always matched case-insensitively here,
 * which is what 0x40 means by desktop NT analogy (INFERRED, see guest_structs.h). Every
 * inline builder in the image stores exactly that, so the boot never reaches the report.
 * A clear 0x40 is reported too: it asks for a case-sensitive match this host cannot give.
 * OBJ_INHERIT, OBJ_PERMANENT, OBJ_EXCLUSIVE and OBJ_OPENIF change what a handle or name
 * MEANS afterwards and none of them is modelled.
 */
static void report_unmodelled_attributes(const char *ordinal_name, const char *name,
                                         uint32_t attributes)
{
    if (attributes == GUEST_OBJ_CASE_INSENSITIVE_INFERRED) {
        return;
    }
    lock();
    unmodelled_attributes_count++;
    unlock();
    kernel_hle_log()("kernel: %s(\"%s\") OBJECT_ATTRIBUTES.attributes is %#x, which this "
                     "host does not model -- names are always matched case-insensitively and "
                     "the other bits are ignored\n",
                     ordinal_name, name, (unsigned)attributes);
}

/*
 * Copy an 8-byte OBJECT_STRING's characters into `out`.
 *
 * `length` is the byte count EXCLUDING the NUL (measured: the guest sets
 * MaximumLength to Length+1 at all 18 static instances), and the characters are
 * single-byte -- which `guest_structs.h` establishes from a byte-wise strlen loop at
 * 0x0037C9D2, not from an assumption that ANSI_STRING means what it does on desktop
 * NT. So this copies `length` bytes and terminates; it does NOT trust a NUL to be
 * there, because MaximumLength is the guest's promise about its own buffer and not
 * about ours.
 */
static bool read_object_string(kernel_guest_ptr address, char *out, size_t out_bytes)
{
    uint16_t length = 0u;
    uint32_t buffer = 0u;
    uint8_t low = 0u;
    uint8_t high = 0u;
    if (!kernel_guest_read_u8(GUEST_FIELD(address, guest_object_string, length),
                              &low) ||
        !kernel_guest_read_u8(GUEST_FIELD_BYTE(address, guest_object_string, length, 1u),
                              &high)) {
        return false;
    }
    length = (uint16_t)((uint32_t)low | ((uint32_t)high << 8));
    if (!kernel_guest_read_u32(GUEST_FIELD(address, guest_object_string, buffer),
                               &buffer)) {
        return false;
    }
    if (buffer == 0u) {
        return false;
    }
    if ((size_t)length + 1u > out_bytes) {
        return false;
    }
    for (uint16_t i = 0u; i < length; i++) {
        uint8_t character = 0u;
        if (!kernel_guest_read_u8(kernel_guest_add(buffer, i), &character)) {
            return false;
        }
        out[i] = (char)character;
    }
    out[length] = '\0';
    return true;
}

/* Write both IO_STATUS_BLOCK fields, at the offsets guest_structs.h derives. A
 * partially-written block would leave the guest reading a stale status next to a
 * fresh information, so a failure to write either is reported. */
static bool write_io_status(kernel_guest_ptr address, uint32_t status,
                            uint32_t information)
{
    if (address == 0u) {
        /* Every measured site passes a real stack local, so a null here is not a
         * shape the guest uses. Tolerated rather than refused, because the status is
         * also returned in eax and 66 of the 68 measured sites branch on that
         * instead -- but reported, because it means our argument order may be wrong. */
        kernel_hle_log()("kernel: NtOpenFile called with a null IoStatusBlock, which "
                         "no measured call site does\n");
        return true;
    }
    return kernel_guest_write_u32(GUEST_FIELD(address, guest_io_status_block, status),
                                  status) &&
           kernel_guest_write_u32(GUEST_FIELD(address, guest_io_status_block, information),
                                  information);
}

/*
 * ARITY-OK(202): 6 stack arguments. The measured table says 6 over 12 sites but flags
 * it NON-UNANIMOUS, so `stack_args_for()` refuses it; the count was confirmed by hand
 * at six sites, and the single disagreeing site (0x0037D887) is explained -- its
 * leading `push esi` is a register save, settled by 0x00433F87 being the same code
 * shape with the same three argument literals and no such push. A 13th site reaching
 * the thunk through a register is invisible to the measurement. Site table in
 * kernel_file.h.
 */
static uint32_t open_file(void *context, bool stored_only)
{
    if (!context) {
        kernel_hle_log()("kernel: NtOpenFile called with no argument frame -- the call "
                         "boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[6];
    for (unsigned i = 0u; i < 6u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtOpenFile could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr handle_out = args[0];
    const uint32_t desired_access = args[1];
    const kernel_guest_ptr object_attributes = args[2];
    const kernel_guest_ptr io_status = args[3];
    const uint32_t share_access = args[4];
    const uint32_t open_options = args[5];

    if (handle_out == 0u || object_attributes == 0u) {
        kernel_hle_log()("kernel: NtOpenFile has no handle out-parameter (%#x) or no "
                         "OBJECT_ATTRIBUTES (%#x)\n",
                         (unsigned)handle_out, (unsigned)object_attributes);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    if (!read_object_attributes(object_attributes, &root_directory, &object_name,
                                &attributes)) {
        kernel_hle_log()("kernel: NtOpenFile could not read the OBJECT_ATTRIBUTES at "
                         "%#x\n",
                         (unsigned)object_attributes);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * THE NAME IS READ BEFORE ANYTHING IS WRITTEN, and that is load-bearing rather
     * than tidy. At the reached site the handle out-parameter IS the slot holding the
     * pointer to the name: `lea eax,[ebp+8]` is argument 0 and `[ebp+8]` is what
     * OBJECT_ATTRIBUTES.object_name points at. Writing the handle first would destroy
     * the name before reading it, and the symptom would be a garbage path rather than
     * anything pointing at the cause.
     */
    char path[KERNEL_FILE_PATH_MAX];
    path[0] = '\0';
    if (object_name == 0u ||
        !read_object_string(object_name, path, sizeof(path))) {
        kernel_hle_log()("kernel: NtOpenFile could not read the OBJECT_STRING at %#x\n",
                         (unsigned)object_name);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    report_unmodelled_attributes("NtOpenFile", path, attributes);
    if (!read_object_path("NtOpenFile", root_directory, path, sizeof(path))) {
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND, 0u);
        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    lock();
    kernel_file_attempt *attempt =
        record_attempt_locked(path, desired_access, share_access, open_options);
    if (!attempt) {
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") not recorded -- the %u-entry "
                         "attempt log is full, so the run report is now incomplete\n",
                         path, KERNEL_FILE_ATTEMPT_MAX);
    }
    /*
     * RESOLVE FIRST, ALLOCATE SECOND. A refused open must consume nothing: the real
     * kernel leaves the handle out-parameter untouched and issues no handle, and
     * `test_kernel_file.c` asserts `kernel_object_live_count() == 0` after a failed open.
     * This title probes for names that cannot resolve -- its whole `host0:` developer
     * tree is absent from a retail disc -- so a handle leaked per failure would be a
     * steady drain rather than a corner case.
     */
    uint32_t resolve_status = STATUS_SUCCESS;
    bool fabricating = false;
    resolution resolved_to;
    if (!resolve_backing_locked(path, desired_access, &resolved_to, &resolve_status,
                                &fabricating, !stored_only)) {
        unlock();
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") REFUSED with status %#010x "
                         "(access %#x share %#x options %#x)\n",
                         path, (unsigned)resolve_status, (unsigned)desired_access,
                         (unsigned)share_access, (unsigned)open_options);
        (void)write_io_status(io_status, resolve_status, 0u);
        return resolve_status;
    }

    if (stored_only && (fabricating || resolved_to.backing == KERNEL_FILE_BACKING_EMPTY)) {
        release_resolution_locked(&resolved_to);
        unlock();
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND, 0u);
        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, ORD_NtOpenFile);
    if (handle == 0u) {
        release_resolution_locked(&resolved_to);
        unlock();
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") could not issue a handle -- the "
                         "object table is full\n",
                         path);
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES, 0u);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!claim_open_locked(path, handle, &resolved_to, open_options)) {
        release_resolution_locked(&resolved_to);
        unlock();
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES, 0u);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    unlock();
    if (!kernel_guest_write_u32(handle_out, handle)) {
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") could not write the handle to "
                         "%#x\n",
                         path, (unsigned)handle_out);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }
    if (!write_io_status(io_status, STATUS_SUCCESS, FILE_INFORMATION_OPENED)) {
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") could not write the "
                         "IO_STATUS_BLOCK at %#x\n",
                         path, (unsigned)io_status);
        return STATUS_INVALID_PARAMETER;
    }

    lock();
    if (attempt) {
        attempt->opened = true;
    }
    unlock();

    if (fabricating) {
        kernel_hle_log()("kernel: NtOpenFile(\"%s\") -> handle %#x for an EMPTY, "
                         "FABRICATED file (no volume is mounted behind that name; "
                         "everything the title reads through this handle is ours)\n",
                         path, (unsigned)handle);
    }
    return STATUS_SUCCESS;
}

/*
 * NtQueryFullAttributesFile (210) support: resolve a name and report what it is, with no
 * handle. Shares NtOpenFile's reader, drive-root policy and resolver (so a name this module
 * refuses to open is refused here for the same reason, and a name it opens is found here),
 * but records no open attempt, because that log is the run report of what the title tried
 * to OPEN. Resolution takes a read-only descriptor, which is used for fstat and closed
 * before this returns, so nothing outlives the call.
 *
 * TIMES. Only a HOST_DIR object has any. XDVDFS carries none per entry (see kernel_io.c) and
 * an EMPTY object is ours, so those report 0 and `times_known` is false.
 */
static bool host_object_times_locked(const resolution *from, kernel_file_attributes *out)
{
    const int descriptor = from->host_fd_plus_one != 0 ? from->host_fd_plus_one - 1
                           : from->dir_fd_plus_one != 0 ? from->dir_fd_plus_one - 1
                                                        : -1;
    struct stat info;
    if (descriptor < 0 || fstat(descriptor, &info) != 0) {
        return false;
    }
    out->last_access_time = dir_filetime(info.st_atim.tv_sec, info.st_atim.tv_nsec);
    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, info.st_mtim.tv_nsec);
    /* Linux keeps no birth time through this interface, so creation takes the last-write
     * time, as the directory listing does (INFERRED). */
    out->creation_time = out->last_write_time;
    return true;
}

uint32_t kernel_file_query_attributes(kernel_guest_ptr object_attributes,
                                      kernel_file_attributes *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    if (object_attributes == 0u ||
        !read_object_attributes(object_attributes, &root_directory, &object_name,
                                &attributes)) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not read the "
                         "OBJECT_ATTRIBUTES at %#x\n",
                         (unsigned)object_attributes);
        return STATUS_INVALID_PARAMETER;
    }
    char path[KERNEL_FILE_PATH_MAX];
    path[0] = '\0';
    if (object_name == 0u || !read_object_string(object_name, path, sizeof(path))) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not read the OBJECT_STRING "
                         "at %#x\n",
                         (unsigned)object_name);
        return STATUS_INVALID_PARAMETER;
    }
    report_unmodelled_attributes("NtQueryFullAttributesFile", path, attributes);
    if (!read_object_path("NtQueryFullAttributesFile", root_directory, path, sizeof(path))) {
        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    lock();
    attribute_query_count++;
    uint32_t status = STATUS_SUCCESS;
    bool fabricating = false;
    resolution resolved_to;
    if (!resolve_locked(path, 0u, &resolved_to, &status, &fabricating)) {
        unlock();
        kernel_hle_log()("kernel: NtQueryFullAttributesFile(\"%s\") REFUSED with status "
                         "%#010x\n",
                         path, (unsigned)status);
        return status;
    }
    out->backing = resolved_to.backing;
    out->is_directory = resolved_to.is_directory;
    out->size = resolved_to.size;
    if (resolved_to.backing == KERNEL_FILE_BACKING_HOST_DIR) {
        out->times_known = host_object_times_locked(&resolved_to, out);
    }
    release_resolution_locked(&resolved_to);
    unlock();
    if (fabricating) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile(\"%s\") -> an EMPTY, FABRICATED "
                         "file (no volume is mounted behind that name)\n",
                         path);
    }
    return STATUS_SUCCESS;
}

/*
 * NtDeleteFile (195): remove one named FILE from a host-directory volume. INFERRED, NT contract,
 * for everything but the one measured mode.
 *
 * MEASURED, the only site is 0x004228EC in the XONLINE local-cache closer 0x0042287C. It closes
 * the handle it keeps for the slot, formats `\Device\Harddisk0\partition1\CACHE%.4s\
 * LocalCache%02d.bin` (index 2, 3 or 4), builds an OBJECT_ATTRIBUTES {root 0, name, attributes
 * 0x40} on its stack and calls NtDeleteFile(&oa). Only the sign of the result is read. That
 * is a FILE, named from root 0, in a `CACHE....` directory of partition1 (the title's data
 * volume, a host-directory volume here), with no handle open on it.
 *
 * Resolution is NtOpenFile's (same reader, same drive-root policy, symbolic links, volume match,
 * cache-view FATX gate and the descriptor-walk escape guarantee), then the leaf goes with
 * `unlinkat` on the walk's own directory descriptor, so no host path is ever assembled.
 *
 * REFUSED LOUDLY, status returned and nothing removed, because none of it is measured:
 *   - a directory (NT would remove an empty one, this host does not guess it): NOT_IMPLEMENTED,
 *   - the volume root or a raw device: ACCESS_DENIED,
 *   - a disc volume (never written): ACCESS_DENIED,
 *   - a name no volume backs that the open path would still answer as a file (declared openable,
 *     or fabricated under the EMPTY policy; no host file exists): NOT_IMPLEMENTED. An undeclared
 *     name under the FAIL policy is an honest NAME_NOT_FOUND,
 *   - an escape, a ".." component or a symbolic link on the way or as the leaf: ACCESS_DENIED,
 *   - a file a LIVE handle still holds: SHARING_VIOLATION (INFERRED). A closed handle's slot is
 *     reclaimed lazily and is not live, so the closer's close-then-delete works,
 *   - a host failure: ACCESS_DENIED for EACCES/EPERM/EROFS, else UNSUCCESSFUL.
 * An absent name or parent is an honest NAME_NOT_FOUND or PATH_NOT_FOUND, not a refusal.
 *
 * ARITY-OK(195): ONE stack argument, `ret` pops 4 through the hand ABI row (NtDeleteFile@4 in
 * the nxdk .def, the oracle agrees, the measured row is one voter).
 */
static bool open_slot_holds_locked(const struct stat *target)
{
    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {
        const open_entry *slot = &open_files[i];
        if (!slot->in_use || slot->host_fd_plus_one == 0 ||
            !kernel_object_file_identity_live(slot->state.handle)) {
            continue;
        }
        struct stat held;
        if (fstat(slot->host_fd_plus_one - 1, &held) == 0 && held.st_dev == target->st_dev &&
            held.st_ino == target->st_ino) {
            return true;
        }
    }
    return false;
}

static uint32_t hostdir_delete_locked(volume_entry *volume, const char *guest_path,
                                      const char *rest)
{
    if (names_raw_view(volume, rest)) {
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") names a virtual raw device, which is "
                         "not a file this host deletes -- REFUSED\n",
                         guest_path);
        return KERNEL_FILE_STATUS_ACCESS_DENIED;
    }
    if (volume->cache_partition != 0u) {
        uint32_t gate = STATUS_SUCCESS;
        if (!cache_view_prepare_locked(volume, guest_path, &gate)) {
            return gate;
        }
    }
    char leaf[KERNEL_FILE_PATH_MAX];
    uint32_t status = STATUS_SUCCESS;
    const int dir_fd = hostdir_walk_locked(volume, guest_path, rest, leaf, sizeof(leaf), &status);
    if (dir_fd < 0) {
        return status;
    }
    if (leaf[0] == '\0') {
        (void)close(dir_fd);
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") names the volume root -- REFUSED\n",
                         guest_path);
        return KERNEL_FILE_STATUS_ACCESS_DENIED;
    }
    char real[KERNEL_FILE_PATH_MAX];
    struct stat info;
    if (!hostdir_real_name_locked(dir_fd, leaf, real, sizeof(real), &info)) {
        (void)close(dir_fd);
        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (S_ISDIR(info.st_mode)) {
        (void)close(dir_fd);
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") names a directory, and deleting one is "
                         "not a measured mode (the title deletes a file) -- REFUSED, nothing "
                         "removed\n",
                         guest_path);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
        (void)close(dir_fd);
        if (S_ISLNK(info.st_mode)) {
            escape_refused_count++;
        }
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") names \"%s\", which is a host %s -- "
                         "REFUSED, nothing removed\n",
                         guest_path, real,
                         S_ISLNK(info.st_mode) ? "SYMBOLIC LINK" : "object that is not a file");
        return KERNEL_FILE_STATUS_ACCESS_DENIED;
    }
    if (open_slot_holds_locked(&info)) {
        (void)close(dir_fd);
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") -- a live handle still holds this file "
                         "open: SHARING VIOLATION (INFERRED), nothing removed. The measured "
                         "caller closes its handle first\n",
                         guest_path);
        return KERNEL_FILE_STATUS_SHARING_VIOLATION;
    }
    const int result = unlinkat(dir_fd, real, 0);
    const int failure = errno;
    (void)close(dir_fd);
    if (result != 0) {
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") -> \"%s\" could not be removed: %s\n",
                         guest_path, real, strerror(failure));
        if (failure == ENOENT) {
            return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return (failure == EACCES || failure == EPERM || failure == EROFS)
                   ? KERNEL_FILE_STATUS_ACCESS_DENIED
                   : STATUS_UNSUCCESSFUL;
    }
    deleted_count++;
    kernel_hle_log()("kernel: NtDeleteFile(\"%s\") -> DELETED \"%s\" under \"%s\" -- a real "
                     "host file removed\n",
                     guest_path, real, volume->host_root);
    return STATUS_SUCCESS;
}

static uint32_t hle_nt_delete_file(void *context)
{
    /* kernel_frame_arg refuses a NULL frame, so a missing frame lands here too. */
    uint32_t object_attributes = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &object_attributes)) {
        kernel_hle_log()("kernel: NtDeleteFile could not read its argument from the guest "
                         "stack (or has no argument frame)\n");
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    if (object_attributes == 0u ||
        !read_object_attributes(object_attributes, &root_directory, &object_name,
                                &attributes)) {
        kernel_hle_log()("kernel: NtDeleteFile could not read the OBJECT_ATTRIBUTES at %#x\n",
                         (unsigned)object_attributes);
        return STATUS_INVALID_PARAMETER;
    }
    char path[KERNEL_FILE_PATH_MAX];
    path[0] = '\0';
    if (object_name == 0u || !read_object_string(object_name, path, sizeof(path))) {
        kernel_hle_log()("kernel: NtDeleteFile could not read the OBJECT_STRING at %#x\n",
                         (unsigned)object_name);
        return STATUS_INVALID_PARAMETER;
    }
    report_unmodelled_attributes("NtDeleteFile", path, attributes);
    if (!read_object_path("NtDeleteFile", root_directory, path, sizeof(path))) {
        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    lock();
    char resolved[KERNEL_FILE_PATH_MAX];
    volume_entry *volume = NULL;
    const char *rest = NULL;
    uint32_t status = STATUS_SUCCESS;
    if (!locate_volume_locked(path, resolved, sizeof(resolved), &volume, &rest, &status)) {
        /* A link walk that did not settle: an absence, answered as the open path answers it. */
    } else if (volume == NULL) {
        /* The open path's own question: is the name declared, or would the missing policy
         * invent an empty file for it. Undeclared under FAIL is an honest absence. Anything
         * the open path would hand back as a file has no host file behind it to remove. */
        if (!is_openable_locked(path) && missing_policy == KERNEL_FILE_MISSING_FAIL) {
            status = KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
        } else {
            kernel_hle_log()("kernel: NtDeleteFile(\"%s\") -- no volume is mounted behind that "
                             "name, so there is no host file to remove (a declared or "
                             "fabricated empty file is not one) -- REFUSED\n",
                             path);
            status = STATUS_NOT_IMPLEMENTED;
        }
    } else if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR) {
        kernel_hle_log()("kernel: NtDeleteFile(\"%s\") is on a disc image, which is never "
                         "written -- REFUSED\n",
                         path);
        status = KERNEL_FILE_STATUS_ACCESS_DENIED;
    } else {
        status = hostdir_delete_locked(volume, path, rest);
    }
    if (status != STATUS_SUCCESS && status != KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND &&
        status != KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND) {
        delete_refused_count++;
    }
    unlock();
    return status;
}

/*
 * ARITY-OK(67): 2 stack arguments, (SymbolicLinkName, DeviceName). The measured table
 * agrees -- 2 over 5 sites, UNANIMOUS, so `stack_args_for()` would accept it anyway --
 * and it was still checked by hand at 0x00381301, where both arguments are pointers to
 * compile-time OBJECT_STRINGs whose contents settle the ORDER beyond doubt:
 *
 *     push 0x5491DC              ; arg1 -> OBJECT_STRING len 14 -> "\Device\CdRom0"
 *     push 0x5491D4              ; arg0 -> OBJECT_STRING len 6  -> "\??\D:"
 *     call dword ptr [0x475804]
 *
 * A device cannot be an alias for a drive letter, so arg0 is the LINK and arg1 is the
 * TARGET. Corroborated at 0x0037DB6B, where the roles are reversed in form but not in
 * meaning: there arg0 is the literal `\??\Z:` and arg1 is a string the function just
 * built, i.e. the computed side is again the target.
 *
 * THIS IS THE ORDINAL THAT MAKES `D:` MEAN THE DISC, and it does so because the guest
 * says it does. See kernel_file.h.
 */
static uint32_t hle_io_create_symbolic_link(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: IoCreateSymbolicLink called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: IoCreateSymbolicLink could not read argument %u "
                             "from the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }

    char name[KERNEL_FILE_PATH_MAX];
    char target[KERNEL_FILE_PATH_MAX];
    if (args[0] == 0u || !read_object_string(args[0], name, sizeof(name))) {
        kernel_hle_log()("kernel: IoCreateSymbolicLink could not read the link name's "
                         "OBJECT_STRING at %#x\n",
                         (unsigned)args[0]);
        return STATUS_INVALID_PARAMETER;
    }
    if (args[1] == 0u || !read_object_string(args[1], target, sizeof(target))) {
        kernel_hle_log()("kernel: IoCreateSymbolicLink(\"%s\") could not read the "
                         "target's OBJECT_STRING at %#x\n",
                         name, (unsigned)args[1]);
        return STATUS_INVALID_PARAMETER;
    }

    lock();
    const bool existed = symlink_target_locked(name) != NULL;
    unlock();
    if (existed) {
        /* The guest TOLERATES this: at 0x0038130B it compares the result against
         * 0xC0000035 and treats that value as success. So a duplicate is reported with
         * exactly that status rather than being silently overwritten, because
         * overwriting would hide a title that is relinking a drive letter on purpose. */
        kernel_hle_log()("kernel: IoCreateSymbolicLink(\"%s\") -- a link of that name "
                         "already exists, reporting a collision (which the site at "
                         "0x0038130B accepts)\n",
                         name);
        return KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION;
    }

    if (!kernel_file_add_symlink(name, target)) {
        kernel_hle_log()("kernel: IoCreateSymbolicLink(\"%s\" -> \"%s\") could not be "
                         "recorded -- the %u-entry link table is full\n",
                         name, target, KERNEL_FILE_SYMLINK_MAX);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }

    kernel_hle_log()("kernel: IoCreateSymbolicLink \"%s\" -> \"%s\" (the title's own "
                     "alias; paths through it now resolve to whatever is mounted "
                     "behind the target)\n",
                     name, target);
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(69): 1 stack argument, the SymbolicLinkName. The measured table says 1 over
 * 4 sites, UNANIMOUS, and it was checked by hand at 0x0037DBCF, where the single push
 * is the OBJECT_STRING that `RtlInitAnsiString` filled in the two instructions before:
 *
 *     lea eax,[ebp-8]; push eax; call [0x475784]   ; RtlInitAnsiString(&str, name)
 *     lea eax,[ebp-8]; push eax
 *     call dword ptr [0x475810]                    ; IoDeleteSymbolicLink(&str)
 *
 * One argument, and it is unambiguously the link's own name rather than a target.
 */
static uint32_t hle_io_delete_symbolic_link(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: IoDeleteSymbolicLink called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t name_ptr = 0u;
    if (!kernel_frame_arg(frame, 0u, &name_ptr)) {
        kernel_hle_log()("kernel: IoDeleteSymbolicLink could not read its argument from "
                         "the guest stack\n");
        return STATUS_INVALID_PARAMETER;
    }

    char name[KERNEL_FILE_PATH_MAX];
    if (name_ptr == 0u || !read_object_string(name_ptr, name, sizeof(name))) {
        kernel_hle_log()("kernel: IoDeleteSymbolicLink could not read the OBJECT_STRING "
                         "at %#x\n",
                         (unsigned)name_ptr);
        return STATUS_INVALID_PARAMETER;
    }

    if (!kernel_file_remove_symlink(name)) {
        /* Not an internal error: the title deletes a link before creating it, at
         * 0x00380B4B immediately before the create at 0x00380B56, which is the ordinary
         * relink idiom. Reported at low volume and answered with the not-found status
         * the real kernel would give. */
        kernel_hle_log()("kernel: IoDeleteSymbolicLink(\"%s\") -- no such link; this is "
                         "the ordinary delete-then-create idiom, not a fault\n",
                         name);
        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    kernel_hle_log()("kernel: IoDeleteSymbolicLink \"%s\" removed\n", name);
    return STATUS_SUCCESS;
}

/* Callers hold the lock. Reclaims slots whose handle the guest closed, by asking the object
 * table, exactly as claim_open_locked does for files: NtClose offers no hook. */
static symlink_handle_entry *claim_symlink_handle_locked(uint32_t handle,
                                                         const char *target)
{
    symlink_handle_entry *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_HANDLE_MAX; i++) {
        if (symlink_handles[i].in_use &&
            kernel_object_find(symlink_handles[i].handle) == NULL) {
            memset(&symlink_handles[i], 0, sizeof(symlink_handles[i]));
        }
        if (!symlink_handles[i].in_use && !slot) {
            slot = &symlink_handles[i];
        }
    }
    if (!slot) {
        return NULL;
    }
    slot->handle = handle;
    memcpy(slot->target, target, strlen(target) + 1u);
    slot->in_use = true;
    return slot;
}

/*
 * ARITY-OK(203): TWO stack arguments, (PHANDLE LinkHandle, POBJECT_ATTRIBUTES). Desktop NT
 * takes THREE here (an ACCESS_MASK in the middle), so a reader checking against NT would
 * expect 3. The measured row `{203u, 2u, 3u, 0}` is non-unanimous, so `stack_args_for()`
 * refuses it and the count comes from the oracle (`NtOpenSymbolicLinkObject@8` in nxdk's
 * xboxkrnl.exe.def, a DIFFERENT kernel build) unless a hand row is added. It was read by
 * hand at all three call sites, and the disagreement is explained:
 *
 *     0x0037F8F2  function prologue `sub esp,0x120`, then `and [ebp-0x1c],0; lea eax,
 *                 [ebp-0x1c]; push eax; lea eax,[ebp-4]; push eax; call` -- the window
 *                 between the prologue and the call holds exactly two pushes
 *     0x0038047F  a leading `push esi`, which is a callee-save: the same function ends
 *                 `pop esi; leave; ret 0xc` at 0x00380508. THIS IS THE NON-UNANIMOUS SITE.
 *     0x0038059B  a leading `push edi`, paired with `pop edi` at 0x003805FE
 *
 * ARGUMENT ORDER is pinned by what each push points at: arg1 is a stack block whose three
 * dwords the guest fills in just before (RootDirectory = 0, ObjectName = a static
 * OBJECT_STRING, Attributes = 0x40), which is exactly the 12-byte OBJECT_ATTRIBUTES
 * `guest_structs.h` measures, and arg0 is a slot the guest later passes to NtClose.
 *
 * Reached at 0x003805A1 as the 173rd recorded call of the boot (the 171st kernel ordinal), opening `\??\D:`.
 */
static uint32_t hle_nt_open_symbolic_link_object(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject called with no argument "
                         "frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtOpenSymbolicLinkObject could not read argument "
                             "%u from the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr handle_out = args[0];
    const kernel_guest_ptr object_attributes = args[1];
    if (handle_out == 0u || object_attributes == 0u) {
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject has no handle out-parameter "
                         "(%#x) or no OBJECT_ATTRIBUTES (%#x)\n",
                         (unsigned)handle_out, (unsigned)object_attributes);
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    char name[KERNEL_FILE_PATH_MAX];
    /* Everything is read before anything is written. NtOpenFile's reached site aliases its
     * handle slot with the name pointer slot. None of these three sites does, so this is
     * robustness and not a measured need. */
    if (!read_object_attributes(object_attributes, &root_directory, &object_name,
                                &attributes) ||
        object_name == 0u || !read_object_string(object_name, name, sizeof(name))) {
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject could not read the "
                         "OBJECT_ATTRIBUTES at %#x or the name it points at\n",
                         (unsigned)object_attributes);
        return STATUS_INVALID_PARAMETER;
    }
    report_unmodelled_attributes("NtOpenSymbolicLinkObject", name, attributes);
    if (!read_object_path("NtOpenSymbolicLinkObject", root_directory, name, sizeof(name))) {
        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    char target[KERNEL_FILE_PATH_MAX];
    lock();
    const char *recorded = symlink_target_locked(name);
    if (!recorded) {
        unlock();
        /* An honest miss, not a fault: the title probes `\??\W:` at 0x0037F8F2 and handles
         * a failure by returning 0. Only links the title itself created are known. */
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject(\"%s\") -- no such link was "
                         "created by the title, reporting STATUS_OBJECT_NAME_NOT_FOUND\n",
                         name);
        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    memcpy(target, recorded, strlen(recorded) + 1u);

    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_SYMLINK,
                                                 ORD_NtOpenSymbolicLinkObject);
    if (handle == 0u) {
        unlock();
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject(\"%s\") could not issue a "
                         "handle -- the object table is full\n",
                         name);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!claim_symlink_handle_locked(handle, target)) {
        unlock();
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject(\"%s\") -- all %u link-handle "
                         "slots are in use\n",
                         name, KERNEL_FILE_SYMLINK_HANDLE_MAX);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    unlock();
    if (!kernel_guest_write_u32(handle_out, handle)) {
        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject(\"%s\") could not write the "
                         "handle to %#x\n",
                         name, (unsigned)handle_out);
        return STATUS_INVALID_PARAMETER;
    }
    kernel_hle_log()("kernel: NtOpenSymbolicLinkObject \"%s\" -> handle %#x (target "
                     "\"%s\", copied from the link the title created)\n",
                     name, (unsigned)handle, target);
    return STATUS_SUCCESS;
}

/* The 16-bit OBJECT_STRING.length, written as two bytes for the reason read_object_string
 * reads it that way: a dword write would clobber maximum_length. */
static bool write_object_string_length(kernel_guest_ptr string, uint16_t length)
{
    const uint32_t at = (uint32_t)offsetof(guest_object_string, length);
    return kernel_guest_write_u8(kernel_guest_add(string, at), (uint8_t)(length & 0xFFu)) &&
           kernel_guest_write_u8(kernel_guest_add(string, at + 1u), (uint8_t)(length >> 8));
}

/*
 * ARITY-OK(215): THREE stack arguments, (HANDLE LinkHandle, POBJECT_STRING LinkTarget,
 * PULONG ReturnedLength OPTIONAL). The measured row `{215u, 3u, 3u, 1}` is unanimous over
 * three sites and `NtQuerySymbolicLinkObject@12` in xboxkrnl.exe.def agrees, so
 * `stack_args_for()` accepts it with no hand row. Read by hand anyway: at 0x003805C3 the
 * three pushes are `push 0` (a literal NULL), `lea eax,[ebp-8]` (the STRING the guest
 * initialised with MaximumLength 0x208 and Buffer = a stack array) and `push [ebp+8]` (the
 * handle just written by ordinal 203), and a leading `push esi` that is a callee-save
 * popped at 0x003805D6.
 *
 * WHAT THE GUEST PINS ABOUT THE OUTPUT, MEASURED:
 *   - ReturnedLength is a DWORD, read with `mov esi,[ebp-8]` at 0x0037F930, and it is
 *     used as the string length: the code then reads Buffer[esi-9] and requires a `\`, and
 *     parses Buffer[esi-8 .. esi-1] as eight hex digits. So it is the length WITHOUT the
 *     NUL (a NUL-inclusive count would make the last "digit" a NUL and the parse fail).
 *     That is one site and a consequence of the code's shape, not a documented contract,
 *     so it is INFERRED rather than MEASURED against a console.
 *   - At 0x0037F915 the guest sets MaximumLength but NEVER INITIALISES Length, so the
 *     incoming Length is ignored and only the outgoing one is written.
 *   - The other two sites pass NULL for ReturnedLength and read STRING.Length instead, so
 *     both must be written.
 * No NUL is written after the characters. The consumer at 0x00380BA8 copies by count and
 * overwrites the last byte it copied when the count runs out, so it tolerates both, and a
 * terminator no measured site needs would be invented behaviour.
 */
static uint32_t hle_nt_query_symbolic_link_object(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject called with no argument "
                         "frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtQuerySymbolicLinkObject could not read argument "
                             "%u from the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t handle = args[0];
    const kernel_guest_ptr string = args[1];
    const kernel_guest_ptr returned_length = args[2];

    const kernel_object_entry *object = kernel_object_find(handle);
    if (!object) {
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- not a handle this "
                         "host issued\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }
    if (object->kind != KERNEL_OBJECT_SYMLINK) {
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- the handle is not a "
                         "symbolic link\n",
                         (unsigned)handle);
        return KERNEL_FILE_STATUS_OBJECT_TYPE_MISMATCH;
    }

    char target[KERNEL_FILE_PATH_MAX];
    bool found = false;
    lock();
    for (unsigned i = 0u; i < KERNEL_FILE_SYMLINK_HANDLE_MAX; i++) {
        if (symlink_handles[i].in_use && symlink_handles[i].handle == handle) {
            memcpy(target, symlink_handles[i].target,
                   strlen(symlink_handles[i].target) + 1u);
            found = true;
            break;
        }
    }
    unlock();
    if (!found) {
        /* The object table says it is a link and this module has no record of it. That is
         * an internal inconsistency and is said out loud rather than answered. */
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- a link handle with no "
                         "recorded target, an internal inconsistency\n",
                         (unsigned)handle);
        return STATUS_UNSUCCESSFUL;
    }

    uint8_t max_low = 0u;
    uint8_t max_high = 0u;
    uint32_t buffer = 0u;
    if (string == 0u ||
        !kernel_guest_read_u8(GUEST_FIELD(string, guest_object_string, maximum_length),
                              &max_low) ||
        !kernel_guest_read_u8(GUEST_FIELD_BYTE(string, guest_object_string, maximum_length, 1u),
                              &max_high) ||
        !kernel_guest_read_u32(GUEST_FIELD(string, guest_object_string, buffer),
                               &buffer) ||
        buffer == 0u) {
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) could not read the "
                         "output OBJECT_STRING at %#x\n",
                         (unsigned)handle, (unsigned)string);
        return STATUS_INVALID_PARAMETER;
    }
    const uint32_t capacity = (uint32_t)max_low | ((uint32_t)max_high << 8);
    const uint32_t length = (uint32_t)strlen(target);

    if (length > capacity) {
        /* INFERRED from desktop NT, and unreached by the measured sites. */
        if (returned_length != 0u) {
            (void)kernel_guest_write_u32(returned_length, length);
        }
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- the target is %u bytes "
                         "and the caller's MaximumLength is %u, STATUS_BUFFER_TOO_SMALL\n",
                         (unsigned)handle, (unsigned)length, (unsigned)capacity);
        return KERNEL_FILE_STATUS_BUFFER_TOO_SMALL;
    }
    for (uint32_t i = 0u; i < length; i++) {
        if (!kernel_guest_write_u8(kernel_guest_add(buffer, i), (uint8_t)target[i])) {
            kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) could not write the "
                             "target to %#x\n",
                             (unsigned)handle, (unsigned)buffer);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (!write_object_string_length(string, (uint16_t)length) ||
        (returned_length != 0u && !kernel_guest_write_u32(returned_length, length))) {
        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) could not write the "
                         "length back\n",
                         (unsigned)handle);
        return STATUS_INVALID_PARAMETER;
    }
    kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -> \"%s\" (%u bytes)\n",
                     (unsigned)handle, target, (unsigned)length);
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(190): NINE stack arguments. THIS SETTLES A DOCUMENTED CONFLICT.
 *
 * `docs/clean-sources-audit.md` records an irreducible gap: four independent sources say
 * NtCreateFile takes 9 parameters, and a fifth claim says 11. The two are 36 bytes and
 * 44 bytes of stack arguments, and since our handler is the __stdcall callee that has to
 * pop them, picking the wrong one desyncs the guest's `esp` by 8 bytes permanently.
 *
 * The conflict was not resolved by preferring a source. It was resolved by COUNTING THE
 * PUSHES IN THIS BINARY, at all 12 reachable call sites. Every one pushes exactly NINE,
 * and no site stages arguments with `sub esp, N` + `mov [esp+k]` -- every argument is a
 * real `push`, so there is no hidden staging to miscount. (Two of the 12 reach the thunk
 * through `edi` loaded from the slot and are invisible to the measured scanner, which is
 * why the count was done by hand.)
 *
 * So: 9, i.e. NT's NtCreateFile without the trailing EaBuffer and EaLength. The
 * 11-parameter claim is refuted FOR THIS IMAGE, which is the only claim that matters.
 *
 * ORDER, pinned at 0x00380127 where every argument but two is a literal:
 *
 *     push 0x4021                   ; arg8 CreateOptions
 *     push 2                        ; arg7 CreateDisposition
 *     push 3                        ; arg6 ShareAccess
 *     push 0x80                     ; arg5 FileAttributes
 *     push 0                        ; arg4 AllocationSize  -- a NULL pointer
 *     lea eax,[ebp-0x10]; push eax  ; arg3 IoStatusBlock
 *     lea eax,[ebp-0x1c]; push eax  ; arg2 ObjectAttributes
 *     push 0x100001                 ; arg1 DesiredAccess
 *     lea eax,[ebp+8];   push eax   ; arg0 FileHandle out
 *
 * arg4 is confirmed to be AllocationSize independently at 0x003DF797, where a local
 * 64-bit value is clamped against 0x40000000 immediately before being passed there -- a
 * clamp that only makes sense on a size.
 *
 * WHAT THIS HANDLER WILL AND WILL NOT DO. It resolves the name exactly as NtOpenFile
 * does, so a path into a mounted disc returns the real file. It CREATES only on a
 * KERNEL_FILE_BACKING_HOST_DIR volume, which exists only because an operator mounted one,
 * and only when the disposition is a creating one. The disc cannot reach that path: the
 * create helper refuses any other backing, and the image descriptor is O_RDONLY anyway. A
 * creating disposition on a name with no writable volume behind it is REFUSED exactly as
 * it always was, unless the missing-file policy says otherwise, in which case the
 * resulting empty file is announced as fabricated like any other. Reporting a successful
 * create and then serving zeros is the failure this whole module is built to avoid.
 *
 * TWO THINGS IT NOW DOES THAT IT DID NOT. The descriptor it opens is O_RDWR when the
 * create asked for write access, which the measured TitleMeta.xbx create at 0x00381027
 * does with DesiredAccess 0x40100000 -- without that, the title's very next NtWriteFile
 * would be refused on a file this handler had just made for it. And an OVERWRITING
 * disposition on an existing name TRUNCATES, reporting FILE_SUPERSEDED or FILE_OVERWRITTEN
 * rather than FILE_OPENED, or is REFUSED where this host cannot truncate. Neither
 * disposition is reached by this boot; both were silently wrong before.
 *
 * WHY IT HAD TO LEARN TO CREATE. See kernel_file.h: the title was REBOOTING ITSELF
 * because this handler refused, and three calls later the reboot path trapped on a NULL
 * function pointer that read like a lifter gap.
 */
static uint32_t hle_nt_create_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtCreateFile called with no argument frame -- the "
                         "call boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[9];
    for (unsigned i = 0u; i < 9u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtCreateFile could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr handle_out = args[0];
    const uint32_t desired_access = args[1];
    const kernel_guest_ptr object_attributes = args[2];
    const kernel_guest_ptr io_status = args[3];
    const uint32_t share_access = args[6];
    const uint32_t disposition = args[7];
    const uint32_t create_options = args[8];

    if (handle_out == 0u || object_attributes == 0u) {
        kernel_hle_log()("kernel: NtCreateFile has no handle out-parameter (%#x) or no "
                         "OBJECT_ATTRIBUTES (%#x)\n",
                         (unsigned)handle_out, (unsigned)object_attributes);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    if (!read_object_attributes(object_attributes, &root_directory, &object_name,
                                &attributes)) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    /* The name is read before anything is written, for the same reason NtOpenFile does
     * it: at 0x00380127 the handle out-parameter is `lea eax,[ebp+8]` while
     * OBJECT_ATTRIBUTES.object_name is built from the same frame, so writing the handle
     * first can destroy the name about to be read. */
    char path[KERNEL_FILE_PATH_MAX];
    path[0] = '\0';
    if (object_name == 0u || !read_object_string(object_name, path, sizeof(path))) {
        kernel_hle_log()("kernel: NtCreateFile could not read the OBJECT_STRING at "
                         "%#x\n",
                         (unsigned)object_name);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }

    report_unmodelled_attributes("NtCreateFile", path, attributes);
    if (!read_object_path("NtCreateFile", root_directory, path, sizeof(path))) {
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND, 0u);
        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    lock();
    kernel_file_attempt *attempt =
        record_attempt_locked(path, desired_access, share_access, create_options);

    /* Resolve first, allocate second, for the reason given at the same point in
     * NtOpenFile: a refused open must consume no handle. */
    uint32_t resolve_status = STATUS_SUCCESS;
    bool fabricating = false;
    resolution resolved_to;
    bool resolved =
        resolve_locked(path, desired_access, &resolved_to, &resolve_status, &fabricating);
    uint32_t information = FILE_INFORMATION_OPENED;

    if (resolved && disposition == FILE_CREATE) {
        /* FILE_CREATE means "create it, and fail if it is already there". Answering
         * success would tell the title it had just made a fresh object over data already
         * sitting in it, which on a volume holding save games is the one lie that costs a
         * user something real. */
        release_resolution_locked(&resolved_to);
        unlock();
        kernel_hle_log()("kernel: NtCreateFile(\"%s\") asked for FILE_CREATE and the name "
                         "already exists -- reporting a collision rather than replacing "
                         "it\n",
                         path);
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION,
                              FILE_INFORMATION_EXISTS);
        return KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION;
    }

    if (resolved && resolved_to.backing == KERNEL_FILE_BACKING_FATX && disposition_overwrites(disposition)) {
        const fatx_status result = resolved_to.is_directory || !resolved_to.writable ? FATX_E_IS_DIR :
            fatx_resize_path(volumes[resolved_to.volume_index].fatx, resolved_to.fatx_path, 0u, 0u, NULL, 0u);
        if (result != FATX_OK) { unlock(); (void)write_io_status(io_status, fatx_status_code(result), 0u); return fatx_status_code(result); }
        resolved_to.size = 0u;
        information = disposition == FILE_SUPERSEDE ? FILE_INFORMATION_SUPERSEDED : FILE_INFORMATION_OVERWRITTEN;
    }
    if (resolved && resolved_to.backing != KERNEL_FILE_BACKING_FATX && disposition_overwrites(disposition)) {
        /*
         * AN OVERWRITING DISPOSITION ON A NAME THAT EXISTS MEANS TRUNCATE IT, and this is
         * the arm that used to open the file untouched and report FILE_OPENED. That was a
         * silently wrong answer: the title would write its new, shorter content and the
         * old file's tail would remain past the end of it, indistinguishable from data the
         * title had written itself.
         *
         * REFUSED rather than quietly not-truncated on anything this host cannot truncate:
         * a mounted disc, a fabricated empty file, a directory, or a handle whose open did
         * not ask for write access. Claiming to have overwritten a file on the user's disc
         * is a lie even though the disc is unharmed, and that lie is the one a later
         * investigation would have no way to see.
         */
        if (resolved_to.backing != KERNEL_FILE_BACKING_HOST_DIR ||
            resolved_to.is_directory || resolved_to.host_fd_plus_one == 0 ||
            !resolved_to.writable || resolved_to.device) {
            release_resolution_locked(&resolved_to);
            write_refused_count++;
            unlock();
            kernel_hle_log()("kernel: NtCreateFile(\"%s\") disposition %u would OVERWRITE "
                             "an existing name, and this host cannot truncate it (no "
                             "writable host-directory backing, or the open asked for no "
                             "write access) -- REFUSED. Opening it untruncated would leave "
                             "the old content past the end of the new\n",
                             path, (unsigned)disposition);
            (void)write_io_status(io_status, KERNEL_FILE_STATUS_ACCESS_DENIED, 0u);
            return KERNEL_FILE_STATUS_ACCESS_DENIED;
        }
        if (ftruncate(resolved_to.host_fd_plus_one - 1, 0) != 0) {
            const int failure = errno;
            release_resolution_locked(&resolved_to);
            unlock();
            kernel_hle_log()("kernel: NtCreateFile(\"%s\") could not truncate for "
                             "disposition %u: %s\n",
                             path, (unsigned)disposition, strerror(failure));
            const uint32_t status = status_for_write_errno(failure);
            (void)write_io_status(io_status, status, 0u);
            return status;
        }
        resolved_to.size = 0u;
        information = (disposition == FILE_SUPERSEDE) ? FILE_INFORMATION_SUPERSEDED
                                                      : FILE_INFORMATION_OVERWRITTEN;
    }

    /*
     * GATED ON THE EXACT FAILURE, not merely on having failed. Only "that name is not
     * there" is a problem creation can solve. The other two matter:
     *
     *   OBJECT_PATH_NOT_FOUND means a PARENT is missing, and this host does not invent
     *       parents -- NT does not either, and the measured sequence relies on it.
     *   ACCESS_DENIED means the path was REFUSED for trying to leave its volume. Walking
     *       it a second time to try creating would refuse it again and count the escape
     *       twice, which inflates a counter whose whole job is to say how often the guest
     *       asked for something it must not have.
     */
    if (!resolved && resolve_status == KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND &&
        disposition_creates(disposition)) {
        /*
         * THE ONLY PLACE A REFUSAL CAN BECOME A SUCCESS, and it can only happen when an
         * operator mounted a writable volume behind this name. The volume is located
         * again rather than carried out of `resolve_locked`, because a resolution that
         * FAILED has nothing trustworthy in it -- and `locate_volume_locked` is the one
         * implementation of the link-and-prefix walk, so the two lookups cannot disagree.
         */
        char rebuilt[KERNEL_FILE_PATH_MAX];
        volume_entry *volume = NULL;
        const char *rest = NULL;
        uint32_t locate_status = STATUS_SUCCESS;
        if (locate_volume_locked(path, rebuilt, sizeof(rebuilt), &volume, &rest, &locate_status) &&
            volume != NULL && volume->backing == KERNEL_FILE_BACKING_FATX) {
            const bool directory = (create_options & FILE_DIRECTORY_FILE) != 0u;
            fatx_status result;
            if (directory && (create_options & FILE_NON_DIRECTORY_FILE) != 0u) result = FATX_E_ARGUMENT;
            else result = directory ? fatx_mkdir_path(volume->fatx, rest) : fatx_write_path(volume->fatx, rest, NULL, 0u);
            resolve_status = fatx_status_code(result);
            if (result == FATX_OK) {
                resolved = resolve_locked(path, desired_access, &resolved_to, &resolve_status, &fabricating);
                if (resolved) information = FILE_INFORMATION_CREATED;
            }
        }
        if (locate_volume_locked(path, rebuilt, sizeof(rebuilt), &volume, &rest,
                                 &locate_status) &&
            volume != NULL && volume->backing == KERNEL_FILE_BACKING_HOST_DIR) {
            const bool wants_directory = (create_options & FILE_DIRECTORY_FILE) != 0u;
            const bool wants_file = (create_options & FILE_NON_DIRECTORY_FILE) != 0u;
            if (wants_directory && wants_file) {
                /* Contradictory, and NT refuses it. Refused here too rather than picking
                 * one, because picking one would make a guest bug look like ours. */
                kernel_hle_log()("kernel: NtCreateFile(\"%s\") set both "
                                 "FILE_DIRECTORY_FILE and FILE_NON_DIRECTORY_FILE in "
                                 "options %#x -- REFUSED, those contradict\n",
                                 path, (unsigned)create_options);
                resolve_status = STATUS_INVALID_PARAMETER;
            } else {
                if (!wants_directory && !wants_file) {
                    /* NT's default for a create with neither bit is a file. Said out loud
                     * because it is a choice made in the absence of a bit, and the two
                     * measured sites both set FILE_DIRECTORY_FILE, so this arm is
                     * unexercised by the boot. */
                    kernel_hle_log()("kernel: NtCreateFile(\"%s\") options %#x name "
                                     "neither a directory nor a file; creating a FILE, "
                                     "which is NT's default\n",
                                     path, (unsigned)create_options);
                }
                resolved =
                    hostdir_create_locked(volume, path, rest, wants_directory,
                                          desired_access, &resolved_to, &resolve_status);
                if (resolved) {
                    information = FILE_INFORMATION_CREATED;
                }
            }
        }
    }

    if (!resolved) {
        unlock();
        kernel_hle_log()("kernel: NtCreateFile(\"%s\", disposition %u) REFUSED with "
                         "status %#010x -- NOTHING WAS CREATED. Creation needs a WRITABLE "
                         "volume mounted behind the name (see --hdd); the disc is "
                         "read-only and a name with nothing behind it has nowhere to "
                         "create\n",
                         path, (unsigned)disposition, (unsigned)resolve_status);
        (void)write_io_status(io_status, resolve_status, 0u);
        return resolve_status;
    }

    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, ORD_NtCreateFile);
    if (handle == 0u) {
        release_resolution_locked(&resolved_to);
        unlock();
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES, 0u);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!claim_open_locked(path, handle, &resolved_to, create_options)) {
        release_resolution_locked(&resolved_to);
        unlock();
        (void)write_io_status(io_status, KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES, 0u);
        return KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES;
    }
    const bool is_directory = resolved_to.is_directory;
    unlock();

    if (!kernel_guest_write_u32(handle_out, handle)) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u);
        return STATUS_INVALID_PARAMETER;
    }
    /*
     * IO_STATUS_BLOCK.information carries the DISPOSITION RESULT on this path, not a
     * byte count. That is measured, not assumed: guest_structs.h records 0x0037D394
     * comparing the field against 3 on an NtCreateFile path and turning the result into
     * ERROR_ALREADY_EXISTS. This CAN now create, so the field carries which of the two
     * happened rather than a constant: FILE_INFORMATION_CREATED when a real object came
     * into existence and FILE_INFORMATION_OPENED when one was already there.
     */
    if (!write_io_status(io_status, STATUS_SUCCESS, information)) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * MARK THE RECORD, which NtOpenFile has always done and this handler never did. The
     * attempt log printed `ERR` beside every NtCreateFile including the ones that
     * succeeded, which was merely untidy while nothing could succeed and is actively
     * misleading now that four directories get created on a real boot.
     */
    lock();
    if (attempt) {
        attempt->opened = true;
    }
    unlock();

    if (fabricating) {
        kernel_hle_log()("kernel: NtCreateFile(\"%s\") -> handle %#x for an EMPTY, "
                         "FABRICATED file (nothing is mounted behind that name; "
                         "everything the title reads through this handle is ours)\n",
                         path, (unsigned)handle);
    } else if (information == FILE_INFORMATION_CREATED) {
        kernel_hle_log()("kernel: NtCreateFile(\"%s\") -> handle %#x, %s just CREATED on "
                         "a real filesystem (information = %u, FILE_CREATED)\n",
                         path, (unsigned)handle, is_directory ? "a directory" : "a file",
                         (unsigned)information);
    } else if (disposition_overwrites(disposition)) {
        /* Announced because it DESTROYED something. A truncation is the one outcome here
         * that loses content the title had previously saved, so it is said out loud rather
         * than being inferable from the information code. */
        kernel_hle_log()("kernel: NtCreateFile(\"%s\") -> handle %#x, an existing file "
                         "TRUNCATED TO ZERO for disposition %u (information = %u, %s)\n",
                         path, (unsigned)handle, (unsigned)disposition,
                         (unsigned)information,
                         information == FILE_INFORMATION_SUPERSEDED ? "FILE_SUPERSEDED"
                                                                    : "FILE_OVERWRITTEN");
    } else if (is_directory) {
        kernel_hle_log()("kernel: NtCreateFile(\"%s\") -> handle %#x, a REAL directory "
                         "that already existed on a mounted volume\n",
                         path, (unsigned)handle);
    }
    return STATUS_SUCCESS;
}

uint32_t kernel_file_open_stored(void *context)
{
    return open_file(context, true);
}

static uint32_t hle_nt_open_file(void *context)
{
    return open_file(context, false);
}

unsigned kernel_file_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_NtOpenFile, hle_nt_open_file},
        {ORD_NtCreateFile, hle_nt_create_file},
        {ORD_NtDeleteFile, hle_nt_delete_file},
        {ORD_IoCreateSymbolicLink, hle_io_create_symbolic_link},
        {ORD_IoDeleteSymbolicLink, hle_io_delete_symbolic_link},
        {ORD_NtOpenSymbolicLinkObject, hle_nt_open_symbolic_link_object},
        {ORD_NtQuerySymbolicLinkObject, hle_nt_query_symbolic_link_object},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
