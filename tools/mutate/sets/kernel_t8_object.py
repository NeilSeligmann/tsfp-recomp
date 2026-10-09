# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for the T8e object ordinals and the T8g mutant ownership model.

NtReleaseMutant (221), NtDuplicateObject (197), and the mutant acquire/release/state API in
`kernel_object.c` that 233/234 and 221 share. Ported from the T8e hand sweeps ("18/18" for 221,
"41/41" for 197 in commits 634c47e and 39b800e) and the T8g mutant ownership work (eac51ee).
One file per owner, see `tools/mutate/sets/_example.py` for the rule and the two traps.

WHY THESE CANNOT FAIL LOUDLY. Every defect here yields a plausible status code or a
plausible handle. A duplicate that names itself instead of the root still reads and writes,
until the original closes and the shared position forks. A mutant released by the wrong
thread still returns success. A duplicate whose copy-out failed leaks one table slot that no
guest variable will ever name. None of them crash, and the live boot reaches the same
place with or without them.

NOT PORTABLE, NOT PRESENT HERE: the two thunk rows (221 at 1 or 3, 197 at 2 or 4) of the T8e
hand sweep live in `src/host/kernel_thunk.c` and are caught only by `test_kernel_t8e_thunk`,
which exists only when the build is configured with a lifted tree (`-DTSFP_LIFTED_DIR`), so
they cannot be expressed against the plain build directory this set runs in.

NOT PORTED, EQUIVALENT: the T8e sweeps removed a redundant NULL-frame guard
(`kernel_frame_arg(NULL)` fails the same way) and `original = entry->handle` vs `= source`
is the same value by construction, so neither has an entry.

CLOSED BY T268: the owner_tag a duplicate is minted with (`ORD_NtDuplicateObject`) is now asserted
by
`test_197_duplicates_a_file_handle_onto_the_same_open_file`, so its swap is a mutant here.
"""

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------ ordinal 221 handler
    {
        "id": "t8-object-221-ordinal-off-by-one",
        "file": "src/xbox/kernel_object.c",
        "old": "#define ORD_NtReleaseMutant 221u",
        "new": "#define ORD_NtReleaseMutant 220u",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the handler can be right and bound to the wrong number, which leaves 221 "
        "a stub that answers success for a release that never happened.",
    },
    {
        "id": "t8-object-221-non-null-previous-count-accepted",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (args[1] != 0u) {\n"
        '        kernel_hle_log()("kernel: NtReleaseMutant(%#x, %#x) REFUSED: a non-NULL '
        'PreviousCount "',
        "new": "    if (args[1] != 0u && false) {\n"
        '        kernel_hle_log()("kernel: NtReleaseMutant(%#x, %#x) REFUSED: a non-NULL '
        'PreviousCount "',
        "targets": ["test_kernel_mutant_dup"],
        "why": "PreviousCount is unmeasured (the one site passes 0). Accepting it means "
        "a guest pointer is silently ignored and the caller reads a stale local as the "
        "previous count.",
    },
    {
        "id": "t8-object-221-previous-count-refusal-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"is unmeasured (the one site passes the literal 0)\\n",\n'
        "                         (unsigned)args[0], (unsigned)args[1]);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": '"is unmeasured (the one site passes the literal 0)\\n",\n'
        "                         (unsigned)args[0], (unsigned)args[1]);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "an unmeasured shape is refused with the status that says so. "
        "STATUS_INVALID_PARAMETER tells the guest its own argument was wrong.",
    },
    {
        "id": "t8-object-221-dead-handle-status",
        "file": "src/xbox/kernel_object.c",
        "old": '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- not a live handle\\n",\n'
        "                         (unsigned)args[0]);\n"
        "        return STATUS_INVALID_HANDLE;",
        "new": '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- not a live handle\\n",\n'
        "                         (unsigned)args[0]);\n"
        "        return STATUS_OBJECT_TYPE_MISMATCH;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a closed or never-issued handle answers STATUS_INVALID_HANDLE. Another "
        "failure code still makes the title log 'Error leaving critical section' but "
        "hides that the handle, not the type, was the problem.",
    },
    {
        "id": "t8-object-221-any-kind-is-a-mutant",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->kind != KERNEL_OBJECT_MUTANT) {\n"
        '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- the handle is not a mutant\\n",',
        "new": "    if (entry->kind != KERNEL_OBJECT_MUTANT && false) {\n"
        '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- the handle is not a mutant\\n",',
        "targets": ["test_kernel_mutant_dup"],
        "why": "without the kind check an event or file handle is run through the mutant "
        "ownership rule on fields it does not use, and answers NOT_OWNED, so the type "
        "mismatch is reported as the wrong problem.",
    },
    {
        "id": "t8-object-221-type-mismatch-status",
        "file": "src/xbox/kernel_object.c",
        "old": '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- the handle is not a '
        'mutant\\n",\n'
        "                         (unsigned)args[0]);\n"
        "        return STATUS_OBJECT_TYPE_MISMATCH;",
        "new": '        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- the handle is not a '
        'mutant\\n",\n'
        "                         (unsigned)args[0]);\n"
        "        return STATUS_INVALID_HANDLE;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a live handle of another kind is not a dead handle. Folding the two "
        "statuses together hides the title passing the wrong handle.",
    },
    {
        "id": "t8-object-221-releases-as-the-recorded-owner",
        "file": "src/xbox/kernel_object.c",
        "old": "    const nt_status status = mutant_release_nolock(entry, "
        "kernel_thread_current_identity(), NULL);",
        "new": "    const nt_status status = mutant_release_nolock(entry, entry->mutant_owner, "
        "NULL);",
        "targets": ["test_kernel_sync_wait"],
        "why": "the ordinal must judge the CALLING thread. Passing the recorded owner "
        "back lets any thread release a mutant another thread holds, which is the one "
        "thing the ownership model exists to refuse. An unowned mutant still refuses "
        "(count 0), so only a held-by-another case notices.",
    },
    {
        "id": "t8-object-221-unowned-release-not-counted",
        "file": "src/xbox/kernel_object.c",
        "old": "        mutant_unowned_release_count++;",
        "new": "        mutant_unowned_release_count += 0u;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the counter is how a run that released a mutant it never held is told "
        "apart from a clean one. Uncounted, the status still returns and nothing "
        "summarises it.",
    },
    {
        "id": "t8-object-221-not-owned-status-swallowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    return status;\n}\n\n/*\n * Ordinal 197 NtDuplicateObject",
        "new": "    return STATUS_SUCCESS;\n}\n\n/*\n * Ordinal 197 NtDuplicateObject",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the handler computes the right status and then discards it. Every "
        "release answers success, so the title's 'Error leaving critical section' "
        "path can never be reached and an unbalanced Unlock goes unnoticed.",
    },
    # ----------------------------------------------------- mutant ownership (T8g model)
    {
        "id": "t8-mutant-release-ignores-the-owner",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->mutant_count == 0u || entry->mutant_owner != owner) {",
        "new": "    if (entry->mutant_count == 0u || (entry->mutant_owner != owner && false)) {",
        "targets": ["test_kernel_sync_wait"],
        "why": "release by a thread that is not the owner must be refused. Dropping the "
        "owner comparison keeps every single-thread assertion green.",
    },
    {
        "id": "t8-mutant-release-ignores-an-unowned-mutant",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->mutant_count == 0u || entry->mutant_owner != owner) {",
        "new": "    if (entry->mutant_owner != owner) {",
        "targets": ["test_kernel_sync_wait", "test_kernel_mutant_dup"],
        "why": "an unowned mutant has owner 0 and count 0. Without the count test a "
        "caller whose identity is also 0, or any owner field left behind, decrements "
        "an unsigned zero and wraps the recursion count to 0xFFFFFFFF.",
    },
    {
        "id": "t8-mutant-release-count-requires-both-conditions",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->mutant_count == 0u || entry->mutant_owner != owner) {",
        "new": "    if (entry->mutant_count == 0u && entry->mutant_owner != owner) {",
        "targets": ["test_kernel_sync_wait", "test_kernel_mutant_dup"],
        "why": "`||` to `&&` refuses only when BOTH are wrong: an unowned mutant is then "
        "released by a stranger and a held one is released by a non-owner.",
    },
    {
        "id": "t8-mutant-release-previous-is-the-count-after",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (previous != NULL) *previous = entry->mutant_count;\n"
        "    entry->mutant_count--;",
        "new": "    entry->mutant_count--;\n"
        "    if (previous != NULL) *previous = entry->mutant_count;",
        "targets": ["test_kernel_sync_wait"],
        "why": "NT's PreviousCount is the count BEFORE the release. Reading it after "
        "reports 0 for the common single hold, which is also what 'unowned' looks like.",
    },
    {
        "id": "t8-mutant-release-previous-never-written",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (previous != NULL) *previous = entry->mutant_count;\n"
        "    entry->mutant_count--;",
        "new": "    if (previous != NULL && false) *previous = entry->mutant_count;\n"
        "    entry->mutant_count--;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the output is optional, so a release that forgets it still succeeds.",
    },
    {
        "id": "t8-mutant-release-clears-the-whole-recursion",
        "file": "src/xbox/kernel_object.c",
        "old": "    entry->mutant_count--;\n"
        "    if (entry->mutant_count == 0u) entry->mutant_owner = 0u;",
        "new": "    entry->mutant_count = 0u;\n"
        "    if (entry->mutant_count == 0u) entry->mutant_owner = 0u;",
        "targets": ["test_kernel_sync_wait"],
        "why": "a recursive hold of N needs N releases. Zeroing the count lets the first "
        "release hand the mutant to anyone while the owner still believes it holds it.",
    },
    {
        "id": "t8-mutant-release-keeps-the-owner-at-zero-count",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->mutant_count == 0u) entry->mutant_owner = 0u;\n"
        "    return STATUS_SUCCESS;",
        "new": "    return STATUS_SUCCESS;",
        "targets": ["test_kernel_sync_wait"],
        "why": "an unowned mutant must read owner 0. A stale owner makes the state "
        "accessor report a holder that is gone.",
    },
    {
        "id": "t8-mutant-release-clears-the-owner-on-every-release",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->mutant_count == 0u) entry->mutant_owner = 0u;\n"
        "    return STATUS_SUCCESS;",
        "new": "    entry->mutant_owner = 0u;\n    return STATUS_SUCCESS;",
        "targets": ["test_kernel_sync_wait"],
        "why": "clearing the owner on an inner release leaves a held mutant (count > 0) "
        "with no owner, so the second release is refused as NOT_OWNED.",
    },
    {
        "id": "t8-mutant-acquire-does-not-record-the-owner",
        "file": "src/xbox/kernel_object.c",
        "old": "        entry->mutant_owner = owner;\n        entry->mutant_count = 1u;",
        "new": "        entry->mutant_owner = 0u;\n        entry->mutant_count = 1u;",
        "targets": ["test_kernel_sync_wait"],
        "why": "a first acquire that forgets who took it makes the release by the owner "
        "answer NOT_OWNED and any other caller's wait look recursive.",
    },
    {
        "id": "t8-mutant-acquire-first-count-is-two",
        "file": "src/xbox/kernel_object.c",
        "old": "        entry->mutant_owner = owner;\n        entry->mutant_count = 1u;",
        "new": "        entry->mutant_owner = owner;\n        entry->mutant_count = 2u;",
        "targets": ["test_kernel_sync_wait"],
        "why": "an off-by-one on the first hold means a single Unlock leaves the mutant "
        "held forever.",
    },
    {
        "id": "t8-mutant-acquire-first-take-not-reported",
        "file": "src/xbox/kernel_object.c",
        "old": "        entry->mutant_count = 1u;\n        if (acquired != NULL) *acquired = true;",
        "new": "        entry->mutant_count = 1u;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the state changes but `*acquired` stays false, so the wait goes on to "
        "refuse as WOULD_BLOCK on a mutant the caller just took.",
    },
    {
        "id": "t8-mutant-acquire-recursion-for-any-owner",
        "file": "src/xbox/kernel_object.c",
        "old": "    } else if (entry->mutant_owner == owner) {\n"
        "        if (entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "new": "    } else if (entry->mutant_owner == owner || true) {\n"
        "        if (entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "targets": ["test_kernel_sync_wait"],
        "why": "a mutant held by another thread must NOT be taken. Granting it to any "
        "caller is mutual exclusion that excludes nothing.",
    },
    {
        "id": "t8-mutant-acquire-recursion-limit-off-by-one",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "new": "        if (entry->mutant_count > KERNEL_OBJECT_MUTANT_RECURSION_MAX) {",
        "targets": ["test_kernel_sync_wait"],
        "why": "the cap is 0x10000 holds. One more lets the count reach 0x10001, which "
        "the exact-boundary assertion exists to forbid.",
    },
    {
        "id": "t8-mutant-acquire-limit-status",
        "file": "src/xbox/kernel_object.c",
        "old": "            status = STATUS_MUTANT_LIMIT_EXCEEDED;",
        "new": "            status = STATUS_UNSUCCESSFUL;",
        "targets": ["test_kernel_sync_wait"],
        "why": "NT answers STATUS_MUTANT_LIMIT_EXCEEDED past the cap. Any other code "
        "reads to a guest as a generic failure.",
    },
    {
        "id": "t8-mutant-acquire-recursion-count-not-incremented",
        "file": "src/xbox/kernel_object.c",
        "old": "            entry->mutant_count++;\n            if (acquired != NULL) *acquired = "
        "true;",
        "new": "            if (acquired != NULL) *acquired = true;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the owner re-enters, is told it acquired, and the count stays 1, so its "
        "second release is refused as NOT_OWNED.",
    },
    {
        "id": "t8-mutant-acquire-recursion-not-reported",
        "file": "src/xbox/kernel_object.c",
        "old": "            entry->mutant_count++;\n            if (acquired != NULL) *acquired = "
        "true;",
        "new": "            entry->mutant_count++;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the count rises but `*acquired` stays false, so a recursive wait is "
        "refused as WOULD_BLOCK against the very thread that owns it.",
    },
    {
        "id": "t8-mutant-acquire-dead-handle-status",
        "file": "src/xbox/kernel_object.c",
        "old": "    nt_status status = STATUS_SUCCESS;\n"
        "    if (entry == NULL) {\n"
        "        status = STATUS_INVALID_HANDLE;\n"
        "    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {",
        "new": "    nt_status status = STATUS_SUCCESS;\n"
        "    if (entry == NULL) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {",
        "targets": ["test_kernel_sync_wait"],
        "why": "a dead handle on the wait side must answer STATUS_INVALID_HANDLE, the "
        "same as the release side, or the same mistake reads two ways.",
    },
    {
        "id": "t8-mutant-acquire-wrong-kind-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else if (entry->mutant_count == 0u) {",
        "new": "    } else if (entry->kind != KERNEL_OBJECT_MUTANT && false) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else if (entry->mutant_count == 0u) {",
        "targets": ["test_kernel_sync_wait"],
        "why": "an event or file handle waited on as a mutant would be 'acquired' by "
        "writing mutant fields into an entry of another kind.",
    },
    {
        "id": "t8-mutant-release-api-dead-handle-status",
        "file": "src/xbox/kernel_object.c",
        "old": "    nt_status status;\n"
        "    if (entry == NULL) {\n"
        "        status = STATUS_INVALID_HANDLE;",
        "new": "    nt_status status;\n"
        "    if (entry == NULL) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the library release path has its own dead-handle arm, separate from the "
        "ordinal's. Only a direct call reaches it.",
    },
    {
        "id": "t8-mutant-release-api-wrong-kind-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        status = mutant_release_nolock(entry, owner, previous);",
        "new": "    } else if (entry->kind != KERNEL_OBJECT_MUTANT && false) {\n"
        "        status = STATUS_OBJECT_TYPE_MISMATCH;\n"
        "    } else {\n"
        "        status = mutant_release_nolock(entry, owner, previous);",
        "targets": ["test_kernel_sync_wait"],
        "why": "the library release must refuse a non-mutant itself, since it is "
        "callable without going through ordinal 221.",
    },
    {
        "id": "t8-mutant-state-accepts-any-kind",
        "file": "src/xbox/kernel_object.c",
        "old": "    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_MUTANT;\n"
        "    if (result) {\n"
        "        if (owner != NULL) *owner = entry->mutant_owner;",
        "new": "    const bool result = entry != NULL;\n"
        "    if (result) {\n"
        "        if (owner != NULL) *owner = entry->mutant_owner;",
        "targets": ["test_kernel_sync_wait"],
        "why": "the accessor reports 'live mutant' as a bool. Answering true for an "
        "event hands the caller owner and count 0 as if they were facts.",
    },
    {
        "id": "t8-mutant-state-swaps-owner-and-count",
        "file": "src/xbox/kernel_object.c",
        "old": "        if (owner != NULL) *owner = entry->mutant_owner;\n"
        "        if (count != NULL) *count = entry->mutant_count;",
        "new": "        if (owner != NULL) *owner = entry->mutant_count;\n"
        "        if (count != NULL) *count = entry->mutant_owner;",
        "targets": ["test_kernel_sync_wait"],
        "why": "owner 1 with count 1 reads identically either way, so only a case with "
        "distinct owner and count notices.",
    },
    # ------------------------------------------------------------ ordinal 197 handler
    {
        "id": "t8-object-197-ordinal-off-by-one",
        "file": "src/xbox/kernel_object.c",
        "old": "#define ORD_NtDuplicateObject 197u",
        "new": "#define ORD_NtDuplicateObject 196u",
        "targets": ["test_kernel_mutant_dup"],
        "why": "right handler, wrong number: 197 stays a stub and DuplicateHandle fails "
        "for the one XONLINE site.",
    },
    {
        "id": "t8-object-197-same-access-value",
        "file": "src/xbox/kernel_object.c",
        "old": "#define KERNEL_OBJECT_DUPLICATE_SAME_ACCESS 2u",
        "new": "#define KERNEL_OBJECT_DUPLICATE_SAME_ACCESS 3u",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the one measured Options value is 2. A constant that drifts refuses "
        "the title's own call.",
    },
    {
        "id": "t8-object-197-options-judged-as-a-bit-test",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (options != KERNEL_OBJECT_DUPLICATE_SAME_ACCESS) {",
        "new": "    if ((options & KERNEL_OBJECT_DUPLICATE_SAME_ACCESS) == 0u) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "Options is a flag set and only exactly 2 is measured. Testing the bit "
        "also accepts 3 (CLOSE_SOURCE too), which closes nothing and says nothing.",
    },
    {
        "id": "t8-object-197-unmeasured-options-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"one site passes the literal 2\\n",\n'
        "                         (unsigned)source, (unsigned)target_out, (unsigned)options);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": '"one site passes the literal 2\\n",\n'
        "                         (unsigned)source, (unsigned)target_out, (unsigned)options);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "refusals of unmeasured shapes are STATUS_NOT_IMPLEMENTED, so a trace "
        "tells 'we did not model this' from 'the guest was wrong'.",
    },
    {
        "id": "t8-object-197-null-target-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (target_out == 0u) {\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a NULL TargetHandle is '
        '"',
        "new": "    if (target_out == 0u && false) {\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a NULL TargetHandle is '
        '"',
        "targets": ["test_kernel_mutant_dup"],
        "why": "NtDuplicateObject with a NULL target is legal NT (it only closes the "
        "source) but unmeasured here. Falling through allocates, fails the write to "
        "address 0 and answers INVALID_PARAMETER instead of the refusal.",
    },
    {
        "id": "t8-object-197-null-target-refusal-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"unmeasured (the one site passes the address of a local)\\n",\n'
        "                         (unsigned)source);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": '"unmeasured (the one site passes the address of a local)\\n",\n'
        "                         (unsigned)source);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the same NOT_IMPLEMENTED contract for the NULL-target refusal.",
    },
    {
        "id": "t8-object-197-current-process-pseudo-handle-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (source == 0xFFFFFFFFu || source == 0xFFFFFFFEu) {",
        "new": "    if (source == 0xFFFFFFFEu) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "-1 (current process) is a pseudo handle, never a table entry. Without "
        "its arm it falls to 'not a live handle' and the refusal reason is lost.",
    },
    {
        "id": "t8-object-197-current-thread-pseudo-handle-allowed",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (source == 0xFFFFFFFFu || source == 0xFFFFFFFEu) {",
        "new": "    if (source == 0xFFFFFFFFu) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "-2 (current thread) is the other pseudo handle and needs its own case, "
        "since the two share one condition and one covered half hides the other.",
    },
    {
        "id": "t8-object-197-dead-handle-status",
        "file": "src/xbox/kernel_object.c",
        "old": '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) -- not a live '
        'handle\\n",\n'
        "                         (unsigned)source);\n"
        "        return STATUS_INVALID_HANDLE;",
        "new": '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) -- not a live '
        'handle\\n",\n'
        "                         (unsigned)source);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a dead source is STATUS_INVALID_HANDLE (INFERRED NT). Reporting it as "
        "'unmodelled' sends the next reader hunting for a missing feature.",
    },
    {
        "id": "t8-object-197-other-kinds-duplicated",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (entry->kind != KERNEL_OBJECT_FILE) {\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a %s handle is not "',
        "new": "    if (entry->kind != KERNEL_OBJECT_FILE && false) {\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a %s handle is not "',
        "targets": ["test_kernel_mutant_dup"],
        "why": "an event or mutant duplicate needs state SHARED between handles, which "
        "this table does not model. Allowing it mints a FILE-kind entry named after "
        "an event, whose signal state is the original's in name only.",
    },
    {
        "id": "t8-object-197-other-kind-refusal-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"shared between the handles\\n",\n'
        "                         (unsigned)source, kind_name(entry->kind));\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": '"shared between the handles\\n",\n'
        "                         (unsigned)source, kind_name(entry->kind));\n"
        "        return STATUS_OBJECT_TYPE_MISMATCH;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a live non-FILE source is refused as NOT_IMPLEMENTED, not as a type "
        "error: the guest's handle is fine, the model is missing.",
    },
    {
        "id": "t8-object-197-duplicate-of-a-duplicate-names-the-duplicate",
        "file": "src/xbox/kernel_object.c",
        "old": "    const uint32_t original = entry->file_dup_of != 0u ? entry->file_dup_of : "
        "entry->handle;",
        "new": "    const uint32_t original = false ? entry->file_dup_of : entry->handle;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "chains must flatten to the root. Naming the middle duplicate works until "
        "the middle one closes, at which point the identity of the third handle "
        "points at a dead entry and its reads and writes fail.",
    },
    {
        "id": "t8-object-197-new-entry-is-not-a-file",
        "file": "src/xbox/kernel_object.c",
        "old": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_FILE,\n"
        "                                                        ORD_NtDuplicateObject);",
        "new": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT,\n"
        "                                                        ORD_NtDuplicateObject);",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the duplicate must itself be a FILE entry or kernel_file's identity "
        "resolution and every file ordinal on it refuse the handle.",
    },
    {
        "id": "t8-object-197-exhaustion-status",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (handle == 0u) {\n"
        "        return KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES;\n"
        "    }\n"
        "    mutable_find(handle)->file_dup_of = original;",
        "new": "    if (handle == 0u) {\n"
        "        return STATUS_INVALID_PARAMETER;\n"
        "    }\n"
        "    mutable_find(handle)->file_dup_of = original;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a full handle table is INSUFFICIENT_RESOURCES, not a bad argument.",
    },
    {
        "id": "t8-object-197-duplicate-not-linked-to-the-original",
        "file": "src/xbox/kernel_object.c",
        "old": "    mutable_find(handle)->file_dup_of = original;",
        "new": "    mutable_find(handle)->file_dup_of = 0u;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "an unlinked duplicate is a fresh file with no open behind it: every "
        "read, write and seek on it fails, though the call itself succeeded.",
    },
    {
        "id": "t8-object-197-failed-copy-out-leaks-the-entry",
        "file": "src/xbox/kernel_object.c",
        "old": "        /* The guest never learns this handle, so nothing will ever close it. */\n"
        "        close_entry_nolock(mutable_find(handle));\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject could not write the handle to %#x, "',
        "new": "        /* The guest never learns this handle, so nothing will ever close it. */\n"
        '        kernel_hle_log()("kernel: NtDuplicateObject could not write the handle to %#x, "',
        "targets": ["test_kernel_mutant_dup"],
        "why": "the handle was never written, so no guest variable names it and nothing "
        "will ever close it. A repeated bad TargetHandle then exhausts the table.",
    },
    {
        "id": "t8-object-197-failed-copy-out-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"duplicate released\\n",\n'
        "                         (unsigned)target_out);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "new": '"duplicate released\\n",\n'
        "                         (unsigned)target_out);\n"
        "        return STATUS_SUCCESS;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "reporting success after an unwritable out-pointer tells the guest it "
        "holds a duplicate whose handle it never received.",
    },
    {
        "id": "t8-object-197-writes-the-original-handle-back",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (!kernel_guest_write_u32(target_out, handle)) {",
        "new": "    if (!kernel_guest_write_u32(target_out, original)) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the guest must receive the NEW handle. Handing back the original makes "
        "the title close its 'duplicate' and with it the original.",
    },
    {
        "id": "t8-object-197-success-status",
        "file": "src/xbox/kernel_object.c",
        "old": '"shared)\\n",\n'
        "                     (unsigned)source, (unsigned)handle, (unsigned)original);\n"
        "    return STATUS_SUCCESS;",
        "new": '"shared)\\n",\n'
        "                     (unsigned)source, (unsigned)handle, (unsigned)original);\n"
        "    return 0x00000103u;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the title reads only the SIGN of the result, so a positive non-zero code "
        "passes its own check. Only an exact-status assertion sees it.",
    },
    # ------------------------------------------------- identity resolution (object side)
    {
        "id": "t8-object-197-identity-ignores-the-duplicate-link",
        "file": "src/xbox/kernel_object.c",
        "old": "    const uint32_t identity = (entry != NULL && entry->file_dup_of != 0u) ? "
        "entry->file_dup_of\n"
        "                                                                            : handle;",
        "new": "    const uint32_t identity = (entry != NULL && entry->file_dup_of != 0u && "
        "false) ? entry->file_dup_of\n"
        "                                                                            : handle;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "every file lookup resolves through this. Returning the handle itself "
        "makes a duplicate a stranger to the open it duplicates.",
    },
    {
        "id": "t8-object-197-identity-live-ignores-the-original",
        "file": "src/xbox/kernel_object.c",
        "old": "    bool live = mutable_find(identity) != NULL;",
        "new": "    bool live = false;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "an open with no duplicate is live because ITS OWN handle is. Without "
        "that arm the slot sweep reclaims every ordinary open file.",
    },
    {
        "id": "t8-object-197-identity-live-ignores-the-duplicates",
        "file": "src/xbox/kernel_object.c",
        "old": "    for (unsigned i = 0u; !live && identity != 0u && i < KERNEL_OBJECT_MAX; i++) {",
        "new": "    for (unsigned i = 0u; !live && identity != 0u && i < KERNEL_OBJECT_MAX && "
        "false; i++) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the open must outlive the original handle while a duplicate is held. "
        "Without the duplicate scan the original's close frees the backing under the "
        "duplicate.",
    },
    {
        "id": "t8-object-197-identity-live-zero-names-every-original",
        "file": "src/xbox/kernel_object.c",
        "old": "    for (unsigned i = 0u; !live && identity != 0u && i < KERNEL_OBJECT_MAX; i++) {",
        "new": "    for (unsigned i = 0u; !live && i < KERNEL_OBJECT_MAX; i++) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "file_dup_of 0 is the 'not a duplicate' marker of every original entry. "
        "Letting identity 0 match it reports a dead handle as live for as long as any "
        "entry exists.",
    },
    {
        "id": "t8-object-197-identity-live-matches-the-wrong-link",
        "file": "src/xbox/kernel_object.c",
        "old": "        live = objects[i].file_dup_of == identity;",
        "new": "        live = objects[i].handle == identity;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the scan must look at the DUPLICATE link. Comparing the entry's own "
        "handle finds nothing once the original is closed.",
    },
    # ---------------------------------------- kernel_file.c lookups resolving duplicates
    {
        "id": "t8-object-197-file-open-info-skips-identity",
        "file": "src/xbox/kernel_file.c",
        "old": "    const uint32_t identity = kernel_object_file_identity(handle);\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            *out = open_files[i].state;",
        "new": "    const uint32_t identity = handle;\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            *out = open_files[i].state;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "NtQueryInformationFile on the duplicate reads the open's state through "
        "kernel_file_open_info. Without identity resolution the duplicate has none.",
    },
    {
        "id": "t8-object-197-file-set-offset-skips-identity",
        "file": "src/xbox/kernel_file.c",
        "old": "    const uint32_t identity = kernel_object_file_identity(handle);\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            open_files[i].state.offset = offset;",
        "new": "    const uint32_t identity = handle;\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            open_files[i].state.offset = offset;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "a seek through the duplicate must move the SHARED position. Skipping "
        "identity makes the seek fail on the duplicate.",
    },
    {
        "id": "t8-object-197-file-read-skips-identity",
        "file": "src/xbox/kernel_file.c",
        "old": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = kernel_object_file_identity(handle);\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        unlock();\n"
        "        return false;\n"
        "    }\n"
        "\n"
        "    if (entry->state.backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "new": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = handle;\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        unlock();\n"
        "        return false;\n"
        "    }\n"
        "\n"
        "    if (entry->state.backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the title READS through its duplicate (XONLINE 0x0042CCE8). Without "
        "identity resolution the read finds no open.",
    },
    {
        "id": "t8-object-197-file-write-skips-identity",
        "file": "src/xbox/kernel_file.c",
        "old": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = kernel_object_file_identity(handle);\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        write_refused_count++;",
        "new": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = handle;\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        write_refused_count++;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the same lookup in writable_entry_locked: a write through the "
        "duplicate must reach the original's backing file.",
    },
    {
        "id": "t8-object-197-file-dir-next-skips-identity",
        "file": "src/xbox/kernel_file.c",
        "old": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = kernel_object_file_identity(handle);\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        *out_status = STATUS_INVALID_HANDLE;",
        "new": "    open_entry *entry = NULL;\n"
        "    const uint32_t identity = handle;\n"
        "    for (unsigned i = 0u; i < KERNEL_FILE_OPEN_MAX; i++) {\n"
        "        if (open_files[i].in_use && open_files[i].state.handle == identity) {\n"
        "            entry = &open_files[i];\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!entry) {\n"
        "        *out_status = STATUS_INVALID_HANDLE;",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the directory enumeration lookup is a fifth, separate copy of the identity "
        "resolution. T8e's first sweep left it alive because only the byte-stream "
        "lookups had a duplicate test.",
    },
    {
        "id": "t8-object-197-file-slot-sweep-ignores-duplicates",
        "file": "src/xbox/kernel_file.c",
        "old": "        if (open_files[i].in_use &&\n"
        "            !kernel_object_file_identity_live(open_files[i].state.handle)) {\n"
        "            /* Through the release helper, not a bare memset: this is where a host-backed",
        "new": "        if (open_files[i].in_use &&\n"
        "            kernel_object_find(open_files[i].state.handle) == NULL) {\n"
        "            /* Through the release helper, not a bare memset: this is where a host-backed",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the slot sweep reclaims a slot whose handle is dead. Asking only about "
        "the original reclaims the open when the original closes first, though a "
        "duplicate is still reading from it.",
    },
    # ------------------------------------------------------------------ T268 gap closure
    {
        "id": "t8-object-197-duplicate-minted-under-another-owner-tag",
        "file": "src/xbox/kernel_object.c",
        "old": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_FILE,\n"
        "                                                        ORD_NtDuplicateObject);\n"
        "    if (handle == 0u) {",
        "new": "    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_FILE,\n"
        "                                                        ORD_NtDuplicateObject + 1u);\n"
        "    if (handle == 0u) {",
        "targets": ["test_kernel_mutant_dup"],
        "why": "the owner tag is the only record of which ordinal minted a FILE handle, so a "
        "live-handle dump attributes the duplicate to the wrong call. Nothing else reads a "
        "FILE entry's tag, so only a direct assertion on the entry sees the swap.",
    },
]
