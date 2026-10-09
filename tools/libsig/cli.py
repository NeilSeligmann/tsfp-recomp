# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line: build a signature database from local XDK libs, then match an XBE.

    ./.venv/bin/python -m tools.libsig.cli build \\
        --libs tmp/xbox-sdk-ref --builds 5849 --out tmp/libsig/db-5849.json.gz
    ./.venv/bin/python -m tools.libsig.cli match \\
        --xbe tmp/oxm-extract/retail/default.xbe --db tmp/libsig/db-5849.json.gz \\
        --functions generated/retail/functions.csv --out generated/retail/libsig_names.csv

Outputs are refused outside tmp/ or generated/ unless --allow-outside-ignored is
given, because the database contains library bytes (docs/provenance.md).
"""

from __future__ import annotations

import argparse
import csv
import random
import re
import sys
from collections import Counter
from collections.abc import Sequence
from pathlib import Path

from tools.libsig.db import DbVersionError, SignatureDb, build_db
from tools.libsig.guard import ProvenanceError, check_output_path
from tools.libsig.match import (
    Image,
    Match,
    Matcher,
    merge_alt_matches,
    propagate,
    refine,
    resolve_by_data,
    resolve_by_xtlid,
    select_seeds,
    symbol_va_votes,
)
from tools.libsig.quality import CLEAN, QUALITIES, classify_seeds
from tools.xbe import parse_xbe
from tools.xtlid import parse_xtlid_db, resolve_xtlid

CSV_COLUMNS = (
    "address",
    "symbol",
    "lib",
    "build",
    "confidence",
    "body_len",
    "alias_set",
    "reason",
    "fixed_bytes",
)
LIB_GLOB = "XDK/xbox/lib/*.lib"
_SURFACE_ROW = re.compile(r'\{0x([0-9a-fA-F]+),\s*"(\w+)",\s*(NULL|"[^"]*")')


def find_builds(root: Path, tokens: Sequence[str]) -> list[tuple[str, Path]]:
    """Resolve build tokens like `5849` to `<root>/<date>_<build>` directories.

    A token matches the part after the last underscore exactly, so `5849` does not
    pick up `5849.6`.
    """
    found = []
    for token in tokens:
        hits = [d for d in sorted(root.iterdir()) if d.is_dir() and d.name.split("_")[-1] == token]
        if not hits:
            raise SystemExit(f"no build directory for {token!r} under {root}")
        found.extend((token, d) for d in hits)
    return found


def load_image(xbe_path: Path) -> Image:
    data = xbe_path.read_bytes()
    xbe = parse_xbe(data)
    segments = []
    names = {}
    for section in xbe.sections:
        if section.executable and section.raw_size:
            segments.append(
                (section.virtual_addr, data[section.raw_addr : section.raw_addr + section.raw_size])
            )
            names[section.virtual_addr] = section.name
    return Image(segments, names)


def load_xtlid_names(xbe_path: Path, db_path: Path) -> dict[int, str]:
    """address -> library function name from the game's own `.XTLID` section."""
    database = parse_xtlid_db(db_path.read_text(encoding="utf-8", errors="replace"))
    resolved, _ = resolve_xtlid(parse_xbe(xbe_path.read_bytes()).xtlid, database)
    return {address: name.name for address, name in resolved.items()}


def load_entries(functions_csv: Path) -> set[int]:
    with functions_csv.open(newline="", encoding="utf-8") as handle:
        return {int(row["entry_va"], 16) for row in csv.DictReader(handle)}


def load_bodies(functions_csv: Path) -> list[tuple[int, int]]:
    """(entry, last body byte) per table row; without `body_max_va` the body is the entry byte."""
    with functions_csv.open(newline="", encoding="utf-8") as handle:
        return [
            (int(row["entry_va"], 16), int(row.get("body_max_va") or row["entry_va"], 16))
            for row in csv.DictReader(handle)
        ]


def load_surface(path: Path) -> dict[int, tuple[str, str | None]]:
    """address -> (family, name or None) from the generated surface table."""
    rows = {}
    for match in _SURFACE_ROW.finditer(path.read_text(encoding="utf-8")):
        name = match.group(3)
        rows[int(match.group(1), 16)] = (
            match.group(2),
            None if name == "NULL" else name.strip('"'),
        )
    return rows


def write_matches(path: Path, matches: dict[int, Match]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(CSV_COLUMNS)
        for address in sorted(matches):
            m = matches[address]
            aliases = "|".join(m.names) if len(m.names) > 1 else m.extra.get("alias_set", "")
            writer.writerow(
                [
                    f"0x{address:08x}",
                    m.symbol,
                    "|".join(m.libs),
                    "|".join(m.builds),
                    m.confidence,
                    m.body_len,
                    aliases,
                    m.extra.get("reason", ""),
                    m.fixed_bytes,
                ]
            )


SEED_COLUMNS = ("target_va", "symbol", "lib", "confidence", "body_len", "quality")


def write_seeds(path: Path, seeds: Sequence[Match], quality: dict[int, str] | None = None) -> None:
    """Write a `CreateFunctionsAt.java` input: first column is the address, the rest is context.

    `quality` is appended as the LAST column, so the first five stay as T340 wrote them.
    """
    quality = quality or {}
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(SEED_COLUMNS)
        for m in seeds:
            writer.writerow(
                [
                    f"0x{m.address:08x}",
                    m.symbol,
                    "|".join(m.libs),
                    m.confidence,
                    m.body_len,
                    quality.get(m.address, ""),
                ]
            )


def cmd_build(args: argparse.Namespace) -> int:
    out = check_output_path(args.out, args.allow_outside_ignored)
    pairs = []
    for token, directory in find_builds(args.libs, args.builds):
        libs = sorted(directory.glob(args.lib_glob))
        if args.exclude_debug:
            libs = [
                p
                for p in libs
                if not (p.stem.endswith("d") and (p.parent / f"{p.stem[:-1]}.lib").exists())
            ]
        pairs.extend((lib, token) for lib in libs)
    db = build_db(pairs)
    db.save(out)
    print(
        f"{len(pairs)} libs, {len(db.signatures)} distinct signatures, "
        f"{len(db.function_names())} names, {db.skipped_members} unparseable members -> {out}"
    )
    return 0


def cmd_match(args: argparse.Namespace) -> int:
    out = check_output_path(args.out, args.allow_outside_ignored)
    seeds_out = None
    if args.seeds_out:
        seeds_out = check_output_path(args.seeds_out, args.allow_outside_ignored)
        if not (args.scan_all and args.functions and args.functions.exists()):
            raise SystemExit("--seeds-out needs --scan-all and an existing --functions table")
    if (args.xtlid_db or args.alt_db) and not args.propagate:
        raise SystemExit("--xtlid-db and --alt-db need --propagate (they join the evidence passes)")
    db = SignatureDb.load(args.db)
    image = load_image(args.xbe)
    entries = load_entries(args.functions) if args.functions else set()
    table_entries = set(entries)
    if args.surface:
        entries |= set(load_surface(args.surface))
    xtlid_names = load_xtlid_names(args.xbe, args.xtlid_db) if args.xtlid_db else {}
    matcher = Matcher(db)
    alt_matchers = [(path, Matcher(SignatureDb.load(path))) for path in args.alt_db]
    # A reloc may name a function only an alternate build's lib defines.
    function_names = db.function_names().union(*(m.db.function_names() for _, m in alt_matchers))
    real_entries = set(entries)
    if args.random:
        rng = random.Random(args.seed)
        text = [(b, len(blob)) for b, blob in image.segments if image.names.get(b) == ".text"]
        base, size = text[0]
        entries = {base + rng.randrange(size - 64) for _ in range(args.random)} - real_entries
        args.shift = 0
        print(f"control: {len(entries)} random .text addresses (seed {args.seed})")
    probes = {a + args.shift for a in entries}
    if args.shift or args.random:
        probes -= real_entries  # a shifted probe landing on another real entry is a true hit

    def scan(found_by: Matcher) -> dict[int, Match]:
        return (
            found_by.scan_all(image)
            if args.scan_all
            else found_by.scan_entries(image, sorted(probes))
        )

    matches = scan(matcher)
    disputed: set[int] = set()
    if args.propagate:
        totals: Counter[str] = Counter()
        conflicts: list[tuple[int, str, list[str]]] = []

        def evidence() -> None:
            nonlocal conflicts
            while True:  # each pass can unlock the other (a resolved match is a new destination)
                while True:
                    step_target = refine(image, matches, function_names)
                    step_data = resolve_by_data(image, matches)
                    totals["target"] += step_target
                    totals["data"] += step_data
                    if not step_target and not step_data:
                        break
                # `.XTLID` goes last: a name the bytes or call sites decide keeps that reason.
                step_xtlid, conflicts = resolve_by_xtlid(matches, xtlid_names)
                totals["xtlid"] += step_xtlid
                if not step_xtlid:
                    break

        evidence()
        merged = 0
        for path, alt_matcher in alt_matchers:
            added_alt, alt_conflicts = merge_alt_matches(matches, scan(alt_matcher))
            merged += added_alt
            print(f"alt-build {path.name}: {added_alt} addresses named only by this db")
            for address, primary_names, alt_names in alt_conflicts:
                print(
                    f"  ALT-CONFLICT 0x{address:08x}: primary {primary_names}, "
                    f"alternate {alt_names} (primary kept)"
                )
        if merged:
            evidence()  # the added rows are undecided too (their reason stays `alt-build`)
        by_target, by_data, by_xtlid = totals["target"], totals["data"], totals["xtlid"]
        print(f"refine: {by_target} matches resolved by reloc targets")
        if xtlid_names:
            print(
                f"xtlid: {by_xtlid} matches resolved, {len(conflicts)} name outside the candidates"
            )
            for address, name, candidates in conflicts:
                print(f"  XTLID-DISAGREE 0x{address:08x}: .XTLID {name}, candidates {candidates}")
        votes = symbol_va_votes(image, matches)
        unanimous = sum(1 for c in votes.values() if len(c) == 1)
        print(
            f"data-address: {by_data} matches resolved; DIR32 symbol VAs from high votes: "
            f"{len(votes)} symbols, {unanimous} unanimous, {len(votes) - unanimous} conflicting, "
            f"{sum(1 for c in votes.values() if len(c) == 1 and sum(c.values()) == 1)} "
            "unanimous on a single vote (not trusted)"
        )
        added, disagreements, agreed, folded = propagate(image, matches, entries, function_names)
        matches.update(added)
        print(
            f"propagation: {agreed} call sites agree, {folded} agree via identical-body alias, "
            f"{len(disagreements)} disagree, "
            f"{len(added)} names added"
        )
        for src, dest, symbol, names in disagreements:
            disputed |= {src, dest}
            print(
                f"  DISAGREE call 0x{src:08x} -> 0x{dest:08x}: "
                f"reloc names {symbol}, matched {names}"
            )
    tiny = [a for a, m in matches.items() if m.confidence == "tiny"]
    if not args.keep_tiny:
        for address in tiny:
            del matches[address]
        print(f"dropped {len(tiny)} unconfirmed tiny-body matches (use --keep-tiny to list them)")
    write_matches(out, matches)
    if seeds_out:
        candidates = select_seeds(matches, table_entries, frozenset(disputed))
        surface = load_surface(args.surface) if args.surface else {}
        quality = classify_seeds(candidates, load_bodies(args.functions), image, set(surface))
        allowed = set(QUALITIES) if "all" in args.seeds_include else {CLEAN, *args.seeds_include}
        seeds = [m for m in candidates if quality[m.address] in allowed]
        write_seeds(seeds_out, seeds, quality)
        counts = Counter(quality.values())
        print(
            f"{len(seeds)} function seeds of {len(candidates)} candidates "
            f"(not in {args.functions}) -> {seeds_out}; by quality "
            + ", ".join(f"{name}={counts[name]}" for name in QUALITIES)
        )
        for m in candidates:
            if m.address in surface and quality[m.address] not in allowed:
                print(
                    f"  surface 0x{m.address:08x} ({surface[m.address][0]}) not seeded: "
                    f"{quality[m.address]} (--seeds-include {quality[m.address]} to write it)"
                )
    print(
        f"{len(matches)} addresses named: "
        f"{dict(Counter(m.confidence for m in matches.values()))} -> {out}"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.libsig.cli", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="build a signature database from local XDK libs")
    build.add_argument(
        "--libs", type=Path, required=True, help="directory holding one directory per build"
    )
    build.add_argument("--builds", nargs="+", required=True, help="build numbers, e.g. 5849")
    build.add_argument(
        "--out", type=Path, required=True, help="database path (under tmp/ or generated/)"
    )
    build.add_argument("--lib-glob", default=LIB_GLOB, help="library glob inside a build directory")
    build.add_argument(
        "--exclude-debug", action="store_true", help="skip <name>d.lib when <name>.lib exists"
    )
    build.add_argument("--allow-outside-ignored", action="store_true")
    build.set_defaults(func=cmd_build)

    match = sub.add_parser("match", help="match a database against an XBE")
    match.add_argument("--xbe", type=Path, required=True)
    match.add_argument("--db", type=Path, required=True)
    match.add_argument(
        "--out", type=Path, required=True, help="CSV path (under tmp/ or generated/)"
    )
    match.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    match.add_argument(
        "--surface", type=Path, default=None, help="also test the xdk_surface.c addresses"
    )
    match.add_argument(
        "--scan-all", action="store_true", help="try every byte offset instead of the table"
    )
    match.add_argument(
        "--propagate", action="store_true", help="cross-check and extend via REL32 call sites"
    )
    match.add_argument(
        "--xtlid-db",
        type=Path,
        default=None,
        help="with --propagate: name undecided matches (weak, alias sets) from the game's own "
        ".XTLID record when it names exactly one candidate (reason `xtlid`)",
    )
    match.add_argument(
        "--alt-db",
        type=Path,
        nargs="*",
        default=[],
        help="with --propagate: databases of another build (for example a later QFE), consulted "
        "only for addresses the primary db left unnamed; rows get reason `alt-build` and never "
        "override, vote for data addresses or propagate names. Conflicts are printed",
    )
    match.add_argument(
        "--shift", type=int, default=0, help="control: shift every candidate by N bytes"
    )
    match.add_argument(
        "--random", type=int, default=0, help="control: test N random .text addresses instead"
    )
    match.add_argument(
        "--keep-tiny",
        action="store_true",
        help="keep unconfirmed matches on bodies of under 4 fixed bytes",
    )
    match.add_argument(
        "--seeds-out",
        type=Path,
        default=None,
        help="with --scan-all: write unique-name matches missing from --functions as a "
        "tools/ghidra/CreateFunctionsAt.java input (under tmp/ or generated/)",
    )
    match.add_argument(
        "--seeds-include",
        nargs="*",
        default=[],
        choices=(*QUALITIES, "all"),
        metavar="QUALITY",
        help="with --seeds-out: also write seeds of these quality classes (default: only "
        f"clean). One of {', '.join(QUALITIES)}, or all",
    )
    match.add_argument("--seed", type=int, default=0)
    match.add_argument("--allow-outside-ignored", action="store_true")
    match.set_defaults(func=cmd_match)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ProvenanceError, DbVersionError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
