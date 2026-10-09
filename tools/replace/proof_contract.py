# SPDX-License-Identifier: GPL-3.0-or-later
"""Versioned vector evidence contract shared by proof producers and replay readers.

Validation authenticates the recorded contract/graph, not the original bytes themselves.
The original execution producer is responsible for fingerprinting/decoding those bytes.
"""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Mapping
from copy import deepcopy
from pathlib import Path

from tools.harness.fixture_selection import validate_selection
from tools.harness.named_global_objects import validate_document as validate_named_graph_document
from tools.harness.scoped_fixture_proof import validate_document as validate_custom_fixture_document

from .input_abi import validate_contract as validate_input_contract

VECTOR_SCHEMA = 2
ISOLATION_CONTRACT = "pristine-image-ordered-patches-tbflush-v1"
VECTOR_CONTRACT = {
    "kind": "raw-xmm-mxcsr-v1",
    "xmm": {"registers": [f"xmm{i}" for i in range(8)], "bits": 128, "comparison": "exact"},
    "mxcsr": {"bits": 16, "comparison": "exact"},
    "missing_output": "refuse-verdict",
    "input_stream": "vector-v1",
    "instruction_subset": "legacy-sse-moves-and-register-bitwise-v1",
    "x87_comparison": "inherited-limited-x87-v1",
    "oracle_case_isolation": ISOLATION_CONTRACT,
    "within_case_native_self_modification": "unsupported",
}
VECTOR_DESCRIPTION = {
    "enabled": True,
    "registers": "all8 XMM raw128-bit exact equality; missing output refuses verdict",
    "mxcsr": "exact16-bit equality; movement/bitwise preserve control/status",
    "input_stream": "vector-v1 SHA256(seed,index,VA,register), first16 little-endian bytes",
    "refused": "MMX/EMMS, arithmetic, aligned-memory movement, unlisted vector forms",
}

# T1620 fp-scalar-v1: additive contract for the scalar-float vector mode. The legacy
# VECTOR_CONTRACT/VECTOR_DESCRIPTION above are byte-identical and still the default.
FP_SCALAR_MODE = "fp-scalar-v1"
FP_SCALAR_CONTRACT = {
    "kind": "raw-xmm-mxcsr-fp-scalar-v1",
    "xmm": {"registers": [f"xmm{i}" for i in range(8)], "bits": 128, "comparison": "exact"},
    "mxcsr": {
        "bits": 16,
        "control_word": "0x1f80 fixed (RC nearest, FTZ 0, DAZ 0, exceptions masked)",
        "comparison": "exact outside sticky status mask 0x3f",
    },
    "missing_output": "refuse-verdict",
    "input_stream": "fp-scalar-v1",
    "instruction_subset": "legacy-sse-moves-bitwise-plus-scalar-add-sub-mul-comis-cvtsi2ss-v1",
    "nan_pair": "two different NaN operands: counted non-verdict, never an agree",
    "subject_mxcsr": "real ldmxcsr/stmxcsr around the replacement",
    "x87_comparison": "inherited-limited-x87-v1",
    "oracle_case_isolation": ISOLATION_CONTRACT,
    "within_case_native_self_modification": "unsupported",
}
FP_SCALAR_DESCRIPTION = {
    "enabled": True,
    "mode": FP_SCALAR_MODE,
    "registers": "all8 XMM raw128-bit exact equality; missing output refuses verdict",
    "mxcsr": "control bits exact, sticky status bits 0x3f masked (Unicorn never sets them)",
    "input_stream": "fp-scalar-v1 SHA256(seed,index,VA,class key): float classes, pointee overlay",
    "refused": "MMX/EMMS, divss/sqrt/min/max/cvt other than cvtsi2ss, packed arithmetic, "
    "MXCSR and FPU control, aligned-memory movement, unlisted vector forms",
}

#: T1624 evidence class of a proof taken in fp-scalar-v1 mode. Counted separately from
#: default-domain, like the vector-state class: its verdicts are only as good as the matrix.
FP_SCALAR_EVIDENCE_CLASS = "fp-scalar"
#: Tracked native-vs-Unicorn matrix result the mode relies on (T1620). Its sha256 is the matrix
#: identity recorded in the receipt, the snapshot record and checked again on replay.
FP_SCALAR_MATRIX_FILE = "docs/evidence/t1620/matrix-report.txt"
FP_SCALAR_RECORD_VERSION = 1
FP_SCALAR_RECORD_KEYS = frozenset(
    {
        "mode",
        "evidence_class",
        "mxcsr_control_word",
        "mxcsr_status_mask",
        "nan_pair_excluded",
        "nan_pair_cases",
        "nan_pair_cap",
        "matrix_result_file",
        "matrix_sha256",
        "receipt_gate_passed",
        "receipt_build_passed",
        "fingerprint",
    }
)

X87_DESCRIPTION = {
    "stack": "depth and values; exact 80-bit when representable by subject",
    "control_word": "exact 16-bit equality",
    "status_word_mask": "0x3800 (TOP, the modeled status subset)",
    "status_word_unmodeled_mask": "0xc7ff",
    "ignored_stack_writes": "[entry ESP - 0x1000, entry ESP), returned frame locals",
}


def _equal(left: object, right: object) -> bool:
    """JSON comparison distinguishes bool/int and preserves register order."""
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


def _sha(value: object, where: str) -> None:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise ValueError(f"{where}: expected lowercase SHA256")


def _va(value: object, where: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"0x[0-9a-f]{8}", value) is None:
        raise ValueError(f"{where}: expected canonical guest VA")
    return value


def validate_contract(value: object) -> None:
    if not (_equal(value, VECTOR_CONTRACT) or _equal(value, FP_SCALAR_CONTRACT)):
        raise ValueError("missing or unsupported raw8-XMM/exact-MXCSR state contract")


def validate_closure(value: object, root: str) -> None:
    if not isinstance(value, dict) or value.get("mode") != "live-direct-callee-closure":
        raise ValueError(f"{root}: missing complete original vector closure provenance")
    if value.get("root") != root:
        raise ValueError(f"{root}: vector closure root mismatch")
    comparison = value.get("vector_comparison")
    if isinstance(comparison, dict) and comparison.get("mode") == FP_SCALAR_MODE:
        if not _equal(comparison, FP_SCALAR_DESCRIPTION):
            raise ValueError(f"{root}: missing or unsupported vector comparison")
        if not _equal(value.get("state_contract"), FP_SCALAR_CONTRACT):
            raise ValueError(f"{root}: fp-scalar-v1 needs the fp-scalar state contract")
    else:
        if not _equal(comparison, VECTOR_DESCRIPTION):
            raise ValueError(f"{root}: missing or unsupported vector comparison")
        if _equal(value.get("state_contract"), FP_SCALAR_CONTRACT):
            raise ValueError(f"{root}: legacy vector comparison with an fp-scalar contract")
    validate_contract(value.get("state_contract"))
    if not _equal(value.get("x87_comparison"), X87_DESCRIPTION):
        raise ValueError(f"{root}: missing or unsupported inherited x87 comparison limits")
    for key in ("xbe_sha256", "functions_sha256", "generated_manifest_sha256"):
        _sha(value.get(key), f"{root}: {key}")
    nodes = value.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ValueError(f"{root}: vector closure needs fingerprinted nodes")
    graph = {}
    for node in nodes:
        if not isinstance(node, dict):
            raise ValueError(f"{root}: invalid closure node")
        va = _va(node.get("va"), root)
        if va in graph:
            raise ValueError(f"{root}: duplicate closure node {va}")
        if "boundary" not in node or node["boundary"] is not None:
            raise ValueError(f"{root}: opaque vector closure boundary")
        if type(node.get("size")) is not int or node["size"] <= 0:
            raise ValueError(f"{root}: invalid original node size")
        _sha(node.get("sha256"), f"{root}: original {va}")
        calls = node.get("calls")
        if not isinstance(calls, list) or len(calls) != len(set(map(str, calls))):
            raise ValueError(f"{root}: invalid/duplicate closure calls")
        graph[va] = {_va(call, root) for call in calls}
    if root not in graph or any(
        target not in graph for calls in graph.values() for target in calls
    ):
        raise ValueError(f"{root}: missing reachable vector closure node")
    reached = {root}
    while True:
        grown = reached | {target for va in reached for target in graph[va]}
        if grown == reached:
            break
        reached = grown
    if reached != graph.keys():
        raise ValueError(f"{root}: disconnected vector closure node")


def vector_enabled(closure: object) -> bool:
    if not isinstance(closure, dict):
        return False
    comparison = closure.get("vector_comparison")
    if comparison is None:
        return False
    if not isinstance(comparison, dict) or type(comparison.get("enabled")) is not bool:
        raise ValueError("invalid vector comparison enabled marker")
    return comparison["enabled"]


def validate_document(document: Mapping[str, object]) -> None:
    """Refuse lost/mixed evidence, including legacy vector documents without schema2."""
    schema = document.get("schema")
    if type(schema) is not int or schema not in (1, VECTOR_SCHEMA):
        raise ValueError("unsupported replacement proof/snapshot schema")
    rows = document.get("functions")
    if not isinstance(rows, list):
        raise ValueError("proof functions must be a list")
    seen_vas = set()
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("proof function must be an object")
        raw_va = row.get("va")
        if not isinstance(raw_va, str) and type(raw_va) is not int:
            raise ValueError("proof function requires a guest VA")
        try:
            va = int(raw_va, 16) if isinstance(raw_va, str) else raw_va
        except ValueError as error:
            raise ValueError("proof function requires a valid guest VA") from error
        if va in seen_vas:
            raise ValueError("duplicate function VA in proof/snapshot")
        seen_vas.add(va)
        if "input_contract" in row:
            validate_input_contract(row["input_contract"])
            if row.get("regs_ignored") != []:
                raise ValueError("exact input contract cannot exclude registers")
    from .synth_contract import validate_document as validate_synth_document

    validate_synth_document(document)
    validate_named_graph_document(document)
    validate_custom_fixture_document(document)
    validate_selection(document)
    top = document.get("live_call_closure")
    top_vector = vector_enabled(top)
    if schema == 1:
        harness = document.get("harness")
        context = harness if isinstance(harness, dict) else document
        contexts = [top, context.get("live_call_closure")]
        raw_runs = context.get("merged_runs") or []
        if isinstance(raw_runs, list):
            contexts.extend(
                run.get("live_call_closure") for run in raw_runs if isinstance(run, dict)
            )
        nested_vector = any(
            vector_enabled(item)
            or (isinstance(item, dict) and item.get("state_contract") is not None)
            for item in contexts
        )
        if (
            top_vector
            or nested_vector
            or any(
                isinstance(row, dict)
                and (
                    row.get("state_contract") is not None
                    or row.get("source_closure") is not None
                    or row.get("fp_scalar") is not None
                )
                for row in rows
            )
        ):
            raise ValueError(
                "legacy schema1 vector provenance requires a validated schema2 contract"
            )
        return
    if (
        type(document.get("state_contract_schema")) is not int
        or document.get("state_contract_schema") != 1
    ):
        raise ValueError("schema2 requires explicit state_contract_schema1")
    runs = document.get("merged_runs")
    if runs is None and document.get("kind") == "replacement-proof-snapshot":
        harness = document.get("harness")
        runs = harness.get("merged_runs") if isinstance(harness, dict) else None
    if runs is not None and not isinstance(runs, list):
        raise ValueError("source runs must be a list")
    by_label = {}
    for run in runs or []:
        if (
            not isinstance(run, dict)
            or not isinstance(run.get("label"), str)
            or run["label"] in by_label
        ):
            raise ValueError("invalid/duplicate source run provenance")
        by_label[run["label"]] = run
    vector_roots = set()
    original_pins = set()
    is_snapshot = document.get("kind") == "replacement-proof-snapshot"
    fp_matrices: set[object] = set()
    fp_rows = 0
    for row in rows:
        if not isinstance(row, dict) or "state_contract" not in row or "source_closure" not in row:
            raise ValueError("schema2 row is missing state contract/provenance")
        contract, closure = row["state_contract"], row["source_closure"]
        if contract is None:
            if closure is not None:
                raise ValueError("scalar row has mixed vector provenance")
            continue
        validate_contract(contract)
        ignored = row.get("regs_ignored")
        if not isinstance(ignored, list) or any(
            name not in ("eax", "ecx", "edx") for name in ignored
        ):
            raise ValueError("vector proof cannot ignore XMM/MXCSR or unsupported state")
        va = _va(row.get("va"), "vector proof")
        validate_closure(closure, va)
        fp_record = validate_fp_scalar_row(row, closure, va, require_record=is_snapshot)
        if fp_record is not None:
            fp_rows += 1
            fp_matrices.add(fp_record["matrix_sha256"])
        vector_roots.add(va)
        original_pins.add(
            tuple(
                closure[key]
                for key in ("xbe_sha256", "functions_sha256", "generated_manifest_sha256")
            )
        )
        if document.get("kind") == "replacement-proof-snapshot" and closure[
            "xbe_sha256"
        ] != document.get("xbe_sha256"):
            raise ValueError(f"{va}: vector closure differs from snapshot oracle")
        seed = row.get("source_seed")
        if type(seed) is not int or seed < 0:
            raise ValueError(f"{va}: missing source seed")
        if by_label:
            run = by_label.get(row.get("source_run"))
            if (
                run is None
                or run.get("seed") != seed
                or run.get("cases_per_function") != row.get("source_cases_per_function")
            ):
                raise ValueError(f"{va}: inconsistent vector source run/seed/depth")
            if not _equal(run.get("live_call_closure"), closure):
                raise ValueError(f"{va}: vector source closure differs from source run")
        else:
            context = (
                document.get("harness")
                if document.get("kind") == "replacement-proof-snapshot"
                else document
            )
            if (
                not isinstance(context, Mapping)
                or seed != context.get("seed")
                or not _equal(context.get("live_call_closure"), closure)
            ):
                raise ValueError(f"{va}: selected vector provenance differs from run")
    if top_vector and (not isinstance(top, dict) or top.get("root") not in vector_roots):
        raise ValueError("vector run root has no state contract")
    for run in by_label.values():
        closure = run.get("live_call_closure")
        if vector_enabled(closure) and closure.get("root") not in vector_roots:
            raise ValueError("source vector root lost its per-root contract")
    if len(original_pins) > 1:
        raise ValueError("mixed original vector closure provenance")
    if len(fp_matrices) > 1:
        raise ValueError("mixed fp-scalar matrix identity")
    if is_snapshot and (document.get("fp_scalar_schema") == FP_SCALAR_RECORD_VERSION) != bool(
        fp_rows
    ):
        raise ValueError("fp-scalar schema marker does not match the fp-scalar rows")
    if not is_snapshot and "fp_scalar_schema" in document:
        raise ValueError("fp-scalar schema marker belongs to the snapshot only")
    if not vector_roots:
        raise ValueError("schema2 vector contract missing from every root")


def contract_copy() -> dict[str, object]:
    return deepcopy(VECTOR_CONTRACT)


def fp_scalar_contract_copy() -> dict[str, object]:
    return deepcopy(FP_SCALAR_CONTRACT)


def is_fp_scalar_contract(value: object) -> bool:
    return _equal(value, FP_SCALAR_CONTRACT)


def fp_scalar_closure(closure: object) -> bool:
    """True when `closure` records the opt-in fp-scalar-v1 vector comparison."""
    if not isinstance(closure, dict):
        return False
    comparison = closure.get("vector_comparison")
    return isinstance(comparison, dict) and comparison.get("mode") == FP_SCALAR_MODE


def fp_scalar_matrix_identity(root: Path) -> dict[str, str]:
    """Identity (path, sha256) of the tracked matrix result under `root`. Raises `ValueError`."""
    path = root / FP_SCALAR_MATRIX_FILE
    if not path.is_file():
        raise ValueError(f"fp-scalar matrix result {FP_SCALAR_MATRIX_FILE} is missing")
    return {
        "result_file": FP_SCALAR_MATRIX_FILE,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }


def _fp_count(value: object, where: str) -> int:
    if type(value) is not int or value < 0:
        raise ValueError(f"{where}: expected a non-negative integer")
    return value


def fp_scalar_record_fingerprint(record: Mapping[str, object]) -> str:
    """sha256 over every recorded input of the mode except the fingerprint itself."""
    body = {key: value for key, value in record.items() if key != "fingerprint"}
    return hashlib.sha256(json.dumps(body, sort_keys=True).encode("ascii")).hexdigest()


def fp_scalar_record(closure: object, root: str) -> dict[str, object]:
    """The compact fp-scalar record derived from the closure's `fp_scalar` receipt block.

    Fails CLOSED (`ValueError`) when the receipt lacks the mode, the pinned MXCSR control word or
    sticky mask, the NaN-pair count or cap, the matrix identity, or the gate and build verdicts.
    A receipt whose own gate or build record failed is still recorded (flagged False), and the
    replay gate then fails the row; structurally missing or altered fields refuse outright.
    """
    from tools.harness.model import FP_SCALAR_MXCSR, FP_SCALAR_STATUS_MASK
    from tools.harness.vector_scalar import EXCLUSION_CAP

    where = f"{root}: fp-scalar receipt"
    if not fp_scalar_closure(closure):
        raise ValueError(f"{where}: closure does not record the fp-scalar-v1 comparison")
    block = closure.get("fp_scalar")  # type: ignore[union-attr]
    if not isinstance(block, dict):
        raise ValueError(f"{where}: missing fp_scalar receipt block")
    if block.get("mode") != FP_SCALAR_MODE:
        raise ValueError(f"{where}: mode is not {FP_SCALAR_MODE}")
    if block.get("mxcsr_control_word") != f"0x{FP_SCALAR_MXCSR:04x}":
        raise ValueError(f"{where}: pinned MXCSR control word missing or changed")
    if block.get("mxcsr_status_mask") != f"0x{FP_SCALAR_STATUS_MASK:02x}":
        raise ValueError(f"{where}: sticky status mask missing or changed")
    cases = _fp_count(block.get("cases"), f"{where}: cases")
    excluded = _fp_count(block.get("nan_pair_excluded"), f"{where}: nan_pair_excluded")
    if excluded > cases:
        raise ValueError(f"{where}: NaN-pair exclusions exceed the case count")
    cap = block.get("nan_pair_cap")
    if type(cap) not in (int, float) or cap != EXCLUSION_CAP:
        raise ValueError(f"{where}: NaN-pair cap missing or not the pinned {EXCLUSION_CAP}")
    matrix = block.get("matrix")
    if (
        not isinstance(matrix, dict)
        or set(matrix) != {"result_file", "sha256"}
        or matrix["result_file"] != FP_SCALAR_MATRIX_FILE
    ):
        raise ValueError(f"{where}: matrix identity missing")
    _sha(matrix["sha256"], f"{where}: matrix sha256")
    gate, build = block.get("gate"), block.get("build")
    if (
        not isinstance(gate, dict)
        or type(gate.get("passed")) is not bool
        or not isinstance(gate.get("failures"), list)
        or gate["passed"] != (not gate["failures"])
    ):
        raise ValueError(f"{where}: receipt gate missing or inconsistent")
    if not isinstance(build, dict) or type(build.get("passed")) is not bool:
        raise ValueError(f"{where}: receipt build record missing")
    record: dict[str, object] = {
        "mode": FP_SCALAR_MODE,
        "evidence_class": FP_SCALAR_EVIDENCE_CLASS,
        "mxcsr_control_word": block["mxcsr_control_word"],
        "mxcsr_status_mask": block["mxcsr_status_mask"],
        "nan_pair_excluded": excluded,
        "nan_pair_cases": cases,
        "nan_pair_cap": cap,
        "matrix_result_file": matrix["result_file"],
        "matrix_sha256": matrix["sha256"],
        "receipt_gate_passed": gate["passed"],
        "receipt_build_passed": build["passed"],
    }
    record["fingerprint"] = fp_scalar_record_fingerprint(record)
    return record


def fp_scalar_gate_ok(record: Mapping[str, object] | None) -> bool:
    """True only when the receipt gate and build passed and the exclusions are within the cap."""
    if not isinstance(record, Mapping):
        return False
    cases, excluded, cap = (
        record.get("nan_pair_cases"),
        record.get("nan_pair_excluded"),
        record.get("nan_pair_cap"),
    )
    return (
        record.get("receipt_gate_passed") is True
        and record.get("receipt_build_passed") is True
        and type(cases) is int
        and type(excluded) is int
        and type(cap) in (int, float)
        and cases > 0
        and excluded <= cap * cases
    )


def validate_fp_scalar_record(record: object, root: str) -> None:
    """Structural check of a stored record (self-consistency, pinned constants, fingerprint)."""
    if not isinstance(record, dict) or set(record) != FP_SCALAR_RECORD_KEYS:
        raise ValueError(f"{root}: fp-scalar record missing or malformed")
    if record["evidence_class"] != FP_SCALAR_EVIDENCE_CLASS:
        raise ValueError(f"{root}: fp-scalar evidence class changed")
    if record["fingerprint"] != fp_scalar_record_fingerprint(record):
        raise ValueError(f"{root}: fp-scalar record fingerprint does not match its inputs")


def validate_fp_scalar_row(
    row: Mapping[str, object], closure: object, root: str, *, require_record: bool
) -> dict[str, object] | None:
    """Per-row fp-scalar checks of `validate_document`. Returns the derived record (or None).

    An fp closure needs the fp contract, a valid receipt and (snapshot rows) the stored record
    equal to the derived one. A legacy closure must carry no fp block and no fp row fields."""
    is_fp = fp_scalar_closure(closure)
    stored = row.get("fp_scalar")
    stored_class = row.get("evidence_class")
    if not is_fp:
        if _equal(row.get("state_contract"), FP_SCALAR_CONTRACT):
            raise ValueError(f"{root}: legacy vector comparison with an fp-scalar contract")
        if (isinstance(closure, dict) and "fp_scalar" in closure) or stored is not None:
            raise ValueError(f"{root}: fp-scalar evidence on a legacy vector row")
        if stored_class is not None:
            raise ValueError(f"{root}: evidence class on a legacy vector row")
        return None
    if not _equal(row.get("state_contract"), FP_SCALAR_CONTRACT):
        raise ValueError(f"{root}: fp-scalar-v1 row needs the fp-scalar state contract")
    derived = fp_scalar_record(closure, root)
    if stored is None and not require_record:
        return derived
    validate_fp_scalar_record(stored, root)
    if not _equal(stored, derived):
        raise ValueError(f"{root}: stored fp-scalar record differs from its receipt")
    if stored_class != FP_SCALAR_EVIDENCE_CLASS:
        raise ValueError(f"{root}: fp-scalar row lacks the fp-scalar evidence class")
    return derived
