/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The runtime the lifted code links against: register file, x87/SSE/MMX state,
 * and the handful of helpers the generated C calls out to.
 *
 * WE WROTE THIS, DELIBERATELY. `docs/lifter-evaluation.md` §9 item 9 records that
 * upstream's only path to an executable is unconditionally Windows -- `WinMain`,
 * `add_executable(WIN32)`, D3D11 -- and §5 records two real pointer-truncation
 * leaks in its kernel bridge. None of its runtime is vendored; this file is the
 * whole of our side of the boundary, and it is deliberately small enough to read.
 *
 * IT INCLUDES BOTH VIEWS OF THE ABI ON PURPOSE. `recomp_abi.h` is the strict-clean
 * declaration the rest of `src/host/` uses; `recomp_types.h` is the generated one
 * the 2.56 M lines of lifted C use. This translation unit includes both and
 * defines every symbol, so a disagreement between them is a compile error here
 * rather than a silent ABI break at link time. ICALL_TRACE_SIZE is a concrete
 * example of why that matters: it is 16 in the generated header, and a runtime
 * that assumed a larger ring would be written past by the dispatch macros.
 *
 * THE TRAP POLICY IS THE ADAPTATION. Upstream logs an unresolved indirect call,
 * sets `eax = 0` and carries on; its own notes call silent-zero a top-three
 * failure mode, and §9 item 1 makes trapping instead the highest-priority change.
 * The three `*_log` functions below do not return. That is the change, applied
 * without editing a single line of generated code -- the macros call them before
 * they zero `eax`, so never coming back is enough.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "host_runtime.h"
#include "kernel_clock.h"
#include "recomp_abi.h"

/* Part of the generated header -- the MMX register file among it -- sits behind
 * RECOMP_GENERATED_CODE, so without this the symbols we are here to define are
 * not even declared and the cross-check silently covers less than it appears to.
 * The same guard also turns on the bare-register aliases (`#define eax g_eax`),
 * which is why no identifier below is named after an x86 register. */
#define RECOMP_GENERATED_CODE 1

/* The generated view. Must come after recomp_abi.h so a mismatch is reported
 * against the hand-written declaration, which is the one a human maintains. */
#include "recomp_types.h"

/* --- memory model ------------------------------------------------------- */

/* Zero: the guest is identity-mapped into the low 4 GB, so a guest address is
 * its own host address. The base-offset form is a strict generalisation we do
 * not use. */
ptrdiff_t g_xbox_mem_offset = 0;

/* Filled in from the XBE's own section table before any guest code runs. Left at
 * zero they would disable the lifted code's garbage-pointer check entirely. */
uint32_t g_xbox_code_lo = 0u;
uint32_t g_xbox_code_hi = 0u;

/* Upstream's --force-return escape hatch. We never generate forced returns, so
 * this stays off; it exists because the generated code references it. */
int g_force_return = 0;

/* --- integer registers -------------------------------------------------- */

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp;
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS int g_df;
RECOMP_TLS uint32_t g_fs_base;

/* --- x87 ---------------------------------------------------------------- */

/* 64-bit doubles where the hardware has 80-bit extended precision. A known,
 * accepted divergence: §7.3 of the evaluation records it, and nothing bit-exact
 * depends on it yet. */
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
/* T973 xemu reference: nxdk main and new thread callback both read 0x027F
 * (all exceptions masked, nearest rounding, 53-bit precision). The CRT saves
 * this word before temporarily changing it; zero falsely requests exceptions.
 * A TLS initializer gives each actual guest thread its own kernel initial state. */
RECOMP_TLS uint16_t g_fp_control_word = 0x027Fu;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;

/* --- flag bridge (T554) ------------------------------------------------- */

/* The flags a provider's ret publishes for the consumer its caller calls next
 * (lifter patch 21, tools/config/flag_bridge.json): a modelled CF PF ZF SF OF word
 * and the mask of bits it could answer. The consumer clears the mask when it reads
 * them, so a stale word traps instead of answering. Per thread like the x87 state. */
RECOMP_TLS uint32_t g_flag_bridge_eflags;
RECOMP_TLS uint32_t g_flag_bridge_mask;

/* --- SSE and MMX -------------------------------------------------------- */

RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

/* --- indirect-call instrumentation -------------------------------------- */

/* A 16-entry ring of the most recent indirect targets. Small, and that is fine:
 * it answers "what was the guest about to call when it died", which is the only
 * question it is asked. */
_Static_assert(ICALL_TRACE_RING == ICALL_TRACE_SIZE,
               "the host's view of the indirect-call ring must match the generated "
               "code's, which masks its index with ICALL_TRACE_SIZE-1 and would "
               "otherwise write outside the array this file defines");

volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

/* --- the trap policy ---------------------------------------------------- */

void recomp_icall_fail_log(uint32_t va)
{
    host_run_stop(HOST_STOP_ICALL_UNRESOLVED, va, 0u,
                  "no lifted, manual or kernel function at this address");
}

void recomp_icall_not_code_log(uint32_t va)
{
    host_run_stop(HOST_STOP_ICALL_NOT_CODE, va, 0u,
                  "target is outside every executable section");
}

/* An untranslated instruction is a stop, with ONE exception: `wbinvd`.
 *
 * `wbinvd` (`0F 09`) writes back and invalidates every cache line. Its only effect
 * is on cache state, never on the CONTENTS of memory: the host's own caches are
 * coherent across every guest thread, and nothing here reads memory around them
 * (no DMA engine, and the GPU is HLE in src/gpu). A no-op is therefore the exact
 * semantics for the guest's observable state, with timing the only difference. The
 * title's intent on hardware is INFERRED to be coherency before a device reads
 * memory. MEASURED: 10 sites are lifted, among them the sixth instruction of the
 * frame loop (0x0003D379), so trapping would end every run at its first frame.
 *
 * It is not silent, because a silent no-op is the failure this file's header names.
 * Each distinct site is announced the first time it runs and every hit is counted
 * (recomp_wbinvd_total, recomp_wbinvd_site_hits), so a run can be judged on how often
 * the title flushed rather than on whether it survived.
 *
 * ONLY `recomp_unimpl` honours it, and only the lifted target compiled with
 * RECOMP_UNIMPL_CONTINUE reaches `recomp_unimpl`. The noreturn `recomp_unimpl_trap`
 * stays an unconditional stop on purpose: it is declared RECOMP_NORETURN, so a call
 * to it cannot be returned from, and a fall-through there is undefined behaviour. */
#define WBINVD_SITE_CAP 64u

static pthread_mutex_t wbinvd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t va;
    uint64_t hits;
} wbinvd_sites[WBINVD_SITE_CAP];
static size_t wbinvd_site_total;
static uint64_t wbinvd_hits_total;
static uint64_t wbinvd_hits_uncapped;

static void note_wbinvd(uint32_t va)
{
    bool announce = false;
    bool overflow = false;
    size_t found = wbinvd_site_total;

    pthread_mutex_lock(&wbinvd_lock);
    wbinvd_hits_total++;
    for (size_t index = 0; index < wbinvd_site_total; index++) {
        if (wbinvd_sites[index].va == va) {
            found = index;
            break;
        }
    }
    if (found < wbinvd_site_total) {
        wbinvd_sites[found].hits++;
    } else if (wbinvd_site_total < WBINVD_SITE_CAP) {
        wbinvd_sites[wbinvd_site_total].va = va;
        wbinvd_sites[wbinvd_site_total].hits = 1u;
        wbinvd_site_total++;
        announce = true;
    } else {
        overflow = wbinvd_hits_uncapped++ == 0u;
    }
    pthread_mutex_unlock(&wbinvd_lock);

    if (announce) {
        fprintf(stderr,
                "recomp: wbinvd at 0x%08X treated as a no-op (cache flush: the host's caches "
                "are coherent and no device reads around them); once per site, hits counted\n",
                va);
    } else if (overflow) {
        fprintf(stderr, "recomp: wbinvd site table full (%u sites); further new sites are "
                        "counted but not announced\n", (unsigned)WBINVD_SITE_CAP);
    }
}

uint64_t recomp_wbinvd_total(void)
{
    pthread_mutex_lock(&wbinvd_lock);
    const uint64_t total = wbinvd_hits_total;
    pthread_mutex_unlock(&wbinvd_lock);
    return total;
}

size_t recomp_wbinvd_site_count(void)
{
    pthread_mutex_lock(&wbinvd_lock);
    const size_t count = wbinvd_site_total;
    pthread_mutex_unlock(&wbinvd_lock);
    return count;
}

uint64_t recomp_wbinvd_site_hits(uint32_t va)
{
    uint64_t hits = 0u;
    pthread_mutex_lock(&wbinvd_lock);
    for (size_t index = 0; index < wbinvd_site_total; index++) {
        if (wbinvd_sites[index].va == va) {
            hits = wbinvd_sites[index].hits;
            break;
        }
    }
    pthread_mutex_unlock(&wbinvd_lock);
    return hits;
}

void recomp_wbinvd_reset(void)
{
    pthread_mutex_lock(&wbinvd_lock);
    memset(wbinvd_sites, 0, sizeof(wbinvd_sites));
    wbinvd_site_total = 0u;
    wbinvd_hits_total = 0u;
    wbinvd_hits_uncapped = 0u;
    pthread_mutex_unlock(&wbinvd_lock);
}

static RECOMP_NORETURN void unimplemented_stop(const char *text, uint32_t va)
{
    host_run_stop(HOST_STOP_UNIMPLEMENTED, va, 0u, text ? text : "?");
    __builtin_unreachable();
}

void recomp_unimpl(const char *text, uint32_t va)
{
    if (text && strcmp(text, "wbinvd") == 0) {
        note_wbinvd(va);
        return;
    }
    unimplemented_stop(text, va);
}

/* The vendored lifter's own name for the same thing, introduced by lifter patch 01
 * (docs/lifter-patches/01-trap-instead-of-silent-continue.md). It is declared
 * RECOMP_NORETURN in recomp_types.h, and that is the whole point of the patch: an
 * unimplemented instruction must stop at its cause rather than setting eax = 0 and
 * continuing into plausible-looking wrongness. It stops for EVERY instruction,
 * `wbinvd` included: a noreturn function cannot hand control back. */
void recomp_unimpl_trap(const char *text, uint32_t va)
{
    unimplemented_stop(text, va);
}

/* A condition code the flag model could not resolve. Patch 02's whole argument is
 * that the old behaviour -- reading a zeroed _flags and silently taking the
 * not-taken branch -- deletes a conditional from the program. Stopping here is the
 * difference between a diagnosable halt and a wrong execution path. */
void recomp_flags_unresolved_trap(const char *cc, uint32_t va)
{
    host_run_stop(HOST_STOP_UNIMPLEMENTED, va, 0u, cc ? cc : "unresolved flags");
    __builtin_unreachable();
}

/* An indirect call or jump whose target could not be resolved to a lifted
 * function. Previously this set eax = 0 and continued.
 *
 * The reason is derived from `kind` rather than fixed, because the two cases are
 * different bugs and the old shared HOST_STOP_UNIMPLEMENTED reported both as
 * "unimplemented instruction" -- which named neither. A "not-code" target is a
 * garbage POINTER the guest computed (nothing is wrong with the translation);
 * anything else is a plausible code address with no function behind it, which is
 * a translation or dispatch gap. Misreporting the first as the second sent a
 * reader looking for a missing opcode that does not exist. */
void recomp_icall_unresolved_trap(uint32_t va, const char *kind)
{
    const bool not_code = kind && strcmp(kind, "not-code") == 0;
    host_run_stop(not_code ? HOST_STOP_ICALL_NOT_CODE : HOST_STOP_ICALL_UNRESOLVED, va,
                  0u, kind ? kind : "unresolved icall");
    __builtin_unreachable();
}

/* A function patch 05 proved non-returning did, in fact, return.
 *
 * This is the escape hatch for the one way that analysis can be wrong. The proof is
 * a structural CFG fixpoint and is one-sided by construction -- every uncertainty
 * answers "returns" -- so it cannot invent a non-returning function out of an
 * unrecovered branch. But it CAN be wrong where a recovered function BOUND is
 * wrong, and there are 1,028 end-bound disagreements on this image, so this is live
 * rather than theoretical. Reaching here names both addresses, which is exactly
 * what is needed to add one entry to --returns-anyway. */
void recomp_noreturn_returned(uint32_t callee_va, uint32_t site_va)
{
    host_run_stop(HOST_STOP_UNIMPLEMENTED, site_va, callee_va,
                  "a function proved noreturn returned");
    __builtin_unreachable();
}

/* --- remaining helpers -------------------------------------------------- */

/* `int 2D` and friends: the XDK debug-print service. Reaching one is interesting
 * but not fatal, so unlike the three above this one returns. */
void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    fprintf(stderr, "guest: debug service 0x%02X arg=0x%08X\n", service, arg_va);
}

/* `rdtsc`. The guest's one deterministic time base, shared with the title's own
 * QueryPerformanceCounter (which IS this instruction, MEASURED at 0x003D1267) and
 * meant to back KeQueryPerformanceCounter. Rate, what advances it and what is not
 * yet wired are in src/xbox/kernel_clock.h.
 *
 * It replaced `static uint64_t ticks; ticks += 1000u;`, which every guest thread
 * incremented with no exclusion and which was constant per read whatever the title
 * was doing. With no frame reported the new clock still moves 1000 per read, so a run
 * that never reaches a vblank is bit-identical to before. A counter that never
 * advances can hang a spin loop forever, which is why a read always creeps. */
uint64_t xbox_ReadTimeStampCounter(void)
{
    return kernel_clock_read();
}

/* Entry/exit tracing, emitted only for functions named by --trace-functions. We
 * pass none, so these are never called; they exist because the generated code
 * declares them unconditionally. */
void recomp_trace_enter(const char *name, uint32_t va)
{
    (void)name;
    (void)va;
}

void recomp_trace_exit(const char *name, uint32_t va)
{
    (void)name;
    (void)va;
}

void recomp_trace_esp(const char *name, const char *tag)
{
    (void)name;
    (void)tag;
}
