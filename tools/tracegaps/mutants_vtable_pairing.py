# SPDX-License-Identifier: GPL-3.0-or-later
"""T728 mutants for the explicit caller/writer/value gates in vtable_pairing.py."""

SUITE = "tests/test_vtable_pairing.py"
FILE = "vtable_pairing.py"

MUTANTS: list[tuple[str, str, str, str, str, bool, str]] = [
    (
        "vtable-pairing-drops-caller-completeness",
        FILE,
        "if not callers_complete or not inputs:",
        "if False:",
        "a missing selector caller no longer blocks narrowing",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-drops-writer-completeness",
        FILE,
        "if not writers_complete:",
        "if False:",
        "an unaccounted global writer no longer blocks narrowing",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-ignores-open-input",
        FILE,
        "if item.open_reasons:",
        "if False:",
        "an unbounded parsed field is treated as a finite input set",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-drops-index-upper-bound",
        FILE,
        "if value > max_index:",
        "if False:",
        "an index above the record-table bound is accepted",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-drops-method-displacement",
        FILE,
        "table_base + stride * index + method_disp",
        "table_base + stride * index",
        "the record's method slot is paired with its object header",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-negative-index-not-cleared",
        FILE,
        "if value < 0:\n                # The verified setter clears the global pointer for signed-negative input.\n                continue",  # noqa: E501
        "if False:\n                # The verified setter clears the global pointer for signed-negative input.\n                continue",  # noqa: E501
        "the selector's negative clear sentinel becomes a table index",
        False,
        SUITE,
    ),
    (
        "vtable-pairing-drops-reference-completeness",
        FILE,
        "if not references_complete:",
        "if False:",
        "an indirect selector caller no longer blocks narrowing",
        False,
        SUITE,
    ),
]
