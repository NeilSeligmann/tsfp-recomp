/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * IRQL bookkeeping: the single highest-value tranche of kernel HLE work.
 *
 * MEASURED (`context.md` §6p): `KfLowerIrql` has 133 call sites and
 * `KeRaiseIrqlToDpcLevel` 112, out of 968 kernel call sites in the whole image.
 * Those two alone are a quarter of every kernel call the game makes.
 *
 * WHAT IRQL IS, AND WHY A STUB IS NOT ENOUGH. On the real Xbox the interrupt
 * request level gates which interrupts can preempt the current code. We have no
 * interrupts to gate, so nothing here *does* anything to scheduling. But the value
 * is not inert: guest code is written as
 *
 *     KIRQL old = KeRaiseIrqlToDpcLevel();
 *     ... critical work ...
 *     KfLowerIrql(old);
 *
 * and the value handed back to `KfLowerIrql` is the one this module returned. A
 * stub returning a constant makes every such pairing lie, and the retail kernel
 * bugchecks on a lower-to-a-higher-level, which is a real bug class we want to keep
 * catching rather than silently paper over. So the level is tracked honestly and
 * misuse is reported.
 *
 * CALLING CONVENTION. `Kf*` is **__fastcall**: the argument arrives in ECX, not on
 * the stack. See `kernel_call.h` -- this is measured from real call sites, and a
 * handler that reached for `kernel_frame_arg(0)` would read the return address.
 *
 * THE LEVEL IS PER-THREAD, and the guest's copy is published on every change.
 *
 * Two bugs lived here and both are measured. First, `current_irql` was a plain
 * `static`, which became wrong the moment `kernel_thread.c` started calling
 * `pthread_create`: two guest threads shared one level. It is thread-local now.
 *
 * Second, and worse: the guest reads its OWN copy of the level, as a byte at
 * `KPCR.Irql`, i.e. `fs:[0x24]`. **Our generated code does this at 6 sites**
 * (`sub_0037E9A7`, `sub_0037E9CF`, `sub_0037FEB5`, `sub_004067F0`, `sub_004069FE`,
 * `sub_0040BAA2`), each comparing against DISPATCH_LEVEL and branching. We wrote
 * that byte once at thread creation and never again, so against 245 raise/lower
 * call sites all six comparisons permanently took the PASSIVE arm. A handler can be
 * perfectly correct and still be ignored if the guest is reading a stale copy.
 *
 * So every change publishes through `kernel_sync_set_irql_publisher`. The host wires
 * it, because writing `fs:[0x24]` needs the lifted runtime's per-thread `fs` base and
 * this layer must not depend on the lifted runtime.
 *
 * STILL NOT MODELLED, and it is a design problem rather than a missing keyword: the
 * guest uses raise-to-DISPATCH **as a lock**, which worked on a uniprocessor Xbox.
 * Thread-local is the right answer to "what is my IRQL" and the wrong answer to the
 * question a device model has to ask, so with real device models this will need a
 * cross-thread notion of "someone is raised". Upstream reached the same conclusion
 * independently and their partial fix is still best-effort; see
 * `docs/upstream-branch-review-2026-10-01.md`.
 */

#ifndef TSFP_XBOX_KERNEL_SYNC_H
#define TSFP_XBOX_KERNEL_SYNC_H

#include <stdint.h>

/* The levels this build actually uses. The hardware defines 0..31; these are the
 * named ones the kernel exports refer to. */
#define KERNEL_IRQL_PASSIVE 0u
#define KERNEL_IRQL_APC 1u
#define KERNEL_IRQL_DISPATCH 2u
#define KERNEL_IRQL_HIGH 31u

/** Register the IRQL ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_sync_register(void);

/** The current IRQL. Exposed for tests and diagnostics, not for guest use. */
uint32_t kernel_sync_current_irql(void);

/** Reset to PASSIVE_LEVEL. For tests, so one case cannot leak into the next. */
void kernel_sync_reset(void);

/**
 * Install the sink that writes the guest's own copy of the level.
 *
 * Called on every change, including the initial reset, so the guest's `KPCR.Irql`
 * byte never disagrees with ours. Passing NULL detaches it, which is what the unit
 * tests do -- they assert on the published values instead of on guest memory, so the
 * suite needs no mapped guest page.
 */
void kernel_sync_set_irql_publisher(void (*publish)(uint32_t level));

/**
 * T370: raise the calling thread's level to `level` and return the previous one, for host code
 * that runs guest code in a context the original runs at a higher level (the vblank callback at
 * DISPATCH_LEVEL). Never lowers, publishes like the guest's own raise.
 */
uint32_t kernel_sync_raise_irql(uint32_t level);

/**
 * T370: put the calling thread's level back to exactly `previous`, whatever the guest code in
 * between did to it (an unbalanced raise or lower included), and publish it. Not a guest lower:
 * it never counts a mismatched pairing.
 */
void kernel_sync_restore_irql(uint32_t previous);

/**
 * How many times a lower-to-a-higher-level was attempted.
 *
 * The real kernel bugchecks on this. We report and clamp instead, because taking
 * down the host process on a bookkeeping disagreement would lose the diagnostic,
 * but the count must be visible or the bug becomes invisible.
 */
uint32_t kernel_sync_bad_lower_count(void);

#endif /* TSFP_XBOX_KERNEL_SYNC_H */
