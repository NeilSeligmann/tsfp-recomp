# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the whole-run call and frame profile (T422): src/host/call_profile.c, its hooks
in src/host/thunk_trace.c, src/gpu/d3d8_frame_profile.c and the option parsing in
src/host/host_options.c.

WHAT THIS SET IS FOR. The profile is observation only, so a wrong number never changes a boot, it
changes the conclusion drawn from one. The failures that matter are a count that drifts (a row keyed
without its caller, an off-by-one in the cut), a thread reported as between calls while it is inside
a wait, a frame digest that hides or invents a change, and a diff that compares frames of different
length. Each mutation introduces one of those and `why` says what a survivor would let through.

NOT REACHABLE: `main.c` wiring (what is enabled, the report call order) and the refusal of
`--profile-calls` while the Swap replay is on live in tsfp_host or need a Vulkan device, so no ctest
suite can kill a mutation there. `tests/test_steady_state_boot.py` boots the real binary instead.

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror`
turns the latter into NOT-A-MUTANT, which reads like evidence.
"""

PROFILE = "src/host/call_profile.c"
TRACE = "src/host/thunk_trace.c"
FRAME = "src/gpu/d3d8_frame_profile.c"
PRESENT = "src/gpu/d3d8_present.c"
OPTIONS = "src/host/host_options.c"

T_PROFILE = ["test_call_profile"]
T_FRAME = ["test_d3d8_frame_profile"]
T_OPTIONS = ["test_host_options"]
T_PRESENT = ["test_d3d8_present"]

MUTATIONS: list[dict] = [
    {
        "id": "profile-row-count-never-rises",
        "file": PROFILE,
        "old": "            slot->row.count++;\n            return;",
        "new": "            return;",
        "targets": T_PROFILE,
        "why": "every address would read as dispatched once however often it ran, and a 350k dispatch loop would look like a quiet one.",
    },
    {
        "id": "profile-row-ignores-the-caller",
        "file": PROFILE,
        "old": "if (slot->row.kind == kind && slot->row.id == id && slot->row.caller == caller) {",
        "new": "if (slot->row.kind == kind && slot->row.id == id) {",
        "targets": T_PROFILE,
        "why": "the callers of an address would merge into whichever came first, so 'which call site loops' could not be answered.",
    },
    {
        "id": "profile-row-ignores-the-kind",
        "file": PROFILE,
        "old": "if (slot->row.kind == kind && slot->row.id == id && slot->row.caller == caller) {",
        "new": "if (slot->row.id == id && slot->row.caller == caller) {",
        "targets": T_PROFILE,
        "why": "ordinal 21 and XDK address 21 share a number, a merged row would credit one with the other's calls.",
    },
    {
        "id": "profile-disabled-still-counts",
        "file": PROFILE,
        "old": "    if (!profile_on) {\n        pthread_mutex_unlock(&profile_lock);\n        return;\n    }\n    if (limit_hit) {",
        "new": "    if (!profile_on && false) {\n        pthread_mutex_unlock(&profile_lock);\n        return;\n    }\n    if (limit_hit) {",
        "targets": T_PROFILE,
        "why": "the default run would pay for and report a profile nobody asked for, and a limit could stop a normal boot.",
    },
    {
        "id": "profile-limit-off-by-one",
        "file": PROFILE,
        "old": "        if (limit_seen >= limit_count) {",
        "new": "        if (limit_seen > limit_count) {",
        "targets": T_PROFILE,
        "why": "the cut would run one dispatch too many, so two boots cut at 'N swaps' would not be the same N.",
    },
    {
        "id": "profile-limit-never-counts",
        "file": PROFILE,
        "old": "        if (limit_set && kind == limit_kind && id == limit_id) {\n            limit_seen++;\n        }",
        "new": "",
        "targets": T_PROFILE,
        "why": "the run would never be cut by guest progress and fall back to the watchdog's wall clock, which is the non-determinism the cut exists to remove.",
    },
    {
        "id": "profile-limit-matches-any-kind",
        "file": PROFILE,
        "old": "    } else if (limit_set && kind == limit_kind && id == limit_id) {\n        if (limit_seen >= limit_count) {",
        "new": "    } else if (limit_set && id == limit_id) {\n        if (limit_seen >= limit_count) {",
        "targets": T_PROFILE,
        "why": "an ordinal cut would be tripped by an XDK address with the same number, ending the run at the wrong place.",
    },
    {
        "id": "profile-limit-is-not-sticky",
        "file": PROFILE,
        "old": "    if (limit_hit) {\n        stop = true;\n    } else if",
        "new": "    if (limit_hit && false) {\n        stop = true;\n    } else if",
        "targets": T_PROFILE,
        "why": "only the thread that tripped the limit would stop, so a second guest thread would run on and the cut would not end the run.",
    },
    {
        "id": "profile-stop-reason-is-not-budget",
        "file": PROFILE,
        "old": "        host_run_stop(HOST_STOP_BUDGET, id,",
        "new": "        host_run_stop(HOST_STOP_FAULT, id,",
        "targets": T_PROFILE,
        "why": "the cut would read as a host fault in the report and the verdict.",
    },
    {
        "id": "profile-stop-loses-the-ordinal",
        "file": PROFILE,
        "old": "kind == THUNK_KIND_ORDINAL ? (unsigned)id : 0u,",
        "new": "0u,",
        "targets": T_PROFILE,
        "why": "an ordinal cut would stop without naming the ordinal.",
    },
    {
        "id": "profile-ring-oldest-first-lost",
        "file": PROFILE,
        "old": "            view->recent[index] = slot->view.recent[(slot->head - held + index) % CALL_PROFILE_RING];",
        "new": "            view->recent[index] = slot->view.recent[index];",
        "targets": T_PROFILE,
        "why": "after the ring wraps the last-dispatches list would be rotated, so the newest call would not be last and the order of the loop would read wrong.",
    },
    {
        "id": "profile-dispatch-never-marks-in-flight",
        "file": PROFILE,
        "old": "            slot->view.current = *event;\n            slot->view.in_flight = slot->depth != 0u;\n            touch(slot);",
        "new": "            slot->view.current = *event;\n            slot->view.in_flight = false;\n            touch(slot);",
        "targets": T_PROFILE,
        "why": "a thread blocked inside a wait would be reported as between calls, hiding exactly the answer 'which wait is it in'.",
    },
    {
        "id": "profile-return-never-closes-the-call",
        "file": PROFILE,
        "old": "            if (slot->depth != 0u) {\n                slot->depth--;",
        "new": "            if (slot->depth != 0u && false) {\n                slot->depth--;",
        "targets": T_PROFILE,
        "why": "every thread would look permanently inside its first call.",
    },
    {
        "id": "profile-return-closes-the-wrong-call",
        "file": PROFILE,
        "old": "                    const uint64_t position = slot->open[slot->depth];\n                    struct timespec finished;",
        "new": "                    const uint64_t position = slot->head - 1u;\n                    struct timespec finished;",
        "targets": T_PROFILE,
        "why": "a nested call returning would be recorded against the last dispatch instead of its own, so the outer call would show a result it never returned.",
    },
    {
        "id": "profile-nested-open-position-forgotten",
        "file": PROFILE,
        "old": "                    slot->open[slot->depth] = slot->head;",
        "new": "                    slot->open[slot->depth] = 0u;",
        "targets": T_PROFILE,
        "why": "the ring entry a return patches would be a stale one.",
    },
    {
        "id": "profile-full-table-not-counted",
        "file": PROFILE,
        "old": "            if (table_used >= CALL_PROFILE_ROWS - 1u) {\n                dropped++;\n                return;",
        "new": "            if (table_used >= CALL_PROFILE_ROWS - 1u) {\n                return;",
        "targets": T_PROFILE,
        "why": "a full table would lose rows without saying so, and the totals would read as complete.",
    },
    {
        "id": "profile-table-never-reports-full",
        "file": PROFILE,
        "old": "            if (table_used >= CALL_PROFILE_ROWS - 1u) {",
        "new": "            if (table_used >= CALL_PROFILE_ROWS) {",
        "targets": T_PROFILE,
        "why": "the table would fill completely and a probe for an absent row would run the whole table before giving up.",
    },
    {
        "id": "profile-scrape-rejects-call-rel32",
        "file": PROFILE,
        "old": "    if (bytes[3] == 0xE8u) {\n        return true;\n    }",
        "new": "    if (bytes[3] == 0xE8u && false) {\n        return true;\n    }",
        "targets": T_PROFILE,
        "why": "the stack scrape would miss every direct call, the commonest return address.",
    },
    {
        "id": "profile-scrape-accepts-any-ff-opcode",
        "file": PROFILE,
        "old": "((bytes[9u - length] >> 3) & 7u) == 2u) {",
        "new": "((bytes[9u - length] >> 3) & 7u) <= 7u) {",
        "targets": T_PROFILE,
        "why": "FF /0 (inc) and FF /6 (push) would count as calls, filling the chain with data that is not a return address.",
    },
    {
        "id": "profile-report-in-flight-label-swapped",
        "file": PROFILE,
        "old": 'view->in_flight ? "INSIDE a call that has not returned" : "between calls",',
        "new": 'view->in_flight ? "between calls" : "INSIDE a call that has not returned",',
        "targets": T_PROFILE,
        "why": "the report would say a spinning thread is blocked and a blocked one is spinning.",
    },
    {
        "id": "trace-hook-dispatch-dropped",
        "file": TRACE,
        "old": "    call_profile_note_dispatch(kind, ordinal, address, return_address, result_known, result);\n",
        "new": "",
        "targets": T_PROFILE,
        "why": "the profile would be wired to nothing and count zero for every real boot.",
    },
    {
        "id": "trace-hook-return-dropped",
        "file": TRACE,
        "old": "    call_profile_note_return(result);\n",
        "new": "",
        "targets": T_PROFILE,
        "why": "a thread would never leave its first call in the profile.",
    },
    {
        "id": "frame-profile-mask-ignores-program-slots",
        "file": FRAME,
        "old": "        if (command.method == METHOD_PROGRAM_LOAD || command.method == METHOD_PROGRAM_START) {\n            masked = 0u;",
        "new": "        if (command.method == METHOD_PROGRAM_LOAD || command.method == METHOD_PROGRAM_START) {\n            masked = command.data;",
        "targets": T_FRAME,
        "why": "ring cursor movement would count as a new frame, so a static screen would read as 11 thousand different frames.",
    },
    {
        "id": "frame-profile-mask-ignores-draw-start-index",
        "file": FRAME,
        "old": "            masked = command.data & 0xFF000000u;",
        "new": "            masked = command.data;",
        "targets": T_FRAME,
        "why": "the draw start index advances like a ring cursor, so it would make every frame distinct.",
    },
    {
        "id": "frame-profile-mask-hides-the-draw-count",
        "file": FRAME,
        "old": "            masked = command.data & 0xFF000000u;",
        "new": "            masked = 0u;",
        "targets": T_FRAME,
        "why": "a frame whose draw changed its vertex count would hash the same as before.",
    },
    {
        "id": "frame-profile-vertex-bytes-not-hashed",
        "file": FRAME,
        "old": "                for (uint32_t offset = 0u; bytes != NULL && offset < length; offset++) {",
        "new": "                for (uint32_t offset = 0u; bytes != NULL && offset < length && false; offset++) {",
        "targets": T_FRAME,
        "why": "an animation that only moves vertex data would read as a frozen frame.",
    },
    {
        "id": "frame-profile-draws-read-as-uncaptured",
        "file": FRAME,
        "old": "            if (!info->vertices_captured) {",
        "new": "            if (info->vertices_captured) {",
        "targets": T_FRAME,
        "why": "captured vertex data would be skipped and counted as missing.",
    },
    {
        "id": "frame-profile-previous-frame-compare-inverted",
        "file": FRAME,
        "old": "        if (full == previous_digest) {",
        "new": "        if (full != previous_digest) {",
        "targets": T_FRAME,
        "why": "a repeated frame would be counted as new and a new one as a repeat.",
    },
    {
        "id": "frame-profile-content-change-not-recorded",
        "file": FRAME,
        "old": "            summary.content_last_change = row.number;\n            if (summary.content_changes",
        "new": "            if (summary.content_changes",
        "targets": T_FRAME,
        "why": "the frame at which the screen last changed is the evidence that a loop has stopped progressing.",
    },
    {
        "id": "frame-profile-new-content-frame-not-recorded",
        "file": FRAME,
        "old": "        summary.content_last_new = row.number;",
        "new": "        summary.content_last_new = 0u;",
        "targets": T_FRAME,
        "why": "'no new content after frame N' is the periodicity claim, it would read as never.",
    },
    {
        "id": "frame-profile-diff-compares-unequal-lengths",
        "file": FRAME,
        "old": "    if (!kept_valid || kept_count != count) {",
        "new": "    if (!kept_valid) {",
        "targets": T_FRAME,
        "why": "frames of different length would be compared command by command against stale data and report differences that are not.",
    },
    {
        "id": "frame-profile-diff-counts-methods-per-command",
        "file": FRAME,
        "old": "        if (listed) {\n            continue;\n        }",
        "new": "        if (listed && false) {\n            continue;\n        }",
        "targets": T_FRAME,
        "why": "a method with several differing commands in one frame pair would be counted several times, overstating how often it changes.",
    },
    {
        "id": "frame-profile-frame-not-released",
        "file": FRAME,
        "old": "    d3d8_gpu_stream_discard(count);\n    decoded_next",
        "new": "    decoded_next",
        "targets": T_FRAME,
        "why": "the bounded recording would fill after 65536 commands and every later frame would be dropped from the digests.",
    },
    {
        "id": "frame-profile-swap-step-compare-wrong",
        "file": FRAME,
        "old": "        if (row.swap_counter == previous_row.swap_counter + 1u) {",
        "new": "        if (row.swap_counter > previous_row.swap_counter) {",
        "targets": T_FRAME,
        "why": "a swap counter that skipped a value would read as strictly +1.",
    },
    {
        "id": "frame-profile-vblank-stall-counted-as-step",
        "file": FRAME,
        "old": "        } else if (row.vblank == previous_row.vblank) {\n            summary.vblank_step_zero++;",
        "new": "        } else if (row.vblank == previous_row.vblank && false) {\n            summary.vblank_step_zero++;",
        "targets": T_FRAME,
        "why": "presents that did not advance the vblank would be lumped with a fall.",
    },
    {
        "id": "frame-profile-watch-change-compare-inverted",
        "file": FRAME,
        "old": "        } else if (value != watch->last) {",
        "new": "        } else if (value == watch->last) {",
        "targets": T_FRAME,
        "why": "'the counter the title polls never moves' is a headline claim, it would read as moving every present.",
    },
    {
        "id": "frame-profile-watch-bound-off-by-one",
        "file": FRAME,
        "old": "    const bool ok = watch_address_count < D3D8_FRAME_PROFILE_WATCHES;",
        "new": "    const bool ok = watch_address_count <= D3D8_FRAME_PROFILE_WATCHES;",
        "targets": T_FRAME,
        "why": "the ninth watch would write past the table.",
    },
    {
        "id": "options-stop-after-count-zero-allowed",
        "file": OPTIONS,
        "old": "*end_count != '\\0' || count == 0u) {",
        "new": "*end_count != '\\0') {",
        "targets": T_OPTIONS,
        "why": "a cut at zero calls would stop the run before the guest does anything and read as a hang.",
    },
    {
        "id": "options-stop-after-without-profile-allowed",
        "file": OPTIONS,
        "old": "        (!out->stop_after_set || out->profile_calls) &&",
        "new": "        (!out->stop_after_set || out->profile_calls || true) &&",
        "targets": T_OPTIONS,
        "why": "the cut counts through the profile, so without it the cut would silently never fire.",
    },
    {
        "id": "options-watch-without-profile-allowed",
        "file": OPTIONS,
        "old": "        (out->profile_watch_count == 0u || out->profile_calls) &&",
        "new": "        (out->profile_watch_count == 0u || out->profile_calls || true) &&",
        "targets": T_OPTIONS,
        "why": "a watch would be accepted and never sampled.",
    },
    {
        "id": "options-watch-list-unbounded",
        "file": OPTIONS,
        "old": "            if (++i >= argc || out->profile_watch_count >= 8u) {",
        "new": "            if (++i >= argc || out->profile_watch_count >= 9u) {",
        "targets": T_OPTIONS,
        "why": "the ninth --profile-watch would write past the option array.",
    },
    {
        "id": "options-ordinal-prefix-ignored",
        "file": OPTIONS,
        "old": "                ordinal = true;\n                text += 4;",
        "new": "                ordinal = false;\n                text += 4;",
        "targets": T_OPTIONS,
        "why": "an ordinal cut would be armed against an XDK address of the same number.",
    },
    {
        "id": "present-observer-never-called",
        "file": PRESENT,
        "old": "    if (present_observer != NULL) {\n        present_observer(&record);\n    }",
        "new": "    if (present_observer != NULL && false) {\n        present_observer(&record);\n    }",
        "targets": T_PRESENT,
        "why": "the frame profile would be installed and never told about a present, so every boot would report no frames.",
    },
    {
        "id": "present-observer-gets-the-wrong-record",
        "file": PRESENT,
        "old": "        present_observer(&record);",
        "new": "        present_observer(&queue[0]);",
        "targets": T_PRESENT,
        "why": "the profile would digest and number frames from the oldest queue slot instead of the one just presented.",
    },
]
