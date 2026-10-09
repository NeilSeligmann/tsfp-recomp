# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for HAL shutdown state and firmware reporting (T468)."""

_T = ["test_kernel_hal"]
MUTATIONS = [
    {
        "id": "khal-register-flag-selects-add",
        "file": "src/xbox/kernel_hal.c",
        "old": "    if ((register_flag & 0xFFu) != 0u) {",
        "new": "    if ((register_flag & 0xFFu) == 0u) {",
        "targets": _T,
        "why": "A true BOOLEAN registers and false deregisters; reversing the branch loses the guest's registration and its shutdown notification.",  # noqa: E501
    },
    {
        "id": "khal-duplicate-count-once",
        "file": "src/xbox/kernel_hal.c",
        "old": "        duplicate_count++;",
        "new": "        duplicate_count += 2u;",
        "targets": _T,
        "why": "Duplicate registration is retained once and counted once; a doubled counter misreports the actual duplicate event.",  # noqa: E501
    },
    {
        "id": "khal-deregister-clears-slot",
        "file": "src/xbox/kernel_hal.c",
        "old": "    registrations[slot] = 0u;",
        "new": "    registrations[slot] = registration;",
        "targets": _T,
        "why": "Deregistering must release the slot so count and later re-registration reflect current guest state.",  # noqa: E501
    },
    {
        "id": "khal-null-registration-not-held",
        "file": "src/xbox/kernel_hal.c",
        "old": "    if (registration == 0u) {\n        return false;\n    }\n    lock();",
        "new": "    if (registration != 0u) {\n        return false;\n    }\n    lock();",
        "targets": _T,
        "why": "Address zero is explicitly not a valid registered guest pointer; the query must reject it.",  # noqa: E501
    },
    {
        "id": "khal-reset-preserves-firmware-sink",
        "file": "src/xbox/kernel_hal.c",
        "old": "    firmware_return_count = 0u;\n    last_firmware_routine = 0u;",
        "new": "    firmware_sink = NULL;\n    firmware_return_count = 0u;\n    last_firmware_routine = 0u;",  # noqa: E501
        "targets": _T,
        "why": "The sink is host wiring, so resetting guest counters must not detach it and let a reboot request continue.",  # noqa: E501
    },
    {
        "id": "khal-firmware-routine-preserved",
        "file": "src/xbox/kernel_hal.c",
        "old": "    last_firmware_routine = routine;",
        "new": "    last_firmware_routine = routine + 1u;",
        "targets": _T,
        "why": "The firmware routine argument determines the requested action and diagnostics must preserve its exact value.",  # noqa: E501
    },
    {
        "id": "khal-no-phy-status",
        "file": "src/xbox/kernel_hal.c",
        "old": "    return STATUS_NO_PHY_DEVICE;",
        "new": "    return 0u;",
        "targets": _T,
        "why": "The title checks the negative status to take its own network failure path; success falsely claims a missing device initialized.",  # noqa: E501
    },
]
