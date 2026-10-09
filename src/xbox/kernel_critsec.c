/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_critsec.h for why owner and recursion depth are host-side while
 * lock_count stays in guest memory.
 *
 * ASSUMED SIGNATURES. The stack-argument counts are MEASURED from the guest's own
 * pushes; the parameter meanings come from the published export list. An arity
 * error is silent, so if a disassembly contradicts one of these, this comment is
 * what to fix.
 *
 *   void __stdcall RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs);      // 1 stack arg
 *   void __stdcall RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs);      // 1 stack arg
 *   void __stdcall RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs); // 1 stack arg
 *
 * All three are __stdcall with the pointer on the STACK, not in ECX, and that was
 * checked rather than assumed. At 0x0037FBEC the guest does `mov ebx, 0x549148` /
 * `push ebx` / `call dword ptr [0x47581C]` and the NEXT instruction at 0x0037FBFB
 * is `mov esi, dword ptr [0x549164]` -- no `add esp, 4`, so the callee pops, which
 * is what makes this __stdcall and not __cdecl. The matching Leave at 0x0037FC18
 * is followed immediately by `pop edi`, same conclusion. Ordinal 291's site at
 * 0x003835D0 is the interesting one: the guest holds the pointer in ECX and
 * nonetheless does `push ecx` before the call, which is only worth doing if the
 * callee reads a stack slot.
 *
 * THE RETURN VALUE IS DISCARDED BY THE GUEST, and that is also checked: the
 * instruction after each of the three calls above overwrites or ignores EAX
 * without testing it. We return STATUS_SUCCESS so the value is at least not
 * misleading if some other site does look.
 *
 * WHAT WE DO NOT IMPLEMENT, AND WHY. Resolved against tools/kernel_ordinals.py,
 * then against the XBE's own thunk table:
 *
 *   278 RtlEnterCriticalSectionAndRegion  -- NOT IMPORTED: no thunk slot
 *   295 RtlLeaveCriticalSectionAndRegion  -- NOT IMPORTED: no thunk slot
 *   306 RtlTryEnterCriticalSection        -- NOT IMPORTED: no thunk slot
 *   101 KeEnterCriticalRegion             -- NOT IMPORTED: no thunk slot
 *   122 KeLeaveCriticalRegion             -- NOT IMPORTED: no thunk slot
 *
 * TSFP imports 151 ordinals and only 277, 291 and 294 of this group are among
 * them, so the guest cannot reach the other five: they have no thunk slot to call
 * through. None of them appears in generated/retail/ordinal_callsites.json, in
 * kernel_arity.inc, or anywhere in the lifted C. Implementing them would mean
 * inventing an arity that nothing in this binary can corroborate, and would also
 * shrink the reported kernel backlog by five ordinals that are not in fact done --
 * the backlog is the work queue, so that would be an actively harmful lie. If a
 * later build of the game imports one, the dispatcher will report it by name,
 * which is the signal to write it then.
 */

#include "kernel_critsec.h"

#include <pthread.h>
#include <stddef.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define ORD_RtlEnterCriticalSection 277u
#define ORD_RtlInitializeCriticalSection 291u
#define ORD_RtlLeaveCriticalSection 294u

/* Field offsets, taken from the struct rather than written out, so the
 * _Static_assert set in guest_structs.h is what guarantees them. There is
 * deliberately NO offset here for +0x14 or +0x18: this module never touches them,
 * and an offset macro for a field whose meaning is unmeasurable would invite one. */
#define CS_OFF_EVENT_HEADER ((uint32_t)offsetof(guest_rtl_critical_section, event_type))
#define CS_OFF_SIGNAL_STATE ((uint32_t)offsetof(guest_rtl_critical_section, event_signal_state))
#define CS_OFF_WAIT_FLINK ((uint32_t)offsetof(guest_rtl_critical_section, event_wait_list_flink))
#define CS_OFF_WAIT_BLINK ((uint32_t)offsetof(guest_rtl_critical_section, event_wait_list_blink))
#define CS_OFF_LOCK_COUNT ((uint32_t)offsetof(guest_rtl_critical_section, lock_count))
#define CS_TOTAL_BYTES ((uint32_t)sizeof(guest_rtl_critical_section))


/* ---------------------------------------------------------------------------
 * THE LOCKS. There are two kinds and the distinction matters.
 *
 * `table_lock` guards this module's own bookkeeping. RECURSIVE, for the same
 * reason guest_mem.c and kernel_object.c are: the critical sections here call
 * kernel_hle_log(), whose sink is caller-supplied and may ask this module a
 * question. Tests install custom sinks.
 *
 * Each tracked critical section additionally owns `lock`, the host mutex the guest
 * thread actually blocks on. THE TABLE LOCK IS ALWAYS RELEASED BEFORE BLOCKING ON
 * ONE OF THOSE. If it were not, a Leave could not reach the table to release the
 * lock the Enter is waiting for, and the two threads would deadlock on our
 * bookkeeping rather than on the guest's lock.
 *
 * WHY THE PER-CS MUTEX IS RECURSIVE EVEN THOUGH WE COUNT RECURSION OURSELVES.
 * A nested Enter takes our own owner fast path and never touches the mutex, so a
 * non-recursive mutex would also work -- right up until a bookkeeping mistake
 * sends a nested Enter down the blocking path, at which point the process
 * deadlocks and the failing assertion is never reached. With a recursive mutex the
 * same mistake surfaces as a wrong counter in a named test. Deadlock is the worst
 * possible way to report a bug, so this costs nothing and buys testability.
 *
 * ENTRIES ARE NEVER FREED OR MOVED. `enter_nolock` holds a pointer to one across a
 * window in which the table lock is dropped, so slot addresses have to be stable
 * for the life of the process. The table is a fixed array and a slot is only ever
 * reused for the same guest address.
 *
 * LOCK ORDER. Nothing in this module calls into kernel_thread.c, kernel_object.c
 * or guest_mem.c's allocator while holding `table_lock` -- only
 * kernel_guest_read_u32/kernel_guest_write_u32, which take no lock of their own --
 * so there is no cycle to order.
 *
 * KNOWN HAZARD, NOT FIXED: if a caller-supplied log sink re-enters this module on
 * the same thread while `table_lock` is held, the recursive mutex lets it in, and
 * an Enter in that nested call would drop its own acquisition only, still leaving
 * the outer one held while it blocks. Nothing in the tree does this and no guest
 * code can cause it, but it is a real consequence of making the table lock
 * recursive and is recorded rather than hidden.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t table_lock;
static pthread_once_t table_lock_once = PTHREAD_ONCE_INIT;

static void table_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&table_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void table_enter(void)
{
    (void)pthread_once(&table_lock_once, table_lock_init);
    (void)pthread_mutex_lock(&table_lock);
}

static void table_leave(void)
{
    (void)pthread_mutex_unlock(&table_lock);
}

typedef struct {
    kernel_guest_ptr cs; /* 0 marks a free slot */
    uint32_t owner;
    uint32_t recursion;
    bool adopted;
    bool lock_ready;
    /* Set when a reset abandoned this slot while its host mutex was still held.
     * See kernel_critsec_reset: the slot can never be handed out again, because
     * the next user would block forever on a lock nobody will release. */
    bool retired;
    pthread_mutex_t lock;
} critsec_entry;

static critsec_entry entries[KERNEL_CRITSEC_MAX];

/* An opaque per-host-thread identity. Assigned lazily under `table_lock`, so the
 * counter needs no atomics, and never 0 so that "unowned" stays distinguishable
 * from "owned by a thread whose token we never set". */
static _Thread_local uint32_t tls_owner_token;
static uint32_t next_owner_token = 1u;

static uint32_t adopted_count;
static uint32_t bad_leave_count;
static uint32_t contended_count;
static uint32_t recursive_enter_count;
static uint32_t implausible_count;
static uint32_t unreadable_count;
static uint32_t table_full_count;
static uint32_t reinit_while_held_count;
/* Deliberately NOT cleared by kernel_critsec_reset: it counts host mutexes that a
 * reset had to abandon, and a reset clearing its own evidence would hide them. */
static uint32_t retired_count;


/* --- guest field access ---------------------------------------------------- */

/* The dword at +0x00: Type, Absolute, Size, Inserted packed little-endian.
 * Composed from the measured constants in guest_structs.h rather than written as
 * the literal 0x00040001, so it cannot drift away from them. */
static uint32_t cs_event_header_word(void)
{
    return ((uint32_t)GUEST_CS_EVENT_TYPE) | (((uint32_t)GUEST_CS_EVENT_SIZE) << 16);
}

/* Does this address hold something shaped like a critical section? Measured
 * discriminators only: Type == 1 (the control event at 0x00549660 has 0 here, which
 * is what made the embedded dispatcher object a measurement) and Size == 4 dwords. */
static bool cs_header_is_plausible(uint32_t header)
{
    const uint32_t type = header & 0xFFu;
    const uint32_t size = (header >> 16) & 0xFFu;
    return type == (uint32_t)GUEST_CS_EVENT_TYPE && size == (uint32_t)GUEST_CS_EVENT_SIZE;
}

static bool cs_read_lock_count(kernel_guest_ptr cs, uint32_t *out)
{
    return kernel_guest_read_u32(cs + CS_OFF_LOCK_COUNT, out);
}

static bool cs_write_lock_count(kernel_guest_ptr cs, uint32_t value)
{
    return kernel_guest_write_u32(cs + CS_OFF_LOCK_COUNT, value);
}

/* Enter raises lock_count, Leave lowers it, from the shipped -1.
 *
 * THIS STEP DIRECTION IS INFERRED, NOT MEASURED, and the inference is labelled
 * here because docs/guest-structs.md lists "the acquire/release algorithm" among
 * the things it could not determine: there is no fast path and no field access
 * anywhere in the image to read it from. What IS measured is the value 0xFFFFFFFF
 * in all three static instances where a plain event holds 2. Reading that -1 as
 * "initialised, unowned" and counting upwards from it is the NT convention; the
 * guest never reads the field, so nothing depends on being right about it except
 * our own diagnostics. */
static void cs_step_lock_count(kernel_guest_ptr cs, int32_t step)
{
    uint32_t value = 0u;
    if (!cs_read_lock_count(cs, &value)) {
        return;
    }
    /* Stepped as UNSIGNED on purpose. The field is a signed counter conceptually --
     * it starts at -1 -- but signed overflow is undefined in C while unsigned wrap
     * is defined and gives bit-for-bit the same two's-complement result. So the
     * arithmetic is done in uint32_t and only the INTERPRETATION is signed. */
    (void)cs_write_lock_count(cs, value + (uint32_t)step);
}


/* --- the table ------------------------------------------------------------- */

static uint32_t owner_token_nolock(void)
{
    if (tls_owner_token == 0u) {
        tls_owner_token = next_owner_token;
        next_owner_token++;
    }
    return tls_owner_token;
}

static critsec_entry *find_nolock(kernel_guest_ptr cs)
{
    for (unsigned i = 0u; i < KERNEL_CRITSEC_MAX; i++) {
        if (entries[i].cs == cs) {
            return &entries[i];
        }
    }
    return NULL;
}

static critsec_entry *claim_slot_nolock(kernel_guest_ptr cs)
{
    for (unsigned i = 0u; i < KERNEL_CRITSEC_MAX; i++) {
        if (entries[i].cs == 0u && !entries[i].retired) {
            critsec_entry *entry = &entries[i];
            entry->cs = cs;
            entry->owner = 0u;
            entry->recursion = 0u;
            entry->adopted = false;
            if (!entry->lock_ready) {
                pthread_mutexattr_t attr;
                (void)pthread_mutexattr_init(&attr);
                (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
                (void)pthread_mutex_init(&entry->lock, &attr);
                (void)pthread_mutexattr_destroy(&attr);
                entry->lock_ready = true;
            }
            return entry;
        }
    }
    table_full_count++;
    kernel_hle_log()("kernel: critical-section table full at %u entries; %#x untracked\n",
                     (unsigned)KERNEL_CRITSEC_MAX, (unsigned)cs);
    return NULL;
}

/**
 * Find the entry for `cs`, creating one from the guest's own fields if we have
 * never seen it.
 *
 * THIS FUNCTION IS THE POINT OF THE MODULE. The three statically-initialised
 * critical sections never pass through ordinal 291 yet carry roughly 105 of the
 * 114 Leave calls, so a table populated only by Initialize would be empty when
 * almost every call arrived. `reason_is_init` only records WHERE the entry came
 * from; an entry is created either way.
 */
static critsec_entry *adopt_nolock(kernel_guest_ptr cs, bool reason_is_init)
{
    if (cs == 0u) {
        unreadable_count++;
        kernel_hle_log()("kernel: critical section at a null address\n");
        return NULL;
    }

    critsec_entry *entry = find_nolock(cs);
    if (entry != NULL) {
        return entry;
    }

    uint32_t header = 0u;
    if (!kernel_guest_read_u32(cs + CS_OFF_EVENT_HEADER, &header)) {
        /* Not adoptable: without readable guest memory we cannot even tell whether
         * this is a critical section, and locking an address we cannot read would
         * invent mutual exclusion over nothing. Counted and named, never silent. */
        unreadable_count++;
        kernel_hle_log()("kernel: critical section at %#x is unreadable; not locking it\n",
                         (unsigned)cs);
        return NULL;
    }

    entry = claim_slot_nolock(cs);
    if (entry == NULL) {
        return NULL;
    }
    entry->adopted = !reason_is_init;
    if (entry->adopted) {
        adopted_count++;
    }

    /* Not checked for an Initialize: making the header valid is precisely what
     * ordinal 291 is for, so whatever was there before is none of our business.
     * Complaining would make every legitimate Initialize of fresh heap memory
     * raise a false alarm, and a diagnostic that cries wolf gets ignored. */
    if (!reason_is_init && !cs_header_is_plausible(header)) {
        /* Adopt anyway. Refusing would turn Enter into a no-op and remove the
         * guest's mutual exclusion, which is far worse than tracking an object
         * that may not be a critical section. Counted so the guess is visible. */
        implausible_count++;
        kernel_hle_log()("kernel: critical section at %#x has header %#x, "
                         "expected type %u size %u; tracking it anyway\n",
                         (unsigned)cs, (unsigned)header, (unsigned)GUEST_CS_EVENT_TYPE,
                         (unsigned)GUEST_CS_EVENT_SIZE);
    }
    return entry;
}


/* --- the handlers --------------------------------------------------------- */

/* ARITY-OK(277): kernel_arity.inc marks ordinal 277 confidence 0, but the
 * disagreement is explained and the minimum is right. Seven of the ten sites push
 * exactly one slot. The three outliers are all in XPP and are callee-saved
 * register saves swallowed by the call bracket, which is the over-counting bias
 * tools/lift/callsites.py documents: at 0x0046D7E0 the bracket contains `push esi`
 * and `push edi` before the real argument, and at 0x0046D90F and 0x0046DA1F the
 * real argument is the literal 0x46C6B8 behind one and two register saves. The
 * same object is the argument at all 114 confidence-1 Leave sites, and the
 * independently disassembled site at 0x0037FBF4 pushes one slot with no `add esp`
 * afterwards. One stack argument, __stdcall. */
static uint32_t enter_critical_section(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    kernel_guest_ptr cs = 0u;
    if (!kernel_frame_arg(frame, 0u, &cs)) {
        kernel_hle_log()("kernel: RtlEnterCriticalSection with no argument frame\n");
        return STATUS_SUCCESS;
    }

    return kernel_critsec_enter_guest(cs);
}

uint32_t kernel_critsec_enter_guest(kernel_guest_ptr cs)
{
    table_enter();
    critsec_entry *entry = adopt_nolock(cs, false);
    if (entry == NULL) {
        table_leave();
        return STATUS_SUCCESS;
    }

    const uint32_t me = owner_token_nolock();
    if (entry->owner == me) {
        /* Already ours. Recursion is tracked here and not in the guest struct
         * because +0x14/+0x18 cannot be told apart; see kernel_critsec.h. */
        entry->recursion++;
        recursive_enter_count++;
        cs_step_lock_count(cs, 1);
        table_leave();
        return STATUS_SUCCESS;
    }

    /* Read under the lock, acted on after it: `entry->owner` is about to be
     * changed by whoever holds it, so sampling it later would be a race on a
     * diagnostic. */
    const bool was_held = entry->owner != 0u;
    pthread_mutex_t *lock = &entry->lock;
    table_leave();

    /* The blocking acquire, with the table lock released. */
    (void)pthread_mutex_lock(lock);

    table_enter();
    if (entry->cs != cs) {
        /* The table was reset while we waited, so this slot no longer describes the
         * critical section we were acquiring. We hold a lock nothing refers to any
         * more; give it straight back rather than recording ownership of a stale
         * slot, and say so, because it means a reset raced a guest thread. */
        kernel_hle_log()("kernel: critical section %#x was reset while thread %u "
                         "waited for it; releasing the stale lock\n",
                         (unsigned)cs, (unsigned)me);
        table_leave();
        (void)pthread_mutex_unlock(lock);
        return STATUS_SUCCESS;
    }
    if (was_held) {
        contended_count++;
    }
    entry->owner = me;
    entry->recursion = 1u;
    cs_step_lock_count(cs, 1);
    table_leave();
    return STATUS_SUCCESS;
}

static uint32_t leave_critical_section(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    kernel_guest_ptr cs = 0u;
    if (!kernel_frame_arg(frame, 0u, &cs)) {
        kernel_hle_log()("kernel: RtlLeaveCriticalSection with no argument frame\n");
        return STATUS_SUCCESS;
    }

    return kernel_critsec_leave_guest(cs);
}

uint32_t kernel_critsec_leave_guest(kernel_guest_ptr cs)
{
    table_enter();
    /* Adopt here too, so that a Leave which is genuinely the first thing we ever
     * see on this address is diagnosed as "left without entering" rather than as
     * an unknown pointer. The two are different bugs. */
    critsec_entry *entry = adopt_nolock(cs, false);
    if (entry == NULL) {
        table_leave();
        return STATUS_SUCCESS;
    }

    const uint32_t me = owner_token_nolock();
    if (entry->owner != me) {
        /* Never pass this silently. Releasing a lock the caller does not hold
         * would let a second thread into a region the first is still inside, and
         * the resulting corruption would surface arbitrarily far away. */
        bad_leave_count++;
        kernel_hle_log()("kernel: RtlLeaveCriticalSection(%#x) by thread %u, "
                         "which does not hold it (owner %u)\n",
                         (unsigned)cs, (unsigned)me, (unsigned)entry->owner);
        table_leave();
        return STATUS_SUCCESS;
    }

    entry->recursion--;
    cs_step_lock_count(cs, -1);
    if (entry->recursion != 0u) {
        /* Still ours: a nested Enter is outstanding, so the host mutex stays held. */
        table_leave();
        return STATUS_SUCCESS;
    }

    entry->owner = 0u;
    pthread_mutex_t *lock = &entry->lock;
    table_leave();
    (void)pthread_mutex_unlock(lock);
    return STATUS_SUCCESS;
}

static uint32_t initialize_critical_section(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    kernel_guest_ptr cs = 0u;
    if (!kernel_frame_arg(frame, 0u, &cs)) {
        kernel_hle_log()("kernel: RtlInitializeCriticalSection with no argument frame\n");
        return STATUS_SUCCESS;
    }

    table_enter();
    critsec_entry *entry = adopt_nolock(cs, true);
    if (entry == NULL) {
        table_leave();
        return STATUS_SUCCESS;
    }

    if (entry->owner != 0u) {
        /* Re-initialising a held critical section is a guest bug. We clear our own
         * record so later calls are coherent, but we do NOT unlock the host mutex:
         * unlocking a recursive mutex this thread may not own is undefined, and
         * guessing here would trade a reported bug for an unreportable one. The
         * lock stays held and is reported, which is the honest outcome. */
        reinit_while_held_count++;
        kernel_hle_log()("kernel: RtlInitializeCriticalSection(%#x) while held by "
                         "thread %u; its host lock stays held\n",
                         (unsigned)cs, (unsigned)entry->owner);
    }
    entry->owner = 0u;
    entry->recursion = 0u;
    entry->adopted = false;

    /* Write back exactly the bytes the XBE ships for its three static critical
     * sections, measured in docs/guest-structs.md. The wait-list head is
     * self-linked to an ABSOLUTE guest address, so it has to be recomputed for
     * this object's own base and cannot be copied from another. */
    (void)kernel_guest_write_u32(cs + CS_OFF_EVENT_HEADER, cs_event_header_word());
    (void)kernel_guest_write_u32(cs + CS_OFF_SIGNAL_STATE, 0u);
    (void)kernel_guest_write_u32(cs + CS_OFF_WAIT_FLINK, cs + GUEST_CS_WAIT_LIST_OFFSET);
    (void)kernel_guest_write_u32(cs + CS_OFF_WAIT_BLINK, cs + GUEST_CS_WAIT_LIST_OFFSET);
    (void)cs_write_lock_count(cs, GUEST_CS_LOCK_COUNT_INIT);

    /* The remaining dwords of the 28-byte object are MEASURED as 0 in all three
     * static images. Reproducing that is copying a measurement; the loop is
     * written over the struct's size on purpose so that neither of those two
     * dwords has to be named or given a meaning here. */
    for (uint32_t off = CS_OFF_LOCK_COUNT + 4u; off < CS_TOTAL_BYTES; off += 4u) {
        (void)kernel_guest_write_u32(cs + off, 0u);
    }

    table_leave();
    return STATUS_SUCCESS;
}


/* --- the locked public surface -------------------------------------------- */

void kernel_critsec_reset(void)
{
    table_enter();
    for (unsigned i = 0u; i < KERNEL_CRITSEC_MAX; i++) {
        /* A HELD slot cannot simply be forgotten. `lock` is still locked and its
         * holder may be another thread, so we can neither unlock it (undefined for
         * a mutex this thread does not own) nor destroy it (undefined while locked).
         * Retire it instead: the address is cleared so a later Enter builds a fresh
         * entry with a fresh mutex, and this slot is never handed out again. The
         * cost is one of KERNEL_CRITSEC_MAX slots; the alternative is a later,
         * unrelated critical section blocking forever on an inherited lock. */
        if (entries[i].owner != 0u && !entries[i].retired) {
            entries[i].retired = true;
            retired_count++;
            kernel_hle_log()("kernel: critical section %#x was still held by thread %u "
                             "at reset; its host lock is retired, not reused\n",
                             (unsigned)entries[i].cs, (unsigned)entries[i].owner);
        }
        entries[i].cs = 0u;
        entries[i].owner = 0u;
        entries[i].recursion = 0u;
        entries[i].adopted = false;
    }
    adopted_count = 0u;
    bad_leave_count = 0u;
    contended_count = 0u;
    recursive_enter_count = 0u;
    implausible_count = 0u;
    unreadable_count = 0u;
    table_full_count = 0u;
    reinit_while_held_count = 0u;
    table_leave();
}

unsigned kernel_critsec_tracked_count(void)
{
    table_enter();
    unsigned tracked = 0u;
    for (unsigned i = 0u; i < KERNEL_CRITSEC_MAX; i++) {
        if (entries[i].cs != 0u) {
            tracked++;
        }
    }
    table_leave();
    return tracked;
}

bool kernel_critsec_state(kernel_guest_ptr cs, kernel_critsec_info *out)
{
    if (out == NULL) {
        return false;
    }
    table_enter();
    const critsec_entry *entry = find_nolock(cs);
    if (entry == NULL) {
        table_leave();
        return false;
    }
    out->cs = entry->cs;
    out->owner = entry->owner;
    out->recursion = entry->recursion;
    out->adopted = entry->adopted;
    out->guest_lock_count = 0u;
    (void)cs_read_lock_count(cs, &out->guest_lock_count);
    table_leave();
    return true;
}

uint32_t kernel_critsec_owner_token(void)
{
    table_enter();
    const uint32_t token = owner_token_nolock();
    table_leave();
    return token;
}

#define CRITSEC_COUNTER_ACCESSOR(fn, var)                                               \
    uint32_t fn(void)                                                                   \
    {                                                                                   \
        table_enter();                                                                  \
        const uint32_t value = (var);                                                   \
        table_leave();                                                                  \
        return value;                                                                   \
    }

CRITSEC_COUNTER_ACCESSOR(kernel_critsec_adopted_count, adopted_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_bad_leave_count, bad_leave_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_contended_count, contended_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_recursive_enter_count, recursive_enter_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_implausible_count, implausible_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_unreadable_count, unreadable_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_table_full_count, table_full_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_reinit_while_held_count, reinit_while_held_count)
CRITSEC_COUNTER_ACCESSOR(kernel_critsec_retired_count, retired_count)

#undef CRITSEC_COUNTER_ACCESSOR

unsigned kernel_critsec_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_RtlEnterCriticalSection, enter_critical_section},
        {ORD_RtlLeaveCriticalSection, leave_critical_section},
        {ORD_RtlInitializeCriticalSection, initialize_critical_section},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
