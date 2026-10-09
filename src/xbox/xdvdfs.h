/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XDVDFS: read-only access to the filesystem on the user's own Xbox disc image.
 *
 * This is layer ONE of two. The disc's root holds just `default.xbe`, `update.xbe`,
 * `dashupdate.xbe` and two directories, `pak/` and `xmv/`; the 403 `P5CK` archives
 * under `pak/` are layer two and are `tools/pak`'s business, not this module's. The
 * guest names paths like `pak/arcade/l_103.pak`, so layer one is what it needs first.
 *
 * ================= WHY THIS IS IN C AND NOT SHELLED OUT =================
 *
 * `tools/xdvdfs` already parses this image correctly in Python (412 files, 79 dirs,
 * 4,046,110,016 bytes), and the instruction was to reuse its logic rather than
 * reinvent the format. The format knowledge below IS taken from it. The CODE is
 * re-expressed in C, and that was a deliberate choice between three options:
 *
 *   1. SHELL OUT to the Python per open/read. Rejected. `NtReadFile` is a hot path --
 *      a title streaming audio issues them continuously -- and a process spawn per
 *      read is not a cost that can be amortised. It would also make the host depend
 *      on `tools/` and a populated `.venv` at runtime, which nothing else in
 *      `src/host` does, so a correctly-built host could still fail to read a disc.
 *   2. PRE-INDEX to a manifest with the Python, then have C read the manifest and
 *      pread the image. Rejected, and this is the interesting one: it removes only
 *      the directory walk, which is the EASY part -- the read path has to exist in C
 *      either way -- while adding a failure mode this project specifically refuses.
 *      A manifest generated from one ISO and used against another would serve
 *      confident, wrong offsets, and the resulting corruption would be attributed to
 *      the lift. There is no cheap way to prove a manifest and an image agree.
 *   3. IMPLEMENT IT IN C. Chosen. The metadata is trivially small -- 80 directory
 *      blobs of at most 2048 bytes, about 160 KB total, which the Python walks in
 *      1 ms -- so the thing a pre-index would have saved costs nothing to do
 *      directly. It is testable in-process against the real image, and `tools/xdvdfs`
 *      remains available as an INDEPENDENT ORACLE for differential testing rather
 *      than as a build dependency. That is strictly more checking than reusing it
 *      would have bought.
 *
 * ================= WHAT IS DELIBERATELY NOT COPIED =================
 *
 * The Python has four properties that would be bugs here, and they are not ported.
 * Each is a real defect found by reading it, not a stylistic difference:
 *
 *   (a) THE NIL SENTINEL. The Python treats only 0xFFFF as "no child". This image
 *       uses **0**, at 291 left links and 280 right links, and uses 0xFFFF ZERO
 *       times. The Python survives only by accident: it pushes offset 0, and 0 is
 *       already in its visited set because it is the directory's own root node, so
 *       the push is discarded on pop. A direct transliteration into a recursive C
 *       walker INFINITELY RECURSES ON THE FIRST LEAF. Both 0 and 0xFFFF are nil here.
 *   (b) 32-BIT SECTOR ARITHMETIC. `start_sector * 2048` overflows 32 bits for files
 *       near the end of a dual-layer disc. Python's integers are arbitrary-precision
 *       and never notice; C must widen BEFORE multiplying. Every byte offset in this
 *       module is computed as `(uint64_t)sector * XDVDFS_SECTOR_SIZE`. On this image
 *       the highest file offset is 0xF1280000, which still fits -- so a 32-bit bug
 *       here would pass every test on this disc and corrupt a larger one.
 *   (c) THE FLAT-LIST FALLBACK. The Python has a branch for a directory whose links
 *       are both 0xFFFF, laying entries out consecutively. It fires ZERO times on the
 *       real image and exists for a synthetic fixture. It is also dangerous: 0xFFFF
 *       is exactly what you read out of the 0xFF padding that fills every directory
 *       tail, and decoding padding yields `attributes = 0xFF` -- which has the
 *       directory bit set -- and `name_length = 0xFF`, i.e. padding that presents as
 *       a directory at sector 0xFFFFFFFF. NOT PORTED. Both links nil means the node
 *       is a leaf, full stop.
 *   (d) WHOLE-FILE READS. The Python reads a file into one allocation; for
 *       `pak/musicts.pak` that is 1.08 GB. This module only ever reads the byte range
 *       asked for.
 *
 * ================= THE FORMAT =================
 *
 * Sectors are 2048 bytes. The volume descriptor is at sector 32, i.e. absolute byte
 * offset 0x10000, and begins with the 20-byte ASCII magic `MICROSOFT*XBOX*MEDIA`,
 * which is repeated at descriptor offset 0x7EC. The root directory's sector and its
 * size IN BYTES are u32s at descriptor offsets 0x14 and 0x18.
 *
 * ONLY offset 0x10000 IS PROBED, and that is a decision rather than an omission.
 * Some Xbox dumps carry a video partition and place the game partition at a base
 * offset; `tools/xdvdfs` probes no such thing, this image needs none (the magic
 * occurs exactly twice in the whole 4 GB file, both inside sector 32), and adding
 * speculative probing for layouts there is no sample of here would be inventing
 * behaviour that cannot be tested. A miss therefore reports the offset probed and the
 * bytes found, which is a diagnosis a later task can act on.
 *
 * A directory is a BINARY TREE of entries packed into its own byte blob, NOT a list.
 * The blob is `size` bytes starting at `sector`, and `size` IS NOT SECTOR-ALIGNED --
 * the root directory of this image is 120 bytes. Bounds come from `size`, never from
 * the enclosing sector, or the walk wanders into padding. Each entry is:
 *
 *     +0x00  u16  left subtree link,  IN 4-BYTE UNITS (so byte offset = link * 4)
 *     +0x02  u16  right subtree link, IN 4-BYTE UNITS
 *     +0x04  u32  start sector
 *     +0x08  u32  size in bytes
 *     +0x0C  u8   attributes; 0x10 is DIRECTORY
 *     +0x0D  u8   name length in bytes, NO terminator
 *     +0x0E  ..   the name
 *
 * Entries are padded with 0xFF to the next 4-byte boundary, and the tail of every
 * directory blob is 0xFF. Measured on this image: no entry straddles a sector
 * boundary, every pad byte is 0xFF, attribute bytes are only ever 0x10 (79
 * directories) or 0x20 (412 files), and name lengths run 3..14.
 *
 * ================= LOOKUP IS LINEAR WITHIN A DIRECTORY, ON PURPOSE =================
 *
 * The on-disc tree IS ordered -- empirically by uppercased bytewise comparison with
 * the shorter name first, which holds for all 80 directories on this disc. A binary
 * descent using that comparator would be O(log n).
 *
 * This module does NOT do that. It walks every node of the current directory's tree
 * and compares names case-insensitively, which depends on no ordering at all. The
 * reason is that the ordering is an EMPIRICAL FINDING about one disc and is not
 * asserted anywhere in the format: `tools/xdvdfs` contains no comparator to agree
 * with, and the discriminating case is subtle -- in `pak/igcs/`, uppercasing puts
 * `igcs1` before `ig_gasc` ('C' 0x43 < '_' 0x5F) while lowercasing reverses it
 * ('c' 0x63 > '_' 0x5F), so a descent written with `tolower` instead of `toupper`
 * silently returns "not found" for files that exist. The cost of not caring is
 * nothing: the busiest directory on this disc has 31 entries, and a miss is a wrong
 * answer while a few extra comparisons are free.
 *
 * ================= WHAT THIS MODULE DOES NOT DO =================
 *
 * No writing, no device-path or drive-letter handling, and no knowledge of `D:` or
 * `\Device\Cdrom0`. Those are the mount layer's job in `kernel_file.c`, which knows
 * what a guest path means; this module takes an already-relative path and accepts
 * either separator. Not thread-safe per reader handle: callers serialise, which the
 * kernel mount layer already does under its own lock.
 */

#ifndef TSFP_XBOX_XDVDFS_H
#define TSFP_XBOX_XDVDFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Bytes per sector. Every byte offset is `(uint64_t)sector * this`. */
#define XDVDFS_SECTOR_SIZE 2048u

/** Sector holding the volume descriptor, so byte offset 0x10000. */
#define XDVDFS_DESCRIPTOR_SECTOR 32u

/** The 20-byte magic at descriptor +0x00, repeated at +0x7EC. Not NUL-terminated. */
#define XDVDFS_MAGIC "MICROSOFT*XBOX*MEDIA"
#define XDVDFS_MAGIC_LENGTH 20u

/** The attribute bit meaning DIRECTORY. The only bit this module reads. */
#define XDVDFS_ATTR_DIRECTORY 0x10u

/** Fixed part of a directory entry, before the name. */
#define XDVDFS_DIRENT_FIXED 14u

/** A name is a u8 length, so this is the ceiling. Longest on this image is 14. */
#define XDVDFS_NAME_MAX 255u

/**
 * Largest directory blob this module will walk, in bytes.
 *
 * Not arbitrary: a subtree link is a u16 in 4-byte units, so no reachable entry can
 * sit past 0xFFFF * 4 = 262140 bytes, and a blob larger than that has unreachable
 * tail regardless. Every directory on this image is 2048 bytes except the 120-byte
 * root, so this is four decimal orders of headroom over what is exercised.
 */
#define XDVDFS_DIR_BYTES_MAX 262144u

/**
 * How many pending tree nodes a single directory walk will hold.
 *
 * The walk is iterative with an explicit stack so a malformed image cannot overflow
 * the host's. The busiest directory on this image has 31 entries; exceeding this is
 * REPORTED as a walk failure rather than silently truncating the directory, because a
 * truncated directory reads as "that file is not on the disc", which is a wrong answer
 * that looks like a legitimate one.
 */
#define XDVDFS_WALK_PENDING_MAX 512u

/** One entry, as decoded from a directory blob. */
typedef struct {
    /* Name as stored, NUL-terminated by this module. Raw bytes; the format does not
     * promise an encoding and this module does not impose one. */
    char name[XDVDFS_NAME_MAX + 1u];
    uint8_t name_length;
    uint32_t start_sector;
    uint32_t size;
    uint8_t attributes;
    bool is_directory;
} xdvdfs_entry;

/** An open image. Treat as opaque; the layout is here only so it can live on a stack. */
typedef struct {
    int fd;
    /* Image size in bytes, so a dirent claiming an absurd extent is rejected BEFORE a
     * read is attempted rather than by the read failing. */
    uint64_t image_bytes;
    uint32_t root_sector;
    uint32_t root_size;
    bool open;
} xdvdfs_reader;

/** Why an operation failed. Distinct values so a diagnostic can name the cause. */
typedef enum {
    XDVDFS_OK = 0,
    /* The path could not be opened, or could not be stat'd. */
    XDVDFS_ERR_IMAGE,
    /* No `MICROSOFT*XBOX*MEDIA` at byte offset 0x10000. */
    XDVDFS_ERR_MAGIC,
    /* A read returned fewer bytes than asked for, or fell outside the image. */
    XDVDFS_ERR_TRUNCATED,
    /* A directory blob is larger than XDVDFS_DIR_BYTES_MAX, or an entry's name or
     * extent falls outside its blob. A malformed image, not a missing file. */
    XDVDFS_ERR_MALFORMED,
    /* The walk needed more pending slots or visited more nodes than a blob of that
     * size can hold -- a cycle in the tree, or a directory beyond our bounds. */
    XDVDFS_ERR_WALK_LIMIT,
    /* The path does not name anything on the disc. The ONLY ordinary failure. */
    XDVDFS_ERR_NOT_FOUND,
} xdvdfs_result;

/** A human-readable name for a result, never NULL. */
const char *xdvdfs_result_str(xdvdfs_result result);

/**
 * Open `path` as an Xbox disc image and validate its volume descriptor.
 *
 * Validates BOTH copies of the magic, at descriptor +0x00 and +0x7EC. The Python
 * checks only the first; checking both is free and makes a coincidental 20-byte match
 * far less likely to be mistaken for a disc.
 */
xdvdfs_result xdvdfs_open(xdvdfs_reader *reader, const char *path);

/** Close an image. Safe on an already-closed or never-opened reader. */
void xdvdfs_close(xdvdfs_reader *reader);

/** Total bytes in the backing image file. 0 when not open. */
uint64_t xdvdfs_image_bytes(const xdvdfs_reader *reader);

/**
 * Resolve a relative path to an entry.
 *
 * `path` is relative to the disc root and may use `\` or `/` interchangeably, may
 * carry leading and duplicate separators, and is matched case-insensitively over
 * ASCII. It must NOT carry a device prefix or a drive letter -- the mount layer strips
 * those, because deciding that `D:` means this image is a mount decision and not a
 * filesystem one. A `.` or `..` component is rejected rather than interpreted.
 *
 * An empty path resolves to the root directory.
 */
xdvdfs_result xdvdfs_lookup(const xdvdfs_reader *reader, const char *path,
                            xdvdfs_entry *out);

/**
 * Read up to `length` bytes from `entry` starting `offset` bytes into it.
 *
 * Reads only the range asked for -- never the whole file. A read starting at or past
 * the end yields 0 bytes and XDVDFS_OK, which is end-of-file and not an error; a read
 * running past the end is CLAMPED to the entry and the short count reported through
 * `*out_read`. Clamped rather than refused because that is what a file read does, and
 * the caller needs the byte count either way.
 */
xdvdfs_result xdvdfs_read(const xdvdfs_reader *reader, const xdvdfs_entry *entry,
                          uint64_t offset, void *buffer, uint32_t length,
                          uint32_t *out_read);

/**
 * Callback for xdvdfs_walk. Return false to stop the walk early.
 *
 * `path` is the entry's full path from the root with `/` separators and original case.
 */
typedef bool (*xdvdfs_visit_fn)(void *context, const char *path,
                                const xdvdfs_entry *entry);

/**
 * Visit every entry on the disc, directories included, depth-first.
 *
 * Exists for tests and for a one-shot report, NOT for lookup -- `xdvdfs_lookup`
 * descends component by component and never materialises the whole tree. Order is
 * deterministic but is NOT sorted and is not the on-disc order; compare counts, sizes
 * and contents against an oracle, never list positions.
 */
xdvdfs_result xdvdfs_walk(const xdvdfs_reader *reader, xdvdfs_visit_fn visit,
                          void *context);

#endif /* TSFP_XBOX_XDVDFS_H */
