# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the opt-in synthetic pad adapter in `src/input/xinput_devices.c` (T717).

Kill suite: ctest `test_xinput_pad` (plain build, no XBE). The XBE-gated differential test
`tests/test_xinput_pad_oracle.py` compares the same code with the original bytes but is not a kill target.
Every `old` occurs exactly once (`tests/test_mutation_anchors.py`).

Families: policy (default off, refusal outside port 0, exactly-once insertion), open shapes (every unmeasured
argument refused), handle and buffer refusals, measured byte contracts (capabilities, state layout, analog
threshold, raw packet, feedback status, rumble report), T723 NULL opens and the removal path.

JUSTIFIED EQUIVALENT (not in the set): (T731) `report_changes[port] = 0u` in `xinput_hle_attach_synthetic_pad` and
`memset(&pad_snapshot, 0, ...)` in `xinput_pad_insert`: the packet number is the difference to the count taken at
the Open and the snapshot is only read while removed and rewritten at every sample, so neither is observable.
Also: replacing `pad_removed ? pad_snapshot : xinput_hle_pad_state(0u)` by the
plain port read survives, because `xinput_hle_detach_synthetic_pad` keeps the port's pad record and nothing can
write it once the port is empty (`xinput_hle_set_synthetic_pad_state` needs a synthetic port). The snapshot is
defence against the HLE ever clearing that record, not a distinguishable behaviour today.
"""

DEV = "src/input/xinput_devices.c"
HLE = "src/input/xinput_hle.c"
OPTIONS = "src/host/host_options.c"
SUITE = ["test_xinput_pad"]


def _m(
    mutation_id: str, old: str, new: str, why: str, file: str = DEV, suite: list[str] | None = None
) -> dict:
    return {
        "id": f"xin-pad-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(suite or SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        "mode-ignores-port",
        "return xinput_hle_connected_count() == 1u && xinput_hle_port_state(0u) == XINPUT_PORT_SYNTHETIC;",
        "return xinput_hle_connected_count() >= 1u;",
        "pad mode must mean exactly port 0 synthetic, a pad on another port is a shape nobody measured.",
    ),
    _m(
        "mode-ignores-enable",
        "if (!pad_enabled) return false;",
        "if (false) return false;",
        "the flag is the opt-in: an attached HLE pad alone must still be refused by the adapter.",
    ),
    _m(
        "insert-changed-flag",
        "const uint32_t inserted[3] = {mask, mask, 0u};",
        "const uint32_t inserted[3] = {mask, 0u, 0u};",
        "without the changed word GetDeviceChanges never reports the insertion.",
    ),
    _m(
        "insert-previous",
        "const uint32_t inserted[3] = {mask, mask, 0u};",
        "const uint32_t inserted[3] = {mask, mask, mask};",
        "a previous mask already holding the pad turns the insertion into no change.",
    ),
    _m(
        "insert-mask",
        "const uint32_t inserted[3] = {mask, mask, 0u};",
        "const uint32_t inserted[3] = {mask << 1u, mask, 0u};",
        "bit 0 is logical port 0.",
    ),
    _m(
        "open-type-unknown",
        'if (!known_type(type)) refuse(OPEN_ENTRY, "unknown device-type table");\n    if (type != GAMEPAD_TYPE || port != 0u || slot == 1u || pad_open || pad_removed) {',
        'if (!known_type(type) && false) refuse(OPEN_ENTRY, "unknown device-type table");\n    if (type != GAMEPAD_TYPE || port != 0u || slot == 1u || pad_open || pad_removed) {',
        "an unmeasured table pointer must still be refused.",
    ),
    _m(
        "open-type-gamepad-only",
        "if (type != GAMEPAD_TYPE || port != 0u",
        "if (port != 0u",
        "a known non-gamepad table opens nothing (measured NULL).",
    ),
    _m(
        "open-port-any",
        "if (type != GAMEPAD_TYPE || port != 0u || slot == 1u",
        "if (type != GAMEPAD_TYPE || port == 0xFFFFFFFEu || slot == 1u",
        "an absent port opens nothing (measured NULL).",
    ),
    _m(
        "open-port-range",
        "if (type != GAMEPAD_TYPE || port != 0u || slot == 1u",
        "if (type != GAMEPAD_TYPE || port > 3u || slot == 1u",
        "ports 4 and 0xFFFFFFFF are absent too.",
    ),
    _m(
        "open-slot-one",
        "|| port != 0u || slot == 1u ||",
        "|| port != 0u || slot == 0xFFFFFFFEu ||",
        "slot 1 opens nothing (measured NULL).",
    ),
    _m(
        "open-slot-nonzero",
        "|| port != 0u || slot == 1u ||",
        "|| port != 0u || slot != 0u ||",
        "only slot 1 selects another slot, the other values open like slot 0 (measured).",
    ),
    _m(
        "open-polling",
        'if (polling != 0u) refuse(OPEN_ENTRY, "polling parameters with an Open that returns NULL are not recovered");',
        'if (polling == 0xFFFFFFFFu) refuse(OPEN_ENTRY, "polling parameters with an Open that returns NULL are not recovered");',
        "polling parameters are not recovered.",
    ),
    _m(
        "open-double",
        "slot == 1u || pad_open || pad_removed) {",
        "slot == 1u || pad_removed) {",
        "a second open returns NULL (measured), not the same handle again.",
    ),
    _m(
        "open-removed",
        "slot == 1u || pad_open || pad_removed) {",
        "slot == 1u || pad_open) {",
        "a removed pad opens nothing (measured NULL).",
    ),
    _m(
        "remove-table-current",
        "    state[0] = 0u;\n    state[1] = 1u;",
        "    state[1] = 1u;",
        "removal clears the current mask.",
    ),
    _m(
        "remove-table-changed",
        "    state[0] = 0u;\n    state[1] = 1u;",
        "    state[0] = 0u;",
        "removal raises the changed mask so GetDeviceChanges reports it.",
    ),
    _m(
        "remove-table-previous",
        "    state[0] = 0u;\n    state[1] = 1u;",
        "    state[0] = 0u;\n    state[1] = 1u;\n    state[2] = 0u;",
        "previous keeps the pad, or the removal is never reported.",
    ),
    _m(
        "remove-detach",
        "(void)xinput_hle_detach_synthetic_pad(0u);",
        "(void)0;",
        "the HLE port goes empty on removal.",
    ),
    _m(
        "remove-twice",
        'if (pad_removed) refuse(OPEN_ENTRY, "removing an already removed pad");',
        "(void)0;",
        "a second removal is refused.",
    ),
    _m(
        "remove-pending",
        "if (pad_pending_feedback != 0u) refuse(OPEN_ENTRY,",
        "if (false) refuse(OPEN_ENTRY,",
        "removal with a transfer in flight depends on a clock this adapter lacks, refused.",
    ),
    _m(
        "remove-no-sample",
        "    sample_pad();\n    uint32_t state[3];",
        "    uint32_t state[3];",
        "the last report is sampled at removal.",
    ),
    _m(
        "removed-state-status",
        "const uint32_t result = pad_removed ? ERROR_DEVICE_NOT_CONNECTED : 0u;",
        "const uint32_t result = 0u;",
        "GetState on a removed pad fails with 0x48F.",
    ),
    _m(
        "removed-caps-status",
        "        at[1] = 0u; at[2] = 0u;\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "        at[1] = 0u; at[2] = 0u;\n        return 0u;",
        "GetCapabilities on a removed pad fails with 0x48F.",
    ),
    _m(
        "removed-caps-bytes",
        "        at[1] = 0u; at[2] = 0u;\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "        at[0] = 0u; at[1] = 0u; at[2] = 0u;\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "a removed pad writes bytes 1 and 2 only, byte 0 is the caller's.",
    ),
    _m(
        "removed-feedback-status",
        "        const uint32_t failed = ERROR_DEVICE_NOT_CONNECTED;",
        "        const uint32_t failed = ERROR_IO_PENDING;",
        "the header status of a removed pad is 0x48F.",
    ),
    _m(
        "removed-feedback-return",
        "        memcpy(at, &failed, 4u);\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "        memcpy(at, &failed, 4u);\n        return ERROR_IO_PENDING;",
        "SetState on a removed pad fails with 0x48F.",
    ),
    _m(
        "removed-feedback-rumble",
        "        memcpy(at, &failed, 4u);\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "        memcpy(at, &failed, 4u);\n        pad_output_count++;\n        return ERROR_DEVICE_NOT_CONNECTED;",
        "no output report is sent to a removed pad.",
    ),
    _m(
        "reset-keeps-removed",
        "pad_open = false; pad_removed = false;",
        "pad_open = false;",
        "a fresh session must not inherit the removal.",
    ),
    _m(
        "handle-identity",
        "if (!pad_open || handle != PAD_HANDLE) refuse(entry,",
        "if (!pad_open || (handle != PAD_HANDLE && false)) refuse(entry,",
        "any value must not pass as the handle.",
    ),
    _m(
        "handle-closed",
        "if (!pad_open || handle != PAD_HANDLE) refuse(entry,",
        "if (handle != PAD_HANDLE) refuse(entry,",
        "use after close must be refused.",
    ),
    _m(
        "close-keeps-open",
        "    require_open(CLOSE_ENTRY, handle);\n    pad_open = false;",
        "    require_open(CLOSE_ENTRY, handle);\n    pad_open = true;",
        "Close must end the session of the handle.",
    ),
    _m(
        "caps-subtype",
        "at[0] = 1u; at[1] = 0u; at[2] = 0u;\n    memset(at + 3u, 0xFF, 22u);",
        "at[0] = 2u; at[1] = 0u; at[2] = 0u;\n    memset(at + 3u, 0xFF, 22u);",
        "measured SubType 1.",
    ),
    _m(
        "caps-length",
        "memset(at + 3u, 0xFF, 22u);\n    return 0u;\n}\nstatic uint32_t locked_xinput_pad_state_read(uint32_t handle, uint32_t out)",
        "memset(at + 3u, 0xFF, 23u);\n    return 0u;\n}\nstatic uint32_t locked_xinput_pad_state_read(uint32_t handle, uint32_t out)",
        "the capabilities are 25 bytes, the 26th is the caller's.",
    ),
    _m(
        "caps-bound",
        'uint8_t *at = kernel_guest_at(out, 25u);\n    if (at == NULL) refuse(CAPS_ENTRY, "capabilities output is not mapped for 25 bytes");\n    if (pad_removed) {',
        'uint8_t *at = kernel_guest_at(out, 1u);\n    if (at == NULL) refuse(CAPS_ENTRY, "capabilities output is not mapped for 25 bytes");\n    if (pad_removed) {',
        "the output must be mapped for the whole 25 bytes.",
    ),
    _m(
        "state-bound",
        'uint8_t *at = kernel_guest_at(out, 22u);\n    if (at == NULL) refuse(STATE_ENTRY, "state output is not mapped for 22 bytes");\n    complete_feedback(STATE_ENTRY);',
        'uint8_t *at = kernel_guest_at(out, 4u);\n    if (at == NULL) refuse(STATE_ENTRY, "state output is not mapped for 22 bytes");\n    complete_feedback(STATE_ENTRY);',
        "the output must be mapped for the whole 22 bytes.",
    ),
    _m(
        "state-threshold",
        "now.analog[i] < XINPUT_PAD_ANALOG_THRESHOLD ? 0u",
        "now.analog[i] <= XINPUT_PAD_ANALOG_THRESHOLD ? 0u",
        "the measured threshold is exclusive below 0x20.",
    ),
    _m(
        "state-threshold-value",
        "#define XINPUT_PAD_ANALOG_THRESHOLD 0x20u",
        "#define XINPUT_PAD_ANALOG_THRESHOLD 0x1Fu",
        "the measured value is 0x20.",
    ),
    _m(
        "state-packet-first",
        "pad_packet = 1u + xinput_hle_synthetic_report_changes(0u) - pad_report_base;",
        "pad_packet = xinput_hle_synthetic_report_changes(0u) - pad_report_base;",
        "the first sample is packet 1 even for a pad at rest.",
    ),
    _m(
        "state-packet-always",
        "pad_packet = 1u + xinput_hle_synthetic_report_changes(0u) - pad_report_base;",
        "pad_packet += 1u + xinput_hle_synthetic_report_changes(0u) - pad_report_base;",
        "the packet number only advances when the report changes.",
    ),
    _m(
        "packet-base-dropped",
        "pad_packet = 1u + xinput_hle_synthetic_report_changes(0u) - pad_report_base;",
        "pad_packet = 1u + xinput_hle_synthetic_report_changes(0u);",
        "changes before the Open do not count (T731, measured).",
    ),
    _m(
        "packet-base-not-set",
        "pad_report_base = xinput_hle_synthetic_report_changes(0u);",
        "pad_report_base = 0u;",
        "every Open starts the packet number again at 1 (T731, measured).",
    ),
    _m(
        "hle-count-every-set",
        "if (memcmp(&before, &after, sizeof(before)) != 0) report_changes[port]++;",
        "report_changes[port]++;",
        "a set that repeats the report is not a report change.",
        file=HLE,
    ),
    _m(
        "hle-count-packet-field",
        "before.packet_number = after.packet_number = 0u;",
        "before.packet_number = 0u;",
        "the packet number field of the host state is not part of the raw report.",
        file=HLE,
    ),
    _m(
        "hle-count-never",
        "if (memcmp(&before, &after, sizeof(before)) != 0) report_changes[port]++;",
        "if (memcmp(&before, &after, sizeof(before)) != 0) report_changes[port] += 0u;",
        "an unpolled change still counts (T731, the old per-poll counting).",
        file=HLE,
    ),
    _m(
        "hle-attach-keeps-pad",
        "    memset(&pads[port], 0, sizeof(pads[port]));\n    report_changes[port] = 0u;",
        "    report_changes[port] = 0u;",
        "a reinserted pad is a new device at rest, not the old pad's last report.",
        file=HLE,
    ),
    _m(
        "polling-autopoll",
        'if ((block[0] & 1u) == 0u) refuse(OPEN_ENTRY, "polling without fAutoPoll is not recovered");',
        "",
        "fAutoPoll clear changes the original's transcript, so it stays refused.",
    ),
    _m(
        "polling-input-zero",
        "if (block[1] == 0u) refuse(OPEN_ENTRY,",
        "if (false) refuse(OPEN_ENTRY,",
        "input interval 0 does not return in the oracle.",
    ),
    _m(
        "polling-output-zero",
        "if ((block[0] & 2u) != 0u && block[2] == 0u) refuse(OPEN_ENTRY,",
        "if (false) refuse(OPEN_ENTRY,",
        "output interval 0 with fInterruptOut does not return in the oracle.",
    ),
    _m(
        "polling-output-zero-always",
        "if ((block[0] & 2u) != 0u && block[2] == 0u) refuse(OPEN_ENTRY,",
        "if (block[2] == 0u) refuse(OPEN_ENTRY,",
        "output interval 0 without fInterruptOut opens like the NULL block (measured).",
    ),
    _m(
        "polling-reserved-bits",
        "if ((block[0] & 0xFCu) != 0u || block[3] != 0u)",
        "if (block[3] != 0u)",
        "reserved flag bits are unmeasured and refused.",
    ),
    _m(
        "polling-reserved-byte",
        "if ((block[0] & 0xFCu) != 0u || block[3] != 0u)",
        "if ((block[0] & 0xFCu) != 0u)",
        "the reserved byte is unmeasured and refused.",
    ),
    _m(
        "polling-unmapped",
        'if (block == NULL) refuse(OPEN_ENTRY, "polling parameter block is not mapped for four bytes");',
        "if (block == NULL) return;",
        "an unmapped polling pointer is refused, not read.",
    ),
    _m(
        "polling-with-null-open",
        'if (polling != 0u) refuse(OPEN_ENTRY, "polling parameters with an Open that returns NULL are not recovered");',
        "",
        "polling with an Open that returns NULL is unmeasured and refused.",
    ),
    _m(
        "polling-ignored",
        "if (polling != 0u) check_polling(polling);\n    pad_open = true;",
        "if (polling != 0u && false) check_polling(polling);\n    pad_open = true;",
        "a polling block must be validated, not skipped.",
    ),
    _m(
        "insert-old-handle-open",
        'if (pad_open) refuse(OPEN_ENTRY, "reinsertion with the old handle still open is not recovered");',
        "",
        "reinsertion beside a still open old handle is unmeasured and refused.",
    ),
    _m(
        "insert-not-removed",
        'if (!pad_removed) refuse(OPEN_ENTRY, "inserting a pad that is not removed");',
        "",
        "inserting a pad that is present is refused.",
    ),
    _m(
        "insert-no-attach",
        "(void)xinput_hle_attach_synthetic_pad(0u);\n    uint32_t state[3];",
        "uint32_t state[3];",
        "the HLE port reports the new pad again.",
    ),
    _m(
        "insert-table-current",
        "    state[0] = 1u;\n    state[1] = 1u;\n    memcpy(table, state, sizeof(state));\n    pad_removed = false;",
        "    state[1] = 1u;\n    memcpy(table, state, sizeof(state));\n    pad_removed = false;",
        "reinsertion sets the current mask.",
    ),
    _m(
        "insert-table-changed",
        "    state[0] = 1u;\n    state[1] = 1u;\n    memcpy(table, state, sizeof(state));\n    pad_removed = false;",
        "    state[0] = 1u;\n    memcpy(table, state, sizeof(state));\n    pad_removed = false;",
        "reinsertion raises the changed mask so GetDeviceChanges reports it.",
    ),
    _m(
        "insert-stays-removed",
        "    memcpy(table, state, sizeof(state));\n    pad_removed = false;",
        "    memcpy(table, state, sizeof(state));",
        "the adapter leaves removed mode.",
    ),
    _m(
        "schedule-off-by-one",
        "++pad_polls == pad_remove_after_polls",
        "++pad_polls == pad_remove_after_polls + 1u",
        "the pad is unplugged after exactly the n-th poll.",
    ),
    _m(
        "schedule-never-removes",
        "xinput_pad_remove();\n    }\n    return result;",
        "}\n    return result;",
        "the scheduled removal calls the same removal as an explicit one.",
    ),
    _m(
        "schedule-before-read",
        "const uint32_t result = pad_removed ? ERROR_DEVICE_NOT_CONNECTED : 0u;",
        "const uint32_t result = pad_removed || (pad_remove_after_polls != 0u && pad_polls + 1u == pad_remove_after_polls) ? ERROR_DEVICE_NOT_CONNECTED : 0u;",
        "the n-th read still succeeds, the removal acts after it.",
    ),
    _m(
        "option-needs-pad",
        "(out->synthetic_pad_remove_after_polls == 0u || out->synthetic_pad) &&",
        "",
        "the removal source needs --synthetic-pad.",
        file=OPTIONS,
        suite=["test_host_options"],
    ),
    _m(
        "option-zero",
        "polls == 0u || polls > UINT32_MAX",
        "polls > UINT32_MAX",
        "zero polls is refused, never means unplug at once.",
        file=OPTIONS,
        suite=["test_host_options"],
    ),
    _m(
        "state-thumb-order",
        "memcpy(at + 14u, &now.thumb_left_x, 2u);\n    memcpy(at + 16u, &now.thumb_left_y, 2u);",
        "memcpy(at + 14u, &now.thumb_left_y, 2u);\n    memcpy(at + 16u, &now.thumb_left_x, 2u);",
        "per-field offsets, a swap inside the layout survives a size check.",
    ),
    _m(
        "state-buttons-offset",
        "memcpy(at + 4u, &now.digital_buttons, 2u);",
        "memcpy(at + 6u, &now.digital_buttons, 2u);",
        "digital buttons sit at 4.",
    ),
    _m(
        "feedback-event",
        'if (event != 0u) refuse(SETSTATE_ENTRY, "a notification event in the feedback header is not recovered");',
        'if (event == 0xFFFFFFFFu) refuse(SETSTATE_ENTRY, "a notification event in the feedback header is not recovered");',
        "an event in the header would need signalling that is not recovered.",
    ),
    _m(
        "feedback-return",
        "    pad_pending_feedback = feedback;\n    return ERROR_IO_PENDING;",
        "    pad_pending_feedback = feedback;\n    return 0u;",
        "SetState returns ERROR_IO_PENDING (997).",
    ),
    _m(
        "feedback-no-pending-status",
        "memcpy(at, &pending, 4u);",
        "(void)pending;",
        "the header status reads 0x3E5 until completion.",
    ),
    _m(
        "feedback-report-id",
        "pad_output[0] = 0u; pad_output[1] = 6u;",
        "pad_output[0] = 0u; pad_output[1] = 4u;",
        "the output report is id 0, length 6.",
    ),
    _m(
        "feedback-motor-offset",
        "memcpy(pad_output + 2u, at + 0x42u, 4u);",
        "memcpy(pad_output + 2u, at + 0x40u, 4u);",
        "motors sit at +0x42 and +0x44.",
    ),
    _m(
        "feedback-no-completion",
        "if (status == ERROR_IO_PENDING && !kernel_guest_write_u32(pad_pending_feedback, 0u))",
        "if (status == ERROR_IO_PENDING && !kernel_guest_write_u32(pad_pending_feedback, 1u))",
        "the status completes to 0.",
    ),
    _m(
        "reset-keeps-open",
        "ready = false; announced = false; pad_open = false;",
        "ready = false; announced = false;",
        "a fresh session must not inherit an open handle.",
    ),
    _m(
        "register-pad-close",
        "count += xinput_hle_register(CLOSE_ENTRY, close_handler) ? 1u : 0u;",
        "(void)close_handler;",
        "all five pad functions register.",
    ),
]
