# SPDX-License-Identifier: GPL-3.0-or-later
"""Root-scoped code exclusion and actual control-flow guard; never a guest fault."""

from __future__ import annotations

import csv
import hashlib
import io
import json
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM
from unicorn import UcError

from .effective_index import EffectiveIndex, SelectedSources
from .effective_index import validate_document as validate_effective_source
from .image import GuestImage, SourceSection
from .model import Case
from .selection import instruction_reason

GUARD_CONTRACT = "named-global-scoped-code-actual-ret-v2"
EFFECTIVE_GUARD_CONTRACT = "named-global-scoped-code-actual-ret-effective-v3"
MAX_NODES = 64
MAX_BYTES = 65536
MAX_CALL_DEPTH = 64


class NamedGlobalCodeSafetyError(RuntimeError):
    """Campaign-fatal contract failure: cannot be counted as an oracle fault."""

    kind = "NAMED-GLOBAL-CODE-SAFETY-VIOLATION"

    def __init__(self, reason: str, **observed: object) -> None:
        self.observed = {"kind": self.kind, "reason": reason, **observed}
        super().__init__(json.dumps(self.observed, sort_keys=True))


@dataclass(frozen=True, slots=True)
class Instruction:
    address: int
    raw: bytes
    kind: str = "ordinary"
    target: int | None = None
    pop: int = 0

    @property
    def following(self) -> int:
        return self.address + len(self.raw)

    def document(self) -> dict[str, object]:
        return dict(
            address=self.address,
            size=len(self.raw),
            sha256=sha(self.raw),
            kind=self.kind,
            target=self.target,
            pop=self.pop,
        )


@dataclass(frozen=True, slots=True)
class Node:
    address: int
    raw: bytes
    instructions: tuple[Instruction, ...]


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def section_sha(image: GuestImage) -> str:
    if not image.source_sections:
        raise ValueError("missing original source sections")
    return sha(
        json.dumps(
            [[s.address, s.size, s.executable, s.writable] for s in image.source_sections],
            separators=(",", ":"),
        ).encode()
    )


def decode_node(address: int, raw: bytes) -> Node:
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    decoded = list(decoder.disasm(raw, address))
    if not decoded or sum(insn.size for insn in decoded) != len(raw):
        raise ValueError("incomplete original instruction decode")
    instructions = []
    for insn in decoded:
        name = insn.mnemonic.lower()
        control = name == "call" or name.startswith("j") or name.startswith("loop")
        if name in ("ret", "retn"):
            # Only unprefixed near32 RET encodings; operand16/far returns refuse.
            if insn.bytes[0] not in (0xC2, 0xC3) or insn.size not in (1, 3):
                raise ValueError("unsupported return width/encoding")
            instructions.append(
                Instruction(
                    insn.address,
                    bytes(insn.bytes),
                    "ret",
                    pop=int.from_bytes(insn.bytes[1:], "little"),
                )
            )
        elif control:
            if any(insn.prefix) or len(insn.operands) != 1 or insn.operands[0].type != X86_OP_IMM:
                raise ValueError("unsupported indirect/prefixed control")
            target = insn.operands[0].imm
            if not 0 <= target < 1 << 32:
                raise ValueError("control target out of32bit range")
            instructions.append(
                Instruction(
                    insn.address, bytes(insn.bytes), "call" if name == "call" else "branch", target
                )
            )
        else:
            if insn.groups and any(group in insn.groups for group in (1, 2, 3, 4, 7)):
                raise ValueError("unsupported control instruction")
            if instruction_reason(name, insn.op_str) is not None:
                raise ValueError("unsupported original state/instruction")
            instructions.append(Instruction(insn.address, bytes(insn.bytes)))
    starts = {insn.address for insn in instructions}
    for insn in instructions:
        if insn.kind == "branch" and insn.target not in starts:
            raise ValueError("branch leaves node or targets instruction interior")
        if insn.kind != "ret" and insn.following not in starts:
            # Initial contract also refuses unconditional tail exits/fallthrough.
            raise ValueError("node has unaccounted fallthrough")
    return Node(address, raw, tuple(instructions))


@dataclass(frozen=True, slots=True)
class Certificate:
    root: int
    image_sha256: str
    index_sha256: str
    sections_sha256: str
    slots: tuple[tuple[int, int], ...]
    nodes: tuple[Node, ...]
    effective_source: EffectiveIndex | None = None

    def validate(self) -> None:
        if (
            type(self.root) is not int
            or type(self.nodes) is not tuple
            or not self.nodes
            or len(self.nodes) > MAX_NODES
            or type(self.slots) is not tuple
        ):
            raise ValueError("invalid certificate root/nodes")
        if any(
            type(value) is not str
            or len(value) != 64
            or any(c not in "0123456789abcdef" for c in value)
            for value in (self.image_sha256, self.index_sha256, self.sections_sha256)
        ):
            raise ValueError("invalid certificate source hashes")
        starts = {node.address for node in self.nodes}
        if len(starts) != len(self.nodes) or self.root not in starts:
            raise ValueError("duplicate/missing certificate root")
        if sum(len(node.raw) for node in self.nodes) > MAX_BYTES:
            raise ValueError("certificate byte bound")
        if not self.slots:
            raise ValueError("missing code-safety slots")
        for pos, span in enumerate(self.slots):
            if type(span) is not tuple or len(span) != 2 or any(type(x) is not int for x in span):
                raise ValueError("malformed code-safety slot")
            lo, hi = span
            if (
                not 0 <= lo < hi <= 1 << 32
                or hi != lo + 4
                or any(lo < b and a < hi for a, b in self.slots[:pos])
            ):
                raise ValueError("invalid/overlapping code-safety slots")
        for pos, node in enumerate(self.nodes):
            if type(node) is not Node:
                raise ValueError("invalid certificate node type")
            if (
                type(node.address) is not int
                or type(node.raw) is not bytes
                or not node.raw
                or not 0 <= node.address < node.address + len(node.raw) <= 1 << 32
                or type(node.instructions) is not tuple
            ):
                raise ValueError("invalid certificate node bounds/bytes")
            if decode_node(node.address, node.raw) != node:
                raise ValueError("inconsistent decoded certificate node")
            if any(
                node.address < other.address + len(other.raw)
                and other.address < node.address + len(node.raw)
                for other in self.nodes[:pos]
            ):
                raise ValueError("overlapping certificate code nodes")
            for insn in node.instructions:
                if type(insn) is not Instruction or type(insn.raw) is not bytes:
                    raise ValueError("nonimmutable certified instruction")
                if any(insn.address < hi and lo < insn.following for lo, hi in self.slots):
                    raise ValueError("certificate slot intersects code")
                if insn.kind == "call" and insn.target not in starts:
                    raise ValueError("missing certified direct callee")
        if self.effective_source is not None:
            if type(self.effective_source) is not EffectiveIndex:
                raise ValueError("immutable actual effective-source object required")
            if sha(self.effective_source.table) != self.index_sha256:
                raise ValueError("effective certificate/table source mismatch")
        validate_document(self.document())

    def document(self) -> dict[str, object]:
        contract = dict(
            version=2,
            guard=GUARD_CONTRACT,
            root=self.root,
            image_sha256=self.image_sha256,
            index_sha256=self.index_sha256,
            sections_sha256=self.sections_sha256,
            slots=[list(x) for x in self.slots],
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
            contract["version"] = 3
            contract["guard"] = EFFECTIVE_GUARD_CONTRACT
            contract["effective_source"] = self.effective_source.document()
        return {
            "contract": contract,
            "sha256": sha(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()),
        }

    def authenticate(self, image: GuestImage, index: bytes) -> None:
        self.validate()
        if (
            sha(image.data) != self.image_sha256
            or sha(index) != self.index_sha256
            or section_sha(image) != self.sections_sha256
        ):
            raise ValueError("stale code-safety source pins")
        rebuilt = build_certificate(
            image, index, self.root, self.slots, effective_source=self.effective_source
        )
        if rebuilt != self:
            raise ValueError("inconsistent code-safety closure")


def build_certificate(
    image: GuestImage,
    index: bytes,
    root: int,
    slots: tuple[tuple[int, int], ...],
    *,
    effective_source: EffectiveIndex | None = None,
) -> Certificate:
    """Authenticate complete original direct closure; index absence proves nothing."""
    if (
        type(image) is not GuestImage
        or type(image.data) is not bytes
        or type(image.base) is not int
        or type(index) is not bytes
        or type(root) is not int
        or not 0 <= image.base < image.base + len(image.data) <= 1 << 32
        or type(image.source_sections) is not tuple
        or not image.source_sections
        or any(type(section) is not SourceSection for section in image.source_sections)
    ):
        raise ValueError("immutable original image/index/section map required")
    if effective_source is not None and (
        type(effective_source) is not EffectiveIndex or effective_source.table != index
    ):
        raise ValueError("actual effective table/source mismatch")
    if effective_source is not None:
        validate_effective_source(effective_source.document())
    sections = image.source_sections
    if any(
        a.address < b.address + b.size and b.address < a.address + a.size
        for pos, a in enumerate(sections)
        for b in sections[:pos]
    ):
        raise ValueError("overlapping original source sections")
    if type(slots) is not tuple or not slots:
        raise ValueError("missing explicit code-exclusion slots")
    for pos, span in enumerate(slots):
        if type(span) is not tuple or len(span) != 2 or any(type(x) is not int for x in span):
            raise ValueError("malformed slot extent")
        lo, hi = span
        if hi != lo + 4 or not image.base <= lo < hi <= image.base + len(image.data):
            raise ValueError("invalid slot extent")
        if any(lo < b and a < hi for a, b in slots[:pos]):
            raise ValueError("duplicate/overlapping slots")
        if not any(
            s.writable and s.address <= lo < hi <= s.address + s.size
            for s in image.source_sections or ()
        ):
            raise ValueError("slot not wholly within writable source section")
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
        if any(i.address < hi and lo < i.following for i in node.instructions for lo, hi in slots):
            raise ValueError("seeded slot overlaps certified instruction bytes")
        nodes[at] = node
        pending.extend(i.target for i in node.instructions if i.kind == "call")
    return Certificate(
        root,
        sha(image.data),
        sha(index),
        section_sha(image),
        slots,
        tuple(nodes[at] for at in sorted(nodes)),
        effective_source,
    )


class ExecutionGuard:
    """Case-local observations only; violations always invalidate the campaign."""

    def __init__(self, certificate: Certificate, case: Case, sentinel: int) -> None:
        certificate.validate()
        if case.va != certificate.root or not any(
            n.address == case.va and len(n.raw) == case.size for n in certificate.nodes
        ):
            raise ValueError("code-safety root/extent mismatch")
        self.certificate = certificate
        self.case = case
        self.instructions = {i.address: i for n in certificate.nodes for i in n.instructions}
        self.returns = [sentinel]
        self.return_slots = [case.esp]
        self.pending: tuple[int, int, int | None] | None = None
        self.violation: NamedGlobalCodeSafetyError | None = None

    def fail(self, reason: str, **observed: object) -> None:
        if self.violation is None:
            self.violation = NamedGlobalCodeSafetyError(
                reason, root=self.case.va, index=self.case.index, **observed
            )

    def preflight(self, read: Callable[[int, int], bytes]) -> None:
        """All certified code must match AFTER every ordered input patch."""
        for node in self.certificate.nodes:
            try:
                current = read(node.address, len(node.raw))
            except UcError as error:
                self.fail(
                    "post-patch certified code unreadable", pc=node.address, read_error=repr(error)
                )
                self.check()
            if current != node.raw:
                self.fail("post-patch certified code changed", pc=node.address)
                self.check()

    def _is_excluded(self, address: int, size: int) -> bool:
        return any(address < hi and lo < address + size for lo, hi in self.certificate.slots)

    def before(self, address: int, size: int, esp: int, read: Callable[[int, int], bytes]) -> None:
        if self.violation:
            return
        if self._is_excluded(address, size):
            self.fail("instruction fetch intersects seeded slot", pc=address, size=size)
            return
        insn = self.instructions.get(address)
        if insn is None or len(insn.raw) != size:
            self.fail("instruction outside certified map", pc=address, size=size)
            return
        try:
            actual = read(address, size)
        except UcError as error:
            self.fail(
                "executed code observation unreadable",
                pc=address,
                size=size,
                read_error=repr(error),
            )
            return
        if actual != insn.raw:
            self.fail("executed code bytes changed", pc=address, size=size)
            return
        if self.pending:
            target, expected_esp, return_word = self.pending
            if address != target or esp != expected_esp:
                self.fail(
                    "actual transfer target/ESP mismatch",
                    pc=address,
                    esp=esp,
                    expected_pc=target,
                    expected_esp=expected_esp,
                )
                return
            if return_word is not None:
                try:
                    pushed = read(esp, 4)
                except UcError as error:
                    self.fail(
                        "actual call return-slot unreadable",
                        pc=address,
                        esp=esp,
                        read_error=repr(error),
                    )
                    return
                if len(pushed) != 4 or int.from_bytes(pushed, "little") != return_word:
                    self.fail("actual call return-slot mismatch", pc=address, esp=esp)
                    return
            self.pending = None
        if insn.kind == "call":
            if len(self.returns) >= MAX_CALL_DEPTH:
                self.fail("runtime call-depth bound", pc=address)
                return
            self.returns.append(insn.following)
            self.return_slots.append((esp - 4) & 0xFFFFFFFF)
            self.pending = (insn.target, (esp - 4) & 0xFFFFFFFF, insn.following)
        elif insn.kind == "ret":
            # A failed read is left to the actual original RET to fault. No target is invented.
            try:
                raw = read(esp, 4)
            except UcError:
                return
            target = int.from_bytes(raw, "little")
            if len(raw) != 4 or not self.returns or target != self.returns[-1]:
                self.fail(
                    "actual consumed return target mismatch",
                    pc=address,
                    esp=esp,
                    target=target,
                    expected_target=self.returns[-1] if self.returns else None,
                )
                return
            if not self.return_slots or esp != self.return_slots[-1]:
                self.fail(
                    "actual consumed return slot ESP mismatch",
                    pc=address,
                    esp=esp,
                    expected_esp=self.return_slots[-1] if self.return_slots else None,
                )
                return
            self.returns.pop()
            slot = self.return_slots.pop()
            self.pending = (target, (slot + 4 + insn.pop) & 0xFFFFFFFF, None)

    def finish(self, eip: int, esp: int) -> None:
        if self.violation:
            raise self.violation
        if (
            self.returns
            or self.return_slots
            or self.pending is None
            or self.pending[:2] != (eip, esp)
        ):
            self.fail("unverified final return/cleanup", pc=eip, esp=esp)
            raise self.violation

    def check(self, cause: Exception | None = None) -> None:
        if self.violation:
            if cause is not None:
                self.violation.observed["secondary_cpu_error"] = repr(cause)
                self.violation.args = (json.dumps(self.violation.observed, sort_keys=True),)
                raise self.violation from cause
            raise self.violation


def validate_document(value: object) -> dict[str, object]:
    """Validate serialized semantic shape; actual bytes/index are bound at execution."""
    if not isinstance(value, dict) or set(value) != {"contract", "sha256"}:
        raise ValueError("missing/malformed code-safety certificate")
    contract = value["contract"]
    keys = {
        "version",
        "guard",
        "root",
        "image_sha256",
        "index_sha256",
        "sections_sha256",
        "slots",
        "nodes",
    }
    if not isinstance(contract, dict):
        raise ValueError("malformed code-safety certificate contract")
    effective = contract.get("version") == 3 and type(contract.get("version")) is int
    if set(contract) != (keys | {"effective_source"} if effective else keys):
        raise ValueError("malformed code-safety certificate contract")
    if effective:
        if contract["guard"] != EFFECTIVE_GUARD_CONTRACT:
            raise ValueError("unknown code-safety version/capability")
        validate_effective_source(contract["effective_source"])
        if contract["effective_source"]["effective_sha256"] != contract["index_sha256"]:
            raise ValueError("effective certificate/source digest mismatch")
    elif (
        type(contract["version"]) is not int
        or contract["version"] != 2
        or contract["guard"] != GUARD_CONTRACT
    ):
        raise ValueError("unknown code-safety version/capability")
    if type(contract["root"]) is not int or not 0 <= contract["root"] < 1 << 32:
        raise ValueError("invalid certificate root")
    for name in ("image_sha256", "index_sha256", "sections_sha256"):
        pin = contract[name]
        if type(pin) is not str or len(pin) != 64 or any(c not in "0123456789abcdef" for c in pin):
            raise ValueError("invalid code-safety source hash")
    slots, nodes = contract["slots"], contract["nodes"]
    if (
        not isinstance(slots, list)
        or not slots
        or not isinstance(nodes, list)
        or not 1 <= len(nodes) <= MAX_NODES
    ):
        raise ValueError("missing certificate slots/nodes")
    for pos, span in enumerate(slots):
        if not isinstance(span, list) or len(span) != 2 or any(type(x) is not int for x in span):
            raise ValueError("invalid certificate slot")
        lo, hi = span
        if (
            not 0 <= lo < hi <= 1 << 32
            or hi != lo + 4
            or any(lo < b and a < hi for a, b in slots[:pos])
        ):
            raise ValueError("invalid/overlapping certificate slot")
    starts = set()
    calls = {}
    occupied = []
    total = 0
    for node in nodes:
        if not isinstance(node, dict) or set(node) != {"address", "size", "sha256", "instructions"}:
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
        if not isinstance(items, list) or not items:
            raise ValueError("missing certificate instruction map")
        cursor = address
        instruction_starts = set()
        branches = []
        for item in items:
            if not isinstance(item, dict) or set(item) != {
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
            if cursor > address + size or any(at < hi and lo < cursor for lo, hi in slots):
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
        or contract["root"] not in starts
        or any(target not in starts for targets in calls.values() for target in targets)
    ):
        raise ValueError("incomplete/over-budget certificate closure")
    reachable = set()
    pending = [contract["root"]]
    while pending:
        at = pending.pop()
        if at not in reachable:
            reachable.add(at)
            pending.extend(calls[at])
    if reachable != starts:
        raise ValueError("extraneous certificate nodes")
    if value["sha256"] != sha(json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()):
        raise ValueError("code-safety certificate hash mismatch")
    return contract


@dataclass(frozen=True, slots=True)
class Declaration:
    """Immutable canonical metadata; no mutable dict or inferred code bounds."""

    certificate_json: str

    def __post_init__(self) -> None:
        if type(self.certificate_json) is not str:
            raise ValueError("immutable certificate JSON required")
        value = json.loads(self.certificate_json)
        validate_document(value)
        if json.dumps(value, sort_keys=True, separators=(",", ":")) != self.certificate_json:
            raise ValueError("noncanonical certificate declaration")

    @classmethod
    def from_document(cls, value: object) -> Declaration:
        validate_document(value)
        return cls(json.dumps(value, sort_keys=True, separators=(",", ":")))

    def document(self) -> dict[str, object]:
        return json.loads(self.certificate_json)

    def bind(self, image: GuestImage, index: bytes) -> Certificate:
        declared = self.document()
        contract = declared["contract"]
        if contract["version"] != 2:
            raise ValueError("effective certificate requires actual selected source roles")
        certificate = build_certificate(
            image, index, contract["root"], tuple(tuple(span) for span in contract["slots"])
        )
        if certificate.document() != declared:
            raise ValueError("actual source differs from declared code-safety certificate")
        return certificate

    def bind_effective(self, image: GuestImage, source: EffectiveIndex) -> Certificate:
        declared = self.document()
        contract = declared["contract"]
        if (
            type(source) is not EffectiveIndex
            or contract.get("effective_source") != source.document()
        ):
            raise ValueError("actual selected effective-source roles differ from certificate")
        certificate = build_certificate(
            image,
            source.table,
            contract["root"],
            tuple(tuple(span) for span in contract["slots"]),
            effective_source=source,
        )
        if certificate.document() != declared:
            raise ValueError("actual effective closure differs from declared certificate")
        return certificate


# Bounded direct-closure capability independently reviewed against source72fa3ecf7
# and scoped-runtime-attempt2; see scoped-capability-ratification-predeclaration.
# Metadata alone never authenticates a game source or satisfies proof gates.
RUNTIME_VALIDATED = True


class SourceBinding:
    """Frozen actual source with live index-file drift checks before every case."""

    def __init__(
        self,
        declaration: Declaration,
        image: GuestImage,
        functions: Path,
        *,
        overrides: Path | None = None,
        additions: Path | None = None,
    ) -> None:
        if type(declaration) is not Declaration or type(image) is not GuestImage:
            raise ValueError("actual immutable declaration/image required")
        self.image = image
        self.functions = functions
        self._selected = None
        if declaration.document()["contract"]["version"] == 3:
            self._selected = SelectedSources(functions, overrides, additions)
            self.index = self._selected.effective.table
            self.certificate = declaration.bind_effective(image, self._selected.effective)
        else:
            self.index = functions.read_bytes()
            self.certificate = declaration.bind(image, self.index)
        self._declaration = declaration
        self._checked = 0
        self._returned = 0
        self._faulted = 0
        self._indices = hashlib.sha256()

    def source(self) -> tuple[GuestImage, bytes]:
        # Do not let an immutable cached byte string hide a changed original index.
        if self._selected is not None:
            self._selected.check()
        elif self.functions.read_bytes() != self.index:
            raise ValueError("actual original function index drifted during campaign")
        return self.image, self.index

    def observe(self, case: Case, observation: object) -> None:
        expected = self.certificate.document()["sha256"]
        if (
            not isinstance(observation, dict)
            or set(observation) != {"certificate_sha256", "case_index", "outcome"}
            or observation["certificate_sha256"] != expected
            or type(observation["case_index"]) is not int
            or observation["case_index"] != case.index
            or observation["outcome"] not in ("returned", "faulted")
        ):
            raise ValueError("missing/mismatched actual code-safety observation")
        self._checked += 1
        self._returned += observation["outcome"] == "returned"
        self._faulted += observation["outcome"] == "faulted"
        self._indices.update(f"{case.index}:{observation['outcome']}\n".encode())

    def document(self, seed: int) -> dict[str, object]:
        self.source()
        summary = {
            "version": self.certificate.document()["contract"]["guard"],
            "certificate_sha256": self.certificate.document()["sha256"],
            "index_sha256": hashlib.sha256(self.index).hexdigest(),
            "seed": seed,
            "checked_cases": self._checked,
            "returned": self._returned,
            "faulted": self._faulted,
            "case_observations_sha256": self._indices.hexdigest(),
            "validated": RUNTIME_VALIDATED,
        }
        if self._selected is not None:
            summary["effective_source"] = self._selected.document()
        return summary


def validate_runtime(value: object, declaration: object, cases: object, seed: object) -> None:
    """Accepted proof provenance requires measured, complete guarded execution."""
    if not RUNTIME_VALIDATED:
        raise ValueError("unvalidated local scoped code-safety runtime capability")
    validate_document(declaration)
    keys = {
        "version",
        "certificate_sha256",
        "index_sha256",
        "seed",
        "checked_cases",
        "returned",
        "faulted",
        "case_observations_sha256",
        "validated",
    }
    effective = declaration["contract"]["version"] == 3
    if not isinstance(value, dict) or set(value) != (
        keys | {"effective_source"} if effective else keys
    ):
        raise ValueError("missing/malformed scoped code-safety runtime provenance")
    if effective and value["effective_source"] != declaration["contract"]["effective_source"]:
        raise ValueError("mismatched actual effective-source runtime provenance")
    if (
        value["version"] != declaration["contract"]["guard"]
        or value["certificate_sha256"] != declaration["sha256"]
        or value["index_sha256"] != declaration["contract"]["index_sha256"]
        or type(seed) is not int
        or type(value["seed"]) is not int
        or value["seed"] != seed
        or value["validated"] is not True
    ):
        raise ValueError("unvalidated/mismatched scoped code-safety runtime provenance")
    if (
        type(cases) is not int
        or cases <= 0
        or any(
            type(value[key]) is not int or value[key] < 0
            for key in ("checked_cases", "returned", "faulted")
        )
        or value["checked_cases"] != cases
        or value["returned"] + value["faulted"] != cases
    ):
        raise ValueError("incomplete scoped code-safety case observations")
    digest = value["case_observations_sha256"]
    if type(digest) is not str or len(digest) != 64:
        raise ValueError("malformed scoped code-safety observation digest")
    try:
        bytes.fromhex(digest)
    except ValueError as error:
        raise ValueError("malformed scoped code-safety observation digest") from error
