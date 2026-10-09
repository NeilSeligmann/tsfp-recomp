/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_event.h for the verified ordinal numbers, the ordinals deliberately
 * left unbound, the full list of WHAT IS NOT MODELLED, and why the object state is
 * host-side rather than written into guest memory. That header is the contract; this
 * file is only how it is met.
 *
 * SIGNATURES, WITH EVERY STACK-ARGUMENT COUNT MEASURED FROM THIS IMAGE. Our handler
 * IS the __stdcall callee, so a wrong count does not fail loudly -- it desyncs the
 * guest's esp permanently and the damage appears arbitrarily far away. None of these
 * is in the `Kf*` or `Obf*` fastcall families (see src/xbox/kernel_call.h), so every
 * argument is on the stack.
 *
 *   LONG     KeSetEvent(PRKEVENT Event, KPRIORITY Increment, BOOLEAN Wait);     // 3
 *   BOOLEAN  KeCancelTimer(PKTIMER Timer);                                      // 1
 *   BOOLEAN  KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);       // 4
 *   VOID     KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type);               // 2
 *   VOID     KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Ctx); // 3
 *   BOOLEAN  KeInsertQueueDpc(PKDPC Dpc, PVOID Arg1, PVOID Arg2);               // 3
 *   BOOLEAN  KeRemoveQueueDpc(PKDPC Dpc);                                       // 1
 *   NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON, KPROCESSOR_MODE,
 *                                  BOOLEAN Alertable, PLARGE_INTEGER Timeout);  // 5
 *   VOID     KeQuerySystemTime(PLARGE_INTEGER CurrentTime);                     // 1
 *
 * KeSetTimer's DueTime is a LARGE_INTEGER passed BY VALUE, so it occupies TWO of the
 * four slots. That is measured, not assumed -- see the ARITY-OK(149) note.
 */

#include "kernel_event.h"

#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_sync.h"
#include "kernel_hle.h"
#include "nt_status.h"

/* ===========================================================================
 * KEVENT LIVES IN GUEST MEMORY, AND IT HAS TO.
 * ===========================================================================
 *
 * The 16-byte dispatcher-object layout below is NOT invented here and is not new:
 * src/xbox/guest_structs.h already derives it from three statically initialised
 * critical sections in the image, with GUEST_EVENT_VA_CONTROL (0x00549660) as the
 * bare-event control that bounded it at 16 bytes. The static asserts at the end of
 * this block tie these offsets to that reviewed derivation, so if it ever changes
 * this file stops compiling instead of silently disagreeing.
 *
 * WHY THE STATE CANNOT BE HOST-SIDE. An earlier version of this module kept the
 * signalled flag in a host table keyed by guest address. That is OBSERVABLY WRONG
 * here, for two independent reasons measured in this image:
 *
 *   1. THE GUEST CLEARS SignalState ITSELF, WITH NO KERNEL CALL. KeResetEvent
 *      (ordinal 138) is not even imported, so the title open-codes the reset.
 *      recomp_0053.c, sub_003D3550:
 *          eax = MEM32(0x3E3F58);          // the D3D device
 *          MEM32(eax + 0x1DC0) = 0;        // <- SignalState := 0, in guest memory
 *          eax = eax + 0x1DBC;             // the KEVENT itself
 *          ... call MEM32(0x475854)        // KeWaitForSingleObject on it
 *      0x1DC0 is exactly 0x1DBC + 4, i.e. the SignalState dword of the very event
 *      being waited on, six instructions later. A host-side flag would still say
 *      "signalled" and the wait would be reported as satisfied when the guest had
 *      just deliberately cleared it. The same pattern appears in sub_003D6870.
 *   2. FOUR EVENTS ARE STACK LOCALS (ebp-0x14 twice, ebp-0x18, ebp-0x28). The same
 *      guest address is therefore reused by unrelated events in different calls, so
 *      a host table keyed on the pointer carries one event's signalled state into
 *      another's. That is a wrong answer that cannot be detected from inside.
 *
 * The guest also open-codes KeInitializeEvent (ordinal 108, which has ZERO call
 * sites) in 10 places, every one writing Type at +0x00, Size = 4 at +0x02,
 * SignalState at +0x04, and a self-linked wait list at +0x08/+0x0C -- which is what
 * independently reproduces guest_structs.h's layout field for field.
 *
 * KTIMER AND KDPC ARE DIFFERENT, and are treated differently below. See the notes on
 * each.
 */

/* +0x00, one byte. Written as a literal 0 or 1 by all 10 open-coded initialisations.
 * The MEANING of 0 versus 1 is NT analogy and is NOT measured -- see the comment in
 * the wait handler for why nothing here depends on it. */
#define GUEST_EVENT_OFFSET_TYPE 0x00u
/* +0x04, one dword. The signalled count. */
#define GUEST_EVENT_OFFSET_SIGNAL_STATE 0x04u
/* The whole dispatcher object. Nothing here writes outside it. */
#define GUEST_EVENT_BYTES 16u

_Static_assert(offsetof(guest_rtl_critical_section, event_type)
                   == GUEST_EVENT_OFFSET_TYPE,
               "KEVENT Type must sit where guest_structs.h derived it, at +0x00");
_Static_assert(offsetof(guest_rtl_critical_section, event_signal_state)
                   == GUEST_EVENT_OFFSET_SIGNAL_STATE,
               "KEVENT SignalState must sit where guest_structs.h derived it, at +0x04");

/* ===========================================================================
 * KDPC: EXACTLY ONE FIELD IS DERIVABLE, AND WRITING IT IS MANDATORY.
 * ===========================================================================
 *
 * A KDPC is 0x1C bytes (measured from 10 zero-gap neighbours) and 24 of those bytes
 * are never touched anywhere in the image -- so DeferredRoutine, DeferredContext,
 * SystemArgument1/2 and the list entry are all kept HOST-SIDE below, because nothing
 * can be derived about where they live and guessing is what lifter patch 06 records
 * the cost of.
 *
 * The exception is the 16-bit Type at +0x00, and it is not optional. The guest GATES
 * its teardown on it. recomp_0059.c, sub_00410B22:
 *      esi = edi + 0x1C;                  // the KDPC
 *      cmp MEM16(esi), 0x13 (16-bit)
 *      if (CMP_NE(...)) goto skip;        // <- not initialised? do not touch it
 *      ... call MEM32(0x475960)           // KeRemoveQueueDpc(esi)
 *      MEM16(esi) = ~MEM16(esi);          // poison, so a double teardown is a no-op
 *
 * So if KeInitializeDpc does not stamp 0x13 into guest memory, that comparison fails,
 * KeRemoveQueueDpc is never called, and stale DPCs are left queued against freed
 * DSOUND objects. The failure is SILENT and nowhere near its cause. The same guard
 * appears at sub_0040AE01 on two more DPCs.
 *
 * The value is read out of the image, not recalled. That it equals NT's DpcObject is
 * corroboration, explicitly not the source. */
#define GUEST_DPC_OFFSET_TYPE 0x00u
#define GUEST_DPC_TYPE_INITIALISED 0x0013u
/* What the guest's own teardown leaves behind: ~0x0013 in 16 bits. A DPC in this
 * state has been torn down once already and must not be mistaken for an
 * uninitialised one. */
#define GUEST_DPC_TYPE_POISONED 0xFFECu

#define ORD_KeCancelTimer 97u
#define ORD_KeInitializeDpc 107u
#define ORD_KeInitializeTimerEx 113u
#define ORD_KeInsertQueueDpc 119u
#define ORD_KeQuerySystemTime 128u
#define ORD_KeRemoveQueueDpc 137u
#define ORD_KeSetEvent 145u
#define ORD_KeSetTimer 149u
#define ORD_KeSetTimerEx 150u
#define ORD_KeWaitForSingleObject 159u
#define ORD_NtSetSystemTime 228u

/* How many distinct guest objects can be tracked.
 *
 * The image's own call sites name only a handful of distinct objects -- the timer and
 * DPC operands are mostly .data globals (0x771690, 0x7713C0, 0x7714B0, 0x771560,
 * 0x7EB460, 0x7EB480) -- so 512 is slack of two orders of magnitude. An exhausted
 * table REPORTS and degrades to not tracking, rather than evicting a live object:
 * evicting one would make a later KeCancelTimer answer "was not set" for a timer that
 * was, which is a wrong answer where declining to track is a stated gap. */
#define KERNEL_EVENT_MAX 512u

/* 100-nanosecond intervals between 1601-01-01 and 1970-01-01.
 *
 * NT system time counts 100ns units from 1601; the host clock counts seconds from
 * 1970. This is the gap, and it is ARITHMETIC rather than a recalled magic number:
 * 369 years containing 89 leap days over 369*365 + 89 = 134774 days, times 86400
 * seconds, times 10,000,000 units per second. 134774 * 86400 = 11644473600 seconds.
 * The multiplication is written out so the value can be checked rather than trusted,
 * and a test recomputes it independently. */
#define KERNEL_EVENT_EPOCH_DELTA_SECONDS 11644473600u
#define KERNEL_EVENT_UNITS_PER_SECOND 10000000u

/* ---------------------------------------------------------------------------
 * THE LOCK.
 *
 * Two guest threads run and both reach this module. Every race here produces a
 * WRONG ANSWER rather than a crash:
 *
 *   - Claiming a table slot is a read-modify-write. Two threads can take the same
 *     slot for two different objects; the loser's object then has no record, so a
 *     later KeCancelTimer on it reports "this timer was never set" for a timer that
 *     was -- our diagnostic blaming the guest for our own bug, the same failure
 *     kernel_object.c documents.
 *   - KeSetEvent must return the PREVIOUS signalled state. Read-then-write without a
 *     lock lets two concurrent sets both return "was clear", so the guest believes
 *     two different threads each performed the transition.
 *   - KeInsertQueueDpc returns whether the DPC was ALREADY queued, which is the same
 *     test-and-set and the same double-success bug.
 *   - The monotonic clock's `last_time` is a read-modify-write, and a lost update is
 *     exactly what makes time go backwards for one of the two threads.
 *
 * RECURSIVE, following kernel_object.c and guest_mem.c: these critical sections call
 * kernel_hle_log(), whose sink is caller-supplied and may ask this module a question.
 * Tests install custom sinks, so this is a real path.
 *
 * LOCK ORDER. This module takes only this lock, and calls nothing that calls back
 * into it, so no cycle is possible.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t event_lock;
static pthread_once_t event_lock_once = PTHREAD_ONCE_INIT;

static void event_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&event_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void event_enter(void)
{
    (void)pthread_once(&event_lock_once, event_lock_init);
    (void)pthread_mutex_lock(&event_lock);
}

static void event_leave(void)
{
    (void)pthread_mutex_unlock(&event_lock);
}

static kernel_event_object objects[KERNEL_EVENT_MAX];
static unsigned tracked;
static kernel_event_dispatch dispatcher;
static void *dispatch_context;
static uint64_t queue_sequence;
static pthread_mutex_t service_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool servicing;

static uint64_t elapsed_units(void)
{
    const uint64_t ticks = kernel_clock_peek();
    return (ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 10000000u +
        (ticks % KERNEL_CLOCK_FREQUENCY_HZ) * 10000000u / KERNEL_CLOCK_FREQUENCY_HZ;
}
static uint32_t unhonoured_waits;
static uint32_t timers_armed;
static uint32_t dpcs_queued;
static uint64_t last_time;
/* The guest's wall clock minus the host's, in 100ns units: what NtSetSystemTime (228) sets. */
static int64_t system_time_offset;
static unsigned system_time_set_count;
static bool table_full_reported;

static const char *kind_name(kernel_event_object_kind kind)
{
    switch (kind) {
    case KERNEL_EVENT_OBJECT_EVENT:
        return "event";
    case KERNEL_EVENT_OBJECT_TIMER:
        return "timer";
    case KERNEL_EVENT_OBJECT_DPC:
        return "DPC";
    case KERNEL_EVENT_OBJECT_NONE:
    default:
        return "untyped";
    }
}

static kernel_event_object *find_nolock(kernel_guest_ptr address)
{
    if (address == 0u) {
        return NULL;
    }
    for (unsigned i = 0u; i < tracked; i++) {
        if (objects[i].address == address) {
            return &objects[i];
        }
    }
    return NULL;
}

/* Find or create the record for `address`, as `kind`. NULL only when the address is
 * 0 or the table is exhausted.
 *
 * A KIND CHANGE IS REPORTED, NOT SILENTLY ACCEPTED. The same guest address arriving
 * first as a timer and then as a DPC means either the guest is reusing storage or we
 * have an argument in the wrong slot -- and the second is a mistake we would
 * otherwise never hear about. The record keeps its new kind either way, because
 * refusing would strand it. */
static kernel_event_object *intern_nolock(kernel_guest_ptr address,
                                         kernel_event_object_kind kind, const char *who)
{
    if (address == 0u) {
        return NULL;
    }
    kernel_event_object *existing = find_nolock(address);
    if (existing) {
        if (existing->kind != kind && existing->kind != KERNEL_EVENT_OBJECT_NONE) {
            kernel_hle_log()("kernel: %s(%#x) -- this address was previously used as a "
                             "%s and is now a %s; either the guest reuses the storage "
                             "or an argument is in the wrong slot\n",
                             who, address, kind_name(existing->kind), kind_name(kind));
        }
        existing->kind = kind;
        return existing;
    }
    if (tracked >= KERNEL_EVENT_MAX) {
        /* Reported ONCE, because this would otherwise be inside whatever loop
         * exhausted it. Degrades to not tracking rather than evicting a live object:
         * an eviction would make a later query return a confidently wrong answer,
         * where an untracked object is a stated gap that still reports. */
        if (!table_full_reported) {
            table_full_reported = true;
            kernel_hle_log()("kernel: event/timer/DPC table full at %u objects -- %s"
                             "(%#x) and any further new object will not be tracked, so "
                             "their state queries will answer as though never "
                             "initialised\n",
                             KERNEL_EVENT_MAX, who, address);
        }
        return NULL;
    }
    kernel_event_object *entry = &objects[tracked++];
    *entry = (kernel_event_object){0};
    entry->address = address;
    entry->kind = kind;
    return entry;
}

/* ---------------------------------------------------------------------------
 * The clock.
 * ------------------------------------------------------------------------- */

/* System time in 100ns units since 1601, forced non-decreasing.
 *
 * MONOTONICITY IS ENFORCED, and not as a nicety. 18 call sites read this, and the
 * obvious thing to do with two readings is subtract them. CLOCK_REALTIME can step
 * backwards (NTP, a manual clock set), and an unsigned guest subtraction across a
 * backward step yields an enormous positive interval rather than a negative one --
 * so a frame-time or timeout computed from it becomes a hang or a divide-by-zero
 * somewhere else entirely. Clamping to last+1 makes the worst case a stalled clock
 * instead.
 *
 * CLOCK_REALTIME rather than CLOCK_MONOTONIC on purpose: this function's contract is
 * a WALL CLOCK with a 1601 epoch, and the guest may well render a date from it.
 * CLOCK_MONOTONIC has an arbitrary zero and would put that date in 1601. */
static bool host_units_nolock(uint64_t *out)
{
    struct timespec now = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return false;
    }
    uint64_t seconds = (uint64_t)now.tv_sec + KERNEL_EVENT_EPOCH_DELTA_SECONDS;
    *out = seconds * KERNEL_EVENT_UNITS_PER_SECOND + (uint64_t)now.tv_nsec / 100u;
    return true;
}

static uint64_t system_time_nolock(void)
{
    uint64_t units;
    uint64_t host_units;
    if (!host_units_nolock(&host_units)) {
        /* No clock. Advance by one tick rather than returning a fixed value, so a
         * guest that polls for a change still makes progress. */
        units = last_time + 1u;
    } else {
        /* The offset is signed and the sum is taken modulo 2^64, which is exact for any
         * offset NtSetSystemTime can produce (both clocks stay below 2^63). */
        units = host_units + (uint64_t)system_time_offset;
    }
    if (units <= last_time) {
        units = last_time + 1u;
    }
    last_time = units;
    return units;
}

uint64_t kernel_event_system_time(void)
{
    event_enter();
    uint64_t now = system_time_nolock();
    event_leave();
    return now;
}

bool kernel_event_set_system_time(uint64_t new_time)
{
    event_enter();
    uint64_t host_units;
    const bool have_clock = host_units_nolock(&host_units);
    if (have_clock) {
        system_time_offset = (int64_t)(new_time - host_units);
        /* A deliberate set is not clock jitter: restart the monotonic guard, or a set backward
         * by years would crawl at one unit per read for years. */
        last_time = 0u;
        system_time_set_count++;
    }
    event_leave();
    return have_clock;
}

unsigned kernel_event_system_time_set_count(void)
{
    event_enter();
    const unsigned count = system_time_set_count;
    event_leave();
    return count;
}

int64_t kernel_event_system_time_offset(void)
{
    event_enter();
    const int64_t offset = system_time_offset;
    event_leave();
    return offset;
}

/* ---------------------------------------------------------------------------
 * The call boundary.
 * ------------------------------------------------------------------------- */

static const kernel_call_frame *frame_of(void *context, const char *who)
{
    if (!context) {
        kernel_hle_log()("kernel: %s called with no argument frame -- the call "
                         "boundary did not supply one\n",
                         who);
        return NULL;
    }
    return (const kernel_call_frame *)context;
}

static bool frame_args(const kernel_call_frame *frame, uint32_t *out, unsigned count,
                       const char *who)
{
    for (unsigned i = 0u; i < count; i++) {
        if (!kernel_frame_arg(frame, i, &out[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest "
                             "stack\n",
                             who, i);
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Events. The state is the guest's, not ours -- see the note at the top.
 * ------------------------------------------------------------------------- */

/* Read a KEVENT's SignalState out of guest memory. False when the address is
 * unusable, which a caller must NOT paper over with a zero: zero is "not signalled",
 * a perfectly plausible value, so substituting it would turn an unreadable event into
 * a confidently-reported unsignalled one. */
static bool event_signal_state_read(kernel_guest_ptr event, uint32_t *out,
                                    const char *who)
{
    if (event == 0u) {
        kernel_hle_log()("kernel: %s(NULL) -- no event to act on\n", who);
        return false;
    }
    if (!kernel_guest_read_u32((kernel_guest_ptr)(event + GUEST_EVENT_OFFSET_SIGNAL_STATE),
                               out)) {
        kernel_hle_log()("kernel: %s(%#x) -- could not read SignalState at +%#x; the "
                         "event is not usable guest memory\n",
                         who, event, GUEST_EVENT_OFFSET_SIGNAL_STATE);
        return false;
    }
    return true;
}

static bool event_signal_state_write(kernel_guest_ptr event, uint32_t value,
                                     const char *who)
{
    if (!kernel_guest_write_u32(
            (kernel_guest_ptr)(event + GUEST_EVENT_OFFSET_SIGNAL_STATE), value)) {
        kernel_hle_log()("kernel: %s(%#x) -- could not write SignalState at +%#x; the "
                         "guest's own view of this event is now stale\n",
                         who, event, GUEST_EVENT_OFFSET_SIGNAL_STATE);
        return false;
    }
    return true;
}

/* The event's Type byte (0 NotificationEvent, 1 SynchronizationEvent). */
static bool event_type_read(kernel_guest_ptr event, uint32_t *out)
{
    const unsigned char *byte = (const unsigned char *)kernel_guest_at(
        (kernel_guest_ptr)(event + GUEST_EVENT_OFFSET_TYPE), 1u);
    if (!byte) {
        return false;
    }
    *out = *byte;
    return true;
}

bool kernel_event_signal_state(kernel_guest_ptr event, uint32_t *out)
{
    if (event == 0u || !out) {
        return false;
    }
    return kernel_guest_read_u32(
        (kernel_guest_ptr)(event + GUEST_EVENT_OFFSET_SIGNAL_STATE), out);
}

static uint32_t hle_ke_set_event(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeSetEvent");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "KeSetEvent")) {
        return 0u;
    }
    const kernel_guest_ptr event = (kernel_guest_ptr)args[0];
    const uint32_t increment = args[1];
    const uint32_t wait = args[2];

    event_enter();
    kernel_event_object *entry = intern_nolock(event, KERNEL_EVENT_OBJECT_EVENT,
                                              "KeSetEvent");

    /* THE SIGNAL STATE IS READ FROM AND WRITTEN TO GUEST MEMORY, not a host flag. See
     * the long note at the top of this file: the guest clears this very dword itself
     * before waiting, and four events are stack locals whose addresses are reused. */
    uint32_t previous = 0u;
    if (!event_signal_state_read(event, &previous, "KeSetEvent")) {
        event_leave();
        return 0u;
    }
    if (!event_signal_state_write(event, 1u, "KeSetEvent")) {
        event_leave();
        return 0u;
    }
    if (entry) {
        entry->last_increment = increment;
        entry->last_seen_signal_state = 1u;
    }
    /* THE RETURN VALUE IS THE PREVIOUS STATE, not the new one. The guest uses it to
     * decide whether IT performed the transition, so inverting this would make every
     * "did I signal it first?" test in the game answer backwards. Normalised to 0/1
     * because the field is a count and the guest treats it as a boolean. */
    previous = (previous != 0u) ? 1u : 0u;

    if (wait != 0u) {
        /* A nonzero Wait asks the kernel to hold dispatcher state and immediately
         * follow with a wait. There is no wait queue here, so this is dropped --
         * reported because it is a real semantic gap, not a no-op. */
        kernel_hle_log()("kernel: KeSetEvent(%#x) with Wait=%u -- the deferred-wait "
                         "behaviour is not modelled; the set itself is applied\n",
                         event, wait);
    }
    event_leave();
    return previous;
}

static bool notification_wait_logged;

static uint32_t hle_ke_wait_for_single_object(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeWaitForSingleObject");
    uint32_t args[5];
    if (!frame || !frame_args(frame, args, 5u, "KeWaitForSingleObject")) {
        /* A wait we could not even read the object for. STATUS_SUCCESS would claim
         * the object was signalled, so report the parameter error instead. */
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_guest_ptr object = (kernel_guest_ptr)args[0];
    const kernel_guest_ptr timeout = (kernel_guest_ptr)args[4];

    event_enter();
    kernel_event_object *entry = find_nolock(object);

    /* READ THE GUEST'S OWN SignalState. This is the whole reason this module does not
     * cache it: sub_003D3550 writes 0 to this exact dword and then waits on the event
     * six instructions later, so a cached flag would report the wait satisfied at the
     * precise moment the guest had deliberately cleared it. */
    uint32_t state = 0u;
    if (!event_signal_state_read(object, &state, "KeWaitForSingleObject")) {
        /* We could not evaluate the object at all, so this is a PARAMETER ERROR and not
         * an unhonoured wait. Kept out of that counter deliberately: its whole value is
         * that it means "a real wait the host declined to honour", and folding
         * unreadable objects into it would dilute the one number that says how far a
         * run can be trusted. STATUS_SUCCESS is emphatically not returned -- that would
         * claim an object we never read was signalled.
         *
         * kernel_guest_at rejects null, out-of-range, unmapped and unreadable
         * (PROT_NONE) ranges. Its probe does not pin the mapping against a
         * concurrent unmap; that residual limit is documented in kernel_call.h. */
        event_leave();
        return STATUS_INVALID_PARAMETER;
    }
    const bool signalled = state != 0u;
    if (entry) {
        entry->last_seen_signal_state = state;
    }

    if (!signalled) {
        /* NOT BLOCKED, AND THIS IS THE MODULE'S HEADLINE GAP. Nothing in this host
         * can ever signal an object: no timer fires and no DPC runs. So a real block
         * would be a guaranteed hang rather than a wait, and returning
         * STATUS_SUCCESS at least lets the guest proceed -- while being a lie about
         * the object's state. Counted and reported so a run can be judged on how
         * much it leaned on waiting. */
        unhonoured_waits++;
        kernel_hle_log()("kernel: KeWaitForSingleObject(%#x) on an object that is not "
                         "signalled%s -- returning STATUS_SUCCESS WITHOUT waiting. "
                         "Nothing here can ever signal it, so blocking would hang. "
                         "This is unhonoured wait %u\n",
                         object, entry ? "" : " and was never seen before",
                         unhonoured_waits);
    } else {
        /* AUTO-RESET FOLLOWS THE DISPATCHER TYPE. EVENT_TYPE is NotificationEvent = 0
         * (manual reset, stays signalled) and SynchronizationEvent = 1 (a satisfied wait
         * sets SignalState back to 0). KeInitializeEvent stores the enum ordinal
         * directly into Header.Type, so 0/1 is the ABI (docs/clean-sources-audit.md
         * section 2.2: nxdk, OpenXDK, Dxbx and Cxbx agree; our own image corroborates
         * it, the critical-section embedded events are Type 1 and the plain control
         * event is Type 0). Only 0 and 1 are acted on: other Type values are other
         * object classes, whose behaviour that audit says not to extrapolate. */
        uint32_t type = 0xFFFFFFFFu;
        const bool have_type = event_type_read(object, &type);
        const bool auto_reset = have_type && type == 1u;
        if (auto_reset) {
            (void)event_signal_state_write(object, 0u, "KeWaitForSingleObject");
        }
        if (auto_reset || !have_type || type > 1u) {
            kernel_hle_log()("kernel: KeWaitForSingleObject(%#x) satisfied by SignalState "
                             "%u (guest Type=%s)%s\n",
                             object, state,
                             have_type ? (type == 1u ? "1" : (type == 0u ? "0" : "other"))
                                       : "unreadable",
                             auto_reset ? "; SynchronizationEvent, SignalState reset to 0"
                                        : "; Type not 0 or 1, SignalState left alone");
        } else if (!notification_wait_logged) {
            /* A notification event waited on every frame would flood the log, so the
             * first one is reported and the rest are silent. */
            notification_wait_logged = true;
            kernel_hle_log()("kernel: KeWaitForSingleObject(%#x) satisfied by SignalState "
                             "%u (NotificationEvent, Type 0, stays signalled; further "
                             "notification waits are not logged)\n", object, state);
        }
    }

    if (timeout != 0u) {
        kernel_hle_log()("kernel: KeWaitForSingleObject(%#x) passed a timeout at %#x "
                         "-- ignored, since the wait does not block\n",
                         object, timeout);
    }
    event_leave();
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Timers.
 * ------------------------------------------------------------------------- */

static uint32_t hle_ke_initialize_timer_ex(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeInitializeTimerEx");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "KeInitializeTimerEx")) {
        return 0u;
    }
    event_enter();
    kernel_event_object *entry = intern_nolock((kernel_guest_ptr)args[0],
                                              KERNEL_EVENT_OBJECT_TIMER,
                                              "KeInitializeTimerEx");
    if (entry) {
        /* A fresh timer is not set and carries no DPC. Re-initialising a set timer
         * is how the guest cancels one without calling KeCancelTimer, so the clear
         * matters. */
        entry->timer_type = args[1];
        entry->timer_set = false;
        entry->due_time = 0;
        entry->timer_dpc = 0u;
        entry->timer_period_ms = 0;
    }
    event_leave();
    return 0u;
}

static uint32_t set_timer(void *context, bool periodic)
{
    const kernel_call_frame *frame = frame_of(context, periodic ? "KeSetTimerEx" : "KeSetTimer");
    uint32_t args[5];
    if (!frame || !frame_args(frame, args, periodic ? 5u : 4u,
                              periodic ? "KeSetTimerEx" : "KeSetTimer")) {
        return 0u;
    }
    const kernel_guest_ptr timer = (kernel_guest_ptr)args[0];
    /* Slots 1 and 2 are ONE LARGE_INTEGER passed by value: low dword first, then
     * high. MEASURED -- see the ARITY-OK(149) note for the sites that pin both the
     * split and the order. Reassembled signed, because every site in this image
     * passes a NEGATIVE value, which means a relative time in 100ns units. */
    const uint64_t raw_due = (uint64_t)args[1] | ((uint64_t)args[2] << 32);
    const kernel_guest_ptr dpc = (kernel_guest_ptr)args[periodic ? 4u : 3u];

    event_enter();
    kernel_event_object *entry = intern_nolock(timer, KERNEL_EVENT_OBJECT_TIMER,
                                              "KeSetTimer");
    /* Returns whether the timer was ALREADY in the queue -- previous state, like
     * KeSetEvent. */
    uint32_t was_set = 0u;
    if (entry) {
        was_set = entry->timer_set ? 1u : 0u;
        entry->timer_set = true;
        entry->due_time = (int64_t)raw_due;
        entry->timer_dpc = dpc;
        entry->timer_period_ms = periodic ? (int32_t)args[3] : 0;
        const uint64_t now = elapsed_units();
        const uint64_t calendar = (int64_t)raw_due < 0 ? 0u : kernel_event_system_time();
        const uint64_t delay = (int64_t)raw_due < 0 ? 0u - raw_due :
                              (raw_due > calendar ? raw_due - calendar : 0u);
        entry->timer_deadline = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
    }
    timers_armed++;
    kernel_hle_log()("kernel: KeSetTimer(%#x) due %lld (%s), dpc %#x -- recorded only; "
                     "NO TIMER EVER FIRES in this host, so this will not elapse and "
                     "its DPC will not run\n",
                     timer, (long long)(int64_t)raw_due,
                     ((int64_t)raw_due < 0) ? "relative, 100ns units" : "absolute",
                     dpc);
    event_leave();
    return was_set;
}

static uint32_t hle_ke_set_timer(void *context)
{
    return set_timer(context, false);
}

static uint32_t hle_ke_set_timer_ex(void *context)
{
    /* Retail 00439FB8: Timer, DueTime low/high, Period, Dpc. RET 14h.
     * Independent original-instruction qualification: T1136. */
    return set_timer(context, true);
}

static uint32_t hle_ke_cancel_timer(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeCancelTimer");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "KeCancelTimer")) {
        return 0u;
    }
    event_enter();
    kernel_event_object *entry = find_nolock((kernel_guest_ptr)args[0]);
    /* TRUE only if the timer really was in the queue. This one IS faithfully
     * modelled: the answer depends on nothing but our own set/cancel bookkeeping, so
     * a guest that sets, cancels and re-cancels sees exactly what hardware would. */
    uint32_t was_set = (entry && entry->timer_set) ? 1u : 0u;
    if (entry) {
        entry->timer_set = false;
        entry->due_time = 0;
        entry->timer_dpc = 0u;
        entry->timer_period_ms = 0;
    }
    event_leave();
    return was_set;
}

static uint32_t hle_ke_query_system_time(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeQuerySystemTime");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "KeQuerySystemTime")) {
        return 0u;
    }
    const kernel_guest_ptr out = (kernel_guest_ptr)args[0];
    if (out == 0u) {
        kernel_hle_log()("kernel: KeQuerySystemTime(NULL) -- nowhere to write the "
                         "time; the guest will read whatever was in its own local\n");
        return 0u;
    }

    event_enter();
    const uint64_t now = system_time_nolock();
    event_leave();

    /* A LARGE_INTEGER is low dword then high dword, little-endian. MEASURED in this
     * image rather than assumed: KeSetTimer sites pass one in from memory as an
     * adjacent pair, pushing MEM32(eax+0x1C) then MEM32(eax+0x18) -- high from the
     * upper address, low from the lower. Written as two 32-bit stores so the guest's
     * 8-byte field is filled exactly, with no chance of an 8-byte host store
     * overrunning a 4-byte guest one. */
    const bool low_ok = kernel_guest_write_u32(out, (uint32_t)(now & 0xFFFFFFFFu));
    const bool high_ok =
        kernel_guest_write_u32((kernel_guest_ptr)(out + 4u), (uint32_t)(now >> 32));
    if (!low_ok || !high_ok) {
        /* A HALF-WRITTEN TIME IS WORSE THAN NONE, so say which happened. A guest
         * that got a low dword and kept a stale high dword holds a time off by
         * multiples of about seven minutes, which looks entirely plausible. */
        kernel_hle_log()("kernel: KeQuerySystemTime(%#x) could not write the %s -- the "
                         "guest's 64-bit time is now PARTIAL and will be plausible but "
                         "wrong\n",
                         out, low_ok ? "high dword" : "low dword");
    }
    return 0u;
}

/*
 * NtSetSystemTime (228): rebase the guest's wall clock. MEASURED: the one site is the XONLINE
 * wrapper 0x00425813, `push 0; lea eax,[esp+8]; push eax; call [0x4759C0]; ret 8`, which takes the
 * 8-byte time by value and passes its address with OldTime NULL. Its two callers (0x00427241,
 * 0x0042758F) build the time with RtlTimeFieldsToTime from a server reply (an all-zero block or
 * exactly 1970-01-01 becomes 0, which is passed on all the same) and ignore the result.
 *
 * INFERRED, NT contract: the new time is the guest's wall clock from now on (an offset from the
 * host clock, read by KeQuerySystemTime), zero and negative times are INVALID_PARAMETER, an
 * unreadable NewTime is ACCESS_VIOLATION. REFUSED loudly with nothing changed: a non-NULL OldTime,
 * which no measured site passes (judged before the time is read).
 *
 * ARITY-OK(228): TWO stack arguments, (NewTime, OldTime). The measured row {228,2,1,1} has one
 * voter, below the quorum, so the hand ABI row in src/host/kernel_thunk.c pins it; the nxdk .def
 * (NtSetSystemTime@8) and the oracle agree.
 */
static uint32_t hle_nt_set_system_time(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "NtSetSystemTime");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "NtSetSystemTime")) {
        return STATUS_INVALID_PARAMETER;
    }
    if (args[1] != 0u) {
        kernel_hle_log()("kernel: NtSetSystemTime with a non-NULL OldTime (%#x) is not a "
                         "measured mode (the one site passes NULL) -- REFUSED, the clock is "
                         "unchanged\n",
                         (unsigned)args[1]);
        return STATUS_NOT_IMPLEMENTED;
    }
    uint32_t low = 0u;
    uint32_t high = 0u;
    if (args[0] == 0u || !kernel_guest_read_u32((kernel_guest_ptr)args[0], &low) ||
        !kernel_guest_read_u32((kernel_guest_ptr)(args[0] + 4u), &high)) {
        kernel_hle_log()("kernel: NtSetSystemTime could not read NewTime at %#x -- ACCESS "
                         "VIOLATION, the clock is unchanged\n",
                         (unsigned)args[0]);
        return STATUS_ACCESS_VIOLATION;
    }
    const uint64_t new_time = (uint64_t)low | ((uint64_t)high << 32);
    if (new_time == 0u || (new_time >> 63) != 0u) {
        kernel_hle_log()("kernel: NtSetSystemTime(%#llx) is zero or negative -- INVALID "
                         "PARAMETER, the clock is unchanged (the XONLINE caller passes 0 when a "
                         "reply carried no time)\n",
                         (unsigned long long)new_time);
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_event_set_system_time(new_time)) {
        kernel_hle_log()("kernel: NtSetSystemTime(%#llx) could not read the host clock to "
                         "compute an offset -- the clock is unchanged\n",
                         (unsigned long long)new_time);
        return STATUS_UNSUCCESSFUL;
    }
    kernel_hle_log()("kernel: NtSetSystemTime(%#llx) -> the guest wall clock is REBASED "
                     "(INFERRED, NT contract): KeQuerySystemTime now reads host time plus "
                     "%lld units. The host clock itself is not touched\n",
                     (unsigned long long)new_time, (long long)kernel_event_system_time_offset());
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * DPCs.
 * ------------------------------------------------------------------------- */

static uint32_t hle_ke_initialize_dpc(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeInitializeDpc");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "KeInitializeDpc")) {
        return 0u;
    }
    event_enter();
    kernel_event_object *entry = intern_nolock((kernel_guest_ptr)args[0],
                                              KERNEL_EVENT_OBJECT_DPC,
                                              "KeInitializeDpc");
    if (entry) {
        entry->dpc_routine = (kernel_guest_ptr)args[1];
        entry->dpc_context = (kernel_guest_ptr)args[2];
        entry->dpc_queued = false;
        entry->dpc_sequence = ++queue_sequence;
        entry->dpc_argument1 = 0u;
        entry->dpc_argument2 = 0u;
    }

    /* STAMP THE TYPE INTO GUEST MEMORY. This is the one KDPC field that is derivable,
     * and writing it is MANDATORY rather than cosmetic: the guest's own teardown does
     * `cmp word ptr [dpc], 0x13` and SKIPS KeRemoveQueueDpc entirely when it does not
     * match (sub_00410B22, sub_0040AE01). Omit this and stale DPCs stay queued against
     * freed DSOUND objects, silently and far from here. Everything else about a KDPC
     * stays host-side, because nothing in the image says where those fields live. */
    const kernel_guest_ptr dpc = (kernel_guest_ptr)args[0];
    uint16_t *type_field =
        (uint16_t *)kernel_guest_at((kernel_guest_ptr)(dpc + GUEST_DPC_OFFSET_TYPE),
                                    sizeof(uint16_t));
    if (type_field) {
        *type_field = (uint16_t)GUEST_DPC_TYPE_INITIALISED;
    } else if (dpc != 0u) {
        kernel_hle_log()("kernel: KeInitializeDpc(%#x) could not write the Type tag at "
                         "+%#x -- the guest's teardown will not recognise this DPC and "
                         "will skip KeRemoveQueueDpc on it\n",
                         dpc, GUEST_DPC_OFFSET_TYPE);
    }
    event_leave();
    return 0u;
}

static uint32_t hle_ke_insert_queue_dpc(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeInsertQueueDpc");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "KeInsertQueueDpc")) {
        return 0u;
    }
    const kernel_guest_ptr dpc = (kernel_guest_ptr)args[0];

    event_enter();
    kernel_event_object *entry = intern_nolock(dpc, KERNEL_EVENT_OBJECT_DPC,
                                              "KeInsertQueueDpc");
    /* FALSE when it was already queued -- the real function refuses to queue a DPC
     * twice, and the guest relies on that to avoid double-processing. Inverting it
     * would be silent. */
    uint32_t inserted = 0u;
    if (entry) {
        if (!entry->dpc_queued) {
            entry->dpc_queued = true;
            entry->dpc_sequence = ++queue_sequence;
            entry->dpc_argument1 = (kernel_guest_ptr)args[1];
            entry->dpc_argument2 = (kernel_guest_ptr)args[2];
            inserted = 1u;
            dpcs_queued++;
        }
        kernel_hle_log()("kernel: KeInsertQueueDpc(%#x) routine %#x -- %s; NO DPC "
                         "ROUTINE IS EVER CALLED in this host, so the deferred work "
                         "will not happen\n",
                         dpc, entry->dpc_routine,
                         inserted ? "queued" : "already queued, refused");
    }
    event_leave();
    return inserted;
}

static uint32_t hle_ke_remove_queue_dpc(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "KeRemoveQueueDpc");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "KeRemoveQueueDpc")) {
        return 0u;
    }
    const kernel_guest_ptr dpc = (kernel_guest_ptr)args[0];

    event_enter();
    kernel_event_object *entry = find_nolock(dpc);
    /* TRUE only if it really was queued. Faithfully modelled for the same reason as
     * KeCancelTimer: the answer depends only on our own bookkeeping. */
    uint32_t was_queued = (entry && entry->dpc_queued) ? 1u : 0u;
    if (entry) {
        entry->dpc_queued = false;
        entry->dpc_sequence = ++queue_sequence;
    }

    /* THE GUEST POISONS THE TYPE TAG AFTER CALLING US, and we must not read that as a
     * corrupt object. sub_00410B22 does `MEM16(dpc) = ~MEM16(dpc)` immediately after
     * this call returns, turning 0x13 into 0xFFEC so that a second teardown skips the
     * whole block. Arriving here with the tag ALREADY poisoned means the guest is
     * tearing the same DPC down twice; that is its business and not an error, but it is
     * worth saying rather than looking like an uninitialised object.
     *
     * Note also that two real objects in this image (the KDPC at 0x7713A4 and the
     * KTIMER at 0x7713C0) are used without ever being initialised through the kernel at
     * all -- they sit in BSS and are therefore all-zero -- so an absent tag is a case
     * that genuinely occurs and must not be treated as a fault. */
    const uint16_t *type_field =
        (const uint16_t *)kernel_guest_at((kernel_guest_ptr)(dpc + GUEST_DPC_OFFSET_TYPE),
                                         sizeof(uint16_t));
    if (type_field && *type_field == (uint16_t)GUEST_DPC_TYPE_POISONED) {
        kernel_hle_log()("kernel: KeRemoveQueueDpc(%#x) on a DPC whose Type tag is "
                         "already poisoned (%#x) -- the guest is tearing it down a "
                         "second time; not an error\n",
                         dpc, GUEST_DPC_TYPE_POISONED);
    }
    event_leave();
    return was_queued;
}

/* ---------------------------------------------------------------------------
 * Observation, for tests and for the end-of-run report.
 * ------------------------------------------------------------------------- */

void kernel_event_set_dispatch(kernel_event_dispatch dispatch, void *opaque)
{
    event_enter();
    dispatcher = dispatch;
    dispatch_context = opaque;
    event_leave();
}

bool kernel_event_service(unsigned *delivered)
{
    if (delivered) *delivered = 0u;
    /* A callback can legitimately advance time; defer recursive service. */
    if (servicing) return true;
    if (kernel_sync_current_irql() > 2u) return false;
    if (pthread_mutex_trylock(&service_lock) != 0) return true;
    servicing = true;
    event_enter();
    if (!dispatcher) {
        event_leave();
        servicing = false;
        (void)pthread_mutex_unlock(&service_lock);
        return false;
    }
    const uint64_t now = elapsed_units();
    for (unsigned i = 0u; i < tracked; ++i) {
        kernel_event_object *timer = &objects[i];
        if (timer->kind != KERNEL_EVENT_OBJECT_TIMER || !timer->timer_set ||
            timer->timer_deadline > now) continue;
        kernel_event_object *dpc = find_nolock(timer->timer_dpc);
        if (dpc && dpc->kind == KERNEL_EVENT_OBJECT_DPC && !dpc->dpc_queued) {
            dpc->dpc_queued = true;
            dpc->dpc_sequence = ++queue_sequence;
            /* INFERRED system arguments; XNET 0043A184 ignores both. */
            dpc->dpc_argument1 = (uint32_t)now;
            dpc->dpc_argument2 = (uint32_t)(now >> 32);
            ++dpcs_queued;
        }
        if (timer->timer_period_ms > 0) {
            const uint64_t period = (uint64_t)timer->timer_period_ms * 10000u;
            const uint64_t remainder = (now - timer->timer_deadline) % period;
            const uint64_t delay = period - remainder;
            timer->timer_deadline = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
        } else {
            timer->timer_set = false;
        }
    }
    const uint64_t boundary = queue_sequence;
    bool complete = true;
    for (;;) {
        kernel_event_object *next = NULL;
        for (unsigned i = 0u; i < tracked; ++i) {
            kernel_event_object *candidate = &objects[i];
            if (candidate->kind == KERNEL_EVENT_OBJECT_DPC && candidate->dpc_queued &&
                candidate->dpc_sequence <= boundary &&
                (!next || candidate->dpc_sequence < next->dpc_sequence)) next = candidate;
        }
        if (!next) break;
        const kernel_event_object pending = *next;
        const kernel_event_dispatch invoke = dispatcher;
        void *opaque = dispatch_context;
        next->dpc_queued = false;
        event_leave();
        const uint32_t old_irql = kernel_sync_raise_irql(2u);
        const bool executed = invoke(pending.dpc_routine, pending.address,
            pending.dpc_context, pending.dpc_argument1, pending.dpc_argument2, opaque);
        kernel_sync_restore_irql(old_irql);
        event_enter();
        if (!executed) {
            /* No successful execution is claimed. Preserve a newer enqueue if any. */
            if (!next->dpc_queued && next->dpc_sequence == pending.dpc_sequence) {
                next->dpc_queued = true;
            }
            complete = false;
            break;
        }
        if (delivered) ++*delivered;
    }
    event_leave();
    servicing = false;
    (void)pthread_mutex_unlock(&service_lock);
    return complete;
}

void kernel_event_reset(void)
{
    event_enter();
    for (unsigned i = 0u; i < KERNEL_EVENT_MAX; i++) {
        objects[i] = (kernel_event_object){0};
    }
    tracked = 0u;
    dispatcher = NULL;
    dispatch_context = NULL;
    queue_sequence = 0u;
    unhonoured_waits = 0u;
    notification_wait_logged = false;
    timers_armed = 0u;
    dpcs_queued = 0u;
    last_time = 0u;
    system_time_offset = 0;
    system_time_set_count = 0u;
    table_full_reported = false;
    event_leave();
}

const kernel_event_object *kernel_event_object_at(kernel_guest_ptr address)
{
    event_enter();
    const kernel_event_object *entry = find_nolock(address);
    event_leave();
    /* Returns a pointer INTO the table, which outlives the lock. Safe here because
     * nothing ever removes or moves an entry -- `tracked` only grows and a slot is
     * never reused -- so the pointer cannot be invalidated. kernel_object.c has the
     * same shape and records the same caveat; the difference is that its table does
     * zero entries on close and this one does not. */
    return entry;
}

unsigned kernel_event_tracked_count(void)
{
    event_enter();
    unsigned snapshot = tracked;
    event_leave();
    return snapshot;
}

uint32_t kernel_event_unhonoured_wait_count(void)
{
    event_enter();
    uint32_t snapshot = unhonoured_waits;
    event_leave();
    return snapshot;
}

uint32_t kernel_event_timers_armed(void)
{
    event_enter();
    uint32_t snapshot = timers_armed;
    event_leave();
    return snapshot;
}

uint32_t kernel_event_dpcs_queued(void)
{
    event_enter();
    uint32_t snapshot = dpcs_queued;
    event_leave();
    return snapshot;
}

/*
 * ARITY ACKNOWLEDGEMENTS. Six of these nine ordinals carry a LOW-CONFIDENCE measured
 * arity, and for four of them the published minimum is outright WRONG. The minimum
 * rule in tools/lift/callsites.py is defended on the grounds that its only error is
 * over-counting; that is FALSE on this image, where a call-site bracket that opens
 * after the arguments were pushed in a predecessor basic block under-counts. Each
 * note below gives the per-site vote and accounts for the minority in BOTH
 * directions, which is strictly stronger evidence than a bound that can only err one
 * way.
 *
 * ARITY-OK(145): three stack arguments, (Event, Increment, Wait). Measured table says
 * 0 and is WRONG. Vote over 27 sites: {0: 2, 1: 1, 3: 24}. The 24-site mode is
 * decisive, and the argument values pin the ORDER as well as the count -- the pushes
 * are overwhelmingly {0, 1, ptr}, i.e. Wait=0, Increment=1, Event=ptr, so the middle
 * slot is the priority increment and the pointer is leftmost in C order. The three
 * low sites are late brackets: recomp_0062.c:17438 pushes ebx and 1 in block
 * loc_0043757E, then the bracket opens at loc_0043758B and catches only `push edi`.
 *
 * ARITY-OK(97): one stack argument, the Timer. Vote over 27 sites: {1: 25, 2: 2}. The
 * published minimum of 1 is right; it is flagged only because two sites disagree, and
 * both are the documented over-count -- recomp_0064.c:4252 has `push esi` as a
 * register SAVE inside the bracket, immediately before the real argument
 * `PUSH 0x7713C0`, which is one of the .data timer globals.
 *
 * ARITY-OK(149): FOUR stack slots for THREE C arguments, because DueTime is a
 * LARGE_INTEGER passed by value. Vote over 20 sites: {4: 19, 5: 1}. The 8-byte split
 * is not inferred from the name, it is read off the pushes: at recomp_0045.c:1925 the
 * order is Dpc, then `ecx |= 0xFFFFFFFF` pushed as the HIGH dword, then
 * eax = 0xFFFFD8F0 as the LOW dword, then the Timer -- combined
 * 0xFFFFFFFFFFFFD8F0 = -10,000, i.e. -1 millisecond in 100ns units. Two more sites
 * decode as clean round negative intervals (-1,000,000 = -100ms at
 * recomp_0064.c:13048; -216,000,000,000 = -6 hours at recomp_0044.c:21306), and
 * several pass an adjacent in-memory pair MEM32(eax+0x1C) then MEM32(eax+0x18) --
 * high from the upper address, low from the lower. Three independent sites decoding
 * to round human intervals could not happen under any other slot split. The single
 * 5-vote site is a `push esi` save after the prologue.
 *
 * ARITY-OK(107): three stack arguments, (Dpc, DeferredRoutine, DeferredContext).
 * Measured table says 2 and is WRONG. Vote over 12 sites: {2: 1, 3: 10, 5: 1}. The
 * middle operand is a .text code address at every site (0x384730, 0x385CF0,
 * 0x3DCA90, 0x410AD7, 0x43A184, 0x46FEFF) -- a function pointer, which is what a
 * DeferredRoutine is and what neither of the other two operands could be. The
 * 2-vote site is a late bracket (recomp_0063.c:49829 pushes the context `esi` before
 * a conditional jump); the 5-vote site has two register saves inside the bracket.
 * Cross-confirmed by object identity: recomp_0044.c:21308 arms Timer 0x7EB480 with
 * Dpc 0x7EB460, the exact pair initialised by the ordinal 113 and ordinal 107 sites
 * seven lines earlier.
 *
 * ARITY-OK(113): two stack arguments, (Timer, Type). Vote over 9 sites: {2: 8, 6: 1}.
 * The lone 6 is the clearest over-count in the image: at recomp_0045.c:2798 the
 * bracket is the function's FIRST statement, so all four of `push ebx; push ebp;
 * push esi; push edi` land inside it before the two real arguments.
 *
 * ARITY-OK(119): three stack arguments, (Dpc, SystemArgument1, SystemArgument2).
 * Measured table says 0 and is WRONG. Vote over 7 sites: {0: 1, 3: 6}. Values are
 * {0, 0, ptr}, so the two system arguments are the unused zeros and the pointer is
 * the Dpc. The single 0-vote site is a late bracket: recomp_0054.c:11943 pushes 0, 0
 * and esi, and the bracket opens at 11950 with nothing left inside it.
 *
 * NOT ACKNOWLEDGED BECAUSE THEY DO NOT NEED IT -- the measurement is already
 * unanimous and confident for these two, and the arity check correctly leaves them
 * alone: 128 KeQuerySystemTime (17 of 17 sites at 1), 137 KeRemoveQueueDpc (5 of 5 at 1).
 *
 * ARITY-OK(159): KeWaitForSingleObject takes 5 stack arguments -- Object, WaitReason,
 * WaitMode, Alertable, Timeout. This note WAS unnecessary and became necessary: the
 * call-site scanner was fixed to see calls through a register and through import jump
 * stubs, which took this ordinal from 9 sites to 12, and the three newly-visible sites
 * disagree, so the measurement is no longer unanimous and the arity check now flags it.
 *
 * The count is nonetheless trusted. The 9 originally-visible sites all vote 5
 * unanimously, and the disagreement comes from the newly-found register-indirect sites,
 * which are structurally prone to the LATE-BRACKET UNDERCOUNT the thunk layer already
 * warns about: a load sitting in an earlier basic block correlates with the argument
 * pushes also sitting in an earlier block, so the bracket opens after them and the site
 * votes low. The minimum-over-sites estimator then takes that low vote. A disagreement
 * of that shape is evidence about the estimator, not about the arity.
 */
static const struct {
    unsigned ordinal;
    kernel_fn handler;
} bindings[] = {
    {ORD_KeCancelTimer, hle_ke_cancel_timer},
    {ORD_KeInitializeDpc, hle_ke_initialize_dpc},
    {ORD_KeInitializeTimerEx, hle_ke_initialize_timer_ex},
    {ORD_KeInsertQueueDpc, hle_ke_insert_queue_dpc},
    {ORD_KeQuerySystemTime, hle_ke_query_system_time},
    {ORD_KeRemoveQueueDpc, hle_ke_remove_queue_dpc},
    {ORD_KeSetEvent, hle_ke_set_event},
    {ORD_KeSetTimer, hle_ke_set_timer},
    {ORD_KeSetTimerEx, hle_ke_set_timer_ex},
    {ORD_KeWaitForSingleObject, hle_ke_wait_for_single_object},
    {ORD_NtSetSystemTime, hle_nt_set_system_time},
};

size_t kernel_event_register(void)
{
    size_t bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
