"""Mutations for the T592 loading bar worker blanks (`src/host/recomp_second_vblank.c`, `recomp_vblank_quiescence.c`).

Each mutation compiles and changes a decision the boot would only show as a different count of
blanks or a hang: a worker that is not held until the owner is parked, a gate that is entered
without its two entry conditions, an owner that may read the counter while the worker's blanks are
delivered, a budget that does not bound. The ctest binary `test_recomp_second_vblank_owner_waits`
runs real guest threads for the owner and the worker. `tsfp_host` is not a ctest binary, so the
announcement, the report line and the schedule line in `main.c` are covered by
`tests/test_vblank_worker_blanks.py` (a boot, skipped without the disc).

NOT MUTATED, equivalent or unobservable by construction: the `record.started` and `!record.finished`
terms of `worker_identity` (a thread that has not started or has finished cannot complete a wait),
`handle != state.producer_handle` in default/evidence delivery (the credited producer is terminated
before that model arms; interactive terminal-producer admission is separately mutated by
`vblank_startup_worker.py`), `!pthread_equal(owner,pthread_self())` in the worker
arm (the owner arm comes first in the chain and the worker identity excludes the owner thread), and
the hold limit arithmetic (a timing constant, bounded by the named stop tests), `handle != 0u` in
`worker_epoch_active` (the thread table has no record for handle 0), the `handle != deliverer` term of the
parked list (the deliverer is skipped by every later loop), and the count bound of `in_handles` (a read
past a short list is garbage, not a defect a unit test can pin).
"""

# The anchors are exact source lines, some longer than the line limit.
# ruff: noqa: E501
SECOND = "src/host/recomp_second_vblank.c"
QUIESCENCE = "src/host/recomp_vblank_quiescence.c"
OPTIONS = "src/host/host_options.c"
T_C = ["test_recomp_second_vblank_owner_waits"]
T_Q = ["test_recomp_vblank_quiescence"]
T_O = ["test_host_options"]


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, targets: list[str] | None = None
) -> dict:
    return {
        "id": f"t592-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets if targets is not None else T_C,
        "why": why,
    }


def mutation_t604(identifier: str, old: str, new: str, why: str) -> dict:
    return {**mutation(identifier, SECOND, old, new, why), "id": f"t604-{identifier}"}


MUTATIONS: list[dict] = [
    # --- the budget and the arming ---------------------------------------------------------
    mutation(
        "budget-off-by-one",
        SECOND,
        "state.worker_delivered >= atomic_load(&worker_budget)",
        "state.worker_delivered > atomic_load(&worker_budget)",
        "one more blank than the operator budgeted reaches the worker.",
    ),
    mutation(
        "arm-ignores-second-event",
        SECOND,
        "owner_budget != 0u &&\n             state.second_delivered && !state.refused && !pthread_equal(owner,pthread_self()) &&",
        "owner_budget != 0u &&\n             !state.refused && !pthread_equal(owner,pthread_self()) &&",
        "a worker's wait before the credited second event is taken for a blank instead of the producer's credit.",
    ),
    mutation(
        "arm-ignores-refused",
        SECOND,
        "state.second_delivered && !state.refused && !pthread_equal(owner,pthread_self()) &&",
        "state.second_delivered && !pthread_equal(owner,pthread_self()) &&",
        "a refused policy keeps delivering to the worker.",
    ),
    mutation(
        "arm-ignores-owner-budget",
        SECOND,
        "state.first_delivered && owner_budget != 0u &&\n             state.second_delivered",
        "state.first_delivered &&\n             state.second_delivered",
        "the worker blanks run without the owner waits they are built on.",
    ),
    mutation(
        "budget-api-ignores-owner-budget",
        SECOND,
        "(budget == 0u || (state.enabled && owner_budget != 0u));",
        "(budget == 0u || state.enabled);",
        "the budget is accepted without the owner waits.",
    ),
    mutation(
        "budget-api-accepts-over-cap",
        SECOND,
        "budget <= RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX &&",
        "budget <= RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX + 1u &&",
        "the documented cap moves.",
    ),
    mutation(
        "budget-api-accepts-inflight",
        SECOND,
        "const bool accepted = state.inflight == false && state.epoch != UINT64_MAX &&\n                          budget <= RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX &&",
        "const bool accepted = state.epoch != UINT64_MAX &&\n                          budget <= RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX &&",
        "the budget changes under a running callback.",
    ),
    # --- who the worker is -----------------------------------------------------------------
    mutation(
        "identity-ignores-start-routine",
        SECOND,
        "record.in_use && record.started && !record.finished && record.start_routine == WORKER_START &&\n           record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;",
        "record.in_use && record.started && !record.finished &&\n           record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;",
        "any thread with the shim as its system routine is taken for the loading bar worker.",
    ),
    mutation(
        "identity-ignores-system-routine",
        SECOND,
        "record.start_routine == WORKER_START &&\n           record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;",
        "record.start_routine == WORKER_START &&\n           record.start_context == 0u && record.control_base == fs;",
        "a thread with the worker's start routine but another entry is the worker.",
    ),
    mutation(
        "identity-ignores-start-context",
        SECOND,
        "record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;",
        "record.system_routine == WORKER_SYSTEM && record.control_base == fs;",
        "a worker started with another context is the measured worker.",
    ),
    mutation(
        "identity-ignores-control-block",
        SECOND,
        "record.system_routine == WORKER_SYSTEM && record.start_context == 0u && record.control_base == fs;",
        "record.system_routine == WORKER_SYSTEM && record.start_context == 0u;",
        "a wait that presents another control block is taken for the worker's.",
    ),
    # --- the hold and the quiescence verdict -----------------------------------------------
    mutation(
        "hold-skips-quiescence",
        SECOND,
        "if (verdict.holds && (owner_blocked_on(handle) || gate_prologue_visible())) break;",
        "if (gate_prologue_visible()) break;",
        "the worker is released with the owner elsewhere, so the owner can sample the counter.",
    ),
    mutation(
        "hold-skips-prologue",
        SECOND,
        "if (verdict.holds && (owner_blocked_on(handle) || gate_prologue_visible())) break;",
        "if (verdict.holds && (owner_blocked_on(handle) || gate_prologue_visible() || true)) break;",
        "the worker steps before the owner's gate stores landed, so the step count depends on host timing.",
    ),
    mutation(
        "hold-ignores-blocked-owner",
        SECOND,
        "if (verdict.holds && (owner_blocked_on(handle) || gate_prologue_visible())) break;",
        "if (verdict.holds && gate_prologue_visible()) break;",
        "an owner already blocked on the worker is still waited for.",
    ),
    mutation(
        "hold-prologue-checks-stage-only",
        SECOND,
        "kernel_guest_read_u32(PROGRESS_ONE,&one) && stage == 0xAu && target == one;",
        "kernel_guest_read_u32(PROGRESS_ONE,&one) && stage == 0xAu;",
        "the worker is released once the stage is stored, before the target float.",
    ),
    mutation(
        "hold-prologue-checks-target-only",
        SECOND,
        "kernel_guest_read_u32(PROGRESS_ONE,&one) && stage == 0xAu && target == one;",
        "kernel_guest_read_u32(PROGRESS_ONE,&one) && target == one;",
        "the worker is released with the stage the previous phase left.",
    ),
    mutation(
        "location-parks-nothing",
        SECOND,
        "return atomic_load(&owner_location) != OWNER_ELSEWHERE;",
        "return false;",
        "the owner is never counted parked, no blank is ever delivered.",
    ),
    mutation(
        "location-parks-everything",
        SECOND,
        "return atomic_load(&owner_location) != OWNER_ELSEWHERE;",
        "return true;",
        "the owner is always counted parked, even in the middle of loading.",
    ),
    mutation(
        "delivery-keeps-no-inflight",
        SECOND,
        "if (!busy) { state.inflight = true; atomic_store(&worker_id,handle); }",
        "if (!busy) { atomic_store(&worker_id,handle); }",
        "a wait during the worker's callback is not refused.",
    ),
    mutation(
        "delivery-forgets-the-worker",
        SECOND,
        "if (!busy) { state.inflight = true; atomic_store(&worker_id,handle); }",
        "if (!busy) { state.inflight = true; }",
        "the epoch never starts, the owner may read the counter while blanks are delivered.",
    ),
    mutation(
        "delivery-counts-only-owner-blanks",
        SECOND,
        "const uint32_t done = state.owner_delivered + state.worker_delivered;",
        "const uint32_t done = state.owner_delivered;",
        "the title counter the delivery expects ignores the worker's blanks.",
    ),
    mutation(
        "delivery-skips-held-owner-parking",
        SECOND,
        "(void)recomp_vblank_quiescence_check_parked(handle,0x3D3550u,&held,held != 0u ? 1u : 0u);",
        "(void)recomp_vblank_quiescence_check_parked(handle,0x3D3550u,&held,0u);",
        "the owner's own blank refuses while the worker idles in its hold, which is by construction.",
    ),
    # --- the gate and the final hold -------------------------------------------------------
    mutation(
        "gate-compare-is-not-strict",
        SECOND,
        "return one > progress;",
        "return one >= progress;",
        "a bar at exactly 1.0 still counts as unfilled, the owner has left and the final hold is skipped.",
    ),
    mutation(
        "gate-compare-reversed",
        SECOND,
        "return one > progress;",
        "return one < progress;",
        "the final hold is taken for every blank but the last.",
    ),
    mutation(
        "final-hold-skipped",
        SECOND,
        "if (worker && !gate_closed()) worker_hold_final(handle);",
        "if (worker && false && !gate_closed()) worker_hold_final(handle);",
        "the worker's stop test races the owner's exit and the blank count depends on host timing.",
    ),
    mutation(
        "final-hold-always",
        SECOND,
        "if (worker && !gate_closed()) worker_hold_final(handle);",
        "if (worker && (!gate_closed() || true)) worker_hold_final(handle);",
        "the worker waits for an owner that has not left the gate.",
    ),
    mutation(
        "final-hold-ignores-the-target",
        SECOND,
        "record.block_state == KERNEL_THREAD_BLOCK_THREAD &&\n            record.block_target == handle) break;",
        "record.block_state == KERNEL_THREAD_BLOCK_THREAD) break;",
        "an owner blocked on some other thread ends the final hold.",
    ),
    # --- the owner's position --------------------------------------------------------------
    mutation(
        "gate-entry-ignores-the-flag",
        SECOND,
        "const bool entered = kernel_guest_read_u32(GATE_FLAG,&flag) && flag == 0u &&",
        "const bool entered = kernel_guest_read_u32(GATE_FLAG,&flag) &&",
        "a call of the gate that returns at once is taken for the spin.",
    ),
    mutation(
        "gate-entry-ignores-running",
        SECOND,
        "kernel_guest_read_u32(GATE_RUNNING,&running) && running != 0u;",
        "kernel_guest_read_u32(GATE_RUNNING,&running);",
        "a gate whose stop function returns early (the bar is not running) is taken for the spin.",
    ),
    mutation(
        "gate-wait-is-any-call",
        SECOND,
        "if (callee == GATE_WAIT) { atomic_store(&owner_location,OWNER_GATE_WAIT); return; }",
        "{ atomic_store(&owner_location,OWNER_GATE_WAIT); return; }",
        "any call out of the gate keeps the owner counted parked.",
    ),
    mutation(
        "gate-exit-call-is-not-a-stop",
        SECOND,
        'if (worker_epoch_active()) refuse("owner left the loading bar gate through a call outside the proof (T592)");',
        'if (worker_epoch_active() && false) refuse("owner left the loading bar gate through a call outside the proof (T592)");',
        "a call outside the proved path goes unnoticed.",
    ),
    mutation(
        "getter-by-owner-is-not-a-stop",
        SECOND,
        "if (callee == GETTER && worker_epoch_active())",
        "if (callee == GETTER && worker_epoch_active() && false)",
        "the owner samples the counter while the worker's blanks are delivered and nothing says so.",
    ),
    mutation(
        "epoch-ignores-the-worker-ending",
        SECOND,
        "return handle != 0u && kernel_thread_get(handle,&record) && !record.terminated;",
        "return handle != 0u && kernel_thread_get(handle,&record);",
        "the owner may never read the counter again once a worker blank was delivered.",
    ),
    # --- the worker's own getter poll --------------------------------------------------------
    mutation(
        "poll-hold-skipped",
        SECOND,
        "if (armed && worker_record_identity(handle)) {\n            worker_hold(handle);",
        "if (armed && worker_record_identity(handle) && false) {\n            worker_hold(handle);",
        "the worker steps at its first poll, before the owner reached the gate.",
    ),
    mutation(
        "poll-hold-ignores-second-event",
        SECOND,
        "const bool armed = state.second_delivered && !state.refused && handle != state.producer_handle;",
        "const bool armed = !state.refused && handle != state.producer_handle;",
        "the producer's own polls before the second event are held.",
    ),
    mutation(
        "poll-hold-ignores-refused",
        SECOND,
        "const bool armed = state.second_delivered && !state.refused && handle != state.producer_handle;",
        "const bool armed = state.second_delivered && handle != state.producer_handle;",
        "a refused policy still holds the worker.",
    ),
    mutation(
        "poll-after-block-is-allowed",
        SECOND,
        "if (owner_blocked_on(handle))\n                refuse(",
        "if (owner_blocked_on(handle) && false)\n                refuse(",
        "a worker polling after the owner blocked on it spins for a blank nobody delivers.",
    ),
    # --- the parked list of the predicate --------------------------------------------------
    mutation(
        "parked-list-parks-any-state",
        QUIESCENCE,
        "recomp_vblank_quiescence_classify(&records[i]) == RVQ_RUNNABLE;",
        "true;",
        "a thread on a host timer or ended by a host stop is parked by being listed.",
        T_Q,
    ),
    mutation(
        "parked-list-parks-everyone",
        QUIESCENCE,
        "in_handles(parked_handles, parked_count, records[i].handle) &&",
        "(in_handles(parked_handles, parked_count, records[i].handle) || true) &&",
        "every runnable thread is parked, listed or not.",
        T_Q,
    ),
    # --- T604: the running flag writers' falsifier ------------------------------------------
    mutation_t604(
        "entry-stop-not-named",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;",
        "return callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;",
        "a non-owner call of the stop function 0x155550 (store 0x15556B) is no falsifier.",
    ),
    mutation_t604(
        "entry-start-not-named",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;",
        "return callee == RUNNING_STOP || callee == RUNNING_START_BODY || callee == GATE;",
        "a non-owner call of the loading bar start 0x156D80 is no falsifier.",
    ),
    mutation_t604(
        "entry-start-body-not-named",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == GATE;",
        "a non-owner call of the start body 0x156D10 (store 0x156D31, the tail jump target) is no falsifier.",
    ),
    mutation_t604(
        "entry-gate-not-named",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY || callee == GATE;",
        "return callee == RUNNING_STOP || callee == RUNNING_START || callee == RUNNING_START_BODY;",
        "a non-owner call of the gate 0x156840 (tail jumps into the stop function) is no falsifier.",
    ),
    mutation_t604(
        "stop-address-off",
        "#define RUNNING_STOP 0x155550u",
        "#define RUNNING_STOP 0x155551u",
        "the stop function address is wrong.",
    ),
    mutation_t604(
        "start-address-off",
        "#define RUNNING_START 0x156D80u",
        "#define RUNNING_START 0x156D81u",
        "the start address is wrong.",
    ),
    mutation_t604(
        "start-body-address-off",
        "#define RUNNING_START_BODY 0x156D10u",
        "#define RUNNING_START_BODY 0x156D11u",
        "the start body address is wrong.",
    ),
    mutation_t604(
        "hook-ignores-budget",
        "if (atomic_load(&worker_budget) == 0u || handle == 0u) return;",
        "if (handle == 0u) return;",
        "the hook tracks and stops with the worker blanks off, flags off is not identical.",
    ),
    mutation_t604(
        "owner-is-checked",
        "if (handle != atomic_load(&owner_id)) {\n        if (running_flag_entry(callee)) check_running_flag_caller(callee,handle);",
        "if (handle != atomic_load(&owner_id) || running_flag_entry(callee)) {\n        if (running_flag_entry(callee)) check_running_flag_caller(callee,handle);",
        "the owner's own call of a writer entry is a stop.",
    ),
    mutation_t604(
        "guard-gate-ignored",
        "if (!in_gate && !worker_epoch_active()) return;",
        "if (!worker_epoch_active()) return;",
        "a non-owner call with the owner in the gate and no epoch is not a stop.",
    ),
    mutation_t604(
        "guard-epoch-ignored",
        "if (!in_gate && !worker_epoch_active()) return;",
        "if (!in_gate) return;",
        "a non-owner call in a worker epoch with the owner past the gate is not a stop.",
    ),
    mutation_t604(
        "guard-always-stops",
        "if (!in_gate && !worker_epoch_active()) return;",
        "if (false) return;",
        "a non-owner call outside the gate and the epoch stops, the falsifier fires where the claim is not relied on.",
    ),
    mutation_t604(
        "gate-wait-not-gate",
        "const bool in_gate = atomic_load(&owner_location) != OWNER_ELSEWHERE;",
        "const bool in_gate = atomic_load(&owner_location) == OWNER_GATE;",
        "the owner blocked in the gate's wait is not in the gate.",
    ),
    mutation_t604(
        "gate-never",
        "const bool in_gate = atomic_load(&owner_location) != OWNER_ELSEWHERE;",
        "const bool in_gate = false;",
        "the owner's gate position is ignored (only the epoch stops).",
    ),
    mutation_t604(
        "reason-swapped",
        'in_gate ? "the owner is in the loading bar gate" : "the worker\'s blanks are delivered"',
        'in_gate ? "the worker\'s blanks are delivered" : "the owner is in the loading bar gate"',
        "the stop names the wrong state.",
    ),
    mutation_t604(
        "text-thread-start-swapped",
        '(T604)",(unsigned)handle,(unsigned)start,',
        '(T604)",(unsigned)start,(unsigned)handle,',
        "the thread and its start are swapped in the stop text.",
    ),
    mutation_t604(
        "start-not-looked-up",
        'const uint32_t start = kernel_thread_get(handle,&self) ? self.start_routine : 0u;\n    snprintf(text,sizeof text,"thread 0x%x',
        'const uint32_t start = 0u;\n    (void)self;\n    snprintf(text,sizeof text,"thread 0x%x',
        "the stop does not say where the thread started.",
    ),
    # --- the option ---------------------------------------------------------------------------
    mutation(
        "option-needs-no-owner-waits",
        OPTIONS,
        "(out->vblank_worker_blanks == 0u || out->vblank_owner_waits != 0u) &&",
        "true &&",
        "the worker blanks parse without the owner waits.",
        T_O,
    ),
]
