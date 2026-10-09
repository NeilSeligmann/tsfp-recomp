# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the flip-aware two-event and owner-wait preflights (T513).

Files: `src/host/recomp_second_vblank.c` (the proof of the drained flip queue and the record the
callback is given) and `src/gpu/d3d8_vblank_effects.c` (the record and input of the latest helper run
and the schedule trace suffix built from them).

TARGETS. The ctest binary `test_recomp_second_vblank_owner_waits` (the second event and the owner
waits with the flip model on, every flip refusal, and every refusal with the model off) kills the
policy mutants. `test_d3d8_vblank_effects` kills the input and trace mutants by exact string. The
boot comparison with the ORIGINAL helper (`tests/test_vblank_owner_waits_flips.py`) needs a rebuilt
`tsfp_host`, which this harness does not rebuild, so it is not a target here (it skips without the
disc and a skip is no kill). `tsfp_host` is no target either: it is not a test, any exit code of
it would count as a kill.

CONVENTION. `&& false` rather than a bare `false`, because `-Wunused-parameter -Werror` turns the
latter into NOT-A-MUTANT, which reads like evidence.
"""

SECOND = "src/host/recomp_second_vblank.c"
EFFECTS = "src/gpu/d3d8_vblank_effects.c"
MAIN = "src/host/main.c"
T_C = ["test_recomp_second_vblank_owner_waits"]
T_EFFECTS = ["test_d3d8_vblank_effects"]
OWNER_LOOP = "        uint32_t value;\n        if (modelled && i == 3u) continue;"
SECOND_LOOP = "            uint32_t value;\n            if (modelled && i == 3u) continue;"


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, targets: list[str] | None = None
) -> dict:
    return {
        "id": f"t513-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets if targets is not None else T_C,
        "why": why,
    }


MUTATIONS: list[dict] = [
    # --- when the proof is the drained queue instead of the measured zeros -----------------
    mutation(
        "model-needs-flip-option",
        SECOND,
        "{ return d3d8_flip_enabled() && d3d8_vblank_effects_enabled(); }",
        "{ return d3d8_vblank_effects_enabled(); }",
        "with --model-flips off the flip words would be accepted as the drained queue: the refusal loosens.",
    ),
    mutation(
        "model-needs-coupled-effects",
        SECOND,
        "{ return d3d8_flip_enabled() && d3d8_vblank_effects_enabled(); }",
        "{ return d3d8_flip_enabled(); }",
        "without the coupled effects nothing completes a flip, the uncoupled constants must still be demanded.",
    ),
    mutation(
        "model-never-active",
        SECOND,
        "{ return d3d8_flip_enabled() && d3d8_vblank_effects_enabled(); }",
        "{ return d3d8_flip_enabled() && d3d8_vblank_effects_enabled() && false; }",
        "the flip model would still stop at the old constants.",
    ),
    mutation(
        "consumer-expected-zero-under-model",
        SECOND,
        "{ return modelled ? (uint32_t)d3d8_flip_hardware_get().flips : 0u; }",
        "{ (void)modelled; return 0u; }",
        "the consumer index is demanded to be zero although flips were processed.",
    ),
    mutation(
        "consumer-expected-flips-without-model",
        SECOND,
        "{ return modelled ? (uint32_t)d3d8_flip_hardware_get().flips : 0u; }",
        "{ (void)modelled; return (uint32_t)d3d8_flip_hardware_get().flips; }",
        "with the model off the consumer index is compared with a stale flips record instead of zero.",
    ),
    mutation(
        "owner-consumer-not-compared",
        SECOND,
        "const uint32_t expected[] = {0x22020u,count,flip_expected_consumer(modelled),0u,",
        "const uint32_t expected[] = {0x22020u,count,0u,0u,",
        "the owner-wait preflight demands a zero consumer index (refuses every modelled flip) or, with the model off, the same.",
    ),
    mutation(
        "second-consumer-not-compared",
        SECOND,
        "const uint32_t expected[] = {0x22020u,second_count,flip_expected_consumer(modelled),0u,",
        "const uint32_t expected[] = {0x22020u,second_count,0u,0u,",
        "the two-event preflight demands a zero consumer index and refuses every modelled flip.",
    ),
    mutation(
        "owner-threshold-always-free",
        SECOND,
        OWNER_LOOP,
        "        uint32_t value;\n        if (i == 3u) continue;",
        "the owner-wait threshold is skipped with the flip model off too (the measured zero is no longer demanded).",
        [*T_C, "test_recomp_second_vblank", "test_recomp_second_vblank_coupled"],
    ),
    mutation(
        "second-threshold-always-free",
        SECOND,
        SECOND_LOOP,
        "            uint32_t value;\n            if (i == 3u) continue;",
        "the two-event threshold is skipped with the flip model off too.",
        [*T_C, "test_recomp_second_vblank", "test_recomp_second_vblank_coupled"],
    ),
    mutation(
        "owner-threshold-never-free",
        SECOND,
        OWNER_LOOP,
        "        uint32_t value;\n        if (false && modelled && i == 3u) continue;",
        "the threshold the queue left is compared with zero, every modelled flip refuses at the owner wait.",
    ),
    mutation(
        "second-threshold-never-free",
        SECOND,
        SECOND_LOOP,
        "            uint32_t value;\n            if (false && modelled && i == 3u) continue;",
        "the threshold the queue left is compared with zero, every modelled flip refuses at the second event.",
    ),
    mutation(
        "owner-modelled-false",
        SECOND,
        "const bool modelled = flip_modelled();\n    const uint32_t expected[] = {0x22020u,count",
        "const bool modelled = false;\n    const uint32_t expected[] = {0x22020u,count",
        "the owner-wait preflight never takes the flip model.",
    ),
    mutation(
        "second-modelled-false",
        SECOND,
        "const bool modelled = flip_modelled();\n        const uint32_t expected[] = {0x22020u,second_count",
        "const bool modelled = false;\n        const uint32_t expected[] = {0x22020u,second_count",
        "the two-event preflight never takes the flip model.",
    ),
    # --- the proof of the drained queue ----------------------------------------------------
    mutation(
        "producer-index-not-compared",
        SECOND,
        "if (producer_index != (uint32_t)hardware.flips || hardware.queued != hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "if (hardware.queued != hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "a flip queued and not completed (producer index ahead of the consumer) passes the preflight.",
    ),
    mutation(
        "queued-not-compared-with-flips",
        SECOND,
        "if (producer_index != (uint32_t)hardware.flips || hardware.queued != hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "if (producer_index != (uint32_t)hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "the model's own count of queued flips is not required to equal the flips processed.",
    ),
    mutation(
        "queue-never-refuses",
        SECOND,
        "if (producer_index != (uint32_t)hardware.flips || hardware.queued != hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "if (false && producer_index != (uint32_t)hardware.flips) return FLIP_REASON_NOT_DRAINED;",
        "no drained-queue proof at all.",
    ),
    mutation(
        "phantom-threshold-accepted",
        SECOND,
        "if (hardware.queued == 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "if (false && hardware.queued == 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "a threshold nothing queued set is taken as what the queue left (the model on, no flip, a stray word).",
    ),
    mutation(
        "phantom-threshold-needs-queued-flip",
        SECOND,
        "if (hardware.queued == 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "if (hardware.queued != 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "the stray-threshold rule applies after a queue instead of before it.",
    ),
    mutation(
        "phantom-threshold-zero-refused",
        SECOND,
        "if (hardware.queued == 0u && threshold != 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "if (hardware.queued == 0u && threshold == 0u) return FLIP_REASON_PHANTOM_THRESHOLD;",
        "the model on with no flip refuses the measured empty state.",
    ),
    mutation(
        "record-count-not-compared",
        SECOND,
        "if (record->count != count || record->flip_index != producer_index ||",
        "if (record->flip_index != producer_index ||",
        "a record of another blank (its count is not this blank's) is delivered.",
    ),
    mutation(
        "record-index-not-compared",
        SECOND,
        "if (record->count != count || record->flip_index != producer_index ||",
        "if (record->count != count ||",
        "a flip processed after the blank that built the record gives a callback record with a stale consumer index.",
    ),
    mutation(
        "record-flags-threshold-ignored",
        SECOND,
        "(record->flags == 2u && threshold != count + 1u))",
        "(record->flags == 2u && false))",
        "flags 2 (the threshold was hit) with a threshold the helper did not bump is delivered.",
    ),
    mutation(
        "record-flags-threshold-off-by-one",
        SECOND,
        "(record->flags == 2u && threshold != count + 1u))",
        "(record->flags == 2u && threshold != count))",
        "the helper bumps the threshold to count + 1 when it is hit, not to count.",
    ),
    mutation(
        "owner-flip-refusal-ignored",
        SECOND,
        "        if (flip_error >= 0) refuse(text->flips[flip_error]);",
        "        if (flip_error >= 0 && false) refuse(text->flips[flip_error]);",
        "the owner wait delivers with a flip state the proof refused.",
    ),
    mutation(
        "second-flip-refusal-ignored",
        SECOND,
        "            if (flip_error >= 0) refuse(second_flip_reasons[flip_error]);",
        "            if (flip_error >= 0 && false) refuse(second_flip_reasons[flip_error]);",
        "the second event delivers with a flip state the proof refused.",
    ),
    mutation(
        "owner-reason-text-swapped",
        SECOND,
        'static const char *const owner_flip_reasons[FLIP_REASONS] = {\n    "owner-wait flip words unreadable", "owner-wait flip queue not drained",',
        'static const char *const owner_flip_reasons[FLIP_REASONS] = {\n    "owner-wait flip words unreadable", "owner-wait flip queue drained",',
        "the named refusal says the opposite of what it found.",
    ),
    # --- what the callback is given ------------------------------------------------------
    mutation(
        "owner-payload-flags-dropped",
        SECOND,
        "const uint32_t payload[3] = {record.count,record.flip_index,record.flags};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "const uint32_t payload[3] = {record.count,record.flip_index,0u};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "the owner-wait callback is told no flip was processed and no threshold was hit.",
    ),
    mutation(
        "owner-payload-count-from-device-not-record",
        SECOND,
        "const uint32_t payload[3] = {record.count,record.flip_index,record.flags};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "const uint32_t payload[3] = {count + 1u,record.flip_index,record.flags};\n    if (!invoke(0x22020u,stack_low,stack_high,payload)) refuse(text->invoke",
        "the callback record carries a count that is not the device's.",
    ),
    mutation(
        "second-payload-index-dropped",
        SECOND,
        "const uint32_t payload[3] = {published_count,record.flip_index,record.flags};",
        "const uint32_t payload[3] = {published_count,0u,record.flags};",
        "the credited second event is told the consumer index is zero.",
    ),
    mutation(
        "second-payload-flags-dropped",
        SECOND,
        "const uint32_t payload[3] = {published_count,record.flip_index,record.flags};",
        "const uint32_t payload[3] = {published_count,record.flip_index,0u};",
        "the credited second event is told no flip was processed.",
    ),
    # --- the record and input of the latest helper run -----------------------------------
    mutation(
        "effects-record-not-kept",
        EFFECTS,
        "        applied++;\n        last_record = record;\n",
        "        applied++;\n",
        "the callback is given an empty record: count 0 against the device's.",
    ),
    mutation(
        "effects-record-kept-before-flips",
        EFFECTS,
        "        applied++;\n        last_record = record;\n",
        "        applied++;\n        last_record = (d3d8_vblank_record){count, 0u, 0u};\n",
        "the record keeps no consumer index and no flags (it was built before the flips ran).",
    ),
    mutation(
        "effects-input-consumer-is-producer",
        EFFECTS,
        "d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX), d3d8_device_load32(D3D8_VBLANK_DEV_COUNT),\n            d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD),",
        "d3d8_device_load32(D3D8_FLIP_DEV_PRODUCER), d3d8_device_load32(D3D8_VBLANK_DEV_COUNT),\n            d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD),",
        "the traced input names the producer index as the consumer, the oracle then flips the wrong slot.",
        T_EFFECTS,
    ),
    mutation(
        "effects-input-count-after",
        EFFECTS,
        "d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX), d3d8_device_load32(D3D8_VBLANK_DEV_COUNT),\n            d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD),",
        "d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX), d3d8_device_load32(D3D8_VBLANK_DEV_COUNT) + 1u,\n            d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD),",
        "the traced input count is the count after the helper, so the oracle runs one blank late.",
        T_EFFECTS,
    ),
    mutation(
        "effects-input-slot-address",
        EFFECTS,
        "d3d8_device_load32(D3D8_FLIP_DEV_SLOT0 + 8u), d3d8_device_load32(D3D8_FLIP_DEV_SLOT1),",
        "d3d8_device_load32(D3D8_FLIP_DEV_SLOT0 + 4u), d3d8_device_load32(D3D8_FLIP_DEV_SLOT1),",
        "the traced slot address is its target, so the display global the oracle writes is not the host's.",
        T_EFFECTS,
    ),
    # --- the schedule trace suffix (it seeds the oracle comparison in the pytest) ------------------
    mutation(
        "trace-flags-dropped",
        EFFECTS,
        "                   threshold, record.flags);",
        "                   threshold, 0u);",
        "the traced record never says a flip was processed, so the comparison with the original loses the flags.",
        T_EFFECTS,
    ),
    mutation(
        "trace-threshold-is-input",
        EFFECTS,
        "                   threshold, record.flags);",
        "                   (threshold & 0u) + in.threshold, record.flags);",
        "the traced threshold after the helper is the one before it.",
        T_EFFECTS,
    ),
    mutation(
        "trace-without-flip-model",
        EFFECTS,
        "    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "    if (false && (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled())) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "every schedule line of a default boot gains the flip words, the T183 records change.",
        T_EFFECTS,
    ),
    mutation(
        "trace-needs-only-the-flip-model",
        EFFECTS,
        "    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "    if (!d3d8_flip_enabled()) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "the trace is printed with the flip model on and the effects off, where nothing ran the helper.",
        T_EFFECTS,
    ),
    mutation(
        "trace-needs-only-the-effects",
        EFFECTS,
        "    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "    if (!d3d8_vblank_effects_enabled()) {\n        return;\n    }\n    d3d8_flip_lock();\n    const d3d8_vblank_input in",
        "the trace is printed for every coupled boot, the T372 schedule records change.",
        T_EFFECTS,
    ),
    mutation(
        "trace-gamma-words-swapped",
        EFFECTS,
        "in.slot[5], in.gamma_pending[0], in.gamma_pending[1], record.count, record.flip_index,",
        "in.slot[5], in.gamma_pending[1], in.gamma_pending[0], record.count, record.flip_index,",
        "the two gamma pending words of the seeded oracle state are swapped.",
        T_EFFECTS,
    ),
    mutation(
        "input-not-kept-before-the-run",
        EFFECTS,
        "        last_input = (d3d8_vblank_input){",
        "        if (false) last_input = (d3d8_vblank_input){",
        "the input of the helper run is never recorded.",
        T_EFFECTS,
    ),
]
