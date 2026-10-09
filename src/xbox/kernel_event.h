/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Events, timers and DPCs.
 *
 * VERIFIED ORDINAL NUMBERS, resolved against tools/kernel_ordinals.py rather than
 * recalled -- context.md section 6m records four being misremembered in a single
 * task, and 249 being used for ObfReferenceObject when it is 251:
 *
 *   97   KeCancelTimer           -- 27 call sites
 *   107  KeInitializeDpc         -- 14 call sites
 *   113  KeInitializeTimerEx     -- 10 call sites
 *   119  KeInsertQueueDpc        --  7 call sites
 *   128  KeQuerySystemTime       -- 18 call sites
 *   137  KeRemoveQueueDpc        --  5 call sites
 *   145  KeSetEvent              -- 27 call sites
 *   149  KeSetTimer              -- 20 call sites
 *   150  KeSetTimerEx            -- five slots at retail 00439FB8 (T1136)
 *   159  KeWaitForSingleObject   --  9 call sites
 *   228  NtSetSystemTime         --  1 call site (T8f), see kernel_event_set_system_time
 *
 * NOT BOUND HERE, and each absence is a measurement rather than an omission. Counts
 * are from generated/retail/ordinal_callsites.json:
 *
 *   108  KeInitializeEvent       -- 0 sites. See the note below: this is the most
 *                                  informative absence in the whole tranche.
 *   138  KeResetEvent            -- 0 sites.
 *   123  KePulseEvent            -- 0 sites.
 *        KeInitializeTimer       -- not an export of this kernel at all; the name
 *                                  resolves to nothing in tools/kernel_ordinals.py,
 *                                  so only the Ex form exists to be called.
 *   158  KeWaitForMultipleObjects-- 1 site. Not needed to make this group coherent.
 *   99   KeDelayExecutionThread  -- 3 sites. A sleep, not part of the event/timer
 *                                  object group.
 *
 * WHY KeInitializeEvent HAVING ZERO CALL SITES MATTERS. 27 calls set events that
 * nothing in the image ever initialises through the kernel. The initialisation is
 * therefore open-coded in the title -- the same thing guest_structs.h found for
 * RtlInitAnsiString at sub_0037C9C4. The consequence for us is specific: we never
 * see an event's TYPE (notification versus synchronisation) go past, so we cannot
 * know it, and any behaviour that depends on it is not modelled. Said plainly here
 * rather than guessed at in the code.
 *
 * ===========================================================================
 * DEFAULT WITHOUT DISPATCH ADAPTER. Read before trusting module returns.
 * ===========================================================================
 *
 *   An opt-in elapsed-clock scheduler is available below. Its coalescing and
 *   SystemArgument values are INFERRED; no production adapter is installed yet.
 *
 *   1. NO TIMER EVER FIRES BY DEFAULT. KeSetTimer records a due time and returns; nothing
 *      expires it, no DPC is queued when it would have elapsed, and no event is
 *      signalled by its passing. A guest waiting for a timer to elapse waits
 *      forever as far as this module is concerned.
 *   2. NO DPC ROUTINE IS EVER CALLED. KeInitializeDpc records the routine address
 *      and KeInsertQueueDpc records that it was queued. Neither invokes anything.
 *      Running one would mean re-entering lifted guest code at dispatch IRQL from
 *      inside a kernel call, which this host has no mechanism for.
 *   3. NO WAIT EVER BLOCKS. KeWaitForSingleObject returns immediately, always. It
 *      cannot block: with no timer firing and no DPC running, nothing exists that
 *      could ever wake it, so a real block would be a guaranteed hang rather than a
 *      wait. Every non-signalled wait is REPORTED, and the count is available
 *      through kernel_event_unhonoured_wait_count() so a run can be judged on how
 *      much it depended on waiting rather than on whether it survived.
 *   4. EVENT TYPE IS HONOURED. A satisfied KeWaitForSingleObject on a Type 1
 *      (SynchronizationEvent) resets SignalState to 0; Type 0 (NotificationEvent)
 *      stays signalled. Other Type values are left alone (docs/clean-sources-audit.md
 *      section 2.2).
 *   5. NO WAIT QUEUES, NO PRIORITY BOOST, NO IRQL ENFORCEMENT. KeSetEvent's
 *      Increment argument is recorded and otherwise ignored, and its Wait argument
 *      has no queue to defer.
 *   6. KeQuerySystemTime IS A REAL HOST CLOCK, not a virtual one. It is not slaved
 *      to any timer this module holds, so guest code that measures its own timer
 *      against it sees the clock advance while the timer never elapses.
 *
 * ===========================================================================
 * WHERE THE OBJECT STATE LIVES: PER STRUCTURE, ON THE EVIDENCE.
 * ===========================================================================
 *
 * This split is not a style choice. Each of the three structures was investigated
 * against the binary and got a different answer, and the rule applied throughout is
 * that a field goes into guest memory when, and only when, its offset is MEASURED.
 * `docs/lifter-patches/06-guest-struct-static-asserts.md` records upstream declaring
 * IO_STATUS_BLOCK at 16 bytes where the guest's is 8, overrunning by 8 into a
 * neighbouring local -- the right bytes written to the wrong place, which does not
 * fault and surfaces arbitrarily far away. That is what a guessed offset costs.
 *
 * KEVENT -- GUEST MEMORY, and it HAS to be. The 16-byte dispatcher-object layout was
 *   already derived in src/xbox/guest_structs.h from three static critical sections;
 *   the guest's 10 open-coded KeInitializeEvent sites reproduce it field for field.
 *   Host-side storage is not merely weaker here, it is WRONG, for two measured
 *   reasons: the guest clears SignalState itself with no kernel call before waiting
 *   (KeResetEvent is not even imported), and four events are STACK LOCALS whose
 *   addresses are reused by unrelated events. kernel_event.c carries the citations.
 *
 * KDPC -- ONE FIELD in guest memory, the rest host-side. The 16-bit Type at +0x00 is
 *   the only derivable field, and writing 0x13 to it is MANDATORY: the guest gates
 *   its own teardown on that comparison and silently skips KeRemoveQueueDpc when it
 *   fails. The other 24 of its 0x1C bytes are never touched anywhere in the image,
 *   so DeferredRoutine, DeferredContext and the system arguments stay host-side --
 *   nothing says where they live and this module does not guess.
 *
 * KTIMER -- ENTIRELY HOST-SIDE, and nothing is lost. Its size is measured (0x28, from
 *   eleven zero-gap neighbours) but NOT ONE BYTE of any of the 12 KTIMER objects is
 *   ever read or written anywhere in the image. The guest relies wholly on
 *   KeInitializeTimerEx/KeSetTimer/KeCancelTimer, so host-side state keyed by guest
 *   address is unobservable-equivalent. This module writes nothing into a KTIMER,
 *   which also means it cannot disturb the flush-packed neighbours that pin the 0x28.
 *
 * TOLERATED, because the image does it: the KDPC at 0x7713A4 and the KTIMER at
 * 0x7713C0 are used without ever being initialised through the kernel. They are BSS,
 * hence all-zero, so an absent Type tag is a real case and not a fault.
 */

#ifndef TSFP_XBOX_KERNEL_EVENT_H
#define TSFP_XBOX_KERNEL_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_hle.h"

/** What kind of object a tracked guest address is being used as. */
typedef enum {
    KERNEL_EVENT_OBJECT_NONE = 0,
    /* Seen at a KeSetEvent or KeWaitForSingleObject site. */
    KERNEL_EVENT_OBJECT_EVENT,
    /* Seen at a KeInitializeTimerEx, KeSetTimer or KeCancelTimer site. */
    KERNEL_EVENT_OBJECT_TIMER,
    /* Seen at a KeInitializeDpc, KeInsertQueueDpc or KeRemoveQueueDpc site. */
    KERNEL_EVENT_OBJECT_DPC,
} kernel_event_object_kind;

/** Everything this module knows about one guest object. */
typedef struct {
    kernel_guest_ptr address;
    kernel_event_object_kind kind;

    /* --- events --- */
    /* THE LAST SignalState WE OBSERVED, for diagnostics ONLY. It is NOT the authority
     * and must never be branched on: the authority is the dword in guest memory,
     * which the guest itself writes without telling us. Kept purely so a report can
     * say what an event looked like last time we touched it. */
    uint32_t last_seen_signal_state;
    /* The last Increment passed to KeSetEvent. Recorded, never acted on: there is no
     * wait queue to boost a priority in. */
    uint32_t last_increment;

    /* --- timers --- */
    /* The Type argument KeInitializeTimerEx was given. Recorded; no behaviour
     * depends on it, because nothing fires. */
    uint32_t timer_type;
    bool timer_set;
    /* The 64-bit DueTime as the guest passed it. NEGATIVE means a relative time in
     * 100-nanosecond units, which is what every site in this image uses; positive
     * would be an absolute system time. Stored raw and signed so neither reading is
     * baked in. */
    int64_t due_time;
    /* Period in milliseconds; zero denotes the one-shot KeSetTimer form. */
    int32_t timer_period_ms;
    uint64_t timer_deadline;
    /* The KDPC a timer was armed with, or 0. */
    kernel_guest_ptr timer_dpc;

    /* --- DPCs --- */
    kernel_guest_ptr dpc_routine;
    kernel_guest_ptr dpc_context;
    bool dpc_queued;
    uint64_t dpc_sequence;
    kernel_guest_ptr dpc_argument1;
    kernel_guest_ptr dpc_argument2;
} kernel_event_object;

/* Opt-in elapsed-clock scheduler. Callback must return normally (no longjmp),
 * and true only after executing all four DPC arguments at dispatch IRQL.
 * A refusal retains the pending DPC and returns false. Install/reset only while
 * guest execution is quiescent. Period coalescing is an INFERRED host policy. */
typedef bool (*kernel_event_dispatch)(kernel_guest_ptr routine,
    kernel_guest_ptr dpc, kernel_guest_ptr context, kernel_guest_ptr argument1,
    kernel_guest_ptr argument2, void *opaque);
void kernel_event_set_dispatch(kernel_event_dispatch dispatch, void *opaque);
bool kernel_event_service(unsigned *delivered);

/** Register this group's ordinals. Returns how many bound. */
size_t kernel_event_register(void);

/** Forget every tracked object and zero every counter. For shutdown and tests. */
void kernel_event_reset(void);

/** The tracked object at `address`, or NULL. Returns a COPY-safe read-only view. */
const kernel_event_object *kernel_event_object_at(kernel_guest_ptr address);

/**
 * Read a KEVENT's SignalState straight out of GUEST memory.
 *
 * This is the authority on whether an event is signalled, not the tracked record --
 * the guest writes this dword itself, with no kernel call, so anything cached is
 * potentially stale. Exposed so a test asserts against the same bytes the guest
 * reads rather than against our bookkeeping, which would otherwise be a test that
 * only proves we agree with ourselves.
 *
 * False when `address` is 0 or not usable guest memory. Never substitutes a zero,
 * because zero is a legitimate "not signalled".
 */
bool kernel_event_signal_state(kernel_guest_ptr address, uint32_t *out);

/** How many distinct guest objects are tracked. */
unsigned kernel_event_tracked_count(void);

/**
 * Waits that returned without the object being signalled.
 *
 * THE HEADLINE DIAGNOSTIC OF THIS MODULE. Every one of these is a place the guest
 * expected to block and did not, so it is the measure of how far a run can be
 * trusted. A run with zero of these never depended on waiting; a run with thousands
 * is not really running.
 */
uint32_t kernel_event_unhonoured_wait_count(void);

/** Timers armed. Nothing ever fires, so this only ever grows. */
uint32_t kernel_event_timers_armed(void);

/** DPCs queued. Nothing ever runs them, so this only ever grows. */
uint32_t kernel_event_dpcs_queued(void);

/**
 * The 64-bit system time KeQuerySystemTime would write, in 100ns units.
 *
 * Exposed so a test can pin the epoch and the monotonicity without reaching through
 * a guest frame.
 */
uint64_t kernel_event_system_time(void);

/**
 * NtSetSystemTime (228) rebases the guest's wall clock to `new_time` (100ns units since 1601).
 * The clock is the host clock plus one signed offset, so a second set REBASES and does not
 * stack on the first, and the monotonic guard restarts so a set BACKWARD does not stall the
 * clock for the length of the step. False (no change) when the host clock cannot be read,
 * because the offset cannot be computed. The caller has already judged `new_time` (non-zero,
 * top bit clear).
 */
bool kernel_event_set_system_time(uint64_t new_time);

/** How many times the guest has set the clock this run. Cleared by `kernel_event_reset`. */
unsigned kernel_event_system_time_set_count(void);

/** The current signed offset from the host clock, in 100ns units. 0 until the guest sets it. */
int64_t kernel_event_system_time_offset(void);

#endif /* TSFP_XBOX_KERNEL_EVENT_H */
