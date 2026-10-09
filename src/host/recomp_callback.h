/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_CALLBACK_H
#define TSFP_RECOMP_CALLBACK_H
#include <stdbool.h>
#include <stdint.h>
#include "host_runtime.h"
/* Exclusive, quiescent private stack [low,high), disjoint from PCR and interrupted
 * stack. Only measured leaf 22020 is supported. No scheduling policy, and no IRQL change
 * unless recomp_callback_set_dispatch_level (T370) is enabled.
 * Temporary FS:[0]=FFFFFFFF isolates callback SEH; this is a context convention,
 * not hardware DPC emulation. Stops/faults restore context then propagate. */
/* False after bad return cleanup can retain callback guest writes. Borrowed
 * stack contents are not restored; only the leaf's top 20-byte frame is checked.
 * Cleanup failures violate the quiescence contract and abort, never resume. */
/* T370, default off. When enabled the callback runs at DISPATCH_LEVEL as the original DPC path
 * does: the calling thread's level is raised through the kernel IRQL model (published to the
 * guest's KPCR, so the callback reads 2 at fs:[0x24]) after every preflight and restored to
 * exactly the previous level on every exit, normal, bad return, stop and fault. A level above
 * DISPATCH_LEVEL at entry is refused rather than lowered. Quiescent configuration. */
void recomp_callback_set_dispatch_level(bool enabled);
/* T538: the calling guest thread's register file as plain bytes, for a diagnostic capture. Read only,
 * no host state changes. `registers` is EAX, ECX, EDX, EBX, ESI, EDI, EBP, ESP; the virtual x87 stack is
 * the doubles the lifted code keeps, not the hardware format. */
typedef struct {
    uint32_t registers[8];
    uint64_t mmx[8];
    uint8_t xmm[8][16];
    double x87[8];
    uint32_t x87_top, x87_control, x87_compare, x87_condition;
} recomp_machine_snapshot;
void recomp_callback_machine_snapshot(recomp_machine_snapshot *out);
bool recomp_callback_run(uint32_t address, uint32_t low, uint32_t high,
                         const uint32_t payload[3]);
/* Original XNET DPC 0043A184 STDCALL4/RET16. Borrowed exclusive stack;
 * arguments are Dpc, DeferredContext, SystemArgument1, SystemArgument2.
 * Preserves full calling context and runs at DISPATCH_LEVEL. Captured stop is
 * returned after cleanup so scheduler bookkeeping can unwind before rethrow.
 * A false return with captured_stop true must be propagated by
 * the caller; it is never a successful deferred operation. */
bool recomp_callback_run_xnet_dpc(uint32_t address, uint32_t low, uint32_t high,
    const uint32_t arguments[4], host_stop *stopped, bool *captured_stop);
#endif
