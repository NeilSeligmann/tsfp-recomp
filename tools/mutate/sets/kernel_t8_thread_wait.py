# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for the thread/sync wait paths: ordinals 233, 234 and 99 (T94, T8g).

OWNED BY THE T258 KERNEL-MUTANTS TASK. One file per owner, so concurrent tasks cannot clobber
each other's mutation bytes -- see `tools/mutate/sets/_example.py` for the rule and for the
traps it records. This ports the hand sweeps recorded only as counts in commits c0f2c05
("5 mutants killed: 233 mode, delay cap, absolute interval, no sleep, mode check") and
eac51ee ("hand mutation sweep 34/34 killed") into anchors that are checked, build-free, by
`tests/test_mutation_anchors.py`.

WHAT THESE ARE CHOSEN TO CATCH. A wait that answers the wrong status does not crash anything:
the guest's wrapper at 0x00380029 maps the answer onto a Win32 result and carries on. A mutant
that returns SUCCESS where a poll should say TIMEOUT, a mutant owned by the wrong identity, an
auto-reset event that stays signalled, or a delay that never sleeps all produce a PLAUSIBLE
run. Every entry below is a rule whose violation does that. The wait ordinals reach their
refusals through `wait_refused` (a hook the tests install), so a gate that is removed is seen
as a missing refusal, not a crash.

Targets are `test_kernel_thread_wait` (233/234 THREAD waits and 99), `test_kernel_sync_wait`
(EVENT and MUTANT waits) and `test_kernel_thread_pseudo_wait` (the -80000 and -160000 pauses on
the current-thread pseudo handle, T577). The EVENT/MUTANT acquire rules live in `kernel_object.c`
(`kernel_object_event_try_wait`, `kernel_object_mutant_try_acquire`) and are mutated here too,
because they are the wait's satisfaction rule and `test_kernel_sync_wait` is their only
consumer.

HANG KILLS. Five entries are detected by a hang, not by a failed assertion
(`t8-wait-233-alertable-and-timeout-swapped`, `t8-wait-gate-irql-unchecked`,
`t8-delay-absolute-interval-admitted`, `t8-wait-thread-unrunnable-thread-admitted`) and one
by a 20 s sleep (`t8-delay-cap-constant-widened`, which admits the suite's own 20 s refusal
case, so it is a kill by assertion only when the default 120 s timeout lets it finish): the
defect is a MISSING refusal, so the wait runs on and parks the host. Four of the five are
true hangs, so do NOT pass a short `--test-timeout` expecting a different verdict, only a
faster one.

TEST GAPS FOUND WHILE PORTING, CLOSED BY T268 (each is a mutant below): 233 with an unreadable
argument frame answers STATUS_INVALID_PARAMETER; 99 with WaitMode `0x101` (the `& 0xFFu` mask); 99
at a raised IRQL; 99 at exactly -(MAX+1) (`>=` vs `>` on DELAY_MAX_100NS); a delay of one second
or more (the `/ 10000000u` scale).

NOT COVERED, ON PURPOSE (equivalent, or unreachable from the suites):
  - the `!record->started` and `record->suspend_count != 0u` clauses are redundant with each
    other, see `t8-wait-thread-unrunnable-thread-admitted`.
  - `object.owner_tag != (unsigned)slot` in the THREAD wait: the object entry's tag and the
    slot found by handle agree by construction, so only a forged object table can differ.
  - `reset_in_progress` in the gates: it is set only by `kernel_thread_reset` while another
    thread is mid-wait, a race the suites do not construct.
  - the `tv_nsec >= 1000000000L` carry in the sleep deadlines: it fires only when the wall
    clock's nanosecond field happens to be within the sleep of a second boundary, so no test
    can reach it deterministically.
  - `+ 1u` in the delay's 100 ns conversion: a 100 ns error in a host sleep is below the
    resolution of any assertion (equivalent in practice).
  - `args[3] == 0u` in the pseudo-handle gate: a NULL timeout reads as 0, which the next
    clause (`pause_nanoseconds == 0L`, zero for every timeout but the two measured ones)
    already refuses, so the clause is equivalent.
"""

THREAD_WAIT_TARGETS = [
    "test_kernel_thread_wait",
    "test_kernel_sync_wait",
    "test_kernel_thread_pseudo_wait",
]

MUTATIONS: list[dict] = [
    # ---------------------------------------------------------------- ordinal 233 shim
    {
        "id": "t8-wait-233-mode-is-kernel-mode",
        "file": "src/xbox/kernel_thread.c",
        "old": "    uint32_t args[4] = {guest[0], 1u, guest[1], guest[2]};",
        "new": "    uint32_t args[4] = {guest[0], 0u, guest[1], guest[2]};",
        "targets": THREAD_WAIT_TARGETS,
        "why": "233 has no WaitMode argument, so the shim INFERS UserMode (1) as the measured "
        "234 site does. Forcing KernelMode makes the shared gate refuse every 233 wait, "
        "and a suite that only drove 234 would never notice 233 had stopped working.",
    },
    {
        "id": "t8-wait-233-alertable-and-timeout-swapped",
        "file": "src/xbox/kernel_thread.c",
        "old": "    uint32_t args[4] = {guest[0], 1u, guest[1], guest[2]};",
        "new": "    uint32_t args[4] = {guest[0], 1u, guest[2], guest[1]};",
        "targets": THREAD_WAIT_TARGETS,
        "why": "233 is (Handle, Alertable, Timeout) where 234 is (Handle, Mode, Alertable, "
        "Timeout). Reading the last two in 234's order treats the timeout POINTER as "
        "the alertable flag, so every timed 233 wait is refused as alertable and every "
        "NULL-timeout one only works because both are zero.",
    },
    # ---------------------------------------------------------------- ordinal 234 gate
    {
        "id": "t8-wait-timeout-unreadable-wrong-status",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (args[3] != 0u && !kernel_guest_read_bytes(args[3], &timeout, "
        "sizeof(timeout))) {\n"
        "        return STATUS_ACCESS_VIOLATION;",
        "new": "    if (args[3] != 0u && !kernel_guest_read_bytes(args[3], &timeout, "
        "sizeof(timeout))) {\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a Timeout pointer that does not map is a guest fault. NT answers "
        "STATUS_ACCESS_VIOLATION; a different failure status is a wrong answer on a "
        "path no title takes, which is exactly why nothing else would catch it.",
    },
    {
        "id": "t8-wait-timeout-unreadable-ignored",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (args[3] != 0u && !kernel_guest_read_bytes(args[3], &timeout, "
        "sizeof(timeout))) {",
        "new": "    if ((args[3] != 0u && !kernel_guest_read_bytes(args[3], &timeout, "
        "sizeof(timeout))) &&\n"
        "        false) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "ignoring the failed read leaves `timeout` at its initial 0, so an unmapped "
        "Timeout pointer becomes a zero-timeout POLL and the guest is told the wait "
        "timed out instead of faulting.",
    },
    {
        "id": "t8-wait-gate-mode-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "\n        args[1] != 1u || (args[2] & 0xFFu) != 0u) {",
        "new": "\n        (args[1] != 1u && false) || (args[2] & 0xFFu) != 0u) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "every measured caller pushes WaitMode 1. Admitting KernelMode waits is "
        "unmeasured scope, and the point of the gate is that unmeasured scope stops "
        "the run loudly rather than being answered as if it were measured.",
    },
    {
        "id": "t8-wait-gate-alertable-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "\n        args[1] != 1u || (args[2] & 0xFFu) != 0u) {",
        "new": "\n        args[1] != 1u || ((args[2] & 0xFFu) != 0u && false)) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "no APC queue exists, so an alertable wait can never be interrupted by one. "
        "Admitting it silently returns SUCCESS/TIMEOUT for a wait whose contract "
        "includes STATUS_USER_APC and STATUS_ALERTED.",
    },
    {
        "id": "t8-wait-gate-irql-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||\n"
        "        args[1] != 1u || (args[2] & 0xFFu) != 0u) {",
        "new": "    if (reset_in_progress || (kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE "
        "&& false) ||\n"
        "        args[1] != 1u || (args[2] & 0xFFu) != 0u) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "waiting at DISPATCH_LEVEL or above is a bug check on the real kernel. "
        "Letting it through turns a guest that raised IRQL and forgot to lower it "
        "into a wait that appears to work.",
    },
    # ---------------------------------------------------------------- pseudo handle pause
    {
        "id": "t8-wait-pseudo-interval-off-by-one",
        "file": "src/xbox/kernel_thread.c",
        "old": "#define PSEUDO_PAUSE_8_MS UINT64_C(0xFFFFFFFFFFFEC780)",
        "new": "#define PSEUDO_PAUSE_8_MS UINT64_C(0xFFFFFFFFFFFEC781)",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the worker pause is measured at exactly -80000 (8 ms). The constant is its "
        "entire scope: shifting it by one unit rejects the measured value (and admits "
        "-80001, which the sleep would then treat as 8 ms anyway).",
    },
    {
        "id": "t8-wait-pseudo-16ms-interval-off-by-one",
        "file": "src/xbox/kernel_thread.c",
        "old": "#define PSEUDO_PAUSE_16_MS UINT64_C(0xFFFFFFFFFFFD8F00)",
        "new": "#define PSEUDO_PAUSE_16_MS UINT64_C(0xFFFFFFFFFFFD8F01)",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: the loader thread pause is measured at exactly -160000 (16 ms, "
        "0x00030160 `push 0x10` through the wrapper 0x00380029). Shifting the constant "
        "by one unit rejects the measured value and would admit -159999.",
    },
    {
        "id": "t8-wait-pseudo-16ms-dropped",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout != PSEUDO_PAUSE_8_MS && timeout != PSEUDO_PAUSE_16_MS) {",
        "new": "    if (timeout != PSEUDO_PAUSE_8_MS) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: dropping the second measured interval puts the loader thread back on "
        "the `unsupported thread/object/timeout scope` stop.",
    },
    {
        "id": "t8-wait-pseudo-any-interval-admitted",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout != PSEUDO_PAUSE_8_MS && timeout != PSEUDO_PAUSE_16_MS) {",
        "new": "    if (timeout != PSEUDO_PAUSE_8_MS && timeout != PSEUDO_PAUSE_16_MS && false) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: the scope is the two MEASURED intervals, not 'any relative timeout'. "
        "An unmeasured interval (-240000, 24 ms) must stay refused by name, a generic "
        "sleep would answer a wait no title was measured making.",
    },
    {
        "id": "t8-wait-pseudo-zero-length-gate-dropped",
        "file": "src/xbox/kernel_thread.c",
        "old": "            pause_nanoseconds == 0L || !identity_valid ||",
        "new": "            (pause_nanoseconds == 0L && false) || !identity_valid ||",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: the gate reads the interval through `pause_nanoseconds`, zero for every "
        "timeout but the two measured ones. Without the clause an unmeasured interval "
        "sleeps zero nanoseconds and is answered STATUS_TIMEOUT at once.",
    },
    {
        "id": "t8-wait-pseudo-scale-doubled",
        "file": "src/xbox/kernel_thread.c",
        "old": "    return (long)((UINT64_C(0) - timeout) * UINT64_C(100));",
        "new": "    return (long)((UINT64_C(0) - timeout) * UINT64_C(200));",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: the sleep follows the requested interval (100 ns per unit). A doubled "
        "scale makes the 8 ms pause last 16 ms: every status stays right and only the "
        "upper bound on the shortest of five repeats sees it.",
    },
    {
        "id": "t8-wait-pseudo-fixed-8ms-sleep",
        "file": "src/xbox/kernel_thread.c",
        "old": "            deadline.tv_nsec += pause_nanoseconds;",
        "new": "            deadline.tv_nsec += 8000000L;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "T577: the pre-T577 behaviour, a fixed 8 ms sleep. With -160000 admitted it "
        "would wake the loader thread at half its requested interval and keep every "
        "status right.",
    },
    {
        "id": "t8-wait-pseudo-alertable-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "args[1] != 1u || (args[2] & 0xFFu) != 0u || args[3] == 0u ||",
        "new": "args[1] != 1u || ((args[2] & 0xFFu) != 0u && false) || args[3] == 0u ||",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the pause is measured non-alertable. An alertable one could be woken early "
        "by an APC the model does not have, so it is outside the measured scope.",
    },
    {
        "id": "t8-wait-pseudo-identity-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "            pause_nanoseconds == 0L || !identity_valid ||",
        "new": "            pause_nanoseconds == 0L || (!identity_valid && false) ||",
        "targets": THREAD_WAIT_TARGETS,
        "why": "0xFFFFFFFE names the CALLING thread. When no live, started, unfinished thread "
        "holds that identity (boot thread, a finished slot, a stale handle) the pause "
        "has no thread to pause and must be refused, not slept as if it had one.",
    },
    {
        "id": "t8-wait-pseudo-sleeps-nothing",
        "file": "src/xbox/kernel_thread.c",
        "old": "            deadline.tv_nsec += pause_nanoseconds;",
        "new": "            deadline.tv_nsec += 0L;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the pause is a REAL host deadline, not a modeled clock advance. A pause that "
        "returns at once keeps every status right and removes the one thing the title "
        "waits for, so the worker polling loop spins.",
    },
    {
        "id": "t8-wait-pseudo-reports-success-not-timeout",
        "file": "src/xbox/kernel_thread.c",
        "old": "        return THREAD_WAIT_TIMEOUT;\n    }\n    if (reset_in_progress ||",
        "new": "        return STATUS_SUCCESS;\n    }\n    if (reset_in_progress ||",
        "targets": THREAD_WAIT_TARGETS,
        "why": "an elapsed pause on a handle that never signals is STATUS_TIMEOUT. SUCCESS "
        "tells the title the current thread signalled, which it cannot have.",
    },
    {
        "id": "t8-wait-pseudo-active-count-leaks",
        "file": "src/xbox/kernel_thread.c",
        "old": "        pthread_mutex_lock(&table_lock);\n        active_waits--;\n"
        "        pthread_mutex_unlock(&table_lock);\n        if (error != 0)\n"
        "            refuse_wait(handle,",
        "new": "        pthread_mutex_lock(&table_lock);\n"
        "        pthread_mutex_unlock(&table_lock);\n        if (error != 0)\n"
        "            refuse_wait(handle,",
        "targets": THREAD_WAIT_TARGETS,
        "why": "`kernel_thread_active_wait_count` is what a reset and the host report read to "
        "know a wait is in flight. A pause that never gives its count back reads as a "
        "wait that never ended.",
    },
    # ---------------------------------------------------------------- THREAD waits
    {
        "id": "t8-wait-thread-finite-timeout-admitted",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout != 0u) {\n        pthread_mutex_unlock(&table_lock);\n"
        "        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);",
        "new": "    if (timeout != 0u && false) {\n        pthread_mutex_unlock(&table_lock);\n"
        "        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a THREAD wait accepts only a NULL or explicit-zero timeout. A finite non-zero "
        "one would be answered with the poll result instead of waiting that long.",
    },
    {
        "id": "t8-wait-thread-dead-handle-is-success",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (!live) {\n        pthread_mutex_unlock(&table_lock);\n        return "
        "STATUS_INVALID_HANDLE;",
        "new": "    if (!live) {\n        pthread_mutex_unlock(&table_lock);\n        return "
        "STATUS_SUCCESS;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "waiting on a closed handle is STATUS_INVALID_HANDLE. SUCCESS reads to the "
        "guest as a satisfied wait on a thread that may still be running.",
    },
    {
        "id": "t8-wait-thread-non-thread-object-admitted",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (object.kind != KERNEL_OBJECT_THREAD) {",
        "new": "    if (object.kind != KERNEL_OBJECT_THREAD && false) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "an object that is neither THREAD, EVENT nor MUTANT has no wait model. Falling "
        "through to the thread slot lookup answers for it with a thread's rules.",
    },
    {
        "id": "t8-wait-thread-unrunnable-thread-admitted",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (!record->started || record->suspend_count != 0u ||\n",
        "new": "    if ((!record->started && false) || (record->suspend_count != 0u && false) ||\n",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a created-suspended thread has not started and a suspended one cannot run to "
        "its exit, so a wait on either could never be satisfied. The gate turns that "
        "deadlock into a loud refusal. The two clauses are REDUNDANT with each other "
        "(a started thread is never suspended, and a never-started one has a suspend "
        "count), so dropping either alone is equivalent and only dropping both is a "
        "mutant.",
    },
    {
        "id": "t8-wait-thread-terminated-reports-timeout",
        "file": "src/xbox/kernel_thread.c",
        "old": "            active_waits--;\n            pthread_mutex_unlock(&table_lock);\n"
        "            return STATUS_SUCCESS;",
        "new": "            active_waits--;\n            pthread_mutex_unlock(&table_lock);\n"
        "            return THREAD_WAIT_TIMEOUT;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a terminated thread satisfies the wait with SUCCESS. TIMEOUT would make the "
        "guest keep waiting on a thread that has already gone.",
    },
    {
        "id": "t8-wait-thread-terminated-leaks-the-active-count",
        "file": "src/xbox/kernel_thread.c",
        "old": "        if (record->terminated) {\n            active_waits--;\n",
        "new": "        if (record->terminated) {\n",
        "targets": THREAD_WAIT_TARGETS,
        "why": "every exit from the wait loop owes the count back. A satisfied wait that "
        "keeps it makes `kernel_thread_active_wait_count` report waiters that left.",
    },
    {
        "id": "t8-wait-thread-never-counts-the-wait",
        "file": "src/xbox/kernel_thread.c",
        "old": "    active_waits++;\n    for (;;) {",
        "new": "    for (;;) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "without the increment the decrement on exit underflows the count, and a "
        "reset that waits for the count to reach zero never sees a wait in progress.",
    },
    {
        "id": "t8-wait-thread-finished-without-terminate-wrong-reason",
        "file": "src/xbox/kernel_thread.c",
        "old": "            refuse_wait(handle, KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE, "
        "callback);",
        "new": "            refuse_wait(handle, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a thread that finished without a recorded termination died of a HOST failure. "
        "Reporting a condition-variable error sends the investigation to the wrong "
        "layer.",
    },
    {
        "id": "t8-wait-thread-poll-reports-success",
        "file": "src/xbox/kernel_thread.c",
        "old": "        if (args[3] != 0u) {\n            active_waits--;\n"
        "            pthread_mutex_unlock(&table_lock);\n            return THREAD_WAIT_TIMEOUT;",
        "new": "        if (args[3] != 0u) {\n            active_waits--;\n"
        "            pthread_mutex_unlock(&table_lock);\n            return STATUS_SUCCESS;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "an explicit zero timeout on a running thread is a poll answered STATUS_TIMEOUT "
        "(0x102). SUCCESS would tell the guest a running thread had exited.",
    },
    # ---------------------------------------------------------------- EVENT / MUTANT waits
    {
        "id": "t8-wait-sync-dispatch-swaps-event-and-mutant",
        "file": "src/xbox/kernel_thread.c",
        "old": "    const nt_status status = kind == KERNEL_OBJECT_EVENT ?",
        "new": "    const nt_status status = kind != KERNEL_OBJECT_EVENT ?",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the two satisfaction rules are different calls. Swapping them runs the mutant "
        "acquire on an event handle and the other way round, and both answer "
        "STATUS_OBJECT_TYPE_MISMATCH, a status the wait would then pass on.",
    },
    {
        "id": "t8-wait-sync-mutant-taken-by-the-wrong-identity",
        "file": "src/xbox/kernel_thread.c",
        "old": "kernel_object_mutant_try_acquire(handle, kernel_thread_current_identity(), "
        "&acquired);",
        "new": "kernel_object_mutant_try_acquire(handle, 0u, &acquired);",
        "targets": THREAD_WAIT_TARGETS,
        "why": "ownership is by the CALLING thread identity so that NtReleaseMutant by the "
        "same thread succeeds and by another is MUTANT_NOT_OWNED. A fixed owner makes "
        "every thread look like the owner, so a second thread recurses into a mutant "
        "it should have blocked on.",
    },
    {
        "id": "t8-wait-sync-event-dispatch-dropped",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (live && (object.kind == KERNEL_OBJECT_EVENT || object.kind == "
        "KERNEL_OBJECT_MUTANT)) {",
        "new": "    if (live && ((object.kind == KERNEL_OBJECT_EVENT && false) ||\n"
        "                 object.kind == KERNEL_OBJECT_MUTANT)) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "if an event handle is not routed to the sync wait it reaches the THREAD path "
        "and is refused as unsupported scope, so NtSetEvent would again have no "
        "consumer, which is the state T8g exists to end.",
    },
    {
        "id": "t8-wait-sync-mutant-dispatch-dropped",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (live && (object.kind == KERNEL_OBJECT_EVENT || object.kind == "
        "KERNEL_OBJECT_MUTANT)) {",
        "new": "    if (live && (object.kind == KERNEL_OBJECT_EVENT ||\n"
        "                 (object.kind == KERNEL_OBJECT_MUTANT && false))) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the title's lock class waits on its one mutant, so this is the measured path "
        "and the one that must not regress to a refusal.",
    },
    {
        "id": "t8-wait-sync-invalid-status-swallowed",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (status != STATUS_SUCCESS) {\n        return status;\n    }\n"
        "    /* T764: with --async-file-io the Event",
        "new": "    if (status != STATUS_SUCCESS) {\n        return STATUS_INVALID_HANDLE;\n    }\n"
        "    /* T764: with --async-file-io the Event",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the object layer's own failure (recursion limit, type mismatch) must reach "
        "the guest unchanged. STATUS_MUTANT_LIMIT_EXCEEDED rewritten as INVALID_HANDLE "
        "makes a runaway recursion look like a closed handle.",
    },
    {
        "id": "t8-wait-sync-null-timeout-treated-as-poll",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout_pointer != 0u && timeout == 0u) {\n        return "
        "THREAD_WAIT_TIMEOUT;",
        "new": "    if (timeout == 0u) {\n        return THREAD_WAIT_TIMEOUT;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a NULL timeout pointer is an INFINITE wait, a pointer to zero is a poll. "
        "Conflating them answers TIMEOUT to the lock class's infinite wait on a held "
        "mutant, where the correct response here is the loud WOULD_BLOCK refusal.",
    },
    {
        "id": "t8-wait-sync-finite-timeout-treated-as-poll",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout_pointer != 0u && timeout == 0u) {\n        return "
        "THREAD_WAIT_TIMEOUT;",
        "new": "    (void)timeout;\n    if (timeout_pointer != 0u) {\n"
        "        return THREAD_WAIT_TIMEOUT;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a finite non-zero timeout that cannot be satisfied is REFUSED (no measured "
        "caller uses one). Answering TIMEOUT instantly invents a wait that elapsed "
        "without time passing.",
    },
    {
        "id": "t8-wait-sync-poll-reports-success",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (timeout_pointer != 0u && timeout == 0u) {\n        return "
        "THREAD_WAIT_TIMEOUT;",
        "new": "    if (timeout_pointer != 0u && timeout == 0u) {\n        return STATUS_SUCCESS;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the poll site 0x003BCD80 passes a zero timeout and branches on the result. "
        "SUCCESS for an unsignaled event or held mutant tells it the object is "
        "available when it is not.",
    },
    {
        "id": "t8-wait-sync-would-block-refused-for-the-wrong-reason",
        "file": "src/xbox/kernel_thread.c",
        "old": "    refuse_wait(handle, KERNEL_THREAD_WAIT_WOULD_BLOCK, callback);\n}\n"
        "\n/* The current-thread",
        "new": "    refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);\n}\n"
        "\n/* The current-thread",
        "targets": THREAD_WAIT_TARGETS,
        "why": "WOULD_BLOCK is the reason the host report keys on to say 'a scheduler is "
        "needed here'. Filing it as unsupported scope hides which gap to close next.",
    },
    {
        "id": "t8-wait-sync-satisfied-wait-ignored",
        "file": "src/xbox/kernel_thread.c",
        "old": "    pthread_mutex_unlock(&table_lock);\n    if (acquired) {\n        return "
        "STATUS_SUCCESS;",
        "new": "    pthread_mutex_unlock(&table_lock);\n    if (acquired) {\n        return "
        "THREAD_WAIT_TIMEOUT;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a wait the object could satisfy returns STATUS_SUCCESS at once whatever the "
        "timeout. TIMEOUT here would make the lock class loop on a mutant it had "
        "just taken.",
    },
    # ---------------------------------------------------------------- object-side satisfaction
    {
        "id": "t8-wait-event-auto-reset-not-consumed",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (entry->event_type == 1u) entry->event_signaled = false;",
        "new": "        if (entry->event_type == 1u && false) entry->event_signaled = false;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "type 1 is a SynchronizationEvent: a satisfied wait RESETS it. If it stays "
        "signalled, every later waiter is also released, so a single NtSetEvent wakes "
        "an unbounded number of waits.",
    },
    {
        "id": "t8-wait-event-notification-consumed",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (entry->event_type == 1u) entry->event_signaled = false;",
        "new": "        if (entry->event_type == 1u || entry->event_type == 0u) "
        "entry->event_signaled = false;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "type 0 is a NotificationEvent and stays signalled until NtResetEvent. "
        "Consuming it too is the mirror error: the second waiter blocks on an event "
        "that was set and never reset.",
    },
    {
        "id": "t8-wait-mutant-foreign-owner-recurses",
        "file": "src/xbox/kernel_object.c",
        "old": "    } else if (entry->mutant_owner == owner) {\n        if (entry->mutant_count "
        ">= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "new": "    } else if (entry->mutant_owner == owner || true) {\n        if "
        "(entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "targets": THREAD_WAIT_TARGETS,
        "why": "only the OWNING identity may recurse; any other thread must not acquire a "
        "held mutant. Always recursing removes mutual exclusion, which no single-"
        "threaded assertion can see -- the suite needs a second identity.",
    },
    {
        "id": "t8-wait-mutant-recursion-limit-off-by-one",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {\n        "
        "    status = STATUS_MUTANT_LIMIT_EXCEEDED;",
        "new": "        if (entry->mutant_count > KERNEL_OBJECT_MUTANT_RECURSION_MAX) {\n         "
        "   status = STATUS_MUTANT_LIMIT_EXCEEDED;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "the count is bounded at 0x10000 acquisitions. One more than the bound is the "
        "classic fencepost, invisible unless the suite recurses exactly to the limit.",
    },
    {
        "id": "t8-wait-mutant-recursion-does-not-count",
        "file": "src/xbox/kernel_object.c",
        "old": "            entry->mutant_count++;\n            if (acquired != NULL) *acquired = "
        "true;",
        "new": "            if (acquired != NULL) *acquired = true;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "a recursive acquire that is not counted is released fully by the first "
        "NtReleaseMutant, so the owner loses the mutant while still inside its "
        "second critical section.",
    },
    {
        "id": "t8-wait-mutant-first-acquire-count-zero",
        "file": "src/xbox/kernel_object.c",
        "old": "        entry->mutant_owner = owner;\n        entry->mutant_count = 1u;\n        "
        "if (acquired != NULL) *acquired = true;",
        "new": "        entry->mutant_owner = owner;\n        entry->mutant_count = 0u;\n        "
        "if (acquired != NULL) *acquired = true;",
        "targets": THREAD_WAIT_TARGETS,
        "why": "taking a free mutant must leave it OWNED (count 1). Count 0 means free, so the "
        "wait reports success while the mutant stays available to everyone else.",
    },
    # ---------------------------------------------------------------- ordinal 99 delay
    {
        "id": "t8-delay-access-violation-wrong-status",
        "file": "src/xbox/kernel_thread.c",
        "old": "        return STATUS_ACCESS_VIOLATION;\n    }\n    "
        "pthread_mutex_lock(&table_lock);\n"
        "    void (*callback)(uint32_t, kernel_thread_wait_refusal) = host_ops.wait_refused;\n"
        "    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||\n"
        "        (args[0] & 0xFFu) != 1u",
        "new": "        return STATUS_INVALID_PARAMETER;\n    }\n    "
        "pthread_mutex_lock(&table_lock);\n"
        "    void (*callback)(uint32_t, kernel_thread_wait_refusal) = host_ops.wait_refused;\n"
        "    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||\n"
        "        (args[0] & 0xFFu) != 1u",
        "targets": ["test_kernel_thread_wait"],
        "why": "the Interval pointer is required. A NULL or unmapped one is a guest fault, "
        "STATUS_ACCESS_VIOLATION, not a parameter error.",
    },
    {
        "id": "t8-delay-mode-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "        (args[0] & 0xFFu) != 1u || interval > 0 ||",
        "new": "        ((args[0] & 0xFFu) != 1u && false) || interval > 0 ||",
        "targets": ["test_kernel_thread_wait"],
        "why": "the measured site pushes WaitMode 1. KernelMode delays are unmeasured scope "
        "and must stop the run rather than be slept as if measured.",
    },
    {
        "id": "t8-delay-absolute-interval-admitted",
        "file": "src/xbox/kernel_thread.c",
        "old": "(args[0] & 0xFFu) != 1u || interval > 0 ||",
        "new": "(args[0] & 0xFFu) != 1u || (interval > 0 && false) ||",
        "targets": ["test_kernel_thread_wait"],
        "why": "a positive Interval is an ABSOLUTE system time. This kernel keeps no absolute "
        "clock to wait against, so it must be refused; admitting it computes a "
        "relative duration from a positive number and sleeps for a nonsense span.",
    },
    {
        "id": "t8-delay-zero-interval-refused",
        "file": "src/xbox/kernel_thread.c",
        "old": "(args[0] & 0xFFu) != 1u || interval > 0 ||",
        "new": "(args[0] & 0xFFu) != 1u || interval >= 0 ||",
        "targets": ["test_kernel_thread_wait"],
        "why": "a zero interval is the Sleep(0) yield and completes at once. Refusing it "
        "stops the run on the most common delay a title issues.",
    },
    {
        "id": "t8-delay-cap-constant-widened",
        "file": "src/xbox/kernel_thread.c",
        "old": "#define DELAY_MAX_100NS UINT64_C(100000000)",
        "new": "#define DELAY_MAX_100NS UINT64_C(1000000000)",
        "targets": ["test_kernel_thread_wait"],
        "why": "the 10 s bound (INFERRED, measured max is far lower) keeps a bad interval "
        "from parking the host. A ten times larger cap admits a delay the suite "
        "expects to be refused.",
    },
    {
        "id": "t8-delay-no-sleep",
        "file": "src/xbox/kernel_thread.c",
        "old": "            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, "
        "NULL);",
        "new": "            error = 0;",
        "targets": ["test_kernel_thread_wait"],
        "why": "99 sleeps on the host clock. A delay that returns SUCCESS without waiting "
        "is a status-correct, time-wrong answer that turns every guest poll loop "
        "into a spin.",
    },
    {
        "id": "t8-delay-sub-second-scale-wrong",
        "file": "src/xbox/kernel_thread.c",
        "old": "        deadline.tv_nsec += (long)((hundred_ns % 10000000u) * 100u);",
        "new": "        deadline.tv_nsec += (long)((hundred_ns % 10000000u) * 10u);",
        "targets": ["test_kernel_thread_wait"],
        "why": "each unit is 100 ns. Scaling the sub-second part by 10 makes a 100 ms delay "
        "last 10 ms, a wrong wait that every status assertion still passes.",
    },
    {
        "id": "t8-delay-never-counts-the-wait",
        "file": "src/xbox/kernel_thread.c",
        "old": "    active_waits++;\n    pthread_mutex_unlock(&table_lock);\n    const uint64_t "
        "hundred_ns",
        "new": "    pthread_mutex_unlock(&table_lock);\n    const uint64_t hundred_ns",
        "targets": ["test_kernel_thread_wait"],
        "why": "a sleeping thread is an active wait for reset and reporting. Without the "
        "increment the decrement on exit underflows the count.",
    },
    {
        "id": "t8-delay-active-count-leaks",
        "file": "src/xbox/kernel_thread.c",
        "old": "    pthread_mutex_lock(&table_lock);\n    active_waits--;\n"
        "    pthread_mutex_unlock(&table_lock);\n    if (error != 0) {\n"
        "        refuse_wait(0u, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);",
        "new": "    pthread_mutex_lock(&table_lock);\n"
        "    pthread_mutex_unlock(&table_lock);\n    if (error != 0) {\n"
        "        refuse_wait(0u, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);",
        "targets": ["test_kernel_thread_wait"],
        "why": "a delay that finishes but keeps its count leaves the wait counter above zero "
        "forever, so a later reset believes a thread is still asleep.",
    },
    {
        "id": "t8-delay-elapsed-reports-timeout",
        "file": "src/xbox/kernel_thread.c",
        "old": "        refuse_wait(0u, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);\n    }\n   "
        " return STATUS_SUCCESS;",
        "new": "        refuse_wait(0u, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);\n    }\n   "
        " return THREAD_WAIT_TIMEOUT;",
        "targets": ["test_kernel_thread_wait"],
        "why": "a completed delay is STATUS_SUCCESS even when alertable (no APC queue "
        "exists). The wrapper at 0x00380098 loops on 0x101 only when alertable, so "
        "TIMEOUT would spin an alertable caller forever.",
    },
    {
        "id": "t8-delay-zero-interval-reports-timeout",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (interval == 0) {\n        pthread_mutex_unlock(&table_lock);\n        "
        "return STATUS_SUCCESS;",
        "new": "    if (interval == 0) {\n        pthread_mutex_unlock(&table_lock);\n        "
        "return THREAD_WAIT_TIMEOUT;",
        "targets": ["test_kernel_thread_wait"],
        "why": "the Sleep(0) yield is STATUS_SUCCESS. TIMEOUT in an alertable caller makes "
        "the wrapper loop on the yield indefinitely.",
    },
    # ---------------------------------------------------------------- T268 gap closures
    {
        "id": "t8-wait-233-unreadable-frame-answers-success",
        "file": "src/xbox/kernel_thread.c",
        "old": "        if (!kernel_frame_arg(frame, i, &guest[i])) {\n"
        "            return STATUS_INVALID_PARAMETER;",
        "new": "        if (!kernel_frame_arg(frame, i, &guest[i])) {\n"
        "            return STATUS_SUCCESS;",
        "targets": ["test_kernel_thread_wait"],
        "why": "233 reads its three arguments itself. A frame that cannot be read must be a "
        "parameter error: answering success tells the guest a wait it never described was "
        "satisfied, with the handle read as stale stack garbage.",
    },
    {
        "id": "t8-delay-wait-mode-high-bits-compared",
        "file": "src/xbox/kernel_thread.c",
        "old": "        (args[0] & 0xFFu) != 1u || interval > 0 ||\n        (interval < 0",
        "new": "        args[0] != 1u || interval > 0 ||\n        (interval < 0",
        "targets": ["test_kernel_thread_wait"],
        "why": "the wrapper pushes WaitMode as a byte, so a stack slot whose upper bits are "
        "stale (0x101) still means mode 1. Comparing the whole dword refuses a legal delay "
        "and stops the run on a Sleep the title makes.",
    },
    {
        "id": "t8-delay-irql-unchecked",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||\n"
        "        (args[0] & 0xFFu) != 1u || interval > 0 ||",
        "new": "    if (reset_in_progress || (kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE && "
        "false) ||\n"
        "        (args[0] & 0xFFu) != 1u || interval > 0 ||",
        "targets": ["test_kernel_thread_wait"],
        "why": "a delay at DISPATCH_LEVEL or above is a hard error on NT (nothing may wait "
        "there). Without the gate the host thread sleeps while the guest believes it is "
        "inside a raised-IRQL section, and the run carries on with a plausible status.",
    },
    {
        "id": "t8-delay-cap-boundary-inclusive",
        "file": "src/xbox/kernel_thread.c",
        "old": "(uint64_t)(-(interval + 1)) >= DELAY_MAX_100NS",
        "new": "(uint64_t)(-(interval + 1)) > DELAY_MAX_100NS",
        "targets": ["test_kernel_thread_wait"],
        "why": "-DELAY_MAX_100NS is the longest accepted delay and one unit more is the first "
        "refused. `>` lets that first refused value through and sleeps 10 s, which no "
        "assertion on a far-over-cap interval can see.",
    },
    {
        "id": "t8-delay-whole-seconds-scale-wrong",
        "file": "src/xbox/kernel_thread.c",
        "old": "        deadline.tv_sec += (time_t)(hundred_ns / 10000000u);",
        "new": "        deadline.tv_sec += (time_t)(hundred_ns / 100000000u);",
        "targets": ["test_kernel_thread_wait"],
        "why": "every measured delay is a few milliseconds, so the whole-second part of the "
        "deadline is zero in every test but one: a delay of one second or more then "
        "sleeps only its fraction, and a title's loading pause returns early.",
    },
]
