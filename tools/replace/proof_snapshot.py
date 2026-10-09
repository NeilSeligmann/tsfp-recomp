# SPDX-License-Identifier: GPL-3.0-or-later
"""Write or verify the tracked replacement proof snapshot (T1462).

    uv run python -m tools.replace.proof_snapshot            # regenerate the snapshot
    uv run python -m tools.replace.proof_snapshot --check    # verify it is current, write nothing

Reads the gitignored `generated/replace/{manifest,proof}.json` (after `merge_proof`) and
the game/library split, and writes `docs/data/replace-proof-snapshot.json`: per function
the VA, name, registration, harness verdict fields the gates read, the source
fingerprint of the working tree, plus the manifest_sha, run seeds, the XBE sha256 that
pins the oracle and the pinned game total. No machine bytes, no XBE content, only hashes.

Refuses to write unless the live proof is whole-registry, matches its manifest and every
manifest function has a harness entry. Part of the proof refresh procedure, see
docs/replace-proof-whole-registry.md.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import UTC, datetime
from pathlib import Path

from tools.coverage import (
    DEFAULT_REPLACE_SNAPSHOT,
    FINGERPRINT_RULE,
    GAME_SOURCE_DIR,
    MAX_NULL_AGREE,
    MIN_COVERAGE,
    MIN_VERDICTS,
    REGION_GAME,
    SNAPSHOT_KIND,
    SNAPSHOT_SCHEMA,
    VECTOR_SNAPSHOT_SCHEMA,
    ProofEntry,
    ReplacementFunction,
    build_report,
    discover_artifacts,
    first_failing_gate,
    git_commit,
    load_inputs,
)
from tools.replace.proof_contract import (
    FP_SCALAR_EVIDENCE_CLASS,
    FP_SCALAR_RECORD_VERSION,
    fp_scalar_matrix_identity,
)

PROOF_FIELDS = (
    "cases",
    "verdicts",
    "agree",
    "disagree",
    "subject_faulted",
    "oracle_faulted",
    "coverage",
    "body_insns",
    "near_vacuous",
    "null_agree_rate",
    "replaced_confirmed",
    "regs_ignored",
)
#: Header fields that vary with the day or commit and are ignored by `--check`.
VOLATILE = ("measured_on", "commit")


class SnapshotError(ValueError):
    pass


def entry_record(
    function: ReplacementFunction,
    entry: ProofEntry,
    *,
    fingerprint: str,
    is_game: bool,
    source_run: str,
    source_cases: int,
    random_cases: int,
    synth_domain: dict | None = None,
) -> dict[str, object]:
    """One snapshot function record. The verdict `gate` is derived, never copied."""
    record: dict[str, object] = {
        "va": f"{function.va:#010x}",
        "name": function.name,
        "convention": function.convention,
        "stack_args": function.stack_args,
        "scratch": list(function.scratch),
        "source": function.source,
        "fingerprint": fingerprint,
        "is_game": is_game,
        "gate": first_failing_gate(function, entry, is_game=is_game),
        "source_run": source_run,
        "source_cases_per_function": source_cases,
        "random_cases": random_cases,
    }
    for name in PROOF_FIELDS:
        value = getattr(entry, name)
        record[name] = list(value) if isinstance(value, tuple) else value
    if function.register_inputs is not None:
        record["register_inputs"] = list(function.register_inputs)
    if entry.named_global_object_contract is not None:
        record["named_global_object_contract"] = dict(entry.named_global_object_contract)
    if entry.input_contract is not None:
        record["input_contract"] = dict(entry.input_contract)
    if entry.fixture_provider_selection is not None:
        record["fixture_provider_selection"] = list(entry.fixture_provider_selection)
    if entry.state_contract is not None:
        record["state_contract"] = dict(entry.state_contract)
        record["source_closure"] = entry.source_closure
        record["source_seed"] = entry.source_seed
    if entry.fp_scalar is not None:
        # T1624: counted as its own evidence class, never as default-domain.
        record["fp_scalar"] = dict(entry.fp_scalar)
        record["evidence_class"] = FP_SCALAR_EVIDENCE_CLASS
    if synth_domain is not None:
        record["synth_domain"] = dict(synth_domain)
    return record


def build_snapshot(root: Path, *, replace_dir: Path | None, xbe: Path | None) -> dict[str, object]:
    """The snapshot document for the artifacts under `root`. Raises `SnapshotError`."""
    artifacts = discover_artifacts(root, xbe=xbe, replace_dir=replace_dir)
    inputs = load_inputs(artifacts, root=root, collect_tests=False)
    report = build_report(inputs)
    evaluation = report.replacement
    if evaluation is None or evaluation.source != "live proof":
        raise SnapshotError(
            "no live whole-registry measurement: need generated/replace manifest.json and "
            "proof.json with the same manifest_sha, the function table and the XBE"
        )
    if inputs.xbe_sha256 is None or inputs.replace_manifest is None or inputs.replace_proof is None:
        raise SnapshotError("the XBE, manifest and proof must all be present")
    if inputs.game_tree is None:
        raise SnapshotError(inputs.game_tree_error or f"{GAME_SOURCE_DIR} is absent")
    tree = inputs.game_tree
    game_vas = {item.function.entry_va for item in report.classified if item.region == REGION_GAME}
    raw_proof = json.loads((artifacts.replace_proof or Path()).read_text(encoding="utf-8"))
    raw_rows = {int(row["va"], 16): row for row in raw_proof["functions"]}
    proof = {entry.va: entry for entry in inputs.replace_proof.entries}
    entries: list[dict[str, object]] = []
    for function in sorted(inputs.replace_manifest.functions, key=lambda item: item.va):
        entry = proof.get(function.va)
        if entry is None:
            raise SnapshotError(f"{function.va:#010x} {function.name}: no harness entry")
        registered = tree.registrations.get(function.va)
        if registered is None or registered[0] != function.source:
            raise SnapshotError(
                f"{function.va:#010x} {function.name}: the manifest says {function.source} but "
                f"src/game registers {registered[0] if registered else 'nothing'}"
            )
        row = raw_rows[function.va]
        is_game = function.va in game_vas
        fingerprint = tree.fingerprint(function.source)
        assert fingerprint is not None
        record = entry_record(
            function,
            entry,
            fingerprint=fingerprint,
            is_game=is_game,
            source_run=row.get("source_run", "unmerged"),
            source_cases=row.get(
                "source_cases_per_function", raw_proof.get("cases_per_function", 0)
            ),
            random_cases=row.get("random_cases", 0),
            synth_domain=row.get("synth_domain"),
        )
        entries.append(record)
    gate_none = sum(1 for item in entries if item["gate"] is None)
    if gate_none != len(evaluation.proven):
        raise SnapshotError(
            f"internal: snapshot proves {gate_none} but the live evaluation proves "
            f"{len(evaluation.proven)}"
        )
    vector = any(entry.state_contract is not None for entry in inputs.replace_proof.entries)
    if vector:
        for record in entries:
            record.setdefault("state_contract", None)
            record.setdefault("source_closure", None)
    fp_rows = [item for item in entries if "fp_scalar" in item]
    if fp_rows:
        try:
            live_matrix = fp_scalar_matrix_identity(root)["sha256"]
        except ValueError as error:
            raise SnapshotError(str(error)) from error
        for item in fp_rows:
            if item["fp_scalar"]["matrix_sha256"] != live_matrix:  # type: ignore[index]
                raise SnapshotError(
                    f"{item['va']} {item['name']}: fp-scalar receipt matrix identity differs "
                    "from the tracked matrix result"
                )
    document = {
        "schema": VECTOR_SNAPSHOT_SCHEMA if vector else SNAPSHOT_SCHEMA,
        "kind": SNAPSHOT_KIND,
        "note": (
            "Tracked replay of the gitignored harness proof. Verdict fields only: no machine "
            "bytes and no owner-file content. Regenerate with python -m tools.replace."
            "proof_snapshot after every proof refresh. See docs/t1462-badge-reproducibility.md."
        ),
        "measured_on": datetime.now(tz=UTC).date().isoformat(),
        "commit": git_commit(root) or "unknown",
        "xbe_sha256": inputs.xbe_sha256,
        "manifest_sha": inputs.replace_manifest.manifest_sha,
        "headers_digest": tree.headers_digest,
        "fingerprint_rule": FINGERPRINT_RULE,
        "game_total": evaluation.game_total,
        "judgeable": evaluation.judgeable,
        "summary": {
            "registered": len(entries),
            "proven": len(evaluation.proven),
            "failed": len(entries) - len(evaluation.proven),
        },
        "harness": {
            "cases_per_function": raw_proof.get("cases_per_function"),
            "seed": raw_proof.get("seed"),
            "edge_cases": raw_proof.get("edge_cases"),
            "merged_runs": raw_proof.get("merged_runs", []),
            "subject": {
                key: value
                for key, value in (raw_proof.get("subject") or {}).items()
                if key in ("repl_cflags", "repl_sha", "tree_sha")
            },
        },
        "gate_thresholds": {
            "min_verdicts": MIN_VERDICTS,
            "min_coverage": MIN_COVERAGE,
            "max_null_agree": MAX_NULL_AGREE,
        },
        "functions": entries,
    }
    if raw_proof.get("synth_domain") is True:
        document["harness"]["synth_domain"] = True
    if raw_proof.get("named_global_object_contracts") is not None:
        document["harness"]["named_global_object_contracts"] = raw_proof[
            "named_global_object_contracts"
        ]
    if raw_proof.get("fixture_provider_selection") is not None:
        document["harness"]["fixture_provider_selection"] = raw_proof["fixture_provider_selection"]
    from .proof_contract import validate_document

    if fp_rows:
        document["fp_scalar_schema"] = FP_SCALAR_RECORD_VERSION
        document["summary"]["fp_scalar_registered"] = len(fp_rows)  # type: ignore[index]
        document["summary"]["fp_scalar_proven"] = sum(  # type: ignore[index]
            1 for item in fp_rows if item["gate"] is None
        )
    if vector:
        document["state_contract_schema"] = 1
        document["harness"]["live_call_closure"] = raw_proof.get("live_call_closure")
    try:
        validate_document(document)
    except ValueError as error:
        raise SnapshotError(str(error)) from error
    return document


def render(document: dict[str, object]) -> str:
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root")
    parser.add_argument(
        "--replace-dir", type=Path, default=None, help="default <root>/generated/replace"
    )
    parser.add_argument(
        "--xbe", type=Path, default=None, help="default: the usual extraction paths"
    )
    parser.add_argument(
        "--out", type=Path, default=None, help=f"default <root>/{DEFAULT_REPLACE_SNAPSHOT}"
    )
    parser.add_argument("--check", action="store_true", help="verify the tracked file is current")
    args = parser.parse_args(argv)
    out = args.out or args.root / DEFAULT_REPLACE_SNAPSHOT
    try:
        document = build_snapshot(args.root, replace_dir=args.replace_dir, xbe=args.xbe)
    except SnapshotError as error:
        print(f"snapshot refused: {error}", file=sys.stderr)
        return 1
    if args.check:
        tracked = json.loads(out.read_text(encoding="utf-8")) if out.is_file() else {}
        stale = [
            key
            for key in sorted(set(document) | set(tracked))
            if key not in VOLATILE and document.get(key) != tracked.get(key)
        ]
        if stale:
            print(f"{out} is NOT current (differs in: {', '.join(stale)})", file=sys.stderr)
            return 1
        print(f"{out} is current")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(document), encoding="utf-8")
    summary = document["summary"]
    print(f"wrote {out}: {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
