/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_io.h for every ordinal number (resolved, not recalled), the
 * hand-verified arities with their call-site tables, what Length = 0x18 does and does
 * not establish about the class-3 structure, and -- most importantly -- the fact that
 * the guest VALIDATES the geometry this module reports and fails the run with
 * 0xC000014F if it is wrong.
 */

#include "kernel_io.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "guest_structs.h"
#include "kernel_async_io.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"
/* For XDVDFS_SECTOR_SIZE only: the allocation size a disc-backed file reports is its
 * end-of-file rounded up to the sector it actually occupies, and the staging buffer is
 * sized in sectors. This module reads no filesystem itself. */
#include "xdvdfs.h"

#define ORD_NtQueryVolumeInformationFile 218u
#define ORD_NtQueryInformationFile 211u
#define ORD_NtSetInformationFile 226u
#define ORD_NtReadFile 219u
#define ORD_NtWriteFile 236u
#define ORD_NtDeviceIoControlFile 196u
#define ORD_NtFsControlFile 200u
#define ORD_NtFlushBuffersFile 198u
#define ORD_NtQueryDirectoryFile 207u
#define ORD_NtQueryFullAttributesFile 210u

/*
 * Offsets within the 56-byte class-0x22 structure.
 *
 * END_OF_FILE at +0x28 is MEASURED: the guest reads both halves of a 64-bit quantity
 * there and tests them against zero (0x00380E11 and 0x00380E16). The others are
 * INFERRED from the 56-byte total, which is the only partition consistent with a
 * 64-bit field landing on +0x28. See kernel_io.h.
 */
#define NETWORK_OPEN_CREATION_TIME_OFFSET 0x00u
#define NETWORK_OPEN_LAST_ACCESS_TIME_OFFSET 0x08u
#define NETWORK_OPEN_LAST_WRITE_TIME_OFFSET 0x10u
#define NETWORK_OPEN_CHANGE_TIME_OFFSET 0x18u
#define NETWORK_OPEN_ALLOCATION_SIZE_OFFSET 0x20u
#define NETWORK_OPEN_END_OF_FILE_OFFSET 0x28u
#define NETWORK_OPEN_ATTRIBUTES_OFFSET 0x30u
/* Alignment padding after the 32-bit attributes, bringing the structure to the 56 bytes
 * the guest declares. Written as zero rather than skipped: see the note at the write. */
#define NETWORK_OPEN_TAIL_OFFSET 0x34u

/* Attribute values the disc itself uses. MEASURED across the whole image: every one of
 * the 491 directory entries carries either 0x10 (79 directories) or 0x20 (412 files),
 * and no other value occurs. So these are not conventional stand-ins -- they are what
 * is on the disc. */
#define FILE_ATTRIBUTE_DIRECTORY_BIT 0x10u
#define FILE_ATTRIBUTE_ARCHIVE_BIT 0x20u

/*
 * THE PRODUCT THE GUEST CHECKS, asserted rather than trusted.
 *
 * `sub_00380D0D` compares FsInformation[+0x10] * FsInformation[+0x14] against
 * 16384 (derived in kernel_io.h from this image's XBE InitFlags) and returns
 * 0xC000014F on a mismatch. Someone editing either factor for an unrelated reason
 * would otherwise break the boot path with a failure that names the device rather
 * than the edit.
 */
_Static_assert(KERNEL_IO_HDD_BYTES_PER_UNIT == 16384u,
               "the guest requires SectorsPerAllocationUnit * BytesPerSector == 16384 "
               "(0x4000 << (XBE InitFlags >> 30), and InitFlags is 5 in this image). "
               "sub_00380D0D returns 0xC000014F when the product disagrees.");

/*
 * Offsets within the 24-byte class-3 structure. MEASURED: the guest reads 32-bit
 * quantities at +0x10 and +0x14 and 64-bit pairs based at +0x00 and +0x08. Written as
 * explicit offsets rather than a host struct because the host's padding rules are not
 * the guest's -- the same reason kernel_file.c reads OBJECT_ATTRIBUTES field by field.
 */
#define FS_SIZE_TOTAL_UNITS_OFFSET 0x00u
#define FS_SIZE_AVAILABLE_UNITS_OFFSET 0x08u
#define FS_SIZE_SECTORS_PER_UNIT_OFFSET 0x10u
#define FS_SIZE_BYTES_PER_SECTOR_OFFSET 0x14u

/*
 * The FABRICATED part: how much space we claim a volume has.
 *
 * Deliberately round powers of two rather than numbers contrived to look like a real
 * console's partition table. If these ever show up in a log or a divergence report
 * they should read as "somebody chose these", because somebody did. 262144 units of
 * 16384 bytes is 4 GiB total with 2 GiB free.
 *
 * The reached site (0x00380D0D) does not read either field -- it validates the
 * geometry and discards the rest -- so on today's trace these are inert. The site at
 * 0x0037D570 does read them, and when a run reaches it the numbers are ours.
 */
#define FABRICATED_TOTAL_UNITS 0x00040000u
#define FABRICATED_AVAILABLE_UNITS 0x00020000u

/*
 * Offsets within the class-1 volume-identity structure. MEASURED from its two
 * in-image consumers (serial read at 0x0037D045 and 0x0037D780, label byte length at
 * 0x0037D75C, label bytes copied from +0x11 at 0x0037D765) -- the full derivation,
 * the 0x18 sizeof evidence and which field is whose invention are in kernel_io.h.
 * The label itself has no offset constant here because no label is ever written: a
 * FATX volume has none (the superblock this title itself lays down at
 * 0x00381574..0x00381598 carries no label field), so VolumeLabelLength is 0 and
 * +0x11 onward stays untouched.
 */
#define FS_VOLUME_CREATION_TIME_OFFSET 0x00u
#define FS_VOLUME_SERIAL_OFFSET 0x08u
#define FS_VOLUME_LABEL_LENGTH_OFFSET 0x0Cu
#define FS_VOLUME_FLAG_OFFSET 0x10u

/*
 * THE LOCK. Two guest threads run and either can query a volume. Every counter below
 * is a read-modify-write, and the counters are this module's record of what it
 * fabricated. RECURSIVE for the reason kernel_file.c, kernel_object.c, kernel_pool.c
 * and kernel_config.c all give: the critical sections call kernel_hle_log(), whose
 * sink is caller-supplied and may re-enter.
 */
static pthread_mutex_t io_lock;
static bool io_lock_ready;
static pthread_once_t io_lock_once = PTHREAD_ONCE_INIT;

static void io_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&io_lock, &attr) == 0) {
        io_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&io_lock_once, io_lock_init);
    if (io_lock_ready) {
        (void)pthread_mutex_lock(&io_lock);
    }
}

static void unlock(void)
{
    if (io_lock_ready) {
        (void)pthread_mutex_unlock(&io_lock);
    }
}

static kernel_io_volume_policy volume_policy = KERNEL_IO_VOLUME_FABRICATE;
static unsigned fabricated_geometry_count;
static unsigned fabricated_identity_count;
static unsigned volume_refused_count;
static unsigned flush_count;
static unsigned directory_entry_count;
static unsigned unknown_class_count;
static unsigned unknown_file_class_count;
static unsigned fabricated_timestamp_count;
static unsigned apc_ignored_count;
static unsigned read_count;
static uint64_t bytes_read;
static unsigned write_count;
static uint64_t bytes_written;
static unsigned write_refused_count;
static unsigned control_count;
static unsigned control_refused_count;

/*
 * CLEARS EVERY COUNTER, which it did not before.
 *
 * A DEFECT FOUND WHILE ADDING THE WRITE PATH, recorded because the test that should have
 * caught it did not. `kernel_io_reset` cleared three counters out of eight and its own
 * comment promised all of them; `test_reset_clears_the_record` asserted
 * `kernel_io_read_count() == 0` afterwards and passed anyway, because the one test that
 * calls reset never performs a read. A reset that silently keeps counters is how one
 * test case's reads get attributed to the next.
 */
void kernel_io_reset(void)
{
    kernel_async_io_reset();
    lock();
    volume_policy = KERNEL_IO_VOLUME_FABRICATE;
    fabricated_geometry_count = 0u;
    fabricated_identity_count = 0u;
    volume_refused_count = 0u;
    flush_count = 0u;
    directory_entry_count = 0u;
    unknown_class_count = 0u;
    unknown_file_class_count = 0u;
    fabricated_timestamp_count = 0u;
    apc_ignored_count = 0u;
    read_count = 0u;
    bytes_read = 0u;
    write_count = 0u;
    bytes_written = 0u;
    write_refused_count = 0u;
    control_count = 0u;
    control_refused_count = 0u;
    unlock();
}

void kernel_io_set_volume_policy(kernel_io_volume_policy policy)
{
    lock();
    volume_policy = policy;
    unlock();
}

unsigned kernel_io_fabricated_geometry_count(void)
{
    lock();
    const unsigned count = fabricated_geometry_count;
    unlock();
    return count;
}

unsigned kernel_io_fabricated_identity_count(void)
{
    lock();
    const unsigned count = fabricated_identity_count;
    unlock();
    return count;
}

unsigned kernel_io_flush_count(void)
{
    lock();
    const unsigned count = flush_count;
    unlock();
    return count;
}

unsigned kernel_io_volume_refused_count(void)
{
    lock();
    const unsigned count = volume_refused_count;
    unlock();
    return count;
}

unsigned kernel_io_unknown_class_count(void)
{
    lock();
    const unsigned count = unknown_class_count;
    unlock();
    return count;
}

uint32_t kernel_io_hdd_bytes_per_unit(void)
{
    return (uint32_t)KERNEL_IO_HDD_BYTES_PER_UNIT;
}

/*
 * Write both IO_STATUS_BLOCK fields, at the offsets guest_structs.h derives (8 bytes
 * total: status at +0x00, information at +0x04). Identical in shape to
 * kernel_file.c's, and deliberately NOT shared with it: that one's null-pointer
 * diagnostic names NtOpenFile, and a shared helper would either print the wrong
 * ordinal or need a name parameter threaded through every call for one log line.
 */
static bool write_io_status(kernel_guest_ptr address, uint32_t status,
                           uint32_t information, const char *who)
{
    if (address == 0u) {
        /* All 5 measured sites pass a real stack local, so a null here is not a shape
         * the guest uses. Tolerated rather than refused, because the status also comes
         * back in eax and every measured site branches on that -- but REPORTED,
         * because it means our argument order may be wrong. */
        kernel_hle_log()("kernel: %s called with a null IoStatusBlock, which no "
                         "measured call site does\n",
                         who);
        return true;
    }
    return kernel_guest_write_u32(address + (uint32_t)offsetof(guest_io_status_block,
                                                              status),
                                 status) &&
           kernel_guest_write_u32(address + (uint32_t)offsetof(guest_io_status_block,
                                                              information),
                                 information);
}

/* Write one little-endian 64-bit value as the low/high halves the guest reads. Split
 * into two 32-bit writes through the guest accessor so an unmapped address is a
 * refusal rather than a host fault. */
static bool write_u64(kernel_guest_ptr address, uint64_t value)
{
    return kernel_guest_write_u32(address, (uint32_t)(value & 0xFFFFFFFFu)) &&
           kernel_guest_write_u32(address + 4u, (uint32_t)(value >> 32));
}

/*
 * Class 1: the volume-identity structure. Layout and consumers are derived in
 * kernel_io.h; the short version is that of the five fields only the serial at +0x08
 * is read by the reached requests, the label length at +0x0C and label bytes at +0x11
 * are read by the same wrapper when a caller asks for the name (none does), and
 * nothing reads the creation time or the +0x10 flag.
 *
 * THE SERIAL IS FABRICATED AND SAYS SO. The original is the per-console FATX
 * superblock dword written from KeQuerySystemTime at format time (this image's own
 * cache-format code writes exactly that superblock), so no original value exists to
 * recover from the disc, and the host directory behind `--hdd` has no superblock.
 * Every measured consumer only stores the value, so a fixed constant consistent
 * across the run satisfies the contract; it is announced and counted on every answer.
 *
 * THE EMPTY LABEL IS ORIGINAL, NOT A DODGE: FATX has no volume label (the superblock
 * the title itself lays down has no label field), so VolumeLabelLength is 0 and no
 * label byte is written -- +0x11 onward stays untouched. That also makes
 * STATUS_BUFFER_OVERFLOW unreachable here: with Length >= 0x18 enforced and zero
 * label bytes, the fixed part always fits. Deliberately no dead overflow arm; the
 * header records that the reached caller's `jl` would treat 0x80000005 as failure,
 * for whichever later task gives a volume a label.
 */
static uint32_t answer_volume_identity(uint32_t file_handle, kernel_guest_ptr io_status,
                                       kernel_guest_ptr fs_information, uint32_t length)
{
    if (length < KERNEL_IO_FS_VOLUME_MIN_BYTES) {
        /* Both measured class-1 sites pass at least the structure's sizeof (a literal
         * 0x18 at 0x0037D02E, a computed 0x11C at the reached site). Refused rather
         * than clamped, exactly as class 3 refuses below its own sizeof. */
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile class %u with Length %u, "
                         "but the derived structure's sizeof is %u and both measured "
                         "sites pass at least that -- REFUSED\n",
                         (unsigned)KERNEL_IO_FS_CLASS_VOLUME, (unsigned)length,
                         (unsigned)KERNEL_IO_FS_VOLUME_MIN_BYTES);
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                              "NtQueryVolumeInformationFile");
        return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
    }

    if (fs_information == 0u) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile has no FsInformation "
                         "buffer\n");
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                              "NtQueryVolumeInformationFile");
        return STATUS_INVALID_PARAMETER;
    }

    lock();
    const kernel_io_volume_policy policy = volume_policy;
    if (policy == KERNEL_IO_VOLUME_REFUSE) {
        volume_refused_count++;
        unlock();
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile class %u on handle %#x "
                         "REFUSED -- no volume's identity is known and the policy is "
                         "to say so rather than fabricate it\n",
                         (unsigned)KERNEL_IO_FS_CLASS_VOLUME, (unsigned)file_handle);
        (void)write_io_status(io_status, STATUS_UNSUCCESSFUL, 0u,
                              "NtQueryVolumeInformationFile");
        return STATUS_UNSUCCESSFUL;
    }
    fabricated_identity_count++;
    unlock();

    /* Field by field at the measured offsets, for the reason class 3 gives: a
     * partially-written structure would hand the guest a fresh serial next to a stale
     * label length, so any failed write fails the whole call. */
    if (!write_u64(fs_information + FS_VOLUME_CREATION_TIME_OFFSET, 0u) ||
        !kernel_guest_write_u32(fs_information + FS_VOLUME_SERIAL_OFFSET,
                                (uint32_t)KERNEL_IO_FABRICATED_VOLUME_SERIAL) ||
        !kernel_guest_write_u32(fs_information + FS_VOLUME_LABEL_LENGTH_OFFSET, 0u) ||
        !kernel_guest_write_u8(fs_information + FS_VOLUME_FLAG_OFFSET, 0u)) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile could not write the "
                         "%u-byte class-%u structure at %#x\n",
                         (unsigned)KERNEL_IO_FS_VOLUME_FIXED_BYTES,
                         (unsigned)KERNEL_IO_FS_CLASS_VOLUME, (unsigned)fs_information);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                              "NtQueryVolumeInformationFile");
        return STATUS_INVALID_PARAMETER;
    }

    /* `information` is the byte count transferred: the fixed part through +0x10 plus
     * zero label bytes. */
    if (!write_io_status(io_status, STATUS_SUCCESS, KERNEL_IO_FS_VOLUME_FIXED_BYTES,
                         "NtQueryVolumeInformationFile")) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile could not write the "
                         "IO_STATUS_BLOCK at %#x\n",
                         (unsigned)io_status);
        return STATUS_INVALID_PARAMETER;
    }

    kernel_hle_log()("kernel: NtQueryVolumeInformationFile(handle %#x, class %u) -> "
                     "serial %#x FABRICATED (fixed for every volume of this run -- the "
                     "per-console FATX format-time value does not exist behind this "
                     "host), label empty (REAL: FATX volumes have no label), creation "
                     "time zero\n",
                     (unsigned)file_handle, (unsigned)KERNEL_IO_FS_CLASS_VOLUME,
                     (unsigned)KERNEL_IO_FABRICATED_VOLUME_SERIAL);
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(218): 5 stack arguments, (FileHandle, IoStatusBlock, FsInformation, Length,
 * FsInformationClass). Counted by hand at the reached site 0x00380D58, where the
 * literals `push 3` and `push 0x18` pin both the count and the order, and corroborated
 * by a byte-level sweep of every section for the thunk slot 0x004757C4: 5 direct
 * `call [slot]` sites, zero reached through a register and zero through a `jmp [slot]`
 * import stub, so the measured scanner's known blind spot does not apply here. All 5
 * sites push 5. The one apparent 6th push (0x0037D560) is a callee-saved register
 * save, proved by sub_0037D4F9's matching `pop edi; pop ebx; pop esi` epilogue rather
 * than asserted. Full site table in kernel_io.h.
 */
static uint32_t query_volume_information_file(void *context, bool stored_only)
{
    if (!context) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile called with no argument "
                         "frame -- the call boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[5];
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtQueryVolumeInformationFile could not read "
                             "argument %u from the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t file_handle = args[0];
    const kernel_guest_ptr io_status = args[1];
    const kernel_guest_ptr fs_information = args[2];
    const uint32_t length = args[3];
    const uint32_t fs_class = args[4];

    /* The handle must be one we issued, and must be a FILE. A thread or event handle
     * arriving here would mean our argument order is wrong, and answering it anyway
     * would hide that. */
    const kernel_object_entry *object = kernel_object_find(file_handle);
    if (!object || object->kind != KERNEL_OBJECT_FILE) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile on handle %#x, which is "
                         "%s -- REFUSED\n",
                         (unsigned)file_handle,
                         object ? "not a file handle" : "a handle we never issued");
        (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u,
                             "NtQueryVolumeInformationFile");
        return STATUS_INVALID_HANDLE;
    }

    if (stored_only && fs_class != KERNEL_IO_FS_CLASS_SIZE) {
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u,
                             "NtQueryVolumeInformationFile");
        return KERNEL_IO_STATUS_INVALID_INFO_CLASS;
    }

    if (fs_class == KERNEL_IO_FS_CLASS_VOLUME) {
        return answer_volume_identity(file_handle, io_status, fs_information, length);
    }

    if (fs_class != KERNEL_IO_FS_CLASS_SIZE) {
        /* Class 5 is reached by one other site in this image (0x0037D732). Its
         * structure is NOT fully derived, and a zero-filled attribute mask is a value
         * the guest would act on -- so this refuses and names the class, which is a
         * bug report a later task can act on. */
        lock();
        unknown_class_count++;
        unlock();
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile class %u REFUSED -- only "
                         "class %u (the 24-byte size/geometry structure) and class %u "
                         "(the volume identity) have derived layouts; this class's "
                         "structure is not derived and a zero-filled answer is one the "
                         "title would act on\n",
                         (unsigned)fs_class, (unsigned)KERNEL_IO_FS_CLASS_SIZE,
                         (unsigned)KERNEL_IO_FS_CLASS_VOLUME);
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u,
                              "NtQueryVolumeInformationFile");
        return KERNEL_IO_STATUS_INVALID_INFO_CLASS;
    }

    if (length < KERNEL_IO_FS_SIZE_BYTES) {
        /* Both measured class-3 sites push exactly 0x18, so a smaller Length is not a
         * shape this image uses. Reported rather than clamped: writing a partial
         * structure would leave the guest reading whichever fields happened to fit. */
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile class %u with Length %u, "
                         "but the derived structure is %u bytes and both measured sites "
                         "push exactly that -- REFUSED\n",
                         (unsigned)fs_class, (unsigned)length,
                         (unsigned)KERNEL_IO_FS_SIZE_BYTES);
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                              "NtQueryVolumeInformationFile");
        return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
    }

    if (fs_information == 0u) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile has no FsInformation "
                         "buffer\n");
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                              "NtQueryVolumeInformationFile");
        return STATUS_INVALID_PARAMETER;
    }

    uint64_t total_units, available_units;
    uint32_t sectors_per_unit, bytes_per_sector;
    const bool actual = kernel_file_volume_space(file_handle, &total_units,
        &available_units, &sectors_per_unit, &bytes_per_sector);
    if (!actual) {
        lock();
        const kernel_io_volume_policy policy = volume_policy;
        if (stored_only || policy == KERNEL_IO_VOLUME_REFUSE) {
            volume_refused_count++;
            unlock();
            (void)write_io_status(io_status, STATUS_UNSUCCESSFUL, 0u,
                                  "NtQueryVolumeInformationFile");
            return STATUS_UNSUCCESSFUL;
        }
        fabricated_geometry_count++;
        unlock();
        total_units = FABRICATED_TOTAL_UNITS;
        available_units = FABRICATED_AVAILABLE_UNITS;
        sectors_per_unit = KERNEL_IO_HDD_SECTORS_PER_UNIT;
        bytes_per_sector = KERNEL_IO_HDD_BYTES_PER_SECTOR;
    }
    if (!write_u64(fs_information + FS_SIZE_TOTAL_UNITS_OFFSET, total_units) ||
        !write_u64(fs_information + FS_SIZE_AVAILABLE_UNITS_OFFSET, available_units) ||
        !kernel_guest_write_u32(fs_information + FS_SIZE_SECTORS_PER_UNIT_OFFSET,
                                sectors_per_unit) ||
        !kernel_guest_write_u32(fs_information + FS_SIZE_BYTES_PER_SECTOR_OFFSET,
                                bytes_per_sector)) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                             "NtQueryVolumeInformationFile");
        return STATUS_INVALID_PARAMETER;
    }

    /* `information` is the byte count transferred. guest_structs.h records the field
     * as a byte count at +0x04, and we wrote the whole structure. */
    if (!write_io_status(io_status, STATUS_SUCCESS, KERNEL_IO_FS_SIZE_BYTES,
                         "NtQueryVolumeInformationFile")) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile could not write the "
                         "IO_STATUS_BLOCK at %#x\n",
                         (unsigned)io_status);
        return STATUS_INVALID_PARAMETER;
    }

    if (actual) {
        kernel_hle_log()("kernel: NtQueryVolumeInformationFile(%#x) actual mounted "
                         "backing capacity, %llu total/%llu available units\n",
                         file_handle, (unsigned long long)total_units,
                         (unsigned long long)available_units);
        return STATUS_SUCCESS;
    }
    kernel_hle_log()("kernel: NtQueryVolumeInformationFile(handle %#x, class %u) -> "
                     "%u bytes/unit (%u sectors x %u bytes, REAL geometry -- the title "
                     "checks this product), %u total and %u free units FABRICATED "
                     "(%u MiB of %u MiB -- there is no hard disk behind this host)\n",
                     (unsigned)file_handle, (unsigned)fs_class,
                     (unsigned)KERNEL_IO_HDD_BYTES_PER_UNIT,
                     (unsigned)KERNEL_IO_HDD_SECTORS_PER_UNIT,
                     (unsigned)KERNEL_IO_HDD_BYTES_PER_SECTOR,
                     (unsigned)FABRICATED_TOTAL_UNITS, (unsigned)FABRICATED_AVAILABLE_UNITS,
                     (unsigned)((uint64_t)FABRICATED_AVAILABLE_UNITS *
                                KERNEL_IO_HDD_BYTES_PER_UNIT / (1024u * 1024u)),
                     (unsigned)((uint64_t)FABRICATED_TOTAL_UNITS *
                                KERNEL_IO_HDD_BYTES_PER_UNIT / (1024u * 1024u)));
    return STATUS_SUCCESS;
}

/*
 * The 0x38-byte FILE_NETWORK_OPEN_INFORMATION, written as ONE range so a buffer that runs
 * off the mapped region receives none of it rather than its first fields. Shared by
 * NtQueryInformationFile class 0x22 (211) and NtQueryFullAttributesFile (210), which are the
 * same structure reached two ways. The 4 tail bytes are written too: the guest declares a
 * 56-byte buffer, 4 of which are alignment padding after the 32-bit attributes, and leaving
 * them alone would hand the title whatever was on its own stack, which is not even
 * deterministic between runs. The real kernel fills the structure it was handed; so does
 * this.
 */
typedef struct {
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint64_t allocation_size;
    uint64_t end_of_file;
    uint32_t attributes;
} network_open_information;

static void store_u64_le(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0u; i < 8u; i++) {
        out[i] = (uint8_t)(value >> (8u * i));
    }
}

static bool write_network_open_information(kernel_guest_ptr address,
                                           const network_open_information *information)
{
    uint8_t bytes[KERNEL_IO_FILE_NETWORK_OPEN_BYTES];
    memset(bytes, 0, sizeof(bytes));
    store_u64_le(&bytes[NETWORK_OPEN_CREATION_TIME_OFFSET], information->creation_time);
    store_u64_le(&bytes[NETWORK_OPEN_LAST_ACCESS_TIME_OFFSET], information->last_access_time);
    store_u64_le(&bytes[NETWORK_OPEN_LAST_WRITE_TIME_OFFSET], information->last_write_time);
    store_u64_le(&bytes[NETWORK_OPEN_CHANGE_TIME_OFFSET], information->change_time);
    store_u64_le(&bytes[NETWORK_OPEN_ALLOCATION_SIZE_OFFSET], information->allocation_size);
    store_u64_le(&bytes[NETWORK_OPEN_END_OF_FILE_OFFSET], information->end_of_file);
    for (unsigned i = 0u; i < 4u; i++) {
        bytes[NETWORK_OPEN_ATTRIBUTES_OFFSET + i] = (uint8_t)(information->attributes >> (8u * i));
    }
    /* The tail at NETWORK_OPEN_TAIL_OFFSET stays zero from the memset above. */
    return kernel_guest_write_bytes(address, bytes, sizeof(bytes));
}

/* XDVDFS allocation rounds EOF to a disc sector. HOST_DIR callers use the
 * unrounded EOF value measured for Xbox HDD NetworkOpenInformation in T951. */
static uint64_t network_open_allocation(uint64_t size)
{
    return (size + XDVDFS_SECTOR_SIZE - 1u) / XDVDFS_SECTOR_SIZE * XDVDFS_SECTOR_SIZE;
}

uint32_t kernel_io_query_volume_stored(void *context)
{
    return query_volume_information_file(context, true);
}

static uint32_t hle_nt_query_volume_information_file(void *context)
{
    return query_volume_information_file(context, false);
}

/* Read the five arguments 211, 226 and 218 all share. False on a stack it cannot
 * read, which the caller turns into a parameter error rather than a zero. */
static bool read_info_args(const kernel_call_frame *frame, const char *who,
                           uint32_t *handle, kernel_guest_ptr *io_status,
                           kernel_guest_ptr *information, uint32_t *length,
                           uint32_t *info_class)
{
    uint32_t args[5];
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest "
                             "stack\n",
                             who, i);
            return false;
        }
    }
    *handle = args[0];
    *io_status = args[1];
    *information = args[2];
    *length = args[3];
    *info_class = args[4];
    return true;
}

/*
 * ARITY-OK(211): 5 stack arguments, (FileHandle, IoStatusBlock, FileInformation, Length,
 * FileInformationClass). Counted at all 9 call sites, agreeing with the measured table.
 * The order is pinned at 0x003801B2, where `Length = 1` is pushed beside a class whose
 * buffer is a single BOOLEAN the guest had just stored -- a one-byte buffer cannot be
 * anything but the information, so Length and FileInformation cannot be transposed.
 * Class/Length pair table in kernel_io.h.
 */
static uint32_t hle_nt_query_information_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtQueryInformationFile called with no argument "
                         "frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t file_handle = 0u;
    kernel_guest_ptr io_status = 0u;
    kernel_guest_ptr information = 0u;
    uint32_t length = 0u;
    uint32_t info_class = 0u;
    if (!read_info_args((const kernel_call_frame *)context, "NtQueryInformationFile",
                        &file_handle, &io_status, &information, &length, &info_class)) {
        return STATUS_INVALID_PARAMETER;
    }

    kernel_file_open file;
    if (!kernel_file_open_info(file_handle, &file)) {
        kernel_hle_log()("kernel: NtQueryInformationFile on handle %#x, which is not an "
                         "open file -- REFUSED\n",
                         (unsigned)file_handle);
        (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u,
                              "NtQueryInformationFile");
        return STATUS_INVALID_HANDLE;
    }
    if (information == 0u) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                              "NtQueryInformationFile");
        return STATUS_INVALID_PARAMETER;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_POSITION) {
        if (length < KERNEL_IO_FILE_POSITION_BYTES) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtQueryInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!write_u64(information, file.offset)) {
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                                  "NtQueryInformationFile");
            return STATUS_INVALID_PARAMETER;
        }
        (void)write_io_status(io_status, STATUS_SUCCESS, KERNEL_IO_FILE_POSITION_BYTES,
                              "NtQueryInformationFile");
        return STATUS_SUCCESS;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_NETWORK_OPEN) {
        if (length < KERNEL_IO_FILE_NETWORK_OPEN_BYTES) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtQueryInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        /*
         * THE SIZE AND THE ATTRIBUTES ARE REAL, THE FOUR TIMESTAMPS ARE NOT.
         *
         * For a disc-backed file the end-of-file is the directory entry's own size and
         * the attribute byte is the one on the disc. XDVDFS carries NO per-file
         * timestamp -- the format has a single creation stamp for the whole volume and
         * nothing per entry -- so there is no honest value to report and the four
         * timestamp fields are written as zero and announced. The allocation size is
         * the end-of-file rounded up to the sector, which is what the file actually
         * occupies on the disc.
         */
        const uint64_t allocation = file.backing == KERNEL_FILE_BACKING_HOST_DIR ? file.size : network_open_allocation(file.size);
        const uint32_t attributes = file.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT
                                                      : FILE_ATTRIBUTE_ARCHIVE_BIT;
        const network_open_information answer = {
            .allocation_size = allocation,
            .end_of_file = file.size,
            .attributes = attributes,
        };
        if (!write_network_open_information(information, &answer)) {
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                                  "NtQueryInformationFile");
            return STATUS_INVALID_PARAMETER;
        }
        lock();
        fabricated_timestamp_count++;
        unlock();
        kernel_hle_log()("kernel: NtQueryInformationFile(\"%s\", class %#x) -> %llu "
                         "bytes, attributes %#x (both REAL, from resolved backing) and four "
                         "FABRICATED zero timestamps (handle-query timestamps unmodeled)\n",
                         file.path, (unsigned)info_class,
                         (unsigned long long)file.size, (unsigned)attributes);
        (void)write_io_status(io_status, STATUS_SUCCESS,
                              KERNEL_IO_FILE_NETWORK_OPEN_BYTES,
                              "NtQueryInformationFile");
        return STATUS_SUCCESS;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_INTERNAL && file.backing == KERNEL_FILE_BACKING_DISC &&
        !file.device) {
        /*
         * FileInternalInformation (class 6, FILE_INTERNAL_INFORMATION { LARGE_INTEGER
         * IndexNumber }, 8 bytes). The class number is the nxdk FILE_INFORMATION_CLASS
         * enum (FileInternalInformation = 6, FilePositionInformation = 14) and the caller
         * is the XAPI GetFileInformationByHandle body at 0x0037D011, which stores the two
         * halves as nFileIndexHigh/Low (measured: class 6, Length 8, buffer EBP-0x10).
         *
         * INFERRED ANSWER. XDVDFS has no inode, but its directory entry's start sector is
         * the one number that uniquely and stably identifies a file on the volume, which
         * is all IndexNumber is for (same file => same index, different files => different).
         * The real kernel's value is not measurable here, so no title may rely on its
         * magnitude. Counted in the fabricated-identity sense by the log line.
         */
        if (length < KERNEL_IO_FILE_INTERNAL_BYTES) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtQueryInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!write_u64(information, (uint64_t)file.disc_sector)) {
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u,
                                  "NtQueryInformationFile");
            return STATUS_INVALID_PARAMETER;
        }
        kernel_hle_log()("kernel: NtQueryInformationFile(\"%s\", class 6 "
                         "FileInternalInformation) -> IndexNumber %u = the file's first "
                         "disc sector (INFERRED identity, XDVDFS has no inode)\n",
                         file.path, (unsigned)file.disc_sector);
        (void)write_io_status(io_status, STATUS_SUCCESS, KERNEL_IO_FILE_INTERNAL_BYTES,
                              "NtQueryInformationFile");
        return STATUS_SUCCESS;
    }

    lock();
    unknown_file_class_count++;
    unlock();
    kernel_hle_log()("kernel: NtQueryInformationFile class %#x (Length %u) on \"%s\" "
                     "REFUSED -- that class's structure is not derived, and a "
                     "zero-filled answer is one the title would act on\n",
                     (unsigned)info_class, (unsigned)length, file.path);
    (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u,
                          "NtQueryInformationFile");
    return KERNEL_IO_STATUS_INVALID_INFO_CLASS;
}

/*
 * ARITY-OK(210): 2 stack arguments, `NtQueryFullAttributesFile(POBJECT_ATTRIBUTES,
 * PFILE_NETWORK_OPEN_INFORMATION)`, STDCALL, NTSTATUS. Measured table {210,2,1,1} (one site,
 * which is every site: the slot is referenced once and no register or stub reaches it),
 * agreeing with nxdk `NtQueryFullAttributesFile@8`. The one site is the XAPI
 * `GetFileAttributesExA` body at 0x00381BD2:
 *
 *     push [ebp+8]; lea eax,[ebp-8]; push eax; call RtlInitAnsiString   ; the name
 *     lea eax,[ebp-0x4c]; push eax          ; arg1 = &info, 0x38 bytes of locals
 *     lea eax,[ebp-0x14]; push eax          ; arg0 = &OBJECT_ATTRIBUTES
 *     mov [ebp-0x14],0xFFFFFFFD             ; root
 *     mov [ebp-0x10],&ansi_string           ; name
 *     mov [ebp-0x0C],0x40                   ; attributes
 *     call dword ptr [0x4758EC]
 *
 * which pins the ORDER (the OBJECT_ATTRIBUTES is the one whose first word is the root
 * sentinel) and the buffer size (info and attributes together are the 0x38 + 0x0C locals).
 * `test eax,eax; jl` is the only use of the status: on failure it hands the value to the
 * error mapper at 0x0037E9FD and returns FALSE. On success the 0x38 bytes are copied field by
 * field into a WIN32_FILE_ATTRIBUTE_DATA, which MEASURES the layout the guest reads:
 * attributes at +0x30, creation +0x00, access +0x08, write +0x10, end of file +0x28 (both
 * halves). It never reads +0x18 (change time) or +0x20 (allocation), which are INFERRED
 * from the NT structure and the existing class-0x22 answer. The only caller of that
 * function is an XONLINE package-size sum (0x0042C789), which uses nFileSizeLow only.
 *
 * WHAT IS REAL AND WHAT IS NOT. Size and the 0x10/0x20 attribute are real (the same rule as
 * class 0x22). A HOST_DIR object's three times are the host's, with creation and change time
 * INFERRED to follow the last write. A disc or empty object has no times and reports four
 * zero FILETIMEs, announced and counted exactly as class 0x22 does. Directory sizes follow
 * the backing. Nothing is written unless the name resolves, as in NT.
 *
 * ERRORS INFERRED from the sibling NtOpenFile handler, which resolves with the same code:
 * a null or unreadable argument is STATUS_INVALID_PARAMETER, a name that does not resolve is
 * that resolution's status (NAME_NOT_FOUND, PATH_NOT_FOUND, ACCESS_DENIED).
 */
static uint32_t hle_nt_query_full_attributes_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t object_attributes = 0u;
    uint32_t information = 0u;
    if (!kernel_frame_arg(frame, 0u, &object_attributes) ||
        !kernel_frame_arg(frame, 1u, &information)) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not read its two arguments "
                         "from the guest stack\n");
        return STATUS_INVALID_PARAMETER;
    }
    kernel_file_attributes found;
    const uint32_t status = kernel_file_query_attributes(object_attributes, &found);
    if (status != STATUS_SUCCESS) {
        return status;
    }

    const uint32_t attributes =
        found.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT : FILE_ATTRIBUTE_ARCHIVE_BIT;
    const network_open_information answer = {
        .creation_time = found.creation_time,
        .last_access_time = found.last_access_time,
        .last_write_time = found.last_write_time,
        /* INFERRED: never read by the one consumer. */
        .change_time = found.last_write_time,
        .allocation_size = found.backing == KERNEL_FILE_BACKING_HOST_DIR ? found.size : network_open_allocation(found.size),
        .end_of_file = found.size,
        .attributes = attributes,
    };
    if (!write_network_open_information(information, &answer)) {
        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not write the %u-byte "
                         "result at %#x\n",
                         (unsigned)KERNEL_IO_FILE_NETWORK_OPEN_BYTES, (unsigned)information);
        return STATUS_INVALID_PARAMETER;
    }
    if (!found.times_known) {
        lock();
        fabricated_timestamp_count++;
        unlock();
    }
    kernel_hle_log()("kernel: NtQueryFullAttributesFile -> %s, %llu bytes, attributes %#x "
                     "(REAL), %s\n",
                     found.is_directory ? "directory" : "file",
                     (unsigned long long)found.size, (unsigned)attributes,
                     found.times_known
                         ? "times from the host (creation and change follow last write, "
                           "INFERRED)"
                         : "four FABRICATED zero timestamps (no per-object time is known)");
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(226): 5 stack arguments, the same five in the same order as 211. Counted at
 * all 14 call sites, agreeing with the measured table; 4 of those sites reach the thunk
 * through `esi` and are invisible to the scanner, which is why they were counted by
 * hand. 0x003801B2 pins the order as described for 211.
 */
static uint32_t hle_nt_set_information_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtSetInformationFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t file_handle = 0u;
    kernel_guest_ptr io_status = 0u;
    kernel_guest_ptr information = 0u;
    uint32_t length = 0u;
    uint32_t info_class = 0u;
    if (!read_info_args((const kernel_call_frame *)context, "NtSetInformationFile",
                        &file_handle, &io_status, &information, &length, &info_class)) {
        return STATUS_INVALID_PARAMETER;
    }

    kernel_file_open file;
    if (!kernel_file_open_info(file_handle, &file)) {
        kernel_hle_log()("kernel: NtSetInformationFile on handle %#x, which is not an "
                         "open file -- REFUSED\n",
                         (unsigned)file_handle);
        (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u,
                              "NtSetInformationFile");
        return STATUS_INVALID_HANDLE;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_POSITION) {
        /* A seek, which costs nothing and mutates no volume. Honoured on every backing for
         * that reason: a position is this module's own bookkeeping. */
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (length < KERNEL_IO_FILE_POSITION_BYTES || information == 0u ||
            !kernel_guest_read_u32(information, &low) ||
            !kernel_guest_read_u32(information + 4u, &high)) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtSetInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        const uint64_t position = (uint64_t)low | ((uint64_t)high << 32);
        if (!kernel_file_open_set_offset(file_handle, position)) {
            (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u,
                                  "NtSetInformationFile");
            return STATUS_INVALID_HANDLE;
        }
        (void)write_io_status(io_status, STATUS_SUCCESS, KERNEL_IO_FILE_POSITION_BYTES,
                              "NtSetInformationFile");
        return STATUS_SUCCESS;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_ALLOCATION) {
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (length < KERNEL_IO_FILE_ALLOCATION_BYTES || information == 0u ||
            information > UINT32_MAX - 7u ||
            !kernel_guest_read_u32(information, &low) ||
            !kernel_guest_read_u32(information + 4u, &high)) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtSetInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        uint32_t status = STATUS_UNSUCCESSFUL;
        const uint64_t allocation = (uint64_t)low | ((uint64_t)high << 32);
        if (!kernel_file_set_allocation(file_handle, allocation, &status)) {
            lock();
            write_refused_count++;
            unlock();
            /* xemu reference immediate negative/readonly refusals leave IOSB
             * untouched; no completed operation is reported on this path. */
            return status;
        }
        /* xemu reference exact profile EOF8040/allocation8040 reports zero. */
        (void)write_io_status(io_status, status, 0u, "NtSetInformationFile");
        return status;
    }

    if (info_class == KERNEL_IO_FILE_CLASS_END_OF_FILE) {
        /*
         * THE SECOND CLASS THIS MODULE ACTS ON, and it only became representable when a
         * writable volume arrived. The refusal that used to stand here said "the only
         * volume behind this host is the user's read-only disc image", which stopped being
         * true the day `--hdd` landed -- a comment that has become a false statement is
         * worse than no comment, because it is the thing a later reader trusts.
         *
         * MEASURED: ordinal 226 is reached ZERO times in the 145-call boot, so nothing
         * here is exercised by starting the title. It is honoured anyway because the
         * alternative leaves a write path that can grow a file and not shrink one, and
         * because `kernel_file_set_end_of_file` is gated by EXACTLY the test a write is --
         * so on a disc, on a fabricated empty file, or on a read-only handle this is a
         * REFUSAL with a status, not a mutation.
         */
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (length < KERNEL_IO_FILE_END_OF_FILE_BYTES || information == 0u ||
            !kernel_guest_read_u32(information, &low) ||
            !kernel_guest_read_u32(information + 4u, &high)) {
            (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u,
                                  "NtSetInformationFile");
            return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
        }
        const uint64_t end_of_file = (uint64_t)low | ((uint64_t)high << 32);
        uint32_t backing_status = STATUS_UNSUCCESSFUL;
        if (!kernel_file_set_end_of_file(file_handle, end_of_file, &backing_status)) {
            lock();
            write_refused_count++;
            unlock();
            (void)write_io_status(io_status, backing_status, 0u, "NtSetInformationFile");
            return backing_status;
        }
        (void)write_io_status(io_status, STATUS_SUCCESS,
                              KERNEL_IO_FILE_END_OF_FILE_BYTES, "NtSetInformationFile");
        return STATUS_SUCCESS;
    }

    lock();
    unknown_file_class_count++;
    unlock();
    kernel_hle_log()("kernel: NtSetInformationFile class %#x (Length %u) on \"%s\" "
                     "REFUSED -- this module derives a structure for the file position "
                     "(%#x), allocation (%#x) and the end of file (%#x) only, and acting on a class whose "
                     "layout is not derived would invent a mutation the title never asked "
                     "for\n",
                     (unsigned)info_class, (unsigned)length, file.path,
                     (unsigned)KERNEL_IO_FILE_CLASS_POSITION,
                     (unsigned)KERNEL_IO_FILE_CLASS_ALLOCATION,
                     (unsigned)KERNEL_IO_FILE_CLASS_END_OF_FILE);
    (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u,
                          "NtSetInformationFile");
    return KERNEL_IO_STATUS_INVALID_INFO_CLASS;
}

/*
 * T743: the asynchronous half of NtReadFile. The bytes are read from the backing NOW (so the
 * file position and every refusal are the synchronous ones), held on the host, and delivered
 * by kernel_async_io when the virtual drive finishes. The guest buffer, the IoStatusBlock and
 * the Event are untouched here: the guest wrapper pre-set IoStatusBlock.Status to
 * STATUS_PENDING (0x0037CC49) and polls it. Returns STATUS_PENDING, a measured immediate EOF
 * (without touching the IoStatusBlock), or the refusal status when nothing could be queued.
 */
static uint32_t queue_async_read(const kernel_file_open *file, uint32_t file_handle,
                                 uint32_t event_handle, kernel_guest_ptr io_status,
                                 kernel_guest_ptr buffer, uint32_t length, uint64_t offset,
                                 bool explicit_offset)
{
    uint8_t *data = malloc(length != 0u ? length : 1u);
    if (data == NULL) {
        (void)write_io_status(io_status, STATUS_NO_MEMORY, 0u, "NtReadFile");
        return STATUS_NO_MEMORY;
    }
    uint32_t got = 0u;
    if (length != 0u && !kernel_file_read_backing(file_handle, offset, data, length, &got)) {
        free(data);
        (void)write_io_status(io_status, STATUS_UNSUCCESSFUL, 0u, "NtReadFile");
        return STATUS_UNSUCCESSFUL;
    }
    if (length != 0u && got == 0u) {
        free(data);
        if (event_handle != 0u) {
            const nt_status event_status = kernel_object_event_clear(event_handle);
            if (event_status != STATUS_SUCCESS) return event_status;
        }
        return STATUS_END_OF_FILE;
    }
    if (got != 0u && kernel_guest_at(buffer, got) == NULL) {
        kernel_hle_log()("kernel: NtReadFile(\"%s\") cannot write %u byte(s) to guest address "
                         "%#x\n",
                         file->path, (unsigned)got, (unsigned)buffer);
        free(data);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, "NtReadFile");
        return STATUS_INVALID_PARAMETER;
    }
    kernel_async_read request = {
        .file_handle = file_handle,
        .event_handle = event_handle,
        .io_status = io_status,
        .buffer = buffer,
        .data = data,
        .length = got,
        .requested = length,
        .volume = kernel_async_io_volume_of_path(file->path),
    };
    if (!kernel_async_io_submit(&request)) {
        kernel_hle_log()("kernel: NtReadFile(\"%s\") asynchronous queue is full (%u requests) or has no "
                         "file object slot, refused with STATUS_INSUFFICIENT_RESOURCES\n",
                         file->path, (unsigned)KERNEL_ASYNC_IO_QUEUE);
        free(data);
        (void)write_io_status(io_status, STATUS_INSUFFICIENT_RESOURCES, 0u, "NtReadFile");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!explicit_offset) {
        (void)kernel_file_open_set_offset(file_handle, offset + got);
    }
    lock();
    read_count++;
    bytes_read += (uint64_t)got;
    unlock();
    return STATUS_PENDING;
}

/*
 * ARITY-OK(219): EIGHT stack arguments, (FileHandle, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, Buffer, Length, ByteOffset). The measured table says SIX and is WRONG;
 * an ABI_TABLE row in src/host/kernel_thunk.c overrides it and explains the scanner's
 * late `_icall_esp` bracket at site 0x003DF10B that produced the 6. All 8 sites push 8.
 * Order pinned at 0x0037D8B6 by a `Length = 0x200` pushed beside a 0x200-byte stack
 * buffer, and by a 64-bit store into the object arg7 points at. See kernel_io.h.
 */
static uint32_t read_file(void *context, bool stored_only)
{
    if (!context) {
        kernel_hle_log()("kernel: NtReadFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[8];
    for (unsigned i = 0u; i < 8u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtReadFile could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t file_handle = args[0];
    const uint32_t event_handle = args[1];
    const uint32_t apc_routine = args[2];
    const kernel_guest_ptr io_status = args[4];
    const kernel_guest_ptr buffer = args[5];
    const uint32_t length = args[6];
    const kernel_guest_ptr byte_offset_ptr = args[7];

    kernel_object_entry file_object;
    if (!kernel_object_get_copy(file_handle, &file_object) ||
        file_object.kind != KERNEL_OBJECT_FILE) {
        kernel_hle_log()("kernel: NtReadFile on handle %#x, which is not a live FILE object "
                         "-- REFUSED\n",
                         (unsigned)file_handle);
        return STATUS_INVALID_HANDLE;
    }

    kernel_file_open file;
    if (!kernel_file_open_info(file_handle, &file)) {
        kernel_hle_log()("kernel: NtReadFile on handle %#x, which is not an open file "
                         "-- REFUSED\n",
                         (unsigned)file_handle);
        return STATUS_INVALID_HANDLE;
    }
    if (event_handle != 0u) {
        kernel_object_entry event;
        if (!kernel_object_get_copy(event_handle, &event)) {
            return STATUS_INVALID_HANDLE;
        }
        if (event.kind != KERNEL_OBJECT_EVENT) {
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
    }
    if (buffer == 0u) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, "NtReadFile");
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * A NULL ByteOffset means "read from the current position and advance it"; a
     * non-null one names an absolute position and, measured at 0x0037D8B6, points at a
     * 64-bit value. Both forms occur in this image, so both are handled: 0x0037D8B6
     * passes a LARGE_INTEGER holding 0x800, and 0x0037F79A and 0x0037F849 pass NULL.
     */
    uint64_t offset = file.offset;
    const bool explicit_offset = byte_offset_ptr != 0u;
    if (explicit_offset) {
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (!kernel_guest_read_u32(byte_offset_ptr, &low) ||
            !kernel_guest_read_u32(byte_offset_ptr + 4u, &high)) {
            kernel_hle_log()("kernel: NtReadFile could not read the 64-bit ByteOffset "
                             "at %#x\n",
                             (unsigned)byte_offset_ptr);
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, "NtReadFile");
            return STATUS_INVALID_PARAMETER;
        }
        offset = (uint64_t)low | ((uint64_t)high << 32);
    }

    if (kernel_async_io_apc_refused(file.open_options, apc_routine)) {
        /* T764: with --async-file-io an ApcRoutine on an asynchronous handle is REFUSED BY NAME,
         * not completed behind the title's back. No retail NtReadFile site passes one (all six
         * push NULL, measured) and there is no ReadFileEx, so delivery at an alertable wait is not
         * something the title needs (kernel_async_io.h, part 3). */
        kernel_hle_log()("kernel: NtReadFile(\"%s\") supplied ApcRoutine %#x on an asynchronous "
                         "handle -- REFUSED with STATUS_NOT_IMPLEMENTED: APC delivery is not "
                         "modelled (kernel_async_io.h part 3), no retail NtReadFile site passes "
                         "one\n",
                         file.path, (unsigned)apc_routine);
        (void)write_io_status(io_status, STATUS_NOT_IMPLEMENTED, 0u, "NtReadFile");
        return STATUS_NOT_IMPLEMENTED;
    }
    if (apc_routine != 0u) {
        /* REPORTED AND NOT CALLED. There is no APC delivery mechanism in this host, and
         * queueing one we cannot deliver would leave the title waiting on a completion
         * that never arrives -- a hang whose cause is nowhere near the symptom. 7 of
         * the 8 measured sites push NULL here, so this is a rare path. */
        lock();
        apc_ignored_count++;
        unlock();
        kernel_hle_log()("kernel: NtReadFile(\"%s\") supplied ApcRoutine %#x, which is "
                         "NOT called -- this host has no APC delivery, and the read "
                         "below completed synchronously instead\n",
                         file.path, (unsigned)apc_routine);
    }
    /* T743/T785: only unbuffered asynchronous reads are queued. A buffered asynchronous handle
     * completes synchronously here; after a successful read, the caller-supplied Event HANDLE
     * is still signalled, as measured on xemu by T763. */
    const bool queued = length != 0u && length <= KERNEL_ASYNC_IO_MAX_BYTES &&
                        kernel_async_io_eligible(file.open_options, event_handle);
    if (kernel_async_io_enabled()) {
        (void)kernel_async_io_service();
    }
    if (queued && !stored_only) {
        return queue_async_read(&file, file_handle, event_handle, io_status, buffer, length,
                                offset, explicit_offset);
    }

    /* A host-side staging buffer, then one copy into guest memory. Reading straight
     * into guest memory would be one copy fewer, but `kernel_guest_at` cannot prove an
     * in-range guest address is mapped -- with the guest space identity-mapped the host
     * MMU is the page table -- so a bogus Buffer would fault inside a `pread` with the
     * lock held. Staging bounds the damage to a reported error. */
    uint8_t staging[XDVDFS_SECTOR_SIZE * 8u];
    uint32_t total = 0u;
    uint32_t status = STATUS_SUCCESS;
    while (total < length) {
        uint32_t want = length - total;
        if (want > (uint32_t)sizeof(staging)) {
            want = (uint32_t)sizeof(staging);
        }
        uint32_t got = 0u;
        if (!(stored_only ? kernel_file_read_backing_stored(file_handle, offset + total, staging, want, &got) :
                            kernel_file_read_backing(file_handle, offset + total, staging, want, &got))) {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (got == 0u) {
            break; /* end of file */
        }
        uint8_t *destination = (uint8_t *)kernel_guest_at(buffer + total, (size_t)got);
        if (!destination) {
            kernel_hle_log()("kernel: NtReadFile(\"%s\") cannot write %u byte(s) to "
                             "guest address %#x\n",
                             file.path, (unsigned)got, (unsigned)(buffer + total));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        memcpy(destination, staging, (size_t)got);
        total += got;
        if (got < want) {
            break; /* short read: end of file */
        }
    }

    if (status != STATUS_SUCCESS) {
        (void)write_io_status(io_status, status, total, "NtReadFile");
        return status;
    }

    if (length != 0u && total == 0u) {
        if (event_handle != 0u) {
            const nt_status event_status = kernel_object_event_clear(event_handle);
            if (event_status != STATUS_SUCCESS) return event_status;
        }
        return STATUS_END_OF_FILE;
    }

    /* Only an implicit read advances the position. An explicit ByteOffset is a
     * positioned read and leaves the handle's own cursor alone, which is what lets the
     * same handle be used for both forms. */
    if (!explicit_offset) {
        (void)kernel_file_open_set_offset(file_handle, offset + total);
    }

    /* T1763: a read through a writable host-directory (hard disk) file is the save-load evidence, so it is logged
     * (the volume is a few files, the disc reads stay silent). tools.save_reload_receipt parses this line. */
    if (file.backing == KERNEL_FILE_BACKING_HOST_DIR) {
        kernel_hle_log()("kernel: NtReadFile(\"%s\") read %u byte(s) at %llu (host directory file)\n", file.path,
                         (unsigned)total, (unsigned long long)offset);
    }

    lock();
    read_count++;
    bytes_read += (uint64_t)total;
    unlock();

    if (total < length) {
        /* A SHORT READ IS REPORTED, not padded. The guest compares
         * IoStatusBlock.information against the Length it asked for (measured at
         * 0x0037F7A4), so it is equipped to notice -- and zero-padding would turn an
         * end-of-file into content. */
        kernel_hle_log()("kernel: NtReadFile(\"%s\") asked %u byte(s) at %llu, "
                         "transferred %u -- short, which is end of a %llu-byte file\n",
                         file.path, (unsigned)length, (unsigned long long)offset,
                         (unsigned)total, (unsigned long long)file.size);
    }
    if (!write_io_status(io_status, STATUS_SUCCESS, total, "NtReadFile")) {
        return STATUS_INVALID_PARAMETER;
    }
    /* T825, measured by T763: publish data and the successful IOSB before signalling the
     * caller's Event HANDLE. Use the handle table, not kernel_event's raw guest-address KEVENT
     * model; this is the same object seam used by queued completion. */
    if (event_handle != 0u) {
        const nt_status event_status = kernel_object_event_set(event_handle, NULL);
        if (event_status != STATUS_SUCCESS) {
            kernel_hle_log()("kernel: NtReadFile(\"%s\") could not set Event %#x after "
                             "synchronous completion (status %#x)\n",
                             file.path, (unsigned)event_handle, (unsigned)event_status);
        }
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_nt_read_file(void *context)
{
    return read_file(context, false);
}
bool kernel_io_read_file_stored(void *context, uint32_t *status)
{
    uint32_t args[8];
    if (!context || !status) return false;
    for (unsigned i = 0u; i < 8u; ++i)
        if (!kernel_frame_arg(context, i, &args[i])) return false;
    kernel_file_open file;
    /* This host boundary refuses absent/fabricated/asynchronous source contracts,
     * not arbitrary guest NTSTATUS. It never queues undeclared completions. */
    if (args[1] || args[2] || args[3] || !kernel_file_open_info(args[0], &file) ||
        file.backing == KERNEL_FILE_BACKING_EMPTY || file.is_directory ||
        !(file.open_options & 0x30u)) return false;
    *status = read_file(context, true);
    return true;
}

/*
 * ARITY-OK(236): EIGHT stack arguments, the same eight in the same order as 219
 * (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset).
 * The measured row `{236u, 6u, 11u, 0}` is non-unanimous and already refused; an ABI_TABLE
 * row in src/host/kernel_thunk.c carries the 8. Five independent lines agree and none
 * dissents -- all 11 sites hand-counted at 8, the site set proved exhaustive by a
 * byte-level sweep of thunk slot 0x004757B8, esp balance uniquely consistent at 8 across
 * arity hypotheses 5..10 in a function with no frame pointer, one function calling 219 and
 * 236 with a byte-for-byte identical argument shape, and two sites inside Win32 `WriteFile`
 * reading every documented OVERLAPPED field at its documented offset. Full argument in
 * kernel_io.h.
 *
 * -------- THE SITE THIS BOOT IS ABOUT TO REACH, READ OFF THE LIFTED CODE --------
 *
 * MEASURED at 0x003810BF, inside `XapiMapLetterToDirectory` (sub_00380E6C), which the boot
 * reaches as call 125. Pushes, in the order they appear:
 *
 *     0x003810AA  push ebx (0)            ; arg7 ByteOffset = NULL -- SURVIVES the call
 *     0x003810AB  push [ebp+0x10]         ; the one argument to sub_0037C9BA
 *     0x003810AE  call 0x37C9BA           ; XGetSectionSize -- `ret 4`, pops only that
 *     0x003810B3  push eax                ; arg6 Length  = the section's VirtualSize
 *                 push esi                ; arg5 Buffer  = the section's loaded base
 *                 push ebp-24             ; arg4 IoStatusBlock
 *                 push ebx; push ebx; push ebx ; arg3 ApcContext, arg2 ApcRoutine, arg1 Event
 *                 push [ebp+0x14]         ; arg0 FileHandle -- the TitleMeta.xbx handle
 *
 * `esi` is the return of `sub_0037C97B([ebp+0x10])`, which calls ordinal 327 XeLoadSection
 * and then returns `MEM32(section + 4)`; `eax` is `sub_0037C9BA([ebp+0x10])`, which returns
 * `MEM32(section + 8)`. Those two offsets are an XBE section header's VirtualAddress and
 * VirtualSize. So the write copies a named XBE section verbatim into the file.
 *
 * TWO CONSEQUENCES THAT DECIDE THIS HANDLER'S SHAPE:
 *
 *   1. ByteOffset IS NULL AT THIS SITE, so the write goes to the handle's CURRENT POSITION
 *      and must advance it. The handle was issued by the NtCreateFile at 0x00381051, so
 *      that position is 0 and a handler that ignored position would land in the right place
 *      ON THIS ONE CALL -- which is exactly why ignoring it is dangerous rather than
 *      harmless. Ordinal 226 NtSetInformationFile, the other way a position is set, is
 *      MEASURED ABSENT from all 145 calls, so the cursor this handler maintains is the only
 *      record of where the next write goes.
 *   2. THE WRITE IS REACHED, as call 125 (return address 0x003810C5), now that ordinal 327
 *      XeLoadSection returns a section base. `sub_0037C9BA` between them has no thunk, so
 *      no kernel call separates the two. MEASURED: the boot makes THREE writes through this
 *      handler (0x003810BF, then 0x00380E3E twice), each copying one XBE section.
 */
static uint32_t hle_nt_write_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtWriteFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[8];
    for (unsigned i = 0u; i < 8u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtWriteFile could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t file_handle = args[0];
    const uint32_t event_handle = args[1];
    const uint32_t apc_routine = args[2];
    const kernel_guest_ptr io_status = args[4];
    const kernel_guest_ptr buffer = args[5];
    const uint32_t length = args[6];
    const kernel_guest_ptr byte_offset_ptr = args[7];

    kernel_file_open file;
    if (!kernel_file_open_info(file_handle, &file)) {
        kernel_hle_log()("kernel: NtWriteFile on handle %#x, which is not an open file "
                         "-- REFUSED\n",
                         (unsigned)file_handle);
        (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u, "NtWriteFile");
        return STATUS_INVALID_HANDLE;
    }
    if (buffer == 0u && length > 0u) {
        kernel_hle_log()("kernel: NtWriteFile(\"%s\") asked for %u byte(s) from a NULL "
                         "Buffer -- REFUSED\n",
                         file.path, (unsigned)length);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, "NtWriteFile");
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * The same two ByteOffset forms NtReadFile handles, and for the same measured reason:
     * a NULL one means "from the current position, and advance it", a non-null one points
     * at a 64-bit absolute position. MEASURED: the site this boot reaches, 0x003810BF,
     * passes NULL; the Win32 `WriteFile` overlapped path at 0x0037CD44 builds a 64-bit
     * pair at [ebp-0x10] and passes its ADDRESS, so both forms exist in this image.
     */
    uint64_t offset = file.offset;
    const bool explicit_offset = byte_offset_ptr != 0u;
    if (explicit_offset) {
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (!kernel_guest_read_u32(byte_offset_ptr, &low) ||
            !kernel_guest_read_u32(byte_offset_ptr + 4u, &high)) {
            kernel_hle_log()("kernel: NtWriteFile could not read the 64-bit ByteOffset "
                             "at %#x\n",
                             (unsigned)byte_offset_ptr);
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, "NtWriteFile");
            return STATUS_INVALID_PARAMETER;
        }
        offset = (uint64_t)low | ((uint64_t)high << 32);
    }

    if (apc_routine != 0u) {
        /* REPORTED AND NOT CALLED, exactly as the read path does: there is no APC delivery
         * in this host, and queueing one that never arrives is a hang whose cause is
         * nowhere near its symptom. */
        lock();
        apc_ignored_count++;
        unlock();
        kernel_hle_log()("kernel: NtWriteFile(\"%s\") supplied ApcRoutine %#x, which is "
                         "NOT called -- this host has no APC delivery, and the write "
                         "below completed synchronously instead\n",
                         file.path, (unsigned)apc_routine);
    }
    if (event_handle != 0u) {
        kernel_hle_log()("kernel: NtWriteFile(\"%s\") supplied Event %#x, which is NOT "
                         "signalled -- the write completed synchronously, so a title "
                         "that waits on it will wait forever\n",
                         file.path, (unsigned)event_handle);
    }

    /*
     * A HOST-SIDE STAGING BUFFER, THEN ONE WRITE, which is the read path's reasoning run
     * backwards. `kernel_guest_at` cannot prove an in-range guest address is mapped -- with
     * the guest space identity-mapped the host MMU is the page table -- so handing a guest
     * pointer straight to `pwrite` would fault inside the kernel module with its lock held.
     * Copying into host memory first bounds the damage to a reported error, and the write
     * site this boot reaches passes a Buffer the guest computed from a section header, which
     * is precisely the kind of pointer that is wrong when something upstream is wrong.
     */
    uint8_t staging[XDVDFS_SECTOR_SIZE * 8u];
    uint32_t total = 0u;
    uint32_t status = STATUS_SUCCESS;
    /*
     * RUNS AT LEAST ONCE, EVEN FOR A ZERO-LENGTH WRITE. A plain `while (total < length)`
     * would skip the body entirely for `length == 0` and fall through to the success
     * report -- which is a path on which the backing and access checks inside
     * `kernel_file_write_backing` are never consulted at all, so a zero-length write to the
     * user's disc or through a read-only handle would come back STATUS_SUCCESS. NT refuses
     * it on the access check, and so does this.
     */
    bool gate_consulted = false;
    while (!gate_consulted || total < length) {
        gate_consulted = true;
        uint32_t want = length - total;
        if (want > (uint32_t)sizeof(staging)) {
            want = (uint32_t)sizeof(staging);
        }
        if (want > 0u) {
            const uint8_t *source =
                (const uint8_t *)kernel_guest_at(buffer + total, (size_t)want);
            if (!source) {
                kernel_hle_log()("kernel: NtWriteFile(\"%s\") cannot read %u byte(s) from "
                                 "guest address %#x\n",
                                 file.path, (unsigned)want, (unsigned)(buffer + total));
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            memcpy(staging, source, (size_t)want);
        }
        uint32_t put = 0u;
        uint32_t backing_status = STATUS_UNSUCCESSFUL;
        const bool complete = kernel_file_write_backing(file_handle, offset + total,
                                                        staging, want, &put,
                                                        &backing_status);
        /*
         * `put` IS COUNTED WHETHER THE CALL SUCCEEDED OR NOT, and the loop exits on
         * anything short. THIS IS THE LINE THAT MUST NOT SAY `want`: reporting the
         * requested count as the transferred one is how a refused write becomes a
         * successful-looking one, and a zero-byte file is what that looks like afterwards.
         */
        total += put;
        if (!complete) {
            status = backing_status;
            break;
        }
    }

    if (status != STATUS_SUCCESS) {
        lock();
        write_refused_count++;
        bytes_written += (uint64_t)total;
        unlock();
        /* The PARTIAL count goes into the IO_STATUS_BLOCK, next to a failing status. The
         * guest is equipped to read both, and a zero there beside a success would be the
         * one answer that cannot be acted on. */
        (void)write_io_status(io_status, status, total, "NtWriteFile");
        kernel_hle_log()("kernel: NtWriteFile(\"%s\") asked %u byte(s) at %llu and "
                         "transferred %u -- FAILED with %#010x, and NOTHING is being "
                         "reported as written that was not\n",
                         file.path, (unsigned)length, (unsigned long long)offset,
                         (unsigned)total, (unsigned)status);
        return status;
    }

    /* Only an implicit write advances the position, which mirrors the read path: an
     * explicit ByteOffset is a positioned write and leaves the handle's own cursor alone.
     * Without this a second implicit write would land back at the first one's offset and
     * overwrite it, which is the mirror image of the silent append the brief warns about. */
    if (!explicit_offset) {
        (void)kernel_file_open_set_offset(file_handle, offset + total);
    }

    lock();
    write_count++;
    bytes_written += (uint64_t)total;
    unlock();

    if (!write_io_status(io_status, STATUS_SUCCESS, total, "NtWriteFile")) {
        return STATUS_INVALID_PARAMETER;
    }
    if (length == 0u) {
        kernel_hle_log()("kernel: NtWriteFile(\"%s\") asked for ZERO bytes, which is a "
                         "no-op NT also succeeds. The access and backing checks still ran, "
                         "so this is not a path around them\n",
                         file.path);
        return STATUS_SUCCESS;
    }
    kernel_hle_log()("kernel: NtWriteFile(\"%s\") wrote %u byte(s) at %llu (%s) -- REAL "
                     "bytes in a REAL file on the operator's own filesystem, which will "
                     "still be there next run\n",
                     file.path, (unsigned)total, (unsigned long long)offset,
                     explicit_offset ? "an explicit ByteOffset, cursor unchanged"
                                     : "the handle's own position, now advanced");
    return STATUS_SUCCESS;
}

unsigned kernel_io_read_count(void)
{
    lock();
    const unsigned count = read_count;
    unlock();
    return count;
}

uint64_t kernel_io_bytes_read(void)
{
    lock();
    const uint64_t total = bytes_read;
    unlock();
    return total;
}

unsigned kernel_io_fabricated_timestamp_count(void)
{
    lock();
    const unsigned count = fabricated_timestamp_count;
    unlock();
    return count;
}

unsigned kernel_io_unknown_file_class_count(void)
{
    lock();
    const unsigned count = unknown_file_class_count;
    unlock();
    return count;
}

unsigned kernel_io_apc_ignored_count(void)
{
    lock();
    const unsigned count = apc_ignored_count;
    unlock();
    return count;
}

unsigned kernel_io_write_count(void)
{
    lock();
    const unsigned count = write_count;
    unlock();
    return count;
}

uint64_t kernel_io_bytes_written(void)
{
    lock();
    const uint64_t total = bytes_written;
    unlock();
    return total;
}

unsigned kernel_io_write_refused_count(void)
{
    lock();
    const unsigned count = write_refused_count;
    unlock();
    return count;
}

/* ============== NtDeviceIoControlFile (196) and NtFsControlFile (200) ============ */

/*
 * ARITY-OK(196): TEN stack arguments, (FileHandle, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer,
 * OutputBufferLength). The measured table is WRONG for 196: it says 12 over ONE site (an
 * over-count, refused by the three-site quorum in kernel_thunk.c).
 * ARITY-OK(200): TEN stack arguments, the same shape. 200 measures 10 over 3 sites,
 * unanimous. Both hand-verified at the call sites in XapiFormatFATVolumeEx:
 *
 *     0x0038143D / 0x00381479  call esi (esi = [0x4757D0], ordinal 196): 10 pushes each,
 *                              the code literal 0x70000 / 0x74004 as the 6th from the top
 *     0x003816D4               call [0x4757CC] (ordinal 200): 10 pushes, literal 0x90020
 *
 * and FORCED by the stack: the function is an ebp frame that ends `pop esi; pop ebx;
 * pop edi; leave; ret 8`, and those three pops read the saved registers off the stack, so
 * a callee that popped 9 or 11 dwords would hand `pop esi` the wrong word. The other two
 * ordinal-200 sites (0x0037DC8F with code 0x9411C, 0x0046D9B7 with 0x90020) also push 10.
 * The CC0 arity oracle agrees (10 and 10), as corroboration only: it is a different
 * kernel build.
 *
 * WHAT THE TITLE READS BACK, per the guest's own code (0x00381454 and onward), and these
 * are the only fields filled with meaning:
 *     0x70000  output +0x14 BytesPerSector, shifted into a sector shift by a lowest-set-bit
 *              scan, so it must be a power of two. +0x00..+0x13 are not read.
 *     0x74004  output +0x08/+0x0C PartitionLength (low, high). It must exceed 0x1000 and
 *              leave room for the FAT and one 0x4000 root cluster, or the format fails
 *              with ERROR_DISK_FULL. +0x00..+0x07 and +0x10.. are not read.
 *     0x90020  nothing: the output is a NULL buffer of length 0 and the status is only
 *              passed to a NtClose that follows regardless.
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u
#define FSCTL_DISMOUNT_VOLUME 0x00090020u

/* DISK_GEOMETRY and PARTITION_INFORMATION, at the offsets the guest's compiler used. */
#define GEOMETRY_BYTES 0x18u
#define GEOMETRY_CYLINDERS_OFFSET 0x00u
#define GEOMETRY_MEDIA_TYPE_OFFSET 0x08u
#define GEOMETRY_TRACKS_OFFSET 0x0Cu
#define GEOMETRY_SECTORS_PER_TRACK_OFFSET 0x10u
#define GEOMETRY_BYTES_PER_SECTOR_OFFSET 0x14u
#define PARTITION_INFO_BYTES 0x20u
#define PARTITION_INFO_STARTING_OFFSET_OFFSET 0x00u
#define PARTITION_INFO_LENGTH_OFFSET 0x08u
#define PARTITION_INFO_HIDDEN_SECTORS_OFFSET 0x10u
#define PARTITION_INFO_NUMBER_OFFSET 0x14u
#define FIXED_MEDIA 12u
/* CHS values nobody reads: the conventional LBA translation, so the cylinder count is the
 * derivable one rather than a made-up number. */
#define GEOMETRY_TRACKS_PER_CYLINDER 255u
#define GEOMETRY_SECTORS_PER_TRACK 63u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u

/*
 * Common to both ordinals. Returns the NT status to give the guest and, when it is
 * STATUS_SUCCESS, leaves `*out_code` and `*out_file` filled. Everything is read and
 * checked BEFORE a byte is written to the guest, so a refusal never leaves a half-written
 * structure behind.
 */
typedef struct {
    uint32_t code;
    kernel_guest_ptr io_status;
    kernel_guest_ptr output;
    uint32_t output_length;
    kernel_file_open file;
} control_request;

static uint32_t control_prologue(void *context, const char *who, control_request *out)
{
    if (!context) {
        kernel_hle_log()("kernel: %s called with no argument frame\n", who);
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[10];
    for (unsigned i = 0u; i < 10u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest "
                             "stack\n",
                             who, i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t file_handle = args[0];
    out->io_status = args[4];
    out->code = args[5];
    out->output = args[8];
    out->output_length = args[9];

    if (!kernel_file_open_info(file_handle, &out->file)) {
        kernel_hle_log()("kernel: %s on handle %#x, which is not an open file -- "
                         "REFUSED\n",
                         who, (unsigned)file_handle);
        (void)write_io_status(out->io_status, STATUS_INVALID_HANDLE, 0u, who);
        return STATUS_INVALID_HANDLE;
    }
    if (args[1] != 0u || args[2] != 0u) {
        /* Reported and not honoured, exactly as the read and write paths do: there is no
         * APC delivery or event signalling here and the request completes synchronously. */
        kernel_hle_log()("kernel: %s(\"%s\") supplied Event %#x / ApcRoutine %#x, which "
                         "are NOT signalled or called -- the request completed "
                         "synchronously\n",
                         who, out->file.path, (unsigned)args[1], (unsigned)args[2]);
    }
    return STATUS_SUCCESS;
}

/* Refuse a control code this module has no answer for, loudly, and count it. */
static uint32_t control_refuse(const control_request *request, const char *who,
                               const char *why)
{
    lock();
    control_refused_count++;
    unlock();
    kernel_hle_log()("kernel: %s(\"%s\", code %#010x) REFUSED with %#010x: %s\n", who,
                     request->file.path, (unsigned)request->code,
                     (unsigned)KERNEL_FILE_STATUS_INVALID_DEVICE_REQUEST, why);
    (void)write_io_status(request->io_status, KERNEL_FILE_STATUS_INVALID_DEVICE_REQUEST,
                          0u, who);
    return KERNEL_FILE_STATUS_INVALID_DEVICE_REQUEST;
}

static uint32_t hle_nt_device_io_control_file(void *context)
{
    control_request request;
    memset(&request, 0, sizeof(request));
    const uint32_t early = control_prologue(context, "NtDeviceIoControlFile", &request);
    if (early != STATUS_SUCCESS) {
        return early;
    }
    if (request.file.cache_partition == 0u) {
        return control_refuse(&request, "NtDeviceIoControlFile",
                              "only the raw view of a formattable cache partition answers "
                              "device controls, and this handle is not one");
    }

    const uint64_t capacity = request.file.size;
    if (request.code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        if (request.output == 0u || request.output_length < GEOMETRY_BYTES) {
            (void)write_io_status(request.io_status, STATUS_BUFFER_TOO_SMALL, 0u,
                                  "NtDeviceIoControlFile");
            kernel_hle_log()("kernel: NtDeviceIoControlFile(\"%s\", 0x70000) output "
                             "buffer %#x of %u byte(s) cannot hold the %u-byte "
                             "DISK_GEOMETRY -- BUFFER_TOO_SMALL\n",
                             request.file.path, (unsigned)request.output,
                             (unsigned)request.output_length, (unsigned)GEOMETRY_BYTES);
            return STATUS_BUFFER_TOO_SMALL;
        }
        const uint64_t cylinder_bytes = (uint64_t)GEOMETRY_TRACKS_PER_CYLINDER *
                                        GEOMETRY_SECTORS_PER_TRACK *
                                        KERNEL_IO_HDD_BYTES_PER_SECTOR;
        if (!write_u64(request.output + GEOMETRY_CYLINDERS_OFFSET,
                       capacity / cylinder_bytes) ||
            !kernel_guest_write_u32(request.output + GEOMETRY_MEDIA_TYPE_OFFSET,
                                    FIXED_MEDIA) ||
            !kernel_guest_write_u32(request.output + GEOMETRY_TRACKS_OFFSET,
                                    GEOMETRY_TRACKS_PER_CYLINDER) ||
            !kernel_guest_write_u32(request.output + GEOMETRY_SECTORS_PER_TRACK_OFFSET,
                                    GEOMETRY_SECTORS_PER_TRACK) ||
            !kernel_guest_write_u32(request.output + GEOMETRY_BYTES_PER_SECTOR_OFFSET,
                                    KERNEL_IO_HDD_BYTES_PER_SECTOR)) {
            kernel_hle_log()("kernel: NtDeviceIoControlFile could not write the "
                             "DISK_GEOMETRY at %#x\n",
                             (unsigned)request.output);
            (void)write_io_status(request.io_status, STATUS_INVALID_PARAMETER, 0u,
                                  "NtDeviceIoControlFile");
            return STATUS_INVALID_PARAMETER;
        }
        lock();
        control_count++;
        unlock();
        (void)write_io_status(request.io_status, STATUS_SUCCESS, GEOMETRY_BYTES,
                              "NtDeviceIoControlFile");
        kernel_hle_log()("kernel: NtDeviceIoControlFile(\"%s\", IOCTL_DISK_GET_DRIVE_"
                         "GEOMETRY) -> %u bytes/sector (the one field the title reads, "
                         "and the real size of an Xbox disk sector), media FixedMedia, "
                         "%llu cylinders of %u x %u derived from the FABRICATED %llu-byte "
                         "partition\n",
                         request.file.path, (unsigned)KERNEL_IO_HDD_BYTES_PER_SECTOR,
                         (unsigned long long)(capacity / cylinder_bytes),
                         (unsigned)GEOMETRY_TRACKS_PER_CYLINDER,
                         (unsigned)GEOMETRY_SECTORS_PER_TRACK,
                         (unsigned long long)capacity);
        return STATUS_SUCCESS;
    }
    if (request.code == IOCTL_DISK_GET_PARTITION_INFO) {
        if (request.output == 0u || request.output_length < PARTITION_INFO_BYTES) {
            (void)write_io_status(request.io_status, STATUS_BUFFER_TOO_SMALL, 0u,
                                  "NtDeviceIoControlFile");
            kernel_hle_log()("kernel: NtDeviceIoControlFile(\"%s\", 0x74004) output "
                             "buffer %#x of %u byte(s) cannot hold the %u-byte "
                             "PARTITION_INFORMATION -- BUFFER_TOO_SMALL\n",
                             request.file.path, (unsigned)request.output,
                             (unsigned)request.output_length,
                             (unsigned)PARTITION_INFO_BYTES);
            return STATUS_BUFFER_TOO_SMALL;
        }
        /* Every byte of the structure is written, so none of it is stale guest stack. Only
         * PartitionLength is read by the title (MEASURED); the rest is the honest zero of a
         * partition whose start we do not model, plus the number it was mounted as. */
        if (!write_u64(request.output + PARTITION_INFO_STARTING_OFFSET_OFFSET, 0u) ||
            !write_u64(request.output + PARTITION_INFO_LENGTH_OFFSET, capacity) ||
            !kernel_guest_write_u32(request.output + PARTITION_INFO_HIDDEN_SECTORS_OFFSET,
                                    0u) ||
            !kernel_guest_write_u32(request.output + PARTITION_INFO_NUMBER_OFFSET,
                                    request.file.cache_partition) ||
            !kernel_guest_write_u32(request.output + 0x18u, 0u) ||
            !kernel_guest_write_u32(request.output + 0x1Cu, 0u)) {
            kernel_hle_log()("kernel: NtDeviceIoControlFile could not write the "
                             "PARTITION_INFORMATION at %#x\n",
                             (unsigned)request.output);
            (void)write_io_status(request.io_status, STATUS_INVALID_PARAMETER, 0u,
                                  "NtDeviceIoControlFile");
            return STATUS_INVALID_PARAMETER;
        }
        lock();
        control_count++;
        unlock();
        (void)write_io_status(request.io_status, STATUS_SUCCESS, PARTITION_INFO_BYTES,
                              "NtDeviceIoControlFile");
        kernel_hle_log()("kernel: NtDeviceIoControlFile(\"%s\", IOCTL_DISK_GET_PARTITION_"
                         "INFO) -> PartitionLength %llu bytes, FABRICATED (the title "
                         "reads only this field, and sizes its FAT from it); partition "
                         "number %u, every other field zero\n",
                         request.file.path, (unsigned long long)capacity,
                         request.file.cache_partition);
        return STATUS_SUCCESS;
    }
    return control_refuse(&request, "NtDeviceIoControlFile",
                          "only 0x70000 (drive geometry) and 0x74004 (partition info) "
                          "are derived from the title's own reads");
}

static uint32_t hle_nt_fs_control_file(void *context)
{
    control_request request;
    memset(&request, 0, sizeof(request));
    const uint32_t early = control_prologue(context, "NtFsControlFile", &request);
    if (early != STATUS_SUCCESS) {
        return early;
    }
    if (request.file.cache_partition == 0u || request.code != FSCTL_DISMOUNT_VOLUME) {
        return control_refuse(&request, "NtFsControlFile",
                              "only FSCTL_DISMOUNT_VOLUME (0x90020) on the raw view of a "
                              "formattable cache partition is derived; 0x9411C and the "
                              "other call sites in this image are not reached yet");
    }
    lock();
    control_count++;
    unlock();
    (void)write_io_status(request.io_status, STATUS_SUCCESS, 0u, "NtFsControlFile");
    kernel_hle_log()("kernel: NtFsControlFile(\"%s\", FSCTL_DISMOUNT_VOLUME) -> success. "
                     "No filesystem driver is mounted on a virtual image, so there is "
                     "nothing to dismount: the directory view re-checks the image's "
                     "FATX magic at every open instead\n",
                     request.file.path);
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(198): 2 stack arguments, (FileHandle, IoStatusBlock). The one site is
 * 0x0037D0F6 in the wrapper 0x0037D0EA (`ret 4`): `push ecx; push ecx` allocate the
 * 8-byte IO_STATUS_BLOCK local, then `lea eax,[ebp-8]; push eax; push [ebp+8]` push
 * IoStatusBlock and FileHandle, so the measured 4 is the two local-allocation pushes
 * (the hand row in kernel_thunk.c and the oracle both say 2; disassembly re-checked).
 * The only caller, CRT _commit at 0x003CFC8A, uses the wrapper's BOOL result, and the
 * wrapper itself tests only the sign of the status (`jl` at 0x0037D0FE).
 *
 * INFERRED: every write in this module is a synchronous pwrite, so no data is buffered
 * on the host and an open file has nothing to flush. The call completes with
 * STATUS_SUCCESS and Information 0, with no fsync. Read-only (disc) and empty backings
 * also succeed. A handle that is not an open file is refused like its siblings.
 */
static uint32_t hle_nt_flush_buffers_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtFlushBuffersFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t file_handle = 0u;
    uint32_t io_status = 0u;
    if (!kernel_frame_arg(frame, 0u, &file_handle) ||
        !kernel_frame_arg(frame, 1u, &io_status)) {
        kernel_hle_log()("kernel: NtFlushBuffersFile could not read its two arguments "
                         "from the guest stack\n");
        return STATUS_INVALID_PARAMETER;
    }
    kernel_file_open file;
    if (!kernel_file_open_info(file_handle, &file)) {
        kernel_hle_log()("kernel: NtFlushBuffersFile on handle %#x, which is not an "
                         "open file -- REFUSED\n",
                         (unsigned)file_handle);
        (void)write_io_status((kernel_guest_ptr)io_status, STATUS_INVALID_HANDLE, 0u,
                              "NtFlushBuffersFile");
        return STATUS_INVALID_HANDLE;
    }
    if (file.backing == KERNEL_FILE_BACKING_FATX) {
        uint32_t status = STATUS_UNSUCCESSFUL;
        (void)kernel_file_flush_fatx(file_handle, &status);
        if (!write_io_status((kernel_guest_ptr)io_status, status, 0u, "NtFlushBuffersFile")) return STATUS_INVALID_PARAMETER;
        return status;
    }
    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, 0u,
                         "NtFlushBuffersFile")) {
        kernel_hle_log()("kernel: NtFlushBuffersFile could not write the IO_STATUS_BLOCK "
                         "at %#x\n",
                         (unsigned)io_status);
        return STATUS_INVALID_PARAMETER;
    }
    lock();
    flush_count++;
    unlock();
    kernel_hle_log()("kernel: NtFlushBuffersFile(\"%s\") -> success, nothing buffered "
                     "(INFERRED: every write is a synchronous pwrite)\n",
                     file.path);
    return STATUS_SUCCESS;
}

/* NtQueryDirectoryFile structure: FILE_DIRECTORY_INFORMATION, class 1. */
#define KERNEL_IO_FILE_CLASS_DIRECTORY 1u
#define DIRECTORY_INFO_HEADER_BYTES 0x40u
#define DIRECTORY_INFO_CREATION_OFFSET 0x08u
#define DIRECTORY_INFO_ACCESS_OFFSET 0x10u
#define DIRECTORY_INFO_WRITE_OFFSET 0x18u
#define DIRECTORY_INFO_CHANGE_OFFSET 0x20u
#define DIRECTORY_INFO_END_OF_FILE_OFFSET 0x28u
#define DIRECTORY_INFO_ALLOCATION_OFFSET 0x30u
#define DIRECTORY_INFO_ATTRIBUTES_OFFSET 0x38u
#define DIRECTORY_INFO_NAME_LENGTH_OFFSET 0x3Cu
#define DIRECTORY_INFO_NAME_OFFSET 0x40u

/* Read the ANSI_STRING mask (length u16, maximum u16, buffer u32). A zero length is a valid
 * "match all" mask even with a null buffer. False when unreadable or longer than `bytes`. */
static bool read_directory_mask(kernel_guest_ptr string, char *out, size_t bytes)
{
    uint8_t low = 0u;
    uint8_t high = 0u;
    uint32_t buffer = 0u;
    if (!kernel_guest_read_u8(string, &low) || !kernel_guest_read_u8(string + 1u, &high) ||
        !kernel_guest_read_u32(string + 4u, &buffer)) {
        return false;
    }
    const size_t length = (size_t)low | ((size_t)high << 8);
    if (length + 1u > bytes || (length != 0u && buffer == 0u)) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        uint8_t character = 0u;
        if (!kernel_guest_read_u8(buffer + (uint32_t)i, &character)) {
            return false;
        }
        out[i] = (char)character;
    }
    out[length] = '\0';
    return true;
}

/*
 * ARITY-OK(207): 10 stack arguments, (FileHandle, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, FileInformation, Length, FileInformationClass, FileName, RestartScan).
 * MEASURED at all three sites (0x003817CC, 0x00381AC8, 0x00381B2F), which push, right to
 * left: restart, FileName, class 1, Length (0x146/0x148/0x148), buffer, IoStatusBlock, then
 * three zeros (Event, ApcRoutine, ApcContext), then the handle. Matches the hand row (10).
 *
 * CLASS 1 only (FILE_DIRECTORY_INFORMATION): every site passes 1, and the consumer
 * sub_00381990 reads exactly +8..+0x1C times, +0x28 size, +0x38 attributes, +0x3C name length
 * and the ANSI name at +0x40, so the layout is the NT one. One entry per call (INFERRED: the
 * consumers read only the first record and never follow NextEntryOffset, which is written 0).
 * Name is NOT NUL-terminated by us (the guest writes its own at +0x40+len, measured).
 * RESTART: site 1 passes 1 on its first call then 0; sites 2 and 3 always 0 (the first call
 * on a handle starts at the beginning either way). FileName: site 2 passes the file component
 * of the path ("*.*" becomes a zero-length string), the others NULL.
 * Event/Apc/ApcContext are all zero at every site, so a nonzero one is REFUSED loudly.
 * End of listing: NO_MORE_FILES / NO_SUCH_FILE, both of which site 1 compares explicitly.
 */
static uint32_t hle_nt_query_directory_file(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtQueryDirectoryFile called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[10];
    for (unsigned i = 0u; i < 10u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtQueryDirectoryFile could not read argument %u from "
                             "the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t handle = args[0];
    const kernel_guest_ptr io_status = args[4];
    const kernel_guest_ptr information = args[5];
    const uint32_t length = args[6];
    const uint32_t info_class = args[7];
    const kernel_guest_ptr mask_string = args[8];
    const bool restart = (args[9] & 0xFFu) != 0u;
    static const char *const who = "NtQueryDirectoryFile";

    kernel_file_open file;
    if (!kernel_file_open_info(handle, &file)) {
        kernel_hle_log()("kernel: NtQueryDirectoryFile on handle %#x, which is not an open "
                         "file -- REFUSED\n",
                         (unsigned)handle);
        (void)write_io_status(io_status, STATUS_INVALID_HANDLE, 0u, who);
        return STATUS_INVALID_HANDLE;
    }
    if (args[1] != 0u || args[2] != 0u || args[3] != 0u) {
        kernel_hle_log()("kernel: NtQueryDirectoryFile with Event %#x, ApcRoutine %#x, "
                         "ApcContext %#x -- REFUSED: every measured site passes all three "
                         "as zero\n",
                         (unsigned)args[1], (unsigned)args[2], (unsigned)args[3]);
        (void)write_io_status(io_status, STATUS_NOT_IMPLEMENTED, 0u, who);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (info_class != KERNEL_IO_FILE_CLASS_DIRECTORY) {
        lock();
        unknown_file_class_count++;
        unlock();
        kernel_hle_log()("kernel: NtQueryDirectoryFile class %#x on \"%s\" REFUSED -- only "
                         "class 1 (FILE_DIRECTORY_INFORMATION) is measured\n",
                         (unsigned)info_class, file.path);
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u, who);
        return KERNEL_IO_STATUS_INVALID_INFO_CLASS;
    }
    if (information == 0u || length < DIRECTORY_INFO_HEADER_BYTES) {
        (void)write_io_status(io_status, KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH, 0u, who);
        return KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH;
    }
    char mask[KERNEL_FILE_DIR_NAME_MAX + 1u];
    mask[0] = '\0';
    if (mask_string != 0u && !read_directory_mask(mask_string, mask, sizeof(mask))) {
        kernel_hle_log()("kernel: NtQueryDirectoryFile FileName at %#x unreadable or longer "
                         "than %u -- REFUSED\n",
                         (unsigned)mask_string, KERNEL_FILE_DIR_NAME_MAX);
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, who);
        return STATUS_INVALID_PARAMETER;
    }

    kernel_file_dir_entry entry;
    uint32_t status = STATUS_SUCCESS;
    if (!kernel_file_dir_next(handle, restart, mask_string != 0u ? mask : NULL,
                              length - DIRECTORY_INFO_HEADER_BYTES, &entry, &status)) {
        (void)write_io_status(io_status, status, 0u, who);
        return status;
    }

    const uint32_t name_length = (uint32_t)strlen(entry.name);
    /* 0x10 directory, 0x20 archive, as the class-0x22 answer does. Allocation is the size
     * rounded to 4096 and nothing reads it (INFERRED), change time follows last write. */
    if (entry.image_backed) {
        const kernel_file_dir_entry *fatx_entry = &entry;
        const kernel_guest_ptr fatx_information = information;
        const uint32_t fatx_result_bytes = DIRECTORY_INFO_HEADER_BYTES + name_length;
        if (!kernel_guest_write_u32(fatx_information, 0u) ||
            !kernel_guest_write_u32(fatx_information + 4u, 0u) ||
            !write_u64(fatx_information + DIRECTORY_INFO_CREATION_OFFSET, fatx_entry->creation_time) ||
            !write_u64(fatx_information + DIRECTORY_INFO_ACCESS_OFFSET, fatx_entry->last_access_time) ||
            !write_u64(fatx_information + DIRECTORY_INFO_WRITE_OFFSET, fatx_entry->last_write_time) ||
            !write_u64(fatx_information + DIRECTORY_INFO_CHANGE_OFFSET, fatx_entry->last_write_time) ||
            !write_u64(fatx_information + DIRECTORY_INFO_END_OF_FILE_OFFSET, fatx_entry->size) ||
            !write_u64(fatx_information + DIRECTORY_INFO_ALLOCATION_OFFSET, fatx_entry->allocation) ||
            !kernel_guest_write_u32(fatx_information + DIRECTORY_INFO_ATTRIBUTES_OFFSET, fatx_entry->attributes) ||
            !kernel_guest_write_u32(fatx_information + DIRECTORY_INFO_NAME_LENGTH_OFFSET, name_length) ||
            !kernel_guest_write_bytes(fatx_information + DIRECTORY_INFO_NAME_OFFSET, fatx_entry->name, name_length)) {
            (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, who); return STATUS_INVALID_PARAMETER;
        }
        (void)write_io_status(io_status, STATUS_SUCCESS, fatx_result_bytes, who);
        lock(); directory_entry_count++; unlock();
        kernel_hle_log()("kernel: NtQueryDirectoryFile FATX image: actual entry/chain bytes, "
                         "timestamp/status/order model INFERRED; no xemu acceptance\n");
        return STATUS_SUCCESS;
    }
    const uint64_t allocation = (entry.size + 4095u) / 4096u * 4096u;
    if (!kernel_guest_write_u32(information, 0u) ||
        !kernel_guest_write_u32(information + 4u, 0u) ||
        !write_u64(information + DIRECTORY_INFO_CREATION_OFFSET, entry.creation_time) ||
        !write_u64(information + DIRECTORY_INFO_ACCESS_OFFSET, entry.last_access_time) ||
        !write_u64(information + DIRECTORY_INFO_WRITE_OFFSET, entry.last_write_time) ||
        !write_u64(information + DIRECTORY_INFO_CHANGE_OFFSET, entry.last_write_time) ||
        !write_u64(information + DIRECTORY_INFO_END_OF_FILE_OFFSET, entry.size) ||
        !write_u64(information + DIRECTORY_INFO_ALLOCATION_OFFSET, allocation) ||
        !kernel_guest_write_u32(information + DIRECTORY_INFO_ATTRIBUTES_OFFSET,
                                entry.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT
                                                   : FILE_ATTRIBUTE_ARCHIVE_BIT) ||
        !kernel_guest_write_u32(information + DIRECTORY_INFO_NAME_LENGTH_OFFSET,
                                name_length) ||
        !kernel_guest_write_bytes(information + DIRECTORY_INFO_NAME_OFFSET, entry.name,
                                  name_length)) {
        (void)write_io_status(io_status, STATUS_INVALID_PARAMETER, 0u, who);
        return STATUS_INVALID_PARAMETER;
    }
    (void)write_io_status(io_status, STATUS_SUCCESS, DIRECTORY_INFO_HEADER_BYTES + name_length,
                          who);
    lock();
    directory_entry_count++;
    unlock();
    kernel_hle_log()("kernel: NtQueryDirectoryFile(\"%s\") -> \"%s\" %s %llu bytes (REAL; "
                     "creation time is the last-write time, INFERRED)\n",
                     file.path, entry.name, entry.is_directory ? "directory" : "file",
                     (unsigned long long)entry.size);
    return STATUS_SUCCESS;
}

unsigned kernel_io_directory_entry_count(void)
{
    lock();
    const unsigned count = directory_entry_count;
    unlock();
    return count;
}

unsigned kernel_io_control_count(void)
{
    lock();
    const unsigned count = control_count;
    unlock();
    return count;
}

unsigned kernel_io_control_refused_count(void)
{
    lock();
    const unsigned count = control_refused_count;
    unlock();
    return count;
}

unsigned kernel_io_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_NtQueryVolumeInformationFile, hle_nt_query_volume_information_file},
        {ORD_NtQueryInformationFile, hle_nt_query_information_file},
        {ORD_NtQueryFullAttributesFile, hle_nt_query_full_attributes_file},
        {ORD_NtSetInformationFile, hle_nt_set_information_file},
        {ORD_NtReadFile, hle_nt_read_file},
        {ORD_NtWriteFile, hle_nt_write_file},
        {ORD_NtDeviceIoControlFile, hle_nt_device_io_control_file},
        {ORD_NtFsControlFile, hle_nt_fs_control_file},
        {ORD_NtFlushBuffersFile, hle_nt_flush_buffers_file},
        {ORD_NtQueryDirectoryFile, hle_nt_query_directory_file},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
