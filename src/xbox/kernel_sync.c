/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_sync.h for why IRQL is tracked rather than stubbed.
 *
 * ASSUMED SIGNATURES, from the Xbox kernel export list. An arity or convention
 * error here is silent, so if a disassembly of a call site contradicts one of
 * these, this comment is the thing to fix:
 *
 *   KIRQL __fastcall KfRaiseIrql(KIRQL NewIrql);        // ECX, returns old
 *   void  __fastcall KfLowerIrql(KIRQL NewIrql);        // ECX, no return
 *   KIRQL __stdcall  KeRaiseIrqlToDpcLevel(void);       // returns old
 *   KIRQL __stdcall  KeRaiseIrqlToSynchLevel(void);     // returns old
 *   KIRQL __stdcall  KeGetCurrentIrql(void);
 *
 * KIRQL is a UCHAR, returned in AL. We return a uint32_t through the dispatcher
 * and the caller uses the low byte, which matches what the call sites do
 * (`mov cl, al`).
 */

#include "kernel_sync.h"

#include "kernel_call.h"
#include "kernel_hle.h"

#define ORD_KeGetCurrentIrql 103u
#define ORD_KeRaiseIrqlToDpcLevel 129u
#define ORD_KeRaiseIrqlToSynchLevel 130u
#define ORD_KfRaiseIrql 160u
#define ORD_KfLowerIrql 161u

/* Per-thread: two guest threads must not share a level. A plain `static` here was
 * correct only while nothing called pthread_create, which stopped being true when the
 * thread model landed. */
static _Thread_local uint32_t current_irql = KERNEL_IRQL_PASSIVE;
static _Thread_local uint32_t bad_lower_count;

/* Writes the guest's own copy of the level. The guest reads it at 6 sites and
 * branches on it, so a change we do not publish is a change the guest never sees. */
static void (*irql_publisher)(uint32_t level);

static void publish(uint32_t level)
{
    if (irql_publisher) {
        irql_publisher(level);
    }
}

void kernel_sync_set_irql_publisher(void (*fn)(uint32_t level))
{
    irql_publisher = fn;
    /* Publish immediately so the guest's copy is never stale between installation
     * and the first raise. */
    publish(current_irql);
}

uint32_t kernel_sync_current_irql(void)
{
    return current_irql;
}

void kernel_sync_reset(void)
{
    current_irql = KERNEL_IRQL_PASSIVE;
    bad_lower_count = 0u;
    publish(current_irql);
}

uint32_t kernel_sync_bad_lower_count(void)
{
    return bad_lower_count;
}

/* A raise returns the PREVIOUS level, which the caller stores and hands back to
 * KfLowerIrql. Getting this backwards would make every raise/lower pair in the
 * game drift, so it is the thing the tests pin hardest. */
static uint32_t raise_to(uint32_t level)
{
    uint32_t previous = current_irql;
    if (level > current_irql) {
        current_irql = level;
        publish(current_irql);
    }
    return previous;
}

uint32_t kernel_sync_raise_irql(uint32_t level)
{
    return raise_to(level);
}

void kernel_sync_restore_irql(uint32_t previous)
{
    current_irql = previous;
    publish(current_irql);
}

static uint32_t handle_kf_raise_irql(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t level = 0u;
    if (!kernel_frame_reg_arg(frame, 0u, &level)) {
        /* __fastcall: the argument is in ECX. A caller that supplied no registers
         * is a bug in the dispatcher, not a guest passing PASSIVE_LEVEL, so report
         * it rather than silently raising to 0 (which would also be a no-op and
         * therefore invisible). */
        kernel_hle_log()("kernel: KfRaiseIrql called with no register arguments\n");
        return current_irql;
    }
    return raise_to(level & 0xFFu);
}

static uint32_t handle_kf_lower_irql(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t level = 0u;
    if (!kernel_frame_reg_arg(frame, 0u, &level)) {
        kernel_hle_log()("kernel: KfLowerIrql called with no register arguments\n");
        return 0u;
    }
    level &= 0xFFu;
    if (level > current_irql) {
        /* The real kernel bugchecks here. Clamp and count instead: killing the host
         * on a bookkeeping disagreement would destroy the diagnostic, but an
         * unreported mismatch means a real guest bug becomes invisible. */
        bad_lower_count++;
        kernel_hle_log()("kernel: KfLowerIrql(%u) raises from %u -- mismatched pairing\n",
                         (unsigned)level, (unsigned)current_irql);
        return 0u;
    }
    current_irql = level;
    publish(current_irql);
    return 0u;
}

/* ARITY-OK(129): takes no arguments at all, so there is no count to get wrong.
 * The measured 0 is low-confidence only because 113 call sites disagree about how
 * many pushes precede the call -- those are the callers' own saves, not arguments.
 * Corroborated by the published signature (KIRQL KeRaiseIrqlToDpcLevel(void)) and
 * by the call sites themselves: none passes anything in ECX either. */
static uint32_t handle_raise_to_dpc(void *context)
{
    (void)context;
    return raise_to(KERNEL_IRQL_DISPATCH);
}

static uint32_t handle_raise_to_synch(void *context)
{
    (void)context;
    /* Uniprocessor Xbox: synchronisation level IS dispatch level. */
    return raise_to(KERNEL_IRQL_DISPATCH);
}

static uint32_t handle_get_current_irql(void *context)
{
    (void)context;
    return current_irql;
}

unsigned kernel_sync_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_KeGetCurrentIrql, handle_get_current_irql},
        {ORD_KeRaiseIrqlToDpcLevel, handle_raise_to_dpc},
        {ORD_KeRaiseIrqlToSynchLevel, handle_raise_to_synch},
        {ORD_KfRaiseIrql, handle_kf_raise_irql},
        {ORD_KfLowerIrql, handle_kf_lower_irql},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
