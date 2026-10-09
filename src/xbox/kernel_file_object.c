/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_file_object.h for the measured evidence, the fabricated layout and
 * the lock-order argument. This file is the mechanism only.
 */

#include "kernel_file_object.h"

#include <pthread.h>
#include <string.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

#define ORD_IoQueryVolumeInformation 76u

/* The measured request: class 5 is FileFsAttributeInformation, whose header is 12
 * bytes (attributes, max component length, name length) followed by the name. A
 * 4-byte name therefore needs 0x10 bytes, and the one measured call passes 0x20. */
#define FS_ATTRIBUTE_CLASS 5u
#define FS_ATTRIBUTE_NAME_LENGTH_OFFSET 0x08u
#define FS_ATTRIBUTE_NAME_OFFSET 0x0Cu
#define FS_ATTRIBUTE_BYTES_NEEDED 0x10u

/* ---------------------------------------------------------------------------
 * THE LOCK. Guards the body heap token and the counters, nothing else: the
 * body-to-handle map lives in the object table (kernel_object_entry.file_body),
 * so lookups never come through here. RECURSIVE for the same reason as every
 * other module lock in this tree: kernel_hle_log()'s sink is caller-supplied.
 * It is only ever a LEAF below the object lock (release) or held around pure
 * guest_mem work (fabricate), per the header's lock-order note.
 * ------------------------------------------------------------------------- */
static pthread_mutex_t body_lock;
static pthread_once_t body_lock_once = PTHREAD_ONCE_INIT;

static void body_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&body_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void body_enter(void)
{
    (void)pthread_once(&body_lock_once, body_lock_init);
    (void)pthread_mutex_lock(&body_lock);
}

static void body_leave(void)
{
    (void)pthread_mutex_unlock(&body_lock);
}

/* The private guest heap every body lives in (the T141 pattern). 0 until the
 * first fabrication. The token carries a generation, so a heap destroyed by
 * reset is recognisably dead rather than aliased by a recycled slot. */
static uint32_t body_heap;
static unsigned live_bodies;
static uint32_t fabricated_count;
static uint32_t query_refused_count;
static uint32_t query_answered_count;

/* Allocate and fill one body block. Returns its guest VA, or 0. Holds only the
 * module lock around the heap work; the caller binds the result afterwards. */
static uint32_t body_block_build(uint32_t first_dword, uint32_t fatx_value)
{
    body_enter();
    if (body_heap == 0u || !guest_heap_valid(body_heap)) {
        body_heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
        if (body_heap == 0u) {
            body_leave();
            kernel_hle_log()("kernel: FILE_OBJECT body heap could not be created\n");
            return 0u;
        }
    }
    const uint32_t body = guest_heap_alloc(body_heap, KERNEL_FILE_OBJECT_BODY_BYTES);
    if (body == 0u) {
        body_leave();
        kernel_hle_log()("kernel: FILE_OBJECT body allocation failed\n");
        return 0u;
    }

    /* Zero the WHOLE block first: a recycled heap block is not fresh-zero, and
     * every byte the measured site never reads must still be a stated value
     * rather than a previous tenant's. */
    unsigned char zeros[KERNEL_FILE_OBJECT_BODY_BYTES];
    memset(zeros, 0, sizeof(zeros));
    bool ok = kernel_guest_write_bytes(body, zeros, sizeof(zeros));
    /* The one pointer the guest reads: FILE_OBJECT+8 -> the FCB-shaped block. */
    ok = ok && kernel_guest_write_u32(body + 8u,
                                      body + KERNEL_FILE_OBJECT_EXTENSION_OFFSET);
    /* GDFX: the file's start sector at +0. FATX: 0 here, which also keeps flag
     * bit 0 CLEAR so the guest proceeds to its +0x1C read. */
    ok = ok && kernel_guest_write_u32(body + KERNEL_FILE_OBJECT_EXTENSION_OFFSET + 0u,
                                      first_dword);
    /* The FATX arm's value. 0 on every backing this host models (header note). */
    ok = ok && kernel_guest_write_u32(body + KERNEL_FILE_OBJECT_EXTENSION_OFFSET +
                                          KERNEL_FILE_OBJECT_FATX_VALUE_OFFSET,
                                      fatx_value);
    if (!ok) {
        (void)guest_heap_free(body_heap, body);
        body_leave();
        kernel_hle_log()("kernel: FILE_OBJECT body at %#x could not be written\n",
                         (unsigned)body);
        return 0u;
    }
    body_leave();
    return body;
}

/*
 * The provider kernel_object calls BEFORE taking its table lock. Fabricates and
 * binds a body for a live FILE handle whose kernel_file backing has a filesystem
 * identity, or returns 0 so the legacy handle surrogate stands.
 */
static uint32_t fabricate_body(uint32_t handle)
{
    kernel_object_entry copy;
    if (!kernel_object_get_copy(handle, &copy) || copy.kind != KERNEL_OBJECT_FILE) {
        return 0u;
    }
    if (copy.file_body != 0u) {
        return copy.file_body;
    }

    kernel_file_open open;
    if (!kernel_file_open_info(handle, &open)) {
        /* A FILE handle another subsystem issued directly, with no open slot.
         * Announced: the handle value it keeps getting is only dereferenceable
         * by ordinal 76's own refusal path. */
        kernel_hle_log()("kernel: FILE handle %#x has no open-file slot -- no "
                         "FILE_OBJECT body, the legacy handle surrogate stands\n",
                         (unsigned)handle);
        return 0u;
    }

    uint32_t fs_name = 0u;
    uint32_t first_dword = 0u;
    if (open.backing == KERNEL_FILE_BACKING_DISC) {
        /* The real start sector from the real directory entry; image-relative,
         * and the image's own base is 0 by construction (header note). */
        fs_name = KERNEL_FILE_OBJECT_FS_NAME_GDFX;
        first_dword = open.disc_sector;
    } else if (open.backing == KERNEL_FILE_BACKING_HOST_DIR && !open.device) {
        fs_name = KERNEL_FILE_OBJECT_FS_NAME_FATX;
        first_dword = 0u;
    } else {
        /* EMPTY-backed fabrications and virtual raw devices have no filesystem
         * identity to answer for. Refusing the body keeps ordinal 76's refusal
         * as the loud backstop instead of inventing a placement. */
        kernel_hle_log()("kernel: FILE handle %#x (backing %d%s) gets no "
                         "FILE_OBJECT body -- no filesystem identity to answer "
                         "ordinal 76 with\n",
                         (unsigned)handle, (int)open.backing,
                         open.device ? ", device" : "");
        return 0u;
    }

    const uint32_t body = body_block_build(first_dword, 0u);
    if (body == 0u) {
        return 0u;
    }
    if (!kernel_object_bind_file_body(handle, body, fs_name)) {
        /* Lost a race, or the handle died in the gap. The loser's block must not
         * leak: nothing else will ever name it. */
        body_enter();
        (void)guest_heap_free(body_heap, body);
        body_leave();
        kernel_object_entry raced;
        if (kernel_object_get_copy(handle, &raced) && raced.file_body != 0u) {
            return raced.file_body;
        }
        return 0u;
    }
    body_enter();
    live_bodies++;
    fabricated_count++;
    body_leave();
    kernel_hle_log()("kernel: FILE handle %#x -> FABRICATED FILE_OBJECT body %#x "
                     "(%s, extension first dword %#x)\n",
                     (unsigned)handle, (unsigned)body,
                     fs_name == KERNEL_FILE_OBJECT_FS_NAME_GDFX ? "GDFX" : "FATX",
                     (unsigned)first_dword);
    return body;
}

/* Called by kernel_object under ITS lock when the last reference to a
 * body-carrying entry goes away. Leaf: only this module's lock and the heap. */
static void release_body(uint32_t body)
{
    body_enter();
    if (body_heap != 0u && guest_heap_valid(body_heap) &&
        guest_heap_free(body_heap, body)) {
        if (live_bodies > 0u) {
            live_bodies--;
        }
    } else {
        kernel_hle_log()("kernel: FILE_OBJECT body %#x could not be freed\n",
                         (unsigned)body);
    }
    body_leave();
}

/*
 * ARITY-OK(76): FIVE stack arguments (FileObject, FsInformationClass, Length,
 * FsInformation, ReturnedLength); the hand thunk row and the T94 audit carry the
 * site evidence. FileObject is a POINTER, not a handle: the wrapper passes back
 * exactly what ObReferenceObjectByHandle published, so only a fabricated body VA
 * is a recognisable argument and anything else is refused loudly.
 */
static uint32_t handle_io_query_volume_information(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[5];
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: IoQueryVolumeInformation could not read "
                             "argument %u from the guest stack\n",
                             i);
            body_enter();
            query_refused_count++;
            body_leave();
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t file_object = args[0];
    const uint32_t fs_class = args[1];
    const uint32_t length = args[2];
    const uint32_t fs_information = args[3];
    const uint32_t returned_length = args[4];

    uint32_t handle = 0u;
    uint32_t fs_name = 0u;
    if (!kernel_object_find_file_body(file_object, &handle, &fs_name)) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation(%#x) is not a "
                         "fabricated FILE_OBJECT body -- REFUSED, nothing "
                         "written\n",
                         (unsigned)file_object);
        body_enter();
        query_refused_count++;
        body_leave();
        return STATUS_INVALID_PARAMETER;
    }
    if (fs_class != FS_ATTRIBUTE_CLASS) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation class %u on handle %#x "
                         "is not implemented (only the measured class 5), "
                         "refusing\n",
                         (unsigned)fs_class, (unsigned)handle);
        body_enter();
        query_refused_count++;
        body_leave();
        return KERNEL_FILE_OBJECT_STATUS_INVALID_INFO_CLASS;
    }
    if (fs_information == 0u) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation with a NULL "
                         "FsInformation buffer -- REFUSED\n");
        body_enter();
        query_refused_count++;
        body_leave();
        return STATUS_INVALID_PARAMETER;
    }
    if (length < FS_ATTRIBUTE_BYTES_NEEDED) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation class 5 Length %#x is "
                         "below the %#x a 4-byte name needs, refusing\n",
                         (unsigned)length, (unsigned)FS_ATTRIBUTE_BYTES_NEEDED);
        body_enter();
        query_refused_count++;
        body_leave();
        return KERNEL_FILE_OBJECT_STATUS_INFO_LENGTH_MISMATCH;
    }

    /* Only the two fields the guest reads (T143 precedent: bytes it never looks
     * at stay unwritten, so a consumer of an unmeasured field shows up as a
     * reader of its own poison rather than of our zeros). */
    if (!kernel_guest_write_u32(
            kernel_guest_add(fs_information, FS_ATTRIBUTE_NAME_LENGTH_OFFSET), 4u) ||
        !kernel_guest_write_u32(
            kernel_guest_add(fs_information, FS_ATTRIBUTE_NAME_OFFSET), fs_name)) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation could not write the "
                         "FsInformation at %#x\n",
                         (unsigned)fs_information);
        body_enter();
        query_refused_count++;
        body_leave();
        return STATUS_INVALID_PARAMETER;
    }
    if (returned_length != 0u &&
        !kernel_guest_write_u32(returned_length, FS_ATTRIBUTE_BYTES_NEEDED)) {
        kernel_hle_log()("kernel: IoQueryVolumeInformation could not write "
                         "ReturnedLength at %#x\n",
                         (unsigned)returned_length);
        body_enter();
        query_refused_count++;
        body_leave();
        return STATUS_INVALID_PARAMETER;
    }
    body_enter();
    query_answered_count++;
    body_leave();
    return STATUS_SUCCESS;
}

/* --- the public surface ---------------------------------------------------- */

unsigned kernel_file_object_register(void)
{
    const kernel_object_file_body_ops ops = {fabricate_body, release_body};
    kernel_object_set_file_body_ops(ops);
    return kernel_hle_register(ORD_IoQueryVolumeInformation,
                               handle_io_query_volume_information)
               ? 1u
               : 0u;
}

void kernel_file_object_reset(void)
{
    body_enter();
    if (body_heap != 0u && guest_heap_valid(body_heap)) {
        (void)guest_heap_destroy(body_heap);
    }
    body_heap = 0u;
    live_bodies = 0u;
    fabricated_count = 0u;
    query_refused_count = 0u;
    query_answered_count = 0u;
    body_leave();
}

unsigned kernel_file_object_body_count(void)
{
    body_enter();
    const unsigned result = live_bodies;
    body_leave();
    return result;
}

uint32_t kernel_file_object_fabricated_count(void)
{
    body_enter();
    const uint32_t result = fabricated_count;
    body_leave();
    return result;
}

uint32_t kernel_file_object_query_refused_count(void)
{
    body_enter();
    const uint32_t result = query_refused_count;
    body_leave();
    return result;
}

uint32_t kernel_file_object_query_answered_count(void)
{
    body_enter();
    const uint32_t result = query_answered_count;
    body_leave();
    return result;
}
