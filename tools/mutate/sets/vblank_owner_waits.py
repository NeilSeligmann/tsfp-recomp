"""Mutations for the T460 owner-wait vblank delivery (`src/host/recomp_second_vblank.c`).

Each mutation is a change that compiles and that the first boot of the XMV movie would not notice by
itself: a delivery that is too eager, too late, unbounded or aimed at the wrong thread changes a
counter the title only reads later. `tsfp_host` is not a ctest binary, so the line in `main.c` that
prints the schedule record, the announcement and the report line are covered by
`tests/test_vblank_owner_waits.py` instead (a boot, skipped without the disc).
"""

# The anchors are exact source lines, some longer than the line limit.
# ruff: noqa: E501
MUTATIONS: list[dict] = [
    {
        "id": "vblank-owner-budget-off-by-one",
        "file": "src/host/recomp_second_vblank.c",
        "old": "state.owner_delivered >= owner_budget",
        "new": "state.owner_delivered > owner_budget",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "one more blank than the operator budgeted is delivered, and the "
            "stop that bounds the policy never fires on the stated number."
        ),
    },
    {
        "id": "vblank-owner-arm-ignores-refused",
        "file": "src/host/recomp_second_vblank.c",
        "old": "!state.refused && handle && fs &&",
        "new": "handle && fs &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "a policy that already refused (a callback faulted, a refill was "
            "seen) keeps delivering on the next wait."
        ),
    },
    {
        "id": "vblank-owner-arm-ignores-second-event",
        "file": "src/host/recomp_second_vblank.c",
        "old": "owner_budget != 0u && state.second_delivered &&",
        "new": "owner_budget != 0u &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "the owner arm is entered before the credited second event, so the "
            "counter would run ahead of the measured 2."
        ),
    },
    {
        "id": "vblank-owner-arm-ignores-budget-off",
        "file": "src/host/recomp_second_vblank.c",
        "old": "state.first_delivered && owner_budget != 0u && state.second_delivered &&",
        "new": "state.first_delivered && state.second_delivered &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the default (budget 0) would deliver, which changes every existing boot.",
    },
    {
        "id": "vblank-owner-arm-ignores-owner-handle",
        "file": "src/host/recomp_second_vblank.c",
        "old": "handle && fs && handle == state.owner_handle && fs == state.owner_fs &&",
        "new": "handle && fs && fs == state.owner_fs &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a wait by another guest thread that shares the PCR is taken for the owner's.",
    },
    {
        "id": "vblank-owner-arm-ignores-owner-fs",
        "file": "src/host/recomp_second_vblank.c",
        "old": "handle && fs && handle == state.owner_handle && fs == state.owner_fs &&",
        "new": "handle && fs && handle == state.owner_handle &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the control block identity is not compared.",
    },
    {
        "id": "vblank-owner-arm-ignores-owner-host-thread",
        "file": "src/host/recomp_second_vblank.c",
        "old": ("fs == state.owner_fs &&\n             pthread_equal(owner,pthread_self())) {"),
        "new": "fs == state.owner_fs && true) {",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "another host thread that presents the owner's identity delivers a "
            "callback concurrently with the owner."
        ),
    },
    {
        "id": "vblank-owner-delivery-skips-quiescence-enabled",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (!recomp_vblank_quiescence_enabled()) refuse(text->quiescence_off);",
        "new": "if (false && !recomp_vblank_quiescence_enabled()) refuse(text->quiescence_off);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "the policy delivers with the check configured off, which is the "
            "one precondition that keeps a runnable reader from sampling the "
            "counter."
        ),
    },
    {
        "id": "vblank-owner-delivery-skips-quiescence-check",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(void)recomp_vblank_quiescence_check_parked(handle,0x3D3550u,&held,held != 0u ? 1u : 0u);",
        "new": "(void)handle; (void)held;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the check is required but never run: a runnable second reader is not noticed.",
    },
    {
        "id": "vblank-owner-delivery-checks-the-wrong-deliverer",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(void)recomp_vblank_quiescence_check_parked(handle,0x3D3550u,&held,held != 0u ? 1u : 0u);",
        "new": "(void)recomp_vblank_quiescence_check_parked(0u,0x3D3550u,&held,held != 0u ? 1u : 0u);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "an unknown deliverer always refuses, so every delivery stops.",
    },
    {
        "id": "vblank-owner-delivery-skips-coupling-requirement",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (!d3d8_vblank_effects_enabled()) refuse(text->coupling_off);",
        "new": "if (false && !d3d8_vblank_effects_enabled()) refuse(text->coupling_off);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "without the coupling nobody published the count, so the callback "
            "record would be a stale value."
        ),
    },
    {
        "id": "vblank-owner-delivery-ignores-delivered-counter",
        "file": "src/host/recomp_second_vblank.c",
        "old": "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done)",
        "new": "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done * 0u)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "after the first delivery the counter is one ahead of the base, so "
            "every later one refuses (kills the running relation, not just the "
            "base)."
        ),
    },
    {
        "id": "vblank-owner-delivery-counter-base-moves",
        "file": "src/host/recomp_second_vblank.c",
        "old": "#define OWNER_WAIT_FIRST_COUNTER 2u",
        "new": "#define OWNER_WAIT_FIRST_COUNTER 1u",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the counter the title has after its startup and credited events is 2, measured.",
    },
    {
        "id": "vblank-owner-delivery-ignores-irql",
        "file": "src/host/recomp_second_vblank.c",
        "old": (
            "if (kernel_sync_current_irql() != 0u || !kernel_guest_read_bytes(fs,pcr,sizeof(pcr)) || pcr[0x24u] != 0u ||\n"
            "        !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) || "
            "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done))"
        ),
        "new": (
            "if (!kernel_guest_read_bytes(fs,pcr,sizeof(pcr)) || pcr[0x24u] != 0u ||\n"
            "        !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) || "
            "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done))"
        ),
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "a delivery from a thread above PASSIVE_LEVEL, which the measured "
            "helper callers do not do."
        ),
    },
    {
        "id": "vblank-owner-delivery-ignores-pcr-level",
        "file": "src/host/recomp_second_vblank.c",
        "old": (
            "pcr[0x24u] != 0u ||\n"
            "        !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) || "
            "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done))"
        ),
        "new": (
            "false ||\n"
            "        !kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) || "
            "title_counter != (startup ? 1u : OWNER_WAIT_FIRST_COUNTER + done))"
        ),
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the guest's own KPCR level byte is not compared with the host model's.",
    },
    {
        "id": "vblank-owner-delivery-ignores-device-pointer",
        "file": "src/host/recomp_second_vblank.c",
        "old": (
            "!kernel_guest_read_u32(0x3E3F58u,&device) || device != DEVICE ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) "
        ),
        "new": (
            "!kernel_guest_read_u32(0x3E3F58u,&device) ||\n"
            "        !kernel_guest_read_u32(0x563918u,&title_counter) "
        ),
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a replaced device object is not noticed.",
    },
    {
        "id": "vblank-owner-delivery-count-not-derived",
        "file": "src/host/recomp_second_vblank.c",
        "old": "const uint32_t count = (uint32_t)d3d8_vblank_effects_applied() + 1u;",
        "new": "const uint32_t count = (uint32_t)d3d8_vblank_effects_applied();",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the count the coupled waits published and the unbacked startup event's one.",
    },
    {
        "id": "vblank-owner-delivery-checks-fewer-words",
        "file": "src/host/recomp_second_vblank.c",
        "old": "for (unsigned i=0u;i<9u;i++) {\n        uint32_t value;\n        if (modelled && i == 3u) continue; /* T513: the threshold is what the flip queue left */\n        if (!kernel_guest_read_u32(DEVICE+offsets[i],&value) || value != expected[i])\n            refuse(text->bookkeeping",
        "new": "for (unsigned i=0u;i<8u;i++) {\n        uint32_t value;\n        if (modelled && i == 3u) continue; /* T513: the threshold is what the flip queue left */\n        if (!kernel_guest_read_u32(DEVICE+offsets[i],&value) || value != expected[i])\n            refuse(text->bookkeeping",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the last measured-empty word (the second pending flag word) is not compared.",
    },
    {
        "id": "vblank-owner-delivery-skips-fs-write-probe",
        "file": "src/host/recomp_second_vblank.c",
        "old": "!kernel_guest_read_u32(fs,&fs_head) || !kernel_guest_write_u32(fs,fs_head) ||\n        !kernel_guest_read_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "new": "!kernel_guest_read_u32(fs,&fs_head) ||\n        !kernel_guest_read_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "a PCR page that stopped being writable is found out by the "
            "callback instead of before it."
        ),
    },
    {
        "id": "vblank-owner-delivery-skips-frame-write-probe",
        "file": "src/host/recomp_second_vblank.c",
        "old": "        !kernel_guest_write_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "new": "        !kernel_guest_read_bytes(stack_high-20u,frame,sizeof(frame)) ||\n        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the private stack's frame is not probed for writing.",
    },
    {
        "id": "vblank-owner-delivery-skips-count-write-probe",
        "file": "src/host/recomp_second_vblank.c",
        "old": "        !kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "new": "        !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the device page's writability is not probed.",
    },
    {
        "id": "vblank-owner-delivery-skips-counter-write-probe",
        "file": "src/host/recomp_second_vblank.c",
        "old": "!kernel_guest_write_u32(DEVICE+0x1DE8u,count) || !kernel_guest_write_u32(0x563918u,title_counter))\n        refuse(text->probes",
        "new": "!kernel_guest_write_u32(DEVICE+0x1DE8u,count))\n        refuse(text->probes",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "the title counter page's writability is not probed, the leaf "
            "faults after the owner already went on."
        ),
    },
    {
        "id": "vblank-owner-delivery-payload-flip-index",
        "file": "src/host/recomp_second_vblank.c",
        "old": "const uint32_t payload[3] = {record.count,record.flip_index,record.flags};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "new": "const uint32_t payload[3] = {record.count,1u,record.flags};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the record's flip index is the device's (0 here).",
    },
    {
        "id": "vblank-owner-delivery-ignores-callback-failure",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke);",
        "new": "if (!invoke(0x22020u,stack_low,stack_high,payload) && false) refuse(text->invoke);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a callback that did not run is counted as delivered.",
    },
    {
        "id": "vblank-owner-delivery-ignores-missing-counter-move",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (!kernel_guest_read_u32(0x563918u,&after) || after != title_counter + 1u)\n        refuse(text->advance",
        "new": "if (!kernel_guest_read_u32(0x563918u,&after) || false)\n        refuse(text->advance",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": (
            "a callback that left the counter alone is counted as delivered, "
            "and the next wait's relation then fails far from the cause."
        ),
    },
    {
        "id": "vblank-owner-delivery-ignores-refill-during-callback",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (refused_during) refuse(text->during);",
        "new": "if (refused_during && false) refuse(text->during);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "another thread's refused note during the callback is dropped.",
    },
    {
        "id": "vblank-owner-delivery-never-counts",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else state.owner_delivered++;",
        "new": "else (void)0;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the budget never drains.",
    },
    {
        "id": "vblank-owner-delivery-never-clears-inflight",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else state.owner_delivered++;\n        state.inflight = false;",
        "new": "else state.owner_delivered++;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "every later wait and poll refuses as reentrant.",
    },
    {
        "id": "vblank-owner-budget-api-accepts-over-cap",
        "file": "src/host/recomp_second_vblank.c",
        "old": "budget <= RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX &&",
        "new": "budget <= RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX + 1u &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the bound on the budget.",
    },
    {
        "id": "vblank-owner-budget-api-accepts-disabled-policy",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(budget == 0u || state.enabled);",
        "new": "true;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a budget on a policy that is off would arm a path that has no owner.",
    },
    {
        "id": "vblank-owner-budget-api-accepts-inflight",
        "file": "src/host/recomp_second_vblank.c",
        "old": "const bool accepted = state.inflight == false && state.epoch != UINT64_MAX &&\n                          budget <= RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX &&",
        "new": "const bool accepted = state.epoch != UINT64_MAX &&\n                          budget <= RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the budget changes under a running callback.",
    },
    {
        "id": "vblank-owner-configure-keeps-the-budget",
        "file": "src/host/recomp_second_vblank.c",
        "old": "stack_high = high; owner_budget = 0u;",
        "new": "stack_high = high;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a stale budget from an earlier epoch arms the next configuration.",
    },
    {
        "id": "vblank-owner-snapshot-hides-the-budget",
        "file": "src/host/recomp_second_vblank.c",
        "old": "*out = state; out->owner_budget = owner_budget;",
        "new": "*out = state;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the report line and the trace read the budget from the snapshot.",
    },
    {
        "id": "vblank-owner-opt-accepts-zero",
        "file": "src/host/host_options.c",
        "old": "budget == 0u || budget > 1000000u",
        "new": "budget > 1000000u",
        "targets": ["test_host_options"],
        "why": "a budget of 0 is the same as the option being absent but reads as on.",
    },
    {
        "id": "vblank-owner-opt-cap-moves",
        "file": "src/host/host_options.c",
        "old": "budget > 1000000u ||",
        "new": "budget > 1000001u ||",
        "targets": ["test_host_options"],
        "why": "the cap is the one the header documents.",
    },
    {
        "id": "vblank-owner-opt-accepts-sign",
        "file": "src/host/host_options.c",
        "old": "budget > 1000000u ||\n        text[0] < '0' || text[0] > '9') {",
        "new": "budget > 1000000u ||\n        false) {",
        "targets": ["test_host_options"],
        "why": "strtoull takes a sign and leading spaces.",
    },
    {
        "id": "vblank-owner-opt-accepts-trailing-text",
        "file": "src/host/host_options.c",
        "old": "if (end == text || *end != '\\0' || budget == 0u",
        "new": "if (end == text || budget == 0u",
        "targets": ["test_host_options"],
        "why": "'5x' parses as 5.",
    },
    {
        "id": "vblank-owner-opt-needs-no-coupling",
        "file": "src/host/host_options.c",
        "old": (
            "(out->headless_second_vblank && out->couple_vblank_effects && "
            "out->check_vblank_quiescence)"
        ),
        "new": "(out->headless_second_vblank && out->check_vblank_quiescence)",
        "targets": ["test_host_options"],
        "why": (
            "the delivery refuses at the first wait, after a boot, instead of at the command line."
        ),
    },
    {
        "id": "vblank-owner-opt-needs-no-quiescence",
        "file": "src/host/host_options.c",
        "old": (
            "(out->headless_second_vblank && out->couple_vblank_effects && "
            "out->check_vblank_quiescence)"
        ),
        "new": "(out->headless_second_vblank && out->couple_vblank_effects)",
        "targets": ["test_host_options"],
        "why": "the quiescence check is part of the contract.",
    },
    {
        "id": "vblank-owner-opt-needs-no-second-event",
        "file": "src/host/host_options.c",
        "old": (
            "(out->headless_second_vblank && out->couple_vblank_effects && "
            "out->check_vblank_quiescence)"
        ),
        "new": "(out->couple_vblank_effects && out->check_vblank_quiescence)",
        "targets": ["test_host_options"],
        "why": "the budget on top of the first-event policy would arm nothing.",
    },
    {
        "id": "vblank-owner-opt-default-on",
        "file": "src/host/host_options.c",
        "old": ("    out->vblank_owner_waits = 0u;\n"),
        "new": ("    out->vblank_owner_waits = 1u;\n"),
        "targets": ["test_host_options"],
        "why": "default off.",
    },
    {
        "id": "vblank-frame-admits-equal-counter",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(int32_t)(counter - last) < 1)",
        "new": "(int32_t)(counter - last) < 0)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a frame wait whose exit test does NOT hold (counter equal to the last exit) runs through and the title would spin on a blank nobody delivers.",
    },
    {
        "id": "vblank-frame-unsigned-distance",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(int32_t)(counter - last) < 1)",
        "new": "(counter - last) < 1u)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the exit test of the title is signed (jl): an unsigned distance admits a counter BEHIND the last exit.",
    },
    {
        "id": "vblank-frame-admit-ignores-budget-off",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else if (state.second_delivered && owner_budget != 0u) {",
        "new": "else if (state.second_delivered) {",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the default (budget 0) would admit a third frame wait, which changes every existing boot.",
    },
    {
        "id": "vblank-frame-admit-ignores-second-event",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else if (state.second_delivered && owner_budget != 0u) {",
        "new": "else if (owner_budget != 0u) {",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the credited SECOND event is taken for a later frame wait and never delivered.",
    },
    {
        "id": "vblank-frame-reads-wrong-last-exit",
        "file": "src/host/recomp_second_vblank.c",
        "old": "kernel_guest_read_u32(FRAME_LAST_EXIT,&last)",
        "new": "kernel_guest_read_u32(FRAME_LAST_EXIT-4u,&last)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the test reads the previous exit's neighbour word (0x7A58AC) instead of the stored counter.",
    },
    {
        "id": "vblank-frame-operands-swapped",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(int32_t)(counter - last) < 1)",
        "new": "(int32_t)(last - counter) < 1)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the distance is taken the wrong way round.",
    },
    {
        "id": "vblank-frame-unreadable-ignored",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (!kernel_guest_read_u32(COUNTER,&counter) || !kernel_guest_read_u32(FRAME_LAST_EXIT,&last))",
        "new": "if (!kernel_guest_read_u32(COUNTER,&counter) && !kernel_guest_read_u32(FRAME_LAST_EXIT,&last))",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "an unreadable last exit word is judged on a zero it never held.",
    },
    {
        "id": "vblank-frame-admit-not-counted",
        "file": "src/host/recomp_second_vblank.c",
        "old": "state.frames_admitted++; admitted = true;",
        "new": "admitted = true;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the report undercounts the frame waits the model let through.",
    },
    {
        "id": "vblank-later-thread-ignores-budget-off",
        "file": "src/host/recomp_second_vblank.c",
        "old": "later_thread = state.second_delivered && owner_budget != 0u &&",
        "new": "later_thread = state.second_delivered &&",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the T587 text appears with the budget off, so every default boot's stop line changes.",
    },
    {
        "id": "vblank-later-thread-ignores-second-event",
        "file": "src/host/recomp_second_vblank.c",
        "old": "later_thread = state.second_delivered && owner_budget",
        "new": "later_thread = owner_budget",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a second producer wait before the credited second event is described as a later thread's.",
    },
    {
        "id": "vblank-later-thread-takes-owner-identity",
        "file": "src/host/recomp_second_vblank.c",
        "old": "owner_budget != 0u && !pthread_equal(owner,pthread_self());",
        "new": "owner_budget != 0u;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the owner's own wrong-identity wait is reported as another thread's.",
    },
    {
        "id": "vblank-later-thread-text-not-used",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (error && later_thread) error = later_thread_refusal(handle);",
        "new": "if (error && !later_thread) error = later_thread_refusal(handle);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the stop keeps the bare T183 text for exactly the case T587 explains.",
    },
    {
        "id": "vblank-later-thread-verdict-inverted",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (verdict.holds) snprintf(text + used",
        "new": "if (!verdict.holds) snprintf(text + used",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the stop says the quiescence holds when a runnable thread refuses it, and the reverse.",
    },
    {
        "id": "vblank-later-thread-wrong-deliverer",
        "file": "src/host/recomp_second_vblank.c",
        "old": "recomp_vblank_quiescence_evaluate(records, count, handle)",
        "new": "recomp_vblank_quiescence_evaluate(records, count, 0u)",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the predicate is evaluated for no thread, so the verdict names the wrong cause.",
    },
    {
        "id": "vblank-later-thread-wrong-start",
        "file": "src/host/recomp_second_vblank.c",
        "old": "kernel_thread_get(handle, &self) ? self.start_routine : 0u;",
        "new": "kernel_thread_get(handle, &self) ? self.start_context : 0u;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the stop names the thread's start parameter as where it started, so 0x156CB0 would read as 0.",
    },
    {
        "id": "vblank-poll-ignores-opt-in",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else if (counter == last && poll_blank_enabled && (interactive || state.owner_delivered < owner_budget)) {",
        "new": "else if (counter == last && (interactive || state.owner_delivered < owner_budget)) {",
        "targets": [
            "test_recomp_second_vblank_owner_waits",
            "pytest:tests/test_vblank_poll_blank_boot.py -q",
        ],
        "why": "the default (flag off) delivers from the poll, which changes every existing boot's stop.",
    },
    {
        "id": "vblank-poll-ignores-budget",
        "file": "src/host/recomp_second_vblank.c",
        "old": "poll_blank_enabled && (interactive || state.owner_delivered < owner_budget)) {",
        "new": "poll_blank_enabled) {",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the poll blank is not bounded by --vblank-owner-waits.",
    },
    {
        "id": "vblank-poll-any-failing-test",
        "file": "src/host/recomp_second_vblank.c",
        "old": "else if (counter == last && poll_blank_enabled",
        "new": "else if ((int32_t)(counter - last) < 1 && poll_blank_enabled",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a counter BEHIND the last exit, which one blank does not satisfy, also gets a blank.",
    },
    {
        "id": "vblank-poll-not-counted",
        "file": "src/host/recomp_second_vblank.c",
        "old": "        state.poll_delivered++;\n",
        "new": "",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the report undercounts the blanks the poll delivered.",
    },
    {
        "id": "vblank-poll-skips-effects",
        "file": "src/host/recomp_second_vblank.c",
        "old": '        if (!d3d8_gpu_model_blank()) { finish(); refuse("poll blank: the clock or the helper effects refused the blank"); }\n',
        "new": "",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the callback is delivered without the blank's own device effects, over a stale count.",
    },
    {
        "id": "vblank-poll-effects-before-quiescence",
        "file": "src/host/recomp_second_vblank.c",
        "old": "        if (!recomp_vblank_quiescence_enabled()) { finish(); refuse(owner_texts.quiescence_off); }\n",
        "new": "",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the blank's device effects are applied before the quiescence refusal, leaving a write behind a stop.",
    },
    {
        "id": "vblank-poll-effects-before-coupling",
        "file": "src/host/recomp_second_vblank.c",
        "old": "        if (!d3d8_vblank_effects_enabled()) { finish(); refuse(owner_texts.coupling_off); }\n",
        "new": "",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the coupling refusal is lost to the blank model's own failure text.",
    },
    {
        "id": "vblank-poll-configure-keeps-opt-in",
        "file": "src/host/recomp_second_vblank.c",
        "old": "owner_budget = 0u; poll_blank_enabled = false;",
        "new": "owner_budget = 0u;",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a configure leaves the previous epoch's poll blank on.",
    },
    {
        "id": "vblank-poll-set-ignores-budget",
        "file": "src/host/recomp_second_vblank.c",
        "old": "(!enabled || owner_budget != 0u);",
        "new": "(!enabled || true);",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the poll blank is accepted without the owner waits it is built on.",
    },
    {
        "id": "vblank-poll-set-always-off",
        "file": "src/host/recomp_second_vblank.c",
        "old": "if (accepted) poll_blank_enabled = enabled;",
        "new": "if (accepted) poll_blank_enabled = false;",
        "targets": [
            "test_recomp_second_vblank_owner_waits",
            "pytest:tests/test_vblank_poll_blank_boot.py -q",
        ],
        "why": "the opt-in never takes.",
    },
    {
        "id": "vblank-model-blank-50hz",
        "file": "src/gpu/d3d8_gpu.c",
        "old": "(d3d8_device_load32(0x1DDCu) & 0x00400000u) != 0u ? 60u : 50u;\n    uint64_t blank_time = 0u;\n    if (!d3d8_vblank_effects_enabled()",
        "new": "(d3d8_device_load32(0x1DDCu) & 0x00400000u) != 0u ? 50u : 60u;\n    uint64_t blank_time = 0u;\n    if (!d3d8_vblank_effects_enabled()",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "the modelled blank uses the other display refresh class.",
    },
    {
        "id": "vblank-model-blank-ignores-effects-off",
        "file": "src/gpu/d3d8_gpu.c",
        "old": "if (!d3d8_vblank_effects_enabled() || !kernel_clock_frame_floor(refresh_hz, &blank_time)) {",
        "new": "if (!kernel_clock_frame_floor(refresh_hz, &blank_time)) {",
        "targets": ["test_recomp_second_vblank_owner_waits"],
        "why": "a blank is modelled with the coupled effects off.",
    },
    {
        "id": "vblank-poll-opt-default-on",
        "file": "src/host/host_options.c",
        "old": "    } else if (!out->vblank_poll_blank && out->vblank_owner_waits != 0u) {\n        out->vblank_poll_blank = true;",
        "new": "    } else if (false) {\n        out->vblank_poll_blank = true;",
        "targets": ["test_host_options"],
        "why": "T762 made the poll blank an announced default with the owner waits: it is no longer on by default.",
    },
    {
        "id": "vblank-poll-opt-needs-nothing",
        "file": "src/host/host_options.c",
        "old": "(!out->vblank_poll_blank || out->vblank_owner_waits != 0u) &&",
        "new": "true &&",
        "targets": ["test_host_options"],
        "why": "the poll blank is accepted without the owner waits.",
    },
    {
        "id": "vblank-poll-opt-not-parsed",
        "file": "src/host/host_options.c",
        "old": "out->vblank_poll_blank = true;\n        } else if",
        "new": "out->vblank_poll_blank = false;\n        } else if",
        "targets": ["test_host_options"],
        "why": "the flag is accepted and ignored.",
    },
]
