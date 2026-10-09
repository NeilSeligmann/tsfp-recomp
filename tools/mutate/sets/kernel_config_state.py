# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for nonvolatile setting query semantics (T468)."""

_T = ["test_kernel_config"]
MUTATIONS = [
    {
        "id": "kcfg-five-arguments-required",
        "file": "src/xbox/kernel_config.c",
        "old": "for (unsigned i = 0u; i < 5u; i++) {\n        if (!kernel_frame_arg(frame, i, &args[i])) {",  # noqa: E501
        "new": "for (unsigned i = 0u; i < 4u; i++) {\n        if (!kernel_frame_arg(frame, i, &args[i])) {",  # noqa: E501
        "targets": _T,
        "why": "The fifth ResultLength argument is optional by value but part of the call frame; reading only four accepts a truncated invocation and misuses the argument slots.",  # noqa: E501
    },
    {
        "id": "kcfg-buffer-small-refused",
        "file": "src/xbox/kernel_config.c",
        "old": "if (stored->length > value_length) {",
        "new": "if (stored->length < value_length) {",
        "targets": _T,
        "why": "A stored value larger than the guest buffer must be refused to prevent truncation; inverting this check accepts the unsafe size.",  # noqa: E501
    },
    {
        "id": "kcfg-fail-policy-refuses-unknown",
        "file": "src/xbox/kernel_config.c",
        "old": "} else if (stored_only || unknown_policy == KERNEL_CONFIG_UNKNOWN_FAIL) {",
        "new": "} else if (stored_only || unknown_policy != KERNEL_CONFIG_UNKNOWN_FAIL) {",
        "targets": _T,
        "why": "Under FAIL policy an unknown setting must be refused and must not fabricate zero bytes.",  # noqa: E501
    },
    {
        "id": "kcfg-fabricated-value-zero-filled",
        "file": "src/xbox/kernel_config.c",
        "old": "        const uint8_t byte = source ? source[i] : 0u;",
        "new": "        const uint8_t byte = source ? source[i] : 0xFFu;",
        "targets": _T,
        "why": "When no EEPROM value exists, the documented fabricated result is a full zero-filled buffer, not arbitrary all-ones data.",  # noqa: E501
    },
    {
        "id": "kcfg-type-is-dword-for-four-bytes",
        "file": "src/xbox/kernel_config.c",
        "old": "const uint32_t type = (produced == 4u) ? CONFIG_TYPE_DWORD : CONFIG_TYPE_BINARY;",
        "new": "const uint32_t type = (produced != 4u) ? CONFIG_TYPE_DWORD : CONFIG_TYPE_BINARY;",
        "targets": _T,
        "why": "Four produced bytes have DWORD type; all other stored lengths are binary.",
    },
    {
        "id": "kcfg-query-index-recorded",
        "file": "src/xbox/kernel_config.c",
        "old": "    record_query_locked(index);",
        "new": "    record_query_locked(index + 1u);",
        "targets": _T,
        "why": "The query audit records the requested index exactly, in first-seen order; shifting it makes the diagnostics lie about the queried setting.",  # noqa: E501
    },
]
