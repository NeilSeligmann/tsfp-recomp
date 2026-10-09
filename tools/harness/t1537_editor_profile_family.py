# SPDX-License-Identifier: GPL-3.0-or-later
"""Single-row namespace77 delegation; preserves the frozen original factory."""

from .model import Case
from .seeding import SeedPolicy
from .t1479_profile_list_cases import make_profile_list_case, profile_list_case_count
from .t1537_editor_signed_table_cases import (
    editor_signed_table_case_count,
    make_editor_signed_table_case,
)

SUPPORTED_VAS = (0x2FE4E0, 0x2FE520)


def editor_profile_family_count(va: int) -> int:
    return profile_list_case_count(va) + editor_signed_table_case_count(va)


def make_editor_profile_family_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    make = make_profile_list_case if va == 0x2FE520 else make_editor_signed_table_case
    return make(seed, ordinal, va, size, policy=policy)
