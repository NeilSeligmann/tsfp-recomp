# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent physical and frozen-stream authority for custom scalar fixtures.

No CPU execution or protocol capability is supplied by this foundation. Original
source authority is rebuilt from the selected XBE; synthetic authority stays marked
synthetic. Physical checks always precede independent expected-stream comparison.
"""

from __future__ import annotations

import csv
import hashlib
import importlib
import inspect
import io
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from types import CodeType, FunctionType

from .image import GuestImage, SourceSection, build_guest_image
from .model import Case
from .seeding import SeedPolicy

CUSTOM_RUNTIME_VALIDATED = False
CONTRACTS: tuple[object, ...] = ()
ROOT = Path(__file__).resolve().parents[2]


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def canonical(value: object) -> bytes:
    def encode(item: object) -> object:
        if isinstance(item, bytes):
            return {"bytes": item.hex()}
        if isinstance(item, tuple | list):
            return [encode(part) for part in item]
        if isinstance(item, dict):
            return {key: encode(part) for key, part in item.items()}
        if isinstance(item, float):
            return {"float": item.hex()}
        return item

    return json.dumps(
        encode(value), sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def case_digest(case: Case) -> str:
    if (
        type(case) is not Case
        or any(
            type(value) is not int for value in (case.seed, case.index, case.va, case.size, case.df)
        )
        or not 0 <= case.va < 1 << 32
        or case.size <= 0
        or case.index < 0
        or case.df not in (0, 1)
        or type(case.regs) is not tuple
        or len(case.regs) != 8
        or any(type(value) is not int or not 0 <= value < 1 << 32 for value in case.regs)
        or type(case.patches) is not tuple
        or any(
            type(patch) is not tuple
            or len(patch) != 2
            or type(patch[0]) is not int
            or type(patch[1]) is not bytes
            for patch in case.patches
        )
    ):
        raise ValueError("immutable canonical Case required")
    return digest(canonical(asdict(case)))


def supported_policy(policy: SeedPolicy) -> None:
    if type(policy) is not SeedPolicy or policy != SeedPolicy():
        raise ValueError("unsupported custom fixture seed policy")


def dependency_pins(directories: tuple[str, ...]) -> tuple[tuple[str, str], ...]:
    if (
        type(directories) is not tuple
        or not directories
        or len(set(directories)) != len(directories)
        or "tools" not in directories
    ):
        raise ValueError("explicit dependency directories required")
    paths: set[Path] = set()
    for directory in directories:
        relative = Path(directory)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("dependency directory outside repository")
        path = ROOT / relative
        if not path.is_dir() or path.resolve() != path.absolute():
            raise ValueError("missing or symlinked dependency directory")
        for member in path.rglob("*.py"):
            if member.resolve() != member.absolute():
                raise ValueError("symlinked dependency member")
            paths.add(member)
    if not paths:
        raise ValueError("empty dependency membership")
    return tuple(sorted((str(path.relative_to(ROOT)), digest(path.read_bytes())) for path in paths))


@dataclass(frozen=True, slots=True)
class Dependencies:
    directories: tuple[str, ...]
    pins: tuple[tuple[str, str], ...]

    @classmethod
    def capture(cls, directories: tuple[str, ...]) -> Dependencies:
        return cls(directories, dependency_pins(directories))

    def check(self) -> None:
        if type(self) is not Dependencies or type(self.pins) is not tuple:
            raise ValueError("immutable dependency identity required")
        if self.pins != dependency_pins(self.directories):
            raise ValueError("custom fixture dependency content/membership drift")


def code_fingerprint(code: CodeType) -> str:
    """Pin immutable bytecode semantics without marshal's live intern/reference state."""

    def constant(value: object) -> object:
        if type(value) is CodeType:
            return payload(value)
        if type(value) is tuple:
            return {"tuple": [constant(item) for item in value]}
        if type(value) is frozenset:
            return {"frozenset": sorted((constant(item) for item in value), key=canonical)}
        if type(value) is complex:
            return {"complex": [value.real.hex(), value.imag.hex()]}
        if value is Ellipsis:
            return {"ellipsis": True}
        if value is None or type(value) in (bool, int, str, bytes, float):
            return {type(value).__name__: value}
        raise ValueError("unsupported loaded factory constant")

    def payload(value: CodeType) -> dict[str, object]:
        return {
            name: getattr(value, name)
            for name in (
                "co_argcount",
                "co_posonlyargcount",
                "co_kwonlyargcount",
                "co_nlocals",
                "co_stacksize",
                "co_flags",
                "co_code",
                "co_names",
                "co_varnames",
                "co_freevars",
                "co_cellvars",
                "co_exceptiontable",
                "co_name",
                "co_qualname",
            )
        } | {"co_consts": [constant(item) for item in value.co_consts]}

    if type(code) is not CodeType:
        raise ValueError("actual immutable loaded code required")
    return digest(canonical(payload(code)))


@dataclass(frozen=True, slots=True)
class FactoryIdentity:
    root: int
    label: str
    namespace: int
    module: str
    name: str
    source_path: str
    source_sha256: str
    code_sha256: str
    dependencies: Dependencies

    @classmethod
    def capture(
        cls, root: int, label: str, namespace: int, factory: object, dependencies: Dependencies
    ) -> FactoryIdentity:
        if type(factory) is not FunctionType or factory.__closure__ or "<" in factory.__qualname__:
            raise ValueError("unsupported loaded fixture factory")
        module = importlib.import_module(factory.__module__)
        if getattr(module, factory.__name__, None) is not factory:
            raise ValueError("factory is not actual loaded module callable")
        filename = inspect.getsourcefile(factory)
        if filename is None:
            raise ValueError("factory source unavailable")
        path = Path(filename).absolute()
        if path.resolve() != path or not path.is_relative_to(ROOT):
            raise ValueError("factory source outside repository")
        relative = str(path.relative_to(ROOT))
        source_sha = digest(path.read_bytes())
        if type(dependencies) is not Dependencies:
            raise ValueError("immutable factory dependencies required")
        dependencies.check()
        if (relative, source_sha) not in dependencies.pins:
            raise ValueError("factory omitted from dependency membership")
        if (
            type(root) is not int
            or not 0 <= root < 1 << 32
            or type(namespace) is not int
            or not 0 <= namespace < 1 << 16
            or type(label) is not str
            or not label
        ):
            raise ValueError("invalid factory root/label/namespace")
        return cls(
            root,
            label,
            namespace,
            factory.__module__,
            factory.__name__,
            relative,
            source_sha,
            code_fingerprint(factory.__code__),
            dependencies,
        )

    def check(self, root: int, label: str, namespace: int, factory: object) -> None:
        if self != self.capture(root, label, namespace, factory, self.dependencies):
            raise ValueError("loaded fixture identity/source drift")


@dataclass(frozen=True, slots=True)
class PhysicalSource:
    image: GuestImage
    kind: str
    image_sha256: str
    sections_sha256: str
    source_path: Path | None
    source_sha256: str | None
    dependencies: Dependencies

    @classmethod
    def synthetic(cls, image: GuestImage, dependencies: Dependencies) -> PhysicalSource:
        return cls._capture(image, "synthetic", None, None, dependencies)

    @classmethod
    def original(
        cls, path: Path, image: GuestImage, dependencies: Dependencies, policy: SeedPolicy
    ) -> PhysicalSource:
        supported_policy(policy)
        raw = path.read_bytes()
        actual = build_guest_image(path, policy=policy)
        if path.read_bytes() != raw or image != actual:
            raise ValueError("actual original complete image/sections mismatch or drift")
        return cls._capture(image, "original-xbe", path, digest(raw), dependencies)

    @classmethod
    def _capture(
        cls,
        image: GuestImage,
        kind: str,
        path: Path | None,
        pin: str | None,
        dependencies: Dependencies,
    ) -> PhysicalSource:
        if (
            type(image) is not GuestImage
            or type(image.data) is not bytes
            or type(image.base) is not int
            or not 0 <= image.base < image.base + len(image.data) <= 1 << 32
            or type(image.source_sections) is not tuple
            or not image.source_sections
            or any(type(section) is not SourceSection for section in image.source_sections)
        ):
            raise ValueError("immutable complete physical source required")
        sections = image.source_sections
        if any(
            a.address < b.address + b.size and b.address < a.address + a.size
            for n, a in enumerate(sections)
            for b in sections[:n]
        ):
            raise ValueError("ambiguous physical source sections")
        if type(dependencies) is not Dependencies:
            raise ValueError("immutable physical source dependencies required")
        dependencies.check()
        return cls(
            image,
            kind,
            digest(image.data),
            digest(canonical([asdict(s) for s in sections])),
            path,
            pin,
            dependencies,
        )

    def check(self) -> None:
        if type(self) is not PhysicalSource or type(self.dependencies) is not Dependencies:
            raise ValueError("immutable physical source identity required")
        self.dependencies.check()
        if self.kind == "original-xbe":
            if (
                self.source_path is None
                or digest(self.source_path.read_bytes()) != self.source_sha256
            ):
                raise ValueError("actual original source drift")
            # A constructed object cannot assert an arbitrary subset of sections.
            if build_guest_image(self.source_path, policy=SeedPolicy()) != self.image:
                raise ValueError("actual original complete image/sections mismatch")
        elif (
            self.kind != "synthetic"
            or self.source_path is not None
            or self.source_sha256 is not None
        ):
            raise ValueError("unknown physical source authority")
        if (
            self._capture(
                self.image, self.kind, self.source_path, self.source_sha256, self.dependencies
            )
            != self
        ):
            raise ValueError("physical source identity mismatch")


def overlap(first: tuple[int, int], second: tuple[int, int]) -> bool:
    return first[0] < second[1] and second[0] < first[1]


def interval(lo: int, hi: int) -> None:
    if type(lo) is not int or type(hi) is not int or not 0 <= lo < hi <= 1 << 32:
        raise ValueError("invalid physical interval")


@dataclass(frozen=True, slots=True)
class Region:
    role: str
    kind: str
    start: int
    end: int
    width: int

    def validate(self) -> None:
        interval(self.start, self.end)
        if (
            type(self.role) is not str
            or not self.role
            or self.role.startswith("baseline-")
            or type(self.kind) is not str
            or self.kind not in ("source", "guest")
            or type(self.width) is not int
            or self.width not in (1, 2, 4, 8)
            or (self.end - self.start) % self.width
        ):
            raise ValueError("invalid explicit patch role")

    def owns(self, span: tuple[int, int]) -> bool:
        return (
            self.start <= span[0] < span[1] <= self.end
            and span[1] - span[0] == self.width
            and (span[0] - self.start) % self.width == 0
        )


@dataclass(frozen=True, slots=True)
class ArgumentOverlay:
    """Explicit later 4-byte overwrite of one cdecl argument slot (S+4*slot).

    The return word S..S+4 and every other frame byte stay protected.
    """

    role: str
    slot: int
    abi: str = "cdecl"

    def validate(self, frame_args: int) -> None:
        if (
            type(self) is not ArgumentOverlay
            or type(self.role) is not str
            or not self.role
            or self.role.startswith("baseline-")
            or type(self.slot) is not int
            or not 1 <= self.slot <= frame_args
            or self.abi != "cdecl"
        ):
            raise ValueError("invalid explicit argument overlay role")


def code_extents(index: bytes) -> tuple[tuple[int, int], ...]:
    if type(index) is not bytes:
        raise ValueError("actual immutable function index required")
    reader = csv.DictReader(io.StringIO(index.decode()))
    if reader.fieldnames != ["entry_va", "size_bytes", "name", "is_thunk", "body_max_va"]:
        raise ValueError("unknown function index shape")
    ranges, starts = [], set()
    for row in reader:
        if set(row) != set(reader.fieldnames):
            raise ValueError("malformed function index row")
        try:
            start, size, end = (
                int(row["entry_va"], 16),
                int(row["size_bytes"]),
                int(row["body_max_va"], 16) + 1,
            )
        except (ValueError, TypeError) as error:
            raise ValueError("malformed function extent") from error
        interval(start, end)
        if start in starts or size <= 0 or start + size > end:
            raise ValueError("duplicate or inconsistent function extent")
        starts.add(start)
        ranges.append((start, end))
    if not ranges:
        raise ValueError("empty known code extents")
    return tuple(sorted(ranges))


@dataclass(frozen=True, slots=True)
class PhysicalPlan:
    source: PhysicalSource
    index: bytes
    regions: tuple[Region, ...]
    policy: SeedPolicy = SeedPolicy()
    known_code: object | None = None
    argument_overlays: tuple[ArgumentOverlay, ...] = ()

    def code_ranges(self) -> tuple[tuple[int, int], ...]:
        if self.known_code is None:
            return code_extents(self.index)
        from .scoped_fixture_prefix import PrefixAuthority

        if type(self.known_code) is not PrefixAuthority:
            raise ValueError("recognized immutable known-code prefix authority required")
        return (self.known_code.protected(self.source, self.index),)

    def validate(self) -> None:
        if type(self) is not PhysicalPlan or type(self.source) is not PhysicalSource:
            raise ValueError("immutable physical plan/source required")
        supported_policy(self.policy)
        self.source.check()
        code = self.code_ranges()
        overlays = self.argument_overlays
        if type(overlays) is not tuple:
            raise ValueError("immutable argument overlay roles required")
        for n, overlay in enumerate(overlays):
            if type(overlay) is not ArgumentOverlay:
                raise ValueError("invalid explicit argument overlay role")
            overlay.validate(self.policy.frame_args)
            if any(overlay.role == other.role for other in (*overlays[:n], *self.regions)):
                raise ValueError("argument overlay role collides with another role")
            if any(overlay.slot == other.slot for other in overlays[:n]):
                raise ValueError("duplicate argument overlay slot")
        if type(self.regions) is not tuple or any(
            type(region) is not Region for region in self.regions
        ):
            raise ValueError("immutable patch regions required")
        image, sections = self.source.image, self.source.image.source_sections
        for n, region in enumerate(self.regions):
            region.validate()
            span = (region.start, region.end)
            if not image.base <= span[0] < span[1] <= image.base + len(image.data):
                raise ValueError("unmapped allocation/role")
            if any(overlap(span, (other.start, other.end)) for other in self.regions[:n]):
                raise ValueError("overlapping explicit allocations/roles")
            protected = (
                (self.policy.sentinel, self.policy.sentinel + 4),
                (
                    self.policy.stack_base,
                    self.policy.stack_base + 0x7FF0 + 4 + 4 * self.policy.frame_args,
                ),
                (
                    self.policy.scratch_base,
                    self.policy.scratch_base + self.policy.scratch_fill_bytes,
                ),
            )
            if any(overlap(span, other) for other in (*code, *protected)):
                raise ValueError("allocation/role overlaps code or seeded protected region")
            if region.kind == "source":
                if not any(
                    s.writable and s.address <= span[0] < span[1] <= s.address + s.size
                    for s in sections
                ):
                    raise ValueError("source role not wholly writable")
            elif any(overlap(span, (s.address, s.address + s.size)) for s in sections):
                raise ValueError("guest allocation overlaps original source")

    def check(self, case: Case, roles: tuple[str, ...]) -> None:
        self.validate()
        case_digest(case)
        if (
            type(case) is not Case
            or type(case.patches) is not tuple
            or type(roles) is not tuple
            or len(roles) != len(case.patches)
        ):
            raise ValueError("complete ordered immutable patches/roles required")
        if len(roles) < 2 or roles[:2] != ("baseline-frame", "baseline-scratch"):
            raise ValueError("complete ordered baseline roles required")
        esp, frame_size = case.esp, 4 + 4 * self.policy.frame_args
        if (
            not self.policy.stack_base <= esp <= self.policy.stack_base + 0x7FF0
            or (esp - self.policy.stack_base) % 16
        ):
            raise ValueError("seeded frame ownership")
        frame = (esp, esp + frame_size)
        image, sections, code = (
            self.source.image,
            self.source.image.source_sections,
            self.code_ranges(),
        )
        for n, (patch, role) in enumerate(zip(case.patches, roles, strict=True)):
            if (
                type(patch) is not tuple
                or len(patch) != 2
                or type(patch[1]) is not bytes
                or not patch[1]
            ):
                raise ValueError("invalid patch bytes/shape")
            at, raw = patch
            if type(at) is not int:
                raise ValueError("invalid physical patch address")
            interval(at, at + len(raw))
            span = at, at + len(raw)
            if not image.base <= span[0] < span[1] <= image.base + len(image.data):
                raise ValueError("patch outside actual mapped extent")
            if any(
                overlap(span, other)
                for other in (*code, (self.policy.sentinel, self.policy.sentinel + 4))
            ):
                raise ValueError("patch overlaps code or sentinel")
            if n == 0:
                if span != frame or raw[:4] != self.policy.sentinel.to_bytes(4, "little"):
                    raise ValueError("complete baseline frame/sentinel required")
            elif overlap(span, frame):
                slots = [o for o in self.argument_overlays if o.role == role]
                if len(slots) != 1 or span != (
                    esp + 4 * slots[0].slot,
                    esp + 4 * slots[0].slot + 4,
                ):
                    raise ValueError("later patch overlaps complete active frame")
                if any(
                    overlap(span, (a, a + len(b)))
                    for (a, b), r in zip(case.patches[2:n], roles[2:n], strict=True)
                    if r in {o.role for o in self.argument_overlays}
                ):
                    raise ValueError("argument overlay slot repeated in one case")
                continue
            if n < 2:
                if any(overlap(span, (s.address, s.address + s.size)) for s in sections):
                    raise ValueError("baseline allocation overlaps source")
                if n == 1 and span != (
                    self.policy.scratch_base,
                    self.policy.scratch_base + self.policy.scratch_fill_bytes,
                ):
                    raise ValueError("complete baseline scratch required")
                continue
            matches = [
                region for region in self.regions if region.role == role and region.owns(span)
            ]
            if len(matches) != 1:
                raise ValueError("unknown role or patch lacks exact physical ownership")


@dataclass(frozen=True, slots=True)
class ExpectedCase:
    digest: str
    roles: tuple[str, ...]

    @classmethod
    def capture(cls, case: Case, roles: tuple[str, ...]) -> ExpectedCase:
        return cls(case_digest(case), roles)


@dataclass(frozen=True, slots=True)
class FrozenStream:
    seed: int
    root: int
    namespace: int
    expected: tuple[ExpectedCase, ...]

    def validate(self) -> None:
        if (
            type(self.seed) is not int
            or type(self.root) is not int
            or not 0 <= self.root < 1 << 32
            or type(self.namespace) is not int
            or not 0 <= self.namespace < 1 << 16
            or type(self.expected) is not tuple
            or not self.expected
            or any(type(item) is not ExpectedCase for item in self.expected)
        ):
            raise ValueError("invalid frozen provider stream")
        for item in self.expected:
            if type(item.roles) is not tuple or any(type(role) is not str for role in item.roles):
                raise ValueError("immutable complete expected roles required")
            if (
                type(item.digest) is not str
                or len(item.digest) != 64
                or any(c not in "0123456789abcdef" for c in item.digest)
            ):
                raise ValueError("invalid expected case digest")

    def check(self, plan: PhysicalPlan, case: Case, ordinal: int, roles: tuple[str, ...]) -> None:
        # The expected stream cannot legitimize an invalid physical write.
        plan.check(case, roles)
        self.validate()
        if type(ordinal) is not int or not 0 <= ordinal < len(self.expected):
            raise ValueError("provider ordinal/count mismatch")
        if (case.seed, case.va, case.index) != (
            self.seed,
            self.root,
            (self.namespace << 40) + ordinal,
        ):
            raise ValueError("provider seed/root/namespace/index mismatch")
        expected = self.expected[ordinal]
        if expected.roles != roles or expected.digest != case_digest(case):
            raise ValueError("complete ordered provider stream differs")

    def document(self) -> dict[str, object]:
        self.validate()
        return dict(
            seed=self.seed,
            root=self.root,
            namespace=self.namespace,
            count=len(self.expected),
            digest=digest(canonical([asdict(item) for item in self.expected])),
        )
