# SPDX-License-Identifier: GPL-3.0-or-later
"""Inactive exact-ECX object authority; no execution or capability on import."""

from __future__ import annotations

from dataclasses import replace
from itertools import product

from tools.replace.manifest import ManifestEntry

from . import scoped_fixture_authority as authority
from .model import Case
from .providers import Provider
from .scoped_fixture_code import CodeOnlyCertificate
from .scoped_fixture_domain import Domain
from .scoped_fixture_prefix import PrefixAuthority
from .seeding import SCRATCH_BASE, SeedPolicy, make_case
from .t1559_this_object_cases import (
    LABEL,
    NAMESPACE,
    ROOT_SIZES,
    make_this_object_case,
    this_object_case_count,
)

SEEDS = (20261001, 20261006)
# Independent declared allocation/stream axes, not read back from factory constants.
BASES = tuple(
    SCRATCH_BASE + offset
    for offset in (0x2000, 0x3001, 0x4002, 0x5003, 0x6000, 0x7001, 0x8002, 0x9003)
)
FILLS = (0, 1, 0x55, 0x7F, 0x80, 0xAA, 0xFE, 0xFF)
CLOSURES = {
    0x3BEB10: (
        (0x3BB890, 13, "1e09d34a9de7a48bd061703ef73f341df8596595468b282c5f19c36b6bf0b98d"),
        (0x3BEB10, 37, "e187837e32919dca3fa652d7c6380c0b6363d0b0bbdceaa46b4ebc0c0759514e"),
    ),
    0x3BEDF0: (
        (0x3BB650, 16, "9d20fb9737545d0eeb198c79dc5d325beabd6d570a4058d3f1e7f4a887df0403"),
        (0x3BEDF0, 51, "91af3bd89600de1b601a8ebea6f0c18ce20dba46adf288b177a3e5e65e129ad3"),
    ),
    0x3BF680: (
        (0x3BB650, 16, "9d20fb9737545d0eeb198c79dc5d325beabd6d570a4058d3f1e7f4a887df0403"),
        (0x3BF680, 66, "6d93e9088b391420b7221234e355f8a2d8660f5ddde14ed2557599e94a3ca84b"),
    ),
}
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"


def validate_entry(entry: ManifestEntry) -> None:
    if (
        type(entry) is not ManifestEntry
        or type(entry.stack_args) is not int
        or entry.va not in ROOT_SIZES
        or (entry.convention, entry.stack_args, entry.returns, entry.scratch, entry.register_inputs)
        != ("cdecl", 0, "eax", (), ("ecx",))
    ):
        raise ValueError("exact constructor ECX/cdecl0/eax/scratch0 ABI required")


def dispatch(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    return make_this_object_case(seed, ordinal, va, size, policy=policy)


def object_regions() -> tuple[authority.Region, ...]:
    return tuple(
        authority.Region(f"this-object-{index}", "guest", base - 16, base + 144, 8)
        for index, base in enumerate(BASES)
    )


def independent_stream(seed: int, va: int) -> tuple[authority.ExpectedCase, ...]:
    if type(seed) is not int or seed not in SEEDS or type(va) is not int or va not in ROOT_SIZES:
        raise ValueError("unsupported exact constructor seed/root")
    result = []
    for ordinal, (base, fill, df) in enumerate(product(BASES, FILLS, (0, 1))):
        original = make_case(seed, (NAMESPACE << 40) + ordinal, va, ROOT_SIZES[va])
        regs = (*original.regs[:1], base, *original.regs[2:])
        patches = tuple((at, bytes([fill]) * 8) for at in range(base - 16, base + 144, 8))
        case = replace(original, regs=regs, df=df, patches=original.patches + patches)
        roles = ("baseline-frame", "baseline-scratch") + (f"this-object-{ordinal // 16}",) * 20
        result.append(authority.ExpectedCase.capture(case, roles))
    return tuple(result)


def build_domain(
    entry: ManifestEntry,
    source: authority.PhysicalSource,
    index: bytes,
    certificate: CodeOnlyCertificate,
    known_code: PrefixAuthority,
) -> Domain:
    """Bind actual original authority; unavailable inputs refuse rather than self-sign."""
    validate_entry(entry)
    if (
        type(source) is not authority.PhysicalSource
        or source.kind != "original-xbe"
        or source.source_sha256 != XBE_SHA
    ):
        raise ValueError("authenticated actual original constructor image required")
    if type(certificate) is not CodeOnlyCertificate or certificate.root != entry.va:
        raise ValueError("actual complete constructor code certificate required")
    if type(known_code) is not PrefixAuthority or known_code.kind != "original-export-prefix-v1":
        raise ValueError("independent original known-code authority required")
    source.check()
    certificate.authenticate(source.image, index)
    actual = tuple(
        sorted((n.address, len(n.raw), authority.digest(n.raw)) for n in certificate.nodes)
    )
    if actual != CLOSURES[entry.va]:
        raise ValueError(
            "actual complete constructor closure differs from archived source contract"
        )
    physical = authority.PhysicalPlan(source, index, object_regions(), known_code=known_code)
    physical.validate()
    identities = tuple(
        authority.FactoryIdentity.capture(entry.va, LABEL, NAMESPACE, fn, source.dependencies)
        for fn in (this_object_case_count, dispatch, make_this_object_case)
    )
    result = Domain(
        entry,
        ROOT_SIZES[entry.va],
        LABEL,
        NAMESPACE,
        identities[0],
        identities[1],
        (identities[2],),
        tuple(
            authority.FrozenStream(seed, entry.va, NAMESPACE, independent_stream(seed, entry.va))
            for seed in SEEDS
        ),
        physical,
        certificate,
    )
    result.validate()
    return result


def inactive_provider(domains: tuple[Domain, ...]) -> Provider:
    if (
        type(domains) is not tuple
        or any(type(d) is not Domain for d in domains)
        or tuple(d.entry.va for d in domains) != tuple(ROOT_SIZES)
    ):
        raise ValueError("complete ordered three-constructor authority required")
    for domain in domains:
        if type(domain) is not Domain:
            raise ValueError("exact constructor Domain required")
        domain.validate()
    provider = Provider(
        NAMESPACE,
        tuple(ROOT_SIZES),
        this_object_case_count,
        dispatch,
        LABEL,
        after_feedback=True,
        custom_contracts=domains,
    )
    for domain in domains:
        for seed in SEEDS:
            domain.authenticate_provider(provider, seed)
    return provider
