# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed `tools/data/function_names.csv` from the registered src/game replacements.

Every registered replacement already carries a chosen name (for example
`game_pair_span` at 0x00156e70). This derives one overlay row per registered
function present in `functions.csv`:

* confidence MEASURED when the function passed every proof gate in the replacement
  proof (`tools.coverage.evaluate_replacements`, the same gates the decompiled
  badge uses), INFERRED when it is registered but unproven or the proof is stale;
* evidence names the source file and the proof state.

Output is deterministic (sorted by VA, fixed evidence text, no dates or hashes).
Only the registry is read. Nothing under generated/ is written.

    python3 -m tools.seed_function_names --manifest generated/replace/manifest.json \\
        --proof generated/replace/proof.json --functions generated/retail/functions.csv
"""

from __future__ import annotations

import argparse
import csv
import io
import re
import sys
from pathlib import Path

from tools.codediff.boundaries import load_function_table_for_export
from tools.coverage import (
    DEFAULT_NAME_OVERLAY,
    NAME_OVERLAY_COLUMNS,
    evaluate_replacements,
    is_placeholder_name,
    read_replacement_manifest,
    read_replacement_proof,
)

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def find_source(src_dir: Path, basename: str) -> str:
    """Repository-relative path of a registered source file, or the bare name."""
    matches = sorted(src_dir.rglob(basename)) if src_dir.is_dir() else []
    return matches[0].as_posix() if len(matches) == 1 else basename


def build_rows(
    manifest_path: Path,
    proof_path: Path,
    functions_path: Path,
    src_dir: Path,
    log: list[str],
) -> list[tuple[str, str, str, str]]:
    manifest = read_replacement_manifest(manifest_path)
    known = {function.entry_va for function in load_function_table_for_export(functions_path)}
    proven: set[int] = set()
    try:
        proof = read_replacement_proof(proof_path)
        evaluation = evaluate_replacements(
            manifest,
            proof,
            game_vas=frozenset(function.va for function in manifest.functions),
            game_total=len(manifest.functions),
        )
        proven = {function.va for function in evaluation.proven}
    except (ValueError, OSError) as error:
        log.append(f"proof unusable, every row INFERRED: {error}")
    rows: list[tuple[str, str, str, str]] = []
    for function in sorted(manifest.functions, key=lambda item: item.va):
        if function.va not in known:
            log.append(f"skip 0x{function.va:08x} {function.name}: not in functions.csv")
            continue
        if IDENTIFIER.match(function.name) is None or is_placeholder_name(function.name):
            log.append(f"skip 0x{function.va:08x} {function.name!r}: not a usable identifier")
            continue
        source = find_source(src_dir, function.source)
        if function.va in proven:
            confidence = "MEASURED"
            evidence = (
                f"{source} replacement proven by tools.replace prove (generated/replace/proof.json)"
            )
        else:
            confidence = "INFERRED"
            evidence = f"{source} registered replacement, no passing proof receipt"
        rows.append((f"0x{function.va:08x}", function.name, confidence, evidence))
    return rows


SEED_EVIDENCE = re.compile(
    r"(replacement proven by tools\.replace|registered replacement, no passing)"
)


def keep_curated(
    rows: list[tuple[str, str, str, str]], existing: Path
) -> list[tuple[str, str, str, str]]:
    """Add rows of the current overlay that were NOT derived from the registry.

    Evidence-named rows (T1261, tools.name_candidates) share the file with the seed.
    A row is registry-derived when its evidence has the seed wording, such rows are
    regenerated, every other row is curated and always kept, it wins over a seed for the same VA.
    """
    if not existing.is_file():
        return rows
    curated: dict[str, tuple[str, str, str, str]] = {}
    with existing.open(newline="", encoding="utf-8") as handle:
        for row in list(csv.reader(handle))[1:]:
            if len(row) == len(NAME_OVERLAY_COLUMNS) and SEED_EVIDENCE.search(row[3]) is None:
                curated[row[0]] = (row[0], row[1], row[2], row[3])
    merged = [row for row in rows if row[0] not in curated]
    merged.extend(curated.values())
    return sorted(merged, key=lambda row: int(row[0], 16))


def render(rows: list[tuple[str, str, str, str]]) -> str:
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(NAME_OVERLAY_COLUMNS)
    writer.writerows(rows)
    return buffer.getvalue()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--manifest", type=Path, default=Path("generated/replace/manifest.json"))
    parser.add_argument("--proof", type=Path, default=Path("generated/replace/proof.json"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument("--src-dir", type=Path, default=Path("src/game"))
    parser.add_argument("--out", type=Path, default=DEFAULT_NAME_OVERLAY)
    parser.add_argument("--check", action="store_true", help="exit 1 if --out differs")
    args = parser.parse_args(argv)
    log: list[str] = []
    rows = build_rows(args.manifest, args.proof, args.functions, args.src_dir, log)
    for line in log:
        print(line, file=sys.stderr)
    measured = sum(1 for row in rows if row[2] == "MEASURED")
    print(f"{len(rows)} seeded rows ({measured} MEASURED, {len(rows) - measured} INFERRED)")
    rows = keep_curated(rows, args.out)
    text = render(rows)
    print(f"{len(rows)} rows including curated evidence rows")
    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.is_file() else ""
        return 0 if current == text else 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
