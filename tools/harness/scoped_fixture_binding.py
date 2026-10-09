# SPDX-License-Identifier: GPL-3.0-or-later
"""Custom protocol's actual source/physical and ordered observation binding.

No declaration is activated here. Local capability remains FALSE independently
of named-global capability, and missing observations abort before subject use.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

from . import scoped_fixture_authority as authority
from .effective_index import SelectedSources
from .image import GuestImage
from .model import Case
from .scoped_fixture_code import CUSTOM_GUARD, CodeOnlyCertificate
from .scoped_fixture_domain import Domain


class Binding:
    def __init__(
        self,
        domain: Domain,
        provider: object,
        seed: int,
        functions: Path,
        *,
        overrides: Path | None = None,
        additions: Path | None = None,
    ) -> None:
        if type(domain) is not Domain or not isinstance(functions, Path):
            raise ValueError("actual custom domain and index path required")
        domain.validate()
        self.domain, self.seed, self.functions = domain, seed, functions
        self.certificate = domain.certificate
        self._selected = None
        if self.certificate.effective_source is not None:
            self._selected = SelectedSources(functions, overrides, additions)
            self.index = self._selected.effective.table
            if self._selected.effective != self.certificate.effective_source:
                raise ValueError("actual custom effective source differs")
        else:
            if overrides is not None or additions is not None:
                raise ValueError("raw custom contract refuses undeclared source layers")
            self.index = functions.read_bytes()
        if domain.physical.known_code is not None:
            actual_raw = (
                self._selected.effective.export
                if self._selected is not None
                else functions.read_bytes()
            )
            if actual_raw != domain.physical.known_code.raw:
                raise ValueError("actual CLI raw source differs from independent prefix reference")
        if self.index != domain.physical.index:
            raise ValueError("actual custom index differs from physical/certificate authority")
        domain.authenticate_provider(provider, seed)
        self._contract = domain.document()
        self._pending: tuple[str, str, int] | None = None
        self._checked = self._returned = self._faulted = 0
        self._observations = hashlib.sha256()
        self._by_kind: dict[str, int] = {}
        self._provider_next = 0
        self.source()

    def source(self) -> tuple[GuestImage, bytes]:
        if (
            type(self.domain) is not Domain
            or type(self.certificate) is not CodeOnlyCertificate
            or self.certificate != self.domain.certificate
        ):
            raise ValueError("exact actual custom domain/certificate required")
        if self._selected is not None:
            if type(self._selected) is not SelectedSources:
                raise ValueError("actual selected custom source required")
            self._selected.check()
        elif self.functions.read_bytes() != self.index:
            raise ValueError("actual custom original index drift")
        self.domain.validate()
        if self.domain.document() != self._contract:
            raise ValueError("actual custom domain/source drift")
        return self.domain.physical.source.image, self.index

    def prepare(self, case: Case, kind: str) -> None:
        if type(kind) is not str:
            raise ValueError("exact custom case phase required")
        if type(self) is not Binding or self._pending is not None:
            raise ValueError("missing prior custom observation or forged binding")
        self.source()
        roles = self.domain.roles(case)
        self.domain.physical.check(case, roles)
        if (case.seed, case.va, case.size) != (self.seed, self.domain.entry.va, self.domain.size):
            raise ValueError("custom actual case seed/root/body differs")
        if kind == self.domain.label:
            ordinal = case.index - (self.domain.namespace << 40)
            if ordinal != self._provider_next:
                raise ValueError("custom provider cases omitted/reordered/repeated")
            self.domain.stream(self.seed).check(self.domain.physical, case, ordinal, roles)
        elif type(kind) is not str or kind not in ("random", "edge", "feedback"):
            raise ValueError("unknown ordinary custom case phase/provider")
        self._pending = authority.case_digest(case), kind, case.index

    def observe(self, case: Case, observation: object) -> None:
        expected = self.certificate.document()["sha256"]
        if (
            self._pending is None
            or authority.case_digest(case) != self._pending[0]
            or type(observation) is not dict
            or set(observation) != {"certificate_sha256", "case_index", "outcome"}
            or observation["certificate_sha256"] != expected
            or type(observation["case_index"]) is not int
            or observation["case_index"] != case.index
            or observation["outcome"] not in ("returned", "faulted")
        ):
            raise ValueError("missing/mismatched actual custom observation")
        self.source()
        self._observations.update(authority.canonical((self._pending, observation)))
        kind = self._pending[1]
        self._by_kind[kind] = self._by_kind.get(kind, 0) + 1
        if kind == self.domain.label:
            self._provider_next += 1
        self._checked += 1
        self._returned += observation["outcome"] == "returned"
        self._faulted += observation["outcome"] == "faulted"
        self._pending = None

    def document(self) -> dict[str, object]:
        self.source()
        if self._pending is not None:
            raise ValueError("incomplete custom observation stream")
        return {
            "protocol": CUSTOM_GUARD,
            "contract_sha256": self._contract["sha256"],
            "certificate_sha256": self.certificate.document()["sha256"],
            "seed": self.seed,
            "checked_cases": self._checked,
            "by_kind": dict(self._by_kind),
            "returned": self._returned,
            "faulted": self._faulted,
            "case_observations_sha256": self._observations.hexdigest(),
            "validated": authority.CUSTOM_RUNTIME_VALIDATED,
        }
