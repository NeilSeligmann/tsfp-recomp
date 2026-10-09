/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Guest-observable kernel structure layouts, MEASURED FROM THE BINARY.
 *
 * Every structure here is one the guest allocates, writes, and reads itself. The
 * kernel HLE only ever shares them. That makes a wrong offset the worst class of
 * bug this project has: it does not fault, it writes the right bytes to the wrong
 * place, and the symptom appears arbitrarily far from the cause.
 *
 * `docs/lifter-patches/06-guest-struct-static-asserts.md` records upstream
 * declaring `XBOX_IO_STATUS_BLOCK` as 16 bytes where the guest's is 8 -- a
 * host-layout struct wearing an `XBOX_` name, overrunning by 8 bytes into
 * whatever local sat next to it. That is the failure mode this file exists to
 * prevent, which is why:
 *
 *   1. Every field is a FIXED-WIDTH type. Guest pointers and handles are
 *      `uint32_t` guest virtual addresses, NOT host pointers. The layout is
 *      therefore the guest's on a 64-bit host, which is the whole point.
 *   2. Every size and every offset carries a `_Static_assert`. A layout without
 *      one is a layout nobody checked.
 *   3. Every field records its evidence inline: how many independent call sites
 *      support it, and where to look.
 *
 * PROVENANCE. `docs/provenance.md` forbids Microsoft XDK headers and leaked
 * symbol or source corpora. Nothing here comes from those. Every offset, width
 * and size below was derived from the user-supplied retail image
 * (SHA-256 `3cfd001a84fc...`) by reading its own call sites and its own
 * statically-initialised data. Where the publicly documented NT layout agrees,
 * that is noted as CORROBORATION and is explicitly labelled -- it is never the
 * source. Where recalled NT knowledge would have given the WRONG answer, that is
 * called out too, because it happened twice (see the critical section, and the
 * absence of `OBJECT_ATTRIBUTES.Length`).
 *
 * Full field tables, per-field confidence, and the list of what could NOT be
 * determined: `docs/guest-structs.md`. Read that before trusting any field here
 * that is marked UNVERIFIED.
 */

#ifndef TSFP_XBOX_GUEST_STRUCTS_H
#define TSFP_XBOX_GUEST_STRUCTS_H

#include <stddef.h>
#include <stdint.h>

/**
 * A guest virtual address.
 *
 * Spelled as its own type so that a host pointer can never be assigned into a
 * guest structure field by accident. The guest is 32-bit; the host is not.
 */
typedef uint32_t guest_va;

/* =========================================================================
 * IO_STATUS_BLOCK -- 8 bytes
 * =========================================================================
 *
 * Unblocks all file I/O. Patch 06 asserts 8 bytes; this was re-derived
 * independently and CONFIRMS it.
 *
 * MEASURED over 68 call sites in 4 executable sections (`.text` 57, XPP 2,
 * XNET 3, D3D 5, XONLINE 1), covering NtOpenFile 12, NtWriteFile 11,
 * NtSetInformationFile 10, NtCreateFile 10, NtQueryInformationFile 8,
 * NtReadFile 6, NtQueryVolumeInformationFile 5, NtQueryDirectoryFile 3,
 * NtFsControlFile 3.
 *
 * THE DECISIVE TEST FOR THE SIZE: is there ANY access at offset >= 8 on a
 * confirmed IoStatusBlock pointer? ZERO, across all 68 sites. The 90 hits in the
 * `[base+8 .. base+15]` window all resolve to named neighbouring objects, and
 * the existence of those live neighbours is itself the proof the struct ends
 * at 8. Three independent forms of that proof:
 *
 *   (a) THE NEIGHBOUR DECLARES ITS OWN SIZE, in the same argument list.
 *       `sub_0042B7CD` (XONLINE) has a 16-byte frame holding exactly two things:
 *
 *           0x0042B7D0  sub  esp, 0x10          ; 16-byte frame, TOTAL
 *           0x0042B7E0  push 8                  ; Length = 8 -- buffer is EIGHT bytes
 *           0x0042B7E2  lea  eax, [ebp - 8]     ; FileInformation
 *           0x0042B7E6  lea  eax, [ebp - 0x10]  ; IoStatusBlock
 *           0x0042B7ED  call dword ptr [0x4757bc]  ; -> NtSetInformationFile
 *
 *       16 - 8 = 8. The call itself states the neighbour's size, so this needs
 *       no assumption about compiler packing.
 *
 *   (b) FOUR 8-BYTE OBJECTS PACKED BACK TO BACK, zero slack. `sub_0037CDCB`
 *       reserves 0x20 and shares one IoStatusBlock at `ebp-0x20` across three
 *       calls, each declaring `Length = 8` for a buffer at `ebp-0x18`,
 *       `ebp-0x10`, `ebp-8`.
 *
 *   (c) THE IOSB IS THE TOPMOST FRAME LOCAL in 8 functions, sitting at `ebp-8`
 *       with nothing touched between `ebp-4` and `ebp+8`. A 16-byte struct there
 *       would overwrite the saved EBP and the return address, and the function
 *       could not return at all.
 *
 * Had this been 16 bytes with an 8-byte Information at offset 8, the bytes
 * destroyed at real sites are: the path string NtCreateFile is reading, the
 * file-position buffer the same NtQueryInformationFile is filling, the
 * OBJECT_ATTRIBUTES the same NtCreateFile is reading, and the running file
 * offset of every streaming read/write loop. Silent, and nowhere near the
 * symptom.
 */
typedef struct guest_io_status_block {
    /* +0x00 NTSTATUS. MEASURED 4 bytes. Callers pre-set it to STATUS_PENDING
     * (0x103) and read it back sign-tested on the async path:
     *     0x0037CC49  mov dword ptr [esi], edi    ; edi = 0x103
     *     0x0037CCB8  mov eax, dword ptr [ebp-8]  ; then cmp/jl -- NTSTATUS sign bit
     * Only 2 functions / 4 accesses, and that scarcity is explained rather than
     * glossed: 66 of 68 sites branch on the function's `eax` return instead, so
     * only the two STATUS_PENDING -> NtWaitForSingleObject paths, where `eax` is
     * stale, must read the field. Offset and width high confidence.
     *
     * NT declares this a union with a `PVOID Pointer`. In THIS image the value is
     * never dereferenced, never passed as a pointer and never range-checked, so
     * "it is a union" is INFERRED BY ANALOGY, NOT MEASURED. Harmless either way:
     * both members are 4 bytes on a 32-bit guest. */
    uint32_t status;

    /* +0x04 Bytes transferred, or the create-disposition result. MEASURED 4
     * bytes, 4 functions / 9 accesses, every one a 32-bit mov/cmp. Three
     * semantically independent uses, so this is not one observation counted
     * thrice:
     *   - stored to the Win32 byte-count out-param:
     *       0x0037CC6C  mov ecx, dword ptr [esi + 4]
     *   - compared against the requested Length to detect a short read:
     *       0x0037F7A4  cmp dword ptr [ebp + 0x64], esi   ; esi = Length arg
     *   - NtCreateFile disposition result, small enum values that cannot be a
     *     byte count, yielding ERROR_ALREADY_EXISTS:
     *       0x0037D394  cmp dword ptr [ebp - 0x10], 3
     * Offsets 0 and 4 are never read together as one 64-bit quantity anywhere,
     * which is what rules out a 64-bit Information. */
    uint32_t information;
} guest_io_status_block;

_Static_assert(sizeof(guest_io_status_block) == 8,
               "guest IO_STATUS_BLOCK is 8 bytes: status 4 + information 4. "
               "Zero accesses at offset >= 8 across 68 measured call sites.");
_Static_assert(offsetof(guest_io_status_block, status) == 0x00, "IOSB.status at +0x00");
_Static_assert(offsetof(guest_io_status_block, information) == 0x04,
               "IOSB.information at +0x04");

/* =========================================================================
 * OBJECT_STRING / ANSI_STRING -- 8 bytes
 * =========================================================================
 *
 * Needed by RtlInitAnsiString (ordinal 289, 22 call sites) and by every
 * NtCreateFile/NtOpenFile path, which reach it through OBJECT_ATTRIBUTES.
 *
 * This is the best-evidenced structure in this file, because it is pinned twice
 * over: by construction code AND by 18 compile-time instances in the image.
 *
 * THE OPEN-CODED RtlInitAnsiString at `sub_0037C9C4` builds the whole struct at
 * `ebp-8` and shows all three fields with their widths:
 *
 *     0x0037C9CC  mov  dword ptr [ebp - 4], eax   ; +0x04 Buffer   <- src   (32-bit)
 *     0x0037C9D2  mov  dl, byte ptr [eax]         ; strlen loop, BYTE-wise
 *     0x0037C9DB  mov  word ptr [ebp - 8], ax     ; +0x00 Length        (16-bit)
 *     0x0037C9E3  mov  word ptr [ebp - 6], ax     ; +0x02 MaximumLength (16-bit)
 *
 * The byte-wise strlen is what proves the characters are single-byte, i.e. ANSI
 * and not UTF-16.
 *
 * THE COMPILE-TIME CROSS-CHECK. 18 statically initialised instances were found
 * in `.rdata`/`.data`. All 18 satisfy BOTH `Length == strlen(Buffer)` AND
 * `MaximumLength == Length + 1`, with Buffer resolving to NUL-terminated ASCII
 * device paths. That joint condition holding 18 times over could not survive any
 * other field split, so the 2/2/4 layout is measured, not assumed. Examples,
 * read straight out of the image:
 *
 *     0x004A13E8  len=6  max=7  buf=0x004A1404  "\??\D:"
 *     0x004A151C  len=28 max=29 buf=0x004A1524  "\Device\Harddisk0\partition0"
 *     0x00549758  len=47 max=48 buf=0x00549728  "...\devkit\xbmovie.dat"
 *
 * ARRAY STRIDE 8, read as raw bytes at 0x004A14EC -- two adjacent instances:
 *     06 00 07 00 5C 15 4A 00 | 06 00 07 00 54 15 4A 00
 *
 * CORROBORATION (labelled, not a source): this matches the documented NT
 * ANSI_STRING shape. Unlike the critical section below, recalled knowledge would
 * have been right here -- but it was still measured.
 */
typedef struct guest_object_string {
    /* +0x00 Byte count excluding the NUL. MEASURED 16-bit: word-sized reads AND
     * word-sized writes in 9 distinct functions, plus 18 static instances. The
     * decisive refutation of a 32-bit field is the word-sized CLEAR --
     *     0x003805A5  and word ptr [ebp - 8], 0
     * which would leave garbage in the high half if the field were 4 bytes.
     * Used as a BYTE index into Buffer, confirming the unit:
     *     0x0037D2A6  cmp byte ptr [eax + edi - 1], 0x5c   ; src[Length-1] == '\'
     *
     * One pattern that could be misread: two sites load a dword spanning +0x00
     * and +0x02 (e.g. 0x0037C9DF). Both immediately write back with `mov word
     * ptr`, so only the low half is live -- compiler width-widening on a
     * just-stored slot, not a 32-bit Length. */
    uint16_t length;

    /* +0x02 Byte capacity of Buffer, including the NUL. MEASURED 16-bit in 6
     * functions. The capacity is provably in BYTES with zero slack:
     *     0x0037F915  mov word ptr [ebp - 0xe], 0x104   ; MaximumLength = 260
     * against a buffer spanning ebp-0x120..ebp-0x1C, i.e. exactly 0x104 bytes.
     * If it counted wide characters that buffer would need 0x208.
     *
     * The guest always sets it to Length+1. Whether the KERNEL requires the NUL
     * to be counted is NOT observable from the caller side. */
    uint16_t maximum_length;

    /* +0x04 Guest VA of NUL-terminated single-byte character data. MEASURED
     * 32-bit in 8 functions plus 18 statics. Read as a dword and indexed as
     * bytes in the same breath:
     *     0x00381A14  mov   edx, dword ptr [ebp - 4]        ; Buffer
     *     0x00381A1D  movzx eax, word ptr [ebp - 8]         ; Length
     *     0x00381A35  cmp   byte ptr [edx + eax - 1], 0x5c  ; Buffer[Length-1]
     * The name `Buffer` is a naming choice; the semantics are measured. */
    guest_va buffer;
} guest_object_string;

_Static_assert(sizeof(guest_object_string) == 8,
               "guest OBJECT_STRING is 8 bytes: length 2 + maximum_length 2 + buffer 4. "
               "Corroborated by an array stride of 8 at guest VA 0x004A14EC.");
_Static_assert(offsetof(guest_object_string, length) == 0x00, "OBJECT_STRING.length at +0x00");
_Static_assert(offsetof(guest_object_string, maximum_length) == 0x02,
               "OBJECT_STRING.maximum_length at +0x02");
_Static_assert(offsetof(guest_object_string, buffer) == 0x04, "OBJECT_STRING.buffer at +0x04");
_Static_assert(sizeof(((guest_object_string *) 0)->length) == 2,
               "OBJECT_STRING.length is 16-bit: measured from word-sized writes and a "
               "word-sized `and ..., 0` clear, which a 32-bit field could not use.");

/* =========================================================================
 * OBJECT_ATTRIBUTES -- 12 bytes, and there is NO Length field
 * =========================================================================
 *
 * Needed by NtCreateFile (190) and NtOpenFile (202).
 *
 * THIS IS THE CASE WHERE RECALLED NT KNOWLEDGE WOULD HAVE CORRUPTED EVERYTHING.
 * Desktop NT's OBJECT_ATTRIBUTES is 24 bytes and BEGINS with a `ULONG Length`
 * holding sizeof(self); `InitializeObjectAttributes` emits `mov dword ptr [x],
 * 24` unconditionally. Assuming that here would shift every subsequent field
 * by 4 and read a handle as a size.
 *
 * MEASURED instead, by a scripted census of every call to NtCreateFile, NtOpenFile,
 * NtOpenSymbolicLinkObject, NtQueryFullAttributesFile, NtDeleteFile, NtCreateEvent
 * and NtCreateMutant (the seven imported ordinals that take an OBJECT_ATTRIBUTES;
 * IoCreateDevice takes an OBJECT_STRING and is NOT one of them, and ordinals 188,
 * 191, 193, 194, 201, 66, 243, 239 and 244 are not imported by this title). The
 * census was RE-DERIVED for T56 from the linear disassembly: 31 call sites reach
 * the file, symlink, query and delete ordinals (27 as `call [slot]`, 4 through a
 * register loaded from the slot) from 26 functions, plus 2 sites (NtCreateEvent,
 * NtCreateMutant) that build their OBJECT_ATTRIBUTES through ONE shared helper,
 * `sub_00381B7C`. The earlier "26 sites in 24 functions" is reproduced only by
 * leaving out the two register-called functions (`sub_00414511`, `sub_00422CD4`) and
 * it never saw the helper. All 31 inline builders write exactly three dwords, at
 * +0x00, +0x04, +0x08, and nothing else, and the helper does the same.
 *
 * THE VALUES, by builder (MEASURED unless marked):
 *
 *   Inline, 31 sites : root = 0, a register holding 0, or a live directory handle
 *                      parameter (`sub_003817A4`, `sub_00381921`), except 9 sites
 *                      that store 0xFFFFFFFD. The register at the two
 *                      `sub_003DF420` sites was not traced. attributes = 0x40 at
 *                      all 31.
 *   Helper 0x00381B7C: root = 0xFFFFFFFC, attributes = 0x80, name = the string the
 *                      caller passes. Reached ONLY by CreateEventA (`sub_0037FF30`)
 *                      and CreateMutexA (`sub_0037FFB1`), and only for a NAMED
 *                      object: an unnamed one passes a NULL OBJECT_ATTRIBUTES.
 *
 *     0x00381B93  mov dword ptr [eax], 0xfffffffc    ; +0x00 root
 *     0x00381B99  mov dword ptr [eax + 8], 0x80      ; +0x08 attributes
 *
 * So `attributes` takes TWO values, not one. An earlier version of this comment claimed
 * a single value because the census never included the two ordinals that go through
 * the helper.
 *
 * VERDICT -- NO Length AT OFFSET 0. Three strands, two negative and one
 * positive and conclusive:
 *
 *   (i)  No size constant is ever stored at +0x00. Across all the inline sites the
 *        stored operand is 0 (a zeroed register or `and ..., 0`), 0xFFFFFFFD, or a
 *        live register, and the helper stores 0xFFFFFFFC. None of 0x18 / 24 / 0x0c /
 *        12 is stored at that offset anywhere.
 *   (ii) The struct is only 12 bytes, so a Length plus the three real fields
 *        could not fit. See the ceiling proof below.
 *   (iii) Offset 0 demonstrably holds a LIVE KERNEL HANDLE. `sub_003817A4`
 *        (FLIRT: `_XapiNukeDirectoryFromHandle@8`, not FindNextFile as an earlier
 *        version of this comment said) passes the same directory handle to
 *        NtQueryDirectoryFile and then into +0x00, to open the just-enumerated
 *        name relatively:
 *            0x003817C9  push  dword ptr [ebp + 8]      ; handle -> NtQueryDirectoryFile
 *            0x0038180D  mov   dword ptr [ebp - 0x24], eax  ; +0x00 = the SAME handle
 *        and conversely +0x00 is NULL exactly when ObjectName is a fully
 *        qualified path (`sub_0037D844`, 0x0037D870).
 *
 * THE CEILING PROOF OF 12 BYTES. `sub_00381921` packs three of this file's
 * structures contiguously against the frame base with zero slack. NtOpenFile
 * takes 6 __stdcall args pushed right to left, so the OA is the one pushed at
 * 0x00381952:
 *
 *     ebp-0x1C  IO_STATUS_BLOCK   (arg 3)         -> next local at ebp-0x14, gap 8
 *     ebp-0x14  OBJECT_STRING     (RtlInitAnsiString dest) -> ebp-0x0C, gap 8
 *     ebp-0x0C  OBJECT_ATTRIBUTES                 -> ebp (frame base), gap 12
 *
 *     0x0038193B  mov dword ptr [ebp - 0xc], eax       ; +0x00 RootDirectory (handle param)
 *     0x00381946  mov dword ptr [ebp - 8], eax         ; +0x04 ObjectName = &the OBJECT_STRING
 *     0x0038195C  mov dword ptr [ebp - 4], 0x40        ; +0x08 Attributes  <- LAST slot
 *
 * `ebp-4` is the last dword before the saved frame pointer, so a fourth field is
 * physically impossible. Four further functions give the same 0x0C gap
 * independently (`sub_0037D231`, `sub_0037DBF1`, `sub_0037F8CF`, `sub_00380E6C`).
 *
 * NOTE: unlike OBJECT_STRING, NO statically initialised OBJECT_ATTRIBUTES exists
 * anywhere in the image -- `.data`, `.rdata` and `.data1` were scanned for a
 * dword triple whose middle element points at one of the 18 confirmed
 * OBJECT_STRINGs, with zero hits. Every one is stack-built, so there is no
 * compile-time cross-check here and the evidence rests on the 26-function census
 * and five stack-gap measurements.
 */
typedef struct guest_object_attributes {
    /* +0x00 Object-directory HANDLE that object_name is relative to. MEASURED
     * 32-bit at all 31 inline sites and in the helper. See the handle-threading
     * evidence above. NOT a Length -- that is the headline finding for this struct.
     *
     * WARNING, UNRESOLVED: 9 of the 31 inline sites (every ANSI-path Win32 shim)
     * store the constant 0xFFFFFFFD here, and the named-event/mutex helper stores
     * 0xFFFFFFFC. The VALUES are measured, their MEANING is not derivable from the
     * guest, because each is a pseudo-handle the kernel resolves internally. The
     * HLE must decide what they denote. That a small negative handle in this
     * position names an object-directory root is INFERRED BY ANALOGY, NOT MEASURED
     * -- treat it as a hypothesis. That the two values differ is MEASURED, so they
     * name different roots and must not be folded into one. */
    guest_va root_directory;

    /* +0x04 Guest VA of a `guest_object_string`. MEASURED 32-bit at every builder.
     * Proven to point at the 8-byte string struct both for stack-built strings
     * (0x00381946 above, pointing at the RtlInitAnsiString destination) and for
     * static ones:
     *     0x0037F8EB  mov dword ptr [ebp - 0x18], 0x549140   ; -> static "\??\W:" */
    guest_va object_name;

    /* +0x08 Flags. MEASURED 32-bit at every builder. Two values occur, never a
     * combination: 0x40 at all 31 inline sites,
     *     0x0037D313  mov dword ptr [ebp - 0x18], 0x40
     * and 0x80 in the named-event/mutex helper at 0x00381B99. Only these two
     * bits are ever exercised, so the rest of the layout is NOT derivable. The
     * offset, width and the two values are measured. The NAMES are INFERRED by
     * analogy with desktop NT, where 0x40 is OBJ_CASE_INSENSITIVE and 0x80 is
     * OBJ_OPENIF, and nothing in this image tests either reading. */
    uint32_t attributes;
} guest_object_attributes;

_Static_assert(sizeof(guest_object_attributes) == 12,
               "guest OBJECT_ATTRIBUTES is 12 bytes: root_directory 4 + object_name 4 + "
               "attributes 4. It has NO Length field, unlike desktop NT's 24-byte form -- "
               "see the ceiling proof in sub_00381921.");
_Static_assert(offsetof(guest_object_attributes, root_directory) == 0x00,
               "OBJECT_ATTRIBUTES.root_directory at +0x00 -- NOT a Length. Every "
               "measured builder stores a handle, NULL, 0xFFFFFFFC or 0xFFFFFFFD here, "
               "never a size constant.");
_Static_assert(offsetof(guest_object_attributes, object_name) == 0x04,
               "OBJECT_ATTRIBUTES.object_name at +0x04");
_Static_assert(offsetof(guest_object_attributes, attributes) == 0x08,
               "OBJECT_ATTRIBUTES.attributes at +0x08");

/** The `attributes` value stored by all 31 inline builders. NOT the only value: see
 * GUEST_OBJ_ATTRIBUTES_NAMED_OBJECT. Kept under this name because the file tests use it. */
#define GUEST_OBJ_ATTRIBUTES_OBSERVED 0x00000040u

/** The `attributes` value stored by the named-event/mutex helper at 0x00381B99. */
#define GUEST_OBJ_ATTRIBUTES_NAMED_OBJECT 0x00000080u

/** Bit values by analogy with desktop NT. INFERRED, see the field comment. */
#define GUEST_OBJ_CASE_INSENSITIVE_INFERRED 0x00000040u
#define GUEST_OBJ_OPENIF_INFERRED 0x00000080u

/** `root_directory` pseudo-handle seen at 9 inline ANSI-path Win32 shims. Meaning UNRESOLVED. */
#define GUEST_OBJ_ROOT_DIRECTORY_PSEUDO 0xFFFFFFFDu

/** `root_directory` pseudo-handle stored by the named-event/mutex helper. Meaning UNRESOLVED,
 * and T142 measured the path that stores it to be STATICALLY DEAD: the helper 0x381B7C has
 * exactly two call sites (0x37FF47, 0x37FFC8), both on the name != NULL branches of
 * CreateEventA 0x37FF30 / CreateMutexA 0x37FFB1, each wrapper has exactly one caller in the
 * image (0x291FD / 0x3D21CA), both pass a NULL name, and no section stores either wrapper's
 * address as data. No in-image execution can deliver this root to the kernel. */
#define GUEST_OBJ_ROOT_NAMED_OBJECT_PSEUDO 0xFFFFFFFCu

/* =========================================================================
 * RTL_CRITICAL_SECTION -- 28 bytes, with an embedded 16-byte dispatcher object
 * =========================================================================
 *
 * Unblocks RtlLeaveCriticalSection (ordinal 294, 114 call sites -- second by call
 * count in the whole image) and RtlEnterCriticalSection (277). Also relevant:
 * RtlInitializeCriticalSection is ordinal 291 at thunk slot 0x475900 (NOT 184,
 * which is NtAllocateVirtualMemory -- a recalled ordinal number that proved
 * wrong on checking, which is why slot-to-ordinal was re-derived from the XBE's
 * own thunk table: 151 slots, 151 distinct ordinals, strictly one-to-one).
 *
 * THIS IS THE SECOND CASE WHERE RECALLED NT KNOWLEDGE WOULD HAVE BEEN WRONG.
 * The Win32 RTL_CRITICAL_SECTION is 24 bytes and starts with DebugInfo,
 * LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount. The guest's
 * is 28 bytes and starts with an EMBEDDED DISPATCHER OBJECT. Assuming the
 * desktop shape would have been wrong in both size and every field.
 *
 * HOW IT WAS MEASURED: three of the image's five critical sections are
 * STATICALLY INITIALISED in the XBE's own data. A scan of all 24 sections for a
 * dispatcher-object signature returned exactly 3 hits and zero false positives,
 * byte-identical modulo the self-pointer:
 *
 *     0x004124B4 (DSOUND): 00040001 00000000 004124BC 004124BC FFFFFFFF 0 0
 *     0x0046C6B8 (XPP)   : 00040001 00000000 0046C6C0 0046C6C0 FFFFFFFF 0 0
 *     0x00549148 (.data) : 00040001 00000000 00549150 00549150 FFFFFFFF 0 0
 *
 * Exactly 7 dwords. The header dword 0x00040001 is little-endian bytes
 * 01 00 04 00, i.e. Type=1, Absolute=0, Size=4, Inserted=0.
 *
 * WHY +0x00..+0x0F IS PROVABLY A 16-BYTE DISPATCHER OBJECT AND NOT A STORY:
 * there is a CONTROL. A bare event at `.data 0x00549660` has the identical
 * self-linked header shape (Type=0, Size=4, wait list pointing at itself) but
 * holds 0x00000002 at +0x10, NOT -1. So the dispatcher object genuinely ends at
 * +0x10 and the -1 is critical-section-specific. Image-wide, only two header
 * shapes have a self-linked wait list: (Type=0,Size=4) once, and (Type=1,Size=4)
 * three times -- and all three Type=1 instances are exactly the addresses the
 * code passes to the Enter/Leave exports.
 *
 * SIZE 0x1C = 28, from five independent directions:
 *   1. The static image contents: 7 dwords, three times over.
 *   2. The neighbour of 0x00549148: its +0x1C and +0x20 hold 00549164 00549164,
 *      a self-linked list head beginning exactly at +0x1C.
 *   3. Four neighbour gaps, all exactly 0x1C. In three of them the following
 *      object is a plain dword needing only 4-byte alignment, so padding cannot
 *      explain the gap -- which is what rules out 0x18.
 *   4. Offset-of inside a wrapper (`sub_003C7B90`): 0xC own bytes + the CS, and
 *      the container's next member is read at +0x28 = 0xC + 0x1C.
 *   5. Bases 0x004124B4, 0x00772384, 0x007724AC are 4- but not 8-aligned, so
 *      there is no 8-byte-aligned member; 7 dwords is consistent.
 *
 * TWO FINDINGS THAT CHANGE WHAT THE IMPLEMENTER SHOULD BUILD:
 *
 *   A. THERE IS NO INLINED INTERLOCKED FAST PATH IN THIS IMAGE. The 114-vs-10
 *      Leave/Enter asymmetry was expected to mean the guest manipulates the
 *      fields directly. It does not. The cause is mundane: `sub_004069FE`
 *      (DSOUND, 30 bytes) is a shared lock helper called by 52 functions, each
 *      of which then calls Leave directly on BOTH its success and error exits.
 *      99 of the 114 Leave sites are DSOUND. All 37 `lock`-prefixed instructions
 *      in the image are misdisassembled padding -- Xbox is uniprocessor, so no
 *      genuine `lock` prefix exists. There is therefore no byte-for-byte
 *      sequence the HLE must match. That is a freedom, not a gap.
 *
 *   B. THE GUEST NEVER READS OR WRITES A SINGLE FIELD. A +/-0x80 operand-window
 *      scan around all five CS addresses across all ten sections finds only
 *      `push <base>` and `mov reg, <base>` -- whole-struct pointers, zero field
 *      accesses. BUT: the three statically-initialised CSes are NEVER passed to
 *      ordinal 291, so roughly 105 of the 114 Leave calls operate on a lock
 *      initialised purely by XBE image data. AN IMPLEMENTATION THAT KEYS CS
 *      STATE OFF A HOST SIDE-TABLE POPULATED ON RtlInitializeCriticalSection
 *      WILL MISS ALMOST EVERY CALL. State must live in guest memory at these
 *      offsets.
 *
 * CORROBORATION (labelled, not a source): the 28-byte embedded-event shape
 * matches the publicly documented Xbox layout. That was checked only AFTER the
 * measurement, and is recorded because it is reassuring, not because it is
 * evidence.
 */
typedef struct guest_rtl_critical_section {
    /* +0x00 Dispatcher-object type. MEASURED = 1 in all 3 static images. The
     * control event at 0x00549660 has 0 here, so the value discriminates a
     * critical section's event from a plain one. Name by analogy. */
    uint8_t event_type;

    /* +0x01 MEASURED = 0 in all 3. Name INFERRED, low confidence. */
    uint8_t event_absolute;

    /* +0x02 Object size in dwords. MEASURED = 4, i.e. a 16-byte dispatcher
     * object -- and that is corroborated by the control object rather than
     * assumed. High confidence on both value and meaning. */
    uint8_t event_size;

    /* +0x03 MEASURED = 0 in all 3. Name INFERRED, low confidence. */
    uint8_t event_inserted;

    /* +0x04 MEASURED = 0 in all 3. Name INFERRED. */
    uint32_t event_signal_state;

    /* +0x08 / +0x0C Wait-list head, MEASURED as self-linked: both dwords equal
     * (base + 8) in all 3 static images. A self-reference is unambiguous
     * evidence of an empty LIST_ENTRY head and cannot be anything else, so these
     * two are high confidence on both offset and meaning.
     *
     * An HLE that relocates or copies a critical section MUST re-point these at
     * their new address; they are absolute guest VAs, not offsets. */
    guest_va event_wait_list_flink;
    guest_va event_wait_list_blink;

    /* +0x10 MEASURED = 0xFFFFFFFF in all 3 static images, where the control
     * event holds 2 -- so this dword is critical-section-specific. The VALUE is
     * measured; that -1 encodes "initialised but unowned" is INFERRED by analogy
     * to NT's LockCount sentinel. Name medium confidence. */
    uint32_t lock_count;

    /* +0x14 and +0x18: MEASURED = 0 in all 3 static images, and touched by
     * NOTHING anywhere in the image.
     *
     * NOT MEASURABLE FROM THIS BINARY, NAMED FROM A CLEAN EXTERNAL SOURCE.
     * Both are zero in every instance and neither is ever accessed, so the order
     * could not be established here -- an earlier version of this header left them
     * `unknown_14`/`unknown_18` rather than guess, which was the right call at the
     * time.
     *
     * The order is now settled by `XboxDev/nxdk` `lib/xboxkrnl/` (**CC0-1.0**,
     * audited clean of Microsoft DDK provenance -- see `docs/clean-sources-audit.md`),
     * corroborated by three further independent sources. Recorded as corroboration
     * rather than measurement: nothing in THIS binary distinguishes them, so if a
     * future measurement contradicts nxdk, the measurement wins. */
    uint32_t recursion_count; /* +0x14, from nxdk (CC0), not measured here */
    uint32_t owning_thread;   /* +0x18, same */
} guest_rtl_critical_section;

_Static_assert(sizeof(guest_rtl_critical_section) == 28,
               "guest RTL_CRITICAL_SECTION is 28 bytes = 7 dwords: a 16-byte embedded "
               "dispatcher object, lock_count, recursion_count and owning_thread. NOT the "
               "24-byte desktop Win32 layout. Measured from 3 static images plus four "
               "neighbour gaps of 0x1C.");
_Static_assert(offsetof(guest_rtl_critical_section, event_type) == 0x00, "CS.event_type at +0x00");
_Static_assert(offsetof(guest_rtl_critical_section, event_absolute) == 0x01,
               "CS.event_absolute at +0x01");
_Static_assert(offsetof(guest_rtl_critical_section, event_size) == 0x02, "CS.event_size at +0x02");
_Static_assert(offsetof(guest_rtl_critical_section, event_inserted) == 0x03,
               "CS.event_inserted at +0x03");
_Static_assert(offsetof(guest_rtl_critical_section, event_signal_state) == 0x04,
               "CS.event_signal_state at +0x04");
_Static_assert(offsetof(guest_rtl_critical_section, event_wait_list_flink) == 0x08,
               "CS.event_wait_list_flink at +0x08 -- self-linked to base+8 when idle");
_Static_assert(offsetof(guest_rtl_critical_section, event_wait_list_blink) == 0x0C,
               "CS.event_wait_list_blink at +0x0C -- self-linked to base+8 when idle");
_Static_assert(offsetof(guest_rtl_critical_section, lock_count) == 0x10,
               "CS.lock_count at +0x10 -- 0xFFFFFFFF in all 3 static images, where the "
               "control event at 0x00549660 holds 2 instead");
_Static_assert(offsetof(guest_rtl_critical_section, recursion_count) == 0x14,
               "CS.recursion_count at +0x14 -- named from nxdk (CC0), not measured "
               "from this binary; both fields are zero in every instance here");
_Static_assert(offsetof(guest_rtl_critical_section, owning_thread) == 0x18,
               "CS.owning_thread at +0x18 -- named from nxdk (CC0), not measured here");

/* --- measured initial values, for constructing or validating a guest CS -----
 *
 * These are the exact bytes the XBE ships for its three static critical
 * sections. An HLE that has to synthesise one should reproduce them, and one
 * that wants to sanity-check a guest pointer can test against them.
 */

/** `event_type` for a critical section's embedded event. Control event has 0. */
#define GUEST_CS_EVENT_TYPE 0x01u

/** `event_size` in dwords: a 16-byte dispatcher object. */
#define GUEST_CS_EVENT_SIZE 0x04u

/** `lock_count` as shipped. -1; believed "initialised but unowned" (INFERRED). */
#define GUEST_CS_LOCK_COUNT_INIT 0xFFFFFFFFu

/** Offset the self-linked wait-list head points at, relative to the CS base. */
#define GUEST_CS_WAIT_LIST_OFFSET 0x08u

/* --- where this image's critical sections live -----------------------------
 *
 * Recorded because the distinction is load-bearing: the first three are
 * initialised ONLY by image data and never pass through ordinal 291, yet they
 * carry the overwhelming majority of the 114 Leave calls.
 */

/** DSOUND global CS. Static-initialised only. Carries ~100 Leave sites. */
#define GUEST_CS_VA_DSOUND 0x004124B4u

/** XPP global CS. Static-initialised only. */
#define GUEST_CS_VA_XPP 0x0046C6B8u

/** `.text` global CS. Static-initialised only. */
#define GUEST_CS_VA_TEXT 0x00549148u

/** A plain event, NOT a critical section. The control that bounded the
 *  dispatcher object at 16 bytes: same header shape, but 2 at +0x10. */
#define GUEST_EVENT_VA_CONTROL 0x00549660u

#endif /* TSFP_XBOX_GUEST_STRUCTS_H */
