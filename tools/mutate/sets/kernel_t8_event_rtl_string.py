# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for ordinals 189/225 (event handles) and 260/308 (counted-string conversion).

OWNED BY T258 (reproducible T8 kernel mutation sets). One file per owner, so concurrent
tasks cannot clobber each other's mutation bytes -- see `tools/mutate/sets/_example.py`
for the rule and for the traps it records.

These replace the hand sweeps recorded only as counts in the T8c commits (5fd2e80 "14/14
effective killed", 1c2c50b-era 260/308 sweep). Each entry was re-derived from the source
and the suite, not copied from a lost script, so the ids and counts differ from the
commit messages.

WHAT THESE ARE CHOSEN TO CATCH. Every rule here can be wrong while the boot still
reaches the same place: NtSetEvent's result is only tested for its sign, the string
converters' output is consumed by guest code that never checks the terminator it did not
write, and an off-by-one in the 16-bit counted-string bound only shows at 32766/32767
characters. So each mutation is a plausible answer, not a crash.

EQUIVALENT MUTANTS CONSIDERED AND DELIBERATELY ABSENT (they cannot be killed by any test,
so listing them would only make the sweep report a survivor nobody can act on):
  - the Type and InitialState guard in kernel_object_create_event is a duplicate of the one
    in kernel_event_handle.c's create_event (T270: Type 0 or 1, InitialState low byte 0),
    both returning STATUS_NOT_IMPLEMENTED. Removing the HANDLER's alone changes only the
    log line, so the handler-side Type mutants below only cover the clauses that the
    handler's own tests reach (Type 0 refused again; `||` for `&&` is rejected by the compiler as
    a tautological compare, so it is NOT-A-MUTANT). Widening
    the OBJECT layer's Type check is a real mutant because test_kernel_object calls it
    directly with Type 2.
  - `frame == NULL` in set_event and `context == NULL` in the string converter:
    `kernel_frame_arg(NULL, ...)` fails the same way, with the same status.
  - `characters = (length + 1) / 2` in the narrowing direction: an odd Unicode length is
    refused before it is used, so the rounding is unreachable.
  - `needed >= COUNTED_STRING_MAX_BYTES`: needed is even when widening and at most 0x8000
    when narrowing, so 0xFFFF itself is unreachable.

THE ZERO-BYTE TRAP. Where a mutation drops a terminator write it substitutes a nonzero
byte rather than deleting the line, because `malloc` returns zeroed pages often enough
that deleting the write can look like a survivor on a fresh heap and a kill on a reused
one.
"""

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ 189 NtCreateEvent
    {
        "id": "t8-event-handle-create-accepts-object-attributes",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    if (args[1] != 0u) {\n        uint32_t root_directory = 0u;",
        "new": "    if (args[1] != 0u && false) {\n        uint32_t root_directory = 0u;",
        "targets": ["test_kernel_event_handle"],
        "why": "the named branch is statically dead in the image, so a non-NULL "
        "OBJECT_ATTRIBUTES must be refused rather than creating an unnamed event the "
        "guest believes is named. Without the refusal the unreadable-block case also "
        "stops being reported and the call succeeds with a plausible handle.",
    },
    {
        "id": "t8-event-handle-create-unreadable-attributes-refusal-status",
        "file": "src/xbox/kernel_event_handle.c",
        "old": '                             "at %#x\\n",\n'
        "                             (unsigned)args[1]);\n"
        "            return STATUS_INVALID_PARAMETER;",
        "new": '                             "at %#x\\n",\n'
        "                             (unsigned)args[1]);\n"
        "            return STATUS_NOT_IMPLEMENTED;",
        "targets": ["test_kernel_event_handle"],
        "why": "an unreadable block mirrors NtCreateMutant's contract and is a malformed "
        "argument, not an unimplemented mode. Reporting 'not implemented' for it tells "
        "the next reader the block was understood and merely unsupported.",
    },
    {
        "id": "t8-event-handle-create-named-refusal-reports-invalid-parameter",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "                         (unsigned)object_name, (unsigned)attributes);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": "                         (unsigned)object_name, (unsigned)attributes);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "a readable named block is the measured-but-unmodelled case and must say "
        "so. INVALID_PARAMETER would make a future named caller look like it passed "
        "bad arguments rather than reached an unimplemented namespace.",
    },
    {
        "id": "t8-event-handle-create-irql-guard-dropped",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    if (kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||",
        "new": "    if ((kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE && false) ||",
        "targets": ["test_kernel_event_handle"],
        "why": "NtCreateEvent at DISPATCH_LEVEL is a different NT status, and the object "
        "layer has no IRQL check of its own, so this guard is the only thing that "
        "refuses a create raised above PASSIVE.",
    },
    {
        "id": "t8-event-handle-create-initial-state-high-bytes-refused",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "        (args[2] != 0u && args[2] != 1u) || (args[3] & 0xFFu) != 0u) {",
        "new": "        (args[2] != 0u && args[2] != 1u) || args[3] != 0u) {",
        "targets": ["test_kernel_event_handle"],
        "why": "InitialState is a BOOLEAN and only its low byte counts. The wrapper at "
        "0x37FF30 pushes a register whose upper bytes are not defined, so refusing "
        "them would reject the real startup call depending on stack garbage.",
    },
    {
        "id": "t8-event-handle-create-type-and-state-swapped",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    return kernel_object_create_event(args[2], args[3], args[0]);",
        "new": "    return kernel_object_create_event(args[3], args[2], args[0]);",
        "targets": ["test_kernel_event_handle"],
        "why": "Type and InitialState are both 0/1 flags that read the same in a log. "
        "Swapping them at the call turns every measured Type 1 / state 0 create into a "
        "Type 0 / state 1 one, refused as initially signaled, and breaks every caller.",
    },
    {
        "id": "t8-event-handle-create-handle-out-is-the-type-argument",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    return kernel_object_create_event(args[2], args[3], args[0]);",
        "new": "    return kernel_object_create_event(args[2], args[3], args[1]);",
        "targets": ["test_kernel_event_handle"],
        "why": "the handle out-pointer is the FIRST argument and the attributes pointer "
        "the second; a one-index slip writes the handle through a NULL pointer, which "
        "is refused as a bad out-parameter rather than failing loudly as a wrong site.",
    },
    {
        "id": "t8-event-handle-create-reads-three-arguments",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    uint32_t args[4];\n    for (unsigned i = 0u; i < 4u; i++) {\n"
        "        if (!kernel_frame_arg(frame, i, &args[i])) {\n"
        "            return STATUS_INVALID_PARAMETER;",
        "new": "    uint32_t args[4] = {0u, 0u, 0u, 0u};\n    for (unsigned i = 0u; i < 3u; i++) "
        "{\n"
        "        if (!kernel_frame_arg(frame, i, &args[i])) {\n"
        "            return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_event_handle"],
        "why": "NtCreateEvent takes FOUR arguments. A frame holding only three must be "
        "refused; reading three and defaulting the fourth to the common value 0 hides "
        "a truncated stack as a successful create.",
    },
    {
        "id": "t8-event-handle-create-type0-refused",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "        (args[2] != 0u && args[2] != 1u) || (args[3] & 0xFFu) != 0u) {",
        "new": "        args[2] != 1u || (args[3] & 0xFFu) != 0u) {",
        "targets": ["test_kernel_event_handle"],
        "why": "T270: the XNET caller 0x431D5F asks for Type 0 (NotificationEvent, "
        "manual reset). Refusing it again stops a boot that reaches 0x3BCF0E there.",
    },
    # ------------------------------------------------- kernel_object event creation (189)
    {
        "id": "t8-event-handle-object-initial-state-high-bytes-refused",
        "file": "src/xbox/kernel_object.c",
        "old": "    if ((type != 0u && type != 1u) || (initial & 0xFFu) != 0u) "
        "return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "new": "    if ((type != 0u && type != 1u) || initial != 0u) "
        "return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "same BOOLEAN rule one layer down. The handler masks before calling, so "
        "only a direct caller passing upper garbage reaches it, and the "
        "kernel_object suite is the one that does.",
    },
    {
        "id": "t8-event-handle-object-type0-refused",
        "file": "src/xbox/kernel_object.c",
        "old": "    if ((type != 0u && type != 1u) || (initial & 0xFFu) != 0u) "
        "return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "new": "    if (type != 1u || (initial & 0xFFu) != 0u) return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "targets": ["test_kernel_event_handle", "test_kernel_object", "test_kernel_sync_wait"],
        "why": "T270: the object half is where the Type 0 acceptance lives. Refusing it "
        "here fails the handler as well, which is the case the XNET caller hits.",
    },
    {
        "id": "t8-event-handle-object-accepts-type-2",
        "file": "src/xbox/kernel_object.c",
        "old": "    if ((type != 0u && type != 1u) || (initial & 0xFFu) != 0u) "
        "return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "new": "    if (type > 2u || (initial & 0xFFu) != 0u) return STATUS_NOT_IMPLEMENTED;\n"
        "    object_enter();\n    uint32_t previous;",
        "targets": ["test_kernel_object"],
        "why": "only EVENT types 0 and 1 are measured. The handler refuses Type 2 first, "
        "so only the direct object-layer call in test_kernel_object notices a widened "
        "check that would record an unmodelled type.",
    },
    {
        "id": "t8-event-handle-object-type-recorded-as-synchronization",
        "file": "src/xbox/kernel_object.c",
        "old": "    entry->event_type = type;",
        "new": "    entry->event_type = 1u;",
        "targets": ["test_kernel_event_handle", "test_kernel_object", "test_kernel_sync_wait"],
        "why": "a Type 0 event recorded as Type 1 is created fine but is auto-reset by "
        "its first satisfied wait, which silently turns a manual-reset event into one "
        "the guest has to set again.",
    },
    {
        "id": "t8-event-handle-object-initial-state-recorded-unmasked",
        "file": "src/xbox/kernel_object.c",
        "old": "    entry->event_initial_state = initial & 0xFFu;",
        "new": "    entry->event_initial_state = initial;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "the recorded initial state is what a later reader trusts. Keeping the "
        "caller's upper bytes records a value no BOOLEAN can hold.",
    },
    {
        "id": "t8-event-handle-object-created-signaled",
        "file": "src/xbox/kernel_object.c",
        "old": "    entry->event_signaled = (initial & 0xFFu) != 0u;",
        "new": "    entry->event_signaled = (initial & 0xFFu) == 0u;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "only initially-clear events are accepted, so the flag is always false at "
        "creation. Inverting the test starts every event signaled and every wait "
        "returns at once, which looks like a fast, healthy boot.",
    },
    {
        "id": "t8-event-handle-object-create-probe-write-skipped",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (!kernel_guest_write_u32(out, previous)) {\n"
        "        object_leave();\n"
        "        return STATUS_INVALID_PARAMETER;\n"
        "    }\n"
        "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT, 189u);",
        "new": "    if (!kernel_guest_write_u32(out, previous) && false) {\n"
        "        object_leave();\n"
        "        return STATUS_INVALID_PARAMETER;\n"
        "    }\n"
        "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT, 189u);",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "the same-bytes write probes the out-pointer BEFORE allocating, so a "
        "read-only page is refused with nothing created. Skipping the probe allocates "
        "first and relies on the later publish failing.",
    },
    {
        "id": "t8-event-handle-object-create-thread-body-alias-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {\n"
        "        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&\n"
        "            objects[i].thread_body != 0u &&\n"
        "            overlap(out, 4u, objects[i].thread_body, THREAD_BODY_SPAN)) {\n"
        "            object_leave();\n"
        "            return STATUS_INVALID_PARAMETER;\n"
        "        }\n"
        "    }\n"
        "    /* Same-bytes write probes all four output bytes before allocation.",
        "new": "    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {\n"
        "        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&\n"
        "            objects[i].thread_body != 0u &&\n"
        "            overlap(out, 4u, objects[i].thread_body, THREAD_BODY_SPAN) && false) {\n"
        "            object_leave();\n"
        "            return STATUS_INVALID_PARAMETER;\n"
        "        }\n"
        "    }\n"
        "    /* Same-bytes write probes all four output bytes before allocation.",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "an event handle written over a thread's fabricated body corrupts the "
        "thread. The out-pointer alias refusal is the only thing between a guest "
        "passing its own thread structure as HandleOut and a silently rewritten body.",
    },
    {
        "id": "t8-event-handle-object-exhaustion-status",
        "file": "src/xbox/kernel_object.c",
        "old": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT, "
        "189u);\n"
        "    if (handle == 0u) {\n"
        "        object_leave();\n"
        "        return KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES;",
        "new": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT, "
        "189u);\n"
        "    if (handle == 0u) {\n"
        "        object_leave();\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "a full object table is a resource failure the guest can retry or "
        "report, not an unimplemented feature. The distinction decides whether a "
        "caller keeps trying.",
    },
    # ------------------------------------------------------------------- 225 NtSetEvent
    {
        "id": "t8-event-handle-set-accepts-previous-state",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    if (args[1] != 0u) {\n"
        '        kernel_hle_log()("kernel: NtSetEvent(%#x, %#x) REFUSED',
        "new": "    if (args[1] != 0u && false) {\n"
        '        kernel_hle_log()("kernel: NtSetEvent(%#x, %#x) REFUSED',
        "targets": ["test_kernel_event_handle"],
        "why": "all four measured sites pass the literal 0, so a non-NULL PreviousState "
        "is unmeasured and must be refused with no state change. Accepting it signals "
        "the event and silently drops the previous-state write the guest asked for.",
    },
    {
        "id": "t8-event-handle-set-previous-state-refusal-status",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "                         (unsigned)args[0], (unsigned)args[1]);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": "                         (unsigned)args[0], (unsigned)args[1]);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_event_handle"],
        "why": "SetEvent maps any negative status to FALSE and the report line carries "
        "the reason, but the status itself is what `test_set_event_refuses_unmeasured_"
        "previous_state` pins; a changed status misclassifies an unmodelled mode as a "
        "caller bug.",
    },
    {
        "id": "t8-event-handle-set-uses-previous-state-as-handle",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    const nt_status status = kernel_object_event_set(args[0], NULL);",
        "new": "    const nt_status status = kernel_object_event_set(args[1], NULL);",
        "targets": ["test_kernel_event_handle"],
        "why": "the handle is the LAST pushed and the first popped; PreviousState is the "
        "literal 0 pushed first. Reading the wrong slot sets handle 0 and fails with "
        "INVALID_HANDLE at every call, which every measured site ignores.",
    },
    {
        "id": "t8-event-handle-set-failure-swallowed",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "    return status;\n}\n\nunsigned kernel_event_handle_register(void)",
        "new": "    return STATUS_SUCCESS;\n}\n\nunsigned kernel_event_handle_register(void)",
        "targets": ["test_kernel_event_handle"],
        "why": "XAPI SetEvent tests only the SIGN of this status to decide TRUE or FALSE. "
        "Returning success for a dead or wrong-kind handle makes SetEvent report TRUE "
        "for a call that signaled nothing, and DSOUND ignores the result entirely.",
    },
    {
        "id": "t8-event-handle-set-bound-to-the-wrong-ordinal",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "kernel_hle_register(225u, set_event)",
        "new": "kernel_hle_register(226u, set_event)",
        "targets": ["test_kernel_event_handle"],
        "why": "a correct handler on the wrong number leaves 225 a stub. The measured "
        "SetEvent wrapper then gets the unimplemented-ordinal path, which does not "
        "signal anything and does not fail loudly in a boot that never waits.",
    },
    {
        "id": "t8-event-handle-register-undercounts-set",
        "file": "src/xbox/kernel_event_handle.c",
        "old": "bound += kernel_hle_register(225u, set_event) ? 1u : 0u;",
        "new": "bound += kernel_hle_register(225u, set_event) ? 0u : 0u;",
        "targets": ["test_kernel_event_handle"],
        "why": "the register count is what the registration report and the 'is 225 "
        "missing' list read. A handler bound but not counted reports 225 as absent.",
    },
    # ------------------------------------------ kernel_object_event_set / _signaled (225)
    {
        "id": "t8-event-handle-object-set-dead-handle-status",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry == NULL) {\n"
        "        status = STATUS_INVALID_HANDLE;\n"
        "    } else if (entry->kind != KERNEL_OBJECT_EVENT) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        if (previous != NULL) *previous = entry->event_signaled;",
        "new": "    if (entry == NULL) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else if (entry->kind != KERNEL_OBJECT_EVENT) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        if (previous != NULL) *previous = entry->event_signaled;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "a closed handle and a handle of another kind are different NT statuses "
        "with different guest-visible meaning (last-error 6 versus 0xC000024B). "
        "Folding the dead case into the mismatch loses which one the guest hit.",
    },
    {
        "id": "t8-event-handle-object-set-wrong-kind-signals-anyway",
        "file": "src/xbox/kernel_object.c",
        "old": "    } else if (entry->kind != KERNEL_OBJECT_EVENT) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        if (previous != NULL) *previous = entry->event_signaled;",
        "new": "    } else if (entry->kind != KERNEL_OBJECT_EVENT && false) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        if (previous != NULL) *previous = entry->event_signaled;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "NtSetEvent on a file handle must refuse without touching it. Without "
        "the kind check the flag is written into a file entry's union and succeeds.",
    },
    {
        "id": "t8-event-handle-object-set-never-signals",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (previous != NULL) *previous = entry->event_signaled;\n"
        "        entry->event_signaled = true;",
        "new": "        if (previous != NULL) *previous = entry->event_signaled;\n"
        "        entry->event_signaled = entry->event_signaled;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "the call returns success and logs nothing, so a set that records "
        "nothing is invisible to every SetEvent caller until something waits.",
    },
    {
        "id": "t8-event-handle-object-set-previous-state-inverted",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (previous != NULL) *previous = entry->event_signaled;\n"
        "        entry->event_signaled = true;",
        "new": "        if (previous != NULL) *previous = !entry->event_signaled;\n"
        "        entry->event_signaled = true;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "no measured site reads PreviousState, but the object-level API reports "
        "it and the first caller that does will trust it. An inverted report is a "
        "plausible, wrong answer.",
    },
    {
        "id": "t8-event-handle-object-set-previous-recorded-after-the-write",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (previous != NULL) *previous = entry->event_signaled;\n"
        "        entry->event_signaled = true;",
        "new": "        entry->event_signaled = true;\n"
        "        if (previous != NULL) *previous = entry->event_signaled;",
        "targets": ["test_kernel_event_handle", "test_kernel_object"],
        "why": "reading the state after setting it reports signaled every time, which "
        "is right for a second set and wrong for the first, the one a test that sets "
        "twice would have to look at.",
    },
    {
        "id": "t8-event-handle-object-signaled-kind-unchecked",
        "file": "src/xbox/kernel_object.c",
        "old": "    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_EVENT;\n"
        "    if (result && out != NULL) *out = entry->event_signaled;",
        "new": "    const bool result = entry != NULL;\n"
        "    if (result && out != NULL) *out = entry->event_signaled;",
        "targets": ["test_kernel_event_handle", "test_kernel_sync_wait"],
        "why": "the query answers 'is this an event, and is it signaled'. Answering for "
        "a non-event reads another kind's bytes as a signal state.",
    },
    # ------------------------------------------------------- 260 / 308 string conversion
    {
        "id": "t8-rtl-string-max-bound-one-too-small",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "#define COUNTED_STRING_MAX_BYTES 0xFFFFu",
        "new": "#define COUNTED_STRING_MAX_BYTES 0xFFFDu",
        "targets": ["test_kernel_rtl_string"],
        "why": "32766 bytes widen to 65534 bytes with the terminator, the largest legal "
        "even size. A bound two bytes short refuses it, and only a test at that size "
        "can see it.",
    },
    {
        "id": "t8-rtl-string-max-bound-too-large",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "#define COUNTED_STRING_MAX_BYTES 0xFFFFu",
        "new": "#define COUNTED_STRING_MAX_BYTES 0x10000u",
        "targets": ["test_kernel_rtl_string"],
        "why": "65536 bytes do not fit a 16-bit MaximumLength. Allowing it truncates "
        "the written Length to 0 and the guest sees an empty string for a long input.",
    },
    {
        "id": "t8-rtl-string-allocate-flag-ignored",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if ((args[2] & 0xFFu) != 0u) {",
        "new": "    if ((args[2] & 0xFFu) != 0u && false) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "AllocateDestinationString means the callee allocates the buffer and "
        "overwrites the descriptor's Buffer. Ignoring the flag converts into a buffer "
        "the caller never supplied.",
    },
    {
        "id": "t8-rtl-string-allocate-flag-whole-dword",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if ((args[2] & 0xFFu) != 0u) {",
        "new": "    if (args[2] != 0u) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "BOOLEAN uses its low byte: the pushed dword has undefined upper bytes. "
        "Testing the whole dword refuses a legitimate FALSE with stack garbage above it.",
    },
    {
        "id": "t8-rtl-string-allocate-refusal-status",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "                         who, destination_address, source_address, args[2]);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": "                         who, destination_address, source_address, args[2]);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_rtl_string"],
        "why": "an unmeasured mode is not a malformed argument. The status is what "
        "separates 'the emulator lacks this' from 'the title called it wrongly'.",
    },
    {
        "id": "t8-rtl-string-reads-two-arguments",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    uint32_t args[3];\n    for (unsigned i = 0u; i < 3u; i++) {",
        "new": "    uint32_t args[3] = {0u, 0u, 0u};\n    for (unsigned i = 0u; i < 2u; i++) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "three stdcall arguments. A frame holding two must be refused; defaulting "
        "the Boolean to the common value FALSE accepts a truncated stack.",
    },
    {
        "id": "t8-rtl-string-unreadable-descriptor-status",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "                         who, destination_address, source_address);\n"
        "        return STATUS_ACCESS_VIOLATION;",
        "new": "                         who, destination_address, source_address);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_rtl_string"],
        "why": "a descriptor in unmapped memory is an access violation, the status the "
        "real routine's exception path produces. Reporting an argument error hides "
        "which kind of bad pointer it was.",
    },
    {
        "id": "t8-rtl-string-odd-ansi-length-refused",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (!widening && (source.length & 1u) != 0u) {",
        "new": "    if ((source.length & 1u) != 0u) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "the odd-length rule belongs to the 16-bit Unicode source only. An ANSI "
        "string of 3 bytes is ordinary, and refusing it fails the measured call.",
    },
    {
        "id": "t8-rtl-string-odd-unicode-length-accepted",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (!widening && (source.length & 1u) != 0u) {",
        "new": "    if (!widening && (source.length & 1u) != 0u && false) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "half a UTF-16 code unit has no defined narrowing. Truncating silently "
        "drops the stray byte with no report, and the output length looks fine.",
    },
    {
        "id": "t8-rtl-string-odd-length-refusal-status",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "                         who, source_address, (unsigned)source.length);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "new": "                         who, source_address, (unsigned)source.length);\n"
        "        return STATUS_ACCESS_VIOLATION;",
        "targets": ["test_kernel_rtl_string"],
        "why": "an odd Unicode length is a bad argument, not a bad pointer.",
    },
    {
        "id": "t8-rtl-string-narrowing-count-not-halved",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "widening ? source.length : (uint32_t)(source.length / 2u);",
        "new": "widening ? source.length : (uint32_t)(source.length / 4u);",
        "targets": ["test_kernel_rtl_string"],
        "why": "the narrowing character count is bytes / 2. A different divisor "
        "converts a prefix and reports a shorter, still well-formed string.",
    },
    {
        "id": "t8-rtl-string-widened-length-not-doubled",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "widening ? characters * 2u : characters;",
        "new": "widening ? characters * 3u : characters;",
        "targets": ["test_kernel_rtl_string"],
        "why": "Length of the output in bytes is two per character when widening. A "
        "wrong factor writes a Length the buffer does not match.",
    },
    {
        "id": "t8-rtl-string-widened-terminator-not-counted",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "converted_bytes + (widening ? 2u : 1u);",
        "new": "converted_bytes + (widening ? 1u : 1u);",
        "targets": ["test_kernel_rtl_string"],
        "why": "a Unicode terminator is TWO bytes. Counting one lets a destination "
        "exactly the string's size pass the capacity check while the write overruns.",
    },
    {
        "id": "t8-rtl-string-narrowed-terminator-not-counted",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "converted_bytes + (widening ? 2u : 1u);",
        "new": "converted_bytes + (widening ? 2u : 0u);",
        "targets": ["test_kernel_rtl_string"],
        "why": "the ANSI terminator is one byte and must be in the capacity check, or a "
        "destination exactly the string's length is accepted and its NUL clobbers "
        "the byte after.",
    },
    {
        "id": "t8-rtl-string-capacity-check-allows-exact-fit-minus-one",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (destination.maximum_length < needed) {",
        "new": "    if (destination.maximum_length + 1u < needed) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "one byte short must overflow. This is the off-by-one that writes the "
        "terminator one byte past the guest's buffer.",
    },
    {
        "id": "t8-rtl-string-capacity-check-refuses-exact-fit",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (destination.maximum_length < needed) {",
        "new": "    if (destination.maximum_length <= needed) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "an exactly sized destination is legal and is how the measured caller "
        "sizes it. Refusing it fails every title that sizes buffers tightly.",
    },
    {
        "id": "t8-rtl-string-overflow-status",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "                         (unsigned)needed);\n"
        "        return STATUS_BUFFER_OVERFLOW;",
        "new": "                         (unsigned)needed);\n        return STATUS_NO_MEMORY;",
        "targets": ["test_kernel_rtl_string"],
        "why": "STATUS_BUFFER_OVERFLOW is a warning the guest can resize on; a different "
        "status sends it down its failure path instead.",
    },
    {
        "id": "t8-rtl-string-empty-source-read-attempted",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (input_bytes != 0u &&\n        !kernel_guest_read_bytes(source.buffer, "
        "input, input_bytes)) {",
        "new": "    if (!kernel_guest_read_bytes(source.buffer, input, input_bytes)) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "an empty string legitimately has Buffer NULL (ordinal 289 writes it). "
        "Reading zero bytes from NULL must not be an access violation.",
    },
    {
        "id": "t8-rtl-string-widened-high-byte-not-zero",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            output[2u * i + 1u] = 0u;",
        "new": "            output[2u * i + 1u] = 0x55u;",
        "targets": ["test_kernel_rtl_string"],
        "why": "the high byte of each widened character is zero. Leaving anything else "
        "yields a plausible-looking Unicode string of the wrong code points.",
    },
    {
        "id": "t8-rtl-string-widened-low-byte-masked",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            output[2u * i] = input[i];",
        "new": "            output[2u * i] = input[i] & 0x7Fu;",
        "targets": ["test_kernel_rtl_string"],
        "why": "ANSI above 0x7F widens by Latin-1, unchanged. Masking to 7 bits is the "
        "wrong mapping for exactly the bytes the high-byte report exists for.",
    },
    {
        "id": "t8-rtl-string-high-byte-report-boundary",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            if (input[i] >= 0x80u) {",
        "new": "            if (input[i] > 0x80u) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "0x80 is the first byte whose mapping is INFERRED. Reporting from 0x81 "
        "leaves one inferred byte silently unreported.",
    },
    {
        "id": "t8-rtl-string-widened-terminator-second-byte",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "        output[converted_bytes + 1u] = 0u;",
        "new": "        output[converted_bytes + 1u] = 0x55u;",
        "targets": ["test_kernel_rtl_string"],
        "why": "the Unicode terminator is two zero bytes. A single zero terminates only "
        "when the buffer happened to be zero beyond it.",
    },
    {
        "id": "t8-rtl-string-widened-terminator-first-byte",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "        output[converted_bytes] = 0u;\n        output[converted_bytes + 1u] = 0u;",
        "new": "        output[converted_bytes] = 0x55u;\n        output[converted_bytes + 1u] = "
        "0u;",
        "targets": ["test_kernel_rtl_string"],
        "why": "the first terminator byte is the one a Unicode-aware reader stops on.",
    },
    {
        "id": "t8-rtl-string-narrowed-terminator",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "        output[converted_bytes] = 0u;\n    }\n    const bool wrote",
        "new": "        output[converted_bytes] = 0x55u;\n    }\n    const bool wrote",
        "targets": ["test_kernel_rtl_string"],
        "why": "the ANSI NUL: without it a C-string reader runs past the converted text.",
    },
    {
        "id": "t8-rtl-string-narrowing-high-byte-ignored",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "(uint32_t)input[2u * i] | ((uint32_t)input[2u * i + 1u] << 8);",
        "new": "(uint32_t)input[2u * i];",
        "targets": ["test_kernel_rtl_string"],
        "why": "a code unit is little-endian 16-bit. Dropping the high byte maps 0x0141 "
        "to 'A' instead of '?', a quiet mistranslation.",
    },
    {
        "id": "t8-rtl-string-narrowing-boundary-0xff",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            output[i] = wide <= 0xFFu ? (uint8_t)wide : (uint8_t)'?';",
        "new": "            output[i] = wide < 0xFFu ? (uint8_t)wide : (uint8_t)'?';",
        "targets": ["test_kernel_rtl_string"],
        "why": "U+00FF is the last character with a Latin-1 mapping. An off-by-one turns "
        "it into '?'.",
    },
    {
        "id": "t8-rtl-string-narrowing-unmappable-not-replaced",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            output[i] = wide <= 0xFFu ? (uint8_t)wide : (uint8_t)'?';",
        "new": "            output[i] = (uint8_t)wide;",
        "targets": ["test_kernel_rtl_string"],
        "why": "truncating to the low byte maps U+0141 to 'A' with no replacement.",
    },
    {
        "id": "t8-rtl-string-narrowing-replacement-character",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "(uint8_t)'?';",
        "new": "(uint8_t)'_';",
        "targets": ["test_kernel_rtl_string"],
        "why": "the substitution is '?', the Windows best-fit default for an unmappable "
        "character. Any other byte changes every converted name's text.",
    },
    {
        "id": "t8-rtl-string-narrowing-report-boundary",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "            if (wide > 0xFFu) {\n                replaced++;",
        "new": "            if (wide > 0x100u) {\n                replaced++;",
        "targets": ["test_kernel_rtl_string"],
        "why": "U+0100 is the first unmappable character and the first one that must be "
        "reported and counted.",
    },
    {
        "id": "t8-rtl-string-destination-written-without-terminator",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "kernel_guest_write_bytes(destination.buffer, output, needed);",
        "new": "kernel_guest_write_bytes(destination.buffer, output, converted_bytes);",
        "targets": ["test_kernel_rtl_string"],
        "why": "the terminator is part of the contract even though Length excludes it. "
        "Not writing it leaves whatever the buffer held, and the Length still looks right.",
    },
    {
        "id": "t8-rtl-string-write-failure-reports-success",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "                         (unsigned)needed);\n        return "
        "STATUS_ACCESS_VIOLATION;\n    }\n"
        "    const uint16_t length",
        "new": "                         (unsigned)needed);\n        return STATUS_SUCCESS;\n    "
        "}\n"
        "    const uint16_t length",
        "targets": ["test_kernel_rtl_string"],
        "why": "an unwritable destination text must fault. Success there tells the guest "
        "its string was converted when nothing landed.",
    },
    {
        "id": "t8-rtl-string-length-written-includes-terminator",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    const uint16_t length = (uint16_t)converted_bytes;",
        "new": "    const uint16_t length = (uint16_t)needed;",
        "targets": ["test_kernel_rtl_string"],
        "why": "Length excludes the terminator; MaximumLength is the capacity. Writing "
        "the terminator into Length makes the guest treat it as a character.",
    },
    {
        "id": "t8-rtl-string-replaced-count-is-calls-not-characters",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "__atomic_fetch_add(&nonascii_count, replaced, __ATOMIC_RELAXED);",
        "new": "__atomic_fetch_add(&nonascii_count, 1u, __ATOMIC_RELAXED);",
        "targets": ["test_kernel_rtl_string"],
        "why": "the counter is how a boot report says how many INFERRED mappings it "
        "relied on. Counting calls instead of characters understates that.",
    },
    {
        "id": "t8-rtl-string-replaced-report-gated-on-zero",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (replaced != 0u) {\n        __atomic_fetch_add",
        "new": "    if (replaced != 0u && false) {\n        __atomic_fetch_add",
        "targets": ["test_kernel_rtl_string"],
        "why": "an inferred mapping must be reported where it happens and counted, "
        "or the 'not measured' caveat never reaches the log.",
    },
    {
        "id": "t8-rtl-string-register-keeps-stale-count",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "size_t kernel_rtl_string_register(void)\n{\n    nonascii_count = 0u;",
        "new": "size_t kernel_rtl_string_register(void)\n{",
        "targets": ["test_kernel_rtl_string"],
        "why": "re-registration is how a test or a restarted run resets the counter. "
        "A stale count carries one run's inferred mappings into the next report.",
    },
    {
        "id": "t8-rtl-string-260-bound-to-the-308-ordinal",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "kernel_hle_register(ORD_RtlAnsiStringToUnicodeString, hle_ansi_to_unicode)",
        "new": "kernel_hle_register(ORD_RtlUnicodeStringToAnsiString, hle_ansi_to_unicode)",
        "targets": ["test_kernel_rtl_string"],
        "why": "a correct handler bound to its sibling's number leaves 260 a stub and "
        "makes 308 widen. Both still return success on well-formed input.",
    },
    {
        "id": "t8-rtl-string-308-runs-the-widening-direction",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    return convert(context, DIRECTION_UNICODE_TO_ANSI, "
        '"RtlUnicodeStringToAnsiString");',
        "new": "    return convert(context, DIRECTION_ANSI_TO_UNICODE, "
        '"RtlUnicodeStringToAnsiString");',
        "targets": ["test_kernel_rtl_string"],
        "why": "ordinal 308 narrowing is the half with the odd-length rule, the '?' "
        "replacement and the one-byte terminator; running the other direction loses all three.",
    },
    {
        "id": "t8-rtl-string-register-undercounts",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "    if (kernel_hle_register(ORD_RtlUnicodeStringToAnsiString, "
        "hle_unicode_to_ansi)) {\n"
        "        registered++;",
        "new": "    if (kernel_hle_register(ORD_RtlUnicodeStringToAnsiString, "
        "hle_unicode_to_ansi)) {\n"
        "        registered += 0u;",
        "targets": ["test_kernel_rtl_string"],
        "why": "the returned count feeds the registration report; a bound ordinal "
        "reported absent sends the next reader after a gap that is not there.",
    },
    # ------------------------------------------------------------------ T268 gap closure
    {
        "id": "t8-rtl-string-descriptor-length-write-fault-reported-as-success",
        "file": "src/xbox/kernel_rtl_string.c",
        "old": "        return STATUS_ACCESS_VIOLATION;\n    }\n    if (replaced != 0u) {",
        "new": "        return STATUS_SUCCESS;\n    }\n    if (replaced != 0u) {",
        "targets": ["test_kernel_rtl_string"],
        "why": "260 and 308 share this tail. A descriptor that is readable but not writable "
        "gets its text converted and then cannot take the new Length: answering success "
        "tells the guest the string is converted while its Length still describes the old "
        "text, so the guest reads a stale-length string with no error anywhere.",
    },
]
