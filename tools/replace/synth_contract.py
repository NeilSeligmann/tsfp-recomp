"""Synthesized-domain receipts and predeclared artifact identities (T1780)."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

from tools.harness.synth_domain import EVIDENCE_CLASS, canonical

KNOWN_VERSIONS = ("t1773-synth-v1", "t1773-synth-v2")


def artifact_pin(path: Path, va: str | int, *, committed: bool = False) -> str:
    if committed:
        root = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True
        )
        if root.returncode or not path.resolve().is_relative_to(
            Path(root.stdout.strip()).resolve()
        ):
            raise ValueError(f"{path}: synth artifact must be tracked inside the repository")
        result = subprocess.run(
            ["git", "ls-files", "--error-unmatch", "--", str(path)], capture_output=True
        )
        if result.returncode:
            raise ValueError(f"{path}: synth artifact must be tracked")
        result = subprocess.run(["git", "diff", "HEAD", "--", str(path)], capture_output=True)
        if result.returncode or result.stdout:
            raise ValueError(f"{path}: synth artifact must match committed content")
    try:
        artifact = json.loads(path.read_text())
        claimed = artifact.pop("sha256")
        actual = hashlib.sha256(canonical(artifact)).hexdigest()
        expected_va = int(va, 16) if isinstance(va, str) else va
        if int(artifact["va"], 16) != expected_va or claimed != actual:
            raise ValueError("artifact hash or root mismatch")
        if (
            artifact["generator_version"] not in KNOWN_VERSIONS
            or artifact["evidence_class"] != EVIDENCE_CLASS
        ):
            raise ValueError("unknown synth generator or evidence class")
        return actual
    except (OSError, KeyError, TypeError, ValueError) as error:
        raise ValueError(f"{path}: invalid synth artifact: {error}") from error


def validate_block(
    block: object, va: str | int, *, pin: str | None = None, check_artifact: bool = True
) -> None:
    if not isinstance(block, dict):
        raise ValueError(f"{va}: missing synthesized-domain block")
    if (
        block.get("evidence_class") != EVIDENCE_CLASS
        or block.get("generator_version") not in KNOWN_VERSIONS
    ):
        raise ValueError(f"{va}: unknown synth generator or evidence class")
    for key in ("domain_sha256", "case_stream_sha256", "executed_stream_sha256"):
        if not isinstance(block.get(key), str) or not re.fullmatch("[0-9a-f]{64}", block[key]):
            raise ValueError(f"{va}: invalid synth {key}")
    if block["case_stream_sha256"] != block["executed_stream_sha256"]:
        raise ValueError(f"{va}: executed synth stream differs from regenerated stream")
    if pin is not None and block["domain_sha256"] != pin:
        raise ValueError(f"{va}: synth artifact pin mismatch")
    if check_artifact:
        path = block.get("artifact")
        if not isinstance(path, str) or artifact_pin(Path(path), va) != block["domain_sha256"]:
            raise ValueError(f"{va}: missing or mismatched synth artifact")
        if json.loads(Path(path).read_text())["generator_version"] != block["generator_version"]:
            raise ValueError(f"{va}: synth artifact generator differs from receipt")


def validate_document(document: dict) -> None:
    context = document.get("harness", document)
    runs = {run["label"]: run for run in context.get("merged_runs", [])}
    for row in document["functions"]:
        if "--only-va" in str(row.get("unjudgeable") or ""):
            continue
        run = runs.get(row.get("source_run"), context)
        expected = run.get("synth_domain") is True or "synth-domain" in (
            row.get("fixture_provider_selection") or run.get("fixture_provider_selection") or []
        )
        block = row.get("synth_domain")
        if expected or block is not None:
            recipe = row.get("recipe") or {}
            path = recipe.get("synth_domain_pin")
            pin = None
            if path is not None:
                if not isinstance(path, str):
                    raise ValueError(f"{row['va']}: synth_domain_pin must be an artifact path")
                pin = artifact_pin(Path(path), row["va"], committed=True)
            validate_block(block, row["va"], pin=pin)
