/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XDVDFS reader. See xdvdfs.h for the format, for why this is C rather than a
 * shell-out to `tools/xdvdfs`, and for the four properties of that Python which are
 * deliberately NOT ported. The header says what the module is for; the comments here
 * say why each line of the implementation is the way it is, and each of the four traps
 * is named at the place it is avoided.
 *
 * NO FEATURE-TEST MACRO IS DEFINED HERE. `open`, `fstat` and `pread` need POSIX
 * declarations, and CMakeLists.txt already compiles `tsfp_xbox` with `_DEFAULT_SOURCE`
 * (which on glibc implies _POSIX_C_SOURCE 200809L). Defining one here as well would
 * mean two places to keep in step, and a mismatch would show up as an implicit
 * declaration rather than as a clear error.
 *
 * `pread` rather than `mmap` or `fseek`/`fread`: a 4 GB mapping gives a SIGBUS on a
 * truncated image where a read gives a short count this module can report, and
 * `fseek`+`fread` carries a shared stream position, so two callers would have to
 * serialise around the seek. `pread` takes the offset as an argument, so the only
 * shared state is the descriptor, and its `off_t` is 64-bit on this host -- which trap
 * 2 below depends on.
 */

#include "xdvdfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Deepest directory nesting a single walk will follow.
 *
 * The frame stack is an array so a malformed image cannot recurse the host's stack
 * away, and it is sized rather than grown so the whole walk is allocation-free. The
 * deepest path on this image is `pak/stream/lv74/anim/lv74.pak`, i.e. four directory
 * levels, so this is four times the headroom actually exercised. Exceeding it is
 * REPORTED as XDVDFS_ERR_WALK_LIMIT, never silently pruned: a pruned subtree reads as
 * "those files are not on the disc", which is a wrong answer that looks legitimate.
 */
#define XDVDFS_WALK_DEPTH_MAX 16u

/*
 * Longest path xdvdfs_walk will build, NUL included.
 *
 * The longest on this image is 37 bytes (`pak/stream/lv74/anim/lv74.pak` and friends).
 * A path that would not fit is reported rather than truncated, for the same reason as
 * above: a truncated path is a path to a different file.
 */
#define XDVDFS_WALK_PATH_MAX 1024u

/* Descriptor offsets, relative to the start of the volume descriptor sector. */
#define XDVDFS_DESCRIPTOR_MAGIC_TAIL 0x7ECu
#define XDVDFS_DESCRIPTOR_ROOT_SECTOR 0x14u
#define XDVDFS_DESCRIPTOR_ROOT_SIZE 0x18u

/* Directory entry field offsets, relative to the start of the entry. */
#define DIRENT_LEFT 0u
#define DIRENT_RIGHT 2u
#define DIRENT_SECTOR 4u
#define DIRENT_SIZE 8u
#define DIRENT_ATTRIBUTES 12u
#define DIRENT_NAME_LENGTH 13u

/* Largest single dirent read: the fixed part plus the largest name a u8 can describe. */
#define DIRENT_READ_MAX (XDVDFS_DIRENT_FIXED + XDVDFS_NAME_MAX)

/*
 * Both nil sentinels. TRAP 1 (xdvdfs.h note (a)).
 *
 * This image encodes "no child" as 0, 571 times across its 80 directories (291 left,
 * 280 right), and uses 0xFFFF exactly zero times. 0 is also the offset of the
 * directory's own root node, so a walker that only recognises 0xFFFF pushes the root
 * back onto its stack from every leaf and never terminates. Both values are nil here.
 */
#define DIRENT_LINK_NIL_ZERO 0x0000u
#define DIRENT_LINK_NIL_MAX 0xFFFFu

/* Little-endian loads from a byte buffer. Done a byte at a time rather than by casting
 * the buffer to a uint32_t*: the buffer is not guaranteed aligned (an entry sits at any
 * 4-byte offset within a blob, and the blob base is sector-aligned but the read buffer
 * is not the blob), and a pointer cast would also silently depend on host endianness. */
static uint16_t load_u16(const uint8_t *raw)
{
    return (uint16_t)((uint16_t)raw[0] | (uint16_t)((uint16_t)raw[1] << 8));
}

static uint32_t load_u32(const uint8_t *raw)
{
    return (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
           ((uint32_t)raw[3] << 24);
}

/*
 * TRAP 2 (xdvdfs.h note (b)): every byte offset in this module comes from here, and
 * the widening happens BEFORE the multiply.
 *
 * `sector * XDVDFS_SECTOR_SIZE` in 32 bits overflows at sector 2097152, i.e. at the
 * 4 GiB mark. The highest offset on THIS image is 0xF1280000 (sector 1975552, for
 * `xmv/frd.xmv`), which still fits -- so a 32-bit multiply here would pass every test
 * against this disc and silently read the wrong place on a larger one. There is exactly
 * one function that does this conversion so there is exactly one line to get right.
 */
static uint64_t sector_offset(uint32_t sector)
{
    return (uint64_t)sector * (uint64_t)XDVDFS_SECTOR_SIZE;
}

/* Case-insensitive over ASCII only, mapped by hand. Deliberately not `tolower` or
 * `strcasecmp`: both are locale-dependent, and `src/xbox/kernel_file.c` (paths_equal)
 * makes exactly this argument -- a path must not start matching differently because the
 * host's locale changed. The disc's names are also not promised to be any encoding, so
 * only the 'A'-'Z' range is touched and every other byte compares as stored. */
static uint8_t ascii_lower(uint8_t byte)
{
    if (byte >= 'A' && byte <= 'Z') {
        return (uint8_t)(byte + ('a' - 'A'));
    }
    return byte;
}

static bool name_matches(const xdvdfs_entry *entry, const char *component,
                         size_t component_length)
{
    if ((size_t)entry->name_length != component_length) {
        return false;
    }
    for (size_t i = 0u; i < component_length; i++) {
        uint8_t left = ascii_lower((uint8_t)entry->name[i]);
        uint8_t right = ascii_lower((uint8_t)component[i]);
        if (left != right) {
            return false;
        }
    }
    return true;
}

/*
 * Read exactly `length` bytes at absolute `offset`, or fail.
 *
 * Loops because `pread` is permitted to return a short count, and retries EINTR because
 * a signal arriving mid-read is not a disc error. A return of 0 is end-of-file with the
 * range unsatisfied, which is XDVDFS_ERR_TRUNCATED and not a silent short answer.
 */
static xdvdfs_result read_exact(const xdvdfs_reader *reader, uint64_t offset, void *buffer,
                               size_t length)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0u;

    while (done < length) {
        ssize_t got = pread(reader->fd, out + done, length - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return XDVDFS_ERR_TRUNCATED;
        }
        if (got == 0) {
            return XDVDFS_ERR_TRUNCATED;
        }
        done += (size_t)got;
    }
    return XDVDFS_OK;
}

/*
 * Does the extent [sector, sector+size) lie inside the image?
 *
 * Checked before any read and before any walk, so a dirent claiming an absurd extent is
 * rejected by name rather than by a read failing somewhere further down. `image_bytes`
 * comes from fstat at open time, which is why the reader stores it.
 */
static bool extent_within_image(const xdvdfs_reader *reader, uint32_t sector, uint32_t size)
{
    uint64_t base = sector_offset(sector);

    if (base > reader->image_bytes) {
        return false;
    }
    return (uint64_t)size <= reader->image_bytes - base;
}

/* State for walking one directory's binary tree. Lives on the caller's stack: no
 * directory blob is ever buffered, so this is the only per-directory cost. */
typedef struct {
    uint64_t base; /* byte offset of the blob's first byte */
    uint32_t size; /* blob length IN BYTES, and the only bound the walk uses */
    uint32_t pending[XDVDFS_WALK_PENDING_MAX];
    uint32_t pending_count;
    uint32_t visited;
    uint32_t visit_limit;
} dir_walk;

static xdvdfs_result dir_walk_begin(const xdvdfs_reader *reader, uint32_t sector,
                                    uint32_t size, dir_walk *walk)
{
    memset(walk, 0, sizeof(*walk));

    if (size > XDVDFS_DIR_BYTES_MAX) {
        return XDVDFS_ERR_MALFORMED;
    }
    if (!extent_within_image(reader, sector, size)) {
        return XDVDFS_ERR_TRUNCATED;
    }

    walk->base = sector_offset(sector);
    walk->size = size;

    /*
     * The cycle guard. An entry is at least XDVDFS_DIRENT_FIXED bytes, so a blob of
     * `size` bytes holds at most size/14 distinct entries; the +1 keeps a 0-byte blob's
     * limit from being 0 and costs nothing. A tree whose links revisit a node therefore
     * trips this instead of looping forever. Counting visits rather than keeping a
     * visited SET is what lets this stay stack-sized -- a set over a 262144-byte blob
     * would be 16 KB of bitmap per directory, per nesting level.
     */
    walk->visit_limit = size / XDVDFS_DIRENT_FIXED + 1u;

    /* A zero-length directory has no root node; anything else starts at offset 0. */
    if (size > 0u) {
        walk->pending[0] = 0u;
        walk->pending_count = 1u;
    }
    return XDVDFS_OK;
}

static xdvdfs_result dir_walk_push(dir_walk *walk, uint16_t link)
{
    /* TRAP 1, enforced here: BOTH 0 and 0xFFFF mean "no child". */
    if (link == DIRENT_LINK_NIL_ZERO || link == DIRENT_LINK_NIL_MAX) {
        return XDVDFS_OK;
    }
    if (walk->pending_count >= XDVDFS_WALK_PENDING_MAX) {
        /* Reported, not dropped: dropping a pending node loses a whole subtree, and the
         * caller would see a file that exists as "not found". */
        return XDVDFS_ERR_WALK_LIMIT;
    }
    /* Links are in 4-BYTE UNITS. The product cannot overflow: 0xFFFF * 4 = 0x3FFFC. */
    walk->pending[walk->pending_count] = (uint32_t)link * 4u;
    walk->pending_count++;
    return XDVDFS_OK;
}

/*
 * Pop one node, decode it, and queue its children. Sets `*out_done` when the tree is
 * exhausted. Iterative with an explicit bounded stack, NOT recursive, so no image can
 * drive the host's stack into its guard page.
 *
 * Reads just this entry -- at most 14+255 bytes -- rather than buffering the blob. A
 * blob may be XDVDFS_DIR_BYTES_MAX (262144) bytes, and xdvdfs_walk holds one of these
 * per nesting level, so buffering would put a quarter of a megabyte per level on the
 * stack to save a handful of preads over metadata the header measures at ~160 KB total.
 */
static xdvdfs_result dir_walk_next(const xdvdfs_reader *reader, dir_walk *walk,
                                   xdvdfs_entry *out, bool *out_done)
{
    *out_done = false;

    if (walk->pending_count == 0u) {
        *out_done = true;
        return XDVDFS_OK;
    }

    walk->pending_count--;
    uint32_t offset = walk->pending[walk->pending_count];

    walk->visited++;
    if (walk->visited > walk->visit_limit) {
        return XDVDFS_ERR_WALK_LIMIT;
    }

    /* Entries are 4-byte aligned by construction (links are in 4-byte units and pads
     * round up to 4), so an unaligned offset means the link did not come from a real
     * entry. */
    if ((offset & 3u) != 0u) {
        return XDVDFS_ERR_MALFORMED;
    }
    /*
     * BOUNDS COME FROM THE BLOB'S `size`, NEVER FROM THE ENCLOSING SECTOR. The root
     * directory of this image is 120 bytes inside a 2048-byte sector, and the 1928 bytes
     * after it are 0xFF. Bounding by the sector would let a link reach that padding,
     * where attributes decode as 0xFF -- which has the directory bit set -- and
     * name_length as 0xFF: padding presenting as a directory at sector 0xFFFFFFFF.
     * Subtracted rather than added so the comparison cannot itself overflow.
     */
    if (offset > walk->size || walk->size - offset < XDVDFS_DIRENT_FIXED) {
        return XDVDFS_ERR_MALFORMED;
    }

    uint32_t available = walk->size - offset;
    uint32_t wanted = available < DIRENT_READ_MAX ? available : (uint32_t)DIRENT_READ_MAX;
    uint8_t raw[DIRENT_READ_MAX];

    xdvdfs_result result = read_exact(reader, walk->base + offset, raw, (size_t)wanted);
    if (result != XDVDFS_OK) {
        return result;
    }

    uint16_t left = load_u16(&raw[DIRENT_LEFT]);
    uint16_t right = load_u16(&raw[DIRENT_RIGHT]);
    uint8_t name_length = raw[DIRENT_NAME_LENGTH];

    /* The name has no terminator, so its length is load-bearing: it must fit inside the
     * blob, which also means it fits inside what was just read. */
    if ((uint32_t)XDVDFS_DIRENT_FIXED + (uint32_t)name_length > available) {
        return XDVDFS_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->start_sector = load_u32(&raw[DIRENT_SECTOR]);
    out->size = load_u32(&raw[DIRENT_SIZE]);
    out->attributes = raw[DIRENT_ATTRIBUTES];
    out->is_directory = (out->attributes & XDVDFS_ATTR_DIRECTORY) != 0u;
    out->name_length = name_length;
    memcpy(out->name, &raw[XDVDFS_DIRENT_FIXED], (size_t)name_length);
    out->name[name_length] = '\0';

    /* Rejected here, before the entry is handed back, so no caller can read or walk an
     * extent that is not in the file. */
    if (!extent_within_image(reader, out->start_sector, out->size)) {
        return XDVDFS_ERR_TRUNCATED;
    }

    /*
     * TRAP 3 (xdvdfs.h note (c)): there is NO flat-list fallback. The Python treats
     * "both links 0xFFFF" as "the next entry follows consecutively"; that branch fires
     * zero times on this image and 0xFFFF is exactly what 0xFF padding decodes to, so
     * porting it would make padding enumerate as entries. Both links nil means LEAF.
     */
    result = dir_walk_push(walk, left);
    if (result != XDVDFS_OK) {
        return result;
    }
    return dir_walk_push(walk, right);
}

/*
 * Find one path component in one directory.
 *
 * Walks the tree rather than descending it by comparison. xdvdfs.h gives the reason at
 * length: the on-disc ordering is an empirical finding about one disc that no comparator
 * in `tools/xdvdfs` agrees with, and in `pak/igcs/` an uppercasing and a lowercasing
 * comparator disagree about `igcs1` versus `ig_gasc`, so a descent with the wrong one
 * returns "not found" for a file that exists. The busiest directory here has 31 entries.
 *
 * Stops at the first match, which is a saving and not an ordering assumption: the walk
 * order is arbitrary either way, and finishing the tree would let an unrelated malformed
 * sibling turn a perfectly resolvable lookup into a failure.
 */
static xdvdfs_result dir_find(const xdvdfs_reader *reader, uint32_t sector, uint32_t size,
                              const char *component, size_t component_length,
                              xdvdfs_entry *out, bool *out_found)
{
    dir_walk walk;

    *out_found = false;

    xdvdfs_result result = dir_walk_begin(reader, sector, size, &walk);
    if (result != XDVDFS_OK) {
        return result;
    }

    for (;;) {
        xdvdfs_entry entry;
        bool done = false;

        result = dir_walk_next(reader, &walk, &entry, &done);
        if (result != XDVDFS_OK) {
            return result;
        }
        if (done) {
            return XDVDFS_OK;
        }
        if (name_matches(&entry, component, component_length)) {
            *out = entry;
            *out_found = true;
            return XDVDFS_OK;
        }
    }
}

const char *xdvdfs_result_str(xdvdfs_result result)
{
    /* Never NULL, and every enumerator is named: a diagnostic that prints "(null)" or a
     * bare number is a diagnostic that costs someone an afternoon. */
    switch (result) {
    case XDVDFS_OK:
        return "ok";
    case XDVDFS_ERR_IMAGE:
        return "image could not be opened or stat'd";
    case XDVDFS_ERR_MAGIC:
        return "no MICROSOFT*XBOX*MEDIA at byte offset 0x10000";
    case XDVDFS_ERR_TRUNCATED:
        return "read fell outside the image or returned short";
    case XDVDFS_ERR_MALFORMED:
        return "directory blob or entry is malformed";
    case XDVDFS_ERR_WALK_LIMIT:
        return "directory walk hit its node or pending limit";
    case XDVDFS_ERR_NOT_FOUND:
        return "path is not on the disc";
    default:
        break;
    }
    /* Reached only for a value that is not in the enum at all, e.g. one read back out of
     * corrupted storage. Still a string. */
    return "unknown xdvdfs result";
}

xdvdfs_result xdvdfs_open(xdvdfs_reader *reader, const char *path)
{
    if (reader == NULL || path == NULL) {
        return XDVDFS_ERR_IMAGE;
    }

    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;

    /* O_CLOEXEC because the host spawns processes and none of them have any business
     * inheriting a handle on the user's disc image. */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return XDVDFS_ERR_IMAGE;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        (void)close(fd);
        return XDVDFS_ERR_IMAGE;
    }

    reader->fd = fd;
    reader->image_bytes = (uint64_t)info.st_size;
    reader->open = true;

    /* ONLY offset 0x10000 is probed. xdvdfs.h explains why that is a decision rather
     * than an omission: this image's magic occurs exactly twice in 4 GB, both inside
     * sector 32, and there is no sample here of a video-partition layout to test
     * speculative probing against. */
    uint64_t descriptor_base = sector_offset(XDVDFS_DESCRIPTOR_SECTOR);
    uint8_t descriptor[XDVDFS_SECTOR_SIZE];

    if (descriptor_base + XDVDFS_SECTOR_SIZE > reader->image_bytes) {
        /* TRUNCATED, not MAGIC: the file is too small to hold a descriptor at all, which
         * is a different diagnosis from "something is there and it is not a disc". */
        xdvdfs_close(reader);
        return XDVDFS_ERR_TRUNCATED;
    }

    xdvdfs_result result =
        read_exact(reader, descriptor_base, descriptor, (size_t)XDVDFS_SECTOR_SIZE);
    if (result != XDVDFS_OK) {
        xdvdfs_close(reader);
        return result;
    }

    /* BOTH copies, at +0x00 and +0x7EC. Checking the second is free and makes a
     * coincidental 20-byte match far less likely to be taken for a disc. */
    if (memcmp(&descriptor[0], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH) != 0 ||
        memcmp(&descriptor[XDVDFS_DESCRIPTOR_MAGIC_TAIL], XDVDFS_MAGIC,
               (size_t)XDVDFS_MAGIC_LENGTH) != 0) {
        xdvdfs_close(reader);
        return XDVDFS_ERR_MAGIC;
    }

    reader->root_sector = load_u32(&descriptor[XDVDFS_DESCRIPTOR_ROOT_SECTOR]);
    /* IN BYTES, and NOT sector-aligned: 120 on this image. */
    reader->root_size = load_u32(&descriptor[XDVDFS_DESCRIPTOR_ROOT_SIZE]);

    /* Validate the root blob now so a nonsense descriptor is a failed open rather than a
     * reader that fails on its first lookup. */
    if (reader->root_size > XDVDFS_DIR_BYTES_MAX) {
        xdvdfs_close(reader);
        return XDVDFS_ERR_MALFORMED;
    }
    if (!extent_within_image(reader, reader->root_sector, reader->root_size)) {
        xdvdfs_close(reader);
        return XDVDFS_ERR_TRUNCATED;
    }

    return XDVDFS_OK;
}

void xdvdfs_close(xdvdfs_reader *reader)
{
    if (reader == NULL) {
        return;
    }
    if (reader->fd >= 0) {
        (void)close(reader->fd);
    }
    /* Zeroed rather than just flagged shut, so a stale root sector cannot be read back
     * out of a closed reader, and so a second close is a no-op. */
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
}

uint64_t xdvdfs_image_bytes(const xdvdfs_reader *reader)
{
    if (reader == NULL || !reader->open) {
        return 0u;
    }
    return reader->image_bytes;
}

xdvdfs_result xdvdfs_lookup(const xdvdfs_reader *reader, const char *path, xdvdfs_entry *out)
{
    if (reader == NULL || out == NULL || !reader->open) {
        return XDVDFS_ERR_IMAGE;
    }
    if (path == NULL) {
        /* A caller error, not a disc problem, but it must not be OK. */
        return XDVDFS_ERR_MALFORMED;
    }

    /*
     * The synthesised root. An empty path resolves to it, and every component below is
     * resolved against whatever `current` is at that point, so the root needs no special
     * case in the loop. It has no name because the disc does not give it one.
     */
    xdvdfs_entry current;
    memset(&current, 0, sizeof(current));
    current.start_sector = reader->root_sector;
    current.size = reader->root_size;
    current.attributes = (uint8_t)XDVDFS_ATTR_DIRECTORY;
    current.is_directory = true;

    size_t index = 0u;
    while (path[index] != '\0') {
        /* Either separator, and runs of them are skipped, so a leading `\` or a doubled
         * `//` from a guest path concatenation resolves the same as the clean form. */
        if (path[index] == '/' || path[index] == '\\') {
            index++;
            continue;
        }

        size_t start = index;
        while (path[index] != '\0' && path[index] != '/' && path[index] != '\\') {
            index++;
        }
        size_t component_length = index - start;

        /* Rejected, not interpreted. Resolving `..` would let a guest path reach above
         * whatever the mount layer scoped it to, and that layer is the only thing that
         * knows what the scope is. */
        if (component_length == 1u && path[start] == '.') {
            return XDVDFS_ERR_MALFORMED;
        }
        if (component_length == 2u && path[start] == '.' && path[start + 1u] == '.') {
            return XDVDFS_ERR_MALFORMED;
        }

        /* A non-final component that is not a directory: NOT_FOUND, because the path
         * names nothing, and the image is not malformed for containing a file. */
        if (!current.is_directory) {
            return XDVDFS_ERR_NOT_FOUND;
        }

        xdvdfs_entry found;
        bool matched = false;
        xdvdfs_result result = dir_find(reader, current.start_sector, current.size,
                                       &path[start], component_length, &found, &matched);
        if (result != XDVDFS_OK) {
            return result;
        }
        if (!matched) {
            return XDVDFS_ERR_NOT_FOUND;
        }
        current = found;
    }

    *out = current;
    return XDVDFS_OK;
}

xdvdfs_result xdvdfs_read(const xdvdfs_reader *reader, const xdvdfs_entry *entry,
                         uint64_t offset, void *buffer, uint32_t length, uint32_t *out_read)
{
    if (out_read != NULL) {
        *out_read = 0u;
    }
    if (reader == NULL || entry == NULL || out_read == NULL || !reader->open) {
        return XDVDFS_ERR_IMAGE;
    }
    if (buffer == NULL && length > 0u) {
        return XDVDFS_ERR_IMAGE;
    }

    /* End of file is not an error: a read at or past the end yields zero bytes and OK,
     * which is what the caller's file abstraction has to report anyway. */
    if (offset >= (uint64_t)entry->size) {
        return XDVDFS_OK;
    }

    /* TRAP 4 (xdvdfs.h note (d)): only the requested range is ever touched. The largest
     * file on this disc is 1.08 GB and nothing here allocates per read. */
    uint64_t remaining = (uint64_t)entry->size - offset;
    uint32_t wanted = (uint64_t)length <= remaining ? length : (uint32_t)remaining;
    if (wanted == 0u) {
        return XDVDFS_OK;
    }

    /* Re-validated rather than trusted: `entry` may have been synthesised by a caller,
     * and this is cheaper than the read it guards. */
    if (!extent_within_image(reader, entry->start_sector, entry->size)) {
        return XDVDFS_ERR_TRUNCATED;
    }

    /* TRAP 2 again: 64-bit throughout. `offset` is already 64-bit and the sector base
     * comes from sector_offset(), so a read near the end of a dual-layer disc lands
     * where it should. */
    uint64_t base = sector_offset(entry->start_sector) + offset;

    xdvdfs_result result = read_exact(reader, base, buffer, (size_t)wanted);
    if (result != XDVDFS_OK) {
        return result;
    }

    /* The short count is REPORTED, not padded over: a clamped read is normal at the end
     * of a file and the caller needs the number either way. */
    *out_read = wanted;
    return XDVDFS_OK;
}

/* One directory being enumerated, plus the length of the path prefix that leads to it.
 * The prefix length is stored rather than the prefix itself so popping a level is a
 * single assignment and the path buffer is never copied. */
typedef struct {
    dir_walk dir;
    size_t prefix_length;
} walk_frame;

xdvdfs_result xdvdfs_walk(const xdvdfs_reader *reader, xdvdfs_visit_fn visit, void *context)
{
    if (reader == NULL || visit == NULL || !reader->open) {
        return XDVDFS_ERR_IMAGE;
    }

    /* Both of these are deliberately plain locals: the walk makes no allocation, so it
     * cannot fail halfway for a reason that has nothing to do with the disc. */
    walk_frame frames[XDVDFS_WALK_DEPTH_MAX];
    char path[XDVDFS_WALK_PATH_MAX];

    path[0] = '\0';

    xdvdfs_result result =
        dir_walk_begin(reader, reader->root_sector, reader->root_size, &frames[0].dir);
    if (result != XDVDFS_OK) {
        return result;
    }
    frames[0].prefix_length = 0u;

    size_t depth = 1u;
    while (depth > 0u) {
        walk_frame *frame = &frames[depth - 1u];
        xdvdfs_entry entry;
        bool done = false;

        result = dir_walk_next(reader, &frame->dir, &entry, &done);
        if (result != XDVDFS_OK) {
            return result;
        }
        if (done) {
            /* This directory is exhausted; the parent resumes with its own prefix intact
             * because it never changed. */
            depth--;
            continue;
        }

        /* Build the full path from the frame's prefix. `/` and original case, as the
         * header promises: the callback is the only place names are reported, and
         * uppercasing them there would make a differential against the Python's paths
         * compare unequal for no reason. */
        size_t prefix_length = frame->prefix_length;
        size_t separator = prefix_length > 0u ? 1u : 0u;
        size_t path_length = prefix_length + separator + (size_t)entry.name_length;
        if (path_length + 1u > sizeof(path)) {
            /* Reported, never truncated: a truncated path names a different file. */
            return XDVDFS_ERR_MALFORMED;
        }

        size_t cursor = prefix_length;
        if (separator != 0u) {
            path[cursor] = '/';
            cursor++;
        }
        memcpy(&path[cursor], entry.name, (size_t)entry.name_length);
        cursor += (size_t)entry.name_length;
        path[cursor] = '\0';

        if (!visit(context, path, &entry)) {
            /* An early stop is the caller's decision, so it is success. */
            return XDVDFS_OK;
        }

        /* Depth-first: a subdirectory is entered immediately, which is why the frame
         * pointer above is re-derived from `depth` on every iteration. A zero-length
         * directory has no blob to enter. */
        if (entry.is_directory && entry.size > 0u) {
            if (depth >= XDVDFS_WALK_DEPTH_MAX) {
                return XDVDFS_ERR_WALK_LIMIT;
            }
            result = dir_walk_begin(reader, entry.start_sector, entry.size,
                                    &frames[depth].dir);
            if (result != XDVDFS_OK) {
                return result;
            }
            frames[depth].prefix_length = cursor;
            depth++;
        }
    }

    return XDVDFS_OK;
}
