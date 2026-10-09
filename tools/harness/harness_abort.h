/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared abort/trap plumbing between the differential harness and the minimal
 * runtime.
 *
 * NOT ARMED BY driver.c. See the "WHICH NEVER RETURN ACTUALLY HAPPENS HERE"
 * comment in runtime_min.c: the harness scores a trap through its SIGABRT
 * handler, not through this jump buffer. The declarations below exist because
 * runtime_min.c defines the symbols, not because anything arms them.
 *
 * The patched lifter emits calls to runtime helpers whose documented contract is
 * "print to stderr and abort": recomp_unimpl_trap, recomp_flags_unresolved_trap,
 * recomp_icall_unresolved_trap. Aborting is correct for a standalone program but
 * fatal for a harness that streams thousands of cases through one process, so
 * the runtime routes the abort through a jump buffer the harness arms. Semantics
 * are preserved: the trap functions still never return to their caller (they are
 * noreturn), the message still goes to stderr, and the case is scored as having
 * no verdict rather than silently agreeing.
 *
 * If no harness has armed the buffer (g_recomp_abort_armed == 0) the trap falls
 * back to a real abort(), which is the upstream contract for ordinary builds.
 */
#ifndef HARNESS_ABORT_H
#define HARNESS_ABORT_H

#include <setjmp.h>
#include <stdint.h>

/* Reason the guest-side execution was cut short. Kept in sync with the strings
 * the harness prints, which difftest consumes. */
enum {
    RECOMP_ABORT_NONE = 0,
    RECOMP_ABORT_CRASH,          /* SIGSEGV/SIGBUS/SIGFPE/SIGILL */
    RECOMP_ABORT_TIMEOUT,        /* watchdog: lifted code did not terminate */
    RECOMP_ABORT_TRAP            /* a recomp_*_trap helper fired */
};

extern sigjmp_buf    g_recomp_abort_jmp;
extern volatile int  g_recomp_abort_armed;
extern volatile int  g_recomp_abort_kind;
extern char          g_recomp_abort_msg[256];

/* Counters for the non-trapping (upstream, -DRECOMP_ALL_CONTINUE) variants.
 * A case that touches any of these has not been shown to agree, it has merely
 * not been shown to disagree, so the harness reports them per case. */
extern int g_harness_unimpl;
extern int g_harness_flags_unresolved;
extern int g_harness_icall_failures;

void recomp_abort_reset(void);

#endif /* HARNESS_ABORT_H */
