# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation-test the format 0x24 comparisons (T253, T472): does the suite notice each defect?

Each mutation edits ONE expression in the model or in the test harness, runs
`tests/test_xmv_format24_original.py` (or with `--suite lifted` the lifted-code comparison
`tests/test_xmv_format24_lifted.py` and its C runner) in a fresh interpreter and restores the file.
A mutation the suite passes is a hole. Every `__pycache__` under tools/xmvscan and tests is deleted
before each run and bytecode writing is off, because a same-size edit within one second can
otherwise reuse a stale `.pyc` and report a mutation killed (or surviving) by old code.

    python -m tools.xmvscan.mutate_format24
    python -m tools.xmvscan.mutate_format24 --suite lifted
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODEL = "tools/xmvscan/format24.py"
SUITE = "tests/test_xmv_format24_original.py"
LIFTED_SUITE = "tests/test_xmv_format24_lifted.py"
RUNNER = "tests/c/xmv_format24_lifted_runner.c"


@dataclass(frozen=True)
class Mutation:
    name: str
    file: str
    old: str
    new: str


MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        "model: U and V swapped",
        MODEL,
        "source = u_plane if column % 2 == 0 else v_plane",
        "source = v_plane if column % 2 == 0 else u_plane",
    ),
    Mutation(
        "model: chroma row rounds up",
        MODEL,
        "chroma = (row // 2) * chroma_stride",
        "chroma = ((row + 1) // 2) * chroma_stride",
    ),
    Mutation(
        "model: chroma column rounds up",
        MODEL,
        "source[chroma + column // 2]",
        "source[min(chroma + (column + 1) // 2, len(source) - 1)]",
    ),
    Mutation(
        "model: Y and chroma bytes swapped",
        MODEL,
        "dest[target + 2 * column] = y_plane[luma + column]",
        "dest[target + 2 * column + 1] = y_plane[luma + column]",
    ),
    Mutation(
        "model: luma sample one column right",
        MODEL,
        "y_plane[luma + column]",
        "y_plane[min(luma + column + 1, len(y_plane) - 1)]",
    ),
    Mutation(
        "model: luma stride one too large",
        MODEL,
        "luma = row * luma_stride",
        "luma = row * (luma_stride + 1)",
    ),
    Mutation(
        "model: writes one extra pixel per row",
        MODEL,
        "for column in range(width):",
        "for column in range(min(width + 1, luma_stride)):",
    ),
    Mutation(
        "model: skips the last row",
        MODEL,
        "for row in range(height):",
        "for row in range(height - 1):",
    ),
    Mutation(
        "model: accepts width past the macroblocks",
        MODEL,
        "<= width <= 16 * mb_cols",
        "<= width <= 16 * mb_cols + 1",
    ),
    Mutation(
        "model: accepts a pitch under one row",
        MODEL,
        "if pitch < 2 * width:",
        "if pitch < width:",
    ),
    Mutation(
        "model: chroma plane shorter than the macroblocks",
        MODEL,
        "return 16 * mb_cols, 16 * mb_rows, 8 * mb_cols, 8 * mb_rows",
        "return 16 * mb_cols, 16 * mb_rows, 8 * mb_cols, 8 * mb_rows + 1",
    ),
    Mutation(
        "harness: U and V plane addresses swapped",
        SUITE,
        "[mb_cols, mb_rows, Y_BASE, U_BASE, V_BASE, width,",
        "[mb_cols, mb_rows, Y_BASE, V_BASE, U_BASE, width,",
    ),
    Mutation(
        "harness: width argument one too large",
        SUITE,
        "Y_BASE, U_BASE, V_BASE, width, height, self.dest, pitch]",
        "Y_BASE, U_BASE, V_BASE, width + 1, height, self.dest, pitch]",
    ),
    Mutation(
        "harness: footprint check accepts the padding",
        SUITE,
        'footprint[row * pitch : row * pitch + 2 * width] = b"\\x01" * (2 * width)',
        'footprint[row * pitch : row * pitch + 32 * mb_cols] = b"\\x01" * (32 * mb_cols)',
    ),
    Mutation(
        "harness: stray writes not asserted",
        SUITE,
        "assert not run.stray_writes, ",
        "assert True, ",
    ),
    Mutation(
        "harness: stray reads not asserted",
        SUITE,
        "assert not run.stray_reads, ",
        "assert True, ",
    ),
    Mutation(
        "harness: full plane read not required",
        SUITE,
        "assert all(touched), ",
        "assert any(touched), ",
    ),
    Mutation(
        "harness: stack pop not checked",
        SUITE,
        "assert run.esp == run.stack + 4 * 10, ",
        "assert run.esp != 0, ",
    ),
    Mutation(
        "harness: callee-saved registers not checked",
        SUITE,
        "assert run.ebp == 0x2468ACE0 and run.restored == run.saved, ",
        "assert True, ",
    ),
    Mutation(
        "harness: write hook ignores the destination",
        SUITE,
        "if self.dest <= address and address + size <= self.dest + self.size:",
        "if False and address + size <= self.dest + self.size:",
    ),
)


# Edits to the lifted-code comparison itself. Model mutations that change converted bytes
# are shared: the lifted suite compares against the same model.
LIFTED_MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        "lifted harness: stack pop not checked",
        LIFTED_SUITE,
        "assert run.returned == 4 * 10, ",
        "assert run.returned != 0, ",
    ),
    Mutation(
        "lifted harness: callee-saved registers not checked",
        LIFTED_SUITE,
        "assert run.saved == (0x13579BDF,",
        "assert len(run.saved) == 4 or (0x13579BDF,",
    ),
    Mutation(
        "lifted harness: stray writes not asserted",
        LIFTED_SUITE,
        "assert run.stray == (0, 0, 0), ",
        "assert True, ",
    ),
    Mutation(
        "lifted harness: write footprint not asserted",
        LIFTED_SUITE,
        "assert run.footprint == bytes(footprint), ",
        "assert True, ",
    ),
    Mutation(
        "lifted harness: output not compared with the model",
        LIFTED_SUITE,
        "assert output == bytes(expected), ",
        "assert True, ",
    ),
    Mutation(
        "lifted harness: both runs use the same fill",
        LIFTED_SUITE,
        "FILLS = (0xA7, 0x58)",
        "FILLS = (0xA7, 0xA7)",
    ),
    Mutation(
        "lifted harness: model sees the second fill as the first",
        LIFTED_SUITE,
        "expected = bytearray([fill]) * (pitch * height)",
        "expected = bytearray([FILLS[0]]) * (pitch * height)",
    ),
    Mutation(
        "lifted harness: footprint covers the padding",
        LIFTED_SUITE,
        'footprint[row * pitch : row * pitch + 2 * width] = b"\\x01" * (2 * width)',
        'footprint[row * pitch : row * pitch + 32 * mb_cols] = b"\\x01" * (32 * mb_cols)',
    ),
    Mutation(
        "lifted harness: crash is not a failure",
        LIFTED_SUITE,
        "assert process.returncode == 0, (",
        "assert process.returncode is not None, (",
    ),
    Mutation(
        "lifted runner: U and V plane addresses swapped",
        RUNNER,
        "mb_rows, y.start, u.start, v.start, width",
        "mb_rows, y.start, v.start, u.start, width",
    ),
    Mutation(
        "lifted runner: width argument one too large",
        RUNNER,
        "v.start, width, height, dest.start, pitch}",
        "v.start, width + 1, height, dest.start, pitch}",
    ),
    Mutation(
        "lifted runner: planes do not end before a guard page",
        RUNNER,
        "Region y = region(0x20200000u - y_size, y_size);",
        "Region y = region(0x20100000u, y_size);",
    ),
    Mutation(
        "lifted runner: poison not checked",
        RUNNER,
        "reply.poison_bad += count_changed(buffers[index], NULL, fill);",
        "reply.poison_bad += 0;",
    ),
    Mutation(
        "lifted runner: planes not checked",
        RUNNER,
        "reply.planes_bad += host_of(y.start)[at] != y_in[at];",
        "reply.planes_bad += 0;",
    ),
    Mutation(
        "lifted runner: stack not checked",
        RUNNER,
        "reply.stack_bad += *host_of(at) != stack_before[at - stack.start];",
        "reply.stack_bad += 0;",
    ),
    Mutation(
        "lifted runner: esp reported as the expected pop",
        RUNNER,
        "Reply reply = {g_esp - esp0,",
        "Reply reply = {40u,",
    ),
    Mutation(
        "lifted runner: ebx reported as the seeded value",
        RUNNER,
        "g_esp - esp0, g_ebx,",
        "g_esp - esp0, 0x13579BDFu,",
    ),
    Mutation(
        "lifted runner: ebp reported as the seeded value",
        RUNNER,
        "g_edi, g_ebp, 0, 0, 0};",
        "g_edi, EBP_VALUE, 0, 0, 0};",
    ),
    Mutation(
        "lifted runner: destination returned before the call",
        RUNNER,
        "sub_00445AAF();\n",
        "",
    ),
    Mutation(
        "lifted runner: a different fill for the destination pages",
        RUNNER,
        "map_region(all[index].start, all[index].start + all[index].size, fill);",
        "map_region(all[index].start, all[index].start + all[index].size, 0);",
    ),
    Mutation(
        "lifted runner: stack frame allowance huge",
        RUNNER,
        "#define FRAME_ALLOWANCE 0x280u",
        "#define FRAME_ALLOWANCE 0xFF00u",
    ),
)


def clear_bytecode() -> None:
    for folder in (ROOT / "tools", ROOT / "tests"):
        for cache in folder.rglob("__pycache__"):
            shutil.rmtree(cache, ignore_errors=True)


def suite_outcome(suite: str) -> str:
    """`passed`, `failed` (an assertion caught the mutant) or `broken` (a syntax or build error)."""
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    command = [sys.executable, "-m", "pytest", suite, "-x", "-q", "-p", "no:cacheprovider"]
    result = subprocess.run(
        command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=900
    )
    if result.returncode == 0:
        return "passed"
    summary = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
    return "failed" if " failed" in summary and " error" not in summary else "broken"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", help="run the mutations whose name contains this text")
    parser.add_argument(
        "--suite",
        choices=("original", "lifted"),
        default="original",
        help="original: T253 original-bytes comparison, lifted: T472 lifted-code comparison",
    )
    arguments = parser.parse_args(argv)
    suite = LIFTED_SUITE if arguments.suite == "lifted" else SUITE
    mutations = (
        MUTATIONS
        if arguments.suite == "original"
        else tuple(m for m in MUTATIONS if m.file == MODEL and "accepts" not in m.name)
        + LIFTED_MUTATIONS
    )
    clear_bytecode()
    if suite_outcome(suite) != "passed":
        print("the unmutated suite fails, nothing to measure")
        return 2
    survivors = []
    selected = [m for m in mutations if not arguments.only or arguments.only in m.name]
    for mutation in selected:
        path = ROOT / mutation.file
        original = path.read_text()
        if original.count(mutation.old) != 1:
            print(f"ANCHOR-DRIFT {mutation.name}: {original.count(mutation.old)} matches")
            survivors.append(mutation.name)
            continue
        try:
            path.write_text(original.replace(mutation.old, mutation.new))
            clear_bytecode()
            outcome = suite_outcome(suite)
        finally:
            path.write_text(original)
            clear_bytecode()
        label = {"failed": "killed  ", "passed": "SURVIVED", "broken": "BROKEN  "}[outcome]
        print(f"{label} {mutation.name}")
        if outcome != "failed":
            survivors.append(mutation.name)
    print(f"{len(selected) - len(survivors)} of {len(selected)} killed")
    return 1 if survivors else 0


if __name__ == "__main__":
    raise SystemExit(main())
