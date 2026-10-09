# SPDX-License-Identifier: GPL-3.0-or-later
"""Coverage by subsystem: how much of each part is named and hand-decompiled and proven (T1521).

    /workspace/.venv/bin/python -m tools.coverage_by_subsystem \
        --xbe /workspace/tmp/build-t565/default.xbe [--generated-dir DIR] [--check]

Writes docs/coverage-by-subsystem.md and docs/data/coverage-by-subsystem.csv. The inputs are
the same ones the badges use (tools.coverage): the function table (export + tracked overrides
+ additions), the game name overlay tools/data/function_names.csv, the library overlay
tools/data/library_names.csv, the .XTLID/FLIRT library evidence, and the tracked proof
snapshot docs/data/replace-proof-snapshot.json (replayed, never a live harness run).

GROUPING RULE (see the generated document for the rationale). A named game function is
assigned to a part by the token after `game_` in its name, through the PART_OF_PREFIX
vocabulary below. Prefixes that are generic nouns or verbs (table, get, static ...) go to
`generic prefix`, prefixes outside the vocabulary to `other prefix`, and a placeholder name
to `unnamed`. Library code is never mixed into the game rows: it has its own rows by
classification evidence. A second view groups game functions by address band.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import io
import sys
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from tools.coverage import (
    REGION_GAME,
    Classified,
    Inputs,
    ReplacementEvaluation,
    apply_overlays,
    classify_functions,
    discover_artifacts,
    is_placeholder_name,
    load_inputs,
    measure_replacement,
    naming_split,
)

DOC_PATH = Path("docs/coverage-by-subsystem.md")
CSV_PATH = Path("docs/data/coverage-by-subsystem.csv")
COMMAND = (
    "/workspace/.venv/bin/python -m tools.coverage_by_subsystem "
    "--xbe /workspace/tmp/build-t565/default.xbe --generated-dir /workspace/generated/retail"
)

PART_UNNAMED = "unnamed"
PART_GENERIC = "generic prefix"
PART_OTHER = "other prefix"
PART_NO_GAME_PREFIX = "named without game_ prefix"

# Part -> prefixes (the token after `game_`). Vocabulary follows docs/t1473-naming-consistency.md
# (families kept distinct there stay distinct here) and the T1504 Jev subsystem list in
# tools/jev_subsystem_map.py (hud, menu, ui_text, net_online, anim, object, weapon, ...).
PARTS: dict[str, tuple[str, ...]] = {
    "hud": tuple(("hud scoreboard radar crosshair").split()),
    "menu and ui": tuple(
        (
            "menu ui text screen font dialog widget frontend overlay popup loadbar front "
            "attract credits"
        ).split()
    ),
    "net and online": tuple(("net online fesl clan messenger session lobby xlive packet").split()),
    "audio": tuple(
        (
            "audio sound music voice sfx dsound movie xmv codec dsp crossfade wave noise fft "
            "ambient"
        ).split()
    ),
    "d3d and render": tuple(
        ("d3d render draw texture model camera mesh shader viewport light sky").split()
    ),
    "effect and particle": tuple(
        ("effect particle decal weather fx explosion clouds lighting post color").split()
    ),
    "weapon": tuple(("weapon hit pickup projectile bullet grenade damage ammo nuke").split()),
    "ai and characters": tuple(("ai char actor npc pathfind bot").split()),
    "anim": tuple(("anim skeleton bone").split()),
    "physics and collision": tuple(
        ("collision physics sweep raycast vehicle contact constraint ray").split()
    ),
    "object and entity": tuple(("object component entity obj prop objects node").split()),
    "player, mode and state": tuple(
        (
            "player mode state team rules game players ingame match arena selected results unlock"
        ).split()
    ),
    "level and script": tuple(
        ("level scene script cutscene terminal trigger scripted spawn enter room sequence").split()
    ),
    "map editor": tuple(("mapedit mapmaker").split()),
    "input": tuple(("input pad controller button").split()),
    "save, pak and file": tuple(("save pak file profile storage load").split()),
    "init and launch": tuple(("launch init boot main").split()),
    "math": tuple(("math matrix4x4 matrix vec vector quat rand random vec3 scalar float").split()),
    "crt, memory and runtime": tuple(
        ("crt eh thunk alloc memory mem str string pool heap").split()
    ),
}

GENERIC_PREFIXES = frozenset(
    (
        "add address any append apply array buffer build byte call callback can check "
        "chunk clear code collect composite context copy count create ctx current delete "
        "deleting destroy do entry event fill find flag format free g701cc4 g701cd8 "
        "generate get global globals grid group handle has indexed install is jump list "
        "live local map nested new packed pair point process read record register release "
        "remove reset resource return run select set setup shared show side sized slot "
        "spatial static store table type update word wrapper write zero"
    ).split()
)

PART_OF_PREFIX: dict[str, str] = {
    prefix: part for part, prefixes in PARTS.items() for prefix in prefixes
}

# Address bands match the T1475-T1480 replace-function tasks and docs/t77-band-*.md.
BANDS: tuple[tuple[int, int], ...] = (
    (0x00000000, 0x00080000),
    (0x00080000, 0x00100000),
    (0x00100000, 0x00180000),
    (0x00180000, 0x00280000),
    (0x00280000, 0x00380000),
    (0x00380000, 0x00470000),
    (0x00470000, 1 << 32),
)

EVIDENCE_DEFAULT = "default-domain"
EVIDENCE_PER_FUNCTION = "per-function run (amended fixture or corrected)"
#: T1624: fp-scalar-v1 proofs, counted apart from default-domain (never merged into it).
EVIDENCE_FP_SCALAR = "fp-scalar"


def part_of_name(name: str) -> str:
    """The part a game function name belongs to; `unnamed` for a Ghidra placeholder."""
    if is_placeholder_name(name):
        return PART_UNNAMED
    tokens = name.split("_")
    if tokens[0] != "game" or len(tokens) < 3:
        return PART_NO_GAME_PREFIX
    prefix = tokens[1].lower()
    if prefix in PART_OF_PREFIX:
        return PART_OF_PREFIX[prefix]
    if prefix in GENERIC_PREFIXES:
        return PART_GENERIC
    return PART_OTHER


def band_label(entry_va: int) -> str:
    for lo, hi in BANDS:
        if lo <= entry_va < hi:
            top = "end" if hi >= 1 << 32 else f"0x{hi:08X}"
            return f"0x{lo:08X}-{top}"
    raise ValueError(entry_va)


@dataclass
class Row:
    view: str
    region: str
    part: str
    total: int = 0
    named: int = 0
    proven: int = 0
    default_domain: int = 0
    per_function: int = 0
    fp_scalar: int = 0
    proven_named: int = 0
    bytes_total: int = 0
    bytes_proven: int = 0

    @property
    def unnamed(self) -> int:
        return self.total - self.named


def _pct(count: int, whole: int) -> str:
    return f"{100 * count / whole:.2f}%" if whole else "n/a"


def build_rows(
    classified: Iterable[Classified], proven_class: dict[int, str]
) -> tuple[list[Row], list[Row], list[Row]]:
    """Return (game parts, game bands, library rows). `proven_class` maps VA to evidence class."""
    parts: dict[str, Row] = {}
    bands: dict[str, Row] = {}
    library: dict[str, Row] = {}
    for item in classified:
        function = item.function
        named = item.is_named
        evidence = proven_class.get(function.entry_va)
        is_game = item.region == REGION_GAME
        targets = (
            [
                parts.setdefault(
                    part_of_name(function.name),
                    Row("part", "game", part_of_name(function.name)),
                ),
                bands.setdefault(
                    band_label(function.entry_va),
                    Row("band", "game", band_label(function.entry_va)),
                ),
            ]
            if is_game
            else [library.setdefault(item.evidence, Row("library", "library", item.evidence))]
        )
        for row in targets:
            row.total += 1
            row.bytes_total += function.size_bytes
            if named:
                row.named += 1
            if evidence is not None and is_game:
                row.proven += 1
                row.bytes_proven += function.size_bytes
                if named:
                    row.proven_named += 1
                if evidence == EVIDENCE_DEFAULT:
                    row.default_domain += 1
                elif evidence == EVIDENCE_FP_SCALAR:
                    row.fp_scalar += 1
                else:
                    row.per_function += 1
    order = [*PARTS, PART_GENERIC, PART_OTHER, PART_NO_GAME_PREFIX, PART_UNNAMED]
    part_rows = sorted(parts.values(), key=lambda r: order.index(r.part))
    band_rows = sorted(bands.values(), key=lambda r: r.part)
    return part_rows, band_rows, sorted(library.values(), key=lambda r: r.part)


def total_row(view: str, region: str, rows: list[Row]) -> Row:
    total = Row(view, region, "TOTAL")
    for field in dataclasses.fields(Row):
        if field.type == "int":
            setattr(total, field.name, sum(getattr(r, field.name) for r in rows))
    return total


CSV_COLUMNS = (
    "view", "region", "part", "total", "named", "named_pct", "proven", "proven_pct",
    "proven_default_domain", "proven_per_function_run", "proven_of_named_pct", "unnamed",
    "bytes_total", "bytes_proven",
)  # fmt: skip


def csv_record(row: Row) -> list[object]:
    return [
        row.view, row.region, row.part, row.total, row.named,
        _pct(row.named, row.total).rstrip("%"),
        row.proven, _pct(row.proven, row.total).rstrip("%"), row.default_domain,
        row.per_function, _pct(row.proven_named, row.named).rstrip("%"), row.unnamed,
        row.bytes_total, row.bytes_proven,
    ]  # fmt: skip


def render_csv(rows: list[Row]) -> str:
    """The CSV. A `proven_fp_scalar` column (T1624) appears only once an fp-scalar row exists."""
    with_fp = any(row.fp_scalar for row in rows)
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow((*CSV_COLUMNS, "proven_fp_scalar") if with_fp else CSV_COLUMNS)
    for row in rows:
        writer.writerow([*csv_record(row), row.fp_scalar] if with_fp else csv_record(row))
    return buffer.getvalue()


def md_table(label: str, rows: list[Row]) -> list[str]:
    lines = [
        f"| {label} | functions | named | named % | proven | proven % | proven of named | "
        "unnamed | bytes | proven bytes |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row.part} | {row.total:,} | {row.named:,} | {_pct(row.named, row.total)} | "
            f"{row.proven:,} | {_pct(row.proven, row.total)} | "
            f"{_pct(row.proven_named, row.named)} | {row.unnamed:,} | {row.bytes_total:,} | "
            f"{row.bytes_proven:,} |"
        )
    return lines


def render_markdown(
    parts: list[Row],
    bands: list[Row],
    library: list[Row],
    *,
    game_total: Row,
    notes: list[str],
    top_other: list[tuple[str, int]],
    snapshot_info: str,
) -> str:
    out = [
        "# Coverage by subsystem",
        "",
        "GENERATED by `tools/coverage_by_subsystem.py` (T1521). Do not edit by hand. Regenerate:",
        "",
        "```sh",
        COMMAND,
        "```",
        "",
        "Machine-readable twin: `docs/data/coverage-by-subsystem.csv`. Add `--check` to fail "
        "when either committed file is stale.",
        "",
        "Inputs: the function table (export + `tools/data/function_overrides.csv` + additions), "
        "the game name overlay `tools/data/function_names.csv`, the library overlay "
        "`tools/data/library_names.csv`, `.XTLID` and FLIRT library evidence (game versus library "
        f"classification of `tools/coverage.py`), and {snapshot_info}.",
        "",
        "## Reconciliation with the badges",
        "",
        f"Game code: {game_total.total:,} functions, named {game_total.named:,} "
        f"({_pct(game_total.named, game_total.total)}), hand-decompiled and proven "
        f"{game_total.proven:,} ({_pct(game_total.proven, game_total.total)}). These are the "
        "numbers of `docs/badges/metrics.json` (naming-game, decompiled-proven) at the commit "
        "of generation. " + " ".join(notes),
        "",
        "## View 1: game code by subsystem (name vocabulary)",
        "",
        *md_table("part", [*parts, game_total]),
        "",
        "Proven evidence classes in this view: default-domain "
        f"{game_total.default_domain:,}, per-function run (amended fixture or corrected) "
        f"{game_total.per_function:,}"
        + (
            f", fp-scalar {game_total.fp_scalar:,} (T1624, counted apart)"
            if game_total.fp_scalar
            else ""
        )
        + ". Per part, see the `proven_default_domain` and "
        "`proven_per_function_run` columns of the CSV.",
        "",
        "## View 2: game code by address band",
        "",
        "Bands are the T1475-T1480 replace-function bands (`docs/t77-band-*.md`).",
        "",
        *md_table("band", [*bands, total_row("band", "game", bands)]),
        "",
        "## Library code (separate, never mixed into the game rows)",
        "",
        "Rows are the classification evidence that made the function library "
        "(`section` = outside `.text`, `xtlid`, `flirt`, `named` = already named inside `.text`). "
        "No library function is counted proven (the proof gate refuses `not-a-game-function`).",
        "",
        *md_table("evidence", [*library, total_row("library", "library", library)]),
        "",
        "## Grouping rule and why",
        "",
        "Primary view: the token after `game_` in the overlay name, mapped through the "
        "`PARTS` vocabulary in `tools/coverage_by_subsystem.py`. This is defensible because "
        "the names are the only whole-program label that exists for all "
        f"{game_total.named:,} named game functions, the vocabulary follows the T1473 review "
        "(`docs/t1473-naming-consistency.md`: families it kept distinct stay distinct, for "
        "example hud versus overlay, sound versus audio, mapedit versus map) and the T1504 "
        "Jev subsystem list (`tools/jev_subsystem_map.py`, `docs/t1504-jev-subsystem-map.md`), "
        "and it is exactly reproducible without any model call. Rows `generic prefix` "
        "(table, get, static, ...) and `other prefix` (not in the vocabulary) are explicit so "
        "nothing is hidden inside a subsystem it was not assigned to. `unnamed` is every game "
        "function that still has a `FUN_*` placeholder. Second view: address bands, which need "
        "no naming at all and match how replacement work is dispatched.",
        "",
        "Most frequent prefixes inside `other prefix` (candidates for the vocabulary): "
        + ", ".join(f"{name} {count}" for name, count in top_other)
        + ".",
        "",
        "## Limitations",
        "",
        "- Names are INFERRED (confidence column of the overlays), so a part is only as good as "
        "the names that put functions in it. Prefix grouping reflects naming habits of the "
        "per-band naming agents (for example `scene` and `weapon` come from one band), not a "
        "verified module boundary.",
        "- `proven` is an UPPER BOUND per the audit: it is the harness verdict count of the "
        "tracked snapshot (`docs/data/replace-proof-snapshot.json`), replayed against the "
        "current `src/game` fingerprints, not an independently audited equivalence. See "
        "`docs/t1462-badge-reproducibility.md` and the caller-audit tasks.",
        "- Proof evidence classes: the snapshot records `source_run`. `default` is the "
        "default-domain run. Any other value is a per-function re-run, which the snapshot "
        "cannot split further into amended fixture versus corrected, so they share one column.",
        "- The band and subsystem views slice the same functions differently and each sums to "
        "the same game total, but a given part spans many bands and vice versa.",
        "- `bytes` is the sum of `size_bytes` (an address count), not the body extent, so "
        "fragmented functions are under-counted compared with `.text` coverage.",
        "- A function named without a `game_` prefix (should be zero) is shown in its own row "
        "rather than guessed.",
        "",
    ]
    return "\n".join(out)


def load_classified(
    root: Path,
    xbe: Path,
    generated_dir: Path | None = None,
    *,
    proof_snapshot: Path | None = None,
) -> tuple[Inputs, tuple[Classified, ...]]:
    """The badges' inputs and the classified, overlaid function table (also used by
    `tools.subsystems`). `proof_snapshot` replaces the tracked snapshot path when given."""
    artifacts = discover_artifacts(root, xbe=xbe)
    if generated_dir:
        artifacts = dataclasses.replace(
            artifacts,
            functions=generated_dir / "functions.csv",
            flirt_names=generated_dir / "flirt_names.csv",
        )
    if artifacts.xbe is None or artifacts.functions is None or artifacts.flirt_names is None:
        raise SystemExit("need --xbe and a generated dir with functions.csv and flirt_names.csv")
    # Force the tracked snapshot replay so the result never depends on a gitignored live proof.
    artifacts = dataclasses.replace(artifacts, replace_manifest=None, replace_proof=None)
    if proof_snapshot is not None:
        artifacts = dataclasses.replace(artifacts, replace_snapshot=proof_snapshot)
    inputs = load_inputs(artifacts, root=root, collect_tests=False)
    assert inputs.functions is not None and inputs.text_lo is not None
    assert inputs.text_hi is not None and inputs.flirt_addresses is not None
    classified = apply_overlays(
        classify_functions(
            inputs.functions,
            text_lo=inputs.text_lo,
            text_hi=inputs.text_hi,
            xtlid_addresses=inputs.xtlid_addresses or frozenset(),
            flirt_addresses=inputs.flirt_addresses,
            class_overrides=inputs.class_overrides,
        ),
        inputs.name_overlay,
        inputs.library_name_overlay,
    )
    return inputs, classified


def proven_classes(
    inputs: Inputs, classified: tuple[Classified, ...]
) -> tuple[ReplacementEvaluation, dict[int, str]]:
    """The replayed proof snapshot's evaluation and VA -> evidence class of every proven VA."""
    split = naming_split(classified)
    outcome = measure_replacement(inputs, classified, split.game_total)
    evaluation: ReplacementEvaluation | None = outcome.evaluation
    if evaluation is None or evaluation.snapshot is None:
        raise SystemExit(f"proof snapshot unusable: {outcome.reason}")
    run_of = {entry.function.va: entry.source_run for entry in evaluation.snapshot.entries}
    fp_vas = {entry.function.va for entry in evaluation.snapshot.entries if entry.proof.fp_scalar}
    proven_class = {
        function.va: EVIDENCE_FP_SCALAR
        if function.va in fp_vas
        else EVIDENCE_DEFAULT
        if run_of[function.va] == "default"
        else EVIDENCE_PER_FUNCTION
        for function in evaluation.proven
    }
    return evaluation, proven_class


def measure(args: argparse.Namespace) -> tuple[str, str]:
    root = Path(args.root)
    inputs, classified = load_classified(
        root, Path(args.xbe), Path(args.generated_dir) if args.generated_dir else None
    )
    evaluation, proven_class = proven_classes(inputs, classified)
    parts, bands, library = build_rows(classified, proven_class)
    game_total = total_row("part", "game", parts)
    notes = []
    if (game_total.total, game_total.named, game_total.proven) != (11061, 10623, 291):
        notes.append(
            "These differ from the 11,061 / 10,623 / 291 quoted when the task was filed because "
            "the overlays and snapshot moved since; the badges are regenerated by the same "
            "loaders, so the totals here are the live ones."
        )
    if evaluation.proven and len(evaluation.proven) != game_total.proven:
        raise SystemExit("proven functions outside the game region: totals do not reconcile")
    other_prefixes: Counter[str] = Counter()
    for item in classified:
        if item.region == REGION_GAME and part_of_name(item.function.name) == PART_OTHER:
            other_prefixes[item.function.name.split("_")[1].lower()] += 1
    snapshot = evaluation.snapshot
    snapshot_info = (
        f"the tracked proof snapshot docs/data/replace-proof-snapshot.json (measured "
        f"{snapshot.measured_on}, commit {snapshot.commit}, {len(snapshot.entries)} entries)"
    )
    markdown = render_markdown(
        parts,
        bands,
        library,
        game_total=game_total,
        notes=notes,
        top_other=other_prefixes.most_common(15),
        snapshot_info=snapshot_info,
    )
    rows = [
        *parts,
        game_total,
        *bands,
        total_row("band", "game", bands),
        *library,
        total_row("library", "library", library),
    ]
    return markdown, render_csv(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--xbe", required=True, help="the pinned retail default.xbe")
    parser.add_argument("--generated-dir", help="directory holding functions.csv, flirt_names.csv")
    parser.add_argument("--check", action="store_true", help="fail if committed outputs are stale")
    args = parser.parse_args(argv)
    markdown, table = measure(args)
    root = Path(args.root)
    if args.check:
        stale = [
            path
            for path, text in ((DOC_PATH, markdown), (CSV_PATH, table))
            if not (root / path).is_file() or (root / path).read_text() != text
        ]
        for path in stale:
            print(f"stale: {path}", file=sys.stderr)
        return 1 if stale else 0
    (root / DOC_PATH).write_text(markdown)
    (root / CSV_PATH).write_text(table)
    print(f"wrote {DOC_PATH} and {CSV_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
