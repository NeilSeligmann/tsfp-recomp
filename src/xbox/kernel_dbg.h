/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DbgPrint (ordinal 8), the one __cdecl export this title imports.
 *
 * ORDINAL NUMBER, RESOLVED NOT RECALLED: `tools/kernel_ordinals.py` line 66, and the
 * oracle row `{8, CDECL, 0, 0}` in src/xbox/kernel_arity_oracle.c. Not in
 * `SUSPECT_ON_XDK_5849`.
 *
 * SIGNATURE, AND WHY THE CONVENTION IS THE WHOLE POINT:
 *
 *     ULONG __cdecl DbgPrint(PCHAR Format, ...);      // variadic, CALLER cleans up
 *
 * The callee cannot know a variadic call's argument count, so it pops NOTHING. The
 * thunk's cleanup for ordinal 8 is therefore the return address alone, carried by the
 * `{8u, THUNK_CC_CDECL, 0u, 0u}` hand row in src/host/kernel_thunk.c, whose evidence
 * (the caller's own `add esp, 0x118` at 0x003DF9BC) lives beside that row. This module
 * only reads the frame; it must never be the thing that pops.
 *
 * HOW THIS TITLE CALLS IT, MEASURED. One site: the import stub sub_00384888
 * (`jmp [0x47594C]`), reached solely from D3D's debug formatter sub_003DF990, which
 * vsprintfs the message into a 0x104-byte stack buffer first and passes THAT single
 * pointer as Format. So in practice Format arrives already formatted and usually
 * carries no specifiers at all -- but "usually" is not a contract, which is why the
 * formatter below still handles specifiers, bounds every guest read, and passes
 * anything it does not model through RAW rather than faulting.
 *
 * WHAT THE HANDLER DOES. Formats into a bounded host buffer and reports through
 * kernel_hle_log() (stderr by default), prefixed "DbgPrint: ". Supported: %% %c %d %i
 * %u %x %X %p %s. Anything else -- including width and flag syntax -- is emitted
 * verbatim and consumes NO argument, so an unknown specifier can never walk the
 * argument cursor onto bytes that are not arguments. A %s whose pointer is NULL or
 * unmapped prints a marker instead of faulting. The return value is always 0
 * (STATUS_SUCCESS): a debug print must never steer the guest.
 *
 * KeBugCheck (ordinal 95) is the other debug-flavoured export bound here.
 *
 *     VOID __stdcall KeBugCheck(ULONG BugCheckCode);     // 1 stack arg, never returns
 *
 * CONTRACT. On hardware this halts the console, so there is no continuation to model.
 * The handler reads the code and reports through kernel_hle_fatal(95, ...), which the
 * host points at its stop machinery and which aborts by default. A frame that cannot
 * supply the code still reports a fatal, with the code marked unreadable. The value
 * returned afterwards is 0 and only a test's capturing hook ever sees it.
 *
 * MEASURED CALLERS, from the lifted guest C. Import slot 0x4759D0, arity row
 * {95, 1, 3 lifted sites, unanimous}, and the nxdk oracle KeBugCheck@4 agrees. Site
 * 0x003CB24B in sub_003CB1BA pushes EBX, which is 0 on the only path that reaches the
 * call (its one caller sub_003CB252 sets ebx=0), with a dead `ret` after it. Site
 * 0x003CC9AA in sub_003CC97D pushes the literal 0xC0000144 and nothing follows but an
 * int3 slide byte. The number 0xC0000144 is MEASURED. Reading it as NT's unhandled
 * exception bugcheck shape is INFERRED.
 */

#ifndef TSFP_XBOX_KERNEL_DBG_H
#define TSFP_XBOX_KERNEL_DBG_H

#include <stddef.h>
#include <stdint.h>

#include "kernel_call.h"

#define KERNEL_DBG_ORD_DBG_PRINT 8u
#define KERNEL_DBG_ORD_KE_BUG_CHECK 95u

/* Bounds. Format scan and output are capped so a lost NUL terminator in guest memory
 * costs a truncated line, never an unbounded walk. */
#define KERNEL_DBG_OUTPUT_MAX 512u
#define KERNEL_DBG_STRING_ARG_MAX 256u

/** Register ordinals 8 and 95. Returns how many bound (0 to 2). */
unsigned kernel_dbg_register(void);

/** Forget the counters. */
void kernel_dbg_reset(void);

/**
 * The formatter, exposed so tests can pin its behaviour directly.
 *
 * Reads the format string at guest address `format` and the arguments after stack
 * slot 0 of `frame` (slot 0 is Format itself), writing at most `out_size - 1` bytes
 * plus a NUL into `out`. Returns the number of bytes written, excluding the NUL.
 * Never faults on guest memory: an unreadable format byte ends the scan with a
 * marker, and a bad %s pointer prints a marker. `frame` may be NULL, in which case
 * every specifier that needs an argument prints its unreadable-argument marker.
 */
size_t kernel_dbg_format(const kernel_call_frame *frame, kernel_guest_ptr format,
                         char *out, size_t out_size);

/** Messages printed since the last reset. */
unsigned kernel_dbg_print_count(void);

/** Calls refused: no frame, or an unreadable Format argument/pointer. */
unsigned kernel_dbg_refused_count(void);

#endif /* TSFP_XBOX_KERNEL_DBG_H */
