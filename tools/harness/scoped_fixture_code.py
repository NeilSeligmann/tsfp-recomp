# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit custom-protocol code-only certificate, isolated from named graphs.

Reuses the unchanged decoded instruction semantics and actual ExecutionGuard.
No dummy globals/slots or implicit old-capability dispatch are permitted.
"""

from __future__ import annotations

import csv
import io
import json
from dataclasses import dataclass

from .code_safety import (
    MAX_BYTES,
    MAX_NODES,
    Certificate,
    ExecutionGuard,
    NamedGlobalCodeSafetyError,
    Node,
    decode_node,
    section_sha,
    sha,
)
from .effective_index import EffectiveIndex
from .effective_index import validate_document as validate_effective
from .image import GuestImage, SourceSection
from .model import Case

CUSTOM_GUARD = "scoped-custom-fixture-direct-closure-v1"


def validate_nodes(nodes: object, root: int) -> None:
    if type(nodes) is not list or not 1 <= len(nodes) <= MAX_NODES:
        raise ValueError("missing/over-budget custom code nodes")
    starts = set()
    calls = {}
    occupied = []
    total = 0
    for node in nodes:
        if type(node) is not dict or set(node) != {"address", "size", "sha256", "instructions"}:
            raise ValueError("malformed certificate node")
        address, size = node["address"], node["size"]
        if (
            type(address) is not int
            or type(size) is not int
            or not 0 <= address < address + size <= 1 << 32
            or address in starts
        ):
            raise ValueError("invalid certificate node bounds")
        if any(address < end and begin < address + size for begin, end in occupied):
            raise ValueError("overlapping certificate nodes")
        occupied.append((address, address + size))
        starts.add(address)
        calls[address] = set()
        total += size
        items = node["instructions"]
        if type(items) is not list or not items:
            raise ValueError("missing certificate instruction map")
        cursor = address
        instruction_starts = set()
        branches = []
        for item in items:
            if type(item) is not dict or set(item) != {
                "address",
                "size",
                "sha256",
                "kind",
                "target",
                "pop",
            }:
                raise ValueError("malformed certificate instruction")
            at, width, kind, target, pop = (
                item[k] for k in ("address", "size", "kind", "target", "pop")
            )
            if (
                type(at) is not int
                or at != cursor
                or type(width) is not int
                or not 1 <= width <= 15
            ):
                raise ValueError("incomplete instruction coverage")
            cursor += width
            instruction_starts.add(at)
            if cursor > address + size:
                raise ValueError("instruction intersects slot/outside node")
            if (
                kind not in ("ordinary", "ret", "call", "branch")
                or type(pop) is not int
                or not 0 <= pop <= 65535
            ):
                raise ValueError("unsupported certificate instruction semantics")
            if kind in ("call", "branch"):
                if type(target) is not int or not 0 <= target < 1 << 32 or pop != 0:
                    raise ValueError("invalid direct target")
                if kind == "call":
                    calls[address].add(target)
                else:
                    branches.append(target)
            elif target is not None or (kind != "ret" and pop != 0):
                raise ValueError("invalid return/ordinary metadata")
            pin = item["sha256"]
            if (
                type(pin) is not str
                or len(pin) != 64
                or any(c not in "0123456789abcdef" for c in pin)
            ):
                raise ValueError("invalid instruction fingerprint")
        if (
            cursor != address + size
            or items[-1]["kind"] != "ret"
            or any(target not in instruction_starts for target in branches)
        ):
            raise ValueError("unaccounted node exit/branch")
        pin = node["sha256"]
        if type(pin) is not str or len(pin) != 64 or any(c not in "0123456789abcdef" for c in pin):
            raise ValueError("invalid node fingerprint")
    if (
        total > MAX_BYTES
        or root not in starts
        or any(target not in starts for targets in calls.values() for target in targets)
    ):
        raise ValueError("incomplete/over-budget certificate closure")
    reachable = set()
    pending = [root]
    while pending:
        at = pending.pop()
        if at not in reachable:
            reachable.add(at)
            pending.extend(calls[at])
    if reachable != starts:
        raise ValueError("extraneous certificate nodes")


def validate_document(value: object) -> dict[str, object]:
    if type(value) is not dict or set(value) != {"contract", "sha256"}:
        raise ValueError("malformed custom code certificate")
    contract = value["contract"]
    keys = {
        "version",
        "guard",
        "root",
        "image_sha256",
        "index_sha256",
        "sections_sha256",
        "nodes",
        "index_kind",
    }
    if type(contract) is not dict:
        raise ValueError("malformed custom certificate contract")
    effective = contract.get("index_kind") == "effective"
    if set(contract) != (keys | {"effective_source"} if effective else keys):
        raise ValueError("unknown custom certificate fields")
    if (
        type(contract["version"]) is not int
        or contract["version"] != 4
        or contract["guard"] != CUSTOM_GUARD
    ):
        raise ValueError("unknown custom code protocol/version")
    if contract["index_kind"] not in ("raw", "effective"):
        raise ValueError("unknown custom index role")
    if type(contract["root"]) is not int or not 0 <= contract["root"] < 1 << 32:
        raise ValueError("invalid custom root")
    for key in ("image_sha256", "index_sha256", "sections_sha256"):
        pin = contract[key]
        if type(pin) is not str or len(pin) != 64 or any(c not in "0123456789abcdef" for c in pin):
            raise ValueError("invalid custom source fingerprint")
    if effective:
        validate_effective(contract["effective_source"])
        if contract["effective_source"]["effective_sha256"] != contract["index_sha256"]:
            raise ValueError("custom effective source mismatch")
    validate_nodes(contract["nodes"], contract["root"])
    if value["sha256"] != sha(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()):
        raise ValueError("custom certificate hash mismatch")
    return contract


@dataclass(frozen=True, slots=True)
class CodeOnlyCertificate:
    root: int
    image_sha256: str
    index_sha256: str
    sections_sha256: str
    nodes: tuple[Node, ...]
    effective_source: EffectiveIndex | None = None

    def document(self) -> dict[str, object]:
        contract = dict(
            version=4,
            guard=CUSTOM_GUARD,
            root=self.root,
            image_sha256=self.image_sha256,
            index_sha256=self.index_sha256,
            sections_sha256=self.sections_sha256,
            index_kind="effective" if self.effective_source is not None else "raw",
            nodes=[
                dict(
                    address=n.address,
                    size=len(n.raw),
                    sha256=sha(n.raw),
                    instructions=[i.document() for i in n.instructions],
                )
                for n in self.nodes
            ],
        )
        if self.effective_source is not None:
            contract["effective_source"] = self.effective_source.document()
        return dict(
            contract=contract,
            sha256=sha(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()),
        )

    def validate(self) -> None:
        if type(self) is not CodeOnlyCertificate or type(self.nodes) is not tuple:
            raise ValueError("exact immutable custom code certificate required")
        if any(
            type(n) is not Node
            or type(n.raw) is not bytes
            or type(n.instructions) is not tuple
            or type(n.address) is not int
            or not 0 <= n.address < n.address + len(n.raw) <= 1 << 32
            for n in self.nodes
        ):
            raise ValueError("immutable custom code nodes required")
        if any(decode_node(n.address, n.raw) != n for n in self.nodes):
            raise ValueError("inconsistent actual custom instruction decode")
        if self.effective_source is not None and type(self.effective_source) is not EffectiveIndex:
            raise ValueError("immutable actual custom effective source required")
        validate_document(self.document())

    def authenticate(self, image: GuestImage, index: bytes) -> None:
        self.validate()
        rebuilt = build_certificate(image, index, self.root, effective_source=self.effective_source)
        if rebuilt != self:
            raise ValueError("actual custom code source/closure mismatch")


def build_certificate(
    image: GuestImage, index: bytes, root: int, *, effective_source: EffectiveIndex | None = None
) -> CodeOnlyCertificate:
    if (
        type(image) is not GuestImage
        or type(image.data) is not bytes
        or type(image.base) is not int
        or not 0 <= image.base < image.base + len(image.data) <= 1 << 32
        or type(index) is not bytes
        or type(root) is not int
        or type(image.source_sections) is not tuple
        or not image.source_sections
        or any(type(section) is not SourceSection for section in image.source_sections)
    ):
        raise ValueError("immutable actual custom image/index/physical sections required")
    if any(
        a.address < b.address + b.size and b.address < a.address + a.size
        for n, a in enumerate(image.source_sections)
        for b in image.source_sections[:n]
    ):
        raise ValueError("ambiguous actual custom physical sections")
    if effective_source is not None:
        if type(effective_source) is not EffectiveIndex or effective_source.table != index:
            raise ValueError("actual custom effective table mismatch")
        validate_effective(effective_source.document())
    reader = csv.DictReader(io.StringIO(index.decode()))
    if reader.fieldnames != ["entry_va", "size_bytes", "name", "is_thunk", "body_max_va"]:
        raise ValueError("unsupported original function index")
    bounds = {}
    for row in reader:
        at, size, last = (
            int(row["entry_va"], 16),
            int(row["size_bytes"]),
            int(row["body_max_va"], 16),
        )
        if at in bounds:
            raise ValueError("duplicate function entry")
        bounds[at] = (size, last)
    nodes = {}
    pending = [root]
    total = 0
    while pending:
        at = pending.pop()
        if at in nodes:
            continue
        if at not in bounds or len(nodes) >= MAX_NODES:
            raise ValueError("unknown/over-budget direct closure")
        size, last = bounds[at]
        if size <= 0 or last != at + size - 1:
            raise ValueError("fragmented/invalid original node extent")
        if any(
            other != at and other < at + size and end >= at for other, (_, end) in bounds.items()
        ):
            raise ValueError("ambiguous overlapping original node extent")
        if not any(
            s.executable and s.address <= at < at + size <= s.address + s.size
            for s in image.source_sections or ()
        ):
            raise ValueError("node outside executable source")
        total += size
        if total > MAX_BYTES:
            raise ValueError("closure byte budget")
        raw = image.code_at(at, size)
        if len(raw) != size:
            raise ValueError("missing original node bytes")
        node = decode_node(at, raw)
        nodes[at] = node
        pending.extend(i.target for i in node.instructions if i.kind == "call")
    certificate = CodeOnlyCertificate(
        root,
        sha(image.data),
        sha(index),
        section_sha(image),
        tuple(nodes[at] for at in sorted(nodes)),
        effective_source,
    )
    certificate.validate()
    return certificate


def guarded_certificate(certificate: object, *, protocol: str) -> CodeOnlyCertificate:
    """Dispatch before any callback/authentication, including cached callers."""
    if protocol != "scoped-fixture-v1" or type(certificate) is not CodeOnlyCertificate:
        raise ValueError("unknown custom protocol or exact certificate type")
    certificate.validate()
    return certificate


class ScopedFixtureSafetyError(NamedGlobalCodeSafetyError):
    kind = "SCOPED-FIXTURE-SAFETY-VIOLATION"


class CustomExecutionGuard(ExecutionGuard):
    def __init__(self, certificate: CodeOnlyCertificate, case: Case, sentinel: int) -> None:
        if type(case) is not Case:
            raise ValueError("exact immutable custom case required")
        guarded_certificate(certificate, protocol="scoped-fixture-v1")
        super().__init__(certificate, case, sentinel)

    def _is_excluded(self, address: int, size: int) -> bool:
        # This protocol has no pointer-global slots. Physical patch ownership is
        # checked separately; every fetch must still match a certified instruction.
        return False

    def fail(self, reason: str, **observed: object) -> None:
        if self.violation is None:
            self.violation = ScopedFixtureSafetyError(
                reason, root=self.case.va, index=self.case.index, **observed
            )


def certificate_for_protocol(
    certificate: object, protocol: str
) -> Certificate | CodeOnlyCertificate:
    if protocol == "named-global" and type(certificate) is Certificate:
        certificate.validate()
        return certificate
    if protocol == "scoped-fixture-v1":
        return guarded_certificate(certificate, protocol=protocol)
    raise ValueError("unknown or mixed code certificate protocol/type")
