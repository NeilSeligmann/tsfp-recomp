/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shadow `recomp_funcs.h`, copied into the stub build directory under that name
 * so the generated chunks pick it up instead of the real one.
 *
 * WHY IT IS SHAPED LIKE THIS
 * --------------------------
 * A generated chunk emits a direct call as
 *
 *     PUSH32(esp, 0x00032F6Cu); RECOMP_ABI_CALL(0x003ABE60u, sub_003ABE60);
 *
 * so every genuine `call` in the lifted code -- direct, and indirect once the
 * dispatch has resolved -- passes through that one macro, WITH the callee's
 * guest VA as its first argument. That is the only interception point the
 * subject side offers: a direct call is a native C call to a sibling symbol, so
 * there is nothing to hook at run time.
 *
 * The macro cannot be overridden with -D, because `recomp_types.h` defines it
 * unconditionally in both arms of an #ifdef. It cannot be shadowed by putting a
 * replacement `recomp_types.h` on the -I path either: the chunks include only
 * "recomp_funcs.h", and THAT file's own quoted include of "recomp_types.h"
 * resolves relative to its own directory, reaching the real header before any
 * -I directory is consulted. Shadowing the header the chunk names directly is
 * what works, which is why this file impersonates recomp_funcs.h and pulls the
 * real one in under an alias.
 *
 * Nothing in the generated tree or in src/ is modified: the stub build
 * symlinks the generated sources into a scratch directory and drops this file
 * beside them.
 */
#include "recomp_funcs_real.h"

#include <stdint.h>

/* The per-case plan distinguishes synthetic stubs from an explicitly proven
 * stack-probe passthrough. Both remain behind strict interception.
 * Replaces the callee with the synthetic behaviour described in call_stub.h.
 * `fn` is still passed so that stubbing can be switched off per run, in which
 * case the real callee is invoked and the subject behaves exactly as before. */
void harness_stub_call(uint32_t va, void (*fn)(void));

#undef RECOMP_ABI_CALL
#define RECOMP_ABI_CALL(va, fn) harness_stub_call((uint32_t)(va), (fn))

/* The dispatch-table unit defines its own lookup types after this include and
 * contains no call sites; it cannot instantiate these generated-code helpers. */
#ifndef RECOMP_DISPATCH_H
/* Capture the generated SAFE macros before replacing them. In an unstubbed run
 * their code-range checks, lookup order, stack recovery and feedback remain exact. */
#ifndef eax
#define eax g_eax
#define HARNESS_DEFINED_EAX_ALIAS
#endif
static inline void harness_real_icall_safe(uint32_t va, uint32_t saved_esp)
{
    RECOMP_ICALL_SAFE(va, saved_esp);
}

static inline void harness_real_icall_safe_at(uint32_t va, uint32_t saved_esp, uint32_t site)
{
    RECOMP_ICALL_SAFE_AT(va, saved_esp, site);
}
#ifdef HARNESS_DEFINED_EAX_ALIAS
#undef eax
#undef HARNESS_DEFINED_EAX_ALIAS
#endif

extern int g_stub_strict;

/* Manual direct calls also use SAFE. Strict comparisons must stub the exact
 * targets in the case's plan before a host lookup or code-range policy can reject
 * them. An unplanned (including computed) target still records STUB-MISS; it is
 * never permitted to run a real callee. NULL is safe only in strict mode. */
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va, saved_esp) do { \
    if (g_stub_strict) harness_stub_call((uint32_t)(va), NULL); \
    else harness_real_icall_safe((uint32_t)(va), (uint32_t)(saved_esp)); \
} while (0)

#undef RECOMP_ICALL_SAFE_AT
#define RECOMP_ICALL_SAFE_AT(va, saved_esp, site) do { \
    if (g_stub_strict) harness_stub_call((uint32_t)(va), NULL); \
    else harness_real_icall_safe_at((uint32_t)(va), (uint32_t)(saved_esp), (uint32_t)(site)); \
} while (0)
#endif
