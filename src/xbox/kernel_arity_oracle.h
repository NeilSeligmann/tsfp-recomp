/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ordinal -> argument arity, derived from a CC0-1.0 module-definition file.
 * See kernel_arity_oracle.c for the source, its licence, and the decoration
 * grammar the numbers come from.
 *
 * HOW THIS IS MEANT TO BE USED, which is the whole design decision.
 *
 * It is a CROSS-CHECK and a LAST-RESORT FALLBACK, never an override. The ordering
 * in `stack_args_for()` is:
 *
 *     1. ABI_TABLE          hand-verified at this image's own call sites
 *     2. MEASURED_ARITIES   this image's call sites, behind quorum + unanimity gates
 *     3. this oracle        a name decoration from a DIFFERENT kernel build
 *     4. refuse, and stop the run
 *
 * The oracle is third DESPITE being structurally stronger evidence than a gated
 * measurement, because it is one file describing a kernel build that is not ours.
 * A hand row carries reasoning -- a forced stack balance, a literal that pins an
 * argument order -- that a decoration does not have, and if the two disagree the
 * hand row may well be the right one. So a disagreement is a FINDING, raised loudly
 * at test time by tests/test_arity_oracle.py, and never resolved silently in favour
 * of either side at runtime.
 *
 * It is third rather than absent because of what step 4 costs: an ordinal with no
 * usable count stops the run. The oracle turns some of those stops into correct
 * continuations, and a continuation whose provenance is NAMED in the diagnostic is
 * strictly better than a stop.
 */

#ifndef TSFP_KERNEL_ARITY_ORACLE_H
#define TSFP_KERNEL_ARITY_ORACLE_H

#include <stdbool.h>
#include <stddef.h>

/* How the export takes its arguments. Mirrors the `.def` decoration grammar. */
typedef enum {
    /* `Name@N`. All arguments on the stack, callee pops them. */
    KERNEL_ARITY_ORACLE_STDCALL = 0,
    /* `@Name@N`. First argument in ecx, second in edx, remainder on the stack.
     * The callee pops only the stack remainder. */
    KERNEL_ARITY_ORACLE_FASTCALL = 1,
    /* Undecorated. Variadic printf-family: the CALLER cleans up, so the callee
     * pops zero and the true argument count is not knowable from a decoration. */
    KERNEL_ARITY_ORACLE_CDECL = 2,
    /* `NONAME DATA`. A variable, not a function. HAS NO ARITY. */
    KERNEL_ARITY_ORACLE_DATA = 3,
} kernel_arity_oracle_cc;

typedef struct {
    unsigned ordinal;
    kernel_arity_oracle_cc convention;
    /* Dwords the CALLEE pops on return. Zero for cdecl because the caller cleans
     * up, and zero for DATA only because the field must hold something -- use the
     * convention to tell those two apart, never this field alone. */
    unsigned stack_args;
    /* Arguments in ecx/edx: 0, 1 or 2. Non-zero only for fastcall. */
    unsigned register_args;
} kernel_arity_oracle_entry;

/* NULL when the oracle has no row for this ordinal. */
const kernel_arity_oracle_entry *kernel_arity_oracle_lookup(unsigned ordinal);

/* Dwords a callee-cleanup thunk must pop. False when the oracle cannot say, which
 * is every DATA export and every ordinal it does not cover. */
bool kernel_arity_oracle_callee_pop(unsigned ordinal, unsigned *out_dwords);

/* Whole-table access, for the suites that sweep every row. */
unsigned kernel_arity_oracle_count(void);
const kernel_arity_oracle_entry *kernel_arity_oracle_at(unsigned index);

#endif /* TSFP_KERNEL_ARITY_ORACLE_H */
