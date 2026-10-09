# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1624 mutation suite: every fp-scalar-v1 snapshot/merge/shard guard must be load-bearing.

    python -m tools.t1624_fp_mutants            # run every mutant, one at a time
    python -m tools.t1624_fp_mutants --list     # print the mutants and exit
    python -m tools.t1624_fp_mutants --only G03 # run one

Each mutant is an exact-text substitution in one source file. The anchor must occur exactly
once (a drifted anchor is reported as DRIFT, never as killed). The mutated tree is tested with
`tests/test_t1624_fp_scalar_snapshot.py` under a 2 GB address-space limit and a hard timeout,
then the file is restored byte for byte. A mutant is KILLED when the test run fails and
SURVIVED when it passes; any SURVIVED or DRIFT mutant makes the exit status non-zero.
"""

from __future__ import annotations

import argparse
import resource
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

TEST = "tests/test_t1624_fp_scalar_snapshot.py"
ADDRESS_LIMIT = 2_000_000 * 1024  # ulimit -v 2000000 (KiB)
PC = "tools/replace/proof_contract.py"
CV = "tools/coverage.py"
PS = "tools/replace/proof_snapshot.py"
PH = "tools/replace/proof_shard.py"
MP = "tools/replace/merge_proof.py"
BY = "tools/coverage_by_subsystem.py"
BP = "tools/replace/batch_prove.py"
VS = "tools/harness/vector_scalar.py"


@dataclass(frozen=True)
class Mutant:
    name: str
    path: str
    anchor: str
    replacement: str
    guard: str


MUTANTS = (
    Mutant("G01", PC, 'if block.get("mode") != FP_SCALAR_MODE:', "if False:", "receipt mode"),
    Mutant(
        "G02",
        PC,
        'if block.get("mxcsr_control_word") != f"0x{FP_SCALAR_MXCSR:04x}":',
        "if False:",
        "pinned MXCSR control word",
    ),
    Mutant(
        "G03",
        PC,
        'if block.get("mxcsr_status_mask") != f"0x{FP_SCALAR_STATUS_MASK:02x}":',
        "if False:",
        "sticky status mask",
    ),
    Mutant("G04", PC, "if excluded > cases:", "if False:", "NaN-pair count within cases"),
    Mutant(
        "G05",
        PC,
        "if type(cap) not in (int, float) or cap != EXCLUSION_CAP:",
        "if False:",
        "NaN-pair cap pinned",
    ),
    Mutant(
        "G06",
        PC,
        '        or matrix["result_file"] != FP_SCALAR_MATRIX_FILE\n',
        "",
        "matrix result file name",
    ),
    Mutant(
        "G07",
        PC,
        '    _sha(matrix["sha256"], f"{where}: matrix sha256")\n',
        "",
        "matrix sha256 format",
    ),
    Mutant(
        "G08",
        PC,
        '        or gate["passed"] != (not gate["failures"])\n',
        "",
        "receipt gate consistency",
    ),
    Mutant(
        "G09",
        PC,
        'if not isinstance(build, dict) or type(build.get("passed")) is not bool:',
        "if False:",
        "receipt build record present",
    ),
    Mutant(
        "G10",
        PC,
        '        record.get("receipt_gate_passed") is True\n',
        "        True\n",
        "replay gate: receipt gate passed",
    ),
    Mutant(
        "G11",
        PC,
        '        and record.get("receipt_build_passed") is True\n',
        "",
        "replay gate: receipt build passed",
    ),
    Mutant(
        "G12",
        PC,
        "        and excluded <= cap * cases\n",
        "",
        "replay gate: exclusions within cap",
    ),
    Mutant(
        "G13",
        PC,
        'if record["fingerprint"] != fp_scalar_record_fingerprint(record):',
        "if False:",
        "record fingerprint",
    ),
    Mutant(
        "G14",
        PC,
        "    if not _equal(stored, derived):\n",
        "    if False:\n",
        "stored record equals receipt",
    ),
    Mutant(
        "G15",
        PC,
        "    if stored_class != FP_SCALAR_EVIDENCE_CLASS:\n",
        "    if False:\n",
        "evidence class present",
    ),
    Mutant(
        "G16",
        PC,
        '    if not _equal(row.get("state_contract"), FP_SCALAR_CONTRACT):\n',
        "    if False:\n",
        "fp row needs fp contract",
    ),
    Mutant(
        "G17",
        PC,
        '        if _equal(row.get("state_contract"), FP_SCALAR_CONTRACT):\n',
        "        if False:\n",
        "legacy row with fp contract",
    ),
    Mutant(
        "G18",
        PC,
        '        if (isinstance(closure, dict) and "fp_scalar" in closure) or stored is not None:',
        "        if False:",
        "fp evidence on a legacy row",
    ),
    Mutant("G19", PC, "    if len(fp_matrices) > 1:\n", "    if False:\n", "one matrix identity"),
    Mutant(
        "G20",
        PC,
        '    if is_snapshot and (document.get("fp_scalar_schema") == FP_SCALAR_RECORD_VERSION) != bool(\n        fp_rows\n    ):',
        "    if False:",
        "snapshot schema marker",
    ),
    Mutant(
        "G21",
        PC,
        "require_record=is_snapshot)",
        "require_record=False)",
        "snapshot rows must store the record",
    ),
    Mutant(
        "G22",
        PC,
        '                    or row.get("fp_scalar") is not None\n',
        "",
        "schema1 rows carry no fp record",
    ),
    Mutant(
        "G23",
        PC,
        "        fp_record = validate_fp_scalar_row(row, closure, va, require_record=is_snapshot)\n",
        "        fp_record = None\n",
        "validate_document runs the fp row check",
    ),
    Mutant(
        "G24",
        PC,
        "if not isinstance(record, dict) or set(record) != FP_SCALAR_RECORD_KEYS:",
        "if not isinstance(record, dict):",
        "record key set",
    ),
    Mutant(
        "M01",
        MP,
        '                if fp_scalar_closure(original.get("source_closure")):\n',
        "                if False:\n",
        "merge carries the record and class",
    ),
    Mutant(
        "C01",
        CV,
        "        return GATE_FP_SCALAR\n    return None",
        "        return None\n    return None",
        "fp gate in first_failing_gate",
    ),
    Mutant(
        "C02",
        CV,
        "ALL_GATES = (*GATE_ORDER, GATE_FP_SCALAR)",
        "ALL_GATES = GATE_ORDER",
        "failure counts know the fp gate",
    ),
    Mutant(
        "C03",
        CV,
        '            and entry.proof.fp_scalar.get("matrix_sha256") != tree.fp_matrix_sha256\n',
        "            and False\n",
        "replay matrix identity",
    ),
    Mutant(
        "C04",
        CV,
        '    if raw.get("state_contract") is not None and fp_scalar_closure(raw.get("source_closure")):\n',
        "    if False:\n",
        "live proof derives the record",
    ),
    Mutant(
        "C05",
        CV,
        "        fp_matrix_sha256=hashlib.sha256(matrix.read_bytes()).hexdigest()\n        if matrix.is_file()\n        else None,",
        "        fp_matrix_sha256=None,",
        "source tree pins the matrix",
    ),
    Mutant(
        "C06",
        CV,
        "        fp_scalar=_fp_scalar_from(raw, va),\n",
        "        fp_scalar=None,\n",
        "ProofEntry carries the record",
    ),
    Mutant(
        "S01",
        PS,
        '        record["evidence_class"] = FP_SCALAR_EVIDENCE_CLASS\n',
        "",
        "snapshot row evidence class",
    ),
    Mutant(
        "S02",
        PS,
        '            if item["fp_scalar"]["matrix_sha256"] != live_matrix:  # type: ignore[index]',
        "            if False:",
        "snapshot refuses a stale matrix identity",
    ),
    Mutant(
        "S03",
        PS,
        '        document["fp_scalar_schema"] = FP_SCALAR_RECORD_VERSION\n',
        "",
        "snapshot schema marker written",
    ),
    Mutant(
        "S04",
        PS,
        '        record["fp_scalar"] = dict(entry.fp_scalar)\n',
        "        pass\n",
        "snapshot row record",
    ),
    Mutant(
        "S05",
        PS,
        '        document["summary"]["fp_scalar_proven"] = sum(  # type: ignore[index]\n            1 for item in fp_rows if item["gate"] is None\n        )',
        '        document["summary"]["fp_scalar_proven"] = 0  # type: ignore[index]',
        "snapshot counts fp proven separately",
    ),
    Mutant(
        "H01",
        PH,
        '    if mode != FP_SCALAR_MODE:\n        raise ShardError(f"{where}: fp-scalar-v1 snapshot row needs',
        '    if False:\n        raise ShardError(f"{where}: fp-scalar-v1 snapshot row needs',
        "shard: fp row needs fp mode",
    ),
    Mutant(
        "H02",
        PH,
        '        if mode != "legacy" or "--fp-build-record" in flags:',
        "        if False:",
        "shard: non-fp row never runs fp mode",
    ),
    Mutant(
        "H03",
        PH,
        "    if len(positions) != 1 or positions[0] + 1 >= len(flags):",
        "    if False:",
        "shard: exactly one build record",
    ),
    Mutant(
        "H04",
        PH,
        '    if "--live-vector-state" not in flags or "--live-call-closure" not in flags:',
        "    if False:",
        "shard: fp recipe needs vector state and closure",
    ),
    Mutant(
        "H05",
        PH,
        '    if len(positions) > 1:\n        raise ShardError(f"{where}: repeated --vector-mode in recipe")',
        '    if False:\n        raise ShardError(f"{where}: repeated --vector-mode in recipe")',
        "shard: --vector-mode appears once",
    ),
    Mutant(
        "H06",
        PH,
        '    if value not in ("legacy", FP_SCALAR_MODE):',
        "    if False:",
        "shard: --vector-mode value set",
    ),
    Mutant(
        "H07",
        PH,
        "        if record[key] != stored[key]:",
        "        if False:",
        "shard: reproduced receipt identity",
    ),
    Mutant(
        "H08",
        PH,
        "        raise ShardError(f\"{function['va']}: fp-scalar-v1 row produced no fp-scalar receipt\")",
        "        return",
        "shard: fp row must reproduce an fp receipt",
    ),
    Mutant(
        "H09",
        PH,
        "        validate_fp_scalar_record(stored, where)\n",
        "        pass\n",
        "shard: plan validates the stored record",
    ),
    Mutant(
        "H10",
        PH,
        "            raise ShardError(f\"{function['va']}: run produced fp-scalar-v1 for a legacy row\")",
        "            return",
        "shard: legacy row never produces fp",
    ),
    Mutant(
        "B01",
        BY,
        "                elif evidence == EVIDENCE_FP_SCALAR:\n                    row.fp_scalar += 1\n",
        "                elif evidence == EVIDENCE_FP_SCALAR:\n                    row.default_domain += 1\n",
        "fp-scalar counted as default-domain",
    ),
    Mutant(
        "B02",
        BY,
        "    with_fp = any(row.fp_scalar for row in rows)",
        "    with_fp = False",
        "fp-scalar CSV column",
    ),
    Mutant(
        "P01",
        BP,
        '        parts.append(fp_scalar_matrix_identity(Path(__file__).resolve().parents[2])["sha256"])',
        "        pass",
        "tuple fingerprint includes the matrix",
    ),
    Mutant("V01", VS, '            "matrix": self.matrix,\n', "", "receipt names the matrix"),
)


def run_one(mutant: Mutant, root: Path, timeout: int) -> str:
    path = root / mutant.path
    original = path.read_bytes()
    text = original.decode("utf-8")
    if text.count(mutant.anchor) != 1:
        return "DRIFT"
    path.write_bytes(text.replace(mutant.anchor, mutant.replacement).encode("utf-8"))
    try:
        completed = subprocess.run(
            [sys.executable, "-m", "pytest", "-q", "-x", "-p", "no:cacheprovider", TEST],
            cwd=root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=timeout,
            preexec_fn=lambda: resource.setrlimit(
                resource.RLIMIT_AS, (ADDRESS_LIMIT, ADDRESS_LIMIT)
            ),
            check=False,
        )
    except subprocess.TimeoutExpired:
        return "KILLED(timeout)"
    finally:
        path.write_bytes(original)
    return "SURVIVED" if completed.returncode == 0 else "KILLED"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root")
    parser.add_argument("--only", action="append", default=[], help="mutant name (repeatable)")
    parser.add_argument("--list", action="store_true", help="print the mutants and exit")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per mutant")
    args = parser.parse_args(argv)
    chosen = [m for m in MUTANTS if not args.only or m.name in args.only]
    if len({m.name for m in MUTANTS}) != len(MUTANTS):
        parser.error("duplicate mutant names")
    if args.list:
        for mutant in chosen:
            print(f"{mutant.name}\t{mutant.path}\t{mutant.guard}")
        return 0
    bad = 0
    for mutant in chosen:
        outcome = run_one(mutant, args.root, args.timeout)
        bad += not outcome.startswith("KILLED")
        print(f"{mutant.name}\t{outcome}\t{mutant.path}\t{mutant.guard}", flush=True)
    print(f"{len(chosen)} mutants, {bad} not killed")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
