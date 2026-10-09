/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Critical sections: ordinals 277 Enter, 294 Leave, 291 Initialize.
 *
 * WHY THIS IS THE NEXT BLOCKER. 294 is the second most-called ordinal in the whole
 * image (114 sites) and 277 is where the guest's main thread actually stops. The
 * first critical-section call the guest ever makes is at 0x0037FBF5, on the object
 * at 0x00549148 -- which is GUEST_CS_VA_TEXT, one of the three the XBE initialises
 * from image data and never passes to ordinal 291. So lazy adoption is not an edge
 * case to handle later; it is required by the very first call.
 *
 * THE LAYOUT IS NOT GUESSED HERE. Every offset comes from
 * `guest_rtl_critical_section` in guest_structs.h, whose `_Static_assert` set is
 * the safety net, and from the derivation in docs/guest-structs.md. This module
 * adds no opinion about the layout; in particular it NEVER reads or interprets the
 * two dwords at +0x14 and +0x18, whose ordering docs/guest-structs.md records as
 * UNMEASURABLE from this binary.
 *
 * WHERE THE STATE LIVES, AND WHY IT IS SPLIT.
 *
 *   - `lock_count` at +0x10 lives in GUEST memory, because that is where the XBE
 *     puts it and because a CS that arrives from image data has no host record to
 *     read. docs/guest-structs.md measured 0xFFFFFFFF there in all three static
 *     instances; we treat it as a signed counter that Enter raises and Leave
 *     lowers. The guest never reads it (a +/-0x80 operand-window scan around all
 *     five CS addresses found only whole-struct pointers), so this bookkeeping is
 *     for our diagnostics, not for the guest's benefit.
 *
 *   - OWNER AND RECURSION DEPTH live in HOST state, keyed by the critical
 *     section's guest address. They are deliberately NOT written into the guest
 *     struct. By analogy to NT they would belong at +0x14 and +0x18, but the
 *     analogy cannot say WHICH goes where: both dwords are 0 in all three static
 *     images and touched by nothing anywhere in the image, so choosing an order
 *     would be a guess presented as a measurement. The guest reads neither dword,
 *     so keeping them host-side costs nothing and keeps the header honest.
 *
 * THE HOST TABLE IS KEYED BY ADDRESS AND POPULATED ON FIRST TOUCH, NOT ON
 * Initialize. That distinction is the whole design. Roughly 105 of the 114 Leave
 * calls act on a lock that never passes through ordinal 291, so a table filled
 * only by Initialize would miss nearly every call; `adopt_nolock` therefore
 * creates an entry for any address Enter or Leave is handed, reading the guest's
 * own already-initialised fields.
 *
 * NOT A SPIN LOCK, AND THAT IS MEASURED. There is no inlined interlocked fast path
 * in this image: all 37 `lock`-prefixed instructions are misdisassembled padding
 * (the Xbox is uniprocessor) and the 114-vs-10 asymmetry is a shared 30-byte DSOUND
 * helper called by 52 functions, each calling Leave on both exits. So there is no
 * byte-for-byte sequence to reproduce and we are free to block on a host mutex.
 *
 * OUT OF SCOPE, ON THE RECORD. The DSOUND helper at 0x004069FE SKIPS the critical
 * section entirely when the IRQL byte at fs:[0x24] is raised, because on a
 * uniprocessor raising to DISPATCH_LEVEL was itself a lock. With two host threads
 * that mutual exclusion is simply gone. This module cannot fix that and does not
 * try: it locks when the guest asks it to and never when it does not, which leaves
 * the hazard exactly as documented rather than deeper.
 */

#ifndef TSFP_XBOX_KERNEL_CRITSEC_H
#define TSFP_XBOX_KERNEL_CRITSEC_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_hle.h"

/**
 * How many distinct critical sections we will track.
 *
 * The image has five. The margin is for heap locks the guest creates at runtime;
 * exhaustion is reported and counted rather than silently dropping a lock.
 */
#define KERNEL_CRITSEC_MAX 64u

/** A tracked critical section's host-side state, copied out for inspection. */
typedef struct {
    /** The guest address this entry is keyed on. */
    kernel_guest_ptr cs;
    /**
     * Who holds it: an opaque per-host-thread token, 0 for unheld.
     *
     * A HOST thread identity, not a guest thread id. Nothing in the image reveals
     * what the guest would consider an owning thread, and we do not need to know:
     * the only question Enter and Leave have to answer is "is the caller the same
     * thread that acquired this", which host identity answers exactly.
     */
    uint32_t owner;
    /** How many times `owner` has entered without leaving. 0 when unheld. */
    uint32_t recursion;
    /** True when we first met this CS outside ordinal 291, i.e. adopted it. */
    bool adopted;
    /** `lock_count` as it currently stands in guest memory at cs+0x10. */
    uint32_t guest_lock_count;
} kernel_critsec_info;

/* Typed access to the same genuine critical-section bodies used by ordinals
 * 277/294. Return and diagnostics preserve the ordinal behavior. */
uint32_t kernel_critsec_enter_guest(kernel_guest_ptr cs);
uint32_t kernel_critsec_leave_guest(kernel_guest_ptr cs);

/** Register the critical-section ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_critsec_register(void);

/**
 * Drop all tracked state.
 *
 * For tests and for a fresh run. Does NOT touch guest memory: a reset must not
 * invent an initial value for a critical section the guest owns.
 */
void kernel_critsec_reset(void);

/** How many distinct critical sections we are tracking. */
unsigned kernel_critsec_tracked_count(void);

/** Copy out one critical section's state. False when we have never seen it. */
bool kernel_critsec_state(kernel_guest_ptr cs, kernel_critsec_info *out);

/** The calling host thread's owner token. Stable for the life of the thread, never 0. */
uint32_t kernel_critsec_owner_token(void);

/** Critical sections adopted on first use rather than created by ordinal 291. */
uint32_t kernel_critsec_adopted_count(void);

/** Leaves of a critical section the caller did not hold. Should be 0. */
uint32_t kernel_critsec_bad_leave_count(void);

/** Enters that had to wait because another thread held the lock. */
uint32_t kernel_critsec_contended_count(void);

/** Enters that found the caller already holding the lock, so did not wait. */
uint32_t kernel_critsec_recursive_enter_count(void);

/** Adoptions whose guest header did not look like a critical section. */
uint32_t kernel_critsec_implausible_count(void);

/** Calls handed an address we could not read, so could not lock at all. */
uint32_t kernel_critsec_unreadable_count(void);

/** Calls refused because the tracking table was full. */
uint32_t kernel_critsec_table_full_count(void);

/** Initialize calls that found the critical section still held by somebody. */
uint32_t kernel_critsec_reinit_while_held_count(void);

/**
 * Host mutexes a reset had to abandon because they were still held.
 *
 * Cumulative across resets on purpose: a reset that cleared this counter would be
 * erasing the evidence of the thing the counter exists to report.
 */
uint32_t kernel_critsec_retired_count(void);

#endif /* TSFP_XBOX_KERNEL_CRITSEC_H */
