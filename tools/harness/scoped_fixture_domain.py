# SPDX-License-Identifier: GPL-3.0-or-later
"""Immutable loaded-factory and independent complete custom-stream authority.

This source foundation supplies no CPU capability or provider adoption. Count,
primary dispatcher and every declared delegate are authenticated separately from
physical ownership and independent expected Case digests.
"""

from __future__ import annotations

import importlib
from dataclasses import asdict, dataclass

from tools.replace.manifest import ManifestEntry

from .model import Case
from .scoped_fixture_authority import (
    FactoryIdentity,
    FrozenStream,
    PhysicalPlan,
    canonical,
    digest,
    supported_policy,
)
from .scoped_fixture_code import CodeOnlyCertificate


@dataclass(frozen=True, slots=True)
class Domain:
    entry: ManifestEntry
    size: int
    label: str
    namespace: int
    count: FactoryIdentity
    dispatcher: FactoryIdentity
    delegates: tuple[FactoryIdentity, ...]
    streams: tuple[FrozenStream, ...]
    physical: PhysicalPlan
    certificate: CodeOnlyCertificate

    def validate(self) -> None:
        if (
            type(self) is not Domain
            or type(self.entry) is not ManifestEntry
            or type(self.size) is not int
            or self.size <= 0
            or type(self.label) is not str
            or not self.label
            or type(self.namespace) is not int
            or not 3 <= self.namespace < 1 << 16
            or self.namespace == 4
            or type(self.physical) is not PhysicalPlan
            or type(self.certificate) is not CodeOnlyCertificate
            or type(self.delegates) is not tuple
            or type(self.streams) is not tuple
            or not self.streams
        ):
            raise ValueError("immutable complete custom domain required")
        supported_policy(self.physical.policy)
        self.physical.validate()
        self.certificate.authenticate(self.physical.source.image, self.physical.index)
        if self.certificate.root != self.entry.va or not any(
            node.address == self.entry.va and len(node.raw) == self.size
            for node in self.certificate.nodes
        ):
            raise ValueError("domain body/root differs from actual certificate")
        identities = (self.count, self.dispatcher, *self.delegates)
        if any(type(identity) is not FactoryIdentity for identity in identities):
            raise ValueError("actual count/dispatcher/delegate identities required")
        if len({(identity.module, identity.name) for identity in identities}) != len(identities):
            raise ValueError("duplicate factory role")
        for identity in identities:
            if (identity.root, identity.label, identity.namespace) != (
                self.entry.va,
                self.label,
                self.namespace,
            ) or identity.dependencies != self.physical.source.dependencies:
                raise ValueError("factory root/label/namespace/dependency mismatch")
            module = importlib.import_module(identity.module)
            identity.check(
                self.entry.va, self.label, self.namespace, getattr(module, identity.name, None)
            )
        seeds = set()
        counts = set()
        for stream in self.streams:
            if type(stream) is not FrozenStream:
                raise ValueError("immutable independent stream required")
            stream.validate()
            if (stream.root, stream.namespace) != (self.entry.va, self.namespace):
                raise ValueError("stream root/namespace differs")
            if stream.seed in seeds:
                raise ValueError("duplicate frozen seed")
            seeds.add(stream.seed)
            counts.add(len(stream.expected))
        if len(counts) != 1:
            raise ValueError("seed-dependent provider count unsupported")

    def stream(self, seed: int) -> FrozenStream:
        if type(seed) is not int:
            raise ValueError("exact frozen seed required")
        matches = [stream for stream in self.streams if stream.seed == seed]
        if len(matches) != 1:
            raise ValueError("unsupported or duplicate custom seed")
        return matches[0]

    def roles(self, case: Case) -> tuple[str, ...]:
        """Derive ownership from core geometry, never a factory callback."""
        if type(case) is not Case:
            raise ValueError("actual immutable Case required")
        roles = ["baseline-frame", "baseline-scratch"]
        for address, raw in case.patches[2:]:
            span = address, address + len(raw)
            matches = [region.role for region in self.physical.regions if region.owns(span)]
            if not matches:
                base = case.esp
                matches = [
                    overlay.role
                    for overlay in self.physical.argument_overlays
                    if span == (base + 4 * overlay.slot, base + 4 * overlay.slot + 4)
                ]
            if len(matches) != 1:
                raise ValueError("patch lacks unique core role ownership")
            roles.append(matches[0])
        return tuple(roles)

    def authenticate_provider(self, provider: object, seed: int) -> tuple[Case, ...]:
        # Imported locally to keep legacy registry import free of custom declarations.
        from .providers import Provider

        if type(provider) is not Provider:
            raise ValueError("actual loaded Provider required")
        self.validate()
        if (
            provider.label != self.label
            or provider.namespace != self.namespace
            or self.entry.va not in provider.supported_vas
            or provider.authenticate is not None
            or provider.bind is not None
        ):
            raise ValueError("provider identity or mixed authority differs")
        self.count.check(self.entry.va, self.label, self.namespace, provider.count)
        self.dispatcher.check(self.entry.va, self.label, self.namespace, provider.make)
        stream = self.stream(seed)
        if provider.case_count(self.entry.va) != len(stream.expected):
            raise ValueError("complete provider count differs")
        cases = []
        for ordinal in range(len(stream.expected)):
            case = provider.case(
                seed, ordinal, self.entry.va, self.size, policy=self.physical.policy
            )
            # Physical checks remain independent and precede expected-stream digest.
            stream.check(self.physical, case, ordinal, self.roles(case))
            cases.append(case)
        # Catch persistent source/membership/factory drift during complete generation.
        self.validate()
        return tuple(cases)

    def document(self) -> dict[str, object]:
        self.validate()
        identities = (self.count, self.dispatcher, *self.delegates)
        contract = {
            "version": 1,
            "protocol": "scoped-fixture-v1",
            "entry": self.entry.as_json(),
            "size": self.size,
            "label": self.label,
            "namespace": self.namespace,
            "source_kind": self.physical.source.kind,
            "source_sha256": self.physical.source.source_sha256,
            "physical_sections": [
                asdict(section) for section in self.physical.source.image.source_sections
            ],
            "regions": [asdict(region) for region in self.physical.regions],
            "seed_policy": asdict(self.physical.policy),
            "factories": [
                {
                    "role": "count" if n == 0 else "dispatcher" if n == 1 else "delegate",
                    "module": identity.module,
                    "name": identity.name,
                    "source": identity.source_path,
                    "source_sha256": identity.source_sha256,
                    "code_sha256": identity.code_sha256,
                }
                for n, identity in enumerate(identities)
            ],
            "dependencies": dict(self.physical.source.dependencies.pins),
            "dependency_directories": list(self.physical.source.dependencies.directories),
            "streams": [stream.document() for stream in self.streams],
            "certificate": self.certificate.document(),
        }
        if self.physical.argument_overlays:
            contract["argument_overlays"] = [asdict(o) for o in self.physical.argument_overlays]
        if self.physical.known_code is not None:
            contract["version"] = 2
            contract["protocol"] = "scoped-fixture-prefix-v2"
            contract["known_code"] = self.physical.known_code.document(
                self.physical.source, self.physical.index, self.certificate.effective_source
            )
        return {"contract": contract, "sha256": digest(canonical(contract))}
