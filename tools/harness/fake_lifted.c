/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A tiny stand-in for the lifted tree, so that driver.c can be TESTED.
 *
 * The production subject is a 64 MB binary linked against 67 generated chunks and
 * built from the retail image, which means every property of driver.c -- the
 * dirty-page bookkeeping, the stub table, the completeness of the initial
 * register state -- was previously only checkable by running the whole harness
 * against the real game and seeing whether the numbers looked sensible. That is
 * how the g_seh_ebp defect survived: the driver comment explained the fix, and
 * nothing anywhere asserted it.
 *
 * This file defines exactly the symbols driver.c and call_stub.c reference, plus a
 * handful of synthetic "lifted" functions whose correct behaviour is obvious by
 * construction. Linked with driver.c and call_stub.c it produces a subject that
 * starts in milliseconds and speaks the real protocol, so the protocol-level
 * regressions can be pinned by a test instead of by a comment.
 *
 * The guest window is identity-mapped by driver.c, so a guest address is just a
 * pointer here.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* --- the register file the generated code and driver.c share --- */
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
__thread uint32_t g_ebx, g_esi, g_edi, g_ebp;
__thread uint32_t g_seh_ebp;
__thread uint32_t g_fs_base;
__thread int g_df;
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
int g_harness_icall_failures = 0;
/* The opt-in EFLAGS publication channel driver.c resets and reports. */
__thread uint32_t g_harness_eflags, g_harness_eflags_mask;

typedef void (*recomp_func_t)(void);

/* Supplied by call_stub.c, mirroring how a real generated chunk reaches it. */
void harness_stub_call(uint32_t va, void (*fn)(void));

#define FAKE_PROBE_ADDR 0x00D00000u
#define FAKE_SECOND_PROBE 0x00D00100u
#define MEM32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* VAs the test addresses these by. Chosen inside the guest window and well away
 * from the generated stack and scratch regions. */
#define VA_ECHO_SEH_EBP 0x00300000u
#define VA_STUB_CALLER  0x00300010u
#define VA_TWO_CALLS    0x00300020u
#define VA_WRITER       0x00300030u
#define VA_ABORTER      0x00300040u
#define VA_WILD_READ    0x003000A0u
#define VA_SEH_DRIFT    0x00300050u
#define VA_FLAGS_RET    0x00300060u
#define VA_FLAGS_STUB_TAIL 0x00300070u
#define VA_X87_RAW_FMUL 0x00300080u
#define VA_X87_LEGACY   0x00300090u
#define VA_CALLEE_A     0x00400000u
#define VA_CALLEE_B     0x00400010u

/* Known values for the flags-publishing fake: ZF|SF set, published under a
 * mask claiming CF/ZF/SF/DF -- so a test can assert both words travel the
 * protocol unchanged and that unclaimed bits stay out of the mask. */
#define FAKE_EFLAGS_VALUE 0x0C0u
#define FAKE_EFLAGS_MASK  0x4C1u

/* The callee marker. If a test sees this in eax, the real callee ran when it
 * should have been stubbed. */
#define FAKE_CALLEE_MARKER 0x11112222u

/* Publishes g_seh_ebp where a test can see it.
 *
 * This is the shape of a lifter-marked `fpo_leaf` prologue: it reads the frame
 * pointer "currently in effect" out of the global rather than from its own frame.
 * If driver.c does not seed g_seh_ebp from the case's ebp, this writes a value
 * left over from a previous case -- a memory divergence with perfectly matching
 * registers, which is exactly what the write-set comparison exists to catch. */
static void fake_echo_seh_ebp(void)
{
    MEM32(FAKE_PROBE_ADDR) = g_seh_ebp;
}

static void fake_callee_a(void)
{
    g_eax = FAKE_CALLEE_MARKER;
}

static void fake_callee_b(void)
{
    g_eax = FAKE_CALLEE_MARKER;
}

/* Mimics the generated direct-call sequence:
 *     PUSH32(esp, retaddr); RECOMP_ABI_CALL(callee_va, sub_callee);
 * The push happens on the simulated guest stack and stays there, exactly as the
 * hardware's `call` leaves it, which is why the stub must only undo the stack
 * adjustment and not the store. */
static void fake_stub_caller(void)
{
    g_esp -= 4;
    MEM32(g_esp) = 0x00300015u; /* the fabricated guest return address */
    harness_stub_call(VA_CALLEE_A, fake_callee_a);
}

/* Two calls to DIFFERENT callees, so a test can check the applied count and that
 * each callee's own pop amount is used. */
static void fake_two_calls(void)
{
    g_esp -= 4;
    MEM32(g_esp) = 0x00300025u;
    harness_stub_call(VA_CALLEE_A, fake_callee_a);
    g_esp -= 4;
    MEM32(g_esp) = 0x0030002Au;
    harness_stub_call(VA_CALLEE_B, fake_callee_b);
}

/* Writes a dword into the scratch arena, for the write-reporting and
 * dirty-page-restore checks. */
static void fake_writer(void)
{
    MEM32(FAKE_SECOND_PROBE) = g_eax;
}

#ifdef FAKE_X87RAW
/* T1510 driver controls: FLD m32 / FMUL m32 / FSTP m32 through the raw runtime, and a body
 * that touches only the legacy double model (which the raw backend must refuse). */
#include "x87_runtime.h"
static void fake_x87_raw_fmul(void)
{
    harness_x87_fld32(FAKE_PROBE_ADDR);
    harness_x87_fmul32(FAKE_PROBE_ADDR);
    harness_x87_fstp32(FAKE_SECOND_PROBE);
}
static void fake_x87_legacy(void)
{
    g_fp_top = 7;
    g_fp_stack[7] = 1.0;
}
#endif

/* Stands in for the lifter's trap family, whose contract is abort(): a case that
 * reaches an unimplemented instruction or an unresolvable indirect call ends up
 * in recomp_unimpl_trap / recomp_icall_unresolved_trap and never returns. Calling
 * abort() directly is the same signal by the same route, without needing the real
 * runtime linked in. Before driver.c handled SIGABRT this killed the subject
 * process, which cost the whole run rather than scoring one case. */
static void fake_aborter(void)
{
    abort();
}

/* T1576: read an unmapped guest-space address (below 4 GiB, outside the guest window) so the
 * driver sees a genuine SEGV with a known si_addr. */
static void fake_wild_read(void)
{
    g_eax = *(volatile uint32_t *)(uintptr_t)(0x30000000u + (g_ecx & 0xFFFu));
}

/* Changes g_seh_ebp and NOT g_ebp, which is the shape lifter patch 10 leaves
 * behind on purpose: it restores g_ebp after a call and deliberately does not
 * restore this one, because this one is the handoff channel to the next callee.
 * Lets a test assert that the drift is OBSERVED and that observing it does not
 * turn into a verdict. */
static void fake_seh_drift(void)
{
    g_seh_ebp = g_ebp ^ 0x00001000u;
}

/* The shape a --publish-eflags ret emits: assemble the flags word and the mask
 * of bits the model answered, store both, return. Known constants here, so a
 * test can assert the exact words come back on the FLAGS line. */
static void fake_flags_ret(void)
{
    g_harness_eflags = FAKE_EFLAGS_VALUE;
    g_harness_eflags_mask = FAKE_EFLAGS_MASK;
}

/* A publish followed by a stubbed TAIL call: the stub is the last thing to run,
 * so the mask must read 0 at exit -- the stub published nothing, and reporting
 * the earlier publish as the exit's flags would be stale. Pins the clear in
 * harness_stub_call. */
static void fake_flags_then_stub_tail(void)
{
    g_harness_eflags = FAKE_EFLAGS_VALUE;
    g_harness_eflags_mask = FAKE_EFLAGS_MASK;
    g_esp -= 4;
    MEM32(g_esp) = 0x00300075u;
    harness_stub_call(VA_CALLEE_A, fake_callee_a);
}

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    switch (xbox_va) {
    case VA_ECHO_SEH_EBP: return fake_echo_seh_ebp;
    case VA_FLAGS_RET:    return fake_flags_ret;
    case VA_FLAGS_STUB_TAIL: return fake_flags_then_stub_tail;
    case VA_STUB_CALLER:  return fake_stub_caller;
    case VA_TWO_CALLS:    return fake_two_calls;
    case VA_WRITER:       return fake_writer;
    case VA_ABORTER:      return fake_aborter;
    case VA_WILD_READ:    return fake_wild_read;
    case VA_SEH_DRIFT:    return fake_seh_drift;
#ifdef FAKE_X87RAW
    case VA_X87_RAW_FMUL: return fake_x87_raw_fmul;
    case VA_X87_LEGACY:   return fake_x87_legacy;
#endif
    case VA_CALLEE_A:     return fake_callee_a;
    case VA_CALLEE_B:     return fake_callee_b;
    default:              return NULL; /* driver.c reports NOFUNC */
    }
}
