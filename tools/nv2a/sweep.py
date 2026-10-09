# SPDX-License-Identifier: GPL-3.0-or-later
"""Translate every program of a corpus and report counts by outcome and cause.

Reads the gitignored corpus written by `tools.nv2a.corpus`, writes the translated GLSL
under `--out` (default `generated/shaders/translated`, also gitignored: translated
shaders derive from the user's executable) and, with `--compile`, checks each with
`glslangValidator`. Only counts and failure causes are printed or written to the report.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from tools.nv2a import translate

COMPILE_TIMEOUT_SECONDS = 60


def compile_check(path: Path, stage: str) -> str | None:
    """None when glslang compiles `path` to SPIR-V that `spirv-val` accepts, else the error."""
    try:
        completed = subprocess.run(
            [
                "glslangValidator",
                "-V",
                "--target-env",
                "vulkan1.1",
                "-S",
                stage,
                "-o",
                str(path.with_suffix(path.suffix + ".spv")),
                str(path),
            ],
            capture_output=True,
            text=True,
            timeout=COMPILE_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return "timeout"
    if completed.returncode != 0:
        lines = [line for line in completed.stdout.splitlines() if "ERROR" in line]
        return lines[0] if lines else f"exit {completed.returncode}"
    return _spirv_check(path.with_suffix(path.suffix + ".spv"))


def _spirv_check(spirv: Path) -> str | None:
    """None when `spirv-val` accepts the module (or is not installed), else its message."""
    if shutil.which("spirv-val") is None:
        return None
    try:
        completed = subprocess.run(
            ["spirv-val", "--target-env", "vulkan1.1", str(spirv)],
            capture_output=True,
            text=True,
            timeout=COMPILE_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return "spirv-val timeout"
    if completed.returncode == 0:
        return None
    return "spirv-val: " + (completed.stderr.strip().splitlines() or ["rejected"])[0]


def sweep(corpus: Path, out: Path, *, compile_shaders: bool, workers: int) -> dict[str, object]:
    report: dict[str, object] = {}
    for population in ("static", "generated"):
        directory = out / population
        directory.mkdir(parents=True, exist_ok=True)
        outcomes: Counter[str] = Counter()
        opcodes: Counter[str] = Counter()
        written: Counter[int] = Counter()
        compiled: list[tuple[Path, str]] = []
        for path in sorted((corpus / population).glob("*.bin")):
            try:
                result = translate.translate(path.read_bytes(), name=f"p_{path.stem[:12]}")
            except translate.Unsupported as error:
                outcomes[f"unsupported:{error.code}"] += 1
                continue
            outcomes["translated"] += 1
            opcodes.update(result.opcodes)
            for address, mask in result.outputs_written.items():
                written[address] += 1 if mask else 0
            for stage, source, suffix in (
                ("comp", translate.compute_test_source(result), ".comp"),
                ("vert", translate.vertex_shader_source(result), ".vert"),
            ):
                target = directory / f"{path.stem}{suffix}"
                target.write_text(source)
                compiled.append((target, stage))
        entry: dict[str, object] = {
            "outcomes": dict(sorted(outcomes.items())),
            "opcode_occurrences_translated": dict(sorted(opcodes.items())),
            "programs_writing_output_address": {str(k): v for k, v in sorted(written.items())},
        }
        if compile_shaders:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                errors = list(pool.map(lambda item: compile_check(*item), compiled))
            failures = Counter(error for error in errors if error)
            entry["compile"] = {
                "shaders": len(compiled),
                "ok": sum(1 for error in errors if error is None),
                "failed": dict(failures.most_common(10)),
            }
        report[population] = entry
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path("generated/shaders/corpus"))
    parser.add_argument("--out", type=Path, default=Path("generated/shaders/translated"))
    parser.add_argument("--compile", action="store_true", help="also compile with glslang")
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    report = sweep(args.corpus, args.out, compile_shaders=args.compile, workers=args.workers)
    text = json.dumps(report, indent=1)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
