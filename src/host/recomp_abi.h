/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ABI the lifted code expects, declared by hand.
 *
 * WHY THIS FILE EXISTS. The generated `recomp_types.h` declares the same symbols,
 * but it arrives with 2.56 M lines of machine-written C that cannot survive this
 * project's warning flags, so it is compiled in a separate target with relaxed
 * ones. Hand-written host code must stay under the strict flags, which means it
 * cannot include the generated header. This is the strict-clean view of the same
 * ABI.
 *
 * HOW THE TWO ARE KEPT HONEST. `recomp_runtime.c` includes BOTH this header and
 * the generated `recomp_types.h`, and it is the translation unit that *defines*
 * every symbol. Any disagreement in type, linkage or thread-storage class is a
 * hard compile error there rather than a silent ABI break -- which is the only
 * failure mode that matters, because two C declarations that disagree link
 * perfectly and then corrupt state far from the cause.
 *
 * REGISTERS ARE THREAD-LOCAL AND 32-BIT. Every guest register, `g_esp` included,
 * is `uint32_t`. That is not an optimisation, it is the memory model: a guest
 * pointer is four bytes, and a host pointer stored in one truncates.
 */

#ifndef TSFP_HOST_RECOMP_ABI_H
#define TSFP_HOST_RECOMP_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Must expand to whatever the generated header's RECOMP_TLS expands to on this
 * compiler. The cross-check in recomp_runtime.c is what enforces that. */
#if defined(__GNUC__) || defined(__clang__)
#define TSFP_RECOMP_TLS __thread
#else
#define TSFP_RECOMP_TLS _Thread_local
#endif

/* Guest integer registers. */
extern TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern TSFP_RECOMP_TLS uint32_t g_ebp;

/*
 * Base of the guest's `fs` segment, which the lifted code reads as `XBOX_FS_BASE`.
 *
 * Thread-local like every other register, and that is exactly why one guest thread
 * per host thread works: each gets its own KPCR without any of this code having to
 * know a second thread exists.
 *
 * It is NOT optional state. Measured across the whole image, guest code touches
 * only `fs:[0x00]`, `fs:[0x20]`, `fs:[0x24]`, `fs:[0x28]` and `fs:[0x58]` -- but it
 * touches them 148 times, and with a zero base those are dereferences of small
 * integers. `src/xbox/kernel_thread.h` holds the measured layout and the evidence
 * for it; `kernel_thread_control_init` builds one.
 */
extern TSFP_RECOMP_TLS uint16_t g_fp_control_word;
extern TSFP_RECOMP_TLS uint32_t g_fs_base;

/* Base added to a truncated guest address to reach host memory. Zero under the
 * identity map, which is what this project ships. */
extern ptrdiff_t g_xbox_mem_offset;

/* Bounds of the title's executable sections. An indirect-call target outside
 * them (and outside the synthetic kernel window) is a garbage pointer, and the
 * lifted code drops it rather than calling it. Zero means "not loaded yet",
 * which the lifted code treats as allow -- so these must be set before any
 * guest code runs or the check is disabled. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* A lifted guest function. Every one of them is void(void): arguments travel on
 * the modelled guest stack and the return value comes back in g_eax, which is
 * what lets one table hold all 16,000-odd of them regardless of their original
 * calling convention. */
typedef void (*recomp_func_t)(void);

/* Generated: guest VA -> lifted function, or NULL. */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/* Generated: build the flat direct-indexed dispatch table. Optional; without it
 * recomp_lookup does a binary search and everything still works. */
int recomp_dispatch_init(void);

/* Generated: how many functions the lift produced. */
size_t recomp_get_count(void);

/* Ring of the most recent indirect-call targets, written by the dispatch macros.
 * It answers one question -- what was the guest about to call when it stopped --
 * which is why sixteen entries is enough.
 *
 * The size is the generated header's ICALL_TRACE_SIZE and must stay equal to it:
 * a host that assumed a larger ring would read past an array the generated code
 * writes with a mask. recomp_runtime.c asserts the two agree at compile time. */
#define ICALL_TRACE_RING 16
extern volatile uint32_t g_icall_trace[ICALL_TRACE_RING];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

/* `wbinvd` is the one untranslated instruction the runtime lets the guest run past: a
 * no-op, announced once per site and COUNTED. Defined in recomp_runtime.c with the
 * reasoning. The accessors are for the run report and the suite; `reset` is for tests. */
uint64_t recomp_wbinvd_total(void);
size_t recomp_wbinvd_site_count(void);
uint64_t recomp_wbinvd_site_hits(uint32_t va);
void recomp_wbinvd_reset(void);

#endif /* TSFP_HOST_RECOMP_ABI_H */
