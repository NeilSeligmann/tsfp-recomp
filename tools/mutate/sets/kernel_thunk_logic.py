# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for the LOGIC in src/host/kernel_thunk.c (T465), not its ABI_TABLE data rows.

OWNED BY THE KERNEL-THUNK-LOGIC TASK. One file per owner, see `_example.py`.

WHAT IS COVERED. The arity resolver `stack_args_for()` (tier order hand, measured, oracle),
the quorum and unanimity gates, the dispatcher's esp unwind, refusal and unimplemented
reporting, the once-per-ordinal oracle announcement, the register hand-off, `patch_table`
and its data-annex redirect (T94), `kernel_thunk_is_va`, window mapping and
`kernel_thunk_measured_as_data`. Every mutation is killed by `test_kernel_thunk_arity`, which
`#include`s the .c and substitutes a SYNTHETIC measured table, so the set needs neither the
lifted tree nor the user's XBE. Do not duplicate `arity_oracle.py`, which mutates the oracle
file and not the thunk.

NO MUTATION ANCHORS ON AN ABI_TABLE ROW. Other sessions edit those constantly and the anchors
would drift. Every anchor below is a logic line.

THE THUNK_CC_CDECL ZERO-POP PATH (T34) HAS NO LOGIC TO MUTATE. The `cc` and `register_args`
fields are never read: a cdecl row is just `stack_args == 0`, so the zero pop is DATA, covered
by the dispatch check on ordinal 8 and by tests/test_arity_oracle.py, and a mutation of it
would have to edit a table row. The only logic it exercises is the esp formula, mutated below.

EQUIVALENT MUTANTS, argued and left out rather than run.

  - `break` to `continue` at the unanimity and quorum refusals. Ordinals are unique in the
    measured table (it is generated one row per ordinal), so after refusing row i the loop
    finds no further match and falls to the oracle exactly as `break` does.
  - `size_t i = 0` loop starts, `static` on file-scope tables, and the `(void)` casts on
    `thunk_trace_thread_id()` and the return-address read: no observable behaviour.
  - Deleting `*src = ...` lines outright would leave `arity_from` in a state the compiler
    flags (-Werror), so those mutations are weakened-but-referenced forms instead.
  - `THUNK_TLS` to a plain global: indistinguishable single-threaded, and the multi-thread
    race it prevents is not reachable from one deterministic suite.
  - Removing the `ordinal > XBOX_KERNEL_ORDINAL_MAX` half of the announcer guard. The announcer
    is only called when the ORACLE answered, and the oracle has no row above 378, so an
    out-of-range ordinal never reaches it. The off-by-one `>=` form IS run, below.
  - `g_oracle_announced` growing by one slot: the array size is not observable.

TWO THINGS NOT REACHED, said plainly.

  1. `kernel_thunk_map_window`'s `got != want` arm needs the kernel to return a different
     address than a MAP_FIXED_NOREPLACE request, which Linux never does.
  2. The `kernel_guest_write_u32` failure `break` in `patch_table` needs a slot that reads
     but refuses a write, which guest memory cannot construct (same gap as xe_section.py).
"""

_TARGET = ["test_kernel_thunk_arity"]

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ tier order
    {
        "id": "thunk-hand-tier-never-matches",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (ABI_TABLE[i].ordinal == ordinal) {\n            *out = ABI_TABLE[i].stack_args;",  # noqa: E501 -- must match the C source exactly
        "new": "        if (ABI_TABLE[i].ordinal == ordinal && false) {\n            *out = ABI_TABLE[i].stack_args;",  # noqa: E501 -- must match the C source exactly
        "targets": _TARGET,
        "why": "THE HAND ROW MUST WIN. It carries reasoning (a forced stack balance, a literal "
        "pinning an argument order) that neither a measurement nor a decoration has. With the "
        "tier dead, ordinal 219 falls to the quorate measured row and pops 6 instead of 8, "
        "which is the plausible-wrong-trace failure the whole table exists to prevent.",
    },
    {
        "id": "thunk-hand-answer-reports-the-wrong-source",
        "file": "src/host/kernel_thunk.c",
        "old": "            *src = ARITY_SOURCE_HAND;",
        "new": "            *src = ARITY_SOURCE_MEASURED;",
        "targets": _TARGET,
        "why": "The number is right and the attribution is wrong. Diagnostics say how "
        "well-founded a pop is, so a hand row reported as a measurement misstates it.",
    },
    {
        "id": "thunk-hand-answer-returns-the-wrong-count",
        "file": "src/host/kernel_thunk.c",
        "old": "            *out = ABI_TABLE[i].stack_args;",
        "new": "            *out = ABI_TABLE[i].stack_args + ABI_TABLE[i].register_args + 1u;",
        "targets": _TARGET,
        "why": "An off-by-something hand answer. Fastcall rows carry register_args and the "
        "stack count EXCLUDES them, so adding them back (plus one) would pop bytes nobody pushed.",
    },
    {
        "id": "thunk-measured-answer-reports-the-oracle-source",
        "file": "src/host/kernel_thunk.c",
        "old": "        *src = ARITY_SOURCE_MEASURED;",
        "new": "        *src = ARITY_SOURCE_ORACLE;",
        "targets": _TARGET,
        "why": "A measured pop attributed to the oracle would also trigger the 'DERIVED FROM "
        "THE nxdk .def ORACLE' announcement for an ordinal that was measured in this image.",
    },
    {
        "id": "thunk-measured-answer-off-by-one",
        "file": "src/host/kernel_thunk.c",
        "old": "        *out = MEASURED_ARITIES[i].stack_args;",
        "new": "        *out = MEASURED_ARITIES[i].stack_args + 1u;",
        "targets": _TARGET,
        "why": "A measured count one dword high over-pops, eating the caller's locals. Row 60 "
        "agrees with the oracle and row 12 does not, so both numbers are pinned.",
    },
    {
        "id": "thunk-measured-row-matched-on-the-wrong-ordinal",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_ARITIES[i].ordinal != ordinal) {\n            continue;\n        }",  # noqa: E501 -- must match the C source exactly
        "new": "        if (MEASURED_ARITIES[i].ordinal == ordinal) {\n            continue;\n        }",  # noqa: E501 -- must match the C source exactly
        "targets": _TARGET,
        "why": "The row filter inverted: every ordinal is answered from the first measured row "
        "that is NOT its own.",
    },
    {
        "id": "thunk-oracle-tier-disabled",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (kernel_arity_oracle_callee_pop(ordinal, &oracle_dwords)) {",
        "new": "    if (kernel_arity_oracle_callee_pop(ordinal, &oracle_dwords) && false) {",
        "targets": _TARGET,
        "why": "Without the third tier every refused measurement becomes HOST_STOP_KERNEL_ABI_"
        "UNKNOWN again, undoing T26's 59 correct continuations.",
    },
    {
        "id": "thunk-oracle-answer-reports-the-measured-source",
        "file": "src/host/kernel_thunk.c",
        "old": "        *src = ARITY_SOURCE_ORACLE;",
        "new": "        *src = ARITY_SOURCE_MEASURED;",
        "targets": _TARGET,
        "why": "An oracle pop presented as measured silences the once-per-ordinal warning "
        "that the number comes from a different kernel build.",
    },
    {
        "id": "thunk-oracle-answer-is-discarded",
        "file": "src/host/kernel_thunk.c",
        "old": "        *out = oracle_dwords;",
        "new": "        *out = oracle_dwords > 0u ? oracle_dwords - 1u : 0u;",
        "targets": _TARGET,
        "why": "The oracle answers and the thunk under-pops by one, leaving bytes on the guest "
        "stack: the permanent esp desync.",
    },
    # ------------------------------------------------------------------ the gates
    {
        "id": "thunk-unanimity-gate-removed",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (!MEASURED_ARITIES[i].unanimous) {",
        "new": "        if (!MEASURED_ARITIES[i].unanimous && false) {",
        "targets": _TARGET,
        "why": "THE BUG T26 FOUND. Six of thirteen verified ordinals were wrong in the measured "
        "table and every one was flagged non-unanimous while the resolver ignored the flag.",
    },
    {
        "id": "thunk-unanimity-gate-inverted",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (!MEASURED_ARITIES[i].unanimous) {",
        "new": "        if (MEASURED_ARITIES[i].unanimous) {",
        "targets": _TARGET,
        "why": "Believes ONLY the disagreeing rows, the exact opposite of the safe direction.",
    },
    {
        "id": "thunk-quorum-gate-removed",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES) {",
        "new": "        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES && false) {",
        "targets": _TARGET,
        "why": "A one-site 'unanimous' row is a tautology. Ordinal 196 would pop TWELVE dwords "
        "from a single call site, more than the export takes, and esp never recovers.",
    },
    {
        "id": "thunk-quorum-boundary-off-by-one-strict",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES) {",
        "new": "        if (MEASURED_ARITIES[i].sites <= MEASURED_ARITY_MIN_SITES) {",
        "targets": _TARGET,
        "why": "The boundary: exactly three voters must be believed. `<=` demands four and "
        "silently refuses every row that has exactly the documented quorum.",
    },
    {
        "id": "thunk-quorum-boundary-off-by-one-lax",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES) {",
        "new": "        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES - 1u) {",
        "targets": _TARGET,
        "why": "The other side of the boundary: two voters admitted, i.e. a pair of correlated "
        "register-indirect sites that the late-bracket undercount makes unreliable.",
    },
    {
        "id": "thunk-quorum-constant-raised",
        "file": "src/host/kernel_thunk.c",
        "old": "#define MEASURED_ARITY_MIN_SITES 3u",
        "new": "#define MEASURED_ARITY_MIN_SITES 4u",
        "targets": _TARGET,
        "why": "The documented value is three, argued at length at the refusal. Four would "
        "refuse correct rows, costing reported stops. The suite pins the constant itself.",
    },
    {
        "id": "thunk-quorum-constant-lowered",
        "file": "src/host/kernel_thunk.c",
        "old": "#define MEASURED_ARITY_MIN_SITES 3u",
        "new": "#define MEASURED_ARITY_MIN_SITES 2u",
        "targets": _TARGET,
        "why": "Two voters are closer to one than to three: the weaker, tempting value.",
    },
    # ------------------------------------------------------------------ dispatch
    {
        "id": "thunk-unwind-forgets-the-return-address",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_esp += 4u + 4u * stack_args;",
        "new": "    g_esp += 4u * stack_args;",
        "targets": _TARGET,
        "why": "The callee pops the return address AND the arguments. Dropping the first leaves "
        "four bytes behind on every kernel call, including the zero-argument and cdecl ones.",
    },
    {
        "id": "thunk-unwind-counts-bytes-as-dwords",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_esp += 4u + 4u * stack_args;",
        "new": "    g_esp += 4u + stack_args;",
        "targets": _TARGET,
        "why": "The byte-versus-dword error at the point of use: correct for zero and cdecl, "
        "wrong for everything else.",
    },
    {
        "id": "thunk-unwind-pops-register-arguments-too",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_esp += 4u + 4u * stack_args;",
        "new": "    g_esp += 4u + 4u * stack_args + (entry ? 4u : 0u);",
        "targets": _TARGET,
        "why": "An extra pop on every call that has a registered entry. Register arguments "
        "are not on the stack and must not be counted.",
    },
    {
        "id": "thunk-result-not-written-to-eax",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_eax = result;",
        "new": "    g_eax = result - result;",
        "targets": _TARGET,
        "why": "The HLE result never reaches the guest: every kernel call returns zero. "
        "Mutated to a weakened-but-referenced form, since a self-assignment trips -Wself-assign.",
    },
    {
        "id": "thunk-result-high-half-always-written",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (frame.has_result_high) {",
        "new": "    if (true) {",
        "targets": _TARGET,
        "why": "edx is clobbered on every call, corrupting the guest's own edx for the 32-bit "
        "handlers. The title consumes EDX:EAX only after 126.",
    },
    {
        "id": "thunk-result-high-half-never-written",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (frame.has_result_high) {",
        "new": "    if (false) {",
        "targets": _TARGET,
        "why": "The 64-bit result of KeQueryPerformanceCounter loses its high half.",
    },
    {
        "id": "thunk-registers-swapped-into-the-frame",
        "file": "src/host/kernel_thunk.c",
        "old": "    kernel_frame_set_registers(&frame, g_ecx, g_edx);",
        "new": "    kernel_frame_set_registers(&frame, g_edx, g_ecx);",
        "targets": _TARGET,
        "why": "Fastcall hands the first argument in ecx. A swap makes KfRaiseIrql raise to the "
        "level that was in edx, invisible to every stdcall handler.",
    },
    {
        "id": "thunk-registers-not-attached",
        "file": "src/host/kernel_thunk.c",
        "old": "    kernel_frame_set_registers(&frame, g_ecx, g_edx);",
        "new": "    kernel_frame_set_registers(&frame, 0u, 0u);",
        "targets": _TARGET,
        "why": "Registers read as zero: a fastcall handler sees a bogus argument.",
    },
    {
        "id": "thunk-trace-result-patched-with-zero",
        "file": "src/host/kernel_thunk.c",
        "old": "    thunk_trace_patch_result(slot, result);",
        "new": "    thunk_trace_patch_result(slot, 0u);",
        "targets": _TARGET,
        "why": "The ordered trace is the deliverable of a bring-up run, and a trace whose "
        "results are all zero reads as a clean run when it was not.",
    },
    {
        "id": "thunk-trace-implemented-flag-lies",
        "file": "src/host/kernel_thunk.c",
        "old": "    const bool implemented = entry && entry->state == KERNEL_ENTRY_IMPLEMENTED;",
        "new": "    const bool implemented = entry != NULL;",
        "targets": _TARGET,
        "why": "Every stub reads as implemented: the stop-on-unimplemented policy never fires "
        "and the run proceeds on a handler that does not exist.",
    },
    {
        "id": "thunk-trace-return-address-lost",
        "file": "src/host/kernel_thunk.c",
        "old": "    slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, ordinal, 0u, return_address,\n                                      implemented);",  # noqa: E501 -- must match the C source exactly
        "new": "    slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, ordinal, 0u, 0u,\n                                      implemented);",  # noqa: E501 -- must match the C source exactly
        "targets": _TARGET,
        "why": "Each trace record loses WHERE in the guest the call came from, which is what "
        "makes a stop report actionable.",
    },
    # ------------------------------------------------------------------ stops and reporting
    {
        "id": "thunk-unanswerable-ordinal-guessed",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (!have_abi) {",
        "new": "    if (!have_abi && false) {",
        "targets": _TARGET,
        "why": "THE REFUSAL ITSELF. Without it a DATA export or unknown ordinal is unwound "
        "with zero arguments, a desynced run that does not crash and keeps lying.",
    },
    {
        "id": "thunk-abi-unknown-stop-reports-the-wrong-reason",
        "file": "src/host/kernel_thunk.c",
        "old": "        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, ordinal,",
        "new": "        host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, return_address, ordinal,",
        "targets": _TARGET,
        "why": "The two stops mean different things to whoever reads the report: one needs a "
        "handler, the other needs an arity.",
    },
    {
        "id": "thunk-abi-unknown-stop-loses-the-return-address",
        "file": "src/host/kernel_thunk.c",
        "old": "        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, ordinal,",
        "new": "        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, 0u, ordinal,",
        "targets": _TARGET,
        "why": "A stop naming an ordinal but not the call site is half a bug report.",
    },
    {
        "id": "thunk-abi-unknown-stop-names-no-ordinal",
        "file": "src/host/kernel_thunk.c",
        "old": "        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, ordinal,",
        "new": "        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, 0u,",
        "targets": _TARGET,
        "why": "The stop record exists to NAME the ordinal.",
    },
    {
        "id": "thunk-unimplemented-stop-ignores-the-policy",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (g_stop_on_missing) {",
        "new": "        if (true) {",
        "targets": _TARGET,
        "why": "--continue-past-unimplemented stops anyway, so the mode that exists to map the "
        "ORDER of the 141 unimplemented calls never gets past the first.",
    },
    {
        "id": "thunk-unimplemented-always-continues",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (g_stop_on_missing) {",
        "new": "        if (false) {",
        "targets": _TARGET,
        "why": "The default bring-up policy (stop at the first unimplemented ordinal) is lost "
        "and the host runs on a stub.",
    },
    {
        "id": "thunk-stop-on-missing-setter-ignores-its-argument",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_stop_on_missing = stop;",
        "new": "    g_stop_on_missing = !stop;",
        "targets": _TARGET,
        "why": "The setter inverts its argument, so --continue-past-unimplemented keeps "
        "stopping and the "
        "default never does. (`= true` would leave `stop` unused and trip -Werror.)",
    },
    {
        "id": "thunk-unimplemented-stop-reports-the-wrong-reason",
        "file": "src/host/kernel_thunk.c",
        "old": "            host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, return_address, ordinal,",
        "new": "            host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, ordinal,",
        "targets": _TARGET,
        "why": "The expected end of a bring-up run reported as an arity failure.",
    },
    {
        "id": "thunk-unimplemented-default-return-dropped",
        "file": "src/host/kernel_thunk.c",
        "old": "        result = entry ? entry->default_return : 0u;",
        "new": "        result = 0u;",
        "targets": _TARGET,
        "why": "A stub's configured default return (an NTSTATUS the title branches on) is "
        "replaced by zero, which reads as STATUS_SUCCESS.",
    },
    # ------------------------------------------------------------------ oracle announcement
    {
        "id": "thunk-oracle-announced-for-every-source",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (arity_from == ARITY_SOURCE_ORACLE) {",
        "new": "    if (arity_from != ARITY_SOURCE_NONE) {",
        "targets": _TARGET,
        "why": "Hand and measured pops announced as derived from a different kernel build, "
        "which buries the one warning that matters.",
    },
    {
        "id": "thunk-oracle-never-announced",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (arity_from == ARITY_SOURCE_ORACLE) {",
        "new": "    if (arity_from == ARITY_SOURCE_NONE) {",
        "targets": _TARGET,
        "why": "An oracle-derived pop runs silently, though its ordinal numbering is an assumption.",  # noqa: E501 -- must match the C source exactly
    },
    {
        "id": "thunk-oracle-announced-on-every-call",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (ordinal > XBOX_KERNEL_ORDINAL_MAX || g_oracle_announced[ordinal]) {",
        "new": "    if (ordinal > XBOX_KERNEL_ORDINAL_MAX) {",
        "targets": _TARGET,
        "why": "The ordered call trace is the deliverable of a bring-up run and must not be "
        "buried under per-call repetition.",
    },
    {
        "id": "thunk-oracle-never-remembers-an-announcement",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_oracle_announced[ordinal] = 1u;",
        "new": "    g_oracle_announced[ordinal] = 0u;",
        "targets": _TARGET,
        "why": "The same repetition by a different route: the flag is written but never holds.",
    },
    {
        "id": "thunk-oracle-announce-bound-excludes-the-last-ordinal",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (ordinal > XBOX_KERNEL_ORDINAL_MAX || g_oracle_announced[ordinal]) {",
        "new": "    if (ordinal >= XBOX_KERNEL_ORDINAL_MAX || g_oracle_announced[ordinal]) {",
        "targets": _TARGET,
        "why": "An off-by-one on the bound: the last ordinal (378) is oracle-answered and "
        "would be silently unannounced.",
    },
    {
        "id": "thunk-oracle-announcement-swaps-its-arguments",
        "file": "src/host/kernel_thunk.c",
        "old": "            ordinal, ordinal_name(ordinal), stack_args);",
        "new": "            stack_args, ordinal_name(ordinal), ordinal);",
        "targets": _TARGET,
        "why": "The warning names the wrong ordinal and the wrong count.",
    },
    # ------------------------------------------------------------------ lookup and window
    {
        "id": "thunk-is-va-lower-bound-exclusive",
        "file": "src/host/kernel_thunk.c",
        "old": "    return va >= KERNEL_THUNK_VA_BASE\n",
        "new": "    return va > KERNEL_THUNK_VA_BASE\n",
        "targets": _TARGET,
        "why": "Ordinal zero's slot stops being a thunk VA.",
    },
    {
        "id": "thunk-is-va-upper-bound-exclusive",
        "file": "src/host/kernel_thunk.c",
        "old": "           && KERNEL_THUNK_ORDINAL(va) <= XBOX_KERNEL_ORDINAL_MAX;",
        "new": "           && KERNEL_THUNK_ORDINAL(va) < XBOX_KERNEL_ORDINAL_MAX;",
        "targets": _TARGET,
        "why": "The last ordinal's import slot resolves to nothing.",
    },
    {
        "id": "thunk-is-va-upper-bound-inclusive-of-the-annex",
        "file": "src/host/kernel_thunk.c",
        "old": "           && KERNEL_THUNK_ORDINAL(va) <= XBOX_KERNEL_ORDINAL_MAX;",
        "new": "           && KERNEL_THUNK_ORDINAL(va) <= XBOX_KERNEL_ORDINAL_MAX + 1u;",
        "targets": _TARGET,
        "why": "The monitor slot above the ordinals is claimed as a kernel thunk.",
    },
    {
        "id": "thunk-lookup-ignores-the-va-check",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (!kernel_thunk_is_va(xbox_va)) {\n        return NULL;\n    }\n    g_pending_ordinal",  # noqa: E501 -- must match the C source exactly
        "new": "    g_pending_ordinal",
        "targets": _TARGET,
        "why": "Every guest address resolves as a kernel call. Mutated by deleting the guard "
        "whole, which compiles because the helper is still used elsewhere.",
    },
    {
        "id": "thunk-lookup-dispatches-the-wrong-ordinal",
        "file": "src/host/kernel_thunk.c",
        "old": "    g_pending_ordinal = KERNEL_THUNK_ORDINAL(xbox_va);",
        "new": "    g_pending_ordinal = KERNEL_THUNK_ORDINAL(xbox_va) + 1u;",
        "targets": _TARGET,
        "why": "The thunk looks up the neighbouring ordinal's ABI entry and handler.",
    },
    {
        "id": "thunk-patch-table-slot-redirect-hardware-info-dropped",
        "file": "src/host/kernel_thunk.c",
        "old": "    case 322u:\n        return KERNEL_THUNK_VA_XBOX_HARDWARE_INFO;",
        "new": "    case 322u:\n        return KERNEL_THUNK_VA(ordinal);",
        "targets": _TARGET,
        "why": "T94: at its own slot the second dword of XboxHardwareInfo overwrites 323 "
        "XboxHDKey.",
    },
    {
        "id": "thunk-patch-table-slot-redirect-krnl-version-dropped",
        "file": "src/host/kernel_thunk.c",
        "old": "    case 324u:\n        return KERNEL_THUNK_VA_XBOX_KRNL_VERSION;",
        "new": "    case 324u:\n        return KERNEL_THUNK_VA(ordinal);",
        "targets": _TARGET,
        "why": "T94: the second dword of XboxKrnlVersion would overwrite 325 XboxSignatureKey.",
    },
    {
        "id": "thunk-patch-table-slot-redirect-wrong-ordinal",
        "file": "src/host/kernel_thunk.c",
        "old": "    case 324u:\n        return KERNEL_THUNK_VA_XBOX_KRNL_VERSION;",
        "new": "    case 325u:\n        return KERNEL_THUNK_VA_XBOX_KRNL_VERSION;",
        "targets": _TARGET,
        "why": "The redirect keyed on a neighbour: 325 (a key) is sent into the annex and 324 "
        "keeps the collision.",
    },
    {
        "id": "thunk-patch-table-default-slot-wrong",
        "file": "src/host/kernel_thunk.c",
        "old": "    default:\n        return KERNEL_THUNK_VA(ordinal);",
        "new": "    default:\n        return KERNEL_THUNK_VA(ordinal + 1u);",
        "targets": _TARGET,
        "why": "Every ordinary import slot points at its neighbour's thunk.",
    },
    {
        "id": "thunk-patch-table-terminator-ignored",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (encoded == 0u) {\n            break;\n        }",
        "new": "        if (encoded == 0u) {\n            left_alone++;\n            continue;\n        }",  # noqa: E501 -- must match the C source exactly
        "targets": _TARGET,
        "why": "The table does not end at its zero terminator, so patching walks into whatever "
        "data follows it.",
    },
    {
        "id": "thunk-patch-table-import-flag-ignored",
        "file": "src/host/kernel_thunk.c",
        "old": "        if ((encoded & 0x80000000u) == 0u) {",
        "new": "        if (false) {",
        "targets": _TARGET,
        "why": "An already-resolved address is overwritten as if it were an ordinal.",
    },
    {
        "id": "thunk-patch-table-ordinal-mask-wrong",
        "file": "src/host/kernel_thunk.c",
        "old": "        unsigned ordinal = (unsigned)(encoded & 0x7FFFFFFFu);",
        "new": "        unsigned ordinal = (unsigned)(encoded & 0xFFFFu);",
        "targets": _TARGET,
        "why": "A mask that keeps bits above the ordinal field mis-reads a corrupted entry "
        "instead of leaving it alone.",
    },
    {
        "id": "thunk-patch-table-out-of-range-ordinal-patched",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (ordinal > XBOX_KERNEL_ORDINAL_MAX) {\n            left_alone++;",
        "new": "        if (ordinal > XBOX_KERNEL_ORDINAL_MAX + 1u) {\n            left_alone++;",
        "targets": _TARGET,
        "why": "One-past-the-last is published as a slot, pointing the import at the monitor slot.",
    },
    {
        "id": "thunk-patch-table-count-not-counted",
        "file": "src/host/kernel_thunk.c",
        "old": "        patched++;",
        "new": "        patched += 2u;",
        "targets": _TARGET,
        "why": "The patched total is the figure the loader reports and checks against.",
    },
    {
        "id": "thunk-patch-table-skipped-not-reported",
        "file": "src/host/kernel_thunk.c",
        "old": "        *skipped = left_alone;",
        "new": "        *skipped = left_alone > 0u ? left_alone - 1u : 0u;",
        "targets": _TARGET,
        "why": "The count of entries the patcher declined to touch is off by one, the figure "
        "the loader "
        "logs. (`= 0u` would leave `left_alone` unused and trip -Werror.)",
    },
    {
        "id": "thunk-patch-table-walks-past-count",
        "file": "src/host/kernel_thunk.c",
        "old": "    for (size_t i = 0; i < count; i++) {\n        kernel_guest_ptr slot",
        "new": "    for (size_t i = 0; i <= count; i++) {\n        kernel_guest_ptr slot",
        "targets": _TARGET,
        "why": "One extra entry patched beyond the caller's bound.",
    },
    {
        "id": "thunk-window-remap-not-idempotent",
        "file": "src/host/kernel_thunk.c",
        "old": "    if (g_window) {\n        return true;\n    }\n    void *want",
        "new": "    void *want",
        "targets": _TARGET,
        "why": "A second map attempt collides with the first (MAP_FIXED_NOREPLACE) and reports "
        "failure for a window that is perfectly mapped.",
    },
    {
        "id": "thunk-window-unmap-leaves-a-stale-pointer",
        "file": "src/host/kernel_thunk.c",
        "old": "        g_window = NULL;",
        "new": "        g_window = (void *)(uintptr_t)KERNEL_THUNK_VA_BASE;",
        "targets": _TARGET,
        "why": "After an unmap the next map believes the window is still there and returns true "
        "with nothing mapped, a later fault on the first thunk read.",
    },
    {
        "id": "thunk-window-unmap-never-unmaps",
        "file": "src/host/kernel_thunk.c",
        "old": "        (void)munmap(g_window, KERNEL_THUNK_WINDOW_BYTES);\n        g_window = NULL;",  # noqa: E501 -- must match the C source exactly
        "new": "        g_window = NULL;",
        "targets": _TARGET,
        "why": "The mapping leaks, so the next map collides with it and the reset test would "
        "see old contents.",
    },
    {
        "id": "thunk-measured-as-data-always-false",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_DATA_ORDINALS[i] == ordinal) {",
        "new": "        if (MEASURED_DATA_ORDINALS[i] == ordinal && false) {",
        "targets": _TARGET,
        "why": "A stop at an ordinal the guest only ever reads loses its 'this is a variable' "
        "explanation.",
    },
    {
        "id": "thunk-measured-as-data-matches-everything",
        "file": "src/host/kernel_thunk.c",
        "old": "        if (MEASURED_DATA_ORDINALS[i] == ordinal) {",
        "new": "        if (MEASURED_DATA_ORDINALS[i] != ordinal) {",
        "targets": _TARGET,
        "why": "Every function is reported as a variable.",
    },
]
