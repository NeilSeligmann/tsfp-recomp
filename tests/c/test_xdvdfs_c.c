/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XDVDFS reader tests.
 *
 * THREE LAYERS, because each catches something the others cannot:
 *
 *   1. A SYNTHETIC IMAGE BUILT HERE, byte by byte, into a sparse temp file. This is the
 *      only layer that can exercise what the real disc does not contain: a nil link
 *      encoded as 0xFFFF (the real disc uses 0 exclusively, 571 times), a file at a
 *      sector past the 4 GiB mark, and a deliberately malformed directory. It is also
 *      the only layer that can run on a clone with no disc.
 *   2. NEGATIVE IMAGES, one per failure mode, each asserting a SPECIFIC result code
 *      rather than "it failed". A bad directory that reports XDVDFS_ERR_NOT_FOUND is the
 *      failure this module exists to avoid: it reads as "that file is not on the disc".
 *   3. GROUND TRUTH against the user's own `discs/tsfp-xbox.iso`, SKIPPED (exit 0) when
 *      absent. The disc is not in the repo and `tools/ci/check-no-disc-data.sh` bans
 *      committing anything derived from it, so there is no fixture here either -- the
 *      numbers below are the only thing checked in, and they came from the independent
 *      Python oracle in `tools/xdvdfs`.
 *
 * THE 4 GiB FILE IS SPARSE. The synthetic image is ftruncate'd past 4 GiB and has 16
 * bytes written up there; it occupies a few kilobytes on disc. That high sector is the
 * ONLY way to catch a 32-bit sector multiply, because the highest offset on the real
 * disc is 0xF1280000 and still fits in 32 bits -- a 32-bit bug passes every real-disc
 * assertion in this file.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED. The mutants and the check each one breaks are
 * recorded next to the checks they break.
 *
 * `_DEFAULT_SOURCE` is defined here rather than left to the build, because this file
 * needs `pwrite`, `ftruncate` and `mkstemp` whichever target picks it up, and an
 * implicit declaration of `pwrite` would truncate its 64-bit offset to an int.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "xdvdfs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures;
static int checks;

/* Named so a failure says which case it belongs to, not just a line number. */
static const char *current_case = "(none)";

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  [%s]  %s\n", __FILE__, __LINE__, current_case, #cond);   \
            failures++;                                                                 \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  [%s]  %s == %u, expected %u\n", __FILE__, __LINE__,      \
                   current_case, #actual, (unsigned)a_, (unsigned)e_);                   \
            failures++;                                                                 \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        uint64_t a_ = (uint64_t)(actual);                                                \
        uint64_t e_ = (uint64_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  [%s]  %s == %" PRIu64 ", expected %" PRIu64 "\n",        \
                   __FILE__, __LINE__, current_case, #actual, a_, e_);                   \
            failures++;                                                                 \
        }                                                                                \
    } while (0)

/* Result codes are compared by value and PRINTED BY NAME: "expected 6, got 5" is a
 * lookup in the header every time, and the distinction between NOT_FOUND and MALFORMED
 * is the whole point of several checks below. */
#define CHECK_RESULT(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        xdvdfs_result a_ = (actual);                                                     \
        xdvdfs_result e_ = (expected);                                                   \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  [%s]  %s == %s, expected %s\n", __FILE__, __LINE__,      \
                   current_case, #actual, xdvdfs_result_str(a_), xdvdfs_result_str(e_)); \
            failures++;                                                                 \
        }                                                                                \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        const char *a_ = (actual);                                                       \
        const char *e_ = (expected);                                                     \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                                         \
            printf("FAIL %s:%d  [%s]  %s == \"%s\", expected \"%s\"\n", __FILE__,        \
                   __LINE__, current_case, #actual, a_ == NULL ? "(null)" : a_, e_);     \
            failures++;                                                                 \
        }                                                                                \
    } while (0)

/* ===================== image construction helpers ===================== */

#define SECTOR XDVDFS_SECTOR_SIZE

#define BUILD_ENTRY_MAX 128u
#define BUILD_BLOB_MAX 8192u

typedef struct {
    const char *name;
    uint32_t sector;
    uint32_t size;
    uint8_t attributes;
} build_entry;

static void store_u16(uint8_t *raw, uint16_t value)
{
    raw[0] = (uint8_t)(value & 0xFFu);
    raw[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void store_u32(uint8_t *raw, uint32_t value)
{
    raw[0] = (uint8_t)(value & 0xFFu);
    raw[1] = (uint8_t)((value >> 8) & 0xFFu);
    raw[2] = (uint8_t)((value >> 16) & 0xFFu);
    raw[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* Deterministic filler so a byte-exact read assertion is possible without a fixture. */
static uint8_t pattern_byte(uint64_t index)
{
    return (uint8_t)((index * 7u + 3u) & 0xFFu);
}

static bool write_at(int fd, uint64_t offset, const void *data, size_t length)
{
    const uint8_t *in = (const uint8_t *)data;
    size_t done = 0u;

    while (done < length) {
        ssize_t got = pwrite(fd, in + done, length - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        done += (size_t)got;
    }
    return true;
}

/*
 * Serialise `count` entries into `blob` as a real binary tree and return its length.
 *
 * The shape is a heap: node i's children are 2i+1 and 2i+2, so every interior node has a
 * NONZERO left AND a nonzero right link in 4-byte units -- a walker that mishandles
 * either side loses a subtree. Missing children alternate between the two nil encodings,
 * 0 for an even index and 0xFFFF for an odd one, so BOTH sentinels appear in every
 * directory with at least four entries. 0 is the trap the real disc springs (it is also
 * the offset of the directory's own root node) and 0xFFFF is the one the real disc never
 * uses, so neither is reachable from the other layer of these tests.
 *
 * Entries are packed to 4-byte boundaries over a blob pre-filled with 0xFF, which is
 * what the real disc does, so the inter-entry pads and the blob tail are 0xFF here too.
 */
static uint32_t build_directory(const build_entry *entries, size_t count, uint8_t *blob,
                                size_t blob_capacity)
{
    uint32_t offsets[BUILD_ENTRY_MAX];
    uint32_t cursor = 0u;

    memset(blob, 0xFF, blob_capacity);

    for (size_t i = 0u; i < count; i++) {
        uint32_t name_length = (uint32_t)strlen(entries[i].name);
        offsets[i] = cursor;
        cursor += ((XDVDFS_DIRENT_FIXED + name_length) + 3u) / 4u * 4u;
    }

    for (size_t i = 0u; i < count; i++) {
        uint8_t *raw = &blob[offsets[i]];
        uint16_t nil = (i % 2u) == 0u ? 0x0000u : 0xFFFFu;
        size_t left_index = 2u * i + 1u;
        size_t right_index = 2u * i + 2u;
        uint16_t left = left_index < count ? (uint16_t)(offsets[left_index] / 4u) : nil;
        uint16_t right = right_index < count ? (uint16_t)(offsets[right_index] / 4u) : nil;
        uint32_t name_length = (uint32_t)strlen(entries[i].name);

        store_u16(&raw[0], left);
        store_u16(&raw[2], right);
        store_u32(&raw[4], entries[i].sector);
        store_u32(&raw[8], entries[i].size);
        raw[12] = entries[i].attributes;
        raw[13] = (uint8_t)name_length;
        memcpy(&raw[14], entries[i].name, (size_t)name_length);
    }

    return cursor;
}

/* Write a blob at `sector`, with the rest of its sectors 0xFF, like a real directory. */
static bool write_directory(int fd, uint32_t sector, const uint8_t *blob, uint32_t length)
{
    uint8_t padded[BUILD_BLOB_MAX];
    uint32_t rounded = (length + SECTOR - 1u) / SECTOR * SECTOR;

    if (rounded == 0u) {
        rounded = SECTOR;
    }
    if (rounded > sizeof(padded)) {
        return false;
    }
    memset(padded, 0xFF, (size_t)rounded);
    memcpy(padded, blob, (size_t)length);
    return write_at(fd, (uint64_t)sector * SECTOR, padded, (size_t)rounded);
}

static bool write_descriptor(int fd, uint32_t root_sector, uint32_t root_size,
                             bool head_magic_good, bool tail_magic_good)
{
    uint8_t descriptor[SECTOR];

    memset(descriptor, 0, sizeof(descriptor));
    memcpy(&descriptor[0], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    memcpy(&descriptor[0x7EC], XDVDFS_MAGIC, (size_t)XDVDFS_MAGIC_LENGTH);
    if (!head_magic_good) {
        descriptor[0] = 'm';
    }
    if (!tail_magic_good) {
        descriptor[0x7EC + 19u] = 'a';
    }
    store_u32(&descriptor[0x14], root_sector);
    store_u32(&descriptor[0x18], root_size);
    return write_at(fd, (uint64_t)XDVDFS_DESCRIPTOR_SECTOR * SECTOR, descriptor,
                    sizeof(descriptor));
}

/* mkstemp into `path`; the caller unlinks. Returns -1 on failure. RELATIVE to the
 * working directory (the build tree under ctest) rather than /tmp, which a container
 * restart wipes; project convention is relative paths throughout. */
static int temp_image(char *path, size_t path_size)
{
    const char *template = "tsfp_xdvdfs_XXXXXX";

    if (strlen(template) + 1u > path_size) {
        return -1;
    }
    memcpy(path, template, strlen(template) + 1u);
    return mkstemp(path);
}

/* ===================== the synthetic image ===================== */

/*
 * Sector map. Chosen so nothing overlaps and so the root directory's size is NOT
 * sector-aligned (116 bytes) and `sub` spans TWO sectors (2204 bytes), both of which the
 * real disc never does -- its root is 120 bytes but every other directory is exactly one
 * full sector.
 */
#define SYN_ROOT_SECTOR 33u
#define SYN_SUB_SECTOR 34u  /* 2204 bytes, so sectors 34 and 35 */
#define SYN_DEEP_SECTOR 40u
#define SYN_DEEPER_SECTOR 41u
#define SYN_FILLER_DATA_SECTOR 47u
#define SYN_ALPHA_SECTOR 48u /* 3000 bytes, so sectors 48 and 49 */
#define SYN_ZETA_SECTOR 50u
#define SYN_MID_SECTOR 51u
#define SYN_INNER_SECTOR 52u
#define SYN_BOTTOM_SECTOR 53u

/*
 * The high file. 2097152 * 2048 is EXACTLY 0x100000000, so a 32-bit sector multiply
 * yields 0 and reads the start of the image instead. That is the mutation this sector
 * number exists to kill, and it is why the test image is sparse and over 4 GiB.
 */
#define SYN_HIGH_SECTOR 2097152u
#define SYN_HIGH_OFFSET 4294967296ull
#define SYN_HIGH_TEXT "HIGHOFFSET64BIT!"
#define SYN_HIGH_SIZE 16u

#define SYN_ALPHA_SIZE 3000u
#define SYN_MID_SIZE 100u
#define SYN_INNER_TEXT "inner!!"
#define SYN_BOTTOM_TEXT "bottom.bin!"

/* 90 fillers push `sub` past one sector: 44 bytes of named entries + 90 * 24 = 2204. */
#define SYN_FILLER_COUNT 90u

static char filler_names[SYN_FILLER_COUNT][16];

static uint32_t syn_root_size;
static uint32_t syn_sub_size;

static bool build_synthetic(int fd)
{
    uint8_t blob[BUILD_BLOB_MAX];
    build_entry entries[BUILD_ENTRY_MAX];

    /* Deepest level first: a parent's entry has to carry its child's blob length. */
    entries[0].name = "bottom.bin";
    entries[0].sector = SYN_BOTTOM_SECTOR;
    entries[0].size = (uint32_t)strlen(SYN_BOTTOM_TEXT);
    entries[0].attributes = 0x20u;
    uint32_t deeper_size = build_directory(entries, 1u, blob, sizeof(blob));
    if (!write_directory(fd, SYN_DEEPER_SECTOR, blob, deeper_size)) {
        return false;
    }

    entries[0].name = "inner.dat";
    entries[0].sector = SYN_INNER_SECTOR;
    entries[0].size = (uint32_t)strlen(SYN_INNER_TEXT);
    entries[0].attributes = 0x20u;
    entries[1].name = "deeper";
    entries[1].sector = SYN_DEEPER_SECTOR;
    entries[1].size = deeper_size;
    entries[1].attributes = XDVDFS_ATTR_DIRECTORY;
    uint32_t deep_size = build_directory(entries, 2u, blob, sizeof(blob));
    if (!write_directory(fd, SYN_DEEP_SECTOR, blob, deep_size)) {
        return false;
    }

    entries[0].name = "deep";
    entries[0].sector = SYN_DEEP_SECTOR;
    entries[0].size = deep_size;
    entries[0].attributes = XDVDFS_ATTR_DIRECTORY;
    entries[1].name = "mid.dat";
    entries[1].sector = SYN_MID_SECTOR;
    entries[1].size = SYN_MID_SIZE;
    entries[1].attributes = 0x20u;
    for (uint32_t i = 0u; i < SYN_FILLER_COUNT; i++) {
        (void)snprintf(filler_names[i], sizeof(filler_names[i]), "f%03u.bin",
                       (unsigned)i);
        entries[2u + i].name = filler_names[i];
        entries[2u + i].sector = SYN_FILLER_DATA_SECTOR;
        /* Distinct sizes, so a lookup that returned the wrong entry is visible. */
        entries[2u + i].size = i + 1u;
        entries[2u + i].attributes = 0x20u;
    }
    syn_sub_size = build_directory(entries, 2u + SYN_FILLER_COUNT, blob, sizeof(blob));
    if (!write_directory(fd, SYN_SUB_SECTOR, blob, syn_sub_size)) {
        return false;
    }

    entries[0].name = "sub";
    entries[0].sector = SYN_SUB_SECTOR;
    entries[0].size = syn_sub_size;
    entries[0].attributes = XDVDFS_ATTR_DIRECTORY;
    entries[1].name = "ALPHA.BIN";
    entries[1].sector = SYN_ALPHA_SECTOR;
    entries[1].size = SYN_ALPHA_SIZE;
    entries[1].attributes = 0x20u;
    entries[2].name = "Zeta.txt";
    entries[2].sector = SYN_ZETA_SECTOR;
    entries[2].size = 4u;
    entries[2].attributes = 0x20u;
    entries[3].name = "HIGH.DAT";
    entries[3].sector = SYN_HIGH_SECTOR;
    entries[3].size = SYN_HIGH_SIZE;
    entries[3].attributes = 0x20u;
    entries[4].name = "empty.dat";
    entries[4].sector = 0u;
    entries[4].size = 0u;
    entries[4].attributes = 0x20u;
    syn_root_size = build_directory(entries, 5u, blob, sizeof(blob));
    if (!write_directory(fd, SYN_ROOT_SECTOR, blob, syn_root_size)) {
        return false;
    }

    /* File payloads. */
    static uint8_t payload[SYN_ALPHA_SIZE];
    for (uint32_t i = 0u; i < SYN_ALPHA_SIZE; i++) {
        payload[i] = pattern_byte(i);
    }
    if (!write_at(fd, (uint64_t)SYN_ALPHA_SECTOR * SECTOR, payload, SYN_ALPHA_SIZE)) {
        return false;
    }
    if (!write_at(fd, (uint64_t)SYN_FILLER_DATA_SECTOR * SECTOR, payload, SECTOR)) {
        return false;
    }
    if (!write_at(fd, (uint64_t)SYN_ZETA_SECTOR * SECTOR, "zeta", 4u)) {
        return false;
    }
    if (!write_at(fd, (uint64_t)SYN_MID_SECTOR * SECTOR, payload, SYN_MID_SIZE)) {
        return false;
    }
    if (!write_at(fd, (uint64_t)SYN_INNER_SECTOR * SECTOR, SYN_INNER_TEXT,
                  strlen(SYN_INNER_TEXT))) {
        return false;
    }
    if (!write_at(fd, (uint64_t)SYN_BOTTOM_SECTOR * SECTOR, SYN_BOTTOM_TEXT,
                  strlen(SYN_BOTTOM_TEXT))) {
        return false;
    }
    if (!write_at(fd, SYN_HIGH_OFFSET, SYN_HIGH_TEXT, SYN_HIGH_SIZE)) {
        return false;
    }
    /* Round the image out to a whole sector past the high file, as a real dump would. */
    if (ftruncate(fd, (off_t)((uint64_t)(SYN_HIGH_SECTOR + 1u) * SECTOR)) != 0) {
        return false;
    }
    return write_descriptor(fd, SYN_ROOT_SECTOR, syn_root_size, true, true);
}

/* ===================== walk accounting ===================== */

typedef struct {
    uint32_t files;
    uint32_t dirs;
    uint64_t file_bytes;
    uint32_t stop_after;
    uint32_t visits;
    bool dump;
    bool saw_path;
    char wanted[256];
} walk_tally;

static bool tally_visit(void *context, const char *path, const xdvdfs_entry *entry)
{
    walk_tally *tally = (walk_tally *)context;

    tally->visits++;
    if (entry->is_directory) {
        tally->dirs++;
    } else {
        tally->files++;
        tally->file_bytes += (uint64_t)entry->size;
    }
    if (tally->wanted[0] != '\0' && strcmp(path, tally->wanted) == 0) {
        tally->saw_path = true;
    }
    if (tally->dump) {
        printf("%c %10u  %s\n", entry->is_directory ? 'D' : 'F', (unsigned)entry->size,
               path);
    }
    if (tally->stop_after != 0u && tally->visits >= tally->stop_after) {
        return false;
    }
    return true;
}

/* ===================== the synthetic case ===================== */

static void test_synthetic(void)
{
    char path[64];
    int fd = temp_image(path, sizeof(path));
    xdvdfs_reader reader;
    xdvdfs_entry entry;
    uint8_t buffer[4096];
    uint32_t got = 0u;

    current_case = "format facts";
    /* Pinned as LITERALS because every synthetic fixture in this suite is BUILT from
     * the same macros the parser checks, so a mutated magic or sector size moves the
     * fixture and the parser together and only the (skippable) real-disc test would
     * notice. The literals are the XDVDFS on-disc format, measured from real discs. */
    CHECK_EQ_U32(XDVDFS_SECTOR_SIZE, 2048u);
    CHECK_EQ_U32(XDVDFS_DESCRIPTOR_SECTOR, 32u);
    CHECK_EQ_U32(XDVDFS_MAGIC_LENGTH, 20u);
    CHECK(memcmp(XDVDFS_MAGIC, "MICROSOFT*XBOX*MEDIA", 20u) == 0);

    current_case = "synthetic";
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    CHECK(build_synthetic(fd));
    (void)close(fd);

    CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
    CHECK_EQ_U64(xdvdfs_image_bytes(&reader), (uint64_t)(SYN_HIGH_SECTOR + 1u) * SECTOR);

    /* The root blob is 116 bytes: not sector-aligned, exactly as the real disc's 120. */
    CHECK_EQ_U32(syn_root_size, 116u);
    CHECK(syn_sub_size > SECTOR); /* `sub` really does span two sectors */
    CHECK_EQ_U32(syn_sub_size, 2204u);

    current_case = "synthetic/empty path is the root";
    CHECK_RESULT(xdvdfs_lookup(&reader, "", &entry), XDVDFS_OK);
    CHECK(entry.is_directory);
    CHECK_EQ_U32(entry.start_sector, SYN_ROOT_SECTOR);
    CHECK_EQ_U32(entry.size, syn_root_size);
    CHECK_EQ_U32(entry.name_length, 0u);

    current_case = "synthetic/walk counts every entry";
    /* Mutant 1 (only 0xFFFF treated as nil) breaks THIS check: offset 0 gets pushed from
     * every leaf whose link is 0, the directory's own root node is revisited, and the
     * visit cap trips -- a bounded failure rather than the infinite recursion a recursive
     * walker would have. */
    walk_tally tally;
    memset(&tally, 0, sizeof(tally));
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_OK);
    CHECK_EQ_U32(tally.files, 4u + 1u + SYN_FILLER_COUNT + 2u);
    CHECK_EQ_U32(tally.dirs, 3u);
    CHECK_EQ_U64(tally.file_bytes,
                 (uint64_t)SYN_ALPHA_SIZE + 4u + SYN_HIGH_SIZE + 0u + SYN_MID_SIZE +
                     (uint64_t)SYN_FILLER_COUNT * (SYN_FILLER_COUNT + 1u) / 2u +
                     strlen(SYN_INNER_TEXT) + strlen(SYN_BOTTOM_TEXT));

    current_case = "synthetic/walk builds nested paths";
    memset(&tally, 0, sizeof(tally));
    (void)snprintf(tally.wanted, sizeof(tally.wanted), "sub/deep/deeper/bottom.bin");
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_OK);
    CHECK(tally.saw_path);

    current_case = "synthetic/walk honours an early stop";
    memset(&tally, 0, sizeof(tally));
    tally.stop_after = 3u;
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_OK);
    CHECK_EQ_U32(tally.visits, 3u);

    current_case = "synthetic/lookup finds a root file";
    CHECK_RESULT(xdvdfs_lookup(&reader, "ALPHA.BIN", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.size, SYN_ALPHA_SIZE);
    CHECK_EQ_U32(entry.start_sector, SYN_ALPHA_SECTOR);
    CHECK(!entry.is_directory);
    CHECK_EQ_STR(entry.name, "ALPHA.BIN");

    current_case = "synthetic/read from offset 0 is byte-exact";
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, 256u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 256u);
    bool exact = true;
    for (uint32_t i = 0u; i < 256u; i++) {
        if (buffer[i] != pattern_byte(i)) {
            exact = false;
        }
    }
    CHECK(exact);

    current_case = "synthetic/read at a non-zero offset is byte-exact";
    /* 1234 is deliberately inside the second sector of a two-sector file, so an
     * implementation that forgot to add the offset to the sector base is visible. */
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 1234u, buffer, 300u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 300u);
    exact = true;
    for (uint32_t i = 0u; i < 300u; i++) {
        if (buffer[i] != pattern_byte(1234u + i)) {
            exact = false;
        }
    }
    CHECK(exact);

    current_case = "synthetic/read past the end is clamped and the short count reported";
    /* Mutant 5 (drop the clamp) breaks this one. */
    CHECK_RESULT(xdvdfs_read(&reader, &entry, SYN_ALPHA_SIZE - 10u, buffer, 64u, &got),
                 XDVDFS_OK);
    CHECK_EQ_U32(got, 10u);
    exact = true;
    for (uint32_t i = 0u; i < 10u; i++) {
        if (buffer[i] != pattern_byte(SYN_ALPHA_SIZE - 10u + i)) {
            exact = false;
        }
    }
    CHECK(exact);

    current_case = "synthetic/read at or past EOF is 0 bytes and OK";
    got = 0xFFFFFFFFu;
    CHECK_RESULT(xdvdfs_read(&reader, &entry, SYN_ALPHA_SIZE, buffer, 64u, &got),
                 XDVDFS_OK);
    CHECK_EQ_U32(got, 0u);
    got = 0xFFFFFFFFu;
    CHECK_RESULT(xdvdfs_read(&reader, &entry, SYN_ALPHA_SIZE + 5000u, buffer, 64u, &got),
                 XDVDFS_OK);
    CHECK_EQ_U32(got, 0u);

    current_case = "synthetic/a zero-length file reads as 0 bytes";
    CHECK_RESULT(xdvdfs_lookup(&reader, "empty.dat", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.size, 0u);
    got = 0xFFFFFFFFu;
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, 64u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 0u);

    current_case = "synthetic/a file past 4 GiB reads correctly";
    /* Mutant 2 (32-bit sector multiply) breaks THIS check and only this check: sector
     * 2097152 * 2048 is exactly 0x100000000, so a 32-bit product is 0 and the read
     * returns the first 16 bytes of the image instead. No assertion against the real
     * disc can catch it, because the real disc's highest offset still fits in 32 bits. */
    CHECK_RESULT(xdvdfs_lookup(&reader, "HIGH.DAT", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, SYN_HIGH_SECTOR);
    CHECK_EQ_U32(entry.size, SYN_HIGH_SIZE);
    memset(buffer, 0, sizeof(buffer));
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, SYN_HIGH_SIZE, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, SYN_HIGH_SIZE);
    CHECK(memcmp(buffer, SYN_HIGH_TEXT, SYN_HIGH_SIZE) == 0);

    current_case = "synthetic/nested lookup two levels down";
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/deep/inner.dat", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.size, (uint32_t)strlen(SYN_INNER_TEXT));
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, 64u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, (uint32_t)strlen(SYN_INNER_TEXT));
    CHECK(memcmp(buffer, SYN_INNER_TEXT, strlen(SYN_INNER_TEXT)) == 0);

    current_case = "synthetic/nested lookup three levels down";
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/deep/deeper/bottom.bin", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.size, (uint32_t)strlen(SYN_BOTTOM_TEXT));
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, 64u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, (uint32_t)strlen(SYN_BOTTOM_TEXT));
    CHECK(memcmp(buffer, SYN_BOTTOM_TEXT, strlen(SYN_BOTTOM_TEXT)) == 0);

    current_case = "synthetic/every entry of a multi-sector directory is reachable";
    /* f089.bin sits at blob offset 2180, i.e. in the SECOND sector of `sub`. A walk
     * bounded by the enclosing sector instead of the blob size cannot reach it, and a
     * lookup that lost a subtree would miss it too. Each filler also has a distinct size,
     * so finding the wrong entry is not mistaken for finding the right one. */
    for (uint32_t i = 0u; i < SYN_FILLER_COUNT; i++) {
        char filler_path[64];
        /* Formatted from the index rather than from filler_names[i]: the compiler cannot
         * prove a row of a 2-D char array is terminated within its row, and -Werror
         * -Wformat-truncation refuses the copy. */
        (void)snprintf(filler_path, sizeof(filler_path), "sub/f%03u.bin", (unsigned)i);
        xdvdfs_result result = xdvdfs_lookup(&reader, filler_path, &entry);
        if (result != XDVDFS_OK || entry.size != i + 1u) {
            current_case = "synthetic/filler lookup";
            CHECK_RESULT(result, XDVDFS_OK);
            CHECK_EQ_U32(entry.size, i + 1u);
        }
    }
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/f089.bin", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.size, 90u);
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 0u, buffer, 200u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 90u);
    exact = true;
    for (uint32_t i = 0u; i < 90u; i++) {
        if (buffer[i] != pattern_byte(i)) {
            exact = false;
        }
    }
    CHECK(exact);

    current_case = "synthetic/lookup is case-insensitive over ASCII";
    CHECK_RESULT(xdvdfs_lookup(&reader, "alpha.bin", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, SYN_ALPHA_SECTOR);
    CHECK_EQ_STR(entry.name, "ALPHA.BIN"); /* original case is preserved in the entry */
    CHECK_RESULT(xdvdfs_lookup(&reader, "ZETA.TXT", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, SYN_ZETA_SECTOR);
    CHECK_RESULT(xdvdfs_lookup(&reader, "SUB/DEEP/DEEPER/BOTTOM.BIN", &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, SYN_BOTTOM_SECTOR);

    current_case = "synthetic/mixed and duplicated separators resolve the same";
    xdvdfs_entry clean;
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/deep/deeper/bottom.bin", &clean), XDVDFS_OK);
    CHECK_RESULT(xdvdfs_lookup(&reader, "\\\\sub\\deep//DEEPER\\\\bottom.BIN", &entry),
                 XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, clean.start_sector);
    CHECK_EQ_U32(entry.size, clean.size);
    CHECK_EQ_STR(entry.name, clean.name);

    current_case = "synthetic/a directory lookup reports is_directory";
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub", &entry), XDVDFS_OK);
    CHECK(entry.is_directory);
    CHECK_EQ_U32(entry.size, syn_sub_size);
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/deep/", &entry), XDVDFS_OK);
    CHECK(entry.is_directory);

    current_case = "synthetic/a missing path is NOT_FOUND and nothing else";
    CHECK_RESULT(xdvdfs_lookup(&reader, "nope.dat", &entry), XDVDFS_ERR_NOT_FOUND);
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/nope.dat", &entry), XDVDFS_ERR_NOT_FOUND);
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/deep/deeper/nope", &entry),
                 XDVDFS_ERR_NOT_FOUND);
    /* A name longer than a u8 can describe cannot be on the disc, but it is not a
     * malformed image either. */
    CHECK_RESULT(xdvdfs_lookup(&reader,
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               &entry),
                 XDVDFS_ERR_NOT_FOUND);

    current_case = "synthetic/a non-final component that is a file is NOT_FOUND";
    CHECK_RESULT(xdvdfs_lookup(&reader, "ALPHA.BIN/more", &entry), XDVDFS_ERR_NOT_FOUND);

    current_case = "synthetic/dot and dot-dot are rejected, not interpreted";
    CHECK_RESULT(xdvdfs_lookup(&reader, ".", &entry), XDVDFS_ERR_MALFORMED);
    CHECK_RESULT(xdvdfs_lookup(&reader, "..", &entry), XDVDFS_ERR_MALFORMED);
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/..", &entry), XDVDFS_ERR_MALFORMED);
    CHECK_RESULT(xdvdfs_lookup(&reader, "sub/./deep", &entry), XDVDFS_ERR_MALFORMED);
    CHECK_RESULT(xdvdfs_lookup(&reader, "../sub", &entry), XDVDFS_ERR_MALFORMED);
    /* `...` is a legal name, so it must NOT be swept up by the dot rejection. */
    CHECK_RESULT(xdvdfs_lookup(&reader, "...", &entry), XDVDFS_ERR_NOT_FOUND);

    current_case = "synthetic/close is idempotent and clears the reader";
    xdvdfs_close(&reader);
    CHECK_EQ_U64(xdvdfs_image_bytes(&reader), 0u);
    CHECK_RESULT(xdvdfs_lookup(&reader, "ALPHA.BIN", &entry), XDVDFS_ERR_IMAGE);
    xdvdfs_close(&reader);
    CHECK_EQ_U64(xdvdfs_image_bytes(&reader), 0u);

    (void)unlink(path);
}

/* ===================== negative images ===================== */

/* Build a one-sector-root image whose root blob is supplied raw, so a specific
 * malformation can be placed byte by byte. */
static int build_raw_image(char *path, size_t path_size, const uint8_t *root_blob,
                          uint32_t root_blob_length, uint32_t declared_root_size,
                          bool head_magic_good, bool tail_magic_good)
{
    int fd = temp_image(path, path_size);
    uint8_t sector[SECTOR];

    if (fd < 0) {
        return -1;
    }
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, root_blob, (size_t)root_blob_length);
    if (!write_at(fd, (uint64_t)SYN_ROOT_SECTOR * SECTOR, sector, sizeof(sector)) ||
        !write_descriptor(fd, SYN_ROOT_SECTOR, declared_root_size, head_magic_good,
                          tail_magic_good) ||
        ftruncate(fd, (off_t)((uint64_t)(SYN_ROOT_SECTOR + 2u) * SECTOR)) != 0) {
        (void)close(fd);
        (void)unlink(path);
        return -1;
    }
    (void)close(fd);
    return 0;
}

/* One leaf entry, both links nil, used as the base for the magic cases. */
static uint32_t one_entry_blob(uint8_t *blob)
{
    build_entry entry;

    entry.name = "only.dat";
    entry.sector = SYN_ROOT_SECTOR + 1u;
    entry.size = 4u;
    entry.attributes = 0x20u;
    return build_directory(&entry, 1u, blob, 64u);
}

static void test_bad_magic(void)
{
    char path[64];
    uint8_t blob[64];
    uint32_t length = one_entry_blob(blob);
    xdvdfs_reader reader;

    current_case = "negative/the first magic copy is checked";
    if (build_raw_image(path, sizeof(path), blob, length, length, false, true) == 0) {
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_ERR_MAGIC);
        CHECK_EQ_U64(xdvdfs_image_bytes(&reader), 0u);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }

    current_case = "negative/the repeated magic at +0x7EC is checked too";
    /* A mutant that checks only the first copy -- which is all `tools/xdvdfs` does --
     * fails exactly here. */
    if (build_raw_image(path, sizeof(path), blob, length, length, true, false) == 0) {
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_ERR_MAGIC);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }

    current_case = "negative/a good descriptor still opens";
    if (build_raw_image(path, sizeof(path), blob, length, length, true, true) == 0) {
        xdvdfs_entry entry;
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
        CHECK_RESULT(xdvdfs_lookup(&reader, "only.dat", &entry), XDVDFS_OK);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }

    current_case = "negative/a path that is not a file at all";
    CHECK_RESULT(xdvdfs_open(&reader, "tsfp_xdvdfs_does_not_exist_0d1e2f"),
                 XDVDFS_ERR_IMAGE);
}

static void test_name_past_blob(void)
{
    char path[64];
    uint8_t blob[64];
    xdvdfs_reader reader;
    xdvdfs_entry entry;
    walk_tally tally;

    memset(&tally, 0, sizeof(tally));

    /* One entry at offset 0 claiming a 200-byte name inside a 20-byte blob. */
    memset(blob, 0xFF, sizeof(blob));
    store_u16(&blob[0], 0x0000u);
    store_u16(&blob[2], 0x0000u);
    store_u32(&blob[4], SYN_ROOT_SECTOR + 1u);
    store_u32(&blob[8], 4u);
    blob[12] = 0x20u;
    blob[13] = 200u;
    memset(&blob[14], 'x', 6u);

    current_case = "negative/a name running past the blob is MALFORMED, not NOT_FOUND";
    if (build_raw_image(path, sizeof(path), blob, 20u, 20u, true, true) != 0) {
        CHECK(false);
        return;
    }
    CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
    CHECK_RESULT(xdvdfs_lookup(&reader, "anything", &entry), XDVDFS_ERR_MALFORMED);
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_ERR_MALFORMED);
    CHECK_EQ_U32(tally.visits, 0u);
    xdvdfs_close(&reader);
    (void)unlink(path);
}

static void test_link_past_blob(void)
{
    char path[64];
    uint8_t blob[128];
    xdvdfs_reader reader;
    xdvdfs_entry entry;
    walk_tally tally;

    memset(&tally, 0, sizeof(tally));

    /*
     * A 20-byte blob whose only entry links right to offset 20 -- one byte past the end
     * -- and a decodable GHOST entry planted at offset 20, inside the same sector but
     * outside the declared blob.
     *
     * Mutant 3 (bound the walk by the enclosing sector instead of the blob's `size`)
     * breaks THIS check: it reaches the ghost, reports OK, and enumerates an entry that
     * is not in the directory. That is the mechanism behind the padding-is-a-directory
     * bug the header describes, with a readable name instead of 0xFF bytes.
     */
    memset(blob, 0xFF, sizeof(blob));
    store_u16(&blob[0], 0x0000u);
    store_u16(&blob[2], 5u); /* 5 * 4 = byte offset 20 */
    store_u32(&blob[4], SYN_ROOT_SECTOR + 1u);
    store_u32(&blob[8], 4u);
    blob[12] = 0x20u;
    blob[13] = 4u;
    memcpy(&blob[14], "REAL", 4u);

    store_u16(&blob[20], 0x0000u);
    store_u16(&blob[22], 0x0000u);
    store_u32(&blob[24], SYN_ROOT_SECTOR + 1u);
    store_u32(&blob[28], 5u);
    blob[32] = 0x20u;
    blob[33] = 9u;
    memcpy(&blob[34], "GHOST.DAT", 9u);

    current_case = "negative/a link past the blob end is MALFORMED and the ghost is "
                   "unreachable";
    if (build_raw_image(path, sizeof(path), blob, 44u, 20u, true, true) != 0) {
        CHECK(false);
        return;
    }
    CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_ERR_MALFORMED);
    /* Exactly one entry was reachable before the bad link was reached, and the ghost was
     * never one of them. */
    CHECK_EQ_U32(tally.visits, 1u);
    CHECK_RESULT(xdvdfs_lookup(&reader, "GHOST.DAT", &entry), XDVDFS_ERR_MALFORMED);
    /* The in-bounds entry is still resolvable: the bound rejects the bad link, it does
     * not condemn the whole directory before reaching a good node. */
    CHECK_RESULT(xdvdfs_lookup(&reader, "REAL", &entry), XDVDFS_OK);
    xdvdfs_close(&reader);
    (void)unlink(path);
}

static void test_cycle(void)
{
    char path[64];
    uint8_t blob[128];
    xdvdfs_reader reader;
    xdvdfs_entry entry;
    walk_tally tally;

    memset(&tally, 0, sizeof(tally));

    /*
     * Three 20-byte entries where the third links back to the second: 0 -> 20 -> 40 -> 20
     * -> ... Links of 0 are nil, so the cycle cannot be built through offset 0, which is
     * precisely why a visited-set that happens to contain 0 is not a cycle guard.
     *
     * The blob is 60 bytes, so the node cap is 60/14 + 1 = 5 and the walk must stop. If
     * this test ever HANGS rather than fails, the cap is gone.
     */
    memset(blob, 0xFF, sizeof(blob));
    for (uint32_t i = 0u; i < 3u; i++) {
        uint8_t *raw = &blob[i * 20u];
        store_u32(&raw[4], SYN_ROOT_SECTOR + 1u);
        store_u32(&raw[8], 4u);
        raw[12] = 0x20u;
        raw[13] = 4u;
        memcpy(&raw[14], i == 0u ? "aaaa" : (i == 1u ? "bbbb" : "cccc"), 4u);
        store_u16(&raw[0], 0x0000u);
        store_u16(&raw[2], 0x0000u);
    }
    store_u16(&blob[2], 5u);        /* node 0 right -> offset 20 */
    store_u16(&blob[20 + 2u], 10u); /* node 1 right -> offset 40 */
    store_u16(&blob[40 + 2u], 5u);  /* node 2 right -> offset 20, the cycle */

    current_case = "negative/a cycle is reported as WALK_LIMIT and does not hang";
    if (build_raw_image(path, sizeof(path), blob, 60u, 60u, true, true) != 0) {
        CHECK(false);
        return;
    }
    CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_ERR_WALK_LIMIT);
    /* The cap fired, so the number of visits is bounded by it rather than unbounded. */
    CHECK(tally.visits <= 60u / XDVDFS_DIRENT_FIXED + 1u);
    /* A lookup that has to see the whole tree hits the same cap; one that matches the
     * first node does not need to, and still succeeds. */
    CHECK_RESULT(xdvdfs_lookup(&reader, "zzzz", &entry), XDVDFS_ERR_WALK_LIMIT);
    CHECK_RESULT(xdvdfs_lookup(&reader, "aaaa", &entry), XDVDFS_OK);
    xdvdfs_close(&reader);
    (void)unlink(path);
}

static void test_oversized_and_out_of_range(void)
{
    char path[64];
    uint8_t blob[64];
    uint32_t length = one_entry_blob(blob);
    xdvdfs_reader reader;

    current_case = "negative/a root blob over XDVDFS_DIR_BYTES_MAX is MALFORMED";
    /* Declared root size of 1 MiB, which no u16 link in 4-byte units could address. */
    if (build_raw_image(path, sizeof(path), blob, length, 1048576u, true, true) == 0) {
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_ERR_MALFORMED);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }

    current_case = "negative/a root blob inside the limit but past EOF is TRUNCATED";
    if (build_raw_image(path, sizeof(path), blob, length, 65536u, true, true) == 0) {
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_ERR_TRUNCATED);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }

    current_case = "negative/an entry extent past EOF is TRUNCATED";
    /* The entry decodes cleanly; its payload claims 4 GB inside a 70 KB file. */
    memset(blob, 0xFF, sizeof(blob));
    store_u16(&blob[0], 0x0000u);
    store_u16(&blob[2], 0x0000u);
    store_u32(&blob[4], 1000000u);
    store_u32(&blob[8], 4096u);
    blob[12] = 0x20u;
    blob[13] = 8u;
    memcpy(&blob[14], "huge.bin", 8u);
    if (build_raw_image(path, sizeof(path), blob, 24u, 24u, true, true) == 0) {
        xdvdfs_entry entry;
        CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
        CHECK_RESULT(xdvdfs_lookup(&reader, "huge.bin", &entry), XDVDFS_ERR_TRUNCATED);
        xdvdfs_close(&reader);
        (void)unlink(path);
    } else {
        CHECK(false);
    }
}

static void test_result_strings(void)
{
    static const xdvdfs_result all[] = {
        XDVDFS_OK,        XDVDFS_ERR_IMAGE,      XDVDFS_ERR_MAGIC,
        XDVDFS_ERR_TRUNCATED, XDVDFS_ERR_MALFORMED, XDVDFS_ERR_WALK_LIMIT,
        XDVDFS_ERR_NOT_FOUND,
    };

    current_case = "result strings are never NULL and never empty";
    for (size_t i = 0u; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *text = xdvdfs_result_str(all[i]);
        CHECK(text != NULL);
        CHECK(text != NULL && text[0] != '\0');
    }
    /* A value outside the enum still has to produce a string. */
    const char *unknown = xdvdfs_result_str((xdvdfs_result)999);
    CHECK(unknown != NULL);
    CHECK(unknown != NULL && unknown[0] != '\0');
}

/* ===================== ground truth: the real disc ===================== */

/*
 * Candidate locations for the user's own image, RELATIVE first.
 *
 * Relative because this project's scripts do not hardcode absolute paths, and the test
 * runs from whichever build directory CTest picked; the absolute entry is the last
 * resort so an out-of-tree build still finds the disc. An override exists so a user who
 * keeps the image elsewhere does not have to move it.
 */
static const char *real_image_path(void)
{
    static const char *candidates[] = {
        "discs/tsfp-xbox.iso",
        "../discs/tsfp-xbox.iso",
        "../../discs/tsfp-xbox.iso",
    };
    const char *override = getenv("TSFP_XBOX_ISO");

    /* AN OVERRIDE THAT IS SET BUT UNREADABLE IS FATAL, not a skip and not a silent
     * fall through to the candidates. Both were wrong and in opposite directions: this
     * suite returned NULL and skipped, while test_disc_seam fell through and ran
     * against the default image. So one stale environment variable made one suite
     * prove nothing and the other prove its half, with nothing in either output
     * distinguishing that from a correct run. The user named an image; if it is not
     * there, say so. */
    if (override != NULL && override[0] != '\0') {
        if (access(override, R_OK) != 0) {
            fprintf(stderr, "FATAL: $TSFP_XBOX_ISO is set but not readable\n");
            exit(1);
        }
        return override;
    }
    for (size_t i = 0u; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
}

/* Every number here came from `tools/xdvdfs`, the independent Python oracle, and is
 * checked in INSTEAD of a fixture: `tools/ci/check-no-disc-data.sh` bans committing
 * anything derived from the disc, and a count is not disc data. */
#define REAL_FILES 412u
#define REAL_DIRS 79u
#define REAL_FILE_BYTES 4046110016ull

static void check_real_entry(const xdvdfs_reader *reader, const char *path,
                             uint32_t sector, uint32_t size, const char *magic)
{
    xdvdfs_entry entry;
    uint8_t head[4];
    uint32_t got = 0u;

    CHECK_RESULT(xdvdfs_lookup(reader, path, &entry), XDVDFS_OK);
    CHECK_EQ_U32(entry.start_sector, sector);
    CHECK_EQ_U32(entry.size, size);
    CHECK(!entry.is_directory);
    if (magic == NULL) {
        return;
    }
    memset(head, 0, sizeof(head));
    CHECK_RESULT(xdvdfs_read(reader, &entry, 0u, head, 4u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 4u);
    CHECK(memcmp(head, magic, 4u) == 0);
}

static void test_real_image(void)
{
    const char *path = real_image_path();
    xdvdfs_reader reader;
    xdvdfs_entry entry;
    xdvdfs_entry mixed;
    uint8_t buffer[64];
    uint32_t got = 0u;

    if (path == NULL) {
        printf("SKIP real image: discs/tsfp-xbox.iso not present (it is the user's own "
               "disc and is not committed); synthetic coverage still ran\n");
        return;
    }

    current_case = "real/open";
    CHECK_RESULT(xdvdfs_open(&reader, path), XDVDFS_OK);
    if (!reader.open) {
        return;
    }
    CHECK_EQ_U32(reader.root_sector, 264u);
    CHECK_EQ_U32(reader.root_size, 120u); /* bytes, and not sector-aligned */

    current_case = "real/walk agrees with the Python oracle on counts and total bytes";
    walk_tally tally;
    memset(&tally, 0, sizeof(tally));
    tally.dump = getenv("XDVDFS_DUMP") != NULL;
    CHECK_RESULT(xdvdfs_walk(&reader, tally_visit, &tally), XDVDFS_OK);
    CHECK_EQ_U32(tally.files, REAL_FILES);
    CHECK_EQ_U32(tally.dirs, REAL_DIRS);
    CHECK_EQ_U64(tally.file_bytes, REAL_FILE_BYTES);

    current_case = "real/default.xbe";
    check_real_entry(&reader, "default.xbe", 265u, 6270976u, "XBEH");

    current_case = "real/pak/anim/bpose.pak";
    check_real_entry(&reader, "pak/anim/bpose.pak", 1357948u, 1104u, "P5CK");

    current_case = "real/pak/stream/lv74/anim/lv74.pak (deepest, smallest)";
    check_real_entry(&reader, "pak/stream/lv74/anim/lv74.pak", 1865270u, 816u, NULL);

    current_case = "real/pak/musicts.pak (largest file, 64-bit offset path)";
    check_real_entry(&reader, "pak/musicts.pak", 194360u, 1080724016u, NULL);
    CHECK_RESULT(xdvdfs_lookup(&reader, "pak/musicts.pak", &entry), XDVDFS_OK);
    /* Near the 1 GB mark INSIDE the file: proves the read offset is added in 64 bits and
     * that nothing here tried to materialise 1.08 GB to get at it. */
    CHECK_RESULT(xdvdfs_read(&reader, &entry, 1080724000ull, buffer, 16u, &got), XDVDFS_OK);
    CHECK_EQ_U32(got, 16u);

    current_case = "real/xmv/frd.xmv (highest byte offset on the disc)";
    check_real_entry(&reader, "xmv/frd.xmv", 1975552u, 1388544u, NULL);
    CHECK_RESULT(xdvdfs_lookup(&reader, "xmv/frd.xmv", &entry), XDVDFS_OK);
    CHECK_EQ_U64((uint64_t)entry.start_sector * XDVDFS_SECTOR_SIZE, 0xF1280000ull);
    CHECK_RESULT(xdvdfs_read(&reader, &entry, entry.size - 8u, buffer, 32u, &got),
                 XDVDFS_OK);
    CHECK_EQ_U32(got, 8u);

    current_case = "real/a case-insensitive mixed-separator path finds the same entry";
    CHECK_RESULT(xdvdfs_lookup(&reader, "pak/anim/bpose.pak", &entry), XDVDFS_OK);
    CHECK_RESULT(xdvdfs_lookup(&reader, "PAK\\ANIM\\BPOSE.PAK", &mixed), XDVDFS_OK);
    CHECK_EQ_U32(mixed.start_sector, entry.start_sector);
    CHECK_EQ_U32(mixed.size, entry.size);
    CHECK_EQ_STR(mixed.name, entry.name);

    current_case = "real/the directory `pak` is a directory";
    CHECK_RESULT(xdvdfs_lookup(&reader, "pak", &entry), XDVDFS_OK);
    CHECK(entry.is_directory);
    CHECK_EQ_U32(entry.attributes, XDVDFS_ATTR_DIRECTORY);
    CHECK_RESULT(xdvdfs_lookup(&reader, "\\PAK\\", &entry), XDVDFS_OK);
    CHECK(entry.is_directory);

    current_case = "real/a missing path on a real disc is still NOT_FOUND";
    CHECK_RESULT(xdvdfs_lookup(&reader, "pak/not_a_real_file.pak", &entry),
                 XDVDFS_ERR_NOT_FOUND);

    xdvdfs_close(&reader);
}

/* CTest's SKIP_RETURN_CODE for the ground-truth registration. 77 is the autotools
 * convention for "skipped". */
#define TEST_SKIP_EXIT 77

/*
 * TWO CTEST REGISTRATIONS, not one, and the split is the point.
 *
 * The synthetic half proves the walk, the bounds and the nil handling against images
 * this file builds itself: 185 checks that run in any checkout and must never be
 * skipped. The ground-truth half needs the user's own disc, which is their property
 * and is never committed.
 *
 * Collapsing both into one suite forced a choice between two lies. Exit 0 on a
 * missing disc -- what this did -- made `ctest` print "Passed" for a run that proved
 * only half of what the name implies, with the announcement buried in stdout nobody
 * reads on a green run. Exiting 77 instead would discard the 185 synthetic checks
 * that genuinely did run. So CMake registers this binary twice: bare, where a missing
 * disc is fine, and with --require-disc, where it is a reported SKIP.
 */
int main(int argc, char **argv)
{
    const bool require_disc = argc > 1 && strcmp(argv[1], "--require-disc") == 0;
    if (require_disc && real_image_path() == NULL) {
        printf("SKIP ground truth: no Xbox disc image (looked for $TSFP_XBOX_ISO and\n"
               "     discs/tsfp-xbox.iso relative to the checkout). The image is the\n"
               "     user's property and is never committed. The synthetic half of this\n"
               "     suite is registered separately and DID run.\n");
        return TEST_SKIP_EXIT;
    }

    test_synthetic();
    test_bad_magic();
    test_name_past_blob();
    test_link_past_blob();
    test_cycle();
    test_oversized_and_out_of_range();
    test_result_strings();
    test_real_image();

    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
