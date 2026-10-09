# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the flip queue and flip processor (T407): src/gpu/d3d8_flip.c, the vblank helper
effects that run it (src/gpu/d3d8_vblank_effects.c), the Swap hook (src/gpu/d3d8_present.c), the
startup callback preflight that proves a drained queue (src/gpu/d3d8_first_vblank.c) and the
`--model-flips` option (src/host/host_options.c).

TARGETS. The ctest binary `test_d3d8_flip` (exact literals, the Swap hook end to end) and the pytest
suite that replays the ORIGINAL bytes of 0x003DC200 and 0x003DC5E0 under the oracle
(`tests/test_d3d8_flip_oracle.py`, dword for dword plus the hardware the original reached through
the register window). The ctest binary is also the compile guard for the pytest targets.

CONVENTION. `&& false` rather than a bare `false`, because `-Wunused-parameter -Werror` turns the
latter into NOT-A-MUTANT, which reads like evidence.
"""

FLIP = "src/gpu/d3d8_flip.c"
EFFECTS = "src/gpu/d3d8_vblank_effects.c"
PRESENT = "src/gpu/d3d8_present.c"
FIRST = "src/gpu/d3d8_first_vblank.c"
OPTIONS = "src/host/host_options.c"
T_FLIP = ["test_d3d8_flip"]
ORACLE = "pytest:tests/test_d3d8_flip_oracle.py"


def mutation(
    identifier: str,
    file: str,
    old: str,
    new: str,
    why: str,
    targets: list[str] | None = None,
) -> dict:
    return {
        "id": f"t407-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets if targets is not None else [*T_FLIP, ORACLE],
        "why": why,
    }


MUTATIONS: list[dict] = [
    # --- the flip processor 0x003DC200 -------------------------------------------------
    mutation(
        "process-slot-parity",
        FLIP,
        "    return D3D8_FLIP_DEV_SLOT0 + (consumer & 1u) * D3D8_FLIP_SLOT_BYTES;",
        "    return D3D8_FLIP_DEV_SLOT0 + ((consumer + 1u) & 1u) * D3D8_FLIP_SLOT_BYTES;",
        "the consumer slot is the low bit of the consumer index, the other slot pending must be a no-op.",
    ),
    mutation(
        "process-ignores-due-count",
        FLIP,
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_COUNT) != d3d8_device_load32(slot + 4u)) {",
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_COUNT) != d3d8_device_load32(slot + 4u) && false) {",
        "a flip for a later blank must wait: showing it early changes the picture a frame ahead.",
    ),
    mutation(
        "process-due-is-at-or-after",
        FLIP,
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_COUNT) != d3d8_device_load32(slot + 4u)) {",
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_COUNT) < d3d8_device_load32(slot + 4u)) {",
        "the original flips only on EQUALITY of the count and the target, a count past it never flips.",
    ),
    mutation(
        "process-slot-not-cleared",
        FLIP,
        "        d3d8_device_store32(slot, 0u);\n        const uint32_t value",
        "        const uint32_t value",
        "the pending word is cleared at the flip, or the next blank shows it again.",
    ),
    mutation(
        "process-global-not-stored",
        FLIP,
        "        d3d8_guest_store32(D3D8_FLIP_GLOBAL_VALUE, value);\n",
        "",
        "0x003E6408 is the address the last flip showed, the ISR programs the display start from it.",
    ),
    mutation(
        "process-global-value",
        FLIP,
        "        d3d8_guest_store32(D3D8_FLIP_GLOBAL_VALUE, value);",
        "        d3d8_guest_store32(D3D8_FLIP_GLOBAL_VALUE, value + 4u);",
        "the global holds the slot's address word unchanged.",
    ),
    mutation(
        "process-display-start-skip-inverted",
        FLIP,
        "    if (d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_START_OFF) != 0u) {\n        return;",
        "    if (d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_START_OFF) == 0u) {\n        return;",
        "+0x1B8 set (as after CreateDevice) skips the display start, clear programs it.",
    ),
    mutation(
        "display-start-field-pitch",
        FLIP,
        "        programmed = value - (pitch >> 1);",
        "        programmed = value - pitch;",
        "the field select branch subtracts half the pitch.",
    ),
    mutation(
        "display-start-field-select-bit",
        FLIP,
        "    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && !top_is_field && field_zero) {",
        "    if ((mode & MODE_BIT_FIELD_SELECT) == 0u && !top_is_field && field_zero) {",
        "the first branch needs mode bit 0x1000000 set.",
    ),
    mutation(
        "display-start-top-bits",
        FLIP,
        "    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && !top_is_field && field_zero) {",
        "    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && top_is_field && field_zero) {",
        "the first branch needs the mode word's top bits to differ from 0x80000000.",
    ),
    mutation(
        "display-start-field-status",
        FLIP,
        "    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && !top_is_field && field_zero) {",
        "    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && !top_is_field && !field_zero) {",
        "the field status word must be zero for either subtraction.",
    ),
    mutation(
        "display-start-second-pitch-sign",
        FLIP,
        "        programmed = value - pitch;",
        "        programmed = value + pitch;",
        "the second branch subtracts the whole pitch.",
    ),
    mutation(
        "display-start-second-needs-pitch-bit",
        FLIP,
        "    } else if (top_is_field && (mode & MODE_BIT_FIELD_PITCH) != 0u && field_zero) {",
        "    } else if (top_is_field && field_zero) {",
        "the second branch needs mode bit 0x200000.",
    ),
    mutation(
        "display-start-second-needs-top",
        FLIP,
        "    } else if (top_is_field && (mode & MODE_BIT_FIELD_PITCH) != 0u && field_zero) {",
        "    } else if ((mode & MODE_BIT_FIELD_PITCH) != 0u && field_zero) {",
        "the second branch needs the mode word's top bits to be exactly 0x80000000.",
    ),
    mutation(
        "display-start-not-recorded",
        FLIP,
        "    hardware.display_start = programmed;\n",
        "",
        "the record holds the value the register would have been given.",
    ),
    mutation(
        "gamma-flag-test",
        FLIP,
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u) == 1u) {",
        "        if (d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u) != 0u) {",
        "the upload runs only for the flag value 1, the original compares with 1 exactly.",
    ),
    mutation(
        "gamma-flag-not-cleared",
        FLIP,
        "            d3d8_device_store32(D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u, 0u);\n",
        "",
        "the flag is cleared after the upload, so only the first flip uploads.",
    ),
    mutation(
        "gamma-ramp-index",
        FLIP,
        "        const uint32_t ramp = d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER) & 1u;",
        "        const uint32_t ramp = 0u;",
        "the ramp and its flag are those of the consumer slot.",
    ),
    mutation(
        "gamma-interleave",
        FLIP,
        "            hardware.gamma[entry * 3u + channel] =",
        "            hardware.gamma[channel * D3D8_FLIP_GAMMA_CHANNEL_BYTES + entry] =",
        "red, green and blue go out interleaved, entry by entry.",
    ),
    mutation(
        "gamma-channel-stride",
        FLIP,
        "            const uint32_t address = ramp + channel * D3D8_FLIP_GAMMA_CHANNEL_BYTES + entry;",
        "            const uint32_t address = ramp + channel * 0x80u + entry;",
        "the channels are 0x100 bytes apart.",
    ),
    mutation(
        "gamma-ramp-base",
        FLIP,
        "    const uint32_t ramp = D3D8_FLIP_DEV_GAMMA_RAMPS + index * D3D8_FLIP_GAMMA_RAMP_BYTES;",
        "    const uint32_t ramp = D3D8_FLIP_DEV_GAMMA_RAMPS + index * 0x100u;",
        "the two ramps are 0x300 bytes apart.",
    ),
    mutation(
        "pgraph-not-recorded",
        FLIP,
        "        hardware.pgraph_increments++;\n        log_event_locked(D3D8_FLIP_HW_PGRAPH_INCREMENT, PGRAPH_FLIP_READ_INCREMENT);\n",
        "",
        "every flip ORs 2 into PGRAPH 0x40071C, which is what lets the GPU's flip stall go.",
    ),
    mutation(
        "consumer-step",
        FLIP,
        "        const uint32_t consumer = d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER) + 1u;",
        "        const uint32_t consumer = d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER) + 2u;",
        "the consumer index moves one slot.",
    ),
    mutation(
        "chain-stays-on-slot",
        FLIP,
        "        slot = slot_address(consumer);\n        hardware.flips++;",
        "        hardware.flips++;",
        "after a flip the loop looks at the NEXT slot, so two due flips both run in one blank.",
    ),
    mutation(
        "flips-count",
        FLIP,
        "        hardware.flips++;\n        processed++;",
        "        hardware.flips++;",
        "the number of flips processed is the original's return value and the helper's flags.",
    ),
    # --- the software method 0x003DC5E0, type 1 -----------------------------------------
    mutation(
        "queue-interval-mask",
        FLIP,
        "    const uint32_t interval = shifted & 7u;",
        "    const uint32_t interval = shifted & 3u;",
        "the interval is three bits.",
    ),
    mutation(
        "queue-address-mask",
        FLIP,
        "    const uint32_t address = shifted & 0xFFFFFFF0u;",
        "    const uint32_t address = shifted & 0xFFFFFFFFu;",
        "the low four bits of the shifted data are the interval and the immediate bit, not the address.",
    ),
    mutation(
        "queue-passed-test",
        FLIP,
        "    if ((int32_t)(target - count) <= 0) {",
        "    if ((int32_t)(target - count) < 0) {",
        "a target equal to the count counts as passed (the original's `jg` skips only a positive distance).",
    ),
    mutation(
        "queue-passed-unsigned",
        FLIP,
        "    if ((int32_t)(target - count) <= 0) {",
        "    if (target <= count) {",
        "the distance is signed 32 bits, a count near the wrap must still compare correctly.",
    ),
    mutation(
        "queue-immediate-swapped",
        FLIP,
        "        target = (shifted & D3D8_FLIP_INTERVAL_IMMEDIATE_BIT) != 0u ? count : count + 1u;",
        "        target = (shifted & D3D8_FLIP_INTERVAL_IMMEDIATE_BIT) != 0u ? count + 1u : count;",
        "the immediate bit shows the flip at once, otherwise at the next blank.",
    ),
    mutation(
        "queue-threshold",
        FLIP,
        "    d3d8_device_store32(D3D8_FLIP_DEV_THRESHOLD, target + interval);",
        "    d3d8_device_store32(D3D8_FLIP_DEV_THRESHOLD, target);",
        "the queue sets the threshold one interval after the target.",
    ),
    mutation(
        "queue-last-target-not-stored",
        FLIP,
        "    d3d8_device_store32(D3D8_FLIP_DEV_LAST_TARGET, target);\n",
        "",
        "the last target is what the next flip's interval counts from.",
    ),
    mutation(
        "queue-slot-target",
        FLIP,
        "    d3d8_device_store32(slot + 4u, target);",
        "    d3d8_device_store32(slot + 4u, count);",
        "the slot holds the target count.",
    ),
    mutation(
        "queue-slot-address",
        FLIP,
        "    d3d8_device_store32(slot + 8u, address);",
        "    d3d8_device_store32(slot + 8u, address + 0x10u);",
        "the slot holds the address with the interval bits cleared.",
    ),
    mutation(
        "queue-slot-pending",
        FLIP,
        "    d3d8_device_store32(slot, 1u);\n    d3d8_device_store32(slot + 4u, target);",
        "    d3d8_device_store32(slot + 4u, target);",
        "the slot is marked pending.",
    ),
    mutation(
        "queue-producer-step",
        FLIP,
        "    d3d8_device_store32(D3D8_FLIP_DEV_PRODUCER, producer + 1u);",
        "    d3d8_device_store32(D3D8_FLIP_DEV_PRODUCER, producer + 2u);",
        "the producer index moves one slot.",
    ),
    mutation(
        "queue-does-not-process",
        FLIP,
        "    hardware.queued++;\n    return d3d8_flip_process_locked();",
        "    hardware.queued++;\n    return 0u;",
        "the original runs the flip processor right after queueing, an immediate flip shows at once.",
    ),
    mutation(
        "queue-counter",
        FLIP,
        "    hardware.queued++;\n",
        "",
        "the queued counter is part of the record.",
    ),
    # --- refusals ----------------------------------------------------------------------
    mutation(
        "refuse-type",
        FLIP,
        "    if ((method_data & 0x1Fu) != D3D8_FLIP_METHOD_TYPE_FLIP) {",
        "    if ((method_data & 0x1Fu) != D3D8_FLIP_METHOD_TYPE_FLIP && false) {",
        "any other software method type reaches code this port does not have.",
    ),
    mutation(
        "refuse-type-mask",
        FLIP,
        "    if ((method_data & 0x1Fu) != D3D8_FLIP_METHOD_TYPE_FLIP) {",
        "    if ((method_data & 0x0Fu) != D3D8_FLIP_METHOD_TYPE_FLIP) {",
        "the type is the low FIVE bits (0x21 is a flip, 0x11 is type 17).",
    ),
    mutation(
        "refuse-callback",
        FLIP,
        "    if (d3d8_device_load32(D3D8_FLIP_DEV_CALLBACK) != 0u) {",
        "    if (d3d8_device_load32(D3D8_FLIP_DEV_CALLBACK) != 0u && false) {",
        "a set flip callback is called by the original with an rdtsc derived record.",
    ),
    mutation(
        "refuse-pending-producer",
        FLIP,
        "    if (d3d8_device_load32(slot_address(producer)) != 0u) {",
        "    if (d3d8_device_load32(slot_address(producer)) != 0u && false) {",
        "a third queued flip would overwrite a pending one, which only the hardware stall prevents.",
    ),
    mutation(
        "refuse-producer-slot",
        FLIP,
        "    if (d3d8_device_load32(slot_address(producer)) != 0u) {",
        "    if (d3d8_device_load32(slot_address(producer + 1u)) != 0u) {",
        "the producer slot is the low bit of the PRODUCER index.",
    ),
    # --- the Swap's data word ----------------------------------------------------------
    mutation(
        "swap-data-default-interval",
        FLIP,
        "    uint32_t interval = present_interval == 0u ? 1u : present_interval;",
        "    uint32_t interval = present_interval;",
        "an interval of 0 reads as 1.",
    ),
    mutation(
        "swap-data-bit-one",
        FLIP,
        "    if ((interval & 1u) != 0u) {\n        bits |= 1u;",
        "    if ((interval & 1u) != 0u) {\n        bits |= 2u;",
        "interval bit 0 ORs 1.",
    ),
    mutation(
        "swap-data-bit-two",
        FLIP,
        "    if ((interval & 2u) != 0u) {\n        bits |= 2u;",
        "    if ((interval & 2u) != 0u) {\n        bits |= 1u;",
        "interval bit 1 ORs 2.",
    ),
    mutation(
        "swap-data-bit-four",
        FLIP,
        "    if ((interval & 4u) != 0u) {\n        bits |= 3u;",
        "    if ((interval & 4u) != 0u) {\n        bits |= 4u;",
        "interval bit 2 ORs 3 (a four frame interval is the three bit code 3).",
    ),
    mutation(
        "swap-data-immediate",
        FLIP,
        "        bits |= D3D8_FLIP_INTERVAL_IMMEDIATE_BIT;",
        "        bits |= 4u;",
        "the sign bit of the interval ORs the immediate bit 8.",
    ),
    mutation(
        "swap-data-shift",
        FLIP,
        "    return (bits << 5) | D3D8_FLIP_METHOD_TYPE_FLIP;",
        "    return (bits << 4) | D3D8_FLIP_METHOD_TYPE_FLIP;",
        "the data is the bits shifted left five.",
    ),
    mutation(
        "swap-data-type",
        FLIP,
        "    return (bits << 5) | D3D8_FLIP_METHOD_TYPE_FLIP;",
        "    return (bits << 5);",
        "the low five bits carry the type 1.",
    ),
    # --- gating --------------------------------------------------------------------------
    mutation(
        "swap-needs-flip-model",
        FLIP,
        "    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {\n        return;",
        "    if (!d3d8_vblank_effects_enabled()) {\n        return;",
        "the flip model is its own opt-in: the coupled effects alone must not queue flips.",
    ),
    mutation(
        "swap-needs-coupled-effects",
        FLIP,
        "    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {\n        return;",
        "    if (!d3d8_flip_enabled()) {\n        return;",
        "a flip queued with no coupled wait to complete it never ends.",
    ),
    mutation(
        "swap-fatal-on-refusal",
        FLIP,
        "    if (refusal != NULL) {\n        d3d8_hle_fatal(FLIP_SOFTWARE_METHOD",
        "    if (refusal != NULL && false) {\n        d3d8_hle_fatal(FLIP_SOFTWARE_METHOD",
        "a refused Swap flip is a named stop, not a silent skip.",
    ),
    mutation(
        "swap-front-surface",
        FLIP,
        "d3d8_guest_load32(front + D3D8_SURFACE_DATA)",
        "d3d8_guest_load32(front + D3D8_SURFACE_LOCK)",
        "the flip shows the front buffer's data address.",
    ),
    mutation(
        "present-hook",
        PRESENT,
        "        d3d8_flip_queue_for_swap();\n",
        "",
        "the Swap's prepare is where the original writes the flip, the hook is that site.",
    ),
    # --- the helper effects ------------------------------------------------------------
    mutation(
        "effects-flip-flags",
        EFFECTS,
        "        if (flips != 0u) {\n            record.flags = 1u;",
        "        if (flips != 0u) {\n            record.flags = 2u;",
        "a processed flip gives flags 1.",
        [
            *T_FLIP,
            "test_d3d8_vblank_effects",
            ORACLE,
            "pytest:tests/test_d3d8_vblank_effects_oracle.py",
        ],
    ),
    mutation(
        "effects-threshold-after-flip",
        EFFECTS,
        "        } else {\n            const uint32_t threshold = d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD);",
        "        }\n        {\n            const uint32_t threshold = d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD);",
        "a processed flip skips the threshold update even when the count meets it.",
        [*T_FLIP, "test_d3d8_vblank_effects", "pytest:tests/test_d3d8_vblank_effects_oracle.py"],
    ),
    mutation(
        "effects-record-flip-index",
        EFFECTS,
        "        record.flip_index = d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX);\n        if (flips != 0u) {",
        "        if (flips != 0u) {",
        "the record carries the consumer index AFTER the flips (the original builds it at callback time).",
        ["test_d3d8_vblank_effects"],
    ),
    mutation(
        "effects-process-order",
        EFFECTS,
        "        const unsigned flips = d3d8_flip_process_locked();",
        "        const unsigned flips = 0u;",
        "the helper runs the flip processor between the count and the threshold.",
        [*T_FLIP, "test_d3d8_vblank_effects", "pytest:tests/test_d3d8_vblank_effects_oracle.py"],
    ),
    # --- the startup callback preflight ---------------------------------------------------
    mutation(
        "first-flip-index",
        FIRST,
        "        expected[2] = (uint32_t)d3d8_flip_hardware_get().flips;",
        "        expected[2] = 0u;",
        "under the flip model the consumer index is the number of flips processed.",
        ["test_d3d8_first_vblank"],
    ),
    mutation(
        "first-flip-producer-drained",
        FIRST,
        "            producer != expected[2]) {",
        "            (producer != expected[2] && producer == 0xFFFFFFFFu)) {",
        "a flip queued but not processed is not the state the startup event can be proven from.",
        ["test_d3d8_first_vblank"],
    ),
    mutation(
        "first-flip-threshold-free",
        FIRST,
        "        if (modelled_flips && i == 3u) {\n            continue;\n        }",
        "",
        "the flip queue leaves a nonzero threshold, which the preflight must accept under the model only.",
        ["test_d3d8_first_vblank"],
    ),
    mutation(
        "first-flip-threshold-update",
        FIRST,
        "       (hits_threshold && !kernel_guest_write_u32(DEVICE+0x1DECu,actual[3]+1u))) {",
        "       (false && hits_threshold && !kernel_guest_write_u32(DEVICE+0x1DECu,actual[3]+1u))) {",
        "the helper updates the threshold when the startup event's count meets it.",
        ["test_d3d8_first_vblank"],
    ),
    mutation(
        "first-flip-model-gate",
        FIRST,
        "    const bool modelled_flips = d3d8_flip_enabled() && d3d8_vblank_effects_enabled();",
        "    const bool modelled_flips = d3d8_flip_enabled();",
        "the relaxed preflight applies only when the flip model AND the coupled effects are on.",
        ["test_d3d8_first_vblank"],
    ),
    # --- the host option -------------------------------------------------------------------
    mutation(
        "option-needs-coupling",
        OPTIONS,
        "        (!out->model_flips || out->couple_vblank_effects) &&\n",
        "",
        "--model-flips without --couple-vblank-effects would queue flips nothing completes.",
        ["test_host_options"],
    ),
    mutation(
        "option-parse",
        OPTIONS,
        '        } else if (strcmp(arg, "--model-flips") == 0) {\n            out->model_flips = true;',
        '        } else if (strcmp(arg, "--model-flips") == 0) {\n            out->model_flips = false;',
        "the option turns the model on.",
        ["test_host_options"],
    ),
]
