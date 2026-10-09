# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic-only domain assembly for the published T1550 runtime plan.

No declarations are globally installed. No emulator/native construction on import.
"""

from __future__ import annotations

from dataclasses import replace
from itertools import product
from pathlib import Path

from tools.replace.manifest import ManifestEntry

from . import scoped_fixture_authority as authority
from . import t1550_runtime_fixtures as fixtures
from .effective_index import SelectedSources
from .image import GuestImage, SourceSection
from .model import Case
from .providers import Provider
from .scoped_fixture_code import build_certificate
from .scoped_fixture_domain import Domain
from .seeding import (
    GUEST_LO,
    GUEST_SPAN,
    SCRATCH_BASE,
    SeedPolicy,
    make_case,
    scratch_arena_bytes,
    seeded_guest_dwords,
)
from .t1525_scoped_controls import Paths, sources

LABEL = "t1550-synthetic-byte-table"


def count(va: int) -> int:
    return fixtures.PROVIDER_COUNT if va == 0x10000 else 0


def delegate(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    authority.supported_policy(policy or SeedPolicy())
    if (va, size) != (0x10000, 29):
        raise ValueError("T1550 frozen provider root/body mismatch")
    return fixtures.make_provider_case(seed, ordinal)


def dispatch(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    return delegate(seed, ordinal, va, size, policy=policy)


def independent_stream(seed: int) -> tuple[authority.ExpectedCase, ...]:
    values = product(
        (0, 1, 2, 3, 127, 128, 254, 255),
        (0xDC0040, 0, 0x1000000, 0xFFFFFFFF),
        (0, 1, 0x80000000, 0xFFFFFFFF),
    )
    expected = []
    for ordinal, (byte, pointer, payload) in enumerate(values):
        case = make_case(seed, (93 << 40) + ordinal, 0x10000, 29)
        patches = (
            (0x20000, bytes([byte])),
            (0x20004, pointer.to_bytes(4, "little")),
            (0x20010 + byte * 4, (0x6B000000 | ordinal).to_bytes(4, "little")),
            (0xDC0040, payload.to_bytes(4, "little")),
        )
        case = replace(case, patches=case.patches + patches)
        roles = (
            "baseline-frame",
            "baseline-scratch",
            "source-byte",
            "source-dword",
            "source-table",
            "guest-payload",
        )
        expected.append(authority.ExpectedCase.capture(case, roles))
    return tuple(expected)


def image() -> GuestImage:
    data = bytearray(GUEST_SPAN)
    programs = fixtures.PROGRAMS | fixtures.GUARD_PROGRAMS
    for address, raw in (*programs.items(), *fixtures.baseline_patches()):
        data[address - GUEST_LO : address - GUEST_LO + len(raw)] = raw
    scratch = scratch_arena_bytes(SeedPolicy())
    data[SCRATCH_BASE - GUEST_LO : SCRATCH_BASE - GUEST_LO + len(scratch)] = scratch
    for address, value in seeded_guest_dwords().items():
        data[address - GUEST_LO : address - GUEST_LO + 4] = value.to_bytes(4, "little")
    sections = tuple(
        SourceSection(at, len(raw), True, False) for at, raw in sorted(programs.items())
    )
    return GuestImage(
        bytes(data), GUEST_LO, sections + (SourceSection(0x20000, 0x1000, True, True),)
    )


def assemble(directory: Path, mode: str) -> tuple[GuestImage, Domain, Provider, Paths]:
    actual = image()
    programs = fixtures.PROGRAMS | fixtures.GUARD_PROGRAMS
    paths = sources(directory, mode, tuple((at, len(raw)) for at, raw in sorted(programs.items())))
    functions, overrides, additions = paths
    selected = (
        SelectedSources(functions, overrides, additions).effective if mode == "effective" else None
    )
    index = selected.table if selected is not None else functions.read_bytes()
    deps = authority.Dependencies.capture(("tools",))
    physical = authority.PhysicalPlan(
        authority.PhysicalSource.synthetic(actual, deps),
        index,
        (
            authority.Region("source-byte", "source", 0x20000, 0x20001, 1),
            authority.Region("source-dword", "source", 0x20004, 0x20010, 4),
            authority.Region("source-table", "source", 0x20010, 0x20410, 4),
            authority.Region("guest-payload", "guest", 0xDC0040, 0xDC0044, 4),
        ),
    )
    identities = tuple(
        authority.FactoryIdentity.capture(0x10000, LABEL, 93, fn, deps)
        for fn in (count, dispatch, delegate)
    )
    domain = Domain(
        ManifestEntry(0x10000, "custom_byte_table", "cdecl", 0, "eax", (), "synthetic.c"),
        29,
        LABEL,
        93,
        identities[0],
        identities[1],
        (identities[2],),
        tuple(
            authority.FrozenStream(seed, 0x10000, 93, independent_stream(seed))
            for seed in fixtures.SEEDS
        ),
        physical,
        build_certificate(actual, index, 0x10000, effective_source=selected),
    )
    provider = Provider(93, (0x10000,), count, dispatch, LABEL, custom_contracts=(domain,))
    return actual, domain, provider, paths


def count_micro(va: int) -> int:
    return 12 if va in (0x10080, 0x100F0, *fixtures.GUARD_PROGRAMS) else 0


def delegate_micro(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    authority.supported_policy(policy or SeedPolicy())
    programs = fixtures.PROGRAMS | fixtures.GUARD_PROGRAMS
    if not count_micro(va) or size != len(programs[va]) or not 0 <= ordinal < 12:
        raise ValueError("unsupported frozen microfixture")
    return make_case(seed, (93 << 40) + ordinal, va, size)


def dispatch_micro(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    return delegate_micro(seed, ordinal, va, size, policy=policy)


def micro_domain(base: Domain, va: int) -> tuple[Domain, Provider]:
    programs = fixtures.PROGRAMS | fixtures.GUARD_PROGRAMS
    if not count_micro(va):
        raise ValueError("unknown frozen micro root")
    size = len(programs[va])
    deps = base.physical.source.dependencies
    label = f"t1550-micro-{va:x}"
    identities = tuple(
        authority.FactoryIdentity.capture(va, label, 93, fn, deps)
        for fn in (count_micro, dispatch_micro, delegate_micro)
    )
    streams = tuple(
        authority.FrozenStream(
            seed,
            va,
            93,
            tuple(
                authority.ExpectedCase.capture(
                    make_case(seed, (93 << 40) + ordinal, va, size),
                    ("baseline-frame", "baseline-scratch"),
                )
                for ordinal in range(12)
            ),
        )
        for seed in fixtures.SEEDS
    )
    entry = ManifestEntry(
        va,
        f"synthetic_micro_{va:x}",
        "stdcall" if va == 0x100F0 else "cdecl",
        2 if va == 0x100F0 else 0,
        "eax",
        (),
        "synthetic.c",
    )
    certificate = build_certificate(
        base.physical.source.image,
        base.physical.index,
        va,
        effective_source=base.certificate.effective_source,
    )
    domain = Domain(
        entry,
        size,
        label,
        93,
        identities[0],
        identities[1],
        (identities[2],),
        streams,
        base.physical,
        certificate,
    )
    return domain, Provider(
        93, (va,), count_micro, dispatch_micro, label, custom_contracts=(domain,)
    )
