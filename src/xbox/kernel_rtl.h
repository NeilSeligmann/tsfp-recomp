/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runtime-library ordinals: string construction and status translation.
 *
 * VERIFIED ORDINAL NUMBERS, resolved against tools/kernel_ordinals.py rather than
 * recalled. context.md section 6m records four ordinals being misremembered in a
 * single task, and 249 being used for ObfReferenceObject when it is 251:
 *
 *   269  RtlCompareMemoryUlong   --  7 call sites (see below)
 *   279  RtlEqualString          --  1 call site
 *   289  RtlInitAnsiString       -- 22 call sites
 *   301  RtlNtStatusToDosError   -- 28 call sites
 *   304  RtlTimeFieldsToTime     --  at least 5 call sites (see below)
 *   305  RtlTimeToTimeFields     --  5 call sites (see below)
 *
 * NOT BOUND HERE, because this image never calls them -- every one has ZERO call
 * sites in generated/retail/ordinal_callsites.json:
 *
 *   286  RtlFreeAnsiString       -- 0 sites
 *   290  RtlInitUnicodeString    -- 0 sites
 *
 * RtlFreeAnsiString is the interesting absence. RtlInitAnsiString does not allocate
 * -- it points the descriptor at the caller's own storage -- so there is nothing to
 * free, and the guest never asking to free one is consistent with that rather than
 * evidence of a leak.
 *
 * =====================================================================
 * ORDINAL 279: RtlEqualString -- the boot's 30th kernel call
 * =====================================================================
 *
 *     BOOLEAN __stdcall RtlEqualString(const STRING *String1, const STRING *String2,
 *                                      BOOLEAN CaseInSensitive);   // 3 stack args
 *
 * It takes COUNTED descriptors, not C strings. Both operands are the 8-byte
 * `guest_object_string` this file's ordinal 289 builds (see guest_structs.h for the
 * measured field offsets and widths), so `Length` governs the comparison and a NUL
 * inside the span is an ordinary byte.
 *
 * ARITY-OK(279): THREE stack arguments, and it needed a hand-verified ABI_TABLE row in
 * src/host/kernel_thunk.c because it has exactly ONE call site -- `stack_args_for()`
 * refuses a measured row with fewer than MEASURED_ARITY_MIN_SITES (3) voters, since
 * "unanimous" over one site is a tautology rather than a measurement.
 *
 * THE SITE IS 0x0037C952, and it is the only one. Thunk slot 0x00475780 (ordinal 279 is
 * thunk index 0) appears exactly once in the whole disassembly, as a `call dword ptr`:
 * no `mov reg, [slot]` and no `jmp [slot]` import stub, so the scanner's known blind
 * spots do not apply and "1 site" is COMPLETE rather than an under-count. The
 * containing function is 0x0037C914, FLIRT-matched as `_XGetSectionHandleA@4` from
 * 5849 xapilib, and its body is that function: build a STRING from the caller's name,
 * walk the XBE section-header array comparing names, return the header or -1.
 *
 *     0x0037C914  push ebp                      ; DISCOUNT -- frame
 *     0x0037C915  mov  ebp, esp
 *     0x0037C917  sub  esp, 0x10                ; 16 bytes of locals = TWO 8-byte STRINGs
 *     0x0037C91A  push ebx                      ; DISCOUNT -- callee-saved
 *     0x0037C91B  mov  ebx, [0x475784]          ; ordinal 289's slot, called via ebx
 *     0x0037C921  push esi                      ; DISCOUNT -- callee-saved
 *     0x0037C922  push edi                      ; DISCOUNT -- callee-saved
 *     ...
 *     0x0037C93F  push [edi+0x14]               ; \ ordinal 289 on the SECOND STRING
 *     0x0037C942  lea  eax,[ebp-8]; push eax    ; /
 *     0x0037C946  call ebx
 *     0x0037C948  push 1                        ; arg2 CaseInSensitive = TRUE
 *     0x0037C94A  lea  eax,[ebp-8];    push eax ; arg1 String2 -- the section's name
 *     0x0037C94E  lea  eax,[ebp-0x10]; push eax ; arg0 String1 -- the caller's name
 *     0x0037C952  call dword ptr [0x475780]     ; RtlEqualString
 *     0x0037C958  test al, al                   ; the result is a BYTE in AL
 *
 * Three pushes, and nothing to discount among them: the basic block begins at
 * 0x0037C948 (the return site of the preceding call), the four discounted pushes sit
 * ABOVE `sub esp, 0x10` in the prologue, and there is no `pop`, `add esp` or `sub esp`
 * between the first push and the call.
 *
 * FOUR INDEPENDENT CONFIRMATIONS, which matters more than the count itself.
 *
 *   1. A FORCED STACK BALANCE, and it derives the number rather than checking it. The
 *      comparison sits in a loop (0x0037C93F..0x0037C961) that contains NO `add esp`
 *      anywhere, and the match exit -- `jne 0x37C977`, `mov eax, edi`, `jmp 0x37C970`,
 *      `pop edi; pop esi; pop ebx` -- contains none either. So for those three
 *      callee-saved registers to be restored, esp at 0x0037C970 must equal esp after
 *      `push edi`, for EVERY iteration count. Each iteration moves esp by
 *      (d289 + d279 - 20) bytes, and the trip count genuinely varies: 0x00380FB7,
 *      0x00380FC4 and 0x00380FD1 call this function with "$$XTINFO", "$$XTIMAGE" and
 *      "$$XSIMAGE", which are sections 13, 14 and 15 of this XBE. One required final
 *      esp across three different trip counts forces the per-iteration delta to ZERO,
 *      hence d289 + d279 = 20, and then the entry balance forces d289 = 8. So
 *      d279 = 12, i.e. THREE dwords. Note this re-derives ordinal 289 as 2 arguments
 *      from scratch and agrees with the hand verification already recorded for it --
 *      and disagrees with the measured table's 1, as that record says it should.
 *   2. THE OPERAND SHAPES. `sub esp, 0x10` is exactly two 8-byte OBJECT_STRINGs and
 *      there is no third local, both are initialised by ordinal 289 immediately before
 *      use, and the second comes from `[edi+0x14]` stepping by 0x38 -- which this
 *      repo's own tools/xbe/parser.py independently confirms as the section header's
 *      name pointer and SECTION_HEADER_SIZE. A one- or two-argument reading has
 *      nowhere to put the second operand.
 *   3. THE LITERAL `push 1` pins the third argument as the BOOLEAN, and `test al, al`
 *      at the return pins the result as a BOOLEAN in AL rather than a wider value.
 *   4. THE EXPORT DECORATION. `xboxkrnl.exe.def` in XboxDev/nxdk (CC0-1.0, verdict
 *      "USE -- derive and cite" in docs/clean-sources-audit.md) lists
 *      `RtlEqualString@12  @ 279 NONAME`. The MSVC `@N` suffix is the TOTAL BYTE SIZE
 *      of the argument list, so 12 bytes is three dwords, and the ordinal in the same
 *      row is 279. No LEADING `@`, so __stdcall and not one of the fastcall families.
 *
 * Why (4) is strong rather than merely available, and the number is MEASURED rather than
 * impressionistic: that one file was cross-checked against EVERY row of ABI_TABLE at the
 * time this was written, and the result is 42 AGREE, 0 DISAGREE, 0 ordinals missing from
 * the DEF file. It reproduces the CONVENTION too -- both fastcall rows (160, 161) carry a
 * leading `@` there and every stdcall row does not -- and it agrees with all three
 * ordinals where the automated call-site scanner was WRONG: `NtReadFile@32` (8, scanner
 * said 6), `RtlInitAnsiString@8` (2, scanner said 1) and `NtCreateFile@36` (9, where the
 * project record says "9, NOT 11").
 *
 * NO NULL MODEL IS QUOTED because none is meaningful here: the DEF file is a
 * reconstruction of the same export table, not an independent draw, so "how likely is 42
 * agreements by chance" is the wrong question. The right one is whether it shares a
 * FAILURE MODE with the things it is corroborating, and it does not -- it is an export
 * NAME DECORATION, so it cannot be fooled by a late `_icall_esp` bracket, by a
 * callee-saved push inside an argument window, or by a `jmp [slot]` import stub, which
 * are the three ways everything else here goes wrong. It is still not a measurement of
 * THIS image, which is why it corroborates (1)-(3) rather than replacing them.
 *
 * WHAT IS NOT MEASURED, said plainly. The comparison ORDER (length first, then bytes)
 * and the case-folding rule are not observable from this site -- the guest passes
 * `CaseInSensitive` as a literal and only looks at the BOOLEAN result. So the folding
 * is ASCII-only and any byte above 0x7F met on a case-insensitive comparison is
 * COUNTED AND NAMED rather than folded by a table we have not measured. See the long
 * note in kernel_rtl.c.
 *
 */

/*
 * =====================================================================
 * ORDINALS 269, 304, 305: the startup-critical additions (saved startup worktree)
 * =====================================================================
 *
 *     ULONG   __stdcall RtlCompareMemoryUlong(PVOID Source, ULONG Length,
 *                                             ULONG Pattern);              // 3 stack args
 *     VOID    __stdcall RtlTimeToTimeFields(const LARGE_INTEGER *Time,
 *                                           TIME_FIELDS *TimeFields);      // 2 stack args
 *     BOOLEAN __stdcall RtlTimeFieldsToTime(TIME_FIELDS *TimeFields,
 *                                           LARGE_INTEGER *Time);          // 2 stack args
 *
 * ARITY-OK(269): 3. MEASURED over 7 distinct call sites (0x38252D, 0x38259C, 0x382679,
 * 0x3826FD, 0x3829F0, 0x382C01, 0x3843D2), all `push 0xFEEEFEEE` (Pattern), `push Length`,
 * `lea eax,[hdr+0x18]; push eax` (Source). Unanimous and over the 3-site quorum, so the
 * measured table answers. The nxdk .def (a different kernel build) says `@12`. INFERRED use:
 * a heap free-fill check, the result is unread at 6 sites and spilled unread at the 7th.
 *
 * ARITY-OK(305): 2. MEASURED over 5 sites, unanimous. The order is pinned at 0x37EBA0, the
 * FileTimeToSystemTime wrapper: the first push is `ebp-24` (the TIME_FIELDS the wrapper then
 * copies out into SYSTEMTIME order) and the second is `ebp-8` (the 8-byte time it loaded from
 * the caller), so arg0 is the time and arg1 the fields. The nxdk .def says `@8`.
 *
 * ARITY-OK(304): 2. MEASURED at 0x37EC37, the SystemTimeToFileTime wrapper: the first push
 * is `ebp-8` (the output, read back into the caller's FILETIME afterwards) and the second is
 * `ebp-24` (the TIME_FIELDS built from the caller's SYSTEMTIME), so arg0 is the INPUT fields
 * and arg1 the OUTPUT time. The wrapper tests AL and takes STATUS_INVALID_PARAMETER on 0
 * (0xC000000D, a literal pushed at 0x37EC41). The measured table has 3 sites (the real count
 * is at least 5: three `call ebx` sites are invisible to the scanner), the .def says `@8`.
 *
 * TIME_FIELDS is 8 CSHORT {Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday} at
 * +0, +2 ... +14. MEASURED: the wrapper at 0x37EC37 fills +0..+12 from SYSTEMTIME (moving
 * SYSTEMTIME +6.. down by one slot because SYSTEMTIME has Weekday at +4) and never writes
 * +14, so the kernel must ignore Weekday on input.
 *
 * INFERRED FROM NT, NOT FROM THIS IMAGE: RtlCompareMemoryUlong rounds Length down to a whole
 * number of words and has no alignment rule. RtlTimeFieldsToTime rejects Year < 1601, Month
 * outside 1..12, a Day past the month's length (Gregorian leap rule), Hour >= 24, Minute or
 * Second >= 60 and Milliseconds >= 1000. Both are the documented NT behaviour, and no site
 * in this image exercises an invalid value.
 *
 * REFUSALS, all logged and counted, never a silent answer. A Time that is negative or past
 * year 32767 (the CSHORT limit) leaves the output untouched. A TimeFields whose tick count
 * would overflow a signed 64-bit value answers FALSE. An unreadable Source answers 0.
 *
 * ABI_TABLE rows (src/host/kernel_thunk.c, the lead's file), all stdcall with no register
 * arguments. 269, 305 and 304 clear the measured quorum, the rows only pin the evidence:
 *     {269u, THUNK_CC_STDCALL, 3u, 0u},  {304u, THUNK_CC_STDCALL, 2u, 0u},
 *     {305u, THUNK_CC_STDCALL, 2u, 0u},
 *
 * =====================================================================
 * ORDINAL 302: RtlRaiseException, a fatal stop and not a dispatch
 * =====================================================================
 *
 *     VOID __stdcall RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);  // 1 stack arg
 *
 * CONTRACT. This host has no exception dispatch model, so the handler decodes the record
 * (code, flags, address, parameter count capped at 15, up to four Info dwords), reports it
 * through kernel_hle_fatal(302, ...) and stops. An unreadable pointer or record goes fatal
 * too, naming the record as unreadable. The value returned after the hook is the
 * ExceptionCode, or 0 when unreadable, and only a capturing test hook ever sees it. The
 * measured callers (3 guest sites, 5 lifted), the record layout and why the continuable FP
 * path would need real dispatch are in the ARITY-OK(302) note in kernel_rtl.c. All three
 * sites are conditional error arms and none is reached by the measured boot.
 */

#ifndef TSFP_XBOX_KERNEL_RTL_H
#define TSFP_XBOX_KERNEL_RTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_hle.h"
#include "nt_status.h"

/**
 * The Win32 error returned for any status this image gives no evidence of.
 *
 * ERROR_MR_MID_NOT_FOUND is what the real function returns for a status it has no
 * entry for, so a guest that checks for it is already prepared. Using it keeps the
 * fallback INSIDE the behaviour the guest can expect rather than inventing a
 * sentinel that no caller has ever seen.
 *
 * The value is not recalled: see the long note in kernel_rtl.c for why a wrong
 * fallback is still safer than a wrong mapping, and what is reported when one is hit.
 */
#define KERNEL_RTL_ERROR_MR_MID_NOT_FOUND 317u

/** Win32 success. */
#define KERNEL_RTL_ERROR_SUCCESS 0u

/** Register the RTL ordinals (seven, with 302). Returns how many bound. */
size_t kernel_rtl_register(void);

/** Clear the counters below. For test isolation. */
void kernel_rtl_reset(void);

/**
 * Translate an NTSTATUS, exactly as ordinal 301 would. Exposed for tests.
 *
 * Separate from the handler so the mapping can be exercised without building a
 * guest stack frame, and so a test can walk the whole table.
 */
uint32_t kernel_rtl_status_to_dos_error(nt_status status);

/**
 * How many translations fell through to the fallback.
 *
 * This is the number that matters. The table deliberately covers only the statuses
 * this image gives evidence of, so a nonzero count here is not a failure -- it is
 * the signal that a status we did not anticipate is now live, and the log line that
 * accompanies it names the value to add.
 */
uint32_t kernel_rtl_unmapped_count(void);

/** RtlCompareMemoryUlong calls refused because the Source range was unreadable. */
uint32_t kernel_rtl_compare_refused_count(void);

/** Time conversions refused: negative or out-of-range time, invalid fields, overflow. */
uint32_t kernel_rtl_time_refused_count(void);

/** The most recent status that fell through, or STATUS_SUCCESS if none has. */
nt_status kernel_rtl_last_unmapped(void);

/**
 * Compare two guest OBJECT_STRINGs, exactly as ordinal 279 would. Exposed for tests.
 *
 * Separate from the handler for the same reason the status table is: the comparison
 * can then be exercised over many operand pairs without building a guest stack frame
 * for each. `equal` receives the BOOLEAN answer and is only written on success.
 *
 * False when either descriptor or either buffer is unusable guest memory. A false
 * return is NOT "not equal" -- the two are different outcomes and collapsing them
 * would make a failed read indistinguishable from a real mismatch.
 */
bool kernel_rtl_strings_equal(uint32_t string1, uint32_t string2, bool case_insensitive,
                              bool *equal);

/** How many unterminated strings were clamped to the 16-bit ANSI_STRING length. */
uint32_t kernel_rtl_truncated_count(void);

/**
 * How many case-insensitive comparisons met a byte above 0x7F.
 *
 * This is the number that matters for ordinal 279, and it is the direct analogue of
 * `kernel_rtl_unmapped_count()`. Folding above ASCII needs RtlUpperChar's OEM
 * code-page table, which is NOT measured from this image, so those bytes are compared
 * WITHOUT folding and counted here. Nonzero means the unmeasured path is live and the
 * accompanying log line names the byte to go and measure.
 */
uint32_t kernel_rtl_nonascii_fold_count(void);

/** The most recent byte above 0x7F met while folding, or 0 if none has been. */
uint8_t kernel_rtl_last_nonascii_fold(void);

#endif /* TSFP_XBOX_KERNEL_RTL_H */
