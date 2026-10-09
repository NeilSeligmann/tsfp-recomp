# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and run the T1479 appendix native controls standalone (no proof tuple).

Each control file `tests/c/t1479_appendix/<va>.inc` defines
`static void test_t1479_appendix_<va>(void)` against the prepare/invoke/CHECK interface of
`standalone.h`. The source under test is a frozen draft (`.c.txt`) or a production `.c`.
Builds at O0/O2/O3 with the strict warning set and runs each binary.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(".")


def build_and_run(
    sources: list[Path], controls: list[str], opt: int, workdir: Path, extra: list[str]
) -> tuple[bool, str]:
    workdir.mkdir(parents=True, exist_ok=True)
    runner = workdir / f"runner_o{opt}.c"
    lines = ['#include "t1479_appendix/standalone.h"']
    lines += [f'#include "t1479_appendix/{va}.inc"' for va in controls]
    lines += ["int main(void)", "{", "    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;"]
    lines += [f"    test_t1479_appendix_{va}();" for va in controls]
    lines += ['    printf("%u checks %u failures\\n", checks, failures);']
    lines += ["    return failures ? 1 : 0;", "}"]
    runner.write_text("\n".join(lines) + "\n")
    binary = workdir / f"runner_o{opt}"
    command = [
        "cc",
        "-std=c11",
        f"-O{opt}",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Isrc/game",
        "-Itests/c",
    ]
    command += extra + [str(runner)]
    for source in sources:
        command += ["-x", "c", str(source)]
    command += ["-x", "c", "src/game/game_registry.c", "-o", str(binary)]
    build = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if build.returncode:
        return False, build.stderr[-3000:]
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    return run.returncode == 0, (run.stdout + run.stderr)[-3000:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", required=True, type=Path)
    parser.add_argument("--control", action="append", required=True, help="VA hex digits, no 0x")
    parser.add_argument("--opt", action="append", type=int, default=None)
    parser.add_argument("--workdir", type=Path, default=Path("tmp/appendix-native"))
    parser.add_argument("--extra", action="append", default=[], help="extra cc argument")
    args = parser.parse_args()
    ok = True
    for opt in args.opt or [0, 2, 3]:
        passed, text = build_and_run(args.source, args.control, opt, args.workdir, args.extra)
        lines = text.strip().splitlines()
        print(f"O{opt}: {'PASS' if passed else 'FAIL'} {lines[-1] if lines else ''}")
        if not passed:
            print(text)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
