# SPDX-License-Identifier: GPL-3.0-or-later
"""List game-function naming candidates with their concrete evidence (T1261).

    python -m tools.name_candidates --lo 0x0 --hi 0x240000 --min-evidence 1
    python -m tools.name_candidates --va 0x00012380 --disasm

For every GAME function (region game, unnamed) in the range it disassembles the
body from the XBE with capstone and reports:

  * kernel imports called (`call/jmp dword [thunk slot]`, ordinal -> name),
  * library callees that already carry a name (functions.csv, FLIRT, overlay),
  * strings referenced by immediate operands (printable ASCII in the XBE),
  * named game callers/callees (the overlay) for propagation,
  * size, call count and whether it is a thin wrapper.

It only READS the XBE and generated/ files. It never prints owner data beyond short
string excerpts (--max-string) and writes nothing unless --out is given. Paths are
relative to the repository root.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.codediff.boundaries import function_table_rows
from tools.kernel_ordinals import KERNEL_ORDINALS, RESOLVED_ON_XDK_5849
from tools.naming_body import EVIDENCE_NOTICE, BodyBounds, BodyEvidence, decode_body, load_bounds
from tools.xbe.parser import parse_xbe

PLACEHOLDER = re.compile(r"^(FUN_|thunk_FUN_|CODEDIFF_|sub_)")
GENERIC = {"game_ftol_adjust", "__floor_default", "__ceil_default", "_sprintf"}
PRINTABLE = re.compile(rb"^[\x20-\x7e\t\r\n]{4,}$")


class Image:
    """XBE bytes addressed by virtual address."""

    def __init__(self, xbe_path: Path, functions_path: Path | None = None) -> None:
        self.data = xbe_path.read_bytes()
        self.bounds = load_bounds(functions_path) if functions_path is not None else {}
        self.body_reports: dict[int, BodyEvidence] = {}
        self.xbe = parse_xbe(self.data)
        self.thunk_lo = self.xbe.kernel_thunk_addr
        self.thunk_hi = self.thunk_lo + 4 * len(self.xbe.kernel_import_ordinals)

    def read(self, va: int, size: int) -> bytes | None:
        off = self.xbe.va_to_offset(va)
        if off is None:
            return None
        for section in self.xbe.sections:
            if section.virtual_addr <= va < section.virtual_addr + section.raw_size:
                available = section.raw_size - (va - section.virtual_addr)
                return self.data[off : off + min(size, available)]
        return None

    def body(self, md: Cs, entry: int, legacy_size: int) -> BodyEvidence:
        bounds = self.bounds.get(entry)
        if bounds is None:
            bounds = BodyBounds.from_row({"entry_va": hex(entry), "size_bytes": str(legacy_size)})
        result = decode_body(self.read, bounds, md)
        # Retain diagnostics only, not potentially millions of Capstone objects.
        self.body_reports[entry] = BodyEvidence(
            result.bounds, (), result.diagnostics, result.skipped_bytes
        )
        return result

    def report_bodies(self) -> None:
        counts = Counter(d for b in self.body_reports.values() for d in b.diagnostics)
        print(
            f"Naming body diagnostics: {dict(sorted(counts.items()))}; "
            "range membership is metadata-dependent; no complete indirect CFG claim",
            file=sys.stderr,
        )

    def cstring(self, va: int, limit: int = 160) -> str | None:
        """Printable NUL-terminated string at va, else None."""
        blob = self.read(va, limit)
        if not blob:
            return None
        end = blob.find(b"\x00")
        if end < 4:
            return None
        raw = blob[:end]
        if not PRINTABLE.match(raw):
            return None
        return raw.decode("ascii")

    def kernel_name(self, slot_va: int) -> str | None:
        if not self.thunk_lo <= slot_va < self.thunk_hi:
            return None
        ordinal = self.xbe.kernel_import_ordinals[(slot_va - self.thunk_lo) // 4]
        if ordinal in RESOLVED_ON_XDK_5849:
            return RESOLVED_ON_XDK_5849[ordinal][0]
        return KERNEL_ORDINALS.get(ordinal, f"ordinal_{ordinal}")


def read_functions(path: Path) -> list[dict[str, str]]:
    return function_table_rows(path)


def load_known_names(root: Path) -> dict[int, str]:
    """VA -> non-placeholder name from functions.csv, FLIRT and the overlay."""
    names: dict[int, str] = {}
    for row in read_functions(root / "generated/retail/functions.csv"):
        if not PLACEHOLDER.match(row["name"]):
            names[int(row["entry_va"], 16)] = row["name"]
    flirt = root / "generated/retail/flirt_names.csv"
    if flirt.exists():
        for row in read_functions(flirt):
            names.setdefault(int(row["entry_va"], 16), row["proposed_name"])
    overlay = root / "tools/data/function_names.csv"
    for row in read_functions(overlay):
        names[int(row["entry_va"], 16)] = row["name"]
    return names


def game_vas(root: Path) -> set[int]:
    """Unnamed GAME entries: the tools.coverage region split (library is excluded)."""
    rows = read_functions(root / "generated/retail/unnamed_functions.csv")
    return {int(r["entry_va"], 16) for r in rows if r["region"] == "game"}


def analyse(image: Image, entry: int, size: int, max_string: int) -> dict[str, object]:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    kernel: list[str] = []
    calls: list[int] = []
    strings: list[str] = []
    insn_count = 0
    body = image.body(md, entry, size)
    for insn in body.instructions:
        insn_count += 1
        if insn.mnemonic in ("call", "jmp") and insn.operands:
            op = insn.operands[0]
            if op.type == X86_OP_IMM:
                calls.append(op.imm)
            elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                name = image.kernel_name(op.mem.disp & 0xFFFFFFFF)
                if name:
                    kernel.append(name)
        for op in insn.operands:
            if op.type == X86_OP_IMM and insn.mnemonic in ("push", "mov", "lea"):
                text = image.cstring(op.imm & 0xFFFFFFFF)
                if text:
                    strings.append(text[:max_string])
            elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                text = image.cstring(op.mem.disp & 0xFFFFFFFF)
                if text and insn.mnemonic == "lea":
                    strings.append(text[:max_string])
    return {
        "insns": insn_count,
        "body_evidence": body.label,
        "kernel": sorted(set(kernel)),
        "calls": calls,
        "strings": list(dict.fromkeys(strings)),
    }


EVIDENCE_SOURCE = (
    "XBE disassembly via tools.name_candidates (generated/retail/functions.csv); "
    "docs/t1261-naming-low.md"
)


def append_names(root: Path, spec: Path, confidence: str, source: str = EVIDENCE_SOURCE) -> int:
    """Merge `va|name|evidence` lines into tools/data/function_names.csv, sorted by VA."""
    overlay = root / "tools/data/function_names.csv"
    with overlay.open(newline="") as handle:
        rows = list(csv.reader(handle))
    header, body = rows[0], rows[1:]
    taken_vas = {int(r[0], 16) for r in body}
    taken_names = {r[1] for r in body}
    functions = {
        int(r["entry_va"], 16) for r in read_functions(root / "generated/retail/functions.csv")
    }
    game = game_vas(root)
    added = 0
    for line in spec.read_text().splitlines():
        if not line.strip():
            continue
        va_text, name, evidence = (part.strip() for part in line.split("|", 2))
        va = int(va_text, 16)
        problems = []
        if va not in functions:
            problems.append("not a function entry")
        elif va not in game:
            problems.append("not game region")
        if va in taken_vas:
            problems.append("VA already named")
        if name in taken_names:
            problems.append("duplicate name")
        if not evidence:
            problems.append("empty evidence")
        if problems:
            print(f"SKIP {va_text} {name}: {', '.join(problems)}", file=sys.stderr)
            continue
        taken_vas.add(va)
        taken_names.add(name)
        body.append([f"0x{va:08x}", name, confidence, f"{evidence}; {source}"])
        added += 1
    body.sort(key=lambda r: int(r[0], 16))
    with overlay.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(body)
    print(f"added {added} rows, overlay now {len(body)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--lo", type=lambda s: int(s, 16), default=0x0)
    parser.add_argument("--hi", type=lambda s: int(s, 16), default=0x240000)
    parser.add_argument(
        "--va", type=lambda s: int(s, 16), action="append", help="analyse function(s)"
    )
    parser.add_argument(
        "--min-evidence", type=int, default=1, help="min strings+kernel+named callees"
    )
    parser.add_argument("--max-string", type=int, default=60)
    parser.add_argument("--disasm", action="store_true", help="with --va, print disassembly")
    parser.add_argument(
        "--drop-generic", action="store_true", help="ignore ftol/floor/sprintf callees"
    )
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--append", type=Path, help="merge 'va|name|evidence' lines into the overlay"
    )
    parser.add_argument("--confidence", default="INFERRED")
    parser.add_argument(
        "--evidence-source", default=EVIDENCE_SOURCE, help="suffix for appended evidence"
    )
    args = parser.parse_args()
    if args.append is not None:
        return append_names(args.root, args.append, args.confidence, args.evidence_source)

    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    game = game_vas(root)
    functions = {
        int(r["entry_va"], 16): int(r["size_bytes"])
        for r in read_functions(root / "generated/retail/functions.csv")
    }
    selected = (
        list(args.va)
        if args.va is not None
        else sorted(v for v in functions if args.lo <= v < args.hi and v in game)
    )
    callers: dict[int, list[int]] = defaultdict(list)
    results: dict[int, dict[str, object]] = {}
    for va in selected:
        info = analyse(image, va, functions[va], args.max_string)
        results[va] = info
        for target in info["calls"]:  # type: ignore[union-attr]
            callers[target].append(va)
    out = []
    for va, info in results.items():
        named_callees = sorted({known[t] for t in info["calls"] if t in known})  # type: ignore[union-attr]
        named_callees = (
            [n for n in named_callees if n not in GENERIC] if args.drop_generic else named_callees
        )
        score = len(info["strings"]) + len(info["kernel"]) + len(named_callees)  # type: ignore[arg-type]
        if args.va is None and score < args.min_evidence:
            continue
        out.append(
            {
                "va": f"0x{va:08x}",
                "size": functions[va],
                "body_evidence": info["body_evidence"],
                "insns": info["insns"],
                "kernel": info["kernel"],
                "strings": info["strings"],
                "named_callees": named_callees,
                "n_callers": len(callers.get(va, [])),
                "current": known.get(va, ""),
            }
        )
    image.report_bodies()
    if args.json:
        json.dump(out, sys.stdout, indent=1)
        return 0
    print(EVIDENCE_NOTICE)
    for item in out:
        print(
            f"{item['va']} size={item['size']} callers={item['n_callers']} "
            f"cur={item['current'] or '-'}"
        )
        print("   evidence:", item["body_evidence"])
        if item["kernel"]:
            print("   kernel:", ", ".join(item["kernel"]))
        if item["named_callees"]:
            print("   callees:", ", ".join(item["named_callees"]))
        for text in item["strings"]:
            print(f"   str: {text!r}")
        if args.disasm:
            md = Cs(CS_ARCH_X86, CS_MODE_32)
            start = int(str(item["va"]), 16)
            for insn in image.body(md, start, functions[start]).instructions:
                print(f"   {insn.address:08x}  {insn.mnemonic} {insn.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
