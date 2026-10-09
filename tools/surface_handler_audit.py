# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit that every registered XDK handler sits at an address whose function it implements.

The D3D8, DSound and XInput handlers are keyed by ADDRESS (see `docs/xdk-dispatch.md`). A handler
registered at a wrong or shifted address silently implements the wrong function, so this tool
joins, per handler-registered surface address:

  * the handler symbol and file, found statically by `tools/xdk_coverage.py`;
  * the function name the handler's own header comment claims (when it names one);
  * the name in the repo's generated surface table (`src/xbox/xdk_surface.c`);
  * the name in the executable's own `.XTLID` (`tools/xtlid_cli.py --out`);
  * the `tools/libsig` name (an optional, LOCAL-ONLY input);
  * the arity: the handler's claimed `ret N`, the binary's `ret imm16` (`xdk_abi.inc`) and the
    library `_Name@N` decoration.

PROVENANCE. libsig names derive from XDK libraries and may never be committed. The tool therefore
writes two outputs. `--out-public` carries only repo-surface and `.XTLID` names plus the class and
the libsig confidence, and is safe to commit. `--out-local` also carries the libsig names and is
refused unless it resolves under `tmp/` or `generated/` (`tools.libsig.guard`).

NAME NORMALISATION (`normalise`). Strip a leading `_` or `@` and a trailing `@N` (N is the argument
byte count, kept separately). A C++ symbol `?Method@Class@@...` becomes class `Class`, method
`Method`. A name `Class_Method` splits at the first `_` when `Class` is a known interface prefix
(`D3DDevice`, `IDirectSoundBuffer`, ...). Comparison is case-insensitive and ignores `_`. Two names
agree when the methods agree and, if both carry a class, the classes agree. A claim without a class
(`GetBackBuffer2`) matches on the method alone. Trailing digits are significant.

CLASSES. AGREE: the handler claim and at least one independent source name the same function and
nothing conflicts. ALIAS: the library match is an identical-code alias set and a named source is a
member. DISAGREE: two named sources conflict, or an alias set contains no named source's name.
SOURCE-MISSING: otherwise (the handler claims no name, or no other source names the address).

COVERAGE GUARD (T365). The classes above say how well an address is NAMED. They do not say that an
address was read. `docs/data/surface-handler-behaviour-audit.json` is the committed record of
which registered addresses were audited by behaviour (against the image), which registrations are
declared unmeasured (outside the 236-address surface, `d3d8_hle_register` rejects them in
production) and which indirect jumps (jump tables) were read. `coverage_gaps` fails, by address,
on a registration that is in none of those, on a record whose registration has gone, and on an
indirect jump in a handler that nobody reviewed, so a new handler at an unmeasured address cannot
go unaudited. `--xbe` adds the image's own `ret imm16` set per handler (a control-flow walk,
`tools.xdk_abi.walk_function`), compared with `xdk_abi.inc` and the header; `--report` compares
the native `xdk_report` registration JSON with the static extraction.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.libsig.guard import check_output_path  # noqa: E402
from tools.xdk_abi import FunctionWalk, SectionMap, executable_sections, walk_function  # noqa: E402
from tools.xdk_coverage import Extractor  # noqa: E402

CLASS_PREFIXES = (
    "D3DDevice",
    "D3DResource",
    "D3DTexture",
    "D3DSurface",
    "D3DCubeTexture",
    "D3DBaseTexture",
    "D3DVertexBuffer",
    "D3DIndexBuffer",
    "D3DVolumeTexture",
    "Direct3D",
    "IDirectSoundBuffer",
    "IDirectSoundStream",
    "IDirectSound",
)
SURFACE_ROW_RE = re.compile(r'\{0x([0-9a-fA-F]{8}),\s*"(\w+)",\s*(NULL|"([^"]*)")')
ABI_ROW_RE = re.compile(r"\{0x([0-9A-Fa-f]{8})u,\s*XDK_CC_(\w+),\s*(\d+)u,\s*(\d+)u,\s*(\w+),")
NAME_TOKEN_RE = re.compile(r"^[A-Z][A-Za-z0-9]*(?:_[A-Za-z0-9]+)*$")
STOP_TOKENS = {"ECX", "NULL", "ASCII", "DWORD", "CPU"}

AGREE = "AGREE"
ALIAS = "ALIAS"
DISAGREE = "DISAGREE"
SOURCE_MISSING = "SOURCE-MISSING"


@dataclass(frozen=True)
class Norm:
    klass: str | None
    method: str
    arg_bytes: int | None


def normalise(name: str) -> Norm:
    """Reduce a decorated or mangled symbol to (class, method, argument bytes)."""
    text = name.strip()
    arg_bytes: int | None = None
    klass: str | None = None
    if text.startswith("?"):
        parts = text[1:].split("@")
        method = parts[0]
        klass = parts[1] if len(parts) > 1 and parts[1] else None
        return Norm(klass.lower() if klass else None, method.lower().replace("_", ""), None)
    text = text.lstrip("_@")
    suffix = re.search(r"@(\d+)$", text)
    if suffix:
        arg_bytes = int(suffix.group(1))
        text = text[: suffix.start()]
    head, _, tail = text.partition("_")
    if tail and head in CLASS_PREFIXES:
        klass, text = head, tail
    return Norm(klass.lower() if klass else None, text.lower().replace("_", ""), arg_bytes)


def same_function(left: Norm, right: Norm) -> bool:
    if left.method != right.method:
        return False
    return left.klass is None or right.klass is None or left.klass == right.klass


def load_surface(path: Path) -> dict[int, tuple[str, str | None]]:
    """address -> (section, name or None) from the generated surface table."""
    rows: dict[int, tuple[str, str | None]] = {}
    for match in SURFACE_ROW_RE.finditer(path.read_text(encoding="utf-8")):
        rows[int(match.group(1), 16)] = (match.group(2), match.group(4))
    return rows


def load_xtlid(path: Path) -> dict[int, str]:
    """address -> name from the `tools.xtlid_cli --out` table."""
    names: dict[int, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        names[int(fields[0], 16)] = fields[1]
    return names


@dataclass
class LibsigRow:
    symbol: str
    confidence: str
    aliases: list[str] = field(default_factory=list)


def load_libsig(path: Path) -> dict[int, LibsigRow]:
    rows: dict[int, LibsigRow] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            aliases = [a for a in row.get("alias_set", "").split("|") if a]
            rows[int(row["address"], 16)] = LibsigRow(row["symbol"], row["confidence"], aliases)
    return rows


def load_abi_pop_args(path: Path) -> dict[int, tuple[int, int]]:
    """address -> (stack args, register args) from the generated `xdk_abi.inc`."""
    rows: dict[int, tuple[int, int]] = {}
    for match in ABI_ROW_RE.finditer(path.read_text(encoding="utf-8")):
        rows[int(match.group(1), 16)] = (int(match.group(3)), int(match.group(4)))
    return rows


def header_claim(
    root: Path, address: int, source_dirs: tuple[str, ...]
) -> tuple[str | None, int | None]:
    """The (name, stack args) a header comment gives for `address`, either may be None.

    Name: a CamelCase token right after `0xADDR,` / `0xADDR:` or in a `0xADDR  Name` table row.
    Arity: `retN` / `ret N` (bytes) or the argument count of `stdcall(a, b)` on that line.
    """
    hex_text = f"0x00{address:06X}"
    address_re = re.compile(hex_text + r"\b", re.IGNORECASE)
    name_re = re.compile(hex_text + r"[ ,:]+\s*([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
    arrow_re = re.compile(r"0x[0-9A-Fa-f]{8}\s*->\s*" + hex_text + r"\s+(\w+)", re.IGNORECASE)
    name: str | None = None
    arity: int | None = None
    for directory in source_dirs:
        for path in sorted((root / directory).glob("*.h")):
            for line in path.read_text(encoding="utf-8").splitlines():
                if not address_re.search(line):
                    continue
                for pattern in (name_re, arrow_re):
                    found = pattern.search(line)
                    if found and name is None and NAME_TOKEN_RE.match(found.group(1)):
                        if found.group(1) not in STOP_TOKENS and not found.group(1).islower():
                            name = found.group(1)
                ret = re.search(r"\bret\s*(\d+)\b", line)
                if ret and arity is None:
                    arity = int(ret.group(1)) // 4
                args = re.search(r"stdcall\(([^)]*)\)", line)
                if args and arity is None:
                    arity = len([a for a in args.group(1).split(",") if a.strip()])
    return name, arity


DISPATCH_ROW_RE = re.compile(r'\{\s*0x([0-9a-fA-F]{8})u?,\s*"(\w+)"')


def dispatch_claims(root: Path, source_dirs: tuple[str, ...]) -> dict[int, str]:
    """address -> name from hand-written `{0xADDR, "Name", ...}` rows in the handler modules."""
    names: dict[int, str] = {}
    for directory in source_dirs:
        for path in sorted((root / directory).glob("*.c")):
            for match in DISPATCH_ROW_RE.finditer(path.read_text(encoding="utf-8")):
                names.setdefault(int(match.group(1), 16), match.group(2))
    return names


def classify(
    claim: str | None, surface: str | None, xtlid: str | None, libsig: LibsigRow | None
) -> tuple[str, str]:
    """Return (class, note) for one address. See the module docstring for the rules."""
    named: dict[str, Norm] = {}
    for key, value in (("claim", claim), ("surface", surface), ("xtlid", xtlid)):
        if value:
            named[key] = normalise(value)
    unique = bool(libsig and libsig.symbol)
    if unique and libsig is not None:
        named["libsig"] = normalise(libsig.symbol)
    keys = list(named)
    for index, first in enumerate(keys):
        for second in keys[index + 1 :]:
            if not same_function(named[first], named[second]):
                return DISAGREE, f"{first} vs {second}"
    if libsig and not libsig.symbol and libsig.aliases:
        others = {k: v for k, v in named.items() if k != "libsig"}
        if not others:
            return SOURCE_MISSING, "alias set only"
        alias_norms = [normalise(a) for a in libsig.aliases]
        if any(same_function(v, a) for v in others.values() for a in alias_norms):
            return ALIAS, "named source is a member of the identical-code alias set"
        return DISAGREE, "alias set contains no named source"
    families = {("xtlid" if k in ("surface", "xtlid") else k) for k in named}
    if "claim" in named and len(families) >= 2:
        return AGREE, "+".join(sorted(families))
    if "claim" in named:
        return SOURCE_MISSING, "claim only"
    if "xtlid" in families:
        return SOURCE_MISSING, "no handler claim, name in .XTLID or surface"
    return SOURCE_MISSING, "no handler claim, no public name"


@dataclass(frozen=True)
class ImageScan:
    """What a control-flow walk of one handler's ORIGINAL function in the image found."""

    return_imms: tuple[int, ...]  # distinct `ret imm16` values (bytes), ascending
    return_vas: tuple[int, ...]
    unresolved_jumps: tuple[int, ...]  # indirect jumps (jump tables) the walk could not follow
    clean: bool  # no undecodable byte and no truncation


def scan_from_walk(walk: FunctionWalk) -> ImageScan:
    return ImageScan(
        tuple(sorted(walk.return_immediates)),
        tuple(sorted(walk.returns)),
        tuple(sorted(walk.unresolved_jumps)),
        walk.clean,
    )


def scan_image(xbe: Path, addresses: list[int]) -> dict[int, ImageScan]:
    sections = SectionMap(executable_sections(xbe))
    return {address: scan_from_walk(walk_function(sections, address)) for address in addresses}


def build_rows(
    root: Path,
    surface: dict[int, tuple[str, str | None]],
    xtlid: dict[int, str],
    libsig_primary: dict[int, LibsigRow],
    libsig_secondary: dict[int, LibsigRow],
    abi: dict[int, tuple[int, int]],
    source_dirs: tuple[str, ...] = ("src/gpu", "src/audio", "src/input"),
    image: dict[int, ImageScan] | None = None,
) -> list[dict]:
    registered, _ = Extractor(root).registrations()
    dispatch = dispatch_claims(root, source_dirs)
    rows: list[dict] = []
    for address in sorted(registered):
        if address not in surface:
            continue
        section, surface_name = surface[address]
        info = registered[address]
        claim, claim_args = header_claim(root, address, source_dirs)
        claim_source = "header" if claim else None
        if claim is None and address in dispatch:
            claim, claim_source = dispatch[address], "dispatch-table"
        primary = libsig_primary.get(address)
        secondary = libsig_secondary.get(address)
        used = primary if primary and (primary.symbol or primary.aliases) else secondary
        klass, note = classify(claim, surface_name, xtlid.get(address), used)
        lib_args = None
        if used and used.symbol:
            lib_args = normalise(used.symbol).arg_bytes
        binary_stack, binary_regs = abi.get(address, (None, None))
        binary_total = None if binary_stack is None else binary_stack + (binary_regs or 0)
        scan = image.get(address) if image is not None else None
        rows.append(
            {
                "address": f"0x{address:08x}",
                "section": section,
                "registry": info["registry"],
                "handler": info["handler"],
                "file": info["file"],
                "claim_name": claim,
                "claim_source": claim_source,
                "surface_name": surface_name,
                "xtlid_name": xtlid.get(address),
                "libsig_confidence": used.confidence if used else None,
                "libsig_name": used.symbol if used and used.symbol else None,
                "libsig_aliases": used.aliases if used and not used.symbol else [],
                "libsig_secondary_differs": bool(
                    primary and secondary and primary.symbol != secondary.symbol
                ),
                "class": klass,
                "note": note,
                "arity_handler_claim": claim_args,
                "arity_binary_stack": binary_stack,
                "arity_binary_total": binary_total,
                "arity_libsig_decoration": None if lib_args is None else lib_args // 4,
                "image_return_imms": None if scan is None else list(scan.return_imms),
                "image_return_vas": None
                if scan is None
                else [f"0x{v:08x}" for v in scan.return_vas],
                "image_unresolved_jumps": (
                    None if scan is None else [f"0x{v:08x}" for v in scan.unresolved_jumps]
                ),
                "image_clean": None if scan is None else scan.clean,
                "arity_image_stack": (
                    scan.return_imms[0] // 4
                    if scan is not None and len(scan.return_imms) == 1
                    else None
                ),
            }
        )
    return rows


def arity_mismatches(rows: list[dict]) -> list[dict]:
    """Rows whose arity sources (claim, binary `ret` plus register args, decoration) disagree.

    The claim is a stack count, so a claim is compared with the binary stack count instead.
    """
    out = []
    for row in rows:
        total = {
            row[k] for k in ("arity_binary_total", "arity_libsig_decoration") if row[k] is not None
        }
        claim_vs_stack = (
            row["arity_handler_claim"] is not None
            and row["arity_binary_stack"] is not None
            and row["arity_handler_claim"] != row["arity_binary_stack"]
        )
        imms = row.get("image_return_imms")
        image_stack = row.get("arity_image_stack")
        image_bad = imms is not None and (
            len(imms) != 1
            or not row.get("image_clean", True)
            or (row["arity_binary_stack"] is not None and image_stack != row["arity_binary_stack"])
            or (
                row["arity_handler_claim"] is not None and image_stack != row["arity_handler_claim"]
            )
        )
        if len(total) > 1 or claim_vs_stack or image_bad:
            out.append(row)
    return out


@dataclass
class Behaviour:
    """The committed record of what was read: `docs/data/surface-handler-behaviour-audit.json`.

    `audited`: address -> ids of the tasks whose pass compared the handler with the image.
    `unmeasured`: registered address outside the surface -> why it is not audited.
    `reviewed_jumps`: handler address -> the indirect jumps (jump tables) that were read.
    """

    audited: dict[int, list[str]]
    unmeasured: dict[int, str]
    reviewed_jumps: dict[int, set[int]]


def load_behaviour(path: Path) -> Behaviour:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 1:
        raise ValueError(f"{path}: unsupported schema {data.get('schema')!r}")
    audited = {int(k, 16): list(v) for k, v in data["audited"].items()}
    unmeasured = {int(k, 16): v for k, v in data["unmeasured"].items()}
    jumps = {
        int(k, 16): {int(j, 16) for j in v} for k, v in data["reviewed_indirect_jumps"].items()
    }
    return Behaviour(audited, unmeasured, jumps)


def coverage_gaps(
    registered: set[int],
    unresolved: list[dict],
    behaviour: Behaviour,
    surface: set[int] | None = None,
    rows: list[dict] | None = None,
) -> dict[str, list[str]]:
    """Every way a handler could be registered without having been read, as sorted address lists.

    An empty result is only meaningful when `registered` and `behaviour` are not empty, so both
    are checked first and an empty one is itself reported (an extractor that finds nothing would
    otherwise agree with an empty record).
    """
    gaps: dict[str, list[str]] = {}

    def add(kind: str, items: list[str]) -> None:
        if items:
            gaps[kind] = sorted(items)

    def hexes(values: set[int]) -> list[str]:
        return [f"0x{v:08x}" for v in values]

    if not registered:
        add("extractor-found-no-registrations", ["-"])
    if not behaviour.audited:
        add("behaviour-record-empty", ["-"])
    add(
        "unresolved-registration",
        [f"{u['file']}:{u['line']} {u['expression']}" for u in unresolved],
    )
    recorded = set(behaviour.audited) | set(behaviour.unmeasured)
    add("registered-but-unaudited", hexes(registered - recorded))
    add("recorded-but-not-registered", hexes(recorded - registered))
    add(
        "audited-and-declared-unmeasured", hexes(set(behaviour.audited) & set(behaviour.unmeasured))
    )
    if surface is not None:
        add("audited-but-outside-surface", hexes(set(behaviour.audited) - surface))
        add("declared-unmeasured-but-in-surface", hexes(set(behaviour.unmeasured) & surface))
    if rows is not None:
        found = {
            int(row["address"], 16): {int(j, 16) for j in row.get("image_unresolved_jumps") or []}
            for row in rows
            if row.get("image_unresolved_jumps") is not None
        }
        add(
            "unreviewed-indirect-jump",
            [
                f"0x{address:08x}->0x{jump:08x}"
                for address, jumps in found.items()
                for jump in jumps - behaviour.reviewed_jumps.get(address, set())
            ],
        )
        add(
            "reviewed-indirect-jump-gone",
            [
                f"0x{address:08x}->0x{jump:08x}"
                for address, jumps in behaviour.reviewed_jumps.items()
                if address in found
                for jump in jumps - found[address]
            ],
        )
    return gaps


def report_gaps(report: dict, expected: set[int]) -> dict[str, list[str]]:
    """Compare the native `xdk_report` registration JSON with the statically found registrations.

    `expected` is the measured, handler-registered set. The native report lists registered
    addresses only inside the surface, so the same set is the right comparison.
    """
    if report.get("kind") != "xdk-handler-registration":
        return {"not-a-registration-report": [str(report.get("kind"))]}
    native = {int(e["address"], 16) for e in report["entries"] if e["registered_handler"]}
    gaps: dict[str, list[str]] = {}
    if not native:
        gaps["native-report-empty"] = ["-"]
    if expected - native:
        gaps["static-but-not-in-native-report"] = sorted(f"0x{a:08x}" for a in expected - native)
    if native - expected:
        gaps["native-report-but-not-static"] = sorted(f"0x{a:08x}" for a in native - expected)
    return gaps


PUBLIC_DROP = ("libsig_name", "libsig_aliases", "arity_libsig_decoration")


def public_rows(rows: list[dict]) -> list[dict]:
    """Rows without any libsig-derived name (safe to commit)."""
    return [{k: v for k, v in row.items() if k not in PUBLIC_DROP} for row in rows]


def counts(rows: list[dict]) -> dict[str, dict[str, int]]:
    table: dict[str, dict[str, int]] = {}
    for row in rows:
        bucket = table.setdefault(row["section"], {})
        bucket[row["class"]] = bucket.get(row["class"], 0) + 1
    return table


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    parser.add_argument("--surface", type=Path, required=True, help="generated xdk_surface.c")
    parser.add_argument("--xtlid", type=Path, required=True, help="tools.xtlid_cli --out table")
    parser.add_argument("--libsig", type=Path, help="primary libsig match CSV (local only)")
    parser.add_argument("--libsig-alt", type=Path, help="secondary libsig match CSV (local only)")
    parser.add_argument("--abi", type=Path, help="generated xdk_abi.inc (binary ret imm16)")
    parser.add_argument("--xbe", type=Path, help="retail default.xbe: walk each handler's original")
    parser.add_argument(
        "--behaviour", type=Path, help="docs/data/surface-handler-behaviour-audit.json (guard)"
    )
    parser.add_argument("--report", type=Path, help="native xdk_report registration JSON")
    parser.add_argument("--out-public", type=Path, help="JSON without libsig names")
    parser.add_argument(
        "--out-local", type=Path, help="JSON with libsig names (tmp/ or generated/)"
    )
    args = parser.parse_args(argv)
    if args.out_local is not None:
        check_output_path(args.out_local)
    if args.out_public is not None and (args.libsig or args.libsig_alt):
        print("note: --out-public drops libsig names by construction", file=sys.stderr)
    surface = load_surface(args.surface)
    registered, unresolved = Extractor(args.root).registrations()
    measured = sorted(address for address in registered if address in surface)
    rows = build_rows(
        args.root,
        surface,
        load_xtlid(args.xtlid),
        load_libsig(args.libsig) if args.libsig else {},
        load_libsig(args.libsig_alt) if args.libsig_alt else {},
        load_abi_pop_args(args.abi) if args.abi else {},
        image=scan_image(args.xbe, measured) if args.xbe else None,
    )
    if args.out_public is not None:
        args.out_public.write_text(json.dumps(public_rows(rows), indent=1) + "\n", encoding="utf-8")
    if args.out_local is not None:
        args.out_local.write_text(json.dumps(rows, indent=1) + "\n", encoding="utf-8")
    for section, bucket in sorted(counts(rows).items()):
        print(section, dict(sorted(bucket.items())))
    mismatched = arity_mismatches(rows)
    print("arity mismatches:", len(mismatched))
    gaps: dict[str, list[str]] = {}
    for row in mismatched:
        gaps.setdefault("arity-mismatch", []).append(row["address"])
    if args.behaviour is not None:
        gaps.update(
            coverage_gaps(
                set(registered), unresolved, load_behaviour(args.behaviour), set(surface), rows
            )
        )
    if args.report is not None:
        gaps.update(report_gaps(json.loads(args.report.read_text(encoding="utf-8")), set(measured)))
    for kind, items in sorted(gaps.items()):
        print(f"GAP {kind}: {' '.join(items)}", file=sys.stderr)
    print("registered:", len(registered), "measured:", len(measured), "gaps:", len(gaps))
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
