# SPDX-License-Identifier: GPL-3.0-or-later
"""T1476 predeclared 000AF330 table-lookup domain; namespace 41, additive to the seed stream."""

from dataclasses import dataclass, replace

from .model import Case
from .seeding import SeedPolicy, make_case

TABLE_LOOKUP_INDEX_BASE = 41 << 40
TABLE_LOOKUP_ROOTS = frozenset({0xAF330})
HOLDER = 0xD60000
STATE = 0xD60100
KEY = 0x1234ABC0
TABLE = 0x7B2780
ENTRY_STRIDE = 0x54
COUNT_ADDRESS = 0x732D48
CHAIN_E = 0xD60200
CHAIN_D = 0xD60300
FOUND_WEIGHT = 40
OTHER_WEIGHT = 4


@dataclass(frozen=True)
class TableLookupConfig:
    count: int
    found: int | None  # matching entry index, None for no match
    broken: str | None  # None, "entry8-null" or "chain4-bad"
    holder: int  # stack argument
    state_pointer: int  # [holder+0x7C]
    weight: int


def build_configs() -> tuple[TableLookupConfig, ...]:
    configs = []
    counts = (-1, 0, 1, 2, 3)
    for count in counts:
        configs.append(TableLookupConfig(count, None, None, HOLDER, STATE, OTHER_WEIGHT))
    founds = [(count, 0) for count in (1, 2, 3)] + [(count, count - 1) for count in (2, 3)]
    for count, index in founds:
        configs.append(TableLookupConfig(count, index, None, HOLDER, STATE, FOUND_WEIGHT))
    for count, index in founds:
        for broken in ("entry8-null", "chain4-bad"):
            configs.append(TableLookupConfig(count, index, broken, HOLDER, STATE, OTHER_WEIGHT))
    configs.append(TableLookupConfig(1, None, None, 0, STATE, OTHER_WEIGHT))
    configs.append(TableLookupConfig(1, None, None, HOLDER, 0, OTHER_WEIGHT))
    return tuple(configs)


CONFIGS = build_configs()
TABLE_LOOKUP_CASES = sum(config.weight for config in CONFIGS)


def table_lookup_case_count(va: int) -> int:
    return TABLE_LOOKUP_CASES if va in TABLE_LOOKUP_ROOTS else 0


def config_for(ordinal: int) -> TableLookupConfig:
    if not 0 <= ordinal < TABLE_LOOKUP_CASES:
        raise ValueError("unsupported T1476 table-lookup ordinal")
    for config in CONFIGS:
        if ordinal < config.weight:
            return config
        ordinal -= config.weight
    raise AssertionError("unreachable")


def word(value: int) -> bytes:
    return (value & 0xFFFFFFFF).to_bytes(4, "little")


def make_table_lookup_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not table_lookup_case_count(va):
        raise ValueError("unsupported T1476 table-lookup address")
    config = config_for(ordinal)
    case = make_case(seed, TABLE_LOOKUP_INDEX_BASE + ordinal, va, size, policy=policy)
    additions = [
        (case.esp + 4, word(config.holder)),
        (HOLDER + 0x7C, word(config.state_pointer)),
        (STATE + 0x60, word(KEY)),
        (STATE + 0x54, word(0xA5000000 + ordinal)),
        (COUNT_ADDRESS, word(config.count)),
    ]
    for index in range(max(0, min(config.count, 3))):
        entry = TABLE + ENTRY_STRIDE * index
        chain_e = CHAIN_E + 0x20 * index
        chain_d = CHAIN_D + 0x20 * index
        matching = index == config.found
        additions.append((entry, word(KEY if matching else KEY ^ 1)))
        additions.append((entry + 8, word(chain_e)))
        additions.append((chain_e + 4, word(chain_d)))
        additions.append((chain_d + 0x30, word(0x5A5A5A5A)))
        if matching and config.broken == "entry8-null":
            additions.append((entry + 8, word(0)))
        if matching and config.broken == "chain4-bad":
            additions.append((chain_e + 4, word(0xFFFFFFF0)))
    return replace(case, patches=(*case.patches, *additions))
