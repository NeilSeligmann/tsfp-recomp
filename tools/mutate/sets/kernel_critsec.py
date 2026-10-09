# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/xbox/kernel_critsec.c`, ordinals 277/291/294 (T383).

T351's inventory flagged this as the largest per-file hole in a partly covered
module (635 lines, zero mutations). 294 is the second most-called ordinal in the
image and 277 is where the guest's main thread stops, and the characteristic
failure of a lock implementation is a deadlock, which reports nothing: the suite's
own watchdog turns those into named failures, and this set leans on that (several
mutants are expected to die by watchdog, which the harness counts as a kill).

What these mutations are chosen to catch, by family:

    guest-bytes  the measured header word, the plausibility discriminators (the
                 control event at 0x00549660 is type 0, which is what made Type a
                 measurement), the lock_count field address and step direction,
                 and Initialize's byte-exact image reproduction including the
                 self-relative wait list and the 28-versus-32-byte tail.
    adoption     find-by-address, adopt-on-first-touch (the 105-of-114 property),
                 the unreadable refusal, init-versus-adopt attribution, retired
                 slots never reused, and the table-full refusal being counted.
    enter        the recursive fast path, recursion and contention accounting,
                 ownership recording, and THE architectural property: the table
                 lock is released before blocking on a per-CS mutex.
    leave        foreign-leave refusal (both directions are already in the suite),
                 recursion unwinding before release, and the actual unlock.
    initialize   re-init-while-held reporting and the records it must and must
                 not clear.
    reset        retiring held slots, the retired evidence surviving reset, and
                 the counters that must be cleared.
    dispatch     the ordinal bindings and the one-stack-argument frame reads.
    locks        both mutexes' recursiveness, each observable through a different
                 deadlock the suite can name.

Kill suite: the plain-build ctest binary `test_kernel_critsec` only (no XBE, no
lifted tree; the suite runs against region-allocator scratch on purpose).

EQUIVALENT MUTANTS CONSIDERED AND LEFT OUT, so nobody re-adds them:
  - dropping the `cs == 0u` arm in `adopt_nolock`: a null address then falls into
    the unreadable-header arm, which increments the SAME counter and refuses the
    same way; only the log text differs, and the suite's sinks count lines rather
    than parse them. Counter-identical.
  - the stale-slot recheck after the blocking acquire (`entry->cs != cs`): its
    window is the instructions between releasing the table lock and acquiring an
    UNHELD per-CS mutex while a reset lands in between; no public API can hold
    the race open, so a test would be a timing lottery.
  - `initialize` unlocking the held mutex instead of leaving it held: unlocking a
    recursive mutex the caller may not own is undefined behaviour, and a test
    cannot assert UB; the observable half (the report and the retire at reset)
    is mutated and killed instead.
  - `claim_slot_nolock` skipping the one-time mutex init (`lock_ready`): on glibc
    a zero-filled mutex is a valid normal mutex and fresh slots are zero-filled,
    so every suite-reachable path behaves identically; the recursiveness that
    does matter is mutated separately via the settype line.
  - `cs_step_lock_count` proceeding after a failed read: the paired write fails
    against the same unreadable memory, so nothing observable changes.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

FILE = "src/xbox/kernel_critsec.c"
SUITE = ["test_kernel_critsec"]


def _mut(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"crit-{mutation_id}",
        "file": FILE,
        "old": old,
        "new": new,
        "targets": list(SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------- guest bytes
    _mut(
        "header-word-size-in-wrong-byte",
        "    return ((uint32_t)GUEST_CS_EVENT_TYPE) | (((uint32_t)GUEST_CS_EVENT_SIZE) << 16);",
        "    return ((uint32_t)GUEST_CS_EVENT_TYPE) | (((uint32_t)GUEST_CS_EVENT_SIZE) << 8);",
        "the header word is composed from the measured constants precisely so it "
        "cannot drift from them; a wrong shift writes 0x00000401 where the XBE "
        "ships 0x00040001 and every adopted object then looks implausible to our "
        "own discriminator.",
    ),
    _mut(
        "plausible-ignores-type",
        "    return type == (uint32_t)GUEST_CS_EVENT_TYPE && size == (uint32_t)GUEST_CS_EVENT_SIZE;",
        "    return (type | 1u) == (uint32_t)GUEST_CS_EVENT_TYPE && size == (uint32_t)GUEST_CS_EVENT_SIZE;",
        "Type is the measured discriminator that separates a critical section "
        "from the control event at 0x00549660 (type 0, same size); blurring it "
        "silently blesses adopting the dispatcher's own event object. (The "
        "straight deletion of the type clause is NOT-A-MUTANT: -Werror rejects "
        "the then-unused variable, so the discriminator is weakened while "
        "staying referenced, the xinput optional-flag precedent.)",
    ),
    _mut(
        "plausible-ignores-size",
        "    return type == (uint32_t)GUEST_CS_EVENT_TYPE && size == (uint32_t)GUEST_CS_EVENT_SIZE;",
        "    return type == (uint32_t)GUEST_CS_EVENT_TYPE && size <= (uint32_t)GUEST_CS_EVENT_SIZE;",
        "Size == 4 dwords is the other measured half; weakened to an upper bound, "
        "any type-1 object with a small size byte passes and the implausible "
        "counter stops being the evidence the adoption guess is built on. (The "
        "straight deletion is NOT-A-MUTANT under -Werror, unused variable.)",
    ),
    _mut(
        "lock-count-read-from-signal-state",
        "    return kernel_guest_read_u32(cs + CS_OFF_LOCK_COUNT, out);",
        "    return kernel_guest_read_u32(cs + CS_OFF_SIGNAL_STATE, out);",
        "+0x10 is where the XBE ships 0xFFFFFFFF; reading the signal state "
        "instead reports a lock count of 0 forever, and the step arithmetic "
        "writes values derived from the wrong field.",
    ),
    _mut(
        "lock-count-written-to-signal-state",
        "    return kernel_guest_write_u32(cs + CS_OFF_LOCK_COUNT, value);",
        "    return kernel_guest_write_u32(cs + CS_OFF_SIGNAL_STATE, value);",
        "the write half of the same field: landing at +0x04 corrupts the signal "
        "state dword Initialize just zeroed while +0x10 keeps whatever was "
        "there, a two-field lie from one wrong offset.",
    ),
    _mut(
        "step-direction-inverted",
        "    (void)cs_write_lock_count(cs, value + (uint32_t)step);",
        "    (void)cs_write_lock_count(cs, value - (uint32_t)step);",
        "the step direction is INFERRED (the file says so), but Enter and Leave "
        "agreeing about it is what the tests pin: inverted, the counter walks "
        "away from the shipped -1 in the wrong direction on the very first "
        "Enter.",
    ),
    # ------------------------------------------------------------- owner tokens
    _mut(
        "owner-tokens-not-advanced",
        "        tls_owner_token = next_owner_token;\n        next_owner_token++;",
        "        tls_owner_token = next_owner_token;",
        "every thread then gets token 1 and every ownership question in the "
        "module becomes vacuously 'yes, that is me': foreign leaves pass, "
        "contention reads as recursion.",
    ),
    _mut(
        "owner-token-reassigned-every-call",
        "    if (tls_owner_token == 0u) {",
        "    if (true) {",
        "the token must be stable for the life of the thread; reassigned per "
        "call, a thread stops being the owner of its own lock between its Enter "
        "and its Leave.",
    ),
    # ------------------------------------------------------------- the table
    _mut(
        "find-matches-any-live-entry",
        "        if (entries[i].cs == cs) {",
        "        if (entries[i].cs != 0u || entries[i].cs == cs) {",
        "the table is keyed by guest address and nothing else; matching any live "
        "entry aliases two different critical sections into one host lock, which "
        "serialises strangers and lets a Leave on one release the other. (The "
        "dead second clause keeps `cs` referenced; dropping the parameter use "
        "outright is NOT-A-MUTANT under -Werror.)",
    ),
    _mut(
        "adopt-skips-the-find",
        "    critsec_entry *entry = find_nolock(cs);\n"
        "    if (entry != NULL) {\n"
        "        return entry;\n"
        "    }",
        "    critsec_entry *entry = find_nolock(cs);\n"
        "    if (entry != NULL && false) {\n"
        "        return entry;\n"
        "    }",
        "every call then claims a fresh slot for an address already tracked, so "
        "the recursion count restarts at every nested Enter and the table leaks "
        "one slot per call until the full refusal fires.",
    ),
    _mut(
        "claim-reuses-a-retired-slot",
        "        if (entries[i].cs == 0u && !entries[i].retired) {",
        "        if (entries[i].cs == 0u) {",
        "a retired slot's host mutex is still held by a thread a reset abandoned; "
        "handed out again, the next critical section blocks forever on a lock "
        "nobody will ever release, which is the exact outcome retiring exists to "
        "prevent.",
    ),
    _mut(
        "table-full-not-counted",
        "    table_full_count++;",
        "    table_full_count += 0u;",
        "a full table means guest locks are silently untracked from here on; the "
        "counter is the only machine-readable trace of that degradation, and the "
        "log line alone scrolls away.",
    ),
    _mut(
        "unreadable-adopted-anyway",
        "    if (!kernel_guest_read_u32(cs + CS_OFF_EVENT_HEADER, &header)) {",
        "    if (!kernel_guest_read_u32(cs + CS_OFF_EVENT_HEADER, &header) && false) {",
        "locking an address we cannot read invents mutual exclusion over "
        "nothing: the entry tracks a lock whose guest fields no write will ever "
        "land in, and the refusal counter stays at zero while it happens.",
    ),
    _mut(
        "init-complains-about-stale-header",
        "    if (!reason_is_init && !cs_header_is_plausible(header)) {",
        "    if (!cs_header_is_plausible(header)) {",
        "making the header valid is precisely what ordinal 291 is for, so fresh "
        "heap garbage under an Initialize is none of our business; complaining "
        "there makes every legitimate init cry wolf and the implausible counter "
        "stops meaning anything.",
    ),
    _mut(
        "adoption-attributed-to-init",
        "    entry->adopted = !reason_is_init;",
        "    entry->adopted = false;",
        "adopted-versus-initialised is the record of HOW we learned about each "
        "lock, and the adopted count is the evidence that the 105-of-114 lazy "
        "adoption property actually fires; never set, the whole mechanism reads "
        "as unused.",
    ),
    # ------------------------------------------------------------- dispatch
    _mut(
        "enter-reads-the-second-argument",
        "    if (!kernel_frame_arg(frame, 0u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlEnterCriticalSection with no argument frame\\n");',
        "    if (!kernel_frame_arg(frame, 1u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlEnterCriticalSection with no argument frame\\n");',
        "one stack argument, measured from the guest's own pushes; reading slot "
        "1 enters whatever dword sits above the pointer, and the real lock is "
        "never acquired while the handler still returns STATUS_SUCCESS.",
    ),
    _mut(
        "leave-reads-the-second-argument",
        "    if (!kernel_frame_arg(frame, 0u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlLeaveCriticalSection with no argument frame\\n");',
        "    if (!kernel_frame_arg(frame, 1u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlLeaveCriticalSection with no argument frame\\n");',
        "the Leave half of the same arity: the held lock is never released and "
        "a garbage address gets the leave, so the guest deadlocks on a lock the "
        "trace says it released.",
    ),
    _mut(
        "init-reads-the-second-argument",
        "    if (!kernel_frame_arg(frame, 0u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlInitializeCriticalSection with no argument frame\\n");',
        "    if (!kernel_frame_arg(frame, 1u, &cs)) {\n"
        '        kernel_hle_log()("kernel: RtlInitializeCriticalSection with no argument frame\\n");',
        "ordinal 291 writes seven dwords; aimed at the wrong argument slot they "
        "land somewhere that is not the critical section, which both corrupts "
        "that somewhere and leaves the real object uninitialised.",
    ),
    _mut(
        "enter-bound-to-278",
        "#define ORD_RtlEnterCriticalSection 277u",
        "#define ORD_RtlEnterCriticalSection 278u",
        "a correct handler bound to the wrong ordinal leaves 277 a stub and "
        "quietly implements RtlEnterCriticalSectionAndRegion, which this XBE "
        "does not even import (no thunk slot).",
    ),
    _mut(
        "leave-bound-to-295",
        "#define ORD_RtlLeaveCriticalSection 294u",
        "#define ORD_RtlLeaveCriticalSection 295u",
        "the same defect on the busiest ordinal of the group: 114 Leave sites "
        "would reach a stub while the backlog reports the ordinal done.",
    ),
    # ------------------------------------------------------------- enter
    _mut(
        "enter-fast-path-removed",
        "    const uint32_t me = owner_token_nolock();\n    if (entry->owner == me) {",
        "    const uint32_t me = owner_token_nolock();\n    if (entry->owner == me && false) {",
        "a nested Enter then takes the blocking path; the recursive host mutex "
        "lets it through, but ownership is re-recorded at depth 1 and the "
        "matching Leaves release a lock the guest still thinks it holds twice.",
    ),
    _mut(
        "recursive-enter-not-counted",
        "        entry->recursion++;\n        recursive_enter_count++;",
        "        entry->recursion++;\n        recursive_enter_count += 0u;",
        "the recursion counter is how a reader distinguishes re-entry from "
        "contention in a run report; uncounted, nested locking reads as if it "
        "never happens in this image.",
    ),
    _mut(
        "recursion-not-raised",
        "        entry->recursion++;\n        recursive_enter_count++;",
        "        entry->recursion += 0u;\n        recursive_enter_count++;",
        "an unraised recursion depth makes the FIRST Leave of a nested pair "
        "release the mutex while the guest is still inside its outer region, "
        "which is the corruption-far-away failure the ownership test exists for.",
    ),
    _mut(
        "nested-enter-skips-lock-count",
        "        entry->recursion++;\n"
        "        recursive_enter_count++;\n"
        "        cs_step_lock_count(cs, 1);",
        "        entry->recursion++;\n        recursive_enter_count++;",
        "the guest field must step once per Enter, nested or not, or the "
        "round-trip back to the shipped -1 stops balancing and our own "
        "diagnostics drift from the memory they describe.",
    ),
    _mut(
        "contention-never-observed",
        "    const bool was_held = entry->owner != 0u;",
        "    const bool was_held = false;",
        "was_held is sampled under the table lock BECAUSE the owner is about to "
        "change under us; hardcoded false, the contended counter reads zero on a "
        "run that actually serialised two threads.",
    ),
    _mut(
        "blocking-owner-not-recorded",
        "    entry->owner = me;\n    entry->recursion = 1u;",
        "    entry->recursion = 1u;",
        "an acquisition that does not record its owner makes the holder's own "
        "Leave look foreign and get refused, so the lock is held forever by "
        "nobody the table can name.",
    ),
    _mut(
        "blocking-recursion-zero",
        "    entry->owner = me;\n    entry->recursion = 1u;",
        "    entry->owner = me;\n    entry->recursion = 0u;",
        "depth 0 while held is a state the unwind logic cannot represent: the "
        "first Leave underflows it to a huge depth and the mutex is never "
        "released.",
    ),
    _mut(
        "enter-blocks-holding-the-table-lock",
        "    table_leave();\n"
        "\n"
        "    /* The blocking acquire, with the table lock released. */\n"
        "    (void)pthread_mutex_lock(lock);",
        "    /* The blocking acquire, with the table lock released. */\n"
        "    (void)pthread_mutex_lock(lock);\n"
        "    table_leave();",
        "THE architectural property of the module: blocking while holding the "
        "table lock means the owner's Leave cannot reach the table to release "
        "the lock being waited for, and the two threads deadlock on our "
        "bookkeeping instead of the guest's lock.",
    ),
    # ------------------------------------------------------------- leave
    _mut(
        "foreign-leave-accepted",
        "    const uint32_t me = owner_token_nolock();\n    if (entry->owner != me) {",
        "    const uint32_t me = owner_token_nolock();\n    if (entry->owner != me && false) {",
        "releasing a lock the caller does not hold lets a second thread into a "
        "region the first is still inside; the refusal plus its counter is the "
        "only thing standing between that and corruption that surfaces "
        "arbitrarily far away.",
    ),
    _mut(
        "bad-leave-not-counted",
        "        bad_leave_count++;",
        "        bad_leave_count += 0u;",
        "the refusal happens but leaves no machine-readable trace, so a guest "
        "bug the module explicitly promises never to pass silently is reported "
        "only in a log line the tests do not parse.",
    ),
    _mut(
        "leave-keeps-recursion",
        "    entry->recursion--;\n    cs_step_lock_count(cs, -1);",
        "    entry->recursion += 0u;\n    cs_step_lock_count(cs, -1);",
        "an undecremented depth means the final Leave still sees recursion "
        "outstanding and never unlocks: the lock leaks held, with the guest "
        "counter correctly stepped to hide it.",
    ),
    _mut(
        "leave-skips-lock-count",
        "    entry->recursion--;\n    cs_step_lock_count(cs, -1);",
        "    entry->recursion--;",
        "Leave must step the guest field back down or the round trip to the "
        "shipped -1 never completes and the guest memory claims a holder that "
        "our own table says is gone.",
    ),
    _mut(
        "leave-releases-under-recursion",
        "    if (entry->recursion != 0u) {\n"
        "        /* Still ours: a nested Enter is outstanding, so the host mutex stays held. */\n"
        "        table_leave();\n"
        "        return STATUS_SUCCESS;\n"
        "    }",
        "    if (false) {\n"
        "        /* Still ours: a nested Enter is outstanding, so the host mutex stays held. */\n"
        "        table_leave();\n"
        "        return STATUS_SUCCESS;\n"
        "    }",
        "releasing on the first Leave of a nested pair opens the outer region to "
        "another thread while its owner is still inside it, which is the exact "
        "unwind ordering the suite's recursion cases pin.",
    ),
    _mut(
        "final-leave-keeps-ownership",
        "    entry->owner = 0u;\n    pthread_mutex_t *lock = &entry->lock;",
        "    pthread_mutex_t *lock = &entry->lock;",
        "the mutex is unlocked but the table still names an owner, so the next "
        "Enter by the same thread takes the free fast path at a fabricated "
        "depth and every other thread's Enter reads as contention with a ghost.",
    ),
    _mut(
        "final-leave-keeps-the-mutex",
        "    entry->owner = 0u;\n"
        "    pthread_mutex_t *lock = &entry->lock;\n"
        "    table_leave();\n"
        "    (void)pthread_mutex_unlock(lock);",
        "    entry->owner = 0u;\n"
        "    pthread_mutex_t *lock = &entry->lock;\n"
        "    table_leave();\n"
        "    (void)lock;",
        "the bookkeeping says released while the host mutex stays locked: the "
        "next contending Enter blocks forever on a lock whose owner record says "
        "nobody holds it.",
    ),
    # ------------------------------------------------------------- initialize
    _mut(
        "init-counts-as-adoption",
        "    critsec_entry *entry = adopt_nolock(cs, true);",
        "    critsec_entry *entry = adopt_nolock(cs, false);",
        "an entry created by ordinal 291 is the one kind we were explicitly told "
        "about; counting it adopted both inflates the lazy-adoption evidence and "
        "runs the plausibility complaint over bytes 291 is about to overwrite.",
    ),
    _mut(
        "reinit-while-held-not-counted",
        "        reinit_while_held_count++;",
        "        reinit_while_held_count += 0u;",
        "re-initialising a held lock is a guest bug whose host mutex stays "
        "deliberately locked; the counter is what makes that honest outcome "
        "visible instead of a mystery wedge later.",
    ),
    _mut(
        "init-keeps-the-owner",
        "    entry->owner = 0u;\n    entry->recursion = 0u;\n    entry->adopted = false;",
        "    entry->recursion = 0u;\n    entry->adopted = false;",
        "291 must leave our record coherent (unowned, depth 0) even when it "
        "found the lock held; a stale owner makes the next Enter by that thread "
        "a free recursive re-entry on a lock 291 just declared fresh.",
    ),
    _mut(
        "init-keeps-the-adopted-flag",
        "    entry->owner = 0u;\n    entry->recursion = 0u;\n    entry->adopted = false;",
        "    entry->owner = 0u;\n    entry->recursion = 0u;",
        "once 291 has named a lock it is no longer adopted; keeping the flag "
        "misfiles it in the diagnostic that justifies the adoption design.",
    ),
    _mut(
        "init-header-written-to-signal-state",
        "    (void)kernel_guest_write_u32(cs + CS_OFF_EVENT_HEADER, cs_event_header_word());",
        "    (void)kernel_guest_write_u32(cs + CS_OFF_SIGNAL_STATE, cs_event_header_word());",
        "the header word must land at +0x00 where the XBE ships it; written to "
        "+0x04 it corrupts the signal state and leaves the type/size bytes as "
        "whatever garbage was there, so our own re-adoption would call it "
        "implausible.",
    ),
    _mut(
        "init-wait-list-absolute",
        "    (void)kernel_guest_write_u32(cs + CS_OFF_WAIT_FLINK, cs + GUEST_CS_WAIT_LIST_OFFSET);",
        "    (void)kernel_guest_write_u32(cs + CS_OFF_WAIT_FLINK, GUEST_CS_WAIT_LIST_OFFSET);",
        "the wait-list head is self-linked to an ABSOLUTE guest address and must "
        "be recomputed per object (the file says it cannot be copied from "
        "another); dropping the base writes the offset constant 8 as a pointer.",
    ),
    _mut(
        "init-lock-count-zero",
        "    (void)cs_write_lock_count(cs, GUEST_CS_LOCK_COUNT_INIT);",
        "    (void)cs_write_lock_count(cs, 0u);",
        "0xFFFFFFFF is MEASURED in all three static instances; initialising to 0 "
        "ships a lock that reads as already-entered-once to anything applying "
        "the NT convention, including our own diagnostics.",
    ),
    _mut(
        "init-tail-overruns-the-object",
        "    for (uint32_t off = CS_OFF_LOCK_COUNT + 4u; off < CS_TOTAL_BYTES; off += 4u) {",
        "    for (uint32_t off = CS_OFF_LOCK_COUNT + 4u; off <= CS_TOTAL_BYTES; off += 4u) {",
        "the object is 28 bytes, not 32: one extra iteration zeroes the dword "
        "just past it, which is whatever the guest allocated next to its lock.",
    ),
    _mut(
        "init-tail-erases-the-lock-count",
        "    for (uint32_t off = CS_OFF_LOCK_COUNT + 4u; off < CS_TOTAL_BYTES; off += 4u) {",
        "    for (uint32_t off = CS_OFF_LOCK_COUNT; off < CS_TOTAL_BYTES; off += 4u) {",
        "starting the zero loop one field early overwrites the -1 the previous "
        "line just wrote, so every 291-initialised lock is born with the wrong "
        "shipped value.",
    ),
    # ------------------------------------------------------------- reset
    _mut(
        "reset-forgets-held-mutexes",
        "        if (entries[i].owner != 0u && !entries[i].retired) {\n"
        "            entries[i].retired = true;",
        "        if (false) {\n            entries[i].retired = true;",
        "a held slot wiped without retiring leaves its locked mutex in the free "
        "pool, and the next critical section that lands on it inherits a lock "
        "nobody will release.",
    ),
    _mut(
        "reset-clears-the-retired-evidence",
        "    reinit_while_held_count = 0u;\n    table_leave();",
        "    reinit_while_held_count = 0u;\n    retired_count = 0u;\n    table_leave();",
        "retired_count is deliberately NOT cleared by reset: it counts mutexes "
        "resets abandoned, and a reset erasing its own evidence is the one "
        "cleanup the comment forbids.",
    ),
    _mut(
        "reset-keeps-the-addresses",
        "        entries[i].cs = 0u;\n        entries[i].owner = 0u;",
        "        entries[i].owner = 0u;",
        "a reset that keeps the keyed addresses leaks one test's locks into the "
        "next one's table, so tracked counts and adoption attribution become "
        "order-dependent across every consumer of reset.",
    ),
    _mut(
        "reset-keeps-the-bad-leaves",
        "    adopted_count = 0u;\n    bad_leave_count = 0u;\n    contended_count = 0u;",
        "    adopted_count = 0u;\n    contended_count = 0u;",
        "the diagnostic counters are per-run evidence; one surviving a reset "
        "charges an earlier run's guest bug to a later run's clean one.",
    ),
    # ------------------------------------------------------------- inspection
    _mut(
        "state-skips-the-guest-counter",
        "    (void)cs_read_lock_count(cs, &out->guest_lock_count);",
        "    out->guest_lock_count = out->guest_lock_count;",
        "guest_lock_count is the only window the tests have onto the guest "
        "field, and the round-trip-to--1 property is asserted through it; "
        "unread, the window shows the caller's own stack garbage.",
    ),
    _mut(
        "tracked-counts-every-slot",
        "        if (entries[i].cs != 0u) {\n            tracked++;",
        "        if (true) {\n            tracked++;",
        "tracked_count is the suite's probe for 'did an entry get created'; "
        "counting free slots pins it at KERNEL_CRITSEC_MAX and both the "
        "adoption and the refusal tests lose their observable.",
    ),
    # ------------------------------------------------------------- the locks
    _mut(
        "table-lock-not-recursive",
        "static void table_lock_init(void)\n"
        "{\n"
        "    pthread_mutexattr_t attr;\n"
        "    (void)pthread_mutexattr_init(&attr);\n"
        "    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);",
        "static void table_lock_init(void)\n"
        "{\n"
        "    pthread_mutexattr_t attr;\n"
        "    (void)pthread_mutexattr_init(&attr);\n"
        "    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);",
        "the table lock is recursive because log sinks are caller-supplied and "
        "may ask this module a question mid-log (the file names exactly this); "
        "non-recursive, a sink's innocent query self-deadlocks the kernel.",
    ),
    _mut(
        "cs-mutex-not-recursive",
        "                pthread_mutexattr_t attr;\n"
        "                (void)pthread_mutexattr_init(&attr);\n"
        "                (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);",
        "                pthread_mutexattr_t attr;\n"
        "                (void)pthread_mutexattr_init(&attr);\n"
        "                (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);",
        "the per-CS mutex is recursive so a bookkeeping mistake surfaces as a "
        "wrong counter instead of a deadlock; the reinit-while-held path really "
        "does send the owner back through the blocking acquire, and with a "
        "normal mutex that is a self-deadlock the watchdog has to name.",
    ),
]
