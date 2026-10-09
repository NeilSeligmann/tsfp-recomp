/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal runtime for differentially testing xboxrecomp-generated code.
 * Defines exactly the symbols the generated chunks reference, and nothing else.
 *
 * WHY THIS LIVES IN THE REPO
 * --------------------------
 * It used to live only in a scratch directory under tmp/, and build_subject.sh
 * defaulted to a COPY OF IT two lifter generations old that is missing the four
 * trap symbols the current lifter emits. Linking the shipped tree against that
 * default fails on 6,575 relocations, so the harness as its own documentation
 * invoked it could not be run against the committed lift at all -- which went
 * unnoticed because the default OBJ_DIR and GEN_DIR were stale in lockstep with
 * it, and an all-defaults link of three mutually consistent stale inputs
 * succeeds. The instrument's runtime is part of the instrument, so it is tracked
 * here next to driver.c and is what build_subject.sh uses with no arguments.
 */
#define RECOMP_HARNESS_RUNTIME 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness_abort.h"

#define ICALL_TRACE_SIZE 256

/* --- memory model: identity map, so offset is zero --- */
ptrdiff_t g_xbox_mem_offset = 0;
uint32_t g_xbox_code_lo = 0x00012000u;
uint32_t g_xbox_code_hi = 0x0046F76Cu;

/* --- integer registers (must match RECOMP_TLS = __thread) --- */
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
__thread uint32_t g_ebx, g_esi, g_edi;
__thread uint32_t g_ebp;
__thread uint32_t g_seh_ebp;
__thread int g_df;

/* Opt-in EFLAGS publication (T16). Written by the rets of a --publish-eflags
 * lift; defined unconditionally so one runtime links against both kinds of
 * tree. The mask names which bits the publishing exit's model answered; the
 * driver resets both to 0 before every case, and harness_stub_call clears the
 * mask so a stubbed tail exit reads as unpublished rather than stale. */
__thread uint32_t g_harness_eflags;
__thread uint32_t g_harness_eflags_mask;
/* The flag bridge words (T554, lifter patch 21), defined here so a subject built from a
 * --flag-bridge lift links. The harness never reads them. */
__thread uint32_t g_flag_bridge_eflags;
__thread uint32_t g_flag_bridge_mask;

/* --- x87 --- */
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
__thread int g_fp_cmp;

/* --- SSE --- */
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
__thread RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
__thread RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* Raw vector proof protocol. Movement/bitwise instructions preserve MXCSR;
 * arithmetic remains refused until the software model implements it. */
__thread uint32_t g_harness_mxcsr;
void harness_vector_set(const uint64_t words[16], uint32_t mxcsr)
{
    RecompXmm *regs[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                          &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    for (int i = 0; i < 8; ++i) memcpy(regs[i]->q, words + i * 2, 16);
    g_harness_mxcsr = mxcsr;
}
void harness_vector_get(uint64_t words[16], uint32_t *mxcsr)
{
    RecompXmm *regs[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                          &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    for (int i = 0; i < 8; ++i) memcpy(words + i * 2, regs[i]->q, 16);
    *mxcsr = g_harness_mxcsr;
}

/* T1620 fp-scalar-v1: the subject runs on the REAL MXCSR. The legacy shadow above is an echo,
 * so a control-bit compare on the subject side proves nothing (the replacement would run on
 * whatever the process happened to hold). In this mode `set` executes ldmxcsr with the case
 * word and `capture` executes stmxcsr into the shadow, then restores the default 0x1F80 so
 * the driver's own float code never inherits a case's rounding or flush mode. A replacement
 * that changes MXCSR (fesetround, _mm_setcsr, a crtfastmath constructor run later) is seen by
 * the exact control-bit compare. */
#define HARNESS_MXCSR_DEFAULT 0x1F80u
void harness_mxcsr_load_real(uint32_t mxcsr)
{
    g_harness_mxcsr = mxcsr;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}
void harness_mxcsr_capture_real(void)
{
    uint32_t value = 0;
    uint32_t restore = HARNESS_MXCSR_DEFAULT;
    __asm__ volatile("stmxcsr %0" : "=m"(value));
    __asm__ volatile("ldmxcsr %0" : : "m"(restore));
    g_harness_mxcsr = value;
}
void harness_mxcsr_restore_default(void)
{
    uint32_t restore = HARNESS_MXCSR_DEFAULT;
    __asm__ volatile("ldmxcsr %0" : : "m"(restore));
}

/* --- indirect-call instrumentation --- */
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

/* Any of these firing during a test means the test touched control flow we
 * deliberately excluded, so make it loud rather than silent. */
int g_harness_icall_failures = 0;
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "HARNESS: icall_fail va=0x%08X\n", va);
    g_harness_icall_failures++;
}
void recomp_icall_not_code_log(uint32_t va)
{
    fprintf(stderr, "HARNESS: icall_not_code va=0x%08X\n", va);
    g_harness_icall_failures++;
}

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* MMX register file. */
typedef union RecompMmx {
    uint64_t q;
    uint32_t u[2];
    int32_t  i[2];
    uint16_t w[4];
    uint8_t  b[8];
    float    f[2];
} RecompMmx;
__thread RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
__thread RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

__thread int g_fp_cc;
__thread uint32_t g_fs_base;

/* Reached when the lifter could not translate an instruction. Any test case
 * that lands here must be reported, never silently passed. */
int g_harness_unimpl = 0;
void recomp_unimpl(const char *text, uint32_t va)
{
    fprintf(stderr, "HARNESS: recomp_unimpl va=0x%08X %s\n", va,
            text ? text : "?");
    g_harness_unimpl++;
}
void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    (void)service; (void)arg_va;
}

/* ------------------------------------------------------------------------- *
 * Symbols the PATCHED lifter emits that upstream's runtime surface does not
 * define. Added so a patched gen tree links against this runtime unchanged.
 *
 * Two families:
 *   - non-trapping (recomp_unimpl, recomp_flags_unresolved): upstream's silent
 *     behaviour, log and return. Selected by the generated header when
 *     RECOMP_ALL_CONTINUE is defined.
 *   - trapping (*_trap): print to stderr and never return.
 *
 * WHICH "NEVER RETURN" ACTUALLY HAPPENS HERE. driver.c does NOT arm
 * g_recomp_abort_jmp, so every trap below takes the abort() path. That is
 * deliberate and it is not a lost run: driver.c installs a SIGABRT handler and
 * scores the case as `FAULT ABORT`, which covers abort() from any source rather
 * than only from this one runtime's helpers. The armed path is kept because it is
 * what a standalone consumer of this runtime would use, but an earlier version of
 * this comment claimed it was what a harness build does, and that was false --
 * nothing armed it, so before the SIGABRT handler existed the first trap killed
 * the subject and cost the entire run.
 * ------------------------------------------------------------------------- */

sigjmp_buf   g_recomp_abort_jmp;
volatile int g_recomp_abort_armed = 0;
volatile int g_recomp_abort_kind  = RECOMP_ABORT_NONE;
char         g_recomp_abort_msg[256];

int g_harness_flags_unresolved = 0;

void recomp_abort_reset(void)
{
    g_recomp_abort_kind = RECOMP_ABORT_NONE;
    g_recomp_abort_msg[0] = '\0';
    g_harness_unimpl = 0;
    g_harness_flags_unresolved = 0;
    g_harness_icall_failures = 0;
}

/* Common tail: record the reason, say so on stderr, then never return. */
__attribute__((noreturn))
static void recomp_trap_tail(void)
{
    fprintf(stderr, "HARNESS TRAP: %s\n", g_recomp_abort_msg);
    fflush(stderr);
    if (g_recomp_abort_armed) {
        g_recomp_abort_kind = RECOMP_ABORT_TRAP;
        siglongjmp(g_recomp_abort_jmp, RECOMP_ABORT_TRAP);
    }
    abort();
}

/* Unimplemented instruction, trapping flavour. */
__attribute__((noreturn))
void recomp_unimpl_trap(const char *text, uint32_t va)
{
    snprintf(g_recomp_abort_msg, sizeof g_recomp_abort_msg,
             "unimpl va=0x%08X %s", va, text ? text : "?");
    recomp_trap_tail();
}

/* Flag read whose producer the lifter could not resolve, silent flavour
 * (upstream behaviour: log and carry on with whatever the flag holds). */
void recomp_flags_unresolved(const char *cc, uint32_t va)
{
    fprintf(stderr, "HARNESS: flags_unresolved va=0x%08X cc=%s\n", va,
            cc ? cc : "?");
    g_harness_flags_unresolved++;
}

__attribute__((noreturn))
void recomp_flags_unresolved_trap(const char *cc, uint32_t va)
{
    snprintf(g_recomp_abort_msg, sizeof g_recomp_abort_msg,
             "flags_unresolved va=0x%08X cc=%s", va, cc ? cc : "?");
    recomp_trap_tail();
}

__attribute__((noreturn))
void recomp_icall_unresolved_trap(uint32_t va, const char *kind)
{
    snprintf(g_recomp_abort_msg, sizeof g_recomp_abort_msg,
             "icall_unresolved va=0x%08X kind=%s", va, kind ? kind : "?");
    recomp_trap_tail();
}

/* A call the analysis marked non-returning actually returned, so the
 * instructions after the call site were never translated. Explicitly NOT
 * downgradable by RECOMP_ALL_CONTINUE -- there is nothing to continue into --
 * so it always takes the trap path. */
__attribute__((noreturn))
void recomp_noreturn_returned(uint32_t callee_va, uint32_t site_va)
{
    snprintf(g_recomp_abort_msg, sizeof g_recomp_abort_msg,
             "noreturn_returned va=0x%08X callee=0x%08X", site_va, callee_va);
    recomp_trap_tail();
}

/* The manual-lift thunk dispatcher. Only a --manual-functions tree references it
 * (236 relocations, all in recomp_xdk_manual.c), and the real one lives in
 * src/host/xdk_thunk.c, which a harness subject does not link. Without this stub
 * the manual tree cannot be linked into a subject at all, so fixing the
 * interception check alone does not make that tree measurable.
 *
 * Aborting is the honest behaviour, not a convenience. The real dispatcher hands
 * control to a host thunk, and a harness has no host: there is no value this could
 * return that would make the case comparable. abort() reaches driver.c's SIGABRT
 * handler and scores the case `FAULT ABORT`, so a case that touches a thunk is
 * recorded as having no usable verdict instead of silently continuing with a
 * fabricated result. Returning quietly here would turn every thunk-reaching case
 * into an AGREE or a DISAGREE that means nothing. */
void xdk_thunk_dispatch_at(uint32_t address)
{
    fprintf(stderr, "HARNESS: xdk_thunk_dispatch_at va=0x%08X (no host in a harness)\n",
            address);
    fflush(stderr);
    abort();
}

/* Explicit stop-only trampolines have no handler result or ABI cleanup. Never
 * substitute success, even when the harness allows ordinary missing calls. */
void xdk_thunk_stop_at(uint32_t address, const char *label, const char *reason)
    __attribute__((noreturn));
void xdk_thunk_stop_at(uint32_t address, const char *label, const char *reason)
{
    fprintf(stderr, "HARNESS: xdk_thunk_stop_at va=0x%08X label=%s reason=%s\n",
            address, label ? label : "<unnamed>", reason ? reason : "explicit stop-only policy");
    fflush(stderr);
    abort();
}

void xbe_entry_point_unused_placeholder(void) { }
uint64_t xbox_ReadTimeStampCounter(void) { return 0; }
void recomp_trace_enter(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_exit(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_esp(const char *n, const char *t) { (void)n; (void)t; }
