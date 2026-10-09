/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File I/O on an open handle: NtQueryVolumeInformationFile (ordinal 218), and the
 * read/query/set family the boot trace reaches after it.
 *
 * SPLIT FROM kernel_file.c ON PURPOSE. That module answers "what does this NAME
 * resolve to" -- OBJECT_ATTRIBUTES, OBJECT_STRING, the mount table. This one answers
 * "what does this HANDLE do", and needs none of that decoding. Keeping them apart
 * means the name-resolution evidence and the per-handle-state evidence do not have to
 * be read together to check either.
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED. Every number below was read out of
 * `tools/kernel_ordinals.py`, not remembered:
 *
 *     218  NtQueryVolumeInformationFile
 *     219  NtReadFile
 *     211  NtQueryInformationFile
 *     226  NtSetInformationFile
 *
 * None of the four is in that file's `SUSPECT_ON_XDK_5849` list.
 *
 * =====================================================================
 * ORDINAL 218: NtQueryVolumeInformationFile -- ARITY 5
 * =====================================================================
 *
 *     NTSTATUS __stdcall NtQueryVolumeInformationFile(
 *             HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
 *             PVOID FsInformation, ULONG Length, ULONG FsInformationClass);
 *
 * ARITY-OK evidence. 5 stack arguments, at all 5 call sites in this image. Counted
 * by hand at the reached site and corroborated by an independent byte-level sweep for
 * the thunk slot 0x004757C4, which found 5 `call [slot]` sites and ZERO reached
 * through a register or through a `jmp [slot]` import stub -- so for once the
 * measured scanner's blind spot does not apply. Sites:
 *
 *     call site     Length        FsInformationClass   note
 *     0x0037D02E    0x18          1
 *     0x0037D570    0x18          3                    + a leading `push esi`
 *     0x0037D716    [ebp-0x24]    1
 *     0x0037D732    esi           5
 *     0x00380D58    0x18          3                    the site REACHED
 *
 * The leading `push esi` at 0x0037D560 is a callee-saved register SAVE, not a sixth
 * argument, and that is settled by the epilogue rather than asserted:
 * `sub_0037D4F9` ends `pop edi; pop ebx; pop esi; leave; ret 0x10`, matching the push
 * order esi (0x0037D560), ebx (0x0037D596), edi (0x0037D597). Three saves, three pops,
 * reverse order. A sixth argument would leave that `ret 0x10` unbalanced.
 *
 * ARGUMENT ORDER, PINNED BY LITERALS at the reached site 0x00380D58:
 *
 *     push 3                     ; arg4 FsInformationClass
 *     push 0x18                  ; arg3 Length
 *     lea eax,[ebp-0x2c]; push   ; arg2 FsInformation -> a 24-byte frame local
 *     lea eax,[ebp-8];    push   ; arg1 IoStatusBlock -- the SAME block NtOpenFile used
 *     push dword ptr [ebp+8]     ; arg0 FileHandle -- the handle NtOpenFile just wrote
 *
 * -------- WHAT Length = 0x18 TOLD US, AND WHAT IT DID NOT --------
 *
 * 0x18 is 24, and 24 is exactly 8 + 8 + 4 + 4. That is a real constraint and not a
 * coincidence, because the guest's own compiler computed it: `Length` is a literal
 * `push 0x18` at BOTH class-3 sites, in two different functions, which is what a
 * `sizeof` of the receiving structure looks like after constant folding.
 *
 * But a size alone cannot distinguish 8+8+4+4 from 4+4+4+4+4+4 or from 8+4+4+8. The
 * shape is pinned because the guest READS THE BUFFER BACK, at offsets and widths that
 * only one partition of 24 bytes admits. At the reached site:
 *
 *     0x00380D6D  mov  eax, dword ptr [ebp - 0x1c]   ; buffer +0x10, 32-bit
 *     0x00380D70  imul eax, dword ptr [ebp - 0x18]   ; buffer +0x14, 32-bit
 *     0x00380D74  cmp  eax, dword ptr [ebp + 0xc]    ; against the caller's expectation
 *     0x00380D77  je   0x380d7e
 *     0x00380D79  mov  esi, 0xc000014f               ; else FAIL
 *
 * and at 0x0037D570, whose buffer is at `ebp-0x34`:
 *
 *     0x0037D58F  mov  esi, dword ptr [ebp - 0x24]   ; buffer +0x10, 32-bit
 *     0x0037D592  imul esi, dword ptr [ebp - 0x20]   ; buffer +0x14, 32-bit
 *     0x0037D59A  push edi (0) / push esi            ; that product, widened to 64-bit
 *     0x0037D59C  push dword ptr [ebp - 0x28]        ; buffer +0x0C  -- high half
 *     0x0037D59F  push dword ptr [ebp - 0x2c]        ; buffer +0x08  -- low half
 *     0x0037D5A2  call 0x3c9ed0                      ; 64x64 multiply helper
 *     0x0037D5A8  push edi (0) / push esi            ; the same product again
 *     0x0037D5A9  push dword ptr [ebp - 0x30]        ; buffer +0x04  -- high half
 *     0x0037D5AE  push dword ptr [ebp - 0x34]        ; buffer +0x00  -- low half
 *     0x0037D5B4  call 0x3c9ed0
 *
 * So, MEASURED and not inferred:
 *   - +0x10 and +0x14 are each 32 bits, read individually, and their PRODUCT is a
 *     bytes-per-allocation-unit. Two independent sites agree.
 *   - +0x00 and +0x08 are each 64 bits -- the low/high halves are pushed as a pair
 *     into a 64x64 multiply helper, which is only meaningful if they are one quantity.
 *   - Each of those two 64-bit quantities is multiplied by the bytes-per-unit to
 *     produce a BYTE count, and the two results are written to two separate caller
 *     out-parameters (`[ebp+0xc]` and `[ebp+0x10]`).
 *   - 8 + 8 + 4 + 4 = 24 = the pushed Length. The size and the field accesses
 *     corroborate each other; NEITHER ALONE WOULD PIN IT.
 *
 * INFERRED, and labelled as such because the evidence genuinely stops here:
 *   - WHICH 64-bit field is the total and which is the available count. Both are read
 *     the same way and multiplied by the same factor; only the order in which the two
 *     out-parameters are filled distinguishes them, and reading that as
 *     (available, total) is the conventional shape of a free-space query, not a
 *     measurement. We put the total at +0x00.
 *   - WHICH 32-bit field is the sectors-per-allocation-unit and which the
 *     bytes-per-sector. `imul` commutes, so no site in this image can tell them apart.
 *     Harmless: only the product is ever used. We order them the conventional way.
 *
 * -------- THE GUEST CHECKS OUR ANSWER, SO THE ANSWER CANNOT BE INVENTED --------
 *
 * This is the part that makes 218 unlike every ordinal implemented before it: a
 * plausible-looking wrong answer does not slide by, it fails the run with a status
 * that blames the device. `sub_00380D0D` requires
 *
 *     FsInformation[+0x10] * FsInformation[+0x14] == <the caller's expectation>
 *
 * or it returns 0xC000014F and the boot path reports a device that is not ready. The
 * expectation is computed in `sub_0037DA8F`, and it is not a constant:
 *
 *     0x0037DACC  mov ecx, dword ptr [0x10124]
 *     0x0037DADD  shr ecx, 0x1e                 ; the TOP TWO BITS
 *     0x0037DAE5  mov esi, 0x4000               ; 16384
 *     0x0037DAEB  shl esi, cl                   ; 16384 << those two bits
 *
 * VA 0x10124 is inside the XBE header, which is mapped at the image base 0x10000, so
 * this is header offset 0x124 -- the `InitFlags` field. In THIS image it reads
 * 0x00000005, whose top two bits are 0, so the expectation is 16384 << 0 = 16384.
 * Read straight out of the user's own executable, not assumed.
 *
 * 16384 bytes per allocation unit is the real geometry of an Xbox hard-disk
 * partition, and it factors the honest way: 32 sectors of 512 bytes. So
 * KERNEL_IO_HDD_BYTES_PER_SECTOR * KERNEL_IO_HDD_SECTORS_PER_UNIT == 16384 is not a
 * number chosen to satisfy the check, it is the geometry that satisfies it because it
 * is the right one. The constants are nevertheless asserted against each other at
 * compile time, so a later edit cannot quietly break the product.
 *
 * WHAT IS STILL FABRICATED HERE, SAID PLAINLY. The GEOMETRY is real. The FREE SPACE
 * IS NOT: there is no Xbox hard disk behind this host, so the total and available
 * allocation-unit counts are numbers we chose. They are announced on every call and
 * counted in `kernel_io_fabricated_geometry_count()`, and the policy is switchable
 * exactly as `kernel_file.h`'s missing-file policy is -- a default that silently
 * reported a healthy disc with plenty of room would manufacture a boot that is not
 * real. Note which way round the risk runs: the reached site does NOT read the free
 * space at all, only the geometry, so on this path the fabrication is inert. The site
 * at 0x0037D570 DOES read it, and when the run gets there the numbers it is handed
 * are ours.
 *
 * NOT MODELLED: FsInformationClass 5 (1 site, 0x0037D732) is REFUSED with an
 * explicit status and a log line naming the class, not answered with a zero-filled
 * buffer. Its structure is only partially evidenced (the same wrapper reads [buf+0]
 * as filesystem flags, [buf+4] as a maximum component length, [buf+8] as a name byte
 * length and [buf+0xC] as name bytes -- recorded here for the task that models it),
 * and a zero-filled attribute mask is a value the guest would act on.
 *
 * -------- CLASS 1: THE VOLUME-IDENTITY STRUCTURE, DERIVED FROM ITS CONSUMERS --------
 *
 * Both class-1 sites sit in XAPI-shaped wrappers, and between them they read every
 * field this host supplies. The layout below is pinned by those reads, not by an NT
 * header:
 *
 *     offset  width  read at                        meaning
 *     +0x00   8      (never read in this image)     volume creation time, by the NT
 *                                                    convention the 0x18 total implies
 *     +0x08   4      0x0037D045 and 0x0037D780      VOLUME SERIAL NUMBER
 *     +0x0C   4      0x0037D75C                     volume label length, IN BYTES
 *     +0x10   1      (never read in this image)     one-byte flag; its extent is
 *                                                    measured by the label starting
 *                                                    at +0x11 rather than +0x14
 *     +0x11   N      0x0037D765 (copy source)       label bytes, single-byte chars
 *
 * THE STRUCTURE'S SIZE IS 0x18, measured twice as compiler output: the site at
 * 0x0037D02E pushes `Length = 0x18` as a literal, and the other wrapper computes its
 * pool allocation as `label capacity + 0x18` (`add esi, 0x18` at 0x0037D675). Both
 * are what a folded `sizeof` looks like, so a Length below 0x18 is refused with
 * INFO_LENGTH_MISMATCH exactly as class 3 refuses below its own sizeof.
 *
 * THE CONSUMERS, so the contract is the measured one:
 *
 *   - sub_0037D011 (site 0x0037D02E, Length literal 0x18, buffer at ebp-0x94): reads
 *     ONLY +0x08 (`mov eax, [ebp-0x8C]` at 0x0037D045) and stores it at +0x1C of a
 *     13-dword record it assembles -- the GetFileInformationByHandle shape, where
 *     +0x1C is the volume-serial slot.
 *   - sub_0037D5ED (site 0x0037D716, THE REACHED ONE): a GetVolumeInformation shape
 *     taking 8 stack arguments. It doubles its two character counts into byte counts
 *     (0x0037D601/0x0037D609), allocates `2*nVolumeNameSize + 0x18` when a name
 *     buffer was passed or a fixed 0x11C when only the serial was asked for
 *     (0x0037D67D), and calls with class 1. After success it reads +0x0C, refuses the
 *     caller when the label does not fit (cmp at 0x0037D75F), copies the label from
 *     +0x11 with a one-BYTE terminator (0x0037D775), and stores +0x08 through the
 *     caller's serial pointer (0x0037D780/0x0037D783).
 *   - All three in-image callers of that wrapper (0x00024CAE, 0x00025891, 0x000258C0)
 *     pass a NULL name buffer and a non-null serial pointer, so every reached request
 *     uses Length 0x11C and consumes ONLY the serial at +0x08. The two startup calls
 *     store it into per-drive records (0x5655B0 + index*0x30 + 0xC; the literals
 *     0x56576C and 0x56579C are records 9 and 10 -- the U: and T: drives whose opens
 *     T80 made succeed).
 *
 * STATUS CONSUMPTION, measured: the reached wrapper tests the return with
 * `test eax, eax; jl` (0x0037D71F), so ANY negative NTSTATUS -- including the warning
 * 0x80000005 STATUS_BUFFER_OVERFLOW -- takes its error arm. A short-buffer partial
 * answer is therefore a FAILURE to this title, never a value it reads. With the empty
 * label below and the 0x18 minimum enforced, the overflow case has no reachable
 * input, so this host does not carry a live overflow arm; this note is the record a
 * later label source must revisit.
 *
 * WHAT THE FIELDS HOLD, AND WHICH PART IS OURS:
 *
 *   - SERIAL: FABRICATED, fixed, announced and counted. The original value is the
 *     FATX superblock dword at +0x04 of the partition, which the console wrote from
 *     the low half of KeQuerySystemTime when the volume was formatted -- this image's
 *     own cache-format code lays down exactly that superblock ('FATX' at +0,
 *     time-low at +4, at 0x00381574..0x00381598) -- so it is per-console and not
 *     recoverable from the disc. A host directory has no superblock to read. Every
 *     measured consumer only STORES the value, so a fixed value consistent across the
 *     run satisfies all of them. One constant for every volume also matches the
 *     original shape of the reached calls: U: and T: live on the same Partition1
 *     volume on a console, and on this host they resolve to the same `--hdd` backing.
 *   - LABEL LENGTH: 0, and that is the ORIGINAL answer, not a dodge: the FATX
 *     superblock this title itself writes has no label field, so a FATX volume has no
 *     label to report. No label bytes are written and +0x11 onward is left untouched.
 *   - CREATION TIME: written as zero. FABRICATED in the weak sense: nothing in this
 *     image reads it, and the superblock's only timestamp doubles as the serial.
 *   - +0x10: written as zero (the NT convention calls it SupportsObjects, FALSE).
 *
 * IO_STATUS_BLOCK.information on success is 0x11: the fixed part through +0x10 plus
 * zero label bytes, which is the byte count actually transferred.
 */

#ifndef TSFP_XBOX_KERNEL_IO_H
#define TSFP_XBOX_KERNEL_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_hle.h"

/*
 * NT status codes this module needs that `src/xbox/nt_status.h` does not carry.
 * Defined here rather than added there because that header is outside this task's
 * ownership. Module-prefixed so two modules defining the same code cannot collide.
 */

/** The buffer the caller offered is too small for the class it asked for. */
#define KERNEL_IO_STATUS_INFO_LENGTH_MISMATCH 0xC0000004u

/** The class is one we have not derived a structure for. */
#define KERNEL_IO_STATUS_INVALID_INFO_CLASS 0xC0000003u

/**
 * FsInformationClass 3: the 24-byte size/geometry structure derived above.
 *
 * Named by its number rather than by a guessed name, because the number is what was
 * measured. 3 is pushed as a literal at 2 of the 5 call sites.
 */
#define KERNEL_IO_FS_CLASS_SIZE 3u

/** The exact byte count the guest declares for class 3. MEASURED: `push 0x18`. */
#define KERNEL_IO_FS_SIZE_BYTES 0x18u

/**
 * FsInformationClass 1: the volume-identity structure derived above. Named by its
 * number for the same reason class 3 is: the number is what was measured (a literal
 * `push 1` at 0x0037D01C and 0x0037D709).
 */
#define KERNEL_IO_FS_CLASS_VOLUME 1u

/** The structure's sizeof, measured twice as folded compiler output: the literal
 * `push 0x18` at 0x0037D02E and the `add esi, 0x18` allocation at 0x0037D675. A
 * Length below this is refused, exactly as class 3 refuses below its own sizeof. */
#define KERNEL_IO_FS_VOLUME_MIN_BYTES 0x18u

/** Bytes actually transferred for a volume with no label: the fixed part through the
 * one-byte flag at +0x10, plus zero label bytes. This is what goes in
 * IO_STATUS_BLOCK.information. */
#define KERNEL_IO_FS_VOLUME_FIXED_BYTES 0x11u

/**
 * The FABRICATED volume serial, fixed for every volume and every call of a run.
 *
 * 'tsfp' in ASCII, so a dump or a divergence report reads as "somebody chose this",
 * because somebody did -- the same reasoning as the round fabricated unit counts.
 * The original is the per-console FATX superblock dword written at format time and
 * is not recoverable from the disc; every measured consumer only stores the value.
 * See the class-1 section above.
 */
#define KERNEL_IO_FABRICATED_VOLUME_SERIAL 0x74736670u

/*
 * Hard-disk partition geometry. REAL, not fabricated: 32 * 512 == 16384, which is
 * what `sub_00380D0D` requires given this image's InitFlags. See the header comment.
 */
#define KERNEL_IO_HDD_BYTES_PER_SECTOR 512u
#define KERNEL_IO_HDD_SECTORS_PER_UNIT 32u

/** The product the guest validates against. Asserted, not assumed, in the .c file. */
#define KERNEL_IO_HDD_BYTES_PER_UNIT                                                     \
    (KERNEL_IO_HDD_BYTES_PER_SECTOR * KERNEL_IO_HDD_SECTORS_PER_UNIT)

/**
 * What a volume-size query reports for space we do not have.
 *
 * SWITCHABLE AND ANNOUNCED, the same shape as kernel_file.h's missing-file policy,
 * and for the same reason: which of these lets the title get further is an empirical
 * question whose answer belongs in a run log, not in a default chosen here.
 */
typedef enum {
    /* Report a plausible non-empty volume with free space, and SAY SO on every call.
     * The default, because the geometry -- the only part the reached site reads -- is
     * real, and refusing the whole call to avoid fabricating the part it ignores
     * would stop the run for no evidence gained. */
    KERNEL_IO_VOLUME_FABRICATE = 0,
    /* Refuse the query outright. The honest answer when there is no volume: the
     * title's error arm runs instead of its success arm, and the two can be
     * compared. */
    KERNEL_IO_VOLUME_REFUSE,
} kernel_io_volume_policy;

/*
 * =====================================================================
 * ORDINAL 211 NtQueryInformationFile / 226 NtSetInformationFile -- ARITY 5 each
 * =====================================================================
 *
 *     NTSTATUS __stdcall NtQueryInformationFile(
 *             HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
 *             PVOID FileInformation, ULONG Length, ULONG FileInformationClass);
 *
 * `NtSetInformationFile` has the same five in the same order. Both counts were read off
 * the guest's call sites -- 211 at 9 sites, 226 at 14 -- and both match the measured
 * table. ORDER pinned by Length/Class pairs that match structure sizes exactly, which
 * is the same kind of evidence `Length = 0x18` gives for ordinal 218:
 *
 *     class  Length  what the pair implies            seen at
 *     0x0E   8       a single 64-bit file position    many
 *     0x22   0x38    a 56-byte open-information block 0x00380E05
 *     0x0D   1       a single BOOLEAN                 0x003801B2
 *     4      0x28    a 40-byte basic-information block
 *     0x14   8       a single 64-bit end-of-file
 *
 * 0x003801B2 is the decisive one for the ORDER: it pushes `Length = 1` with
 * `class = 0x0D` and the byte it passes was just written by
 * `mov byte ptr [ebp+0xb], 1`. A one-byte buffer cannot be anything but the
 * information, so Length and FileInformation cannot be transposed.
 *
 * -------- CLASS 0x22's LAYOUT, AND WHICH PART IS MEASURED --------
 *
 * `Length = 0x38` is 56. MEASURED at 0x00380E05, and the guest then reads the buffer
 * back as a 64-bit quantity at +0x28:
 *
 *     0x00380E11  cmp dword ptr [ebp - 0x2c], ebx   ; buffer +0x28, ebx = 0
 *     0x00380E16  cmp dword ptr [ebp - 0x28], ebx   ; buffer +0x2C
 *
 * Both halves against zero, which is a "is this file empty" test. So +0x28 being a
 * 64-bit END-OF-FILE is MEASURED. The rest of the 56 bytes is INFERRED from the total
 * and from convention: four 64-bit timestamps at +0x00, +0x08, +0x10 and +0x18, a
 * 64-bit allocation size at +0x20, and 32-bit attributes at +0x30 with 4 bytes of tail.
 * 8*4 + 8 + 8 + 4 + 4 = 56, which is the only partition of 56 consistent with a 64-bit
 * field landing on +0x28. Said plainly because a later task may need the timestamps and
 * should know they rest on convention, not on this image.
 *
 * XDVDFS HAS NO PER-FILE TIMESTAMPS -- the format carries one creation stamp for the
 * whole volume and nothing per entry. So for a disc-backed file the size and the
 * attributes are REAL, read from the directory entry, and the four timestamps are
 * FABRICATED zeros and announced as such.
 *
 * =====================================================================
 * ORDINAL 219 NtReadFile -- ARITY 8, AND THE MEASURED TABLE SAYS 6
 * =====================================================================
 *
 *     NTSTATUS __stdcall NtReadFile(
 *             HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
 *             PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
 *             PLARGE_INTEGER ByteOffset);
 *
 * EIGHT stack arguments, 32 bytes, at all 8 call sites. `kernel_arity.inc` says SIX,
 * and that is the most dangerous single number encountered in this work: our handler is
 * the __stdcall callee and must pop what the caller pushed, so believing 6 would leave
 * 8 bytes of arguments on the guest's stack and desync `esp` for the rest of the run.
 * An `ABI_TABLE` row in src/host/kernel_thunk.c overrides it, with the reasoning there.
 *
 * WHY THE SCANNER IS WRONG, since "the table is wrong" deserves a mechanism. It takes
 * the MINIMUM across sites, on the premise that over-counting is its only dangerous
 * error. At site 0x003DF10B the lifter's `_icall_esp` bracket opens two pushes late, so
 * `push edi` (Length) and `push 0x3E6428` (ByteOffset) fall outside the bracket and that
 * site tallies 6 while pushing 8. Per-site tallies are {8,8,8,8,6,10}; the minimum is
 * the single wrong number in the set.
 *
 * ORDER pinned at 0x0037D8B6, which is worth quoting because it names six of the eight:
 *
 *     lea eax,[ebp-0x18];  push eax   ; arg7 ByteOffset -> a LARGE_INTEGER local
 *     push 0x200                      ; arg6 Length
 *     lea eax,[ebp-0x22c]; push eax   ; arg5 Buffer -- a 0x200-byte local, matching
 *     lea eax,[ebp-0x20];  push eax   ; arg4 IoStatusBlock
 *     push esi; push esi; push esi    ; arg3 ApcContext, arg2 ApcRoutine, arg1 Event
 *     push dword ptr [ebp-8]          ; arg0 FileHandle
 *     mov dword ptr [ebp-0x18], 0x800 ; ByteOffset.LowPart  = 0x800
 *     mov dword ptr [ebp-0x14], esi   ; ByteOffset.HighPart = 0
 *
 * `Length = 0x200` and a `0x200`-byte stack buffer is the pairing that makes Buffer and
 * Length unmistakable, and the 64-bit store into the thing arg7 points at is what makes
 * ByteOffset a POINTER to a 64-bit offset rather than an offset. The Event/Apc trio is
 * NULL at 7 of the 8 sites.
 *
 * EVENT/IOSB ERROR BOUNDARIES (T1011, grounded in T1007's 76 xemu guest records):
 * positive-length zero-byte EOF returns STATUS_END_OF_FILE, clears the supplied Event,
 * and leaves the caller's IOSB unchanged. Immediate invalid file/Event/wrong-kind Event
 * validation also leaves the IOSB unchanged and does not change Event state. Zero-length
 * EOF and positive partial success remain successful completions. T1011 does not
 * generalize these observations to accepted asynchronous failures or cancellation; the
 * measured disc completion/clear race remains outside this host contract.
 *
 * Asynchronous handling is opt-in. A non-null ApcRoutine is REPORTED AND NOT CALLED --
 * queueing an APC we have no mechanism to deliver would be worse than saying so.
 *
 * =====================================================================
 * ORDINAL 236 NtWriteFile -- NOW BOUND. AND TWO MORE THAT ARE NOT
 * =====================================================================
 *
 * Each has a hand-verified arity recorded below, because an arity is cheap to establish
 * once and expensive to re-establish later, and because an ARITY is a different claim
 * from an IMPLEMENTATION. 236 now has a handler; 87 and 81 do not, for reasons that
 * differ per ordinal. 87 and 81 are absent from the boot trace, which is MEASURED
 * and not assumed. 236 is present: the boot makes three writes.
 *
 * ---------------------------------------------------------------------
 * ORDINAL 236: NtWriteFile -- ARITY 8, AND IT NOW HAS A HANDLER
 * ---------------------------------------------------------------------
 *
 *     NTSTATUS __stdcall NtWriteFile(
 *             HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
 *             PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
 *             ULONG Length, PLARGE_INTEGER ByteOffset);   // 8 stack args, 32 bytes
 *
 * ARITY-OK(236): EIGHT stack arguments, identical to ordinal 219. The measured row
 * `{236u, 6u, 11u, 0}` is non-unanimous and therefore already refused; an ABI_TABLE row
 * in src/host/kernel_thunk.c carries the 8. FIVE independent lines agree and none
 * dissents, which makes this stronger than the 219 row it mirrors:
 *
 *   1. ALL 11 SITES HAND-COUNTED AT 8, unanimously -- where 219's hand tallies were
 *      {8,8,8,8,6,10} with outliers in both directions, 236's are 8 eleven times.
 *   2. THE SITE SET IS PROVED EXHAUSTIVE. The little-endian literal of thunk slot
 *      0x004757B8 occurs exactly 11 times in the 6.27 MB image and every occurrence is
 *      the operand of a `call dword ptr`. No `jmp [slot]` stub, no register-indirect
 *      site. The ordinal-24 failure mode (all 12 sites behind a stub) cannot apply.
 *   3. ESP BALANCE IS UNIQUELY CONSISTENT AT 8, swept over arity hypotheses 5..10
 *      across all seven containing functions. The sharpest case is `FUN_003df0a0`,
 *      which has NO FRAME POINTER -- there is no `leave` to paper over an esp error, so
 *      the depth at each of its five `ret`s must be exactly zero, and it is zero only
 *      at 8 (-12, -8, -4, 0, +4, +8 for 5..10). Independent of any push counting.
 *   4. ONE FUNCTION CALLS BOTH 219 AND 236 WITH A BYTE-FOR-BYTE IDENTICAL ARGUMENT
 *      SHAPE. `FUN_003df0a0` calls NtReadFile at 0x003DF10B -- the very site the
 *      ARITY-OK(219) note names as the one that tallied 6 -- and NtWriteFile at
 *      0x003DF375, with the same ByteOffset global 0x3E6428, the same IoStatusBlock
 *      global 0x3E6420, the same FileHandle global 0x3E6434, the same Buffer source
 *      [esi+0x8DC], and the identical three-instruction round-up-to-512 Length. This
 *      pins 236's arity to 219's hand-verified 8 directly.
 *   5. THE ARGUMENT NAMES COME FROM TWO PUBLIC CONTRACTS INDEPENDENT OF THIS IMAGE.
 *      Sites 0x0037CD44 and 0x0037CD7D are both inside Win32 `WriteFile` (FLIRT-named,
 *      and its `ret 0x14` matches the documented five-argument prototype). The
 *      synchronous path forwards hFile/lpBuffer/nNumberOfBytesToWrite directly, and the
 *      overlapped path at 0x0037CD44 reads every documented OVERLAPPED field at its
 *      documented offset -- Internal(+0), InternalHigh(+4), Offset(+8), OffsetHigh(+0xC),
 *      hEvent(+0x10) -- building a 64-bit pair at [ebp-0x10] and pushing its ADDRESS as
 *      arg7. That is what makes ByteOffset a POINTER rather than an offset, and it names
 *      Event, ApcRoutine, ApcContext and IoStatusBlock at the same time.
 *
 * A SECOND SCANNER UNDER-COUNT MECHANISM, distinct from 219's late bracket and worth
 * recording because it will recur. At sites 0x00380E3E and 0x003810BF an argument is
 * pushed BEFORE AN INTERVENING STDCALL CALL:
 *
 *     0x00380E29  push ebx                ; arg7 ByteOffset = NULL -- SURVIVES the call
 *     0x00380E2A  push dword ptr [ebp+8]  ; the one argument to XGetSectionSize
 *     0x00380E2D  call 0x37C9BA           ; XGetSectionSize -- `ret 4`, pops only that
 *     0x00380E32  push eax                ; arg6 Length = the size just returned
 *
 * The callee pops exactly one dword, so `push ebx` is still live when NtWriteFile is
 * entered -- and no bracket that opens at a basic-block boundary can see it. Those two
 * sites tally 7. Full per-site set {8,8,8,7,7,8,8,8,8,6,8}; the minimum is 6, which is
 * exactly what the measured table publishes.
 *
 * WHICH SITE THE BOOT IS ABOUT TO REACH, AND WHY 236 WAS NOT WHAT BLOCKED IT.
 *
 * The note this block replaced predicted the write was near-term and named
 * `_XapiCopySectionToFile@16` (0x00380D85, site 0x00380E3E) as the likely one. READING THE
 * LIFTED CODE SHOWS A DIFFERENT SITE, and the correction matters because the two sit in
 * different functions:
 *
 *   - Trace calls 122-124 are NtCreateFile from 0x00381057, NtQueryInformationFile from
 *     0x00381079 and XeLoadSection from 0x0037C987. All three are inside
 *     `XapiMapLetterToDirectory` (sub_00380E6C, 0x00380E6C..0x0038119F) or a leaf it
 *     calls, NOT inside `_XapiCopySectionToFile@16`. The create at 0x00381051 is the one
 *     that makes `UDATA\45410066\TitleMeta.xbx`, with DesiredAccess 0x40100000 and the
 *     handle landing in `[ebp+0x14]`.
 *   - The class-0x22 query at 0x00381073 then tests the file's 64-bit end-of-file at
 *     buffer +0x28 against zero (0x00381084 and 0x0038108D). OUR FILE IS EMPTY, so both
 *     halves are zero and the branch FALLS THROUGH to 0x00381096.
 *   - 0x00381096 compares `[ebp+0x10]` against 0xFFFFFFFF. It is not equal, so the run
 *     takes 0x0038109C: `call sub_0037C97B([ebp+0x10])`, and THAT is the function holding
 *     the XeLoadSection call at 0x0037C987 the run stops on.
 *   - `sub_0037C97B` returns `MEM32(section + 4)` when XeLoadSection succeeds and 0 when
 *     it does not. `sub_0037C9BA` at 0x003810AE returns `MEM32(section + 8)` and contains
 *     NO thunk -- it is two instructions and a `ret 4`. Those two offsets are an XBE
 *     section header's VirtualAddress and VirtualSize.
 *   - So the next kernel call after 327 returns a non-zero base is NtWriteFile at
 *     0x003810BF, with Buffer = the section's loaded base, Length = its VirtualSize, and
 *     ByteOffset = NULL (the `push ebx` at 0x003810AA, which survives the intervening
 *     `ret 4`). THERE IS NO KERNEL CALL IN BETWEEN.
 *
 * MEASURED AFTER ORDINAL 327 LANDED: with this handler bound the boot makes THREE
 * NtWriteFile calls and the three files are non-empty. TitleMeta.xbx 152 bytes (call 125,
 * from 0x003810C5), TitleImage.xbx 10240 bytes (call 132, from 0x00380E44) and SaveImage.xbx
 * 4096 bytes (call 139, from 0x00380E44). Each is BYTE-IDENTICAL to the XBE section it was
 * copied from ($$XTINFO, $$XTIMAGE, $$XSIMAGE), compared against the user's own executable.
 * Before 327 landed the run stopped one call short of the write, so 236 was never what made
 * the file empty: 327 was. Before the write path landed the run stopped ON 236.
 *
 * ---------------------------------------------------------------------
 * ORDINAL 87: IofCompleteRequest -- FASTCALL, TWO REGISTER ARGS, NO HANDLER
 * ---------------------------------------------------------------------
 *
 *     VOID __fastcall IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
 *                                       -- Irp in ECX, PriorityBoost in DL
 *
 * ARITY-OK(87): ZERO stack arguments and TWO REGISTER arguments. The measured row
 * `{87u, 0u, 9u, 1}` is unanimous over 9 sites and would be ACCEPTED as it stands --
 * and that is the problem, because a measured row carries no CONVENTION. Accepting the
 * zero makes the dispatcher pop the right number of bytes (zero) for the wrong reason,
 * and the next handler written against it would reach for `kernel_frame_arg(0)` and get
 * the return address. So this ordinal wants an explicit `THUNK_CC_FASTCALL` row even
 * though nothing is numerically wrong. See src/xbox/kernel_call.h.
 *
 * MEASURED at all 9 sites (thunk slot 0x004758CC, 9 image-wide occurrences, every one a
 * `call dword ptr`): zero pushes, ECX loaded with a PIRP, and -- answering the question
 * directly -- ALL NINE SET EDX, every one of them with an 8-bit `xor dl, dl`. ECX is
 * provably the IRP: every site writes an NTSTATUS to [ecx+0x10] and a byte count to
 * [ecx+0x14], i.e. IoStatus.Status and IoStatus.Information, and 0x0046E56F does
 * `mov eax,[ecx+0x5C]; or byte ptr [eax+3],1` -- an inlined IoMarkIrpPending through
 * Tail.Overlay.CurrentStackLocation.
 *
 * THE PRIORITY BOOST IS A BYTE, and one site proves it rather than suggesting it:
 *
 *     0x0046ED6F  mov  edx, dword ptr [esp+8]    ; EDX = the NTSTATUS parameter, LIVE
 *     0x0046ED7C  mov  ecx, dword ptr [esi+0x10]
 *     0x0046ED91  mov  dword ptr [ecx+0x10], edx ; last use of the 32-bit EDX
 *     0x0046ED94  xor  dl, dl                    ; clobber ONLY the low byte
 *     0x0046ED96  call dword ptr [0x4758CC]
 *
 * The compiler had a live 32-bit value in EDX and destroyed exactly one byte of it
 * immediately before the call. Nothing clobbers DL for no reason, and clobbering only
 * DL means the parameter is byte-wide. CONSEQUENCE FOR ANY FUTURE HANDLER: `frame->edx`
 * MUST BE MASKED TO 8 BITS. At 0x0046ED96 the upper 24 bits are the top three bytes of
 * an NTSTATUS, so an unmasked read yields a garbage PriorityBoost.
 *
 * AND AN ESP-BALANCE PROOF that needs no push counting. `sub_0046E56F` has two mutually
 * exclusive branches reconverging at 0x0046E5B7: one calls ordinal 87 with ZERO pushes,
 * the other calls ordinal 83 `IoStartPacket` with THREE. Both must reach the shared
 * epilogue `pop edi; pop esi; pop ebx; pop ebp; ret 8` at the same esp, and there is no
 * `add esp` anywhere -- so 87 pops 0 and 83 pops 12.
 *
 * WHY THERE IS NO HANDLER. There is no IRP model in this tree. The guest builds its own
 * IRPs and DEVICE_OBJECTs via `IoCreateDevice` (the XAPI memory-unit driver at
 * `sub_0046c904`), and what IofCompleteRequest still has to do after the caller has
 * already filled in IoStatus is run the completion routines in the IRP's stack and
 * unqueue it. That needs the IRP layout, and guest_structs.h does not derive it. A
 * VOID no-op would SILENTLY DROP a completion and hang whatever waits on it, which is
 * exactly the plausible-wrong-answer failure this project refuses. Deriving the layout
 * is a measurement task, not a guess, and it has not been done.
 *
 * NOT ESTABLISHED, on the record: the RETURN convention. All 9 sites ignore EAX, so the
 * call sites cannot distinguish VOID from a value-returning form.
 *
 * ---------------------------------------------------------------------
 * ORDINAL 81: IoStartNextPacket -- ARITY 1, NOT 2, AND NO HANDLER
 * ---------------------------------------------------------------------
 *
 *     VOID __stdcall IoStartNextPacket(PDEVICE_OBJECT DeviceObject);   // 1 stack arg
 *
 * ARITY-OK(81): ONE stack argument. The measured row `{81u, 1u, 8u, 1}` is unanimous
 * over 8 sites and is CORRECT, so no ABI_TABLE row is needed. It is recorded here
 * because the number looks wrong to anyone checking against NT: desktop
 * `IoStartNextPacket(PDEVICE_OBJECT, BOOLEAN Cancelable)` takes TWO, so the obvious
 * conclusion is that the scanner under-counted at all 8 sites identically. It did not.
 *
 * MEASURED at all 8 sites (thunk slot 0x004758D0, 8 image-wide occurrences, every one a
 * `call dword ptr`): exactly one push each, NO site pushes a 0/1 literal, and NO site
 * loads ECX. At all 8 the instruction before the push is a `call` -- ordinal 87's at
 * six of them -- which destroys ECX, and ECX is never reloaded. So it is not
 * fastcall-like despite sitting beside `Iof*` exports, and the single push is its only
 * argument.
 *
 * WHY THE NT TWO-ARGUMENT FORM DOES NOT APPLY: THE XBOX I/O MANAGER HAS NO IRP
 * CANCELLATION. In the same driver, ordinal 83 `IoStartPacket` is called with THREE
 * pushes rather than NT's four -- `push 0` (Key), `push ecx` (Irp), `push edi`
 * (DeviceObject) at 0x0046E5B1 -- so it has lost `PDRIVER_CANCEL CancelFunction`. The
 * same amputation removes `BOOLEAN Cancelable` from 81. Ordinal 82
 * `IoStartNextPacketByKey` exists separately, so the Key-taking variant is a different
 * export and not a reason for 81 to take two. XboxDev/nxdk's CC0 header agrees
 * independently: `IoStartNextPacket(IN PDEVICE_OBJECT DeviceObject)`, and its DEF file
 * decorates it `IoStartNextPacket@4`.
 *
 * ESP BALANCE SETTLES 81 AND 87 TOGETHER in one 62-byte function, `sub_0046E5C9`:
 * `push esi` ... `mov ecx,[esi+0x10]; xor dl,dl; call [0x4758CC]` (87, no pushes) ...
 * `push dword ptr [esi]; call [0x4758D0]` (81) ... `pop esi; ret 8`. The save and
 * restore cancel, the one push is the only other stack motion, and there is no
 * `add esp`. For `pop esi` to restore ESI and `ret 8` to find its return address, 81
 * must pop exactly 4 and 87 exactly 0. A two-argument 81 would pop 8 and the function
 * would return into garbage.
 *
 * THE ARGUMENT IS THE DEVICE_OBJECT, pinned by a literal in the driver's own init
 * (`MU_Init`, sub_0046c904): after `IoCreateDevice(..., &DeviceObject)` it does
 * `extension[0x00] = DeviceObject`, so `[esi]` at the seven indirect sites is the
 * extension's back-pointer. Site 0x0046F0A2 proves it a second way without needing
 * MU_Init: `sub_0046F071` is a dispatch routine where `[ebp+0xC]` is unambiguously the
 * IRP (`mov [edi+0x10], 0xC0000240`), so `[ebp+8]` is the DeviceObject, and there the
 * one push is `push ebx` -- the DeviceObject DIRECTLY.
 *
 * WHY THERE IS NO HANDLER: same reason as 87. Starting the next packet means pulling an
 * IRP off the device's queue and calling the driver's StartIo routine, which needs both
 * the DEVICE_OBJECT and IRP layouts. A no-op would stall the device queue forever.
 *
 * ONE MORE ARITY ESTABLISHED IN PASSING, recorded so it is not re-derived: ordinal 83
 * `IoStartPacket` takes THREE stack arguments (DeviceObject, Irp, Key), hand-counted at
 * 0x0046E5B1. Its measured row `{83u, 3u, 1u, 1}` agrees on the number but has ONE
 * voter, so `stack_args_for()` refuses it and 83 will need its own ABI_TABLE row before
 * it can ever be dispatched.
 */

/*
 * ORDINAL 210 NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION),
 * stdcall, TWO arguments, is bound here beside 211 because it answers the SAME 0x38-byte
 * structure as class 0x22 of NtQueryInformationFile, by name instead of by handle. The name is
 * resolved by `kernel_file_query_attributes`; the arity derivation and the measured layout are in
 * the ARITY-OK(210) comment in kernel_io.c.
 */

/** FileInformationClass values this module answers. Measured; see above. */
#define KERNEL_IO_FILE_CLASS_INTERNAL 0x06u
#define KERNEL_IO_FILE_CLASS_POSITION 0x0Eu
#define KERNEL_IO_FILE_CLASS_NETWORK_OPEN 0x22u
/* Allocation/EOF are reached by the retail profile-save wrapper (T951).
 * Earlier 145-call boot evidence did not reach them; it is not a live-flow limit. */
#define KERNEL_IO_FILE_CLASS_ALLOCATION 0x13u
#define KERNEL_IO_FILE_CLASS_END_OF_FILE 0x14u

/** Byte sizes the guest declares for those classes. MEASURED literals. */
#define KERNEL_IO_FILE_POSITION_BYTES 8u
#define KERNEL_IO_FILE_INTERNAL_BYTES 8u
#define KERNEL_IO_FILE_NETWORK_OPEN_BYTES 0x38u
#define KERNEL_IO_FILE_END_OF_FILE_BYTES 8u
#define KERNEL_IO_FILE_ALLOCATION_BYTES 8u

/** Register this module's ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_io_register(void);

/** How many reads were served, and how many bytes in total. */
unsigned kernel_io_read_count(void);
uint64_t kernel_io_bytes_read(void);

/**
 * How many writes were served, and how many bytes in total.
 *
 * `kernel_io_bytes_written()` counts bytes that REACHED THE FILESYSTEM, including the
 * partial count from a write that then failed. It never counts bytes a caller asked for,
 * because a report that did would say the title saved its data on a run where every byte
 * was refused.
 */
unsigned kernel_io_write_count(void);
uint64_t kernel_io_bytes_written(void);

/**
 * How many writes, or end-of-file sets, came back refused or short.
 *
 * Counted separately from the byte total on purpose: zero bytes written is the same number
 * whether the title wrote nothing or whether every write it attempted was refused, and
 * those are different findings.
 */
unsigned kernel_io_write_refused_count(void);

/** How many information queries/sets named a class whose structure is not derived. */
unsigned kernel_io_unknown_file_class_count(void);

/**
 * How many NETWORK_OPEN answers (class 0x22 of 211, and ordinal 210) reported FABRICATED zero
 * timestamps, because the object has no per-object time to report (a disc entry, an empty
 * file). A host-directory object reports the host's own times and is not counted.
 */
unsigned kernel_io_fabricated_timestamp_count(void);

/** How many reads were asked to run asynchronously via a non-null ApcRoutine. */
unsigned kernel_io_apc_ignored_count(void);

/** Forget every counter and return to the default policy. For tests. */
void kernel_io_reset(void);

/* Per-request query refusing fabricated space; preserves global policy. */
uint32_t kernel_io_query_volume_stored(void *context);
/* Synchronous actual stored-byte read; false is source/invocation refusal,
 * never a fabricated guest status. Preserves default/async read policies. */
bool kernel_io_read_file_stored(void *context, uint32_t *status);

/** Choose what a volume-size query does about space we do not have. */
void kernel_io_set_volume_policy(kernel_io_volume_policy policy);

/** How many volume-size queries were answered with FABRICATED free space. */
unsigned kernel_io_fabricated_geometry_count(void);

/** NtFlushBuffersFile (198) calls completed on an open file. */
unsigned kernel_io_flush_count(void);

/** NtQueryDirectoryFile (207) calls that returned a directory entry. */
unsigned kernel_io_directory_entry_count(void);

/** How many volume-identity queries were answered with the FABRICATED fixed serial. */
unsigned kernel_io_fabricated_identity_count(void);

/** How many volume queries were refused, for any reason. */
unsigned kernel_io_volume_refused_count(void);

/** How many volume queries named a class whose structure is not derived. */
unsigned kernel_io_unknown_class_count(void);

/** How many device/file-system control requests were ANSWERED (196 and 200 together). */
unsigned kernel_io_control_count(void);

/** How many were REFUSED: a handle that is not a cache partition's raw view, or a code
 * with no derived answer. Separate from the answered count so a refused format reads as
 * one rather than as a title that never asked. */
unsigned kernel_io_control_refused_count(void);

/**
 * The bytes-per-allocation-unit this host reports for a hard-disk volume.
 *
 * Exposed so a test can assert the product the guest checks WITHOUT restating the
 * factors, which is the mistake that would let both drift together.
 */
uint32_t kernel_io_hdd_bytes_per_unit(void);

#endif /* TSFP_XBOX_KERNEL_IO_H */
