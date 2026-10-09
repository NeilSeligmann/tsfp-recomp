# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded named object graphs, explicit initialization only, never inferred layouts."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import asdict, dataclass, replace
from math import prod
from pathlib import Path

from .code_safety import Declaration as CodeSafetyDeclaration
from .code_safety import SourceBinding, validate_runtime
from .effective_index import SelectedSources
from .image import GuestImage, SourceSection
from .model import Case
from .seeding import (
    GUEST_HI,
    GUEST_LO,
    GUEST_SPAN,
    KPCR_BASE,
    SCRATCH_WORDS,
    SEEDED_THUNK_SLOTS,
    THUNK_TARGET_BASE,
    TLS_BLOCK_BASE,
    TLS_BLOCK_STRIDE,
    TLS_SLOT_COUNT,
    SeedPolicy,
    make_case,
)

NAMESPACE = 91
BACKGROUNDS = 8
MAX_CASES = 65536
MAX_BYTES = 65536
LABEL = "named-global-objects"
ROOT = Path(__file__).resolve().parents[2]


def _name(value: object) -> None:
    if not isinstance(value, str) or re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,63}", value) is None:
        raise ValueError("invalid object/global name")


def _sha(value: object) -> None:
    if not isinstance(value, str) or re.fullmatch("[0-9a-f]{64}", value) is None:
        raise ValueError("missing/invalid SHA256 pin")


def _range(address: int, size: int) -> tuple[int, int]:
    if (
        type(address) is not int
        or type(size) is not int
        or size <= 0
        or not GUEST_LO <= address < address + size <= GUEST_HI
    ):
        raise ValueError("unmapped/wrapping initialization range")
    return address, address + size


def _overlap(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] < right[1] and right[0] < left[1]


@dataclass(frozen=True, slots=True)
class Field:
    offset: int
    width: int
    values: tuple[int, ...]

    def __post_init__(self) -> None:
        if (
            type(self.offset) is not int
            or self.offset < 0
            or type(self.width) is not int
            or self.width not in (1, 2, 4)
            or type(self.values) is not tuple
            or not self.values
            or any(
                type(value) is not int or not 0 <= value < 1 << (8 * self.width)
                for value in self.values
            )
            or len(set(self.values)) != len(self.values)
        ):
            raise ValueError("invalid scalar field")


@dataclass(frozen=True, slots=True)
class Link:
    offset: int
    target: str
    addend: int = 0

    def __post_init__(self) -> None:
        _name(self.target)
        if (
            type(self.offset) is not int
            or self.offset < 0
            or type(self.addend) is not int
            or self.addend < 0
        ):
            raise ValueError("invalid object link")


@dataclass(frozen=True, slots=True)
class Object:
    name: str
    size: int
    fields: tuple[Field, ...] = ()
    links: tuple[Link, ...] = ()
    alignment: int = 4

    def __post_init__(self) -> None:
        _name(self.name)
        if (
            type(self.size) is not int
            or not 0 < self.size <= MAX_BYTES
            or type(self.alignment) is not int
            or self.alignment not in (1, 2, 4, 8, 16)
            or type(self.fields) is not tuple
            or any(not isinstance(field, Field) for field in self.fields)
            or type(self.links) is not tuple
            or any(not isinstance(link, Link) for link in self.links)
        ):
            raise ValueError("invalid object layout")
        occupied = set()
        for member in (*self.fields, *self.links):
            width = member.width if isinstance(member, Field) else 4
            slots = set(range(member.offset, member.offset + width))
            if member.offset + width > self.size or occupied & slots:
                raise ValueError("overlapping/out-of-object field or link")
            occupied.update(slots)


@dataclass(frozen=True, slots=True)
class Global:
    name: str
    address: int
    target: str
    modes: tuple[str, ...] = ("object",)

    def __post_init__(self) -> None:
        _name(self.name)
        _name(self.target)
        _range(self.address, 4)
        if (
            type(self.modes) is not tuple
            or not self.modes
            or any(mode not in ("object", "null", "unmapped") for mode in self.modes)
            or len(set(self.modes)) != len(self.modes)
        ):
            raise ValueError("invalid named-global pointer modes")


@dataclass(frozen=True, slots=True)
class Domain:
    va: int
    body_size: int
    body_sha256: str
    image_sha256: str
    sections_sha256: str
    witness: str
    witness_sha256: str
    objects: tuple[Object, ...]
    globals: tuple[Global, ...]
    arena: int = 0x00C00000
    code_safety: CodeSafetyDeclaration | None = None

    def __post_init__(self) -> None:
        _range(self.va, self.body_size)
        for pin in (self.body_sha256, self.image_sha256, self.sections_sha256, self.witness_sha256):
            _sha(pin)
        _range(self.arena, 1)
        path = Path(self.witness)
        if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "docs":
            raise ValueError("witness must be a tracked docs-relative path")
        if (
            type(self.objects) is not tuple
            or not self.objects
            or any(not isinstance(obj, Object) for obj in self.objects)
            or type(self.globals) is not tuple
            or not self.globals
            or any(not isinstance(item, Global) for item in self.globals)
        ):
            raise ValueError("missing/invalid named graph")
        if self.code_safety is not None:
            if type(self.code_safety) is not CodeSafetyDeclaration:
                raise ValueError("invalid scoped code-safety declaration")
            scoped = self.code_safety.document()["contract"]
            if (
                scoped["root"] != self.va
                or scoped["image_sha256"] != self.image_sha256
                or scoped["slots"] != [[item.address, item.address + 4] for item in self.globals]
            ):
                raise ValueError("code-safety declaration belongs to another root/image/slot set")
        names = [obj.name for obj in self.objects]
        global_names = [item.name for item in self.globals]
        if len(set(names)) != len(names) or len(set(global_names)) != len(global_names):
            raise ValueError("duplicate named object/global")
        sizes = {obj.name: obj.size for obj in self.objects}
        for item in self.globals:
            if item.target not in sizes:
                raise ValueError("missing global target")
        for obj in self.objects:
            for link in obj.links:
                if link.target not in sizes or link.addend >= sizes[link.target]:
                    raise ValueError("missing/out-of-object link target")
        ranges = [_range(item.address, 4) for item in self.globals]
        if any(
            _overlap(left, right)
            for index, left in enumerate(ranges)
            for right in ranges[index + 1 :]
        ):
            raise ValueError("overlapping global pointer slots")
        addresses = self.addresses()
        end = max(addresses[obj.name] + obj.size for obj in self.objects)
        _range(self.arena, end - self.arena)
        if end - self.arena > MAX_BYTES or any(
            _overlap((self.arena, end), span) for span in ranges
        ):
            raise ValueError("allocator overflow/global overlap")
        if self.count > MAX_CASES:
            raise ValueError("named graph Cartesian domain exceeds bound")

    @property
    def count(self) -> int:
        return (
            BACKGROUNDS
            * prod(len(field.values) for obj in self.objects for field in obj.fields)
            * prod(len(item.modes) for item in self.globals)
        )

    def addresses(self) -> dict[str, int]:
        cursor = self.arena
        result = {}
        for obj in self.objects:
            cursor = (cursor + obj.alignment - 1) & -obj.alignment
            result[obj.name] = cursor
            cursor += obj.size
        return result

    def document(self) -> dict[str, object]:
        graph = asdict(self)
        if self.code_safety is None:
            del graph["code_safety"]
        else:
            graph["code_safety"] = self.code_safety.document()
        contract = {
            "version": 1 if self.code_safety is None else 2,
            "namespace": NAMESPACE,
            "backgrounds": BACKGROUNDS,
            "backing": "SHA256(seed,background,VA,object,block)-v1",
            "graph": graph,
        }
        contract = json.loads(json.dumps(contract))
        return {
            "contract": contract,
            "sha256": hashlib.sha256(
                json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest(),
        }

    def authenticate(
        self,
        image: GuestImage,
        size: int,
        *,
        witness_root: Path = ROOT,
        functions: Path | None = None,
        overrides: Path | None = None,
        additions: Path | None = None,
    ) -> dict[str, object]:
        if image.base != GUEST_LO or len(image.data) != GUEST_SPAN:
            raise ValueError("named graph requires the complete shared guest window")
        if (
            size != self.body_size
            or hashlib.sha256(image.code_at(self.va, size)).hexdigest() != self.body_sha256
        ):
            raise ValueError("named graph source body extent/hash mismatch")
        if hashlib.sha256(image.data).hexdigest() != self.image_sha256:
            raise ValueError("named graph complete image pin mismatch")
        if (
            hashlib.sha256((witness_root / self.witness).read_bytes()).hexdigest()
            != self.witness_sha256
        ):
            raise ValueError("named graph witness pin mismatch")
        sections = image.source_sections
        if (
            type(sections) is not tuple
            or not sections
            or any(not isinstance(section, SourceSection) for section in sections)
        ):
            raise ValueError("named graph requires complete source section map")
        if section_digest(sections) != self.sections_sha256:
            raise ValueError("named graph complete section map pin mismatch")
        ranges = [(section.address, section.address + section.size) for section in sections]
        if any(
            _overlap(left, right)
            for index, left in enumerate(ranges)
            for right in ranges[index + 1 :]
        ):
            raise ValueError("ambiguous overlapping source sections")
        if not any(
            section.executable
            and section.address <= self.va
            and self.va + size <= section.address + section.size
            for section in sections
        ):
            raise ValueError("named graph body not within executable section")
        if self.code_safety is not None:
            if functions is None:
                raise ValueError("scoped code safety requires actual original function index")
            if self.code_safety.document()["contract"]["version"] == 3:
                actual = SelectedSources(functions, overrides, additions)
                self.code_safety.bind_effective(image, actual.effective)
                actual.check()
            else:
                self.code_safety.bind(image, functions.read_bytes())
        for item in self.globals:
            if not any(
                section.writable
                and (not section.executable or self.code_safety is not None)
                and section.address <= item.address
                and item.address + 4 <= section.address + section.size
                for section in sections
            ):
                raise ValueError("named global is not writable non-code data")
        addresses = self.addresses()
        arena = (self.arena, max(addresses[obj.name] + obj.size for obj in self.objects))
        if any(_overlap(arena, loaded) for loaded in ranges):
            raise ValueError("named object arena overlaps loaded image")
        return self.document()

    def case(self, seed: int, ordinal: int, size: int, *, policy: SeedPolicy | None = None) -> Case:
        if type(ordinal) is not int or not 0 <= ordinal < self.count or size != self.body_size:
            raise ValueError("unsupported named graph root extent/ordinal")
        policy = policy or SeedPolicy()
        background = ordinal % BACKGROUNDS
        base = make_case(seed, (NAMESPACE << 40) + background, self.va, size, policy=policy)
        addresses = self.addresses()
        objects = [(addresses[obj.name], addresses[obj.name] + obj.size) for obj in self.objects]
        reserved = [
            (policy.stack_base - 0x10000, policy.stack_base + 0x10000 + policy.frame_args * 4),
            (policy.scratch_base, policy.scratch_base + SCRATCH_WORDS * 4),
            (KPCR_BASE, TLS_BLOCK_BASE + TLS_BLOCK_STRIDE * TLS_SLOT_COUNT),
            (THUNK_TARGET_BASE, THUNK_TARGET_BASE + 4 * len(SEEDED_THUNK_SLOTS)),
            (policy.sentinel, policy.sentinel + 1),
            *[(address, address + len(data)) for address, data in base.patches],
        ]
        slots = [(item.address, item.address + 4) for item in self.globals]
        if any(_overlap(span, other) for span in (*objects, *slots) for other in reserved):
            raise ValueError("named graph overlaps seeded stack/scratch/TLS/sentinel")
        position = ordinal // BACKGROUNDS
        axes = [field.values for obj in self.objects for field in obj.fields] + [
            item.modes for item in self.globals
        ]
        chosen = []
        for values in reversed(axes):
            position, index = divmod(position, len(values))
            chosen.append(values[index])
        chosen.reverse()
        patches = list(base.patches)
        field_index = 0
        for obj in self.objects:
            payload = bytearray()
            for block in range((obj.size + 31) // 32):
                key = f"{seed}:{background}:{self.va}:{obj.name}:{block}".encode()
                payload.extend(hashlib.sha256(key).digest())
            del payload[obj.size :]
            for field in obj.fields:
                value = chosen[field_index]
                field_index += 1
                payload[field.offset : field.offset + field.width] = value.to_bytes(
                    field.width, "little"
                )
            for link in obj.links:
                value = addresses[link.target] + link.addend
                payload[link.offset : link.offset + 4] = value.to_bytes(4, "little")
            patches.append((addresses[obj.name], bytes(payload)))
        for item, mode in zip(self.globals, chosen[field_index:], strict=True):
            value = {"object": addresses[item.target], "null": 0, "unmapped": GUEST_HI}[mode]
            patches.append((item.address, value.to_bytes(4, "little")))
        return replace(base, index=(NAMESPACE << 40) + ordinal, patches=tuple(patches))


@dataclass(frozen=True, slots=True)
class Factory:
    domains: tuple[Domain, ...]

    def __post_init__(self) -> None:
        if (
            type(self.domains) is not tuple
            or any(not isinstance(domain, Domain) for domain in self.domains)
            or len(set(self.supported_vas)) != len(self.domains)
        ):
            raise ValueError("invalid/duplicate named graph root contracts")

    @property
    def supported_vas(self) -> tuple[int, ...]:
        return tuple(domain.va for domain in self.domains)

    def domain(self, va: int) -> Domain:
        for domain in self.domains:
            if domain.va == va:
                return domain
        raise ValueError("unsupported named graph root")

    def count(self, va: int) -> int:
        return next((domain.count for domain in self.domains if domain.va == va), 0)

    def make(
        self, seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
    ) -> Case:
        return self.domain(va).case(seed, ordinal, size, policy=policy)

    def bind(
        self,
        va: int,
        image: GuestImage,
        functions: Path,
        *,
        overrides: Path | None = None,
        additions: Path | None = None,
    ) -> SourceBinding | None:
        declaration = self.domain(va).code_safety
        if declaration is None:
            return None
        return SourceBinding(
            declaration, image, functions, overrides=overrides, additions=additions
        )

    def authenticate(
        self,
        va: int,
        size: int,
        image: GuestImage,
        *,
        seed: int,
        policy: SeedPolicy | None = None,
        functions: Path | None = None,
        overrides: Path | None = None,
        additions: Path | None = None,
    ) -> dict[str, object]:
        domain = self.domain(va)
        contract = domain.authenticate(
            image, size, functions=functions, overrides=overrides, additions=additions
        )
        for background in range(BACKGROUNDS):
            domain.case(seed, background, size, policy=policy)
        return contract


def section_digest(sections: tuple[SourceSection, ...]) -> str:
    return hashlib.sha256(
        json.dumps(
            [asdict(section) for section in sections], sort_keys=True, separators=(",", ":")
        ).encode()
    ).hexdigest()


def validate_contract(value: object) -> None:
    """Validate serialized initialization provenance without reading private image data."""
    if not isinstance(value, dict) or set(value) != {"contract", "sha256"}:
        raise ValueError("missing/malformed named graph provenance")
    contract = value["contract"]
    if not isinstance(contract, dict) or set(contract) != {
        "version",
        "namespace",
        "backgrounds",
        "backing",
        "graph",
    }:
        raise ValueError("malformed named graph contract")
    if type(contract["version"]) is not int or contract["version"] not in (1, 2):
        raise ValueError("unknown named graph version")
    for name, expected in (("namespace", NAMESPACE), ("backgrounds", BACKGROUNDS)):
        if type(contract[name]) is not int or contract[name] != expected:
            raise ValueError("unknown named graph schema/namespace/backgrounds")
    graph = contract["graph"]
    if not isinstance(graph, dict):
        raise ValueError("missing named graph declaration")
    try:
        objects = []
        for raw in graph["objects"]:
            if (
                not isinstance(raw, dict)
                or not isinstance(raw["fields"], list)
                or not isinstance(raw["links"], list)
            ):
                raise ValueError("malformed named object")
            fields = []
            for field in raw["fields"]:
                if not isinstance(field["values"], list):
                    raise ValueError("malformed named field values")
                fields.append(Field(**{**field, "values": tuple(field["values"])}))
            objects.append(
                Object(
                    **{
                        **raw,
                        "fields": tuple(fields),
                        "links": tuple(Link(**link) for link in raw["links"]),
                    }
                )
            )
        globals_ = []
        for raw in graph["globals"]:
            if not isinstance(raw, dict) or not isinstance(raw["modes"], list):
                raise ValueError("malformed named global modes")
            globals_.append(Global(**{**raw, "modes": tuple(raw["modes"])}))
        scoped = {}
        if contract["version"] == 2:
            scoped["code_safety"] = CodeSafetyDeclaration.from_document(graph["code_safety"])
        elif "code_safety" in graph:
            raise ValueError("v1 graph cannot contain scoped code safety")
        domain = Domain(
            **{**graph, **scoped, "objects": tuple(objects), "globals": tuple(globals_)}
        )
    except (TypeError, KeyError, AttributeError) as error:
        raise ValueError("malformed named graph declaration") from error
    if domain.document() != value:
        raise ValueError("named graph provenance hash/schema mismatch")


ROW_FIELD = "named_global_object_contract"
CONTEXT_FIELD = "named_global_object_contracts"


def attach_context(document: dict[str, object]) -> None:
    contracts = {row["va"]: row[ROW_FIELD] for row in document["functions"] if ROW_FIELD in row}
    if contracts:
        document[CONTEXT_FIELD] = contracts


def validate_document(document: dict[str, object]) -> None:
    context = document.get("harness", document)
    expected = context.get(CONTEXT_FIELD)
    rows = document.get("functions", [])
    seen = set()
    for row in rows:
        if "named_global_code_safety" in row and ROW_FIELD not in row:
            raise ValueError("orphaned scoped code-safety runtime provenance")
        if ROW_FIELD in row:
            if row["va"] in seen:
                raise ValueError("duplicate named graph per-root provenance")
            seen.add(row["va"])
    actual = {row["va"]: row[ROW_FIELD] for row in rows if ROW_FIELD in row}
    if expected is None and not actual:
        return
    if not isinstance(expected, dict) or expected != actual:
        raise ValueError("inconsistent/missing named graph per-root provenance")
    for va, contract in actual.items():
        validate_contract(contract)
        root = int(va, 16) if isinstance(va, str) else va
        if contract["contract"]["graph"]["va"] != root:
            raise ValueError("named graph provenance belongs to another root")
        row = next(row for row in rows if row["va"] == va)
        if contract["contract"]["version"] == 2:
            validate_runtime(
                row.get("named_global_code_safety"),
                contract["contract"]["graph"]["code_safety"],
                row.get("cases"),
                context.get("seed"),
            )
        elif "named_global_code_safety" in row:
            raise ValueError("orphaned scoped code-safety runtime provenance")
