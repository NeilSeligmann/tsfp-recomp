/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NtQueryVolumeInformationFile (218), NtQueryInformationFile (211),
 * NtSetInformationFile (226).
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. THE GEOMETRY PRODUCT. `sub_00380D0D` multiplies the two 32-bit fields of the
 *      class-3 structure together and compares the result against its caller's
 *      expectation, returning 0xC000014F when they disagree. For this image that
 *      expectation is 16384, derived from the XBE's own InitFlags. A wrong product does
 *      not look like a wrong product -- it looks like a hard disk that is not ready. The
 *      first test asserts the product WITHOUT restating its factors, so an edit that
 *      changed both in compensating directions still fails.
 *   2. THE 8+8+4+4 SHAPE. The guest reads 32-bit values at +0x10 and +0x14 and 64-bit
 *      pairs at +0x00 and +0x08. Writing four 32-bit fields instead would put the
 *      geometry where the guest reads the free space, and the product check would then
 *      compare two halves of a unit count.
 *   3. ANSWERING A CLASS WE HAVE NOT DERIVED. Class 5 is reached by another site in
 *      this image. A zero-filled attribute mask is a value the title would act on, so
 *      that must REFUSE. Class 1 (the volume identity) now has a derived layout: the
 *      serial at +0x08 is the only field the reached requests consume, the label
 *      length at +0x0C must be 0 (FATX volumes have no label) and no label byte may
 *      be invented at +0x11.
 *   4. SILENTLY FABRICATING. The free space is ours. If that stops being announced and
 *      counted, a later divergence gets attributed to the title.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY DISC. Every guest structure
 * is built in scratch guest memory at the offsets `guest_structs.h` derives, and the
 * frames are synthesised with `kernel_frame_build`, so no image address is resolved and
 * no disc image is needed.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#define _POSIX_C_SOURCE 200809L
#include "kernel_io.h"
#include "xnet_volume.h"

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
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

static void capture_clear(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

/* --- scratch guest memory ------------------------------------------------- */

#define SCRATCH_BYTES 0x10000u

static kernel_guest_ptr scratch;

static kernel_guest_ptr scratch_at(uint32_t offset)
{
    return scratch + offset;
}

static bool scratch_init(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    return scratch != 0u;
}

/* Offsets within the scratch region. Kept far apart so an overrun by one structure
 * lands in untouched space rather than in the next structure, which would make a
 * bounds bug look like a field-offset bug. */
#define OFF_FRAME 0x0100u
#define OFF_IOSB 0x0400u
#define OFF_FSINFO 0x0500u
#define OFF_SCRATCH2 0x0600u

static uint32_t read32(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    if (!kernel_guest_read_u32(address, &value)) {
        printf("FAIL could not read guest %#x\n", (unsigned)address);
        failures++;
    }
    return value;
}

static uint64_t read64(kernel_guest_ptr address)
{
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4u) << 32);
}

/* Fill a span with a byte that is not zero and not a plausible field value, so a field
 * the handler FAILED to write is distinguishable from one it wrote as zero. 0xA5 rather
 * than 0xFF because 0xFF-filled 64-bit fields read as a plausible "huge" value. */
static void poison(kernel_guest_ptr address, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes; i += 4u) {
        (void)kernel_guest_write_u32(address + i, 0xA5A5A5A5u);
    }
}

/* Invoke an ordinal through the HLE dispatcher with a synthetic stdcall frame. */
static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch_at(OFF_FRAME), 0x100u, args, count)) {
        printf("FAIL could not build a %u-argument frame\n", count);
        failures++;
        return STATUS_UNSUCCESSFUL;
    }
    /* `kernel_hle_call` returns the handler's status directly. A stub returns its
     * default, which is 0 -- indistinguishable from STATUS_SUCCESS -- so every test
     * below also asserts an OBSERVABLE effect (bytes written, or a counter moving)
     * rather than trusting the status alone. A handler that was never registered would
     * otherwise read as a pass. */
    return kernel_hle_call(ordinal, &frame);
}

/* A file handle backed by nothing, which is what the reached site has: the title opens
 * a partition root, not a file with content. Returns 0 on failure. */
static uint32_t open_empty_file(const char *path)
{
    CHECK(kernel_file_add_openable(path));
    /* Drive the real NtOpenFile so the open-file slot is created the way the guest
     * creates it, rather than by reaching behind the module's back. */
    const uint32_t oa = OFF_SCRATCH2;
    const uint32_t name = OFF_SCRATCH2 + 0x20u;
    const uint32_t text = OFF_SCRATCH2 + 0x40u;
    const uint32_t handle_out = OFF_SCRATCH2 + 0x200u;
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        (void)kernel_guest_write_u8(scratch_at(text) + (uint32_t)i,
                                    (uint8_t)path[i]);
    }
    /* OBJECT_STRING: 2/2/4, length EXCLUDING the NUL. */
    (void)kernel_guest_write_u8(scratch_at(name), (uint8_t)(length & 0xFFu));
    (void)kernel_guest_write_u8(scratch_at(name) + 1u, (uint8_t)((length >> 8) & 0xFFu));
    (void)kernel_guest_write_u8(scratch_at(name) + 2u, (uint8_t)((length + 1u) & 0xFFu));
    (void)kernel_guest_write_u8(scratch_at(name) + 3u,
                                (uint8_t)(((length + 1u) >> 8) & 0xFFu));
    (void)kernel_guest_write_u32(scratch_at(name) + 4u, scratch_at(text));
    /* OBJECT_ATTRIBUTES: 12 bytes, no Length field. */
    (void)kernel_guest_write_u32(scratch_at(oa) + 0u, 0u);
    (void)kernel_guest_write_u32(scratch_at(oa) + 4u, scratch_at(name));
    (void)kernel_guest_write_u32(scratch_at(oa) + 8u, 0x40u);

    const uint32_t args[6] = {scratch_at(handle_out), 0x100001u, scratch_at(oa),
                              scratch_at(OFF_IOSB),   3u,        0x800021u};
    const uint32_t status = call_ordinal(202u, args, 6u);
    if (status != STATUS_SUCCESS) {
        return 0u;
    }
    return read32(scratch_at(handle_out));
}

static void reset_all(void)
{
    kernel_hle_init();
    kernel_object_reset();
    kernel_file_reset();
    kernel_io_reset();
    /* Exact counts, not ">= 1". If a binding were dropped, every call below would hit a
     * stub returning 0, which reads as STATUS_SUCCESS -- so the registration count is
     * the first line of defence against a suite that passes against nothing. */
    CHECK_EQ_U32(kernel_file_register(), 7u);
    CHECK_EQ_U32(kernel_io_register(), 10u);
    capture_clear();
}

/* ========================================================================= */

/*
 * THE PRODUCT THE GUEST CHECKS.
 *
 * Asserted against the literal 16384 and against the module's own accessor, and
 * deliberately NOT against KERNEL_IO_HDD_SECTORS_PER_UNIT * BYTES_PER_SECTOR -- that
 * would be restating the implementation and would pass for any pair of factors.
 *
 * MUTATION: change KERNEL_IO_HDD_BYTES_PER_SECTOR to 2048 (so the product becomes
 * 65536) and this fails on the written +0x10/+0x14 fields; the _Static_assert in
 * kernel_io.c catches it at compile time first, which is the point of having both.
 *
 * MUTATION THAT SURVIVED, and the reason the per-field assertions below exist: SWAP
 * the two kernel_guest_write_u32 calls in kernel_io.c. The product is unchanged, so
 * neither the guest's check nor this test noticed. Killed now.
 */
static void test_class3_reports_the_geometry_the_guest_validates(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    poison(scratch_at(OFF_IOSB), 8u);
    capture_clear();

    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x18u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);

    const uint32_t sectors_per_unit = read32(scratch_at(OFF_FSINFO) + 0x10u);
    const uint32_t bytes_per_sector = read32(scratch_at(OFF_FSINFO) + 0x14u);

    /* THE CHECK THAT MATTERS: the product, which sub_00380D0D compares against
     * 0x4000 << (InitFlags >> 30) and rejects with 0xC000014F. */
    CHECK_EQ_U32(sectors_per_unit * bytes_per_sector, 16384u);
    CHECK_EQ_U32(kernel_io_hdd_bytes_per_unit(), 16384u);
    /* Neither factor may be zero or one: a 1 x 16384 "geometry" would satisfy the
     * product while describing a disk with 16 KiB sectors, which no Xbox has. */
    CHECK(sectors_per_unit > 1u);
    CHECK(bytes_per_sector > 1u);

    /*
     * AND EACH FACTOR IN ITS OWN FIELD. Added because a mutation SURVIVED the suite:
     * swapping the two writes in kernel_io.c leaves the product at 32 * 512 == 16384,
     * so the guest's own validation cannot catch it AND neither could anything above.
     * That is the worst shape a defect can have here -- it passes both the oracle and
     * the test, and reports a volume with 32-byte sectors to every other consumer.
     *
     * Asserted against the HARDWARE LITERALS, not against
     * KERNEL_IO_HDD_BYTES_PER_SECTOR: 512-byte sectors is a property of the Xbox disk,
     * independently citable, and 32 of them is what makes a 16 KiB allocation unit.
     * Using the macros would restate the implementation and the swap would survive
     * again, which is the exact trap the comment above this test warns about -- it
     * identified the trap correctly and then still left the ordering uncovered.
     */
    CHECK_EQ_U32(bytes_per_sector, 512u);
    CHECK_EQ_U32(sectors_per_unit, 32u);

    /* The IO_STATUS_BLOCK is 8 bytes, and `information` is the transferred count. */
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0x18u);

    CHECK(captured_contains("FABRICATED"));
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 1u);
}

/*
 * THE 8+8+4+4 SHAPE, asserted by where the handler wrote and did NOT write.
 *
 * The 24-byte structure must be fully written: no byte of it may still hold the poison.
 * And byte 24 onwards must be UNTOUCHED -- the guest declares Length 0x18 and a handler
 * that wrote 32 bytes would corrupt whatever local sits above the buffer, which at the
 * reached site is the frame's saved registers.
 *
 * MUTATION: write the two 32-bit fields at +0x08/+0x0C instead of +0x10/+0x14 and
 * `fields at +0x10/+0x14 must not be poison` fails. Widen the write past 24 bytes and
 * `nothing past +0x18 is touched` fails.
 */
static void test_class3_writes_exactly_24_bytes_in_the_derived_shape(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x18u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);

    /* Every one of the six 4-byte words inside the structure was written. */
    for (uint32_t offset = 0u; offset < 0x18u; offset += 4u) {
        checks++;
        if (read32(scratch_at(OFF_FSINFO) + offset) == 0xA5A5A5A5u) {
            printf("FAIL field at +%#x was never written (still poison)\n",
                   (unsigned)offset);
            failures++;
        }
    }
    /* And nothing past the declared Length was. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x18u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x1Cu), 0xA5A5A5A5u);

    /* The two 64-bit counts: total at +0x00, available at +0x08, and available must not
     * exceed total -- a free-space report larger than the volume is one the title could
     * divide by and get nonsense. */
    const uint64_t total = read64(scratch_at(OFF_FSINFO) + 0x00u);
    const uint64_t available = read64(scratch_at(OFF_FSINFO) + 0x08u);
    CHECK(total > 0u);
    CHECK(available <= total);
    /* Both must read as plausible 64-bit quantities, i.e. their high halves are zero
     * rather than poison -- which is what proves they were written as 64-bit pairs and
     * not as two independent 32-bit fields. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x04u), 0u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x0Cu), 0u);
}

/*
 * A CLASS WHOSE STRUCTURE IS NOT DERIVED MUST REFUSE.
 *
 * Class 5 is reached by a real site in this image (0x0037D732). Answering it with
 * zeros would hand the title a blank attribute mask and let it proceed on it. Class 2
 * is reached by nothing and derived by nothing, so it stands in for every other
 * number.
 *
 * MUTATION: drop the class check so every class falls through to the class-3 writer,
 * and `class 5 refused` and `class 2 refused` both fail.
 */
static void test_an_underived_class_is_refused_and_counted(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    capture_clear();
    const uint32_t class5[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                0x18u, 5u};
    CHECK(call_ordinal(218u, class5, 5u) != STATUS_SUCCESS);
    /* And it wrote NOTHING -- a refusal that still filled the buffer would be worse
     * than answering, because the status says "ignore this" and the bytes say otherwise. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO)), 0xA5A5A5A5u);
    CHECK(captured_contains("REFUSED"));

    const uint32_t class2[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                0x18u, 2u};
    CHECK(call_ordinal(218u, class2, 5u) != STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_io_unknown_class_count(), 2u);
    /* The derived classes must NOT have moved this counter: answering class 1 while
     * still counting it as unknown would double-report every identity query. */
    const uint32_t class1[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                0x18u, 1u};
    CHECK_EQ_U32(call_ordinal(218u, class1, 5u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_io_unknown_class_count(), 2u);
}

/*
 * CLASS 1 WRITES THE IDENTITY AT THE MEASURED OFFSETS, AND NOTHING ELSE.
 *
 * Offsets asserted with INDEPENDENT literals, never through the constants the handler
 * itself uses: +0x08 is where 0x0037D045 and 0x0037D780 read the serial, +0x0C is
 * where 0x0037D75C reads the label byte length, and +0x11 is where the label would
 * start -- so the word at +0x10 must come back with exactly its LOW byte written
 * (the one-byte flag) and its upper three bytes still poison, which pins both that no
 * label byte was invented and that the flag was not widened to 32 bits.
 *
 * This is the 0x0037D02E caller shape: Length is the literal 0x18 that site pushes.
 *
 * MUTATION: write the serial at +0x0C instead of +0x08 and `serial at +0x08` fails.
 * Write the label length as a 32-bit zero at +0x10 and `word at +0x10` fails. Report
 * information 0x18 instead of 0x11 and the IO_STATUS_BLOCK check fails.
 */
static void test_class1_writes_the_identity_at_the_measured_offsets(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    poison(scratch_at(OFF_IOSB), 8u);
    capture_clear();

    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x18u, 1u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);

    /* Creation time: written, as zero, as a 64-bit pair -- not left poison. */
    CHECK_EQ_U64(read64(scratch_at(OFF_FSINFO) + 0x00u), 0u);
    /* THE FIELD THE REACHED REQUESTS CONSUME: the serial at +0x08, the fixed
     * fabricated value, asserted as a literal rather than through the macro. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x08u), 0x74736670u);
    /* Label length 0: FATX volumes have no label, and a nonzero value would send the
     * wrapper's copy loop reading bytes we never wrote. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x0Cu), 0u);
    /* One byte written at +0x10, and NOTHING at +0x11..+0x13: no invented label. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x10u), 0xA5A5A500u);
    /* And nothing past the fixed part within the declared Length, or beyond it. */
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x14u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x18u), 0xA5A5A5A5u);

    /* IO_STATUS_BLOCK: success, and information is the 0x11 bytes transferred (the
     * fixed part through +0x10 plus zero label bytes), not the 0x18 buffer size. */
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0x11u);

    CHECK(captured_contains("FABRICATED"));
    CHECK_EQ_U32(kernel_io_fabricated_identity_count(), 1u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);
}

/*
 * THE REACHED CALLER SHAPE: Length 0x11C, and only the serial is consumed.
 *
 * Replays what sub_0037D5ED does on the calls the boot actually makes (0x00025891 and
 * 0x000258C0, both with a NULL name buffer): a 0x11C-byte pool buffer, class 1, and
 * afterwards the `test eax, eax; jl` at 0x0037D71F, the label-length read at +0x0C
 * and the serial read at +0x08. The label tail of the big buffer must stay untouched
 * -- the wrapper only reads as many label bytes as +0x0C declares, so bytes we never
 * declared must be bytes we never wrote.
 *
 * MUTATION: zero-fill the whole Length (the "helpful" implementation) and the three
 * tail checks fail.
 */
static void test_class1_reached_shape_consumes_only_the_serial(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    /* OFF_FSINFO only has 0x100 bytes before the next structure; the reached shape
     * needs 0x11C, so it gets its own span well clear of everything else. */
    const uint32_t big = 0x0800u;
    poison(scratch_at(big), 0x140u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(big), 0x11Cu, 1u};
    const uint32_t status = call_ordinal(218u, args, 5u);

    /* The caller's own acceptance test is `jl`: any negative status is its error arm. */
    CHECK((int32_t)status >= 0);
    /* What it then consumes. */
    CHECK_EQ_U32(read32(scratch_at(big) + 0x08u), 0x74736670u);
    CHECK_EQ_U32(read32(scratch_at(big) + 0x0Cu), 0u);
    /* The 0x105 bytes of label space it offered stay untouched, start to end. */
    CHECK_EQ_U32(read32(scratch_at(big) + 0x14u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(big) + 0xA0u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(big) + 0x118u), 0xA5A5A5A5u);
    /* information stays 0x11: bytes transferred, not buffer offered. */
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0x11u);
}

/*
 * A CLASS-1 BUFFER BELOW THE STRUCTURE'S SIZEOF IS REFUSED, NOT PARTIALLY FILLED.
 *
 * 0x18 is the sizeof the guest's own compiler folded (the literal push at 0x0037D02E
 * and the `add esi, 0x18` at 0x0037D675), so 0x17 is below every shape this image
 * uses. The status is asserted as the literal 0xC0000004, not through the module's
 * macro.
 *
 * MUTATION: remove the class-1 length guard and `nothing was written` fails.
 */
static void test_class1_short_buffer_is_refused(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    poison(scratch_at(OFF_IOSB), 8u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x17u, 1u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), 0xC0000004u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x00u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x08u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 0u), 0xC0000004u);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0u);
    CHECK_EQ_U32(kernel_io_fabricated_identity_count(), 0u);
}

/*
 * A SHORT BUFFER MUST BE REFUSED, NOT PARTIALLY FILLED.
 *
 * Both measured class-3 sites push exactly 0x18, so a smaller Length is not a shape this
 * image uses -- but a handler that clamped instead of refusing would write 4 fields into
 * a 16-byte buffer and corrupt 8 bytes of a guest frame.
 *
 * MUTATION: remove the `length < KERNEL_IO_FS_SIZE_BYTES` guard and
 * `nothing was written into the short buffer` fails.
 */
static void test_a_buffer_shorter_than_the_structure_is_refused(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_FSINFO), 0x40u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x10u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x00u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x10u), 0xA5A5A5A5u);
}

/*
 * A HANDLE WE NEVER ISSUED, AND A HANDLE OF THE WRONG KIND.
 *
 * If the argument order were wrong, arg0 would be an IoStatusBlock pointer rather than a
 * handle, and a handler that accepted anything would answer happily. Rejecting both a
 * bogus value and a live handle of the wrong kind is what makes that mistake visible.
 *
 * MUTATION: drop the `kind != KERNEL_OBJECT_FILE` test and `a thread handle is refused`
 * fails.
 */
static void test_a_bad_or_wrong_kind_handle_is_refused(void)
{
    reset_all();

    const uint32_t args[5] = {0xDEADBEEFu, scratch_at(OFF_IOSB),
                              scratch_at(OFF_FSINFO), 0x18u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_INVALID_HANDLE);

    const uint32_t thread = kernel_object_create(KERNEL_OBJECT_THREAD, 255u);
    CHECK(thread != 0u);
    const uint32_t wrong[5] = {thread, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                               0x18u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, wrong, 5u), STATUS_INVALID_HANDLE);
}

/*
 * THE REFUSE POLICY MUST ACTUALLY REFUSE.
 *
 * The fabricate/refuse pair exists so the two paths can be COMPARED. If the switch did
 * nothing, every run would silently take the fabricating path and the comparison the
 * pair was built for would be impossible.
 *
 * MUTATION: ignore the policy in the handler and `the refuse policy returns a failure`
 * fails.
 */
static void test_the_refuse_policy_is_honoured_and_writes_nothing(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    kernel_io_set_volume_policy(KERNEL_IO_VOLUME_REFUSE);
    poison(scratch_at(OFF_FSINFO), 0x40u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x18u, 3u};
    CHECK(call_ordinal(218u, args, 5u) != STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO)), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_io_volume_refused_count(), 1u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);

    /* The identity class honours the same switch: a run comparing the two paths must
     * see them diverge on BOTH classes or the comparison lies for one of them. */
    const uint32_t identity[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                  0x18u, 1u};
    CHECK(call_ordinal(218u, identity, 5u) != STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 0x08u), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_io_volume_refused_count(), 2u);
    CHECK_EQ_U32(kernel_io_fabricated_identity_count(), 0u);
}

/*
 * THE FILE POSITION ROUND-TRIPS THROUGH 226 AND BACK OUT OF 211.
 *
 * Class 0x0E is a single 64-bit offset, and Length 8 is what the guest pushes with it.
 * A 64-bit position matters: the largest file on the disc is over 1 GB, so a 32-bit
 * position would wrap on a seek the title is entitled to make. The value chosen here is
 * deliberately above 2^32 to catch exactly that.
 *
 * MUTATION: store the position into a uint32_t, or read only the low half in 226, and
 * `the position above 2^32 round-trips` fails.
 */
static void test_the_file_position_round_trips_as_64_bits(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    const uint64_t wanted = 0x1234567890ULL; /* well above 2^32 */
    (void)kernel_guest_write_u32(scratch_at(OFF_FSINFO),
                                 (uint32_t)(wanted & 0xFFFFFFFFu));
    (void)kernel_guest_write_u32(scratch_at(OFF_FSINFO) + 4u,
                                 (uint32_t)(wanted >> 32));
    const uint32_t set_args[5] = {handle, scratch_at(OFF_IOSB),
                                  scratch_at(OFF_FSINFO), 8u, 0x0Eu};
    CHECK_EQ_U32(call_ordinal(226u, set_args, 5u), STATUS_SUCCESS);

    kernel_file_open state;
    CHECK(kernel_file_open_info(handle, &state));
    CHECK_EQ_U64(state.offset, wanted);

    poison(scratch_at(OFF_SCRATCH2) + 0x400u, 0x20u);
    const uint32_t query_args[5] = {handle, scratch_at(OFF_IOSB),
                                    scratch_at(OFF_SCRATCH2) + 0x400u, 8u, 0x0Eu};
    CHECK_EQ_U32(call_ordinal(211u, query_args, 5u), STATUS_SUCCESS);
    CHECK_EQ_U64(read64(scratch_at(OFF_SCRATCH2) + 0x400u), wanted);
}

/*
 * CLASS 0x22 IS 56 BYTES AND PUTS END-OF-FILE AT +0x28.
 *
 * That offset is the MEASURED part of the layout: the guest reads both halves of a
 * 64-bit quantity there and tests them against zero at 0x00380E11 and 0x00380E16. The
 * guest is checking whether the file is empty, so writing the size anywhere else makes a
 * non-empty file look empty, or the reverse.
 *
 * MUTATION: write end-of-file at +0x20 instead of +0x28 and `end of file is at +0x28`
 * fails while nothing else does -- which is exactly the silent failure the offset guards
 * against.
 */
static void test_class22_is_56_bytes_with_end_of_file_at_0x28(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    const uint32_t buffer = OFF_SCRATCH2 + 0x400u;
    poison(scratch_at(buffer), 0x60u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(buffer), 0x38u,
                              0x22u};
    CHECK_EQ_U32(call_ordinal(211u, args, 5u), STATUS_SUCCESS);

    /* This handle has no content, so end-of-file is 0 -- which is the value the guest's
     * own zero-test is looking for, and it must be a FULL 64-bit zero. */
    CHECK_EQ_U64(read64(scratch_at(buffer) + 0x28u), 0u);
    /* The whole 56 bytes were written, and nothing past them was. */
    for (uint32_t offset = 0u; offset < 0x38u; offset += 4u) {
        checks++;
        if (read32(scratch_at(buffer) + offset) == 0xA5A5A5A5u) {
            printf("FAIL class 0x22 field at +%#x was never written\n",
                   (unsigned)offset);
            failures++;
        }
    }
    CHECK_EQ_U32(read32(scratch_at(buffer) + 0x38u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0x38u);
}

/*
 * A CLASS 226 CANNOT HONOUR MUST BE REFUSED, NOT REPORTED SUCCESSFUL.
 *
 * Two classes now have a derived structure and an effect this host can produce -- the file
 * position and the end of file -- and every other one is refused with a status. A
 * disposition set that returned success would tell the title a mutation happened that did
 * not, and the title would act as though its storage had changed.
 *
 * MUTATION: return STATUS_SUCCESS for every class and `a disposition set is refused`
 * fails.
 */
static void test_an_unhonourable_set_is_refused(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    capture_clear();
    (void)kernel_guest_write_u32(scratch_at(OFF_FSINFO), 1u);
    /* class 0x0D with Length 1 is the disposition set measured at 0x003801B2. */
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO), 1u,
                              0x0Du};
    CHECK(call_ordinal(226u, args, 5u) != STATUS_SUCCESS);
    CHECK(captured_contains("REFUSED"));
    CHECK(kernel_io_unknown_file_class_count() > 0u);
}

/*
 * A WRITE AGAINST A HANDLE WITH NOTHING WRITABLE BEHIND IT IS REFUSED, AND REPORTS ZERO.
 *
 * This suite mounts nothing at all, which is the host's DEFAULT STATE, so a handle here is
 * a fabricated empty file with no host object. The two things asserted are the status and
 * the IO_STATUS_BLOCK.information, and the second is the one that matters: a refused write
 * that reported the requested byte count would look, from the title's side, exactly like a
 * successful one, and the zero-byte file it leaves behind would look like the title's own
 * choice.
 *
 * MUTATION: have `hle_nt_write_file` report `want` rather than the transferred count, and
 * the information assertion below fails while the status one still passes.
 */
static void test_a_write_with_nothing_writable_behind_it_is_refused(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\save.dat");
    CHECK(handle != 0u);
    capture_clear();
    poison(scratch_at(OFF_IOSB), 8u);
    for (uint32_t i = 0u; i < 16u; i++) {
        (void)kernel_guest_write_u8(scratch_at(OFF_SCRATCH2) + 0x400u + i, (uint8_t)i);
    }
    /* The 8 arguments in the order measured at 0x003810BF, with ByteOffset NULL. */
    const uint32_t args[8] = {handle,
                              0u,
                              0u,
                              0u,
                              scratch_at(OFF_IOSB),
                              scratch_at(OFF_SCRATCH2) + 0x400u,
                              16u,
                              0u};
    CHECK(call_ordinal(236u, args, 8u) != STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) +
                        (uint32_t)offsetof(guest_io_status_block, information)),
                 0u);
    CHECK_EQ_U32(kernel_io_write_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK(kernel_io_write_refused_count() > 0u);
    CHECK(captured_contains("REFUSED"));

    /* And the end-of-file class is refused through the same gate rather than separately. */
    (void)kernel_guest_write_u32(scratch_at(OFF_FSINFO), 0u);
    (void)kernel_guest_write_u32(scratch_at(OFF_FSINFO) + 4u, 0u);
    const uint32_t eof_args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO), 8u,
                                  0x14u};
    CHECK(call_ordinal(226u, eof_args, 5u) != STATUS_SUCCESS);
}

/*
 * THE WRITE HANDLER NEEDS ALL EIGHT ARGUMENTS, and fails rather than inventing the ones it
 * cannot read.
 *
 * WHY THIS IS WORTH A TEST. The measured arity table publishes SIX for ordinal 236 and the
 * hand count is EIGHT; an ABI_TABLE row overrides it. If the handler only ever read the six
 * the table claims, Length and ByteOffset would come from whatever followed the frame, and
 * on this host that is plausible-looking garbage rather than an obvious fault. The frame
 * below is clamped to seven slots, so argument 7 is genuinely unreadable.
 */
static void test_the_write_handler_needs_all_eight_arguments(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\save.dat");
    CHECK(handle != 0u);
    capture_clear();

    const uint32_t args[7] = {handle,  0u, 0u, 0u, scratch_at(OFF_IOSB),
                              scratch_at(OFF_SCRATCH2) + 0x400u, 16u};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, scratch_at(OFF_FRAME), 0x100u, args, 7u));
    /* Clamped to exactly the slots built, so reading an eighth FAILS rather than returning
     * whatever happens to follow. */
    frame.stack_limit = scratch_at(OFF_FRAME) + (7u + 1u) * 4u;
    CHECK(kernel_hle_call(236u, &frame) != STATUS_SUCCESS);
    CHECK(captured_contains("could not read argument 7"));
}

/*
 * RESET MUST ACTUALLY CLEAR THE COUNTERS.
 *
 * The counters are how a run reports what it fabricated. A reset that left them set
 * would make the next case inherit the previous one's fabrication count, and the whole
 * record would drift -- silently, and in the direction of looking worse than reality.
 *
 * A DEFECT THIS TEST USED TO MISS. It asserted `kernel_io_read_count() == 0` after the
 * reset and passed, while `kernel_io_reset` cleared three counters out of eight -- because
 * the test never performed a read, so the counter it checked was zero before the reset as
 * well as after. Every counter is now MOVED FIRST and then checked, which is the only
 * version of this test that proves anything.
 */
static void test_reset_clears_the_record(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);
    const uint32_t args[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                              0x18u, 3u};
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);
    CHECK(kernel_io_fabricated_geometry_count() > 0u);

    /* Move the volume-refusal counter. */
    kernel_io_set_volume_policy(KERNEL_IO_VOLUME_REFUSE);
    CHECK(call_ordinal(218u, args, 5u) != STATUS_SUCCESS);
    CHECK(kernel_io_volume_refused_count() > 0u);
    /* Move the underived-volume-class counter. */
    kernel_io_set_volume_policy(KERNEL_IO_VOLUME_FABRICATE);
    const uint32_t class5[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                0x18u, 5u};
    CHECK(call_ordinal(218u, class5, 5u) != STATUS_SUCCESS);
    CHECK(kernel_io_unknown_class_count() > 0u);
    /* Move the fabricated-identity counter. */
    const uint32_t class1[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO),
                                0x18u, 1u};
    CHECK_EQ_U32(call_ordinal(218u, class1, 5u), STATUS_SUCCESS);
    CHECK(kernel_io_fabricated_identity_count() > 0u);
    /* Move the underived-file-class counter. */
    const uint32_t bad_set[5] = {handle, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO), 1u,
                                 0x0Du};
    CHECK(call_ordinal(226u, bad_set, 5u) != STATUS_SUCCESS);
    CHECK(kernel_io_unknown_file_class_count() > 0u);
    /* Move the read counter with a zero-length NtReadFile on an empty-backed handle.
     * Positive-length zero-byte EOF now returns STATUS_END_OF_FILE without counting
     * completion (T1011); zero-length EOF remains a successful completion. */
    const uint32_t read_args[8] = {handle,
                                   0u,
                                   0u,
                                   0u,
                                   scratch_at(OFF_IOSB),
                                   scratch_at(OFF_SCRATCH2) + 0x400u,
                                   0u,
                                   0u};
    CHECK_EQ_U32(call_ordinal(219u, read_args, 8u), STATUS_SUCCESS);
    CHECK(kernel_io_read_count() > 0u);
    /* Move the write-refusal counter. */
    CHECK(call_ordinal(236u, read_args, 8u) != STATUS_SUCCESS);
    CHECK(kernel_io_write_refused_count() > 0u);

    kernel_io_reset();
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);
    CHECK_EQ_U32(kernel_io_fabricated_identity_count(), 0u);
    CHECK_EQ_U32(kernel_io_volume_refused_count(), 0u);
    CHECK_EQ_U32(kernel_io_unknown_class_count(), 0u);
    CHECK_EQ_U32(kernel_io_unknown_file_class_count(), 0u);
    CHECK_EQ_U32(kernel_io_read_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_read(), 0u);
    CHECK_EQ_U32(kernel_io_write_count(), 0u);
    CHECK_EQ_U64(kernel_io_bytes_written(), 0u);
    CHECK_EQ_U32(kernel_io_write_refused_count(), 0u);
    CHECK_EQ_U32(kernel_io_apc_ignored_count(), 0u);
}

/*
 * ORDINAL 198 NtFlushBuffersFile(FileHandle, IoStatusBlock), arity 2 (call site
 * 0x0037D0F6, wrapper `ret 4`, hand row ARITY-OK(198)). Every write is a synchronous
 * pwrite so nothing is buffered here: a flush on an open file completes with
 * STATUS_SUCCESS and Information 0 (INFERRED, the one reached wrapper tests only the
 * sign of the status). A handle that is not an open file is refused.
 *
 * MUTATIONS: return failure for a file, skip the IO_STATUS_BLOCK write, accept a
 * thread handle, or write a nonzero Information, and a check below fails.
 */
static void test_flush_buffers_completes_on_a_file_and_refuses_other_handles(void)
{
    reset_all();
    const uint32_t handle = open_empty_file("\\Device\\Harddisk0\\partition1\\");
    CHECK(handle != 0u);

    poison(scratch_at(OFF_IOSB), 8u);
    const uint32_t args[2] = {handle, scratch_at(OFF_IOSB)};
    CHECK_EQ_U32(call_ordinal(198u, args, 2u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 0u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 0u);
    CHECK_EQ_U32(kernel_io_flush_count(), 1u);

    poison(scratch_at(OFF_IOSB), 8u);
    const uint32_t bad[2] = {0xDEADBEEFu, scratch_at(OFF_IOSB)};
    CHECK_EQ_U32(call_ordinal(198u, bad, 2u), STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 0u), STATUS_INVALID_HANDLE);

    const uint32_t thread = kernel_object_create(KERNEL_OBJECT_THREAD, 255u);
    CHECK(thread != 0u);
    const uint32_t wrong[2] = {thread, scratch_at(OFF_IOSB)};
    CHECK_EQ_U32(call_ordinal(198u, wrong, 2u), STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(kernel_io_flush_count(), 1u);

    /* An IO_STATUS_BLOCK the guest cannot write is a parameter error, never a success that
     * left the title's status block unwritten, and the flush is not counted.
     * BREAKS THIS: reporting STATUS_SUCCESS from the failed IO_STATUS_BLOCK write. */
    const uint32_t unwritable[2] = {handle, 0x2000u};
    CHECK_EQ_U32(call_ordinal(198u, unwritable, 2u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_io_flush_count(), 1u);
}

static void test_actual_mounted_volume_and_strict_refusal(void)
{
    reset_all();
    const uint32_t fabricated = open_empty_file("\\Device\\Harddisk0\\Partition1\\absent");
    CHECK(fabricated != 0u);
    uint32_t args[5] = {fabricated, scratch_at(OFF_IOSB), scratch_at(OFF_FSINFO), 24u, 3u};
    kernel_call_frame frame;
    CHECK(kernel_frame_build(&frame, scratch_at(OFF_FRAME), 0x100u, args, 5u));
    poison(scratch_at(OFF_FSINFO), 32u);
    CHECK_EQ_U32(kernel_io_query_volume_stored(&frame), STATUS_UNSUCCESSFUL);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO)), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 1u);

    char directory[] = "/tmp/tsfp-t1091-volume-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char filename[256];
    (void)snprintf(filename, sizeof(filename), "%s/probe", directory);
    FILE *file = fopen(filename, "wb");
    CHECK(file != NULL);
    if (file) CHECK(fclose(file) == 0);
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\Partition1", directory));
    const uint32_t actual = open_empty_file("\\Device\\Harddisk0\\Partition1\\probe");
    CHECK(actual != 0u);
    args[0] = actual;
    CHECK(kernel_frame_build(&frame, scratch_at(OFF_FRAME), 0x100u, args, 5u));
    struct statvfs backing;
    CHECK(statvfs(directory, &backing) == 0);
    poison(scratch_at(OFF_FSINFO), 32u);
    CHECK_EQ_U32(kernel_io_query_volume_stored(&frame), STATUS_SUCCESS);
    CHECK_EQ_U64(read64(scratch_at(OFF_FSINFO)),
                 (uint64_t)backing.f_blocks * backing.f_frsize / 16384u);
    CHECK(read64(scratch_at(OFF_FSINFO) + 8u) <= read64(scratch_at(OFF_FSINFO)));
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 16u) *
                 read32(scratch_at(OFF_FSINFO) + 20u), 16384u);
    CHECK_EQ_U32(read32(scratch_at(OFF_FSINFO) + 24u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(scratch_at(OFF_IOSB) + 4u), 24u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 1u);
    kernel_io_set_volume_policy(KERNEL_IO_VOLUME_REFUSE);
    CHECK_EQ_U32(call_ordinal(218u, args, 5u), STATUS_SUCCESS);
    kernel_file_reset();
    CHECK(unlink(filename) == 0);
    CHECK(rmdir(directory) == 0);
}

static unsigned volume_error_calls;
static uint32_t volume_error_status;
static bool volume_error_open;
static bool record_volume_error(void *context, uint32_t status, bool open_phase)
{
    CHECK(context == &volume_error_calls);
    ++volume_error_calls;
    volume_error_status = status;
    volume_error_open = open_phase;
    return true; /* Declared error/TLS boundary, not real guest TLS in this fixture. */
}
static void test_xnet_volume_helper_real_backing(void)
{
    reset_all();
    (void)kernel_object_register();
    const uint32_t path = scratch_at(0x8000u);
    const uint32_t output[3] = {scratch_at(0x9000u), scratch_at(0x9010u), scratch_at(0x9020u)};
    CHECK(kernel_guest_write_bytes(path, "T:", 3u));
    xnet_volume_kernel source = {scratch_at(0x1000u), scratch_at(0x2000u), 32u,
                                record_volume_error, &volume_error_calls};
    volume_error_calls = 0u;
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    uint32_t result = 0xEEEEEEEEu;
    poison(output[0], 48u);
    CHECK(xnet_volume_space_kernel(&source, path, output, &result));
    CHECK_EQ_U32(result, 0u);
    CHECK_EQ_U32(volume_error_calls, 1u);
    CHECK_EQ_U32(volume_error_status, KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(volume_error_open);
    CHECK_EQ_U32(read32(output[0]), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);

    char directory[] = "/tmp/tsfp-t1091-volume-helper-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\Partition1", directory));
    CHECK(kernel_file_add_symlink("\\??\\T:", "\\Device\\Harddisk0\\Partition1"));
    struct statvfs backing;
    CHECK(statvfs(directory, &backing) == 0);
    result = 0xEEEEEEEEu;
    CHECK(xnet_volume_space_kernel(&source, path, output, &result));
    CHECK_EQ_U32(result, 1u);
    CHECK_EQ_U32(volume_error_calls, 1u); /* Successful helper preserves last error. */
    CHECK_EQ_U64(read64(output[1]), (uint64_t)backing.f_blocks * backing.f_frsize / 16384u * 16384u);
    CHECK_EQ_U64(read64(output[0]), read64(output[2]));
    CHECK(read64(output[0]) <= read64(output[1]));
    CHECK_EQ_U32(read32(output[0] + 8u), 0xA5A5A5A5u);
    CHECK_EQ_U32(kernel_file_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_io_fabricated_geometry_count(), 0u);
    kernel_file_reset();
    CHECK(rmdir(directory) == 0);
}

int main(void)
{
    kernel_hle_set_log(capture_printer);
    if (!scratch_init()) {
        printf("FAIL could not allocate scratch guest memory\n");
        return 1;
    }

    test_actual_mounted_volume_and_strict_refusal();
    test_xnet_volume_helper_real_backing();
    test_class3_reports_the_geometry_the_guest_validates();
    test_class3_writes_exactly_24_bytes_in_the_derived_shape();
    test_an_underived_class_is_refused_and_counted();
    test_class1_writes_the_identity_at_the_measured_offsets();
    test_class1_reached_shape_consumes_only_the_serial();
    test_class1_short_buffer_is_refused();
    test_a_buffer_shorter_than_the_structure_is_refused();
    test_a_bad_or_wrong_kind_handle_is_refused();
    test_the_refuse_policy_is_honoured_and_writes_nothing();
    test_the_file_position_round_trips_as_64_bits();
    test_class22_is_56_bytes_with_end_of_file_at_0x28();
    test_an_unhonourable_set_is_refused();
    test_a_write_with_nothing_writable_behind_it_is_refused();
    test_the_write_handler_needs_all_eight_arguments();
    test_flush_buffers_completes_on_a_file_and_refuses_other_handles();
    test_reset_clears_the_record();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
