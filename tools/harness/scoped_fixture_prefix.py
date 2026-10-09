# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit known-code SUPERSET authority relative to a reviewed whole export.

This is not exact function membership or universal executable-code discovery.
Original references are tooling-owned and initially empty, never factory-signed.
"""

from __future__ import annotations

import csv
import io
from dataclasses import asdict, dataclass
from pathlib import Path

from . import scoped_fixture_authority as authority
from .effective_index import EffectiveIndex, pin_valid
from .image import SourceSection
from .scoped_fixture_authority import PhysicalSource, canonical, digest
from .seeding import GUEST_HI, GUEST_LO

HEADER = ("entry_va", "size_bytes", "name", "is_thunk", "body_max_va")
SEMANTICS = "ghidra-function-body-count-max-v1"
MAX_EXPORT_BYTES = 16 * 1024 * 1024
MAX_EXPORT_ROWS = 65536


def rows(raw: bytes, source: PhysicalSource) -> tuple[tuple[int, int, str, bool, int], ...]:
    if type(raw) is not bytes or type(source) is not PhysicalSource:
        raise ValueError("immutable actual export/source required")
    return _rows(
        raw,
        source.image.base,
        source.image.base + len(source.image.data),
        source.image.source_sections,
    )


def _rows(
    raw: bytes, base: int, image_end: int, sections: tuple[SourceSection, ...]
) -> tuple[tuple[int, int, str, bool, int], ...]:
    if type(raw) is not bytes or not 0 < len(raw) <= MAX_EXPORT_BYTES:
        raise ValueError("complete export exceeds bounded byte limit")
    reader = csv.DictReader(io.StringIO(raw.decode("utf-8")))
    if reader.fieldnames != list(HEADER):
        raise ValueError("unknown complete export header")
    output = []
    starts = set()
    for row in reader:
        if len(output) >= MAX_EXPORT_ROWS:
            raise ValueError("complete export exceeds bounded row limit")
        if set(row) != set(HEADER) or any(type(row[k]) is not str for k in HEADER):
            raise ValueError("malformed complete export row")
        try:
            start = int(row["entry_va"], 16)
            count = int(row["size_bytes"], 10)
            maximum = int(row["body_max_va"], 16)
        except (TypeError, ValueError) as error:
            raise ValueError("invalid complete export numbers") from error
        if row["is_thunk"] not in ("true", "false"):
            raise ValueError("unknown complete export thunk value")
        if (
            start in starts
            or not base <= start <= maximum < image_end
            or not 0 < count <= image_end - base
            or maximum >= 0xFFFFFFFF
        ):
            raise ValueError("duplicate or unmapped complete export extent")
        for address in (start, maximum):
            if not any(s.address <= address < s.address + s.size for s in sections):
                raise ValueError("complete export address lacks source-section ownership")
        starts.add(start)
        output.append((start, count, row["name"], row["is_thunk"] == "true", maximum))
    if not output:
        raise ValueError("empty complete export")
    return tuple(output)


@dataclass(frozen=True, slots=True)
class OriginalReference:
    identity: str
    image_sha256: str
    sections_sha256: str
    raw_sha256: str
    row_count: int
    membership_sha256: str
    exporter_sha256: str
    provenance_sha256: str
    exporter_path: str
    provenance_path: str
    semantics: str = SEMANTICS


# Only independently reviewed, published source/provenance pins may populate this.
# Factory/domain arguments cannot manufacture an original reference.
ORIGINAL_REFERENCES: tuple[OriginalReference, ...] = (
    OriginalReference(
        identity="retail-functions-05f42622-t1550-read-v1",
        image_sha256="f20f560cebea79f0eca3c111a19032c9d90c5d3a2c6071ea45be1c7ffa74e5ef",
        sections_sha256="44c1c2093c03c7ec7a48c6558a8f575b4b935524c0f2ee618cb30c2508eb6d69",
        raw_sha256="05f426225c494ffd94a6b3150e02eb2a97f199f49c66e86bbd275a60f7dbd9ee",
        row_count=12434,
        membership_sha256="b277fcf05ff07503589f5cdfdc02c02a797e91ff2fba2699577a78fe11ca8964",
        exporter_sha256="d5f18c918e868ab6defedfc51f6d9fc1456222e97eb94eb59029f5f6f8dfa412",
        provenance_sha256="7f7ec84e05645481df6eee406879152151fcaed1e6eae5f53c8187b6766912c4",
        exporter_path="tools/ghidra/ExportFunctionBounds.java",
        provenance_path="docs/evidence/t1550/original-prefix-reference-read-attempt1.json",
    ),
)


@dataclass(frozen=True, slots=True)
class PrefixAuthority:
    kind: str
    reference: str
    raw: bytes
    exporter_sha256: str
    provenance_sha256: str
    image_sha256: str
    sections_sha256: str
    row_count: int
    membership_sha256: str

    def validate_fields(self) -> None:
        if (
            type(self) is not PrefixAuthority
            or type(self.kind) is not str
            or self.kind not in ("original-export-prefix-v1", "synthetic-program-prefix-v1")
            or type(self.raw) is not bytes
            or not 0 < len(self.raw) <= MAX_EXPORT_BYTES
            or type(self.row_count) is not int
            or not 0 < self.row_count <= MAX_EXPORT_ROWS
            or type(self.reference) is not str
            or not self.reference
            or any(
                not pin_valid(pin)
                for pin in (
                    self.exporter_sha256,
                    self.provenance_sha256,
                    self.image_sha256,
                    self.sections_sha256,
                    self.membership_sha256,
                )
            )
        ):
            raise ValueError("incomplete prefix source/provenance pins")

    def protected(self, source: PhysicalSource, effective: bytes) -> tuple[int, int]:
        if type(self) is not PrefixAuthority or type(source) is not PhysicalSource:
            raise ValueError("recognized immutable prefix authority required")
        self.validate_fields()
        if (source.image.base, source.image.base + len(source.image.data)) != (GUEST_LO, GUEST_HI):
            raise ValueError("prefix authority supports only default complete guest mapping")
        source.check()
        raw_rows = rows(self.raw, source)
        selected_rows = rows(effective, source)
        if (
            self.image_sha256 != source.image_sha256
            or self.sections_sha256 != source.sections_sha256
            or type(self.row_count) is not int
            or self.row_count != len(raw_rows)
            or self.membership_sha256 != digest(canonical(raw_rows))
        ):
            raise ValueError("whole export/image/membership authority mismatch")
        if self.kind == "original-export-prefix-v1":
            if source.kind != "original-xbe":
                raise ValueError("original prefix requires actual original image")
            matches = [
                item
                for item in ORIGINAL_REFERENCES
                if type(item) is OriginalReference and item.identity == self.reference
            ]
            if len(matches) != 1:
                raise ValueError("independently trusted original reference unavailable")
            item = matches[0]
            for relative, expected in (
                (item.exporter_path, item.exporter_sha256),
                (item.provenance_path, item.provenance_sha256),
            ):
                path = Path(relative)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError("original reference path outside repository")
                actual = authority.ROOT / path
                if actual.resolve() != actual.absolute() or digest(actual.read_bytes()) != expected:
                    raise ValueError("original exporter/provenance live-source drift")
            if item.semantics != SEMANTICS or (
                item.image_sha256,
                item.sections_sha256,
                item.raw_sha256,
                item.row_count,
                item.membership_sha256,
                item.exporter_sha256,
                item.provenance_sha256,
            ) != (
                self.image_sha256,
                self.sections_sha256,
                digest(self.raw),
                self.row_count,
                self.membership_sha256,
                self.exporter_sha256,
                self.provenance_sha256,
            ):
                raise ValueError("independent original whole-export reference mismatch")
        elif self.kind == "synthetic-program-prefix-v1":
            if source.kind != "synthetic":
                raise ValueError("synthetic inventory cannot authorize original image")
        else:
            raise ValueError("unknown prefix authority variant")
        # Keep all raw maxima even when a legitimate override shortens a body.
        return source.image.base, 1 + max(row[4] for row in (*raw_rows, *selected_rows))

    def document(
        self, source: PhysicalSource, effective: bytes, layers: EffectiveIndex | None
    ) -> dict[str, object]:
        protected = self.protected(source, effective)
        if layers is not None and (
            type(layers) is not EffectiveIndex
            or layers.export != self.raw
            or layers.table != effective
        ):
            raise ValueError("prefix selected raw/layers differ")
        if layers is None and self.raw != effective:
            raise ValueError("raw prefix authority differs from selected raw source")
        return dict(
            version=1,
            authority=asdict(self) | {"raw": self.raw.hex()},
            mapped_start=source.image.base,
            mapped_end=source.image.base + len(source.image.data),
            protected_start=protected[0],
            protected_end=protected[1],
            effective=effective.hex(),
            overrides=layers.overrides.hex()
            if layers is not None and layers.overrides is not None
            else None,
            additions=layers.additions.hex()
            if layers is not None and layers.additions is not None
            else None,
        )


def validate_document(
    value: object,
    certificate: dict[str, object],
    sections: list[dict[str, object]],
    source_kind: str,
) -> tuple[int, int]:
    """Full reader: reconstruct all source rows/layers, not a claimed prefix/hash."""
    keys = {
        "version",
        "authority",
        "mapped_start",
        "mapped_end",
        "protected_start",
        "protected_end",
        "effective",
        "overrides",
        "additions",
    }
    if (
        type(value) is not dict
        or set(value) != keys
        or type(value["version"]) is not int
        or value["version"] != 1
    ):
        raise ValueError("unknown prefix document schema")
    encoded = value["authority"]
    fields = set(PrefixAuthority.__dataclass_fields__)
    if type(encoded) is not dict or set(encoded) != fields:
        raise ValueError("incomplete prefix authority fields")

    def raw_hex(item: object) -> bytes:
        if type(item) is not str or len(item) > 2 * MAX_EXPORT_BYTES or item != item.lower():
            raise ValueError("noncanonical prefix bytes")
        try:
            raw = bytes.fromhex(item)
        except ValueError as error:
            raise ValueError("invalid prefix bytes") from error
        if raw.hex() != item:
            raise ValueError("noncanonical prefix bytes")
        return raw

    raw = raw_hex(encoded["raw"])
    selected = raw_hex(value["effective"])
    meta = PrefixAuthority(**(encoded | {"raw": raw}))
    meta.validate_fields()
    lo, hi = value["mapped_start"], value["mapped_end"]
    if type(lo) is not int or type(hi) is not int or not 0 <= lo < hi <= 1 << 32:
        raise ValueError("invalid prefix mapped bounds")
    if (lo, hi) != (GUEST_LO, GUEST_HI):
        raise ValueError("prefix reader unsupported complete guest mapping")
    geometry = tuple(SourceSection(**section) for section in sections)
    a, b = _rows(raw, lo, hi, geometry), _rows(selected, lo, hi, geometry)
    if (
        type(meta.row_count) is not int
        or meta.row_count != len(a)
        or meta.membership_sha256 != digest(canonical(a))
        or meta.image_sha256 != certificate["image_sha256"]
        or meta.sections_sha256 != digest(canonical(sections))
        or digest(selected) != certificate["index_sha256"]
        or any(not pin_valid(pin) for pin in (meta.exporter_sha256, meta.provenance_sha256))
    ):
        raise ValueError("prefix reader source/membership binding mismatch")
    if certificate["index_kind"] == "effective":
        overrides = raw_hex(value["overrides"]) if value["overrides"] is not None else None
        additions = raw_hex(value["additions"]) if value["additions"] is not None else None
        effective = EffectiveIndex.build(raw, overrides, additions)
        if effective.table != selected or effective.document() != certificate["effective_source"]:
            raise ValueError("prefix reader selected role/layer mismatch")
    elif raw != selected or value["overrides"] is not None or value["additions"] is not None:
        raise ValueError("raw prefix reader refuses undeclared layers")
    if meta.kind == "original-export-prefix-v1":
        matches = [
            ref
            for ref in ORIGINAL_REFERENCES
            if type(ref) is OriginalReference and ref.identity == meta.reference
        ]
        if source_kind != "original-xbe" or len(matches) != 1:
            raise ValueError("prefix reader independent original reference unavailable")
        ref = matches[0]
        if ref.semantics != SEMANTICS or (
            ref.image_sha256,
            ref.sections_sha256,
            ref.raw_sha256,
            ref.row_count,
            ref.membership_sha256,
            ref.exporter_sha256,
            ref.provenance_sha256,
        ) != (
            meta.image_sha256,
            meta.sections_sha256,
            digest(raw),
            meta.row_count,
            meta.membership_sha256,
            meta.exporter_sha256,
            meta.provenance_sha256,
        ):
            raise ValueError("prefix reader trusted original reference mismatch")
        for relative, pin in (
            (ref.exporter_path, ref.exporter_sha256),
            (ref.provenance_path, ref.provenance_sha256),
        ):
            path = Path(relative)
            actual = authority.ROOT / path
            if (
                path.is_absolute()
                or ".." in path.parts
                or actual.resolve() != actual.absolute()
                or digest(actual.read_bytes()) != pin
            ):
                raise ValueError("prefix reader exporter/provenance source drift")
    elif meta.kind != "synthetic-program-prefix-v1" or source_kind != "synthetic":
        raise ValueError("prefix reader authority/source variant mismatch")
    protected = (lo, 1 + max(row[4] for row in (*a, *b)))
    if any(
        type(value[key]) is not int for key in ("protected_start", "protected_end")
    ) or protected != (value["protected_start"], value["protected_end"]):
        raise ValueError("prefix reader computed exclusion mismatch")
    return protected
