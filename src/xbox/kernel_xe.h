/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XBE section residency: ordinals 327 XeLoadSection and 328 XeUnloadSection.
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED. 327 is XeLoadSection and 328 is
 * XeUnloadSection, read out of `tools/kernel_ordinals.py` (lines 385-386), which is
 * the single source of truth derived from the kernel export table. Neither is in
 * `SUSPECT_ON_XDK_5849`.
 *
 * WHAT THESE DO ON HARDWARE. An XBE section carries a reference count and a PRELOAD
 * flag. Sections with PRELOAD are resident from image load; the rest are demand
 * paged, and the title brings one in with XeLoadSection and releases it with
 * XeUnloadSection. The first load commits the pages and reads the section body out of
 * the XBE file, the last unload decommits them. Everything in between is reference
 * counting.
 *
 *     NTSTATUS __stdcall XeLoadSection(PXBE_SECTION_HEADER Section);
 *     NTSTATUS __stdcall XeUnloadSection(PXBE_SECTION_HEADER Section);
 *
 * WHY THIS IS THE NEXT THING THE GUEST WANTS. The boot stopped here at 124 kernel
 * calls, on thread 2 at guest 0x0037C987 -- which is the RETURN address; the call is
 * at 0x0037C981. It arrives immediately after the section-name scan that ordinal 279
 * RtlEqualString serves, calls 28..56 of the trace, and the scan's 14th comparison is
 * the one that returns 1.
 *
 * =========================================================================
 * THE MEASURED ARITY FOR 327 IS WRONG, AND THE WRONG NUMBER IS KNOWABLE
 * =========================================================================
 *
 * `generated/lifted/gen/kernel_arity.inc` carries:
 *
 *     {327u, 2u, 1u, 1},   -- TWO arguments, ONE site, flagged unanimous
 *     {328u, 1u, 1u, 1},   -- ONE argument,  ONE site, flagged unanimous
 *
 * Both flags are tautologies: over a single voter, agreement is self-agreement. Both
 * are refused by `stack_args_for()` in src/host/kernel_thunk.c for having fewer than
 * MEASURED_ARITY_MIN_SITES sites, which is the refusal doing its job -- because 327's
 * published 2 IS WRONG. The real count is ONE for both, and the reason 327 over-counts
 * while its neighbour 328 does not is visible in the instruction stream.
 *
 * THE TWO SITES, WHICH ARE ADJACENT FUNCTIONS AND DIFFER IN EXACTLY ONE WAY.
 *
 * sub_0037C97B, one site, FLIRT shape `_XLoadSectionByHandle@4`:
 *
 *     0x0037C97B  push esi                  ; CALLEE-SAVED REGISTER, not an argument
 *     0x0037C97C  mov  esi, [esp + 8]       ; this function's OWN argument, hSection
 *     0x0037C980  push esi                  ; the ONE argument to XeLoadSection
 *     0x0037C981  call dword ptr [0x475788] ; -> XeLoadSection
 *     0x0037C987  test eax, eax             ; NTSTATUS
 *     0x0037C989  jge  0x37C995
 *     0x0037C98B  push eax; call 0x37E9FD   ; SetLastError-alike, itself `ret 4`
 *     0x0037C991  xor  eax, eax             ; failure -> return NULL
 *     0x0037C993  jmp  0x37C998
 *     0x0037C995  mov  eax, [esi + 4]       ; success -> Section->VirtualAddress
 *     0x0037C998  pop  esi
 *     0x0037C999  ret  4
 *
 * sub_0037C99C, one site, FLIRT shape `_XUnloadSectionByHandle@4`:
 *
 *     0x0037C99C  push dword ptr [esp + 4]  ; the ONE argument, forwarded in place
 *     0x0037C9A0  call dword ptr [0x47578C] ; -> XeUnloadSection
 *     0x0037C9A6  test eax, eax
 *     0x0037C9A8  jge  0x37C9B4
 *     0x0037C9AA  push eax; call 0x37E9FD
 *     0x0037C9B0  xor  eax, eax             ; failure -> FALSE
 *     0x0037C9B2  jmp  0x37C9B7
 *     0x0037C9B4  xor  eax, eax; inc eax    ; success -> TRUE
 *     0x0037C9B7  ret  4
 *
 * THE ONE DIFFERENCE IS THE ONLY DIFFERENCE THAT MATTERS. 327's site opens with a
 * callee-saved `push esi` because it needs the handle again after the call, to read
 * `[esi+4]`. 328's site never needs its argument again, so it forwards it with
 * `push [esp+4]` and saves nothing. `tools/lift/callsites.py` brackets an `_icall_esp`
 * window on basic-block boundaries, and a function entry IS a block head, so at
 * 0x0037C97B the window opens on the prologue and tallies BOTH pushes -- 2. At
 * 0x0037C99C there is no prologue to swallow and it tallies 1, correctly.
 *
 * This is a THIRD distinct scanner miscount mechanism, and it is the first that is
 * an OVER-count rather than an under-count. The two already on the record both lose
 * pushes: the late bracket at ordinal 219 (src/xbox/kernel_io.h) and the push hoisted
 * above a branch at ordinal 340 (src/xbox/kernel_crypto.h). Recorded here because the
 * `minimum across sites` rule that fixes those two would make this one WORSE, and
 * because a single-site over-count has no outlier for a minimum rule to discard.
 *
 * -------------------------------------------------------------------------
 * ARITY-OK(327): ONE stack argument. THREE independent confirmations.
 * -------------------------------------------------------------------------
 *
 * (1) A FORCED STACK BALANCE DERIVES THE 1 RATHER THAN COUNTING IT, which is the
 *     ordinal 279 standard. sub_0037C97B contains NO `add esp` on any path, one
 *     `pop esi`, and ends `ret 4`. Let esp be E at entry, so [E] is the return
 *     address and [E+4] the argument.
 *
 *         push esi            -> esp = E-4
 *         push esi            -> esp = E-8
 *         call                -> esp = E-12  (the CALL pushes the return address)
 *         XeLoadSection pops the return address and N argument dwords
 *                             -> esp = E-12 + 4 + 4N = E-8 + 4N
 *         pop esi at 0x0037C998 must restore the register saved at [E-4],
 *         so it requires esp = E-4   =>   E-8 + 4N = E-4   =>   N = 1.
 *
 *     At N = 2 the `pop esi` loads THE RETURN ADDRESS into esi and `ret 4` jumps to
 *     whatever the caller's argument happened to be. The error arm balances too: the
 *     only thing on it is `push eax; call 0x37E9FD`, and sub_0037E9FD ends `ret 4`,
 *     so it pops its own argument and the arm is esp-neutral.
 *
 * (2) THE ARGUMENT FETCH PROVES WHICH PUSH IS THE PROLOGUE. `mov esi, [esp+8]` at
 *     0x0037C97C reads this function's own argument. A stdcall callee's argument is
 *     at [esp+4] on entry, so reaching it at [esp+8] means EXACTLY ONE push has
 *     happened since entry. That pins 0x0037C97B as the callee-save and leaves
 *     0x0037C980 as the only argument push. This is a direct reading of the
 *     instruction that the scanner's block-aligned window cannot make.
 *
 * (3) CORROBORATED BY A COMMITTED DECORATION, FROM A DIFFERENT KERNEL BUILD.
 *     nxdk's `lib/xboxkrnl/xboxkrnl.exe.def` (CC0-1.0, verdicted USE in
 *     docs/provenance.md) lists at lines 334-335:
 *
 *         XeLoadSection@4     @ 327 NONAME
 *         XeUnloadSection@4   @ 328 NONAME
 *
 *     The MSVC `@4` is the argument BYTE count, so one dword each, and the absence of
 *     a leading `@` makes both __stdcall rather than __fastcall. READ FOR
 *     CORROBORATION ONLY and not sufficient on its own: that file describes kernel
 *     builds 3944/4039 and this title is XDK 5849. It agrees with (1) and (2), and it
 *     DISAGREES with the measured table -- which is the useful part, since (1) and (2)
 *     are both readings of this binary and (3) is not.
 *
 * -------------------------------------------------------------------------
 * ARITY-OK(328): ONE stack argument. The measured 1 is right, for a reason.
 * -------------------------------------------------------------------------
 *
 * A row is still needed, because one site is below MEASURED_ARITY_MIN_SITES and
 * `stack_args_for()` refuses it regardless of whether it happens to be correct. The
 * same forced balance derives it. sub_0037C99C has NO prologue, NO `pop`, and NO
 * `add esp`:
 *
 *         push [esp+4]        -> esp = E-4
 *         call                -> esp = E-8
 *         callee pops         -> esp = E-8 + 4 + 4N = E-4 + 4N
 *         `ret 4` at 0x0037C9B7 requires esp = E   =>   N = 1.
 *
 * And the def file says `@4`. The CONTRAST with 327 is itself evidence: the same
 * scanner, the same bracket rule, two adjacent one-site functions, and the one whose
 * tally is right is exactly the one with nothing in its prologue to miscount.
 *
 * THUNK SLOTS, AND WHY `1 site` IS COMPLETE HERE. 0x475788 (327) and 0x47578C (328)
 * each occur exactly once in the disassembly, as the `call dword ptr` above. No
 * register-indirect call, no `jmp [slot]` import stub. So for these two the scanner
 * has no blind spot of the kind that made ordinal 47's real site count 12 rather
 * than 10, and the site set really is exhaustive.
 *
 * =========================================================================
 * THE GUEST STRUCT, MEASURED FROM THIS IMAGE'S OWN BYTES
 * =========================================================================
 *
 * A `handle` here is NOT an object-manager handle. It is a guest pointer straight at
 * an XBE_SECTION_HEADER inside the mapped image headers, and this is measured, not
 * assumed. `_XGetSectionHandleA@4` at sub_0037C914 builds it:
 *
 *     0x0037C92C  mov  esi, [0x1011C]   ; XBE_HEADER.NumberOfSections
 *     0x0037C932  mov  edi, [0x10120]   ; XBE_HEADER.SectionHeadersAddress
 *     0x0037C938  imul esi, esi, 0x38   ; stride = sizeof(XBE_SECTION_HEADER)
 *     0x0037C93B  add  esi, edi         ; one past the last header
 *     0x0037C93F  push [edi + 0x14]     ; SectionNameAddress -> RtlInitAnsiString
 *      ...        call RtlEqualString
 *     0x0037C95C  add  edi, 0x38        ; next header
 *     0x0037C977  mov  eax, edi         ; the MATCHED HEADER POINTER is the handle
 *     0x0037C96D  or   eax, 0xFFFFFFFF  ; no match -> -1
 *
 * Read straight out of `build/default.xbe`, the absolute addresses the guest uses
 * decode exactly:
 *
 *     file 0x104  BaseAddress              0x00010000   (so VA == file offset + base
 *                                                        throughout the header region)
 *     file 0x108  SizeOfHeaders            0x00002000
 *     file 0x11C  NumberOfSections         0x00000017   = 23
 *     file 0x120  SectionHeadersAddress    0x00010370
 *
 * 23 * 0x38 = 0x508, so the table runs 0x00010370..0x00010878 and sits well inside
 * the 0x2000 bytes of headers that `xbe_map()` copies to BaseAddress. The table is
 * therefore LIVE GUEST MEMORY, which is what makes the reference count below real.
 *
 * FIELD OFFSETS. Three are MEASURED by guest instructions that read them, and the
 * stride is measured by the `imul ... 0x38` above:
 *
 *     +0x04  VirtualAddress    read at 0x0037C995, and sub_0037C97B RETURNS it
 *     +0x08  VirtualSize       read at 0x0037C9BE by `_XGetSectionSize@4`
 *     +0x14  SectionNameAddress  read at 0x0037C93F
 *
 * +0x18 SectionReferenceCount is NOT read by any instruction in this image, so it is
 * INFERRED from the published XBE layout. It is not a bare inference: its NEIGHBOURS
 * are pinned by an arithmetic structure that cannot arise by coincidence. Dumping
 * +0x18/+0x1C/+0x20 for all 23 sections gives
 *
 *     sec  0   refcount 0   head 0x10878   tail 0x1087A
 *     sec  1   refcount 0   head 0x1087A   tail 0x1087C
 *     ...                   (+2 per section)
 *     sec 12   refcount 0   head 0x10890   tail 0x10892
 *     sec 13   refcount 0   head 0x10892   tail 0x10892   <-- head == tail
 *     sec 14   refcount 0   head 0x10892   tail 0x10894
 *     ...
 *     sec 22   refcount 0   head 0x108A2   tail 0x108A2   <-- head == tail
 *
 * Every section's TAIL counter address is the next section's HEAD counter address,
 * stepping by 2 bytes, which is what HeadSharedPageReferenceCountAddress and
 * TailSharedPageReferenceCountAddress mean: one WORD counter per shared page
 * boundary. The two sections where head == tail are exactly sections 13 and 22, the
 * only two whose body fits inside a single 4 KB page (13 is 0x98 bytes at 0x007F4020,
 * 22 is 0x950 bytes at 0x0089C400) -- so they have one boundary page, not two. The
 * chain ends at 0x108A2+2 = 0x108A4, which is precisely where section 0's name string
 * begins. A 22-word array abutting the name table, chained by shared page, is not a
 * coincidence, and it brackets +0x18 at the published offset with a 4-byte field.
 * All 23 reference counts are ZERO in the file, which is what a not-yet-loaded count
 * must be.
 *
 * WHICH SECTIONS, AND WHY THEY ARE THE DEMAND-PAGED ONES. Of 23 sections, exactly
 * four lack XBE_SECTION_FLAG_PRELOAD (0x02): 13 (flags 0x08), 14 (0x28), 15 (0x18)
 * and 22 (0x08). The title asks for three of them BY NAME, and the name addresses in
 * the headers resolve in the file to
 *
 *     sec 13  name at 0x000108F1 -> "$$XTINFO"
 *     sec 14  name at 0x000108FA -> "$$XTIMAGE"
 *     sec 15  name at 0x00010904 -> "$$XSIMAGE"
 *
 * matching the three literals pushed at 0x00380FB2 (0x004A1AEC "$$XTINFO"),
 * 0x00380FBC (0x004A1AE0 "$$XTIMAGE") and 0x00380FC9 (0x004A1AD4 "$$XSIMAGE"). The
 * one the boot actually loads is "$$XTINFO", section 13: its handle lands in
 * [ebp+0x10] at 0x00380FC1 and that is the slot pushed at 0x0038109C. So the first
 * XeLoadSection this title ever makes asks for 0x98 bytes at 0x007F4020. That a
 * reference-counted loader is asked for precisely the four sections the flags mark
 * as NOT preloaded is the strongest single corroboration that the model here is the
 * right one.
 *
 * WHAT THE TITLE DOES WITH IT, which is why the next blocker is not ours. At
 * sub_00380E6C the loaded section is written straight to a file:
 *
 *     0x0038109C  push [ebp+0x10];  call 0x37C97B   ; XLoadSectionByHandle -> esi = VA
 *     0x003810A6  cmp esi, 0; je ...                ; a NULL VirtualAddress is failure
 *     0x003810AA  push ebx(=0)                      ; NtWriteFile ByteOffset, pushed EARLY
 *     0x003810AB  push [ebp+0x10];  call 0x37C9BA   ; XGetSectionSize -> eax, `ret 4`
 *     0x003810B3  push eax; push esi; ...           ; Length, Buffer
 *     0x003810BF  call dword ptr [0x4757B8]         ; NtWriteFile, ordinal 236
 *     0x003810C5  push [ebp+0x10];  call 0x37C99C   ; XUnloadSectionByHandle
 *
 * INCIDENTALLY THIS CONFIRMS THE ORDINAL 236 ROW. src/host/kernel_thunk.c records
 * that 0x003810BF is one of the two sites where an argument is pushed BEFORE an
 * intervening stdcall, so no block-aligned bracket can see it. Here it is: `push ebx`
 * at 0x003810AA, then `push [ebp+0x10]; call 0x37C9BA` where the callee is `ret 4`
 * and pops only its own argument, leaving `push ebx` live. Counting the pushes that
 * actually reach NtWriteFile gives 8, not the 7 the window sees nor the 6 the
 * measured minimum publishes.
 *
 * =========================================================================
 * WHAT IS MODELLED HERE, AND WHAT IS NOT
 * =========================================================================
 *
 * THE REFERENCE COUNT IS REAL AND IT LIVES IN GUEST MEMORY, at Section+0x18, exactly
 * where the hardware kernel keeps it. No host-side shadow count: two sources of truth
 * for one number is how a bookkeeping bug survives its own tests. The host-side
 * numbers below are COUNTERS AND ANOMALY TALLIES, never the count itself.
 *
 * THE PAGES ARE ALREADY THERE, AND THAT IS SAID OUT LOUD. `xbe_map()` copies EVERY
 * section with a nonzero raw size, PRELOAD or not, so sections 13, 14, 15 and 22 are
 * resident before the title ever asks. On hardware they would not be. That makes a
 * correct XeLoadSection almost pure bookkeeping -- and it makes a WRONG one invisible,
 * which is why the first load logs the discrepancy rather than letting the trace imply
 * a paging event happened.
 *
 * RESIDENCY IS VERIFIED, NOT ASSUMED. XeLoadSection makes exactly one promise: when
 * it returns success, the section body is readable. The handler probes the section's
 * own virtual range with `kernel_guest_range_readable()` (kernel_call.c), a
 * process_vm_readv probe that refuses unmapped and PROT_NONE pages without touching a
 * guest byte. A section the host did not map, or mapped unreadable, is REPORTED and
 * refused rather than being handed back a success the memory cannot honour. The probe
 * is a check, not a lock: a concurrent unmap can still invalidate the answer, and that
 * residual fault lands in the structured fault handler (host_runtime.c).
 *
 * NOT MODELLED, deliberately and on the record:
 *   - NO COMMIT AND NO DECOMMIT. The first load does not allocate and the last unload
 *     does not release. Decommitting would unmap memory `xbe_map()` owns. Checked
 *     HLE reads would then refuse the range, while direct lifted reads could still
 *     fault; section mapping ownership and reload are not implemented. A section
 *     stays mapped at refcount 0, and `kernel_xe_stale_resident_count()` says how
 *     many times that has been true.
 *   - NO SHARED PAGE COUNTERS. The WORD counters at +0x1C and +0x20 exist so the
 *     kernel can tell when a boundary page shared by two sections may be decommitted.
 *     We never decommit, so incrementing them would be ceremony, and getting the
 *     shared-versus-private condition wrong would be a silent lie about a field the
 *     measurement above pins precisely. Left at zero.
 *   - NO SECTION DIGEST CHECK. +0x24 holds a 20-byte SHA digest the real loader
 *     verifies against the paged-in body. src/xbox/kernel_crypto.c has the SHA
 *     machinery, so this is reachable later, but verifying a digest over bytes we
 *     copied from the same file they were digested from would prove nothing about
 *     paging.
 *   - NO PAGE PROTECTION. XBE_SECTION_FLAG_WRITABLE and the head/tail read-only page
 *     flags are read and reported, never enforced, matching guest_mem.c.
 */

#ifndef TSFP_XBOX_KERNEL_XE_H
#define TSFP_XBOX_KERNEL_XE_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_call.h"

/* sizeof(XBE_SECTION_HEADER), MEASURED from `imul esi, esi, 0x38` at 0x0037C938. */
#define KERNEL_XE_SECTION_HEADER_BYTES 0x38u

/**
 * Tell the module where the guest's XBE header is, so a handle can be checked.
 *
 * WITHOUT THIS THERE IS NO BOUNDS CHECK. A handle is a raw guest pointer, and the only
 * way to know it names a real section is to walk the image's own section table at
 * XBE_HEADER.SectionHeadersAddress. src/xbox does not link src/loader, so the base has
 * to arrive from the host. Call it once, before guest code runs, with
 * `image.base_address`.
 *
 * When it is never called the handlers still reference-count correctly -- the count
 * lives in the handle's own structure -- but they say ONCE, loudly, that the bounds
 * check is disabled. An announced missing check is a gap; a silent one is a lie.
 */
void kernel_xe_set_image_base(kernel_guest_ptr xbe_base);

/** The base last given to kernel_xe_set_image_base(), or 0 when none was. */
kernel_guest_ptr kernel_xe_image_base(void);

/**
 * Section count and table address as read from guest memory, or false.
 *
 * False when no image base was supplied, when the header is unreadable, or when the
 * magic at the base is not 'XBEH' -- a base that does not point at an XBE is a host
 * wiring error and must not be treated as an empty section table, because an empty
 * table would make every handle out of range and every load fail for the wrong reason.
 */
bool kernel_xe_section_table(kernel_guest_ptr *table, uint32_t *count);

/**
 * Zero-based section index for a handle, or false when the handle is not a section.
 *
 * This IS the bounds check in queryable form: true exactly when `section` lies inside
 * the image's section table AND is aligned to the 0x38 stride. A pointer into the
 * middle of a header is refused, because the fields would all be read at the wrong
 * offsets and every value would still look plausible.
 */
bool kernel_xe_section_index(kernel_guest_ptr section, uint32_t *index);

/** Reference count read from guest memory at Section+0x18. False if unreadable. */
bool kernel_xe_section_reference_count(kernel_guest_ptr section, uint32_t *count);

/** Reset the host-side counters. Does NOT touch the guest's reference counts. */
void kernel_xe_reset(void);

/** Successful XeLoadSection calls. */
unsigned kernel_xe_load_count(void);

/** Successful XeUnloadSection calls. */
unsigned kernel_xe_unload_count(void);

/**
 * Unloads that found the reference count already zero.
 *
 * The single most important number in this module. A handler that returned success
 * without incrementing would look perfect until an unload arrived, so this is where
 * that bug surfaces. Nonzero means either the guest is unbalanced or we are.
 */
unsigned kernel_xe_underflow_count(void);

/** Calls refused for a bad handle: null, unreadable, or outside the section table. */
unsigned kernel_xe_refused_count(void);

/** Loads refused because the section's own pages are not mapped in the host. */
unsigned kernel_xe_not_resident_count(void);

/** Unloads that dropped a count to zero while leaving the pages mapped anyway. */
unsigned kernel_xe_stale_resident_count(void);

/** Register ordinals 327 and 328. Returns how many bound. */
unsigned kernel_xe_register(void);

#endif /* TSFP_XBOX_KERNEL_XE_H */
