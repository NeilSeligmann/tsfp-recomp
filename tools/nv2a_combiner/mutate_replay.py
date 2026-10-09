# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation check of the PYTHON side of the combiner replay (T75).

The C side is mutation-tested inside `tests/test_gpu_combiner_replay.py` (19 doctored copies of
`src/gpu`). The Python side, `replay_modules.py` (the key, the register reads) and
`glsl.replay_fragment_shader` (the interface), is checked here: each mutation is applied in
place, the replay tests that compare it with the C code and the device run are executed, the file
is restored (always), and a mutation that leaves them green is reported as SURVIVED.

    python -m tools.nv2a_combiner.mutate_replay            # exit 1 when any mutation survives
"""

# ruff: noqa: E501
from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEST = "tests/test_gpu_combiner_replay.py"
SELECT = "not mutated and not unmutated and not 196"


@dataclass(frozen=True)
class Mutation:
    name: str
    file: str
    old: str
    new: str


MODULES = "tools/nv2a_combiner/replay_modules.py"
GLSL = "tools/nv2a_combiner/glsl.py"
MUTATIONS = (
    Mutation("final word 1 low bits keyed", MODULES, "canonical[cfg.IDX_FINAL1] &= FINAL1_KEY_MASK", "pass"),
    Mutation("control word extra bits keyed", MODULES, "canonical[cfg.IDX_CONTROL] &= CONTROL_KEY_MASK", "pass"),
    Mutation("unused stages keep their words", MODULES, "        if stage >= count:", "        if stage >= 9:"),
    Mutation("initial spare0 alpha ignored", MODULES,
             "if stage == 0 and initial_alpha and mode == cfg.TEX_2D_PROJECTIVE:", "if False:"),
    Mutation("the sum register forgets spare0", MODULES,
             "return (1 << _SPARE0) | (1 << _V1) if register == cfg.REG_SUM else 0",
             "return (1 << _V1) if register == cfg.REG_SUM else 0"),
    Mutation("a mux does not read spare0", MODULES,
             "            if half.output.mux:\n                mask |= 1 << _SPARE0",
             "            if half.output.mux:\n                pass"),
    Mutation("texture varying at the wrong location", GLSL,
             'lines.append(f"layout(location = {3 + stage}) in vec4 vs_oT{stage};")',
             'lines.append(f"layout(location = {2 + stage}) in vec4 vs_oT{stage};")'),
    Mutation("sampler at the wrong binding", GLSL,
             "layout(set = 0, binding = {2 + stage}) uniform sampler2D",
             "layout(set = 0, binding = {3 + stage}) uniform sampler2D"),
    Mutation("constants block at binding 0", GLSL,
             'lines.append("layout(set = 0, binding = 1, std140) uniform Nv2aConstants {")',
             'lines.append("layout(set = 0, binding = 0, std140) uniform Nv2aConstants {")'),
)  # fmt: skip


def run_tests() -> tuple[bool, str]:
    done = subprocess.run(
        ["uv", "run", "--offline", "pytest", TEST, "-x", "-q", "-p", "no:cacheprovider", "-k", SELECT],
        cwd=ROOT, capture_output=True, text=True, timeout=1800, check=False,
    )  # fmt: skip
    lines = [line for line in done.stdout.splitlines() if line.strip()]
    return done.returncode != 0, lines[-1] if lines else done.stderr[:200]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Mutation check of the combiner replay Python side."
    )
    parser.add_argument("--only", help="substring of a mutation name")
    args = parser.parse_args(argv)
    survivors = 0
    for mutation in MUTATIONS:
        if args.only and args.only not in mutation.name:
            continue
        path = ROOT / mutation.file
        original = path.read_text()
        if original.count(mutation.old) != 1:
            print(f"ANCHOR DRIFTED {mutation.name}")
            survivors += 1
            continue
        path.write_text(original.replace(mutation.old, mutation.new))
        try:
            killed, summary = run_tests()
        finally:
            path.write_text(original)
        print(f"{'KILLED' if killed else 'SURVIVED'} {mutation.name} | {summary}")
        survivors += 0 if killed else 1
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
