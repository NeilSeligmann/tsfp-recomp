/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fabricated FILE_OBJECT bodies and ordinal 76 IoQueryVolumeInformation.
 *
 * WHAT IS REPLAYED. The one measured chain, with the guest's own literal shapes:
 * `_XGetFilePhysicalSortKey@4` (0x0037D14F) calls ordinal 246 with a file handle
 * and IoFileObjectType, calls ordinal 76 with the returned pointer and
 * (5, 0x20, buf, &len), checks [buf+8] == 4 and the name dword at [buf+0xC], then
 * DEREFERENCES [p+8] and the pointer found there (GDFX: [[p+8]], FATX:
 * [[p+8]+0x1C] gated on flag bit 0 of byte [[p+8]]), and finally passes p to
 * ordinal 250. Its caller 0x000290F0 computes `offset/2048 + key`. Every test
 * below performs those reads the way the guest performs them, from guest memory,
 * so a body whose double dereference lands anywhere wrong FAILS here rather than
 * in a silent garbage sort key.
 *
 * THE DISC IS SYNTHETIC. The GDFX branch needs a real DISC-backed open with a
 * known start sector, and the user's image is never committed, so this suite
 * writes a minimal valid XDVDFS image to /tmp: the descriptor magic at both
 * measured offsets, one root entry `PAK.BIN` at sector 40. That makes the GDFX
 * replay and both registered mutants deterministic on a clean checkout.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "kernel_file_object.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "nt_status.h"

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
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__,         \
                   #actual, (unsigned)a_, (unsigned)e_);                                \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* --- captured log, so refusals can be asserted by their words ------------- */

static char captured[8192];
static size_t captured_used;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (captured_used < sizeof(captured) - 1u) {
        int written = vsnprintf(captured + captured_used,
                                sizeof(captured) - captured_used, format, args);
        if (written > 0) {
            captured_used += (size_t)written;
            if (captured_used > sizeof(captured) - 1u) {
                captured_used = sizeof(captured) - 1u;
            }
        }
    }
    va_end(args);
    return 0;
}

static void capture_clear(void)
{
    captured_used = 0u;
    captured[0] = '\0';
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* --- scratch guest memory -------------------------------------------------- */

#define SCRATCH_BYTES 0x10000u
#define OFF_FRAME 0x0100u
#define OFF_OUT 0x0400u
#define OFF_TYPE 0x0440u
#define OFF_FSINFO 0x0500u
#define OFF_RETLEN 0x0580u
#define OFF_OPEN 0x0600u
#define OFF_IOSB 0x0A00u

static kernel_guest_ptr scratch;

static kernel_guest_ptr at(uint32_t offset)
{
    return scratch + offset;
}

#define POISON 0xA5A5A5A5u

static void poison(uint32_t offset, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes; i += 4u) {
        CHECK(kernel_guest_write_u32(at(offset + i), POISON));
    }
}

static uint32_t read32(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    if (!kernel_guest_read_u32(address, &value)) {
        printf("FAIL %s: could not read guest %#x\n", __FILE__, (unsigned)address);
        failures++;
    }
    return value;
}

static uint8_t read8(kernel_guest_ptr address)
{
    uint8_t value = 0u;
    if (!kernel_guest_read_u8(address, &value)) {
        printf("FAIL %s: could not read guest byte %#x\n", __FILE__,
               (unsigned)address);
        failures++;
    }
    return value;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(OFF_FRAME), 0x100u, args, count)) {
        printf("FAIL could not build a %u-argument frame\n", count);
        failures++;
        return 0xC0000001u;
    }
    return kernel_hle_call(ordinal, &frame);
}

/* Ordinals 250/251 are fastcall: the object pointer rides in ECX. */
static void call_fastcall(unsigned ordinal, uint32_t object)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, at(OFF_FRAME), 0x100u, NULL, 0u)) {
        printf("FAIL could not build a fastcall frame\n");
        failures++;
        return;
    }
    kernel_frame_set_registers(&frame, object, 0u);
    (void)kernel_hle_call(ordinal, &frame);
}

/* --- the synthetic XDVDFS image -------------------------------------------- */

/* The one file on the synthetic disc. The start sector is the value the GDFX
 * replay must see back out of [[FileObject+8]]. */
#define SYNTH_FILE_SECTOR 40u
#define SYNTH_FILE_BYTES 4096u
#define SYNTH_IMAGE_BYTES (48u * 2048u)

static char disc_image_path[64];

static bool write_all(int fd, uint64_t offset, const void *data, size_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)data;
    size_t done = 0u;
    while (done < bytes) {
        ssize_t got = pwrite(fd, cursor + done, bytes - done, (off_t)(offset + done));
        if (got <= 0) {
            return false;
        }
        done += (size_t)got;
    }
    return true;
}

static bool build_synthetic_disc(void)
{
    strcpy(disc_image_path, "/tmp/tsfp-file-object-disc-XXXXXX");
    int fd = mkstemp(disc_image_path);
    if (fd < 0) {
        return false;
    }
    bool ok = ftruncate(fd, (off_t)SYNTH_IMAGE_BYTES) == 0;

    /* The volume descriptor at sector 32: the 20-byte magic at +0 and +0x7EC,
     * root sector and root size (IN BYTES) at +0x14 and +0x18, exactly the
     * offsets xdvdfs.h documents. */
    static const char magic[20] = "MICROSOFT*XBOX*MEDIA";
    uint8_t descriptor[2048];
    memset(descriptor, 0, sizeof(descriptor));
    memcpy(descriptor, magic, sizeof(magic));
    memcpy(descriptor + 0x7EC, magic, sizeof(magic));
    const uint32_t root_sector = 33u;
    const uint32_t root_size = 24u; /* one 14+7 byte entry, padded to 4 */
    memcpy(descriptor + 0x14, &root_sector, 4u);
    memcpy(descriptor + 0x18, &root_size, 4u);
    ok = ok && write_all(fd, 32u * 2048u, descriptor, sizeof(descriptor));

    /* One root entry: left/right links nil (0), start sector, size, attribute
     * 0x20 (a plain file: the directory bit 0x10 is clear), name "PAK.BIN". */
    uint8_t entry[24];
    memset(entry, 0xFF, sizeof(entry)); /* real images pad directories with 0xFF */
    memset(entry, 0, 4u);
    const uint32_t start = SYNTH_FILE_SECTOR;
    const uint32_t size = SYNTH_FILE_BYTES;
    memcpy(entry + 4u, &start, 4u);
    memcpy(entry + 8u, &size, 4u);
    entry[12] = 0x20u;
    entry[13] = 7u;
    memcpy(entry + 14u, "PAK.BIN", 7u);
    ok = ok && write_all(fd, (uint64_t)root_sector * 2048u, entry, sizeof(entry));

    /* Recognisable content at the file's extent, so a read test elsewhere could
     * tell these bytes from zeros. */
    ok = ok && write_all(fd, (uint64_t)SYNTH_FILE_SECTOR * 2048u, "P5CK", 4u);
    (void)close(fd);
    return ok;
}

/* --- a writable host directory --------------------------------------------- */

static char host_dir_path[64];

static bool build_host_dir(void)
{
    strcpy(host_dir_path, "/tmp/tsfp-file-object-hdd-XXXXXX");
    if (mkdtemp(host_dir_path) == NULL) {
        return false;
    }
    char file_path[96];
    snprintf(file_path, sizeof(file_path), "%s/save.bin", host_dir_path);
    int fd = open(file_path, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        return false;
    }
    const bool ok = write_all(fd, 0u, "savedata", 8u);
    (void)close(fd);
    return ok;
}

static void remove_host_dir(void)
{
    char file_path[96];
    snprintf(file_path, sizeof(file_path), "%s/save.bin", host_dir_path);
    (void)unlink(file_path);
    (void)rmdir(host_dir_path);
}

/* --- setup / teardown ------------------------------------------------------- */

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_object_reset();
    kernel_file_object_reset();
    kernel_file_reset();
    kernel_file_unmount_all();
    /* Exact counts: a dropped binding would make every call below hit a stub
     * returning 0, which reads as STATUS_SUCCESS. */
    CHECK_EQ_U32(kernel_object_register(), 7u);
    CHECK_EQ_U32(kernel_file_register(), 7u);
    CHECK_EQ_U32(kernel_file_object_register(), 1u);
    kernel_hle_set_log(capture_printer);
    capture_clear();

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FATAL: could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    kernel_file_unmount_all();
    kernel_file_reset();
    kernel_object_reset();
    kernel_file_object_reset();
    guest_mem_reset();
    scratch = 0u;
}

/* Drive the real NtOpenFile (202), so the open-file slot is created the way the
 * guest creates it. Access/share/options are the measured 0x37D53E literals. */
static uint32_t open_path_access(const char *path, uint32_t access)
{
    const uint32_t oa = OFF_OPEN;
    const uint32_t name = OFF_OPEN + 0x20u;
    const uint32_t text = OFF_OPEN + 0x40u;
    const uint32_t handle_out = OFF_OPEN + 0x200u;
    const size_t length = strlen(path);
    for (size_t i = 0u; i < length; i++) {
        CHECK(kernel_guest_write_u8(at(text) + (uint32_t)i, (uint8_t)path[i]));
    }
    CHECK(kernel_guest_write_u8(at(name), (uint8_t)(length & 0xFFu)));
    CHECK(kernel_guest_write_u8(at(name) + 1u, (uint8_t)((length >> 8) & 0xFFu)));
    CHECK(kernel_guest_write_u8(at(name) + 2u, (uint8_t)((length + 1u) & 0xFFu)));
    CHECK(kernel_guest_write_u8(at(name) + 3u,
                                (uint8_t)(((length + 1u) >> 8) & 0xFFu)));
    CHECK(kernel_guest_write_u32(at(name) + 4u, at(text)));
    CHECK(kernel_guest_write_u32(at(oa) + 0u, 0u));
    CHECK(kernel_guest_write_u32(at(oa) + 4u, at(name)));
    CHECK(kernel_guest_write_u32(at(oa) + 8u, 0x40u));

    const uint32_t args[6] = {at(handle_out), access, at(oa),
                              at(OFF_IOSB),   3u,        0x800021u};
    const uint32_t status = call_ordinal(202u, args, 6u);
    if (status != STATUS_SUCCESS) {
        printf("FAIL open of \"%s\" returned %#x\n", path, (unsigned)status);
        failures++;
        return 0u;
    }
    return read32(at(handle_out));
}

static uint32_t open_path(const char *path)
{
    return open_path_access(path, 0x100001u);
}


/* Ordinal 246 with the measured three-argument shape. The object-type VALUE the
 * guest pushes is a data export's content; any nonzero stands in for it, since
 * the type is recorded and not enforced. */
static uint32_t reference(uint32_t handle)
{
    CHECK(kernel_guest_write_u32(at(OFF_OUT), POISON));
    const uint32_t args[3] = {handle, 0x8003F000u, at(OFF_OUT)};
    return call_ordinal(246u, args, 3u);
}

/* Ordinal 76 with the measured five-argument shape. */
static uint32_t query_volume(uint32_t file_object, uint32_t fs_class, uint32_t length)
{
    const uint32_t args[5] = {file_object, fs_class, length, at(OFF_FSINFO),
                              at(OFF_RETLEN)};
    return call_ordinal(76u, args, 5u);
}

static uint32_t references_of(uint32_t handle)
{
    kernel_object_entry copy;
    memset(&copy, 0, sizeof(copy));
    return kernel_object_get_copy(handle, &copy) ? copy.references : 0xFFFFFFFFu;
}

/* ========================================================================= */

/*
 * The GDFX chain, end to end, on the synthetic disc.
 *
 * MUTATION (registered as file-object-gdfx-start-sector-at-the-wrong-offset):
 * write the start sector at extension +4 instead of +0 and the [[p+8]] read
 * below sees 0 instead of 40. MUTATION (file-object-disc-answers-the-wrong-name):
 * answer FATX for a DISC backing and the name check and the key both fail here.
 */
static void test_gdfx_replay_double_dereference_and_consumer_math(void)
{
    setup();
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_image_path));
    const uint32_t handle = open_path("\\Device\\CdRom0\\PAK.BIN");
    CHECK(handle != 0u);
    CHECK_EQ_U32(references_of(handle), 1u);

    /* 246 publishes a BODY, not the handle value, and it is real guest memory. */
    CHECK_EQ_U32(reference(handle), STATUS_SUCCESS);
    const uint32_t body = read32(at(OFF_OUT));
    CHECK(body != handle);
    CHECK(body != POISON);
    CHECK(body != 0u);
    CHECK_EQ_U32(references_of(handle), 2u);
    CHECK_EQ_U32(kernel_file_object_body_count(), 1u);

    /* Ordinal 76, class 5, the measured Length 0x20. Only +8 and +0xC written. */
    poison(OFF_FSINFO, 0x20u);
    CHECK(kernel_guest_write_u32(at(OFF_RETLEN), POISON));
    CHECK_EQ_U32(query_volume(body, 5u, 0x20u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 8u), 4u);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 0xCu), KERNEL_FILE_OBJECT_FS_NAME_GDFX);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 0u), POISON);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 4u), POISON);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 0x10u), POISON);
    CHECK_EQ_U32(read32(at(OFF_RETLEN)), 0x10u);

    /* The guest's own double dereference: key = [[p+8]], the file's start
     * sector. The image is a standalone game partition, so its base is 0 and
     * the sector is image-relative, exactly what the backing reads with. */
    const uint32_t extension = read32(body + 8u);
    CHECK(extension != 0u);
    const uint32_t key = read32(extension);
    CHECK_EQ_U32(key, SYNTH_FILE_SECTOR);

    /* The consumer 0x000290F0: offset/2048 + key, signed divide. */
    const int32_t offset = 5000;
    const int32_t lba = ((offset + ((offset >> 31) & 0x7FF)) >> 11) + (int32_t)key;
    CHECK_EQ_U32((uint32_t)lba, SYNTH_FILE_SECTOR + 2u);

    /* The wrapper's unconditional ObfDereferenceObject, then the close. The body
     * is freed WITH the handle, not before. */
    call_fastcall(250u, body);
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK_EQ_U32(kernel_file_object_body_count(), 1u);
    const uint32_t close_args[1] = {handle};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    CHECK(kernel_object_find(handle) == NULL);
    CHECK_EQ_U32(kernel_file_object_body_count(), 0u);
    teardown();
}

/*
 * The FATX chain on a real host directory. The flag byte is CLEAR (the guest
 * proceeds to its +0x1C read) and the value there is 0: a host directory has no
 * cluster map, so 0 is the only non-invented placement, and it is also what the
 * guest's own flagged arm computes.
 *
 * MUTATION: writing the FATX value anywhere but +0x1C, or setting flag bit 0,
 * is NOT observable through the value (both read 0) -- which is exactly why the
 * GDFX test above pins the offsets with a NONZERO sector. This test pins the
 * name dword and the gate byte instead.
 */
static void test_fatx_replay_flag_clear_and_cached_body(void)
{
    setup();
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1",
                                     host_dir_path));
    const uint32_t handle = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK(handle != 0u);

    CHECK_EQ_U32(reference(handle), STATUS_SUCCESS);
    const uint32_t body = read32(at(OFF_OUT));
    CHECK(body != handle);
    CHECK(body != 0u);

    poison(OFF_FSINFO, 0x20u);
    CHECK_EQ_U32(query_volume(body, 5u, 0x20u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 8u), 4u);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 0xCu), KERNEL_FILE_OBJECT_FS_NAME_FATX);

    const uint32_t extension = read32(body + 8u);
    CHECK(extension != 0u);
    /* The guest's gate: byte [[p+8]] bit 0 must be CLEAR, then [[p+8]+0x1C]. */
    CHECK_EQ_U32(read8(extension) & 1u, 0u);
    CHECK_EQ_U32(read32(extension + KERNEL_FILE_OBJECT_FATX_VALUE_OFFSET), 0u);

    /* A second 246 on the same handle reuses the SAME body: one allocation per
     * open, however many times the guest converts the handle. */
    CHECK_EQ_U32(reference(handle), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_OUT)), body);
    CHECK_EQ_U32(references_of(handle), 3u);
    CHECK_EQ_U32(kernel_file_object_body_count(), 1u);

    call_fastcall(250u, body);
    call_fastcall(250u, body);
    CHECK_EQ_U32(references_of(handle), 1u);
    const uint32_t close_args[1] = {handle};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_file_object_body_count(), 0u);
    teardown();
}

/*
 * NtClose BEFORE the balancing dereference: the wrapper's final 250 is then the
 * last reference and must destroy the entry and free the body, because nothing
 * else ever will.
 */
static void test_close_then_dereference_frees_the_body(void)
{
    setup();
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1",
                                     host_dir_path));
    const uint32_t handle = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK(handle != 0u);
    CHECK_EQ_U32(reference(handle), STATUS_SUCCESS);
    const uint32_t body = read32(at(OFF_OUT));

    const uint32_t close_args[1] = {handle};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    /* Still live: the body reference holds it, and 76 still answers. */
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK_EQ_U32(query_volume(body, 5u, 0x20u), STATUS_SUCCESS);

    call_fastcall(250u, body);
    CHECK(kernel_object_find(handle) == NULL);
    CHECK_EQ_U32(kernel_file_object_body_count(), 0u);

    /* An EXTRA dereference on the dead body is refused and reported, and an
     * extra one on a LIVE body with no outstanding reference likewise. */
    capture_clear();
    call_fastcall(250u, body);
    const uint32_t again = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK(again != 0u);
    CHECK_EQ_U32(reference(again), STATUS_SUCCESS);
    const uint32_t body2 = read32(at(OFF_OUT));
    call_fastcall(250u, body2);
    capture_clear();
    call_fastcall(250u, body2);
    CHECK(captured_contains("no outstanding reference"));
    CHECK_EQ_U32(references_of(again), 1u);
    /* And 251 takes one back, for the guests that re-reference by pointer. */
    call_fastcall(251u, body2);
    CHECK_EQ_U32(references_of(again), 2u);
    call_fastcall(250u, body2);
    teardown();
}

/*
 * Everything that is not the measured question is REFUSED, each with its own
 * status, and nothing is written on any refusal path.
 */
static void test_refusals_other_classes_unknown_bodies_short_lengths(void)
{
    setup();
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1",
                                     host_dir_path));
    const uint32_t handle = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK_EQ_U32(reference(handle), STATUS_SUCCESS);
    const uint32_t body = read32(at(OFF_OUT));

    poison(OFF_FSINFO, 0x20u);
    /* Classes other than 5: the title's own wrapper raises 0xC000000D when the
     * ANSWER is unrecognised, but an unimplemented class is refused with the
     * same INVALID_INFO_CLASS kernel_io uses, and loudly. */
    CHECK_EQ_U32(query_volume(body, 1u, 0x20u),
                 KERNEL_FILE_OBJECT_STATUS_INVALID_INFO_CLASS);
    CHECK_EQ_U32(query_volume(body, 0u, 0x20u),
                 KERNEL_FILE_OBJECT_STATUS_INVALID_INFO_CLASS);
    /* A FileObject that is not a fabricated body: the raw handle value (what the
     * legacy surrogate would have produced), and a stray pointer. */
    CHECK_EQ_U32(query_volume(handle, 5u, 0x20u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(query_volume(at(OFF_OPEN), 5u, 0x20u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(query_volume(0u, 5u, 0x20u), STATUS_INVALID_PARAMETER);
    /* A Length below what the 4-byte name needs. */
    CHECK_EQ_U32(query_volume(body, 5u, 0xCu),
                 KERNEL_FILE_OBJECT_STATUS_INFO_LENGTH_MISMATCH);
    /* A NULL FsInformation buffer. */
    const uint32_t null_buffer_args[5] = {body, 5u, 0x20u, 0u, at(OFF_RETLEN)};
    CHECK_EQ_U32(call_ordinal(76u, null_buffer_args, 5u), STATUS_INVALID_PARAMETER);
    /* Nothing was written by any refusal. */
    for (uint32_t i = 0u; i < 0x20u; i += 4u) {
        CHECK_EQ_U32(read32(at(OFF_FSINFO) + i), POISON);
    }
    CHECK(kernel_file_object_query_refused_count() >= 7u);
    CHECK_EQ_U32(kernel_file_object_query_answered_count(), 0u);

    call_fastcall(250u, body);
    const uint32_t close_args[1] = {handle};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    teardown();
}

/*
 * Handles that have no filesystem identity keep the LEGACY surrogate: a FILE
 * handle another subsystem issued directly (no kernel_file slot) and an
 * EMPTY-backed fabricated file. Ordinal 76 then refuses their surrogate value,
 * which the wrapper turns into a clean -1 rather than a garbage key.
 */
static void test_handles_without_identity_keep_the_legacy_surrogate(void)
{
    setup();
    /* Directly issued FILE handle: no open-file slot behind it. */
    const uint32_t bare = kernel_object_create(KERNEL_OBJECT_FILE, 0x7777u);
    CHECK(bare != 0u);
    CHECK_EQ_U32(reference(bare), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_OUT)), bare);
    CHECK_EQ_U32(kernel_file_object_body_count(), 0u);
    CHECK_EQ_U32(query_volume(bare, 5u, 0x20u), STATUS_INVALID_PARAMETER);
    call_fastcall(250u, bare);
    CHECK(kernel_object_release(bare));

    /* EMPTY-backed fabricated file: openable, but nothing is behind it. */
    CHECK(kernel_file_add_openable("\\Device\\CdRom0\\ghost.bin"));
    const uint32_t ghost = open_path("\\Device\\CdRom0\\ghost.bin");
    CHECK(ghost != 0u);
    capture_clear();
    CHECK_EQ_U32(reference(ghost), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_OUT)), ghost);
    CHECK(captured_contains("no filesystem identity"));
    CHECK_EQ_U32(kernel_file_object_body_count(), 0u);
    call_fastcall(250u, ghost);
    const uint32_t close_args[1] = {ghost};
    CHECK_EQ_U32(call_ordinal(187u, close_args, 1u), STATUS_SUCCESS);
    teardown();
}

static void test_real_allocation_preserves_eof_and_data(void)
{
    setup();
    CHECK(kernel_io_register() > 0u);
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1", host_dir_path));
    const uint32_t handle = open_path_access("\\Device\\Harddisk0\\partition1\\save.bin", 0x100003u);
    CHECK(handle != 0u);
    CHECK(kernel_file_open_set_offset(handle, 7u));
    uint32_t status = 0u;
    CHECK(kernel_file_set_allocation(handle, 65536u, &status));
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    char path[96];
    snprintf(path, sizeof(path), "%s/save.bin", host_dir_path);
    struct stat metadata;
    CHECK(stat(path, &metadata) == 0);
    CHECK_EQ_U32(metadata.st_size, 8u);
    CHECK(metadata.st_blocks >= 128);
    kernel_file_open info;
    CHECK(kernel_file_open_info(handle, &info));
    CHECK_EQ_U32(info.offset, 7u);
    CHECK_EQ_U32(info.size, 8u);
    CHECK(kernel_file_set_allocation(handle, 0u, &status));
    CHECK(kernel_file_set_allocation(handle, 2u, &status));
    CHECK(stat(path, &metadata) == 0);
    CHECK_EQ_U32(metadata.st_size, 8u);
    CHECK(metadata.st_blocks < 128);
    char data[8];
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, data, sizeof(data)) == 8);
    CHECK(memcmp(data, "savedata", 8u) == 0);
    (void)close(fd);
    CHECK(!kernel_file_set_allocation(handle, UINT64_MAX, &status));
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);
    CHECK(!kernel_file_set_allocation(handle, INT64_MAX, &status));
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);
    /* Literal ordinal ABI: eight-byte LARGE_INTEGER, class19, IOSB success0. */
    CHECK(kernel_guest_write_u32(at(OFF_FSINFO), 8040u));
    CHECK(kernel_guest_write_u32(at(OFF_FSINFO) + 4u, 0u));
    const uint32_t allocation_args[5] = {handle, at(OFF_IOSB), at(OFF_FSINFO), 8u, 19u};
    CHECK_EQ_U32(call_ordinal(226u, allocation_args, 5u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0u);
    const uint32_t short_args[5] = {handle, at(OFF_IOSB), at(OFF_FSINFO), 7u, 19u};
    CHECK_EQ_U32(call_ordinal(226u, short_args, 5u), KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH);
    CHECK(kernel_guest_write_u32(at(OFF_FSINFO) + 4u, 0xffffffffu));
    CHECK(kernel_guest_write_u32(at(OFF_IOSB), 0xa5a5a5a5u));
    CHECK(kernel_guest_write_u32(at(OFF_IOSB) + 4u, 0x11223344u));
    CHECK_EQ_U32(call_ordinal(226u, allocation_args, 5u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), 0xa5a5a5a5u);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0x11223344u);
    const uint32_t query_args[5] = {handle, at(OFF_IOSB), at(OFF_FSINFO), 56u, 34u};
    CHECK_EQ_U32(call_ordinal(211u, query_args, 5u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 32u), 8u);
    CHECK_EQ_U32(read32(at(OFF_FSINFO) + 40u), 8u);
    const uint32_t readonly = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK(!kernel_file_set_allocation(readonly, 65536u, &status));
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_ACCESS_DENIED);
    const uint32_t close1[1] = {handle}, close2[1] = {readonly};
    CHECK_EQ_U32(call_ordinal(187u, close1, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(call_ordinal(187u, close2, 1u), STATUS_SUCCESS);
    const uint32_t reopened = open_path("\\Device\\Harddisk0\\partition1\\save.bin");
    CHECK(kernel_file_open_info(reopened, &info));
    CHECK_EQ_U32(info.size, 8u);
    const uint32_t close3[1] = {reopened};
    CHECK_EQ_U32(call_ordinal(187u, close3, 1u), STATUS_SUCCESS);
    CHECK(!kernel_file_set_allocation(0xdeadbeefu, 8u, &status));
    CHECK_EQ_U32(status, STATUS_INVALID_HANDLE);
    CHECK(kernel_file_mount_disc("\\Device\\CdRom0", disc_image_path));
    const uint32_t disc = open_path("\\Device\\CdRom0\\PAK.BIN");
    CHECK(!kernel_file_set_allocation(disc, 8u, &status));
    CHECK_EQ_U32(status, KERNEL_FILE_STATUS_ACCESS_DENIED);
    const uint32_t close_disc[1] = {disc};
    CHECK_EQ_U32(call_ordinal(187u, close_disc, 1u), STATUS_SUCCESS);
    teardown();
}

int main(void)
{
    if (!build_synthetic_disc() || !build_host_dir()) {
        printf("FATAL: could not build the /tmp fixtures\n");
        return EXIT_FAILURE;
    }

    test_real_allocation_preserves_eof_and_data();
    test_gdfx_replay_double_dereference_and_consumer_math();
    test_fatx_replay_flag_clear_and_cached_body();
    test_close_then_dereference_frees_the_body();
    test_refusals_other_classes_unknown_bodies_short_lengths();
    test_handles_without_identity_keep_the_legacy_surrogate();

    (void)unlink(disc_image_path);
    remove_host_dir();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
