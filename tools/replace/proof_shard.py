# SPDX-License-Identifier: GPL-3.0-or-later
"""Run deterministic per-function proof recipes in parallel, with scratch outputs only.

--shards 1 and --shards N execute identical --only-va recipes. This is deliberately
not equivalent to a monolithic harness sweep: that sweep advances the random index
between roots, whereas --only-va starts at zero. See docs/t1498-proof-shards.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from tools.replace.merge_proof import merge
from tools.replace.proof_contract import (
    FP_SCALAR_MODE,
    fp_scalar_closure,
    fp_scalar_record,
    validate_fp_scalar_record,
)
from tools.replace.synth_contract import artifact_pin, validate_block


class ShardError(ValueError):
    pass


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


#: Receipt identity that a re-run must reproduce exactly (counts may differ and are re-gated).
FP_IDENTITY_KEYS = (
    "mode",
    "evidence_class",
    "mxcsr_control_word",
    "mxcsr_status_mask",
    "nan_pair_cap",
    "matrix_result_file",
    "matrix_sha256",
)


def recipe_vector_mode(flags: list[str], where: str) -> str:
    """The recipe's `--vector-mode` value ("legacy" when absent). Exactly one, from the set."""
    positions = [index for index, flag in enumerate(flags) if flag == "--vector-mode"]
    if len(positions) > 1:
        raise ShardError(f"{where}: repeated --vector-mode in recipe")
    if not positions:
        return "legacy"
    value = flags[positions[0] + 1] if positions[0] + 1 < len(flags) else None
    if value not in ("legacy", FP_SCALAR_MODE):
        raise ShardError(f"{where}: --vector-mode needs legacy or {FP_SCALAR_MODE}, got {value!r}")
    return value


def check_fp_recipe(row: dict, flags: list[str], where: str) -> None:
    """Fail closed: an fp-scalar-v1 snapshot row is re-run in fp mode with its build record, and
    a row that is not fp-scalar-v1 is never re-run in fp mode."""
    mode = recipe_vector_mode(flags, where)
    stored = row.get("fp_scalar")
    row_is_fp = stored is not None or row.get("evidence_class") is not None
    if not row_is_fp:
        if mode != "legacy" or "--fp-build-record" in flags:
            raise ShardError(f"{where}: recipe requests fp-scalar-v1 for a row that is not")
        return
    try:
        validate_fp_scalar_record(stored, where)
    except ValueError as error:
        raise ShardError(str(error)) from error
    if mode != FP_SCALAR_MODE:
        raise ShardError(f"{where}: fp-scalar-v1 snapshot row needs --vector-mode {FP_SCALAR_MODE}")
    if "--live-vector-state" not in flags or "--live-call-closure" not in flags:
        raise ShardError(f"{where}: fp-scalar-v1 recipe needs --live-vector-state and closure")
    positions = [index for index, flag in enumerate(flags) if flag == "--fp-build-record"]
    if len(positions) != 1 or positions[0] + 1 >= len(flags):
        raise ShardError(f"{where}: fp-scalar-v1 recipe needs exactly one --fp-build-record PATH")


def check_fp_reproduction(function: dict, proof: dict) -> None:
    """After the run: the produced receipt must carry the snapshot row's fp identity."""
    stored = function.get("fp_identity")
    row = next(item for item in proof["functions"] if item["va"] == function["va"])
    closure = row.get("source_closure")
    produced = fp_scalar_closure(closure)
    if stored is None:
        if produced:
            raise ShardError(f"{function['va']}: run produced fp-scalar-v1 for a legacy row")
        return
    if not produced:
        raise ShardError(f"{function['va']}: fp-scalar-v1 row produced no fp-scalar receipt")
    try:
        record = fp_scalar_record(closure, function["va"])
    except ValueError as error:
        raise ShardError(str(error)) from error
    for key in FP_IDENTITY_KEYS:
        if record[key] != stored[key]:
            raise ShardError(f"{function['va']}: fp-scalar {key} differs from the snapshot row")


def plan(manifest: dict, snapshot: dict, recipes: dict, selected: list[str], shards: int) -> dict:
    """Partition registrations, retaining the complete manifest identity in every run."""
    if shards < 1:
        raise ShardError("shards must be positive")
    rows = {int(row["va"], 16): row for row in snapshot["functions"]}
    runs = {run["label"]: run for run in snapshot["harness"].get("merged_runs", [])}
    wanted = {int(va, 16) for va in selected}
    registered = {int(row["va"], 16) for row in manifest["functions"]}
    if wanted - registered:
        raise ShardError("selected VA is absent from manifest")
    if not registered:
        raise ShardError("manifest has no functions")
    functions = []
    for va in sorted(wanted or registered):
        if va not in rows:
            raise ShardError(f"{va:#010x}: no snapshot recipe; supply a matching snapshot")
        row = rows[va]
        label = row["source_run"]
        run = runs.get(label, snapshot["harness"] if label == "default" else None)
        if run is None:
            raise ShardError(f"{va:#010x}: snapshot has no source run {label!r}")
        if row["source_cases_per_function"] != run.get("cases_per_function"):
            raise ShardError(f"{va:#010x}: snapshot count differs from its source run")
        if (
            type(run.get("seed")) is not int
            or type(row["source_cases_per_function"]) is not int
            or row["source_cases_per_function"] < 1
        ):
            raise ShardError(f"{va:#010x}: invalid seed/count recipe")
        recipe = recipes.get(f"{va:#010x}", row.get("recipe"))
        if recipe is None:
            raise ShardError(
                f"{va:#010x}: snapshot omits closure/provider recipe; explicit recipe required"
            )
        if (
            recipe.get("snapshot_manifest_sha", snapshot["manifest_sha"])
            != snapshot["manifest_sha"]
        ):
            raise ShardError(f"{va:#010x}: recipe belongs to a different snapshot")
        if not isinstance(recipe.get("args"), list):
            raise ShardError(f"{va:#010x}: recipe args must be an explicit list")
        flags = recipe["args"]
        allowed = {
            "--live-call-closure",
            "--live-vector-state",
            "--vector-mode",
            "--fp-build-record",
            "--live-call-boundary",
            "--live-memory-dword",
            "--live-stack-dword",
            "--static-jump-tables",
            "--legacy-seeding",
            "--compare-eflags",
            "--synth-domain",
            "--synth-domain-version",
        }
        for flag in flags:
            if not isinstance(flag, str) or (flag.startswith("--") and flag not in allowed):
                raise ShardError(f"{va:#010x}: unsafe/unknown recipe argument {flag!r}")
        if not recipe.get("evidence") or "fixture_providers" not in recipe:
            raise ShardError(f"{va:#010x}: recipe needs evidence and fixture_providers")
        check_fp_recipe(row, flags, f"{va:#010x}")
        synth = row.get("synth_domain")
        synth_expected = run.get("synth_domain") is True or "synth-domain" in (
            row.get("fixture_provider_selection") or run.get("fixture_provider_selection") or []
        )
        synth_requested = "--synth-domain" in flags
        pin_path = recipe.get("synth_domain_pin")
        if synth_expected or synth is not None or synth_requested or pin_path is not None:
            if not synth_requested or not isinstance(pin_path, str):
                raise ShardError(
                    f"{va:#010x}: synth recipe requires --synth-domain and synth_domain_pin"
                )
            try:
                pin = artifact_pin(Path(pin_path), va, committed=True)
                validate_block(synth, va, pin=pin)
                positions = [i for i, flag in enumerate(flags) if flag == "--synth-domain-version"]
                version = "t1773-synth-v1"
                if positions:
                    if len(positions) != 1 or positions[0] + 1 >= len(flags):
                        raise ValueError("synth recipe needs exactly one generator version")
                    version = flags[positions[0] + 1]
                if version != synth["generator_version"]:
                    raise ValueError("synth recipe generator version differs from snapshot")
            except ValueError as error:
                raise ShardError(str(error)) from error
        function = {
            "va": f"{va:#010x}",
            "cases_per_function": row["source_cases_per_function"],
            "seed": run["seed"],
            "edge_cases": snapshot["harness"]["edge_cases"],
            "recipe": recipe,
        }
        if synth is not None:
            function["synth_identity"] = dict(synth)
            function["synth_pin"] = pin
        if row.get("fp_scalar") is not None:
            function["fp_identity"] = {key: row["fp_scalar"][key] for key in FP_IDENTITY_KEYS}
        functions.append(function)
    # Largest runs first balances the raised-count ladder without changing any recipe.
    buckets: list[list[dict]] = [[] for _ in range(shards)]
    costs = [0] * shards
    for function in sorted(functions, key=lambda item: (-item["cases_per_function"], item["va"])):
        index = min(range(shards), key=lambda index: (costs[index], index))
        buckets[index].append(function)
        costs[index] += function["cases_per_function"]
    return {
        "kind": "replacement-shard-plan",
        "manifest_sha": manifest["manifest_sha"],
        "random_index_policy": "per-function-zero",
        "shards": buckets,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for name in ("manifest", "snapshot", "recipes", "out-dir"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--only-va", action="append", default=[])
    parser.add_argument("--plan-only", action="store_true")
    for name in ("xbe", "functions", "subject"):
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--snapshot-out", type=Path)
    args = parser.parse_args(argv)
    try:
        # Keep every artifact in an explicitly selected scratch directory.
        scratch = Path("tmp").resolve()
        if not args.out_dir.resolve().is_relative_to(scratch):
            raise ShardError("out-dir must be a new directory under repository tmp")
        if args.out_dir.exists():
            raise ShardError("out-dir must be new (preserve prior receipts)")
        if args.snapshot_out and not args.snapshot_out.resolve().is_relative_to(
            args.out_dir.resolve()
        ):
            raise ShardError("snapshot-out must be inside out-dir")
        manifest = read(args.manifest)
        document = plan(
            manifest, read(args.snapshot), read(args.recipes), args.only_va, args.shards
        )
        document["input_sha256"] = {
            name: hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in (
                ("manifest", args.manifest),
                ("snapshot", args.snapshot),
                ("recipes", args.recipes),
            )
        }
        if not args.plan_only and any(
            getattr(args, name) is None for name in ("xbe", "functions", "subject")
        ):
            raise ShardError("execution requires --xbe, --functions and --subject")
        write(args.out_dir / "plan.json", document)
        write(args.out_dir / "manifest.json", manifest)
        if args.plan_only:
            return 0

        def execute(bucket: tuple[int, list[dict]]) -> list[tuple[str, dict]]:
            index, functions = bucket
            outputs = []
            for function in functions:
                directory = args.out_dir / f"shard-{index}" / function["va"]
                directory.mkdir(parents=True)
                command = [
                    sys.executable,
                    "-m",
                    "tools.harness.cli",
                    "--replacement",
                    "--min-size",
                    "1",
                    "--max-size",
                    "512",
                    "--no-stub-calls",
                    "--no-memmove-probe",
                    "--only-va",
                    function["va"],
                    "--cases-per-function",
                    str(function["cases_per_function"]),
                    "--seed",
                    str(function["seed"]),
                    "--edge-cases" if function["edge_cases"] else "--no-edge-cases",
                ]
                for name in ("xbe", "functions", "subject"):
                    command.extend([f"--{name}", str(getattr(args, name).resolve())])
                command.extend(
                    [
                        "--image",
                        str(directory / "guest.img"),
                        "--out",
                        str(directory / "results.csv"),
                        "--proof-out",
                        str(directory / "proof.json"),
                        *function["recipe"]["args"],
                    ]
                )
                if function.get("synth_pin"):
                    command.extend(
                        ["--synth-domain-pin", f"{function['va']}={function['synth_pin']}"]
                    )
                write(directory / "command.json", {"argv": command})
                with (directory / "run.log").open("w") as log:
                    completed = subprocess.run(
                        command, stdout=log, stderr=subprocess.STDOUT, check=False
                    )
                if completed.returncode not in (0, 1):
                    raise ShardError(
                        f"{function['va']}: harness refused with exit {completed.returncode}"
                    )
                proof = read(directory / "proof.json")
                if proof["manifest_sha"] != manifest["manifest_sha"]:
                    raise ShardError("subject manifest differs from input manifest")
                judged = [row for row in proof["functions"] if row["va"] == function["va"]]
                if len(judged) != 1 or "--only-va" in str(judged[0].get("unjudgeable")):
                    raise ShardError(f"{function['va']}: no measured entry")
                check_fp_reproduction(function, proof)
                if function.get("synth_identity") is not None:
                    validate_block(
                        judged[0].get("synth_domain"), function["va"], pin=function["synth_pin"]
                    )
                    for key in (
                        "generator_version",
                        "domain_sha256",
                        "case_stream_sha256",
                        "executed_stream_sha256",
                    ):
                        if judged[0]["synth_domain"][key] != function["synth_identity"][key]:
                            raise ShardError(f"{function['va']}: synth {key} differs from snapshot")
                elif judged[0].get("synth_domain") is not None:
                    raise ShardError(f"{function['va']}: unexpected synthesized-domain receipt")
                outputs.append((function["va"], proof))
            return outputs

        with ThreadPoolExecutor(max_workers=args.shards) as pool:
            outputs = [
                item for batch in pool.map(execute, enumerate(document["shards"])) for item in batch
            ]
        outputs.sort()
        if not outputs:
            raise ShardError("manifest has no functions")
        base = outputs[0][1]
        # First run supplies the full manifest rows; each other root overrides once.
        merged = merge(base, outputs[1:], outputs[0][0])
        write(args.out_dir / "proof.json", merged)
        if args.snapshot_out:
            from tools.replace.proof_snapshot import build_snapshot

            snapshot = build_snapshot(Path("."), replace_dir=args.out_dir, xbe=args.xbe)
            by_va = {
                function["va"]: function for bucket in document["shards"] for function in bucket
            }
            for row in snapshot["functions"]:
                if row["va"] in by_va:
                    row["recipe"] = by_va[row["va"]]["recipe"]
            snapshot["random_index_policy"] = document["random_index_policy"]
            write(args.snapshot_out, snapshot)
        return 0
    except (ShardError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"shard proof refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
