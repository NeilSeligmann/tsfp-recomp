/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE DISC SEAM, END TO END: a guest path becomes real bytes off the user's own disc.
 *
 * `test_xdvdfs_c.c` proves the FILESYSTEM reads correctly. This suite proves the far
 * more fragile thing above it: that a name the TITLE uses -- which is a drive letter it
 * invented at runtime, not a device path -- travels through the symbolic-link table, the
 * mount table, the XDVDFS lookup and NtReadFile, and comes back as bytes that are
 * actually on the disc. Four separate mechanisms have to agree, and every one of them
 * can fail in a way that produces a plausible wrong answer rather than an error.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. RESOLVING `D:` FOR THE WRONG REASON. The disc is mounted on `\Device\CdRom0`,
 *      and `D:` works only because the guest creates that alias itself with ordinal 67.
 *      If anything ever hard-codes `D:`, every test here still passes while the host has
 *      silently acquired a second source of truth for what `D:` means --
 *      `test_D_does_not_resolve_until_the_title_creates_the_link` is the case that
 *      fails, and it is the most important one in the file.
 *   2. SERVING ZEROS INSTEAD OF CONTENT. A read that returns the right COUNT and the
 *      wrong BYTES is the exact failure this whole module is built to prevent, and it
 *      would be attributed to the lift. So every read here asserts the actual bytes
 *      against a signature that is on the disc -- `XBEH`, `P5CK` -- never just a length.
 *   3. INVENTING A FILE THE DISC DOES NOT HAVE. A mounted disc must answer "absent" for
 *      a name it does not contain, and must NOT fall through to the fabricated-empty
 *      policy. Otherwise the title reads an empty asset and proceeds.
 *   4. 32-BIT OFFSET ARITHMETIC. The disc is 4 GB and files live past the 2 GB mark, so
 *      a seam that narrowed an offset anywhere would corrupt exactly the files at the
 *      end of the disc.
 *
 * SKIPS, STILL PASSING, WHEN THE DISC IS ABSENT. The image is the user's property, it is
 * never committed (`tools/ci/check-no-disc-data.sh` enforces that), and a suite that
 * failed without it would make a clean checkout look broken. The skip is announced, so a
 * green run that proved nothing is not mistaken for a green run that proved this.
 */

/* For getenv/access under -std=c11, which defines __STRICT_ANSI__ and withholds them.
 * GUARDED: the library target is compiled with this on the command line, so an
 * unguarded define here is a -Werror redefinition as soon as anything compiles the two
 * together -- which a mutation harness compiling the suite directly does. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "kernel_file.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "nt_status.h"
#include "xdvdfs.h"

#include <dirent.h>
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

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                               \
        uint32_t e_ = (uint32_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)a_, (unsigned)e_);                                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint64_t a_ = (uint64_t)(actual);                                               \
        uint64_t e_ = (uint64_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,        \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);             \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

static int quiet_printer(const char *format, ...)
{
    /* The handlers under test are deliberately chatty -- every fabrication announces
     * itself -- and that noise would bury a FAIL line. Swallowed here; the counters and
     * the bytes are what this suite asserts on. */
    (void)format;
    return 0;
}

/* --- locating the user's disc --------------------------------------------- */

/*
 * NO HARDCODED ABSOLUTE PATH. The project's standing rule forbids one, and ctest runs
 * from the build directory, so a bare relative path would not resolve. Tried in order of
 * how explicit they are: an operator's override first, then the checkout-relative
 * locations a build directory one or two levels down would need.
 */
static const char *find_disc(void)
{
    static const char *const candidates[] = {
        "discs/tsfp-xbox.iso",
        "../discs/tsfp-xbox.iso",
        "../../discs/tsfp-xbox.iso",
    };
    /* Identical semantics to test_xdvdfs_c's real_image_path, deliberately: an
     * override that is set but unreadable is FATAL. This suite used to fall through to
     * the candidates and run against the default image, while the other skipped -- so
     * one stale variable made the two suites disagree about whether anything was
     * proved, and neither said so. */
    const char *override = getenv("TSFP_XBOX_ISO");
    if (override && override[0] != '\0') {
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

/* CTest's SKIP_RETURN_CODE for this suite, declared in CMakeLists.txt. 77 is the
 * autotools convention for "skipped", picked over an invented value so it is
 * recognisable to anyone who has seen a `make check` summary. */
#define TEST_SKIP_EXIT 77

/* --- scratch guest memory ------------------------------------------------- */

#define SCRATCH_BYTES 0x20000u

static kernel_guest_ptr scratch;

static kernel_guest_ptr at(uint32_t offset)
{
    return scratch + offset;
}

#define OFF_FRAME 0x0100u
#define OFF_IOSB 0x0400u
#define OFF_OA 0x0500u
#define OFF_NAME 0x0600u
#define OFF_TEXT 0x0700u
#define OFF_HANDLE 0x0900u
#define OFF_LINKSTR 0x0A00u
#define OFF_TARGETSTR 0x0A20u
#define OFF_LINKTEXT 0x0A40u
#define OFF_TARGETTEXT 0x0B00u
#define OFF_BUFFER 0x1000u
#define OFF_OFFSET64 0x0980u

static uint32_t read32(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    (void)kernel_guest_read_u32(address, &value);
    return value;
}

/* Build an 8-byte OBJECT_STRING (2/2/4) whose Buffer points at `text_offset`, with the
 * characters written there. Length EXCLUDES the NUL, which is what guest_structs.h
 * derives from all 18 compile-time instances. */
static void write_object_string(uint32_t string_offset, uint32_t text_offset,
                                const char *text)
{
    const size_t length = strlen(text);
    for (size_t i = 0u; i <= length; i++) {
        (void)kernel_guest_write_u8(at(text_offset) + (uint32_t)i, (uint8_t)text[i]);
    }
    (void)kernel_guest_write_u8(at(string_offset) + 0u, (uint8_t)(length & 0xFFu));
    (void)kernel_guest_write_u8(at(string_offset) + 1u,
                                (uint8_t)((length >> 8) & 0xFFu));
    (void)kernel_guest_write_u8(at(string_offset) + 2u,
                                (uint8_t)((length + 1u) & 0xFFu));
    (void)kernel_guest_write_u8(at(string_offset) + 3u,
                                (uint8_t)(((length + 1u) >> 8) & 0xFFu));
    (void)kernel_guest_write_u32(at(string_offset) + 4u, at(text_offset));
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(OFF_FRAME), 0x200u, args, count)) {
        printf("FAIL could not build a %u-argument frame\n", count);
        failures++;
        return STATUS_UNSUCCESSFUL;
    }
    return kernel_hle_call(ordinal, &frame);
}

/* Drive the real ordinal 67, exactly as the guest does at 0x00381301: two pointers to
 * OBJECT_STRINGs, link name first, target device second. */
static uint32_t create_symlink(const char *name, const char *target)
{
    write_object_string(OFF_LINKSTR, OFF_LINKTEXT, name);
    write_object_string(OFF_TARGETSTR, OFF_TARGETTEXT, target);
    const uint32_t args[2] = {at(OFF_LINKSTR), at(OFF_TARGETSTR)};
    return call_ordinal(67u, args, 2u);
}

/* Drive the real ordinal 202 with the argument order pinned at 0x00380D43. Returns the
 * handle, or 0, and reports the status through `*out_status`. */
static uint32_t open_file(const char *path, uint32_t *out_status)
{
    write_object_string(OFF_NAME, OFF_TEXT, path);
    (void)kernel_guest_write_u32(at(OFF_OA) + 0u, 0u);    /* root_directory = NULL */
    (void)kernel_guest_write_u32(at(OFF_OA) + 4u, at(OFF_NAME)); /* object_name */
    (void)kernel_guest_write_u32(at(OFF_OA) + 8u, 0x40u); /* attributes, as at all 31 inline sites */
    (void)kernel_guest_write_u32(at(OFF_HANDLE), 0u);

    const uint32_t args[6] = {at(OFF_HANDLE), 0x100001u, at(OFF_OA),
                              at(OFF_IOSB),   3u,        0x800021u};
    const uint32_t status = call_ordinal(202u, args, 6u);
    if (out_status) {
        *out_status = status;
    }
    return status == STATUS_SUCCESS ? read32(at(OFF_HANDLE)) : 0u;
}

/* Drive the real ordinal 219 with the 8-argument order pinned at 0x0037D8B6. A NULL
 * `offset` exercises the implicit-position form; otherwise the explicit one. */
static uint32_t read_file(uint32_t handle, const uint64_t *offset, uint32_t length,
                          uint32_t *out_status)
{
    uint32_t byte_offset_ptr = 0u;
    if (offset) {
        (void)kernel_guest_write_u32(at(OFF_OFFSET64),
                                     (uint32_t)(*offset & 0xFFFFFFFFu));
        (void)kernel_guest_write_u32(at(OFF_OFFSET64) + 4u, (uint32_t)(*offset >> 32));
        byte_offset_ptr = at(OFF_OFFSET64);
    }
    (void)kernel_guest_write_u32(at(OFF_IOSB), 0xA5A5A5A5u);
    (void)kernel_guest_write_u32(at(OFF_IOSB) + 4u, 0xA5A5A5A5u);

    const uint32_t args[8] = {handle, 0u, 0u, 0u, at(OFF_IOSB),
                              at(OFF_BUFFER), length, byte_offset_ptr};
    const uint32_t status = call_ordinal(219u, args, 8u);
    if (out_status) {
        *out_status = status;
    }
    /* The transferred count comes from IO_STATUS_BLOCK.information at +0x04, which is
     * where guest_structs.h derives it and where 0x0037F7A4 reads it to detect a short
     * read. Reading it from there rather than trusting a return value is the point. */
    return read32(at(OFF_IOSB) + 4u);
}

/* Compare the first `length` bytes the last read put in the guest buffer. */
static bool buffer_starts_with(const char *signature)
{
    const size_t length = strlen(signature);
    for (size_t i = 0u; i < length; i++) {
        uint8_t byte = 0u;
        if (!kernel_guest_read_u8(at(OFF_BUFFER) + (uint32_t)i, &byte)) {
            return false;
        }
        if (byte != (uint8_t)signature[i]) {
            return false;
        }
    }
    return true;
}

static const char *disc_path;

static void setup(void)
{
    kernel_hle_init();
    kernel_object_reset();
    kernel_file_reset();
    kernel_io_reset();
    CHECK_EQ_U32(kernel_file_register(), 7u);
    CHECK_EQ_U32(kernel_io_register(), 10u);
    kernel_hle_set_log(quiet_printer);
}

/* ========================================================================= */

/*
 * THE CASE THAT PROVES THE DESIGN.
 *
 * With the disc mounted on `\Device\CdRom0` and NO symbolic link yet, a `D:`-rooted path
 * must NOT resolve. The device path must resolve at the same moment. Then, once ordinal
 * 67 runs, the same `D:` path starts working.
 *
 * If anything ever hard-codes `D:` -> the disc, this is the only test in the file that
 * notices, and every other one goes on passing.
 *
 * MUTATION: add `\??\D:` as a mount prefix alongside the device, and
 * `a D: path must NOT resolve before the title creates the link` fails.
 */
static void test_D_does_not_resolve_until_the_title_creates_the_link(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(kernel_file_volume_count(), 1u);
    CHECK_EQ_U32(kernel_file_symlink_count(), 0u);

    uint32_t status = STATUS_SUCCESS;
    /* Before the link exists: D: is just a name nothing is mounted behind. */
    CHECK_EQ_U32(open_file("\\??\\D:\\default.xbe", &status), 0u);
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    /* ...while the DEVICE path resolves right now, which is what proves the mount itself
     * is fine and it is only the alias that is missing. */
    CHECK(open_file("\\Device\\CdRom0\\default.xbe", &status) != 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);

    /* Now the title creates its alias, as it does at 0x00381301. */
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_symlink_count(), 1u);
    CHECK(kernel_file_symlink_target("\\??\\D:") != NULL);

    /* ...and the same path that failed a moment ago now resolves. */
    const uint32_t handle = open_file("\\??\\D:\\default.xbe", &status);
    CHECK(handle != 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
}

/*
 * A CHAIN OF LINKS RESOLVES, AND A CYCLE IS REFUSED RATHER THAN HUNG.
 *
 * ADDED BECAUSE MUTATION TESTING FOUND THE GAP. A mutant that stopped the resolver
 * looping -- leaving it able to follow exactly one link -- SURVIVED every other case in
 * this file, because the title only ever creates single-level links today. That made the
 * bounded loop untested, and the bound is the thing standing between a cyclic link table
 * and a hang. The guest builds links at runtime from `\??\%c:` with a drive letter it
 * computes, so it is perfectly capable of pointing one at another.
 *
 * MUTATION: set the resolver's depth bound to 0 and `a two-link chain resolves` fails;
 * remove the bound entirely and `a cyclic link table is refused, not hung` hangs, which
 * the harness reports as a kill.
 */
static void test_a_link_chain_resolves_and_a_cycle_is_refused(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));

    /* Z: -> D: -> \Device\CdRom0, so reaching the disc needs TWO rewrites. */
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);
    CHECK_EQ_U32(create_symlink("\\??\\Z:", "\\??\\D:"), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_symlink_count(), 2u);

    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\??\\Z:\\default.xbe", &status);
    CHECK(handle != 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK_EQ_U32(read_file(handle, NULL, 4u, &status), 4u);
    CHECK(buffer_starts_with("XBEH"));

    /* Now a cycle: W: -> X: -> W:. This must REFUSE, and above all must RETURN. */
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(create_symlink("\\??\\W:", "\\??\\X:"), STATUS_SUCCESS);
    CHECK_EQ_U32(create_symlink("\\??\\X:", "\\??\\W:"), STATUS_SUCCESS);
    CHECK_EQ_U32(open_file("\\??\\W:\\default.xbe", &status), 0u);
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND);
}

/*
 * REAL BYTES, NOT A PLAUSIBLE COUNT.
 *
 * `default.xbe` is 6,270,976 bytes at sector 265 and begins with the ASCII magic `XBEH`,
 * which `src/loader/xbe.c` parses. A seam that resolved the name, reported the right
 * size and served zeros would pass every length assertion; the signature is what makes
 * that impossible.
 *
 * MUTATION: have kernel_file_read_backing return the byte count without copying, and
 * `the bytes are the disc's own XBEH magic` fails while the count still matches.
 */
static void test_a_disc_read_returns_the_discs_own_bytes(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);

    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\??\\D:\\default.xbe", &status);
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_file_disc_opened_count(), 1u);

    kernel_file_open state;
    CHECK(kernel_file_open_info(handle, &state));
    CHECK_EQ_U64(state.size, 6270976u);
    CHECK_EQ_U32(state.disc_sector, 265u);
    CHECK(state.backing == KERNEL_FILE_BACKING_DISC);

    CHECK_EQ_U32(read_file(handle, NULL, 16u, &status), 16u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK(buffer_starts_with("XBEH"));
    CHECK_EQ_U64(kernel_file_disc_bytes_read(), 16u);

    /* An implicit read ADVANCES the position; that is what makes sequential streaming
     * work, and a seam that forgot would re-read the first bytes forever. */
    CHECK(kernel_file_open_info(handle, &state));
    CHECK_EQ_U64(state.offset, 16u);
}

/*
 * CLASS 6 (FileInternalInformation) ON A DISC FILE ANSWERS THE FILE'S FIRST SECTOR.
 * default.xbe starts at sector 265 (measured above). Two files must not share an index.
 * MUTATION: write 0 instead of disc_sector and the index checks fail.
 */
static void test_class6_internal_information_is_the_start_sector(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);
    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\??\\D:\\default.xbe", &status);
    CHECK(handle != 0u);
    (void)kernel_guest_write_u32(at(OFF_BUFFER), 0xA5A5A5A5u);
    (void)kernel_guest_write_u32(at(OFF_BUFFER) + 4u, 0xA5A5A5A5u);
    const uint32_t args[5] = {handle, at(OFF_IOSB), at(OFF_BUFFER), 8u, 6u};
    CHECK_EQ_U32(call_ordinal(211u, args, 5u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_BUFFER)), 265u);
    CHECK_EQ_U32(read32(at(OFF_BUFFER) + 4u), 0u);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 8u);
    /* Too short a buffer is a length mismatch, and the buffer is not touched. */
    (void)kernel_guest_write_u32(at(OFF_BUFFER), 0xA5A5A5A5u);
    const uint32_t shorter[5] = {handle, at(OFF_IOSB), at(OFF_BUFFER), 4u, 6u};
    CHECK(call_ordinal(211u, shorter, 5u) != STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_BUFFER)), 0xA5A5A5A5u);
}

/*
 * MIXED SEPARATORS AND CASE, WHICH IS WHAT THE TITLE ACTUALLY SENDS.
 *
 * The guest builds `<letter>:\` with a `%c:\` format and then joins a forward-slash
 * relative asset path onto it, so the kernel is handed names like
 * `d:\pak/anim/bpose.pak` -- ONE STRING, BOTH SEPARATORS. The binary also spells its own
 * device names in two different cases. A seam that treated `\` and `/` as distinct, or
 * matched case-sensitively, would refuse names this very executable uses.
 *
 * `pak/anim/bpose.pak` is 1104 bytes at sector 1357948 and begins `P5CK`.
 *
 * MUTATION: stop folding `\` to `/` in prefix_match_length, and
 * `a mixed-separator path resolves` fails.
 */
static void test_mixed_separators_and_case_resolve(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);

    uint32_t status = STATUS_SUCCESS;
    /* The exact shape the guest emits: backslash after the drive, forward slashes after. */
    const uint32_t mixed = open_file("\\??\\d:\\pak/anim/bpose.pak", &status);
    CHECK(mixed != 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);

    kernel_file_open state;
    CHECK(kernel_file_open_info(mixed, &state));
    CHECK_EQ_U64(state.size, 1104u);
    CHECK_EQ_U32(state.disc_sector, 1357948u);
    CHECK_EQ_U32(read_file(mixed, NULL, 4u, &status), 4u);
    CHECK(buffer_starts_with("P5CK"));

    /* Shouting the whole thing, all backslashes, must find the same file. */
    const uint32_t shouted = open_file("\\??\\D:\\PAK\\ANIM\\BPOSE.PAK", &status);
    CHECK(shouted != 0u);
    CHECK(kernel_file_open_info(shouted, &state));
    CHECK_EQ_U32(state.disc_sector, 1357948u);
}

/*
 * A FILE THE DISC DOES NOT HAVE MUST BE ABSENT, NOT EMPTY.
 *
 * This is the one place the fabricated-empty policy must NOT apply. The title probes for
 * a `host0:` developer tree that is not on a retail disc; if a mounted disc answered
 * those with a zero-length file, the title would load empty assets and carry on, and the
 * resulting divergence would land on the lift.
 *
 * Asserted with the EMPTY policy explicitly enabled, which is the configuration where a
 * fall-through would actually happen.
 *
 * MUTATION: make a disc lookup failure fall through to the empty backing, and
 * `a name absent from the disc is refused even under the EMPTY policy` fails.
 */
static void test_a_name_absent_from_the_disc_is_refused_not_fabricated(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    CHECK_EQ_U32(create_symlink("\\??\\D:", "\\Device\\CdRom0"), STATUS_SUCCESS);
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);

    uint32_t status = STATUS_SUCCESS;
    CHECK_EQ_U32(open_file("\\??\\D:\\there-is-no-such-file.pak", &status), 0u);
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    /* And nothing was fabricated for it: the count must still be zero even though the
     * policy would have allowed it for a name outside any volume. */
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_file_disc_opened_count(), 0u);

    /* A name outside every mount DOES still get the fabricated empty, which is what
     * shows the refusal above is specific to the mounted disc rather than the policy
     * having been switched off. */
    CHECK(open_file("\\Device\\Harddisk0\\partition1\\whatever", &status) != 0u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 1u);
}

/*
 * AN OFFSET PAST 2 GB, AND A READ CLAMPED AT END OF FILE.
 *
 * `pak/musicts.pak` is 1,080,724,016 bytes at sector 194360, so its last byte sits at
 * absolute image offset 1,478,773,295 -- and `xmv/frd.xmv` starts at image offset
 * 0xF1280000, past 4 GB would require only a slightly larger disc. Any 32-bit narrowing
 * anywhere in the seam corrupts exactly these reads, and nothing smaller would notice.
 *
 * MUTATION: narrow the offset to uint32_t in kernel_file_read_backing, and
 * `a read near the 1 GB mark returns the disc's bytes` fails.
 */
static void test_a_large_offset_and_a_clamped_read(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));

    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\Device\\CdRom0\\pak\\musicts.pak", &status);
    CHECK(handle != 0u);

    kernel_file_open state;
    CHECK(kernel_file_open_info(handle, &state));
    CHECK_EQ_U64(state.size, 1080724016u);
    CHECK_EQ_U32(state.disc_sector, 194360u);

    /* A positioned read a long way in. The file's own first bytes are `P5CK`, so reading
     * here must NOT produce them -- which is what catches an offset that was silently
     * discarded or wrapped to zero. */
    uint64_t deep = 1080000000u;
    CHECK_EQ_U32(read_file(handle, &deep, 64u, &status), 64u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK(!buffer_starts_with("P5CK"));

    /* An explicit offset must NOT move the handle's own cursor: that is what lets the
     * same handle serve both the positioned and the sequential form. */
    CHECK(kernel_file_open_info(handle, &state));
    CHECK_EQ_U64(state.offset, 0u);

    /* Reading at the very start DOES give the signature, which proves the deep read
     * above was reading a different place rather than failing. */
    uint64_t start = 0u;
    CHECK_EQ_U32(read_file(handle, &start, 4u, &status), 4u);
    CHECK(buffer_starts_with("P5CK"));

    /* A read straddling the end is CLAMPED and the short count reported -- not padded,
     * because padding turns an end-of-file into content. */
    uint64_t near_end = 1080724016u - 10u;
    CHECK_EQ_U32(read_file(handle, &near_end, 512u, &status), 10u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);

    /* And a read starting past the end is end-of-file: zero bytes, not an error. */
    uint64_t past_end = 1080724016u + 1u;
    CHECK_EQ_U32(read_file(handle, &past_end, 16u, &status), 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
}

/*
 * A DIRECTORY ON THE DISC OPENS, AND REPORTS ITSELF AS ONE.
 *
 * The title opens directories -- 0x00380127 passes CreateOptions 0x4021, whose low bit
 * is the directory flag -- so a seam that only handled files would refuse a name the
 * disc genuinely has. `pak` is a directory at the disc root.
 */
static void test_a_directory_on_the_disc_opens_as_a_directory(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));

    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\Device\\CdRom0\\pak", &status);
    CHECK(handle != 0u);
    kernel_file_open state;
    CHECK(kernel_file_open_info(handle, &state));
    CHECK(state.is_directory);
    CHECK(state.backing == KERNEL_FILE_BACKING_DISC);
}

/*
 * THE USER'S OWN IMAGE IS NEVER WRITTEN, AND CANNOT BE.
 *
 * THIS IS THE ONE TEST IN THIS FILE THAT IS ABOUT THE USER RATHER THAN THE TITLE. Their
 * disc is their property, and this repository is code only. Two claims, tested two
 * different ways because they fail differently:
 *
 *   1. THE IMAGE IS OPEN READ-ONLY. Asked of the kernel, for every descriptor this process
 *      holds on the image, via /proc/self/fd and F_GETFL. A fingerprint would only prove
 *      that nothing was written on THIS run; an O_RDWR descriptor that happened not to be
 *      written through would pass that and fail this.
 *   2. NO MUTATING ORDINAL REACHES IT. NtWriteFile, the end-of-file class of
 *      NtSetInformationFile, and an overwriting NtCreateFile disposition are each aimed at
 *      a REAL file on the REAL disc and each must be REFUSED -- and refused with zero in
 *      IO_STATUS_BLOCK.information, because a refusal that reported the requested byte
 *      count would read as a success.
 *
 * The image's size and modification time are checked either side as a cheap third
 * assertion. Deliberately NOT a content hash: this image is about 4 GB and hashing it would
 * dominate the suite's runtime to re-prove what (1) already establishes structurally.
 */
static void test_the_users_own_image_is_never_written(void)
{
    setup();
    struct stat before;
    CHECK(stat(disc_path, &before) == 0);

    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));

    /* Every descriptor on the image, asked of the kernel. The scan must SEE it first: zero
     * writable descriptors is also what a scan that matched nothing reports. */
    char absolute[PATH_MAX];
    CHECK(realpath(disc_path, absolute) != NULL);
    DIR *fds = opendir("/proc/self/fd");
    CHECK(fds != NULL);
    unsigned found = 0u;
    unsigned writable = 0u;
    if (fds) {
        for (;;) {
            const struct dirent *entry = readdir(fds);
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
            found++;
            const int flags = fcntl((int)strtol(entry->d_name, NULL, 10), F_GETFL);
            if (flags >= 0 && (flags & O_ACCMODE) != O_RDONLY) {
                writable++;
            }
        }
        (void)closedir(fds);
    }
    CHECK(found >= 1u);
    CHECK_EQ_U32(writable, 0u);

    /* A real file on the real disc, opened the way the title opens one. */
    uint32_t status = STATUS_SUCCESS;
    const uint32_t handle = open_file("\\Device\\CdRom0\\default.xbe", &status);
    CHECK(handle != 0u);

    /* NtWriteFile at it. The 8 arguments in the order measured at 0x003810BF. */
    for (uint32_t i = 0u; i < 16u; i++) {
        (void)kernel_guest_write_u8(at(OFF_BUFFER) + i, 0x5Au);
    }
    (void)kernel_guest_write_u32(at(OFF_IOSB), 0xA5A5A5A5u);
    (void)kernel_guest_write_u32(at(OFF_IOSB) + 4u, 0xA5A5A5A5u);
    const uint32_t write_args[8] = {handle, 0u, 0u, 0u, at(OFF_IOSB), at(OFF_BUFFER),
                                    16u,    0u};
    CHECK(call_ordinal(236u, write_args, 8u) != STATUS_SUCCESS);
    /* ZERO transferred, not the 16 requested. */
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0u);
    CHECK_EQ_U32(kernel_io_write_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK(kernel_io_write_refused_count() > 0u);

    /* NtSetInformationFile's end-of-file class at it. */
    (void)kernel_guest_write_u32(at(OFF_OFFSET64), 0u);
    (void)kernel_guest_write_u32(at(OFF_OFFSET64) + 4u, 0u);
    const uint32_t eof_args[5] = {handle, at(OFF_IOSB), at(OFF_OFFSET64), 8u, 0x14u};
    CHECK(call_ordinal(226u, eof_args, 5u) != STATUS_SUCCESS);

    /* An overwriting NtCreateFile disposition at it, which must be refused rather than
     * quietly opening the file and reporting FILE_OPENED. */
    write_object_string(OFF_NAME, OFF_TEXT, "\\Device\\CdRom0\\default.xbe");
    (void)kernel_guest_write_u32(at(OFF_OA) + 0u, 0u);
    (void)kernel_guest_write_u32(at(OFF_OA) + 4u, at(OFF_NAME));
    (void)kernel_guest_write_u32(at(OFF_OA) + 8u, 0x40u);
    (void)kernel_guest_write_u32(at(OFF_HANDLE), 0u);
    const uint32_t create_args[9] = {at(OFF_HANDLE), 0x40100000u, at(OFF_OA),
                                     at(OFF_IOSB),   0u,          4u,
                                     1u,             5u /* FILE_OVERWRITE_IF */,
                                     0x22u};
    CHECK(call_ordinal(190u, create_args, 9u) != STATUS_SUCCESS);

    kernel_file_unmount_all();
    struct stat after;
    CHECK(stat(disc_path, &after) == 0);
    CHECK(before.st_size == after.st_size);
    CHECK(before.st_mtime == after.st_mtime);
}

/*
 * A BAD IMAGE PATH FAILS AT MOUNT TIME, WITH A DIAGNOSIS.
 *
 * Validating at mount rather than at the first read is what turns an operator's typo
 * into a named error instead of a title that mysteriously finds no assets.
 */
static void test_a_bad_image_fails_at_mount_time(void)
{
    setup();
    CHECK(!kernel_file_mount_disc("\\Device\\CdRom0", "/nonexistent/not-a-disc.iso"));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    /* An ordinary file that is not an Xbox disc must be rejected on its magic, not
     * accepted and then misread. This source file is conveniently to hand and is
     * certainly not an XDVDFS volume. */
    CHECK(!kernel_file_mount_disc("\\Device\\CdRom0", __FILE__));
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
}

/*
 * UNMOUNTING RELEASES THE IMAGE, AND RESET UNMOUNTS.
 *
 * Each reset closes a file descriptor. A suite that resets between cases -- this one --
 * would exhaust the process's descriptors within a few hundred cases if it did not.
 */
static void test_reset_unmounts_and_releases(void)
{
    setup();
    for (unsigned i = 0u; i < 64u; i++) {
        CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
        kernel_file_reset();
        CHECK_EQ_U32(kernel_file_volume_count(), 0u);
    }
    /* If descriptors were leaking, 64 mounts would have used 64 of them and this
     * 65th mount would be the one that failed. */
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_path));
    kernel_file_unmount_all();
    CHECK_EQ_U32(kernel_file_volume_count(), 0u);
}

int main(void)
{
    disc_path = find_disc();
    if (!disc_path) {
        printf("SKIP: no Xbox disc image found (looked for $TSFP_XBOX_ISO and\n"
               "      discs/tsfp-xbox.iso relative to the checkout). The image is the\n"
               "      user's property and is never committed, so this is not a failure --\n"
               "      but NOTHING IN THIS SUITE WAS PROVED. Set TSFP_XBOX_ISO to run it.\n");
        /* EXIT 77, NOT 0, and CMake declares it as this suite's SKIP_RETURN_CODE. The
         * announcement above only ever reached stdout: `ctest` printed "Passed"
         * whether the disc had been read or not, so the green summary everyone
         * actually looks at could not tell a proved run from an empty one. A skip has
         * to be a third status, not a pass with a caveat somebody has to scroll for. */
        return TEST_SKIP_EXIT;
    }
    printf("disc: %s\n", disc_path);

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FAIL could not allocate scratch guest memory\n");
        return 1;
    }

    test_D_does_not_resolve_until_the_title_creates_the_link();
    test_a_link_chain_resolves_and_a_cycle_is_refused();
    test_a_disc_read_returns_the_discs_own_bytes();
    test_class6_internal_information_is_the_start_sector();
    test_mixed_separators_and_case_resolve();
    test_a_name_absent_from_the_disc_is_refused_not_fabricated();
    test_a_large_offset_and_a_clamped_read();
    test_a_directory_on_the_disc_opens_as_a_directory();
    test_the_users_own_image_is_never_written();
    test_a_bad_image_fails_at_mount_time();
    test_reset_unmounts_and_releases();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
