/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File opening: NtOpenFile (ordinal 202).
 *
 * ORDINAL NUMBER, RESOLVED NOT RECALLED. 202 is NtOpenFile, read out of
 * `tools/kernel_ordinals.py`. Not in that file's `SUSPECT_ON_XDK_5849` list.
 *
 * WHY IT IS NEXT. It is where the bring-up run stopped once ordinals 47 and 24 were
 * implemented: thread 2 reaches it at guest 0x00380D43, the 14th kernel call of the
 * run, after 255, 187, 277, 294, 47, 107, 113, 24, 301, 149, 184, 184 and 291.
 *
 * ARITY: 6 STACK ARGUMENTS.
 *
 *     NTSTATUS __stdcall NtOpenFile(
 *             PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
 *             POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
 *             ULONG ShareAccess, ULONG OpenOptions);
 *
 * `kernel_arity.inc` measures `{202u, 6u, 12u, 0}` -- 6 over 12 sites, flagged NOT
 * unanimous, so `stack_args_for()` refuses it and an ABI_TABLE row is mandatory. The
 * count was therefore checked by hand. There are 14 sites, not 12: TWO more reach the
 * thunk slot 0x4757E8 through a register. A single load `mov ebx, [0x4757E8]` at
 * 0x004145AF feeds `call ebx` at BOTH 0x004145F6 and 0x0041465C -- `ebx` is callee-saved
 * and is not overwritten until 0x00414677, so one load serves two calls. Both push the
 * same six arguments in the same order, so both are NtOpenFile and the arity of 6 is
 * unaffected. (An earlier version of this comment said 13 sites and one register site;
 * the scanner has since been taught to see through registers and reports 14.) This was
 * the same blind spot that hid 2 of ordinal 47's sites and all 12 of ordinal 24's.
 * Located by thunk slot
 * rather than by a generated chunk file and line: the chunking is regenerated with
 * every lift, so a file:line reference here would silently go stale while still
 * looking authoritative.
 *
 * Sites read by hand, with the literals that make the ORDER unambiguous:
 *
 *     return addr   DesiredAccess  ShareAccess  OpenOptions   note
 *     0x0037D53E    0x100001       3            0x800021
 *     0x0037D887    0xC0100000     3            0x10          + a leading `push esi`
 *     0x00380D43    0x100001       3            0x800021      the site REACHED
 *     0x00381969    0x110101       3            0x4021
 *     0x00381C99    0x10000        7            0x4040
 *     0x00433F87    0xC0100000     3            0x10
 *
 * The leading `push esi` at 0x0037D887 is a register SAVE, not a seventh argument,
 * and that is settled by comparison rather than by assertion: 0x00433F87 is the same
 * code shape in another function with the same three literals and no `push esi`. That
 * one site is where the non-unanimous flag comes from.
 *
 * ARGUMENT ORDER, PINNED AT THE REACHED SITE. `sub_00380D0D` builds the whole call
 * out of its own frame, and the OBJECT_ATTRIBUTES it builds is the independent
 * confirmation of the 12-byte layout in `guest_structs.h`:
 *
 *     MEM32(ebp-0x14) &= 0           ; OA +0x00 root_directory = NULL
 *     MEM32(ebp-0x10)  = [ebp+8]     ; OA +0x04 object_name    = the caller's string
 *     MEM32(ebp-0x0C)  = 0x40        ; OA +0x08 attributes     = 0x40, as at all 31 inline sites
 *     push 0x800021                  ; arg5 OpenOptions
 *     push 3                         ; arg4 ShareAccess
 *     lea eax,[ebp-8];   push eax    ; arg3 IoStatusBlock
 *     lea eax,[ebp-0x14]; push eax   ; arg2 ObjectAttributes -> the block above
 *     push 0x100001                  ; arg1 DesiredAccess
 *     lea eax,[ebp+8];   push eax    ; arg0 FileHandle out
 *
 * Note arg0 aliases the caller's own first parameter slot, which already holds the
 * OBJECT_STRING pointer that arg2 points at. So a handler that wrote the handle
 * BEFORE reading the name would destroy the name it was about to read. The order of
 * operations in the implementation is a decision, and it is stated there.
 *
 * ALL THREE GUEST STRUCTURES ARE ALREADY DERIVED, in `docs/guest-structs.md` and
 * `src/xbox/guest_structs.h`, and nothing new is guessed here: OBJECT_ATTRIBUTES is
 * 12 bytes with no Length field, OBJECT_STRING is 8 bytes (2/2/4) with single-byte
 * characters, and IO_STATUS_BLOCK is 8 bytes. This module reads and writes exactly
 * those fields and no others.
 *
 * ================= THERE IS NOW A FILE SYSTEM BEHIND THIS =================
 *
 * This section used to say there was not. What changed is `--disc`: the user's own
 * Xbox disc image can now be mounted behind a device name, and an open that resolves
 * into it returns REAL BYTES from REAL SECTORS. See `src/xbox/xdvdfs.h` for the
 * filesystem itself and for why it is implemented in C rather than shelled out to
 * `tools/xdvdfs`.
 *
 * WHICH NAME THE DISC MOUNTS BEHIND IS NOT A GUESS. The obvious thing would be to
 * mount it on `D:`, and that would be wrong. MEASURED: the guest creates that mapping
 * ITSELF. At 0x00381301 it calls `IoCreateSymbolicLink` (ordinal 67) with two
 * OBJECT_STRINGs read straight out of the image:
 *
 *     arg0  OBJECT_STRING at 0x005491D4  len 6   -> "\??\D:"
 *     arg1  OBJECT_STRING at 0x005491DC  len 14  -> "\Device\CdRom0"
 *
 * So `D:` is the guest's own alias for `\Device\CdRom0`, created at boot. The disc
 * therefore mounts behind the DEVICE, and `D:` resolves through the symbolic-link
 * table this module keeps -- which means the mapping is the title's, not ours. Had we
 * hard-coded `D:` we would have had two sources of truth for the same alias, and the
 * guest's would have been the one we were not watching.
 *
 * The same two ordinals carry the rest of the title's namespace: `\??\T:` and `\??\U:`
 * for title and user data on the hard disk, and `\??\%c:` built at runtime for memory
 * units. Those are RECORDED by this module; only names an operator actually mounted
 * resolve to content.
 *
 * CASE IS NOT CONSISTENT IN THE BINARY ITSELF -- the image spells both `\Device\Cdrom0`
 * and `\Device\CdRom0`, and both `partition1` and `Partition1`. Matching is therefore
 * case-insensitive over ASCII throughout, which is not a convenience: a case-sensitive
 * match would refuse names the same executable spells two ways.
 *
 * What happens to a name with nothing mounted behind it is still an explicit,
 * switchable POLICY rather than a quiet default:
 *
 *   KERNEL_FILE_MISSING_FAIL  -- the default. Return a not-found status, write the
 *       failure into the IO_STATUS_BLOCK, and leave the handle out-parameter alone,
 *       which is what the real kernel does on a failed open. Every measured call
 *       site has an error arm: the reached one is `cmp eax, 0 / jl`.
 *   KERNEL_FILE_MISSING_EMPTY -- issue a handle for a zero-length file anyway, and
 *       SAY SO. This exists so the two paths can be COMPARED rather than one
 *       assumed, exactly as the host's `--stub-status` pair does; which of them
 *       reaches further into the title's startup is an empirical question whose
 *       answer belongs in a run log, not in a guess made here.
 *
 * `kernel_file_add_openable()` is the seam a later task uses once there is a real
 * volume behind a path. Nothing in the handler changes when that happens.
 *
 * ============ AND THERE IS NOW A WRITABLE VOLUME BEHIND IT TOO ============
 *
 * WHY. MEASURED at 0x00381321: the XDK startup creates `TDATA\45410066` with
 * NtCreateFile, disposition 3 (FILE_OPEN_IF) and options 0x4021
 * (FILE_DIRECTORY_FILE | ...), on `\Device\Harddisk0\partition1`. It got
 * 0xC0000034 because this module refused to create anything, and the guest tests
 * only for STATUS_DISK_FULL -- anything else jumps to XLaunchNewImage at
 * `sub_0037CB7D`, which notifies a debug monitor through a NULL pointer and would
 * then call HalReturnToFirmware(2). THE TITLE WAS REBOOTING ITSELF. A read-only
 * host could not get past its own storage layer.
 *
 * SO `KERNEL_FILE_BACKING_HOST_DIR` IS A SECOND KIND OF VOLUME, and it is a
 * DIFFERENT volume from the disc. Three properties are structural rather than
 * conventional, because each of them is a way to lose the user's data or their
 * trust:
 *
 *   1. THE DISC IS NEVER WRITTEN. `xdvdfs_open` opens the image `O_RDONLY`, and the
 *      create and write paths here refuse any volume whose backing is not
 *      HOST_DIR. There is no code path on which an image descriptor reaches a
 *      creating call, and `kernel_file_mount_host_dir` refuses a path that is not a
 *      directory, so an operator cannot point `--hdd` at their ISO either.
 *   2. NOTHING IS WRITABLE UNLESS AN OPERATOR ASKED. A host-directory volume exists
 *      only because `kernel_file_mount_host_dir` was called. With no such mount the
 *      refusal above is bit-for-bit what it always was, including its status and its
 *      log line. A title that silently got a working hard disk it was never given is
 *      how a wrong answer surfaces far from its cause.
 *   3. THE GUEST CANNOT NAME A HOST PATH OUTSIDE THE BACKING DIRECTORY, and that is
 *      true by construction rather than by inspection: no absolute host path is ever
 *      assembled. The volume holds an open descriptor on its root, each guest path
 *      component is opened with `openat(..., O_NOFOLLOW)` relative to the previous
 *      one, and a component of `..` or `.` is REFUSED. `O_NOFOLLOW` means a host
 *      symbolic link planted inside the backing directory fails the walk instead of
 *      being followed, which also closes the window a check-then-use `realpath`
 *      would leave open. The guest is untrusted input and is treated as such.
 *
 * NOT FOUND ON A WRITABLE VOLUME IS STILL AN HONEST FAILURE. A host-directory volume
 * answers for the names it actually contains and refuses the rest, exactly as a
 * mounted disc does -- the fabricate-empty policy is bypassed on both. What changes is
 * that NtCreateFile with a creating disposition can turn that refusal into a real
 * directory or a real file.
 *
 * ==================== AND IT CAN NOW BE WRITTEN TO ====================
 *
 * `kernel_file_write_backing` exists, and a HOST_DIR-backed regular file can be opened
 * with a WRITABLE host descriptor. The four properties above are unchanged by that, and
 * each is unchanged for a reason rather than by luck:
 *
 *   - THE ESCAPE GUARANTEE DOES NOT INVOLVE THE DESCRIPTOR'S MODE. It is the
 *     `openat`-relative walk from the volume's root descriptor that confines a guest
 *     path, and the walk is the same code whether the leaf is opened `O_RDONLY` or
 *     `O_RDWR`. There is still no string-joined host path anywhere in this module.
 *   - THE DISC STILL CANNOT BE WRITTEN, and now for three independent reasons rather
 *     than two. `xdvdfs_open` opens the image `O_RDONLY`; `kernel_file_mount_host_dir`
 *     refuses anything that is not a directory; and `kernel_file_write_backing` refuses
 *     any backing that is not HOST_DIR before it reaches a `pwrite`. The disc reader's
 *     descriptor lives in a `xdvdfs_reader`, which the write path never names.
 *   - NOTHING IS WRITABLE WITHOUT `--hdd`. A HOST_DIR volume exists only because
 *     `kernel_file_mount_host_dir` was called. A write against any other backing --
 *     a mounted disc, a fabricated empty file, a name with nothing behind it -- is
 *     REFUSED and counted, never reported as a success that transferred nothing. That
 *     last failure mode is exactly what a zero-byte file looks like.
 *
 * WHICH OPENS BECOME WRITABLE: the ones the guest ASKED to write through. A HOST_DIR
 * regular file is opened `O_RDWR` when its ACCESS_MASK carries a write bit, and
 * `O_RDONLY` otherwise, and a write through a read-only handle is refused with
 * ACCESS_DENIED. That is NT's own rule rather than an extra restriction invented here,
 * and it is MEASURED to be satisfied by the one write this boot sets up: at 0x00381027
 * the title creates `UDATA\45410066\TitleMeta.xbx` with DesiredAccess 0x40100000, i.e.
 * GENERIC_WRITE | SYNCHRONIZE. Directories never get a descriptor at all, so a write to
 * a directory handle is refused on the backing check before access is even consulted.
 *
 * AND AN OVERWRITING DISPOSITION NOW MEANS WHAT IT SAYS. FILE_SUPERSEDE, FILE_OVERWRITE
 * and FILE_OVERWRITE_IF on a name that already exists TRUNCATE it, and report
 * FILE_SUPERSEDED (0) or FILE_OVERWRITTEN (3) in IO_STATUS_BLOCK.information. Before
 * this they opened the existing file and reported FILE_OPENED, leaving the title's new
 * content sitting in front of the old file's tail -- a silently wrong answer rather than
 * a refusal. On a volume that cannot be truncated (a mounted disc, a fabricated empty
 * file) those dispositions are now REFUSED instead of quietly succeeding: claiming to
 * have overwritten a file on the user's disc is a lie even though the disc is unharmed.
 *
 * NOT MODELLED, said plainly: ShareAccess and OpenOptions are RECORDED but not enforced
 * -- there is no second opener to share with, so enforcing sharing would be inventing
 * failures rather than preventing them. DesiredAccess is now enforced in ONE direction
 * only: it decides whether a host descriptor can write, and it is never used to refuse a
 * READ. Nonzero object roots remain refused except the bounded inferred policy
 * for 0xFFFFFFFD plus ASCII drive-letter absolute names: these are prefixed with
 * \??\ and resolved through the title's own links. Dot components, drive-relative
 * names, other roots and direct device names with this sentinel remain refused.
 * The policy is announced once per reset; callback/event namespaces are unrelated.
 */

#ifndef TSFP_XBOX_KERNEL_FILE_H
#define TSFP_XBOX_KERNEL_FILE_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_hle.h"

/*
 * NT status codes this module needs that `src/xbox/nt_status.h` does not carry.
 * Defined here rather than added there because that header is outside this task's
 * ownership. Module-prefixed so two modules doing the same thing cannot collide.
 */
#define KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define KERNEL_FILE_STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
/* NtQuerySymbolicLinkObject, target longer than the caller's MaximumLength. INFERRED
 * from desktop NT: not reached by a measured site, whose MaximumLength is 0x104 or 0x208. */
#define KERNEL_FILE_STATUS_BUFFER_TOO_SMALL 0xC0000023u
/* NtQuerySymbolicLinkObject on a handle that is not a link. INFERRED from desktop NT,
 * where the type check inside ObReferenceObjectByHandle gives this. Not measured. */
#define KERNEL_FILE_STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
/* Returned when a symbolic link of that name already exists. NOT a guess: the guest
 * compares IoCreateSymbolicLink's result against this exact value at 0x0038130B and
 * branches to its success path, so the title itself names the code. */
#define KERNEL_FILE_STATUS_OBJECT_NAME_COLLISION 0xC0000035u
/* A guest path that tried to leave its backing directory, or that named a host
 * symbolic link. ACCESS_DENIED rather than a not-found code because the two are
 * different findings: not-found says "ask for something else", denied says "that was
 * refused", and an escape attempt must never read as an ordinary miss. */
#define KERNEL_FILE_STATUS_ACCESS_DENIED 0xC0000022u
/*
 * THE ONE STORAGE FAILURE THE TITLE HANDLES RATHER THAN REBOOTING ON.
 *
 * MEASURED: after the create at 0x00381321 the guest compares the status against this
 * value specifically, and every other failure falls through to XLaunchNewImage. So it is
 * reported when a create genuinely ran out of space on the host, and NEVER as a
 * convenient way to keep the boot alive -- a fabricated "disk full" would make the title
 * take its own out-of-space path on a host with terabytes free, and that wrong turn would
 * surface nowhere near here.
 */
#define KERNEL_FILE_STATUS_DISK_FULL 0xC000007Fu
/* A raw partition no filesystem driver recognises. Answered for the directory view of a
 * cache partition the title has not formatted yet. INFERRED to be the status a console's
 * FATX driver gives for a RAW volume: the title treats every negative result of that open
 * the same way (it formats and retries), so the exact value is not load-bearing. */
#define KERNEL_FILE_STATUS_UNRECOGNIZED_VOLUME 0xC000014Fu
/* NtQueryDirectoryFile: the listing ran out (MEASURED: compared at 0x003818C0) and the first
 * call matched nothing (also compared there; the NO_SUCH_FILE role is INFERRED from NT). */
#define KERNEL_FILE_STATUS_NO_MORE_FILES 0x80000006u
#define KERNEL_FILE_STATUS_NO_SUCH_FILE 0xC000000Fu
#define KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
/* An IOCTL or FSCTL this module has no answer for. */
#define KERNEL_FILE_STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
/* NtDeleteFile of a file another live handle holds open. INFERRED from desktop NT (a delete
 * opens the file with no FILE_SHARE_DELETE). Not measured: the one measured delete follows the
 * title's own CloseHandle of that file. */
#define KERNEL_FILE_STATUS_SHARING_VIOLATION 0xC0000043u

/* The longest guest path this module will hold. The image's longest static
 * OBJECT_STRING is 47 bytes and the guest's own largest MaximumLength is 0x104
 * (260), measured at 0x0037F915, so 260 plus a NUL is the bound the guest itself
 * implies. */
#define KERNEL_FILE_PATH_MAX 261u

/** What to do with a name that is not in the openable set. See the header comment. */
typedef enum {
    KERNEL_FILE_MISSING_FAIL = 0,
    KERNEL_FILE_MISSING_EMPTY,
} kernel_file_missing_policy;

/** One open the guest attempted, recorded for the run report. */
typedef struct {
    char path[KERNEL_FILE_PATH_MAX];
    uint32_t desired_access;
    uint32_t share_access;
    uint32_t open_options;
    uint32_t attempts;
    bool opened;
} kernel_file_attempt;

/** Register the file ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_file_register(void);

/** Forget every openable name, every recorded attempt and the counters. */
void kernel_file_reset(void);

/** Choose what a name outside the openable set does. */
void kernel_file_set_missing_policy(kernel_file_missing_policy policy);

/**
 * Declare `path` openable, as a zero-length file.
 *
 * The seam a later task uses to put a real volume behind a path. Matching is exact
 * and case-insensitive. All 31 measured inline OBJECT_ATTRIBUTES builders store
 * 0x40 (the shared named-event/mutex helper stores 0x80); the reading of 0x40 as
 * case-insensitivity is INFERRED in `guest_structs.h`. This is deliberately
 * the lenient direction: a case-sensitive match could refuse a name the
 * real kernel would have opened, and a refusal that should have succeeded is the
 * error that stops a run.
 *
 * False for a null or over-long path, or a full table.
 */
bool kernel_file_add_openable(const char *path);

/** How many names are declared openable. */
unsigned kernel_file_openable_count(void);

/** How many distinct paths the guest has tried to open. */
unsigned kernel_file_attempt_count(void);

/**
 * One recorded attempt, oldest first, or NULL past the end.
 *
 * The paths ARE the deliverable of a bring-up run: they say exactly which volumes a
 * later task has to provide. Returned rather than merely logged so the host can
 * report them in one block instead of scattered through the trace.
 */
const kernel_file_attempt *kernel_file_attempt_at(unsigned index);

/** How many opens were refused because the name was not in the openable set. */
unsigned kernel_file_refused_count(void);

/** How many opens were satisfied by a FABRICATED empty file. */
unsigned kernel_file_fabricated_count(void);

/** How many opens were refused for naming a directory to be relative to. */
unsigned kernel_file_relative_refused_count(void);

/**
 * How many opens carried an OBJECT_ATTRIBUTES.attributes other than 0x40, the one value
 * this host's always-case-insensitive matching agrees with. The bits are ignored and the
 * open proceeds, so this is the only trace that the guest asked for something else.
 */
unsigned kernel_file_unmodelled_attributes_count(void);

/* ======================= volumes, symlinks and open files ================= */

/** How many mounts and symbolic links one run may hold. */
#define KERNEL_FILE_VOLUME_MAX 16u
#define KERNEL_FILE_SYMLINK_MAX 32u

/** How many NtOpenSymbolicLinkObject handles may be live at once (see kernel_file.c). */
#define KERNEL_FILE_SYMLINK_HANDLE_MAX 16u

/** How many files may be open at once. Bounded by the object table either way. */
#define KERNEL_FILE_OPEN_MAX 64u

/**
 * How deep a chain of symbolic links will be followed.
 *
 * The guest creates its links with `IoCreateSymbolicLink` and nothing stops it
 * pointing one at another, or at itself. A bounded walk that REPORTS exhaustion beats
 * both an unbounded one (which hangs) and a single-step one (which would silently stop
 * resolving if the title ever chained two).
 */
#define KERNEL_FILE_SYMLINK_DEPTH_MAX 8u

/** What is behind a mounted volume. */
typedef enum {
    /* Nothing. Opens resolve to a FABRICATED zero-length file, announced on every
     * open. This is what `--mount` has always given. */
    KERNEL_FILE_BACKING_EMPTY = 0,
    /* The user's own Xbox disc image, read through src/xbox/xdvdfs.h. Real sectors,
     * real bytes, nothing fabricated. READ-ONLY, structurally: the image descriptor is
     * O_RDONLY and no creating path accepts this backing. */
    KERNEL_FILE_BACKING_DISC,
    /* A real writable host directory, standing in for a hard-disk partition. The only
     * backing on which NtCreateFile genuinely creates anything. See the header comment
     * for the three structural properties this one has to hold. */
    KERNEL_FILE_BACKING_HOST_DIR,
    KERNEL_FILE_BACKING_FATX,
} kernel_file_backing;

/*
 * The longest host path this module will record for a mounted directory.
 *
 * Recorded for DIAGNOSTICS ONLY. No host path is ever assembled from guest input -- the
 * walk is `openat` relative to the volume's root descriptor -- so this bound constrains
 * what an operator may pass on the command line and nothing the guest can influence.
 */
#define KERNEL_FILE_HOST_PATH_MAX 4096u

/**
 * Mount the user's disc image behind a guest device prefix.
 *
 * `prefix` is a guest device path such as `\Device\CdRom0`. It is NOT `D:`: the guest
 * creates that alias itself with ordinal 67, measured at 0x00381301, so mounting the
 * device is what makes the title's own symbolic link resolve. Opening and validating
 * the image happens here, so a bad path or a non-Xbox image fails at mount time with a
 * diagnosis rather than at the title's first read.
 *
 * False, with a log line naming the reason, when the prefix table is full or the image
 * cannot be opened or carries no XDVDFS volume descriptor.
 */
bool kernel_file_mount_disc(const char *prefix, const char *image_path);

/**
 * Mount a real, WRITABLE host directory behind a guest device prefix.
 *
 * `prefix` is a guest device path such as `\Device\Harddisk0\partition1`. `host_dir`
 * must already exist and be a directory; it is NOT created, because creating it would
 * mean a typo silently produced an empty hard disk somewhere the operator did not
 * intend. The directory is opened here and the descriptor is what every later path walk
 * starts from, so the mount is pinned to the directory that was validated rather than
 * to a name that could be replaced underneath it.
 *
 * REFUSED, each with a log line naming the reason: a null or over-long argument, a full
 * volume table, a path that cannot be opened, and a path that is not a directory. That
 * last one is the structural reason an operator cannot hand their disc image to this
 * function.
 *
 * This is the ONLY way a writable volume comes into existence. Nothing defaults to it.
 */
bool kernel_file_mount_host_dir(const char *prefix, const char *host_dir);

/*
 * The largest virtual device this module will back. A raw device has a size the guest can
 * query and a title can ask to write anywhere inside it, so the bound is what stops a
 * wrong capacity argument from becoming a sparse file of unbounded length on the
 * operator's disk. The title touches bytes 0x800..0xA00 (MEASURED) and a console's config
 * area is 512 KiB (INFERRED from the Xbox disk layout, not measured here), so 16 MiB is
 * 32x the larger figure.
 */
#define KERNEL_FILE_DEVICE_CAPACITY_MAX (16ull * 1024ull * 1024ull)
/* The longest host file name a device mount accepts: NAME_MAX on every filesystem this
 * runs on. A name, never a path. */
#define KERNEL_FILE_DEVICE_NAME_MAX 255u

/**
 * Mount a VIRTUAL RAW DEVICE behind a guest device prefix, backed by one regular file.
 *
 * WHY IT EXISTS. MEASURED: the title opens `\Device\Harddisk0\partition0` read/write
 * (access 0xC0100000, options 0x10) from 0x0037D887, which is `XapiSelectCachePartition`
 * inside `XMountUtilityDrive`, and from 0x00433F87, `XNetOpenConfigVolume`. The first
 * reads 0x200 bytes at offset 0x800, checks a magic dword, and writes the same 0x200 bytes
 * back at 0x800. On a console that is the hard disk's CONFIG AREA. A raw host device would
 * be a different safety class from a save file, so what the guest gets is a regular file.
 *
 * WHAT THE GUEST GETS, all of it announced:
 *   - the device opens ONLY by its exact name (a name inside it, or with a trailing
 *     separator, is not found), and a read/write open of it is the one thing it answers;
 *   - a read inside `capacity_bytes` returns what the title wrote there, and ZEROS for
 *     every byte it never wrote. Those zeros are FABRICATED and are counted and logged,
 *     because a fresh config area is the title's first view of a console that has never
 *     been set up, and that is our choice rather than a measurement;
 *   - a read or write that runs past `capacity_bytes` transfers nothing (reads) or is
 *     REFUSED (writes), the way a partition ends;
 *   - end-of-file set and overwriting dispositions are REFUSED: a device has no length to
 *     change;
 *   - writes persist in `host_dir`/`file_name`, so the title's own config survives the run.
 *
 * WHAT IT CAN NEVER BE. The file is looked at with `fstatat(AT_SYMLINK_NOFOLLOW)` BEFORE
 * any open, and anything that exists and is not a regular file (a device node, a FIFO, a
 * symbolic link, a directory) REFUSES THE MOUNT. It is opened with O_NOFOLLOW relative to
 * a descriptor on `host_dir`, so no host path is assembled, and an existing file larger
 * than `capacity_bytes` refuses the mount rather than being adopted.
 *
 * `prefix` must not end in a separator. `file_name` is a single component. `host_dir` must
 * already exist and be a directory. `capacity_bytes` must be in
 * (0, KERNEL_FILE_DEVICE_CAPACITY_MAX]. The file is created if absent, mode 0600.
 *
 * CALLER'S CHOICE, not ours: where `host_dir` is. Passing the `--hdd` directory puts the
 * file in `partition1`'s root, where the title could also name it. That is the same
 * capability the title already has over `partition0`, so nothing is gained by it, but a
 * caller that wants the namespaces apart should pass a different directory.
 *
 * False, with a log line naming the reason, on any refusal.
 */
bool kernel_file_mount_host_device(const char *prefix, const char *host_dir,
                                   const char *file_name, uint64_t capacity_bytes);

/* Read-only capability for an actual mounted raw-device backing. The caller owns
 * root_fd and closes it; the duplicate remains valid across volume unmount or
 * replacement of the original directory pathname. filename is one validated
 * component under that root, never an arbitrary host path. Capacity retains the
 * mount's announced inferred geometry; it is not proof of stored sector bytes.
 * The snapshot is unchanged on failure. No guest handle or file is created. */
typedef struct {
    int root_fd;
    char filename[KERNEL_FILE_DEVICE_NAME_MAX + 1u];
    uint64_t capacity;
} kernel_file_device_backing;
bool kernel_file_device_backing_acquire(const char *prefix, kernel_file_device_backing *out);


/** The smallest N `kernel_file_mount_cache_partition` accepts: the XDK counts cache
 * partitions from 3 (MEASURED: `XapiSelectCachePartition` stores `index + 3`). */
#define KERNEL_FILE_CACHE_PARTITION_FIRST 3u
/** The largest: the host accepts at most MAX_CACHE_PARTITIONS (8) of them, so 3 + 8 - 1. */
#define KERNEL_FILE_CACHE_PARTITION_LAST 10u
/** The first four bytes the title's own `XapiFormatFATVolumeEx` writes at offset 0 of the
 * raw partition: "FATX". MEASURED from the disassembly at 0x00381574 (`mov [esi],
 * 0x58544146`). It is the ONLY thing this module ever reads back from a cache image. */
#define KERNEL_FILE_FATX_MAGIC 0x58544146u

/**
 * Mount a FORMATTABLE CACHE PARTITION behind a guest device prefix: a raw view and a
 * directory view under ONE name.
 *
 * WHY. MEASURED (docs/disc-io.md section 2e): `XMountUtilityDrive` picks a cache partition,
 * and on a first run formats it itself with `XapiFormatFATVolumeEx` (raw open of
 * `PartitionN`, two IOCTLs, a run of raw writes laying down a FATX image, one FSCTL), then
 * validates it by opening the DIRECTORY view `PartitionN\` and binds `Z:` to it. The title
 * reads NOTHING back from the raw image. So the volume is split:
 *
 *   - the EXACT name is a virtual raw device, exactly as `kernel_file_mount_host_device`
 *     makes one, backed by the regular file `image_name` under `host_dir`, and the only
 *     handle on which NtDeviceIoControlFile / NtFsControlFile are answered;
 *   - a name WITH a separator (`PartitionN\`, `PartitionN\x`) is a real host directory,
 *     `content_dir` under `host_dir`, created lazily and ONLY once the raw image starts
 *     with "FATX" (the title's own format wrote it). Until then it answers
 *     KERNEL_FILE_STATUS_UNRECOGNIZED_VOLUME, which is what makes the title format.
 *
 * NO FAT IS PARSED. The raw image is write-only here and the directory view is not derived
 * from it: after the title formats, the two are independent. Both are announced.
 *
 * SAFETY, the same as every other mount in this module. The backing is a regular file or a
 * directory under `host_dir`, never a host device. It is looked at with AT_SYMLINK_NOFOLLOW
 * before any open and opened O_NOFOLLOW relative to a descriptor on `host_dir`, and no host
 * path is assembled from guest input. `host_dir` must already exist and be a directory.
 *
 * `number` is the N of `PartitionN`, 3..10, and `prefix` must be that partition's guest
 * name without a trailing separator. `capacity_bytes` is what the IOCTLs report as the
 * partition length and is bounded like any device, (0, KERNEL_FILE_DEVICE_CAPACITY_MAX].
 */
bool kernel_file_mount_cache_partition(const char *prefix, const char *host_dir,
                                       unsigned number, const char *image_name,
                                       const char *content_dir, uint64_t capacity_bytes);

/** How many opens resolved into the DIRECTORY view of a cache partition. */
unsigned kernel_file_cache_view_opened_count(void);

/** How many opens resolved into a virtual raw device. */
unsigned kernel_file_device_opened_count(void);

/**
 * How many bytes a read served as FABRICATED zeros: bytes inside the device but past the
 * backing file's current end, which this module zero-fills itself.
 * Reported separately from `kernel_file_host_bytes_read()`, which counts bytes that came
 * out of a real file: a run report that folded these in would claim the config area held
 * content it never had. A hole INSIDE the file (a write at 0x800 leaves 0..0x7FF) is served
 * by the host's sparse file and counts as a host read, so the two counters together cover
 * every byte transferred but neither alone says how much the title ever wrote.
 */
uint64_t kernel_file_device_zero_bytes(void);

/** How many volumes are mounted, of any backing. */
/* Actual mounted backing capacity. Host directories use fstatvfs on their live
 * root descriptor, normalized to title allocation units; FATX uses its real FAT.
 * Returns false for absent, fabricated, stale or unsupported backing. */
/* Per-request NtOpenFile refusing absent/fabricated backing without changing
 * global missing-file policy. Real open/status/handle bookkeeping is shared. */
uint32_t kernel_file_open_stored(void *context);

bool kernel_file_volume_space(uint32_t handle, uint64_t *total_units,
    uint64_t *available_units, uint32_t *sectors_per_unit, uint32_t *bytes_per_sector);

unsigned kernel_file_volume_count(void);

/** Close any mounted disc image and forget every volume. */
void kernel_file_unmount_all(void);

/**
 * Record a symbolic link, as `IoCreateSymbolicLink` does.
 *
 * Exposed so a test can establish the same table the guest does without driving the
 * ordinal through a synthetic frame, and so an operator could pre-seed a link the
 * title is not reached yet to create.
 */
bool kernel_file_add_symlink(const char *name, const char *target);

/** Forget a symbolic link. False when no such link exists. */
bool kernel_file_remove_symlink(const char *name);

/** How many symbolic links are recorded. */
unsigned kernel_file_symlink_count(void);

/** The target of `name`, or NULL. Case-insensitive over ASCII. */
const char *kernel_file_symlink_target(const char *name);

/** How many NtOpenSymbolicLinkObject handles are still live. For tests and the run report. */
unsigned kernel_file_symlink_handle_count(void);

/** One open file's state, copied out rather than pointed at. */
typedef struct {
    uint32_t handle;
    char path[KERNEL_FILE_PATH_MAX];
    kernel_file_backing backing;
    bool is_directory;
    /* Size in bytes. 0 for a fabricated empty file, and for a directory. */
    uint64_t size;
    /* The current file position, which NtReadFile advances and
     * NtSetInformationFile's position class sets. */
    uint64_t offset;
    /* First sector of the file within the disc image, when backing is DISC. */
    uint32_t disc_sector;
    /*
     * Whether this handle's host descriptor can WRITE.
     *
     * True only for a HOST_DIR-backed regular file whose open asked for write access and
     * got it. Recorded per handle rather than re-derived at write time on purpose: the
     * descriptor was opened once, with one mode, and a write that re-decided the
     * question could disagree with the open that produced the descriptor it is using.
     */
    bool writable;
    /* This handle is a VIRTUAL RAW DEVICE (partition0 or a cache partition's raw view).
     * Per HANDLE and not per volume, because a cache partition's name answers with a raw
     * device for its exact name and a directory for everything under it. */
    bool device;
    /* The N of `PartitionN` when this handle is the raw view of a FORMATTABLE cache
     * partition, else 0. The only handles NtDeviceIoControlFile and NtFsControlFile answer. */
    unsigned cache_partition;
    /* The NtCreateFile CreateOptions or NtOpenFile OpenOptions of the open (T743). Bits 0x10 and
     * 0x20 (FILE_SYNCHRONOUS_IO_ALERT, _NONALERT) make the handle synchronous, none of them an
     * asynchronous (overlapped) one. */
    uint32_t open_options;
} kernel_file_open;

/**
 * Copy out the state of an open handle. False when the handle is not an open file.
 *
 * COPIED, not pointed at, which is the opposite of what `kernel_file_attempt_at` does
 * and deliberately so: the attempt log is write-once per slot, whereas an open file's
 * `offset` is mutated by every read. Handing out a pointer into it would let a caller
 * read a position another thread is advancing.
 */
bool kernel_file_open_info(uint32_t handle, kernel_file_open *out);

/** Set an open file's position. False when the handle is not an open file. */
bool kernel_file_open_set_offset(uint32_t handle, uint64_t offset);

/** How many files are open. */
unsigned kernel_file_open_count(void);

/**
 * Read from whatever backs an open file.
 *
 * `*out_read` is the byte count transferred, which may be short at end of file; that
 * is not an error, and the caller needs the count either way. False only when the read
 * could not be attempted -- a bad handle, or a disc image that failed underneath us.
 *
 * A read of a file backed by KERNEL_FILE_BACKING_EMPTY always transfers 0 bytes. It
 * does NOT zero-fill the caller's buffer, because a zero-filled buffer and a real read
 * of zeros are indistinguishable to the guest, and the whole point of the empty
 * backing is that the title should be able to tell.
 */
/* Request-specific stored bytes only: no virtual-device zero fill or EMPTY
 * fallback. Short actual reads preserve the untouched caller buffer tail. */
bool kernel_file_read_backing_stored(uint32_t handle, uint64_t offset, void *buffer,
                                     uint32_t length, uint32_t *out_read);

bool kernel_file_read_backing(uint32_t handle, uint64_t offset, void *buffer,
                              uint32_t length, uint32_t *out_read);

/** T1633: an observer of the guest's file activity, for the route event waits (src/host/route_probe.c). OPEN fires when an open
 *  succeeded (path as the guest gave it), READ after a successful read (`bytes` = transferred), WRITE after a write that landed.
 *  Called under the file lock: it must be quick and must not call back into this module. NULL removes it. Set before any guest
 *  thread runs. */
typedef enum { KERNEL_FILE_EVENT_OPEN = 1, KERNEL_FILE_EVENT_READ, KERNEL_FILE_EVENT_WRITE } kernel_file_event_kind;
typedef void (*kernel_file_observer)(kernel_file_event_kind kind, const char *path, uint64_t bytes);
void kernel_file_set_observer(kernel_file_observer observer);

/**
 * Write to whatever backs an open file. THE ONLY FUNCTION IN THIS TREE THAT WRITES BYTES
 * TO A HOST FILE ON THE GUEST'S BEHALF.
 *
 * TRUE MEANS ALL `length` BYTES LANDED, and that is the contract rather than a usual
 * case. `*out_written` is always the count that actually reached the filesystem, so a
 * caller that reports `*out_written` can never claim a transfer that did not happen --
 * reporting the REQUESTED count as the transferred one is precisely how a zero-byte file
 * ends up looking like a successful write.
 *
 * `*out_status` carries the NT status to hand the guest, so the ONE storage failure the
 * title handles rather than rebooting on survives the trip: a genuine ENOSPC or EDQUOT
 * becomes KERNEL_FILE_STATUS_DISK_FULL and nothing else ever does.
 *
 * REFUSED, counted in `kernel_file_write_refused_count()`, and logged with the reason:
 * a handle this module does not know; any backing that is not
 * KERNEL_FILE_BACKING_HOST_DIR (which is what keeps the user's disc and every fabricated
 * empty file unwritable); a directory; and a handle whose open did not ask for write
 * access. A refusal NEVER returns true, because a write that succeeded having written
 * nothing is the failure mode this whole module exists to avoid.
 *
 * A zero-length write is a no-op that succeeds, which is what NT does.
 */
bool kernel_file_write_backing(uint32_t handle, uint64_t offset, const void *buffer,
                               uint32_t length, uint32_t *out_written,
                               uint32_t *out_status);

/**
 * Truncate or extend whatever backs an open file, as NT's FileEndOfFileInformation does.
 *
 * Gated by EXACTLY the same test as `kernel_file_write_backing`, so it adds no new
 * capability class -- only another operation inside one an operator already granted with
 * `--hdd`. DESTRUCTIVE when it shrinks, which is why it is gated rather than permissive:
 * nothing reaches it without a writable HOST_DIR handle.
 *
 * False with `*out_status` set on any refusal, exactly as the write path does.
 */
bool kernel_file_set_end_of_file(uint32_t handle, uint64_t length, uint32_t *out_status);

/** Real host reservation/release; preserves EOF and file position. Same writable
 * regular-file gate as writes; negative/overflow/device requests are refused. */
bool kernel_file_set_allocation(uint32_t handle, uint64_t length, uint32_t *out_status);

/** The longest host name a directory listing returns (NAME_MAX, matching the device mount). */
#define KERNEL_FILE_DIR_NAME_MAX 255u

/** One directory entry for NtQueryDirectoryFile. Times are NT FILETIMEs. */
typedef struct {
    char name[KERNEL_FILE_DIR_NAME_MAX + 1u];
    bool is_directory;
    uint64_t size;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    /* Direct FATX entries carry actual attributes and allocated chain bytes. */
    bool image_backed;
    uint32_t attributes;
    uint64_t allocation;
} kernel_file_dir_entry;

/**
 * Return the next entry of an open HOST_DIR or explicit FATX directory, in case-folded name order, after the
 * last one returned. `restart` rewinds; `mask` (NULL or empty = all, `*` and `?` supported)
 * is honoured only on the first call after open or restart. Dot entries, symbolic links and
 * special files are hidden. False with `*out_status` set: NO_SUCH_FILE (first call, no match),
 * NO_MORE_FILES (exhausted), INFO_LENGTH_MISMATCH (name longer than `max_name_bytes`, cursor
 * kept), INVALID_PARAMETER (not a directory), NOT_IMPLEMENTED (disc/empty/device backing),
 * INVALID_HANDLE.
 */
bool kernel_file_dir_next(uint32_t handle, bool restart, const char *mask,
                          uint32_t max_name_bytes, kernel_file_dir_entry *out,
                          uint32_t *out_status);

/**
 * What a name resolves to, for NtQueryFullAttributesFile (210), answered with no handle.
 * `times_known` is true only for a HOST_DIR object, whose three times are the host's
 * (NT FILETIMEs). A disc or empty object has none to report, and every time is 0 there.
 * `size` is the resolved size: the end-of-file of a file, and for a directory whatever
 * its backing records (a HOST_DIR directory is 0, a disc directory is its table size).
 */
typedef struct {
    kernel_file_backing backing;
    bool is_directory;
    bool times_known;
    uint64_t size;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
} kernel_file_attributes;

/**
 * Resolve the OBJECT_ATTRIBUTES at `object_attributes` exactly as NtOpenFile does (same
 * name reader, same bounded drive-root policy, same symlinks, volumes and refusals) but
 * issue no handle and keep no descriptor. Returns STATUS_SUCCESS with `*out` filled, or
 * the resolution's status (NAME_NOT_FOUND, PATH_NOT_FOUND, ACCESS_DENIED, ...) with `*out`
 * zeroed. INVALID_PARAMETER for a null or unreadable OBJECT_ATTRIBUTES or name. Under the
 * EMPTY missing policy an undeclared name answers a FABRICATED empty file, as an open of
 * it would, and that is counted by `kernel_file_fabricated_count`.
 */
uint32_t kernel_file_query_attributes(kernel_guest_ptr object_attributes,
                                      kernel_file_attributes *out);

/** How many attribute queries (ordinal 210) reached resolution, whatever their outcome. */
unsigned kernel_file_attribute_query_count(void);

/** How many files NtDeleteFile (195) really removed from a host directory this run. */
unsigned kernel_file_deleted_count(void);

/**
 * How many NtDeleteFile calls were REFUSED loudly with nothing removed: a directory, the volume
 * root or a raw device, a disc or no volume, an escape, a live handle (sharing violation) or a
 * host failure. An honest absence (name or path not found) is not a refusal and is not counted.
 */
unsigned kernel_file_delete_refused_count(void);

/** How many bytes have been served from a real disc image this run. */
uint64_t kernel_file_disc_bytes_read(void);

/** How many opens resolved into a real disc image. */
unsigned kernel_file_disc_opened_count(void);

/** How many opens resolved into a real host directory. */
unsigned kernel_file_host_opened_count(void);

/** How many bytes have been served from a real host directory this run. */
uint64_t kernel_file_host_bytes_read(void);

/**
 * How many bytes have been WRITTEN to a real host directory this run.
 *
 * Counted as bytes that reached `pwrite` successfully, never as bytes requested. A run
 * report that quoted the request would say the title saved its data on a run where the
 * filesystem refused every byte.
 */
uint64_t kernel_file_host_bytes_written(void);

/**
 * How many writes, or end-of-file sets, were REFUSED and for any reason.
 *
 * Separate from the byte count and reported rather than merely returned: a title whose
 * every write is being refused is a title whose save data does not exist, and that has to
 * be visible in a run report rather than inferable from a byte total of zero.
 */
unsigned kernel_file_write_refused_count(void);

/**
 * How many directories and files NtCreateFile genuinely created on a writable volume.
 *
 * Separate from `kernel_file_fabricated_count()` and that distinction is the point: a
 * fabricated empty file is ours and tells the title a lie, whereas a created directory
 * is a real object on a real filesystem that will still be there on the next run.
 */
unsigned kernel_file_created_count(void);

/**
 * How many guest paths were refused for trying to leave their backing directory.
 *
 * Counted and reported rather than merely refused. A title that is repeatedly asking for
 * `..` is doing something this host does not understand, and a silent refusal would make
 * that look like an ordinary missing file.
 */
unsigned kernel_file_escape_refused_count(void);

/* Explicit host image backing: caller lifetime serialized by run_locked. */
#include "../input/mu_fatx.h"
void kernel_file_run_locked(void (*job)(void *), void *context);
bool kernel_file_mount_fatx(const char *prefix, fatx_volume *image, bool (*flush_image)(void *), void *context);
bool kernel_file_unmount_fatx(const char *prefix);
bool kernel_file_flush_fatx(uint32_t handle, uint32_t *status);
#endif /* TSFP_XBOX_KERNEL_FILE_H */
