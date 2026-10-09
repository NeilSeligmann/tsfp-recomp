"""Merge per-function replacement proofs into ONE proof.json (T1266 refresh 3).

The harness takes one `--cases-per-function` per run, but a few registrations only reach
a verdict at a raised count (see docs/t77-round4-low.md). Every proof entry already
records its own `cases`, `random_cases` and verdict data, and `tools.coverage` gates read
only those per-entry values, never the top-level `cases_per_function`. So one file may
carry entries from several runs as long as nothing is picked silently:

* every input proof must share the manifest_sha of the base,
* an override replaces the base entry for its VA and the VA must exist in the base,
* a VA may be overridden by exactly ONE override (no best-of-N across runs),
* an override entry must be a judged entry (not a `--only-va` skip row),
* each entry gets `source_run` and `source_cases_per_function` so the origin is recorded.

Thresholds are never touched. `--snapshot` then refreshes the tracked proof snapshot (T1462).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from tools.harness.named_global_objects import ROW_FIELD, attach_context
from tools.harness.scoped_fixture_proof import attach_context as attach_custom_fixture_context

from .proof_contract import (
    FP_SCALAR_EVIDENCE_CLASS,
    fp_scalar_closure,
    fp_scalar_record,
    validate_document,
)

SKIP_MARK = "--only-va"


class MergeError(ValueError):
    pass


def _load(path: Path) -> dict:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("kind") != "replacement-proof":
        raise MergeError(f"{path}: not a replacement-proof")
    return document


def _judged(entry: dict) -> bool:
    return SKIP_MARK not in str(entry.get("unjudgeable") or "")


def merge(base: dict, overrides: list[tuple[str, dict]], base_label: str) -> dict:
    """Return the merged document. `overrides` is `[(label, proof_document), ...]`."""
    inputs = [(base_label, base), *overrides]
    if len({label for label, _ in inputs}) != len(inputs):
        raise MergeError("duplicate source run label")
    for label, document in inputs:
        try:
            validate_document(document)
        except ValueError as error:
            raise MergeError(f"{label}: {error}") from error

    def selected(document: dict) -> dict:
        result = (
            {"fixture_provider_selection": document["fixture_provider_selection"]}
            if document.get("fixture_provider_selection") is not None
            else {}
        )

        if document.get("synth_domain") is True:
            result["synth_domain"] = True
        return result

    origins = {entry["va"]: (base_label, base, entry) for entry in base["functions"]}
    sha = base["manifest_sha"]
    merged = {}
    for entry in base["functions"]:
        merged[entry["va"]] = {
            **entry,
            "source_run": base_label,
            "source_cases_per_function": base["cases_per_function"],
        }
    taken: dict[str, str] = {}
    runs = [
        {
            "label": base_label,
            "cases_per_function": base["cases_per_function"],
            "seed": base["seed"],
            **selected(base),
        }
    ]
    for label, document in overrides:
        if document["manifest_sha"] != sha:
            raise MergeError(f"{label}: manifest_sha differs from the base")
        used = 0
        for entry in document["functions"]:
            previous = merged.get(entry["va"])
            if previous is not None and previous.get("input_contract") != entry.get(
                "input_contract"
            ):
                raise MergeError(f"{entry['va']}: incompatible or missing exact input contract")
            if previous is not None and previous.get("scoped_fixture_contract") != entry.get(
                "scoped_fixture_contract"
            ):
                raise MergeError(f"{entry['va']}: incompatible or missing custom fixture contract")
            if not _judged(entry):
                continue
            va = entry["va"]
            if va not in merged:
                raise MergeError(f"{label}: {va} is not in the base proof")
            if va in taken:
                raise MergeError(f"{va} supplied by both {taken[va]} and {label}")
            previous = merged[va]
            if previous.get(ROW_FIELD) != entry.get(ROW_FIELD):
                raise MergeError(f"{va}: incompatible or missing named graph provenance")
            if previous.get("state_contract") is not None and (
                previous.get("state_contract") != entry.get("state_contract")
                or previous.get("source_closure") != entry.get("source_closure")
            ):
                raise MergeError(
                    f"{va}: incompatible scalar/vector state or original closure override"
                )
            origins[va] = (label, document, entry)
            taken[va] = label
            merged[va] = {
                **entry,
                "source_run": label,
                "source_cases_per_function": document["cases_per_function"],
            }
            used += 1
        if used == 0:
            raise MergeError(f"{label}: no judged entry")
        runs.append(
            {
                "label": label,
                "cases_per_function": document["cases_per_function"],
                "seed": document["seed"],
                **selected(document),
            }
        )
    out = {key: value for key, value in base.items() if key != "functions"}
    out["merged_runs"] = runs
    out["functions"] = [merged[va] for va in sorted(merged)]
    attach_context(out)
    attach_custom_fixture_context(out)
    vector = any(document["schema"] == 2 for _, document in inputs)
    if vector:
        out["schema"] = 2
        out["state_contract_schema"] = 1
        out["live_call_closure"] = None
    explicit_selection = any(
        entry.get("fixture_provider_selection") is not None
        for _, document in inputs
        for entry in document["functions"]
    )
    synth = any(
        document.get("synth_domain") is True
        or any(row.get("synth_domain") is not None for row in document["functions"])
        for _, document in inputs
    )
    if vector or explicit_selection or synth:
        preserved_runs = {}
        for row in out["functions"]:
            label, document, original = origins[row["va"]]
            prior_runs = document.get("merged_runs")
            if prior_runs:
                original_label = original.get("source_run")
                source = next((run for run in prior_runs if run["label"] == original_label), None)
                if source is None:
                    raise MergeError(f"{row['va']}: missing nested source run")
                run_label = f"{label}/{original_label}"
                run = {**source, "label": run_label}
            else:
                run_label = label
                run = {
                    "label": label,
                    "seed": document["seed"],
                    "cases_per_function": document["cases_per_function"],
                    "live_call_closure": document.get("live_call_closure"),
                    "subject": document.get("subject"),
                    "proof_schema": document["schema"],
                    **selected(document),
                }
            if run_label in preserved_runs and preserved_runs[run_label] != run:
                raise MergeError("inconsistent source run provenance")
            preserved_runs[run_label] = run
            row.update(
                source_run=run_label,
                source_cases_per_function=run["cases_per_function"],
            )
            if vector:
                row.update(
                    source_seed=original.get("source_seed"),
                    state_contract=original.get("state_contract"),
                    source_closure=original.get("source_closure"),
                )
                if fp_scalar_closure(original.get("source_closure")):
                    # T1624: the fp-scalar-v1 record and class travel with the row.
                    try:
                        row.update(
                            fp_scalar=fp_scalar_record(original["source_closure"], row["va"]),
                            evidence_class=FP_SCALAR_EVIDENCE_CLASS,
                        )
                    except ValueError as error:
                        raise MergeError(f"{row['va']}: {error}") from error
        out["merged_runs"] = list(preserved_runs.values())
    try:
        validate_document(out)
    except ValueError as error:
        raise MergeError(f"merged proof: {error}") from error
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", type=Path, required=True, help="whole-registry proof.json")
    parser.add_argument(
        "--override",
        action="append",
        default=[],
        metavar="LABEL=PATH",
        help="one raised-count proof whose judged entries replace the base entries (repeatable)",
    )
    parser.add_argument("--base-label", default="default")
    parser.add_argument(
        "--snapshot",
        action="store_true",
        help="after merging, rewrite the tracked docs/data/replace-proof-snapshot.json from "
        "--out and the manifest.json beside it (T1462); needs the XBE and the function table",
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    overrides = []
    for item in args.override:
        label, _, path = item.partition("=")
        if not path:
            parser.error(f"--override needs LABEL=PATH, got {item!r}")
        overrides.append((label, _load(Path(path))))
    try:
        document = merge(_load(args.base), overrides, args.base_label)
    except MergeError as error:
        print(f"merge refused: {error}", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"merged {len(overrides)} override runs into {args.out}")
    if args.snapshot:
        # Lazy: the snapshot tool needs the whole coverage stack, merging alone does not.
        from tools.replace import proof_snapshot

        return proof_snapshot.main(["--replace-dir", str(args.out.parent)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
