/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fabricated guest FILE_OBJECT bodies, and ordinal 76 IoQueryVolumeInformation:
 * the pair that lets the one measured file-handle ObReferenceObjectByHandle site
 * run on real guest memory instead of a handle value it would then dereference.
 *
 * =========================== THE MEASURED SHAPE ===========================
 *
 * Ordinal 76 has exactly ONE call site, 0x0037D188, inside the wrapper at
 * 0x0037D14F, which generated/retail/manifest.json names `_XGetFilePhysicalSortKey@4`
 * (`ret 4`). Its sole in-image caller is 0x000290F0. Re-disassembled for this task
 * (generated/retail/src/sub_0037d14f.c, sub_000290f0.c), the wrapper does:
 *
 *     ObReferenceObjectByHandle(handle, IoFileObjectType, &p);   // fails -> -1
 *     IoQueryVolumeInformation(p, 5, 0x20, buf, &returned);      // fails -> deref, -1
 *     if ([buf+8] == 4) {
 *         if ([buf+0xC] == 0x58544146)        // "FATX"
 *             key = (byte [[p+8]] & 1) ? 0 : [[p+8]+0x1C];
 *         else if ([buf+0xC] == 0x58464447)   // "GDFX"
 *             key = [[p+8]];
 *     } else raise 0xC000000D;                 // then key = -1
 *     ObfDereferenceObject(p);                 // EVERY post-reference path
 *
 * So the GUEST ITSELF dereferences `p+8` and then the pointer found there. The
 * consumer 0x000290F0 holds a 0x28-slot table of CreateFileA handles at 0x6612D0
 * (every recorded path is prefixed "d:\", i.e. disc pak files), calls the wrapper,
 * and on success returns `offset/2048 + key` (signed divide, `sar 0xB` after a
 * 0x7FF bias). The result is only ever used as a SORT KEY: callers store it on a
 * load request and insert the request in key order (sub_00063770, sub_00063040).
 *
 * A NOTE THE T94 AUDIT GOT SLIGHTLY WRONG, corrected here from the same
 * disassembly: the block behind `[FileObject+8]` is PER FILE (an FCB shape), not a
 * per-volume device extension, because the consumer adds the file's own byte
 * offset over 2048 to it and calls the result the file's physical sort key. On
 * GDFX its first dword is therefore the FILE'S OWN START SECTOR, and the FATX
 * read at +0x1C is gated on flag bit 0 of the block's first byte, which the audit
 * note omitted.
 *
 * ======================== WHAT THIS MODULE FABRICATES ====================
 *
 * For a live KERNEL_OBJECT_FILE handle with a kernel_file open slot behind it,
 * one owned guest allocation of 0x30 bytes (the T141 pattern: a private guest
 * heap, real low-4GB memory, freed with the handle):
 *
 *     +0x00..+0x07  zero            (FILE_OBJECT header fields, never read by the
 *                                    measured site; zeros are FABRICATED filler)
 *     +0x08         u32             -> body+0x10, the one pointer the guest reads
 *     +0x0C         zero
 *     +0x10..+0x2F  the FCB-shaped block the guest double-dereference lands on:
 *                     +0x00  u32    GDFX: the file's start sector in the image.
 *                                   FATX: 0, so flag bit 0 is CLEAR and the guest
 *                                   proceeds to its +0x1C read.
 *                     +0x1C  u32    FATX: 0 (see below). GDFX: 0, never read.
 *
 * WHICH VALUE GOES AT +0 FOR GDFX, derived rather than guessed: the mounted disc
 * is a standalone game-partition image whose descriptor is probed at sector 32
 * only (xdvdfs.h), so every sector this host knows is image-relative and the
 * partition's own base is 0 by construction. `kernel_file_open.disc_sector` is
 * the file's real start sector from the real directory entry, so
 * `disc_sector + offset/2048` is exactly the data's sector in the modeled address
 * space and preserves physical ordering, which is all the consumer uses.
 *
 * WHY FATX ANSWERS 0: the FATX-shaped backings here are host DIRECTORIES
 * (partition1 and the cache-partition content views). They have no cluster map
 * and no sector placement at all, so any nonzero value would be INVENTED. 0 is
 * also the value the guest's own flagged arm produces, so the title demonstrably
 * handles it. No FATX handle reaches the measured consumer anyway: all 0x28
 * table slots are "d:\" opens.
 *
 * WHO GETS NO BODY, on the record: a FILE handle with no kernel_file slot (issued
 * directly by another subsystem), an EMPTY-backed fabricated file, and a virtual
 * raw device. Those keep the legacy handle-value surrogate from
 * ObReferenceObjectByHandle, and ordinal 76 then REFUSES the value loudly with
 * STATUS_INVALID_PARAMETER, which the wrapper turns into a clean -1 and the
 * consumer into 0. A fabricated placement for a file that has none would be a
 * silent wrong answer; the refusal is the honest one and the title handles it.
 *
 * ORDINAL 76 ANSWERS EXACTLY THE MEASURED QUESTION: class 5
 * (FileFsAttributeInformation) on a fabricated body, writing FileSystemNameLength
 * 4 at +0x08 and the name dword at +0x0C ("FATX" or "GDFX" per the handle's
 * backing), ReturnedLength 0x10, and NOTHING else (the T143 precedent: bytes the
 * guest never reads stay unwritten). Every other class is refused with
 * STATUS_INVALID_INFO_CLASS, an unknown FileObject with STATUS_INVALID_PARAMETER,
 * and a Length below 0x10 with STATUS_INFO_LENGTH_MISMATCH.
 *
 * ARITY-OK(76): FIVE stack arguments (FileObject, FsInformationClass, Length,
 * FsInformation, ReturnedLength), measured at the one site; the sixth-looking
 * push at 0x0037D178 is a callee-save `push edi` (docs/tasks.md T94 audit). The
 * hand thunk row {76u, THUNK_CC_STDCALL, 5u} predates this module.
 *
 * LOCK ORDER. Fabrication runs with NO other lock held (kernel_object calls the
 * provider before taking its table lock): it reads the object table and
 * kernel_file under their own locks, then builds the block under this module's
 * lock, then binds under the object lock. Release is called BY kernel_object
 * under the object lock and takes only this module's lock and the guest heap
 * lock, so the one nesting is object -> file-object -> guest_mem and there is no
 * cycle with kernel_file's file -> object order.
 */

#ifndef TSFP_XBOX_KERNEL_FILE_OBJECT_H
#define TSFP_XBOX_KERNEL_FILE_OBJECT_H

#include <stdbool.h>
#include <stdint.h>

/* The filesystem name dwords the guest compares against, little-endian "FATX" and
 * "GDFX". FATX is MEASURED twice over: the guest's own format writes it
 * (kernel_file.h KERNEL_FILE_FATX_MAGIC) and the wrapper compares it. */
#define KERNEL_FILE_OBJECT_FS_NAME_FATX 0x58544146u
#define KERNEL_FILE_OBJECT_FS_NAME_GDFX 0x58464447u

/* One fabricated body: 0x10 header bytes plus the 0x20-byte FCB-shaped block. */
#define KERNEL_FILE_OBJECT_BODY_BYTES 0x30u
#define KERNEL_FILE_OBJECT_EXTENSION_OFFSET 0x10u
/* The FATX arm's read offset inside the block, measured at 0x0037D1B6. */
#define KERNEL_FILE_OBJECT_FATX_VALUE_OFFSET 0x1Cu

/* Statuses this module needs that nt_status.h does not carry, module-prefixed per
 * the kernel_file.h convention. INVALID_INFO_CLASS matches kernel_io's value. */
#define KERNEL_FILE_OBJECT_STATUS_INVALID_INFO_CLASS 0xC0000003u
#define KERNEL_FILE_OBJECT_STATUS_INFO_LENGTH_MISMATCH 0xC0000004u

/**
 * Register ordinal 76 with the HLE dispatcher and install the FILE-body provider
 * into kernel_object. Returns how many ordinals bound (1).
 */
unsigned kernel_file_object_register(void);

/**
 * Destroy the body heap and forget every fabricated body. Quiescent test setup
 * only, and AFTER kernel_object_reset: a live object entry whose file_body
 * pointed into the destroyed heap would be a dangling guest pointer.
 */
void kernel_file_object_reset(void);

/** Fabricated bodies currently live (bound to a live handle and not yet freed). */
unsigned kernel_file_object_body_count(void);

/** How many bodies were ever fabricated this session. */
uint32_t kernel_file_object_fabricated_count(void);

/** How many ordinal-76 calls were refused, for any reason. Each is logged. */
uint32_t kernel_file_object_query_refused_count(void);

/** How many ordinal-76 calls answered class 5 successfully. */
uint32_t kernel_file_object_query_answered_count(void);

#endif /* TSFP_XBOX_KERNEL_FILE_OBJECT_H */
