/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HAL ordinals: shutdown-notification registration.
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED. 47 is HalRegisterShutdownNotification and
 * 49 is HalReturnToFirmware, read out of `tools/kernel_ordinals.py` (the single source
 * of truth, derived from the kernel export table). 47 was never a drifting entry. 49
 * WAS -- it is the one case `SUSPECT_ON_XDK_5849` flagged -- and it is settled below,
 * so it now sits in `RESOLVED_ON_XDK_5849` with its evidence.
 *
 * WHY THIS IS THE NEXT THING THE GUEST WANTS. It is where a bring-up run stopped
 * with zero variance over 10 runs: thread 2 reaches it at guest 0x00381288 after
 * PsCreateSystemThreadEx, NtClose, RtlEnterCriticalSection and
 * RtlLeaveCriticalSection. `generated/retail/ordinal_callsites.json` counts 10 call
 * sites; there are in fact 12, see below.
 *
 * ARITY, MEASURED AT ALL 10 SITES RATHER THAN TAKEN FROM THE TABLE.
 *
 *     VOID __stdcall HalRegisterShutdownNotification(
 *             PVOID ShutdownRegistration, BOOLEAN Register);
 *
 * `generated/lifted/gen/kernel_arity.inc` carries `{47u, 2u, 10u, 0}` -- 2 stack
 * arguments over 10 sites, NOT unanimous -- and `stack_args_for()` in
 * src/host/kernel_thunk.c refuses a non-unanimous measurement, correctly, because
 * the minimum-across-sites rule under-counts when a call-site bracket opens late.
 * So the count was verified by hand at every one of the 10 sites (thunk slot
 * `MEM32(0x4758C0)`), and all 10 push exactly two arguments before the return
 * address:
 *
 *     0x00381288  push ebx(=1);      push 0x549650   -> (0x549650, 1)
 *     0x003DD216  push ebx;          push eax
 *     0x003DD703  push ebx;          push eax
 *     0x0040AE85  push ebx;          push edi
 *     0x0040B1FD  push 1;            push eax        -> register
 *     0x00410B74  push ebx;          push esi
 *     0x00410C83  push 1;            push eax        -> register
 *     0x0043AA41  push 0;            push eax        -> DEregister
 *     0x0043AFF7  push 1;            push eax        -> register
 *     0x0046D2F4  push edx;          push ecx
 *
 * TWO MORE SITES THE MEASURING TOOL CANNOT SEE, found while checking the above and
 * worth recording because they are the strongest evidence here. In sub_0043A... the
 * thunk slot is loaded into a register once and called through it twice:
 *
 *     edi = MEM32(0x4758C0);
 *     0x0043AC8B  push 1;  push esi   -> register   esi
 *     0x0043ACBB  push 0;  push esi   -> DEregister esi
 *
 * `tools/lift/callsites.py` keys on `MEM32(<thunk slot>)` appearing as the indirect
 * target, so a call through a register is invisible to it: the real site count is 12,
 * not the 10 it reports. Both of these push exactly 2 arguments as well, and the
 * SAME pointer `esi` is handed in with 1 and then with 0, which is a register/
 * deregister pair of one block -- that is the behaviour this module has to get right.
 *
 * stdcall pushes right to left, so the LAST push is argument 0: the registration
 * pointer, with the flag second. The five literal-flag sites (1 at 0x0040B1FD,
 * 0x00410C83, 0x0043AFF7 and 0x0043AC8B; 0 at 0x0043AA41 and 0x0043ACBB) confirm
 * which argument is which -- a 0/1 literal is a BOOLEAN and 0x549650 is a .data
 * address, not a boolean. That is the independent check the measured table cannot
 * give, and it is why `kernel_hal_shutdown_unmatched_count()` exists: a swapped
 * pair would show up there immediately.
 *
 * It returns VOID, so nothing reads the result; we return 0.
 *
 * NO GUEST STRUCT LAYOUT IS DERIVED, AND THAT IS A DELIBERATE CHOICE, NOT A GAP.
 * The real kernel threads the caller's block onto a priority-ordered kernel list,
 * which means writing link fields INTO the guest's structure. We do not, because
 * the list is only ever walked by the kernel at shutdown and we never shut down,
 * and because the guest never reads the block back: the one static registration in
 * the image is at 0x549650, and `MEM32(0x549650)` appears nowhere in the 2.56 M
 * lines of lifted code -- the address occurs exactly once, as the push above. So
 * the registration set is kept HOST-SIDE, keyed by the guest pointer, and
 * `docs/guest-structs.md` needs no new entry. If a later task finds the guest
 * reading its own link fields, that is the point at which the layout has to be
 * derived from those reads.
 *
 * WHAT IS THEREFORE NOT MODELLED, said plainly:
 *   - PRIORITY ORDERING. The real insert is ordered by a Priority field inside the
 *     block. Reading it would need the layout we just declined to guess, and
 *     nothing observes the order, so registrations are held unordered.
 *   - THE NOTIFICATION ITSELF. No routine is ever called, because nothing in this
 *     host initiates a shutdown. When HalInitiateShutdown (360) or
 *     HalReturnToFirmware (49) is implemented, this set is what it must walk.
 *
 * ===========================================================================
 * ORDINAL 49: IT REALLY IS HalReturnToFirmware, AND THAT WAS IN DOUBT.
 *
 * `tools/kernel_ordinals.py` lists 49 in SUSPECT_ON_XDK_5849 as ambiguous between
 * `HalReturnToFirmware` and `HalRequestSoftwareInterrupt`. The two are not
 * interchangeable: one never returns and reboots the console, the other returns and
 * lowers an interrupt request. Resolved here from the image, by three independent
 * lines of evidence, so the entry can come off that list.
 *
 *   1. ALL FOUR CALL SITES PUSH EXACTLY ONE STACK ARGUMENT.
 *      `HalRequestSoftwareInterrupt` is __fastcall: its KIRQL arrives in ECX and it
 *      pushes NOTHING. `generated/lifted/gen/kernel_arity.inc` carries
 *      `{49u, 1u, 4u, 1}` -- one stack argument, four sites, unanimous -- and four
 *      sites clears the three-site corroboration bar in `stack_args_for()`, so this
 *      one needs no hand-written ABI row. A fastcall export cannot produce four
 *      unanimous single-push sites.
 *
 *   2. AT 0x00381253 THE NEXT BYTE IS AN `int3` WITH NO INCOMING CONTROL FLOW.
 *      `loc_00381259` appears exactly once in the lifted source: its own label, with
 *      no `goto` anywhere. It is unreachable padding, and a compiler emits padding
 *      after a call only when it knows the call does not come back.
 *
 *   3. THE OTHER THREE SITES' FOLLOWING CODE IS NOT FALL-THROUGH EITHER.
 *      At 0x0037CBB3 the instruction after the call is `loc_0037CBB9`, reached ONLY
 *      by `goto loc_0037CBB9` from the `jl` at 0x0037CB98 -- the failure arm of the
 *      launch-data fill. It then calls ordinal 301, `RtlNtStatusToDosError`, with the
 *      failed status in EAX. That is a coherent error path that happens to sit after
 *      a non-returning call, not a continuation of it.
 *
 * The four measured arguments are 2, 2, 4 and 2 (all literals, all `push imm8`):
 *
 *     0x0037CBB3  push 2   in XLaunchNewImage, after filling the launch-data page
 *     0x003809E5  push 2
 *     0x00381253  push 4   the site followed by the int3
 *     0x00414806  push 2
 *
 * WHAT THE ROUTINE NUMBERS MEAN IS NOT DERIVED HERE. nxdk (CC0) names the
 * enumerators, but this module deliberately does not depend on that: it reports the
 * raw value, because the host's job is to STOP and say what was asked, and a wrong
 * name attached to a correct number would be worse than no name. See
 * `docs/provenance.md`.
 *
 * WHY A SINK RATHER THAN A RETURN VALUE. On hardware this call does not return -- the
 * console reboots. A handler that returned 0 would let the guest run on past its own
 * reboot, inventing a trace that no console could produce. src/xbox must not depend on
 * the host runtime, so the host installs a sink and that sink is what never comes
 * back. With no sink installed the call is reported and DOES return, which is only
 * correct for unit tests and says so loudly.
 */

#ifndef TSFP_XBOX_KERNEL_HAL_H
#define TSFP_XBOX_KERNEL_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_hle.h"

/** Register the HAL ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_hal_register(void);

/** Forget every registration and zero the counters. For tests. */
void kernel_hal_reset(void);

/** How many shutdown registrations are currently held. */
unsigned kernel_hal_shutdown_count(void);

/** Whether `registration` is currently held. */
bool kernel_hal_shutdown_registered(kernel_guest_ptr registration);

/**
 * How many times a block already held was registered again.
 *
 * On the real kernel this corrupts the list, because the same LIST_ENTRY ends up
 * threaded twice. We keep one entry and count, because reproducing the corruption
 * would destroy the evidence and silently tolerating it would hide a guest bug --
 * or, far more likely at this stage, a bug of ours in the arity or argument order.
 */
unsigned kernel_hal_shutdown_duplicate_count(void);

/**
 * How many times a block that was NOT held was deregistered.
 *
 * Same reasoning as the duplicate count. A nonzero value here with a zero
 * duplicate count is the signature of argument 0 and argument 1 being swapped,
 * which is the single most likely way to get this ordinal wrong.
 */
unsigned kernel_hal_shutdown_unmatched_count(void);

/** How many registrations were refused because the table was full. */
unsigned kernel_hal_shutdown_overflow_count(void);

/**
 * The registration slot bound.
 *
 * Exposed so the overflow test can fill the table without hard-coding the bound in
 * two places, which would let the two drift apart and quietly stop testing
 * overflow at all.
 */
unsigned kernel_hal_shutdown_capacity(void);

/**
 * Install the sink that handles a firmware return, or NULL to detach.
 *
 * `routine` is the raw value the guest pushed, unnamed on purpose.
 * `pending_notifications` is how many shutdown registrations the real kernel would
 * have walked before returning to firmware, passed so the sink can report a number
 * this layer knows and the host does not.
 *
 * THE SINK IS NOT EXPECTED TO RETURN. On hardware the console reboots here. If it
 * does return, the handler reports that and the run continues on borrowed credibility
 * -- which is why the count below exists.
 */
void kernel_hal_set_firmware_sink(void (*sink)(uint32_t routine,
                                               unsigned pending_notifications));

/**
 * How many times the guest asked to return to firmware.
 *
 * Should be 0 or 1 on any honest run: a second one means the first did not stop
 * anything, so every observation after it was made on a console that had already
 * rebooted.
 */
unsigned kernel_hal_firmware_return_count(void);

/** The routine value from the most recent firmware return, or 0 if there was none. */
uint32_t kernel_hal_last_firmware_routine(void);

/*
 * Ordinals 252 PhyGetLinkState (1 stdcall arg) and 253 PhyInitialize (2 stdcall args). There
 * is no Ethernet PHY: PhyInitialize FAILS (STATUS_NO_SUCH_DEVICE) and PhyGetLinkState reports
 * no link (0). Both are the title's own handled network-failure states, MEASURED from the
 * guest's use, see kernel_hal.c. Announced on the first call of each.
 */

/** PhyInitialize calls answered (all failures, since no PHY exists). */
unsigned kernel_hal_phy_initialize_count(void);

/** PhyGetLinkState calls answered with "no link". */
unsigned kernel_hal_phy_link_query_count(void);

#endif /* TSFP_XBOX_KERNEL_HAL_H */
