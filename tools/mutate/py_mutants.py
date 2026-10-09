# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the `PYTHON_MUTATIONS` of a mutation set (T560).

`c_suites.py` rebuilds a ctest binary per mutation and calls a mutation that cannot change one
INVALID, so mutations of Python sources have no way through it. This applies each one to the file in
place, runs the pytest targets named by the mutation and counts a failing run (exit status 1) as a
kill. The file is restored in a `finally` and its `__pycache__` removed after every write, because
CPython trusts a cached bytecode of the same size and second. Run it in a clean worktree: a dirty
target file is refused.

    python tools/mutate/py_mutants.py --set gpu_window_clip
"""

from __future__ import annotations

import argparse
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

SETS_DIR = Path(__file__).parent / "sets"
PYTEST_PREFIX = "pytest:"


def load_python_mutations(name: str) -> list[dict]:
    path = SETS_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"_pymut_{name}", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"{path}: cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    found = getattr(module, "PYTHON_MUTATIONS", None)
    if not found:
        raise SystemExit(f"{path}: defines no PYTHON_MUTATIONS")
    return list(found)


def verdict(mutation: dict, pytest_filter: str | None, timeout: int) -> str:
    for target in mutation["targets"]:
        arguments = target.removeprefix(PYTEST_PREFIX).split()
        extra = ["-k", pytest_filter] if pytest_filter else []
        done = subprocess.run(  # noqa: S603
            [
                sys.executable,
                "-m",
                "pytest",
                "-q",
                "-x",
                "-p",
                "no:cacheprovider",
                *arguments,
                *extra,
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        if done.returncode == 1:
            return f"KILLED by {arguments[0]}"
    return "SURVIVED"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--set", required=True, help="mutation set module name, e.g. gpu_window_clip"
    )
    parser.add_argument("--only", help="run a single mutation by id")
    parser.add_argument("--pytest-filter", help="pytest -k expression added to every target")
    parser.add_argument("--timeout", type=int, default=900, help="seconds per pytest target")
    args = parser.parse_args(argv)
    mutations = load_python_mutations(args.set)
    if args.only:
        mutations = [m for m in mutations if m["id"] == args.only]
    if not mutations:
        raise SystemExit("no mutation selected")
    dirty = subprocess.run(  # noqa: S603
        ["git", "status", "--porcelain", "--", *sorted({m["file"] for m in mutations})],  # noqa: S607
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    ).stdout.strip()
    if dirty:
        raise SystemExit(f"refusing to start, a target file is dirty:\n{dirty}")
    killed = 0
    for mutation in mutations:
        path = Path(mutation["file"])
        original = path.read_bytes()
        text = original.decode()
        if text.count(mutation["old"]) != 1:
            print(f"{mutation['id']} ANCHOR-DRIFT")
            continue
        try:
            path.write_text(text.replace(mutation["old"], mutation["new"], 1))
            shutil.rmtree(path.parent / "__pycache__", ignore_errors=True)
            result = verdict(mutation, args.pytest_filter, args.timeout)
        finally:
            path.write_bytes(original)
            shutil.rmtree(path.parent / "__pycache__", ignore_errors=True)
        killed += result.startswith("KILLED")
        print(f"{mutation['id']} {result}", flush=True)
    print(f"{killed} of {len(mutations)} killed")
    return 0 if killed == len(mutations) else 1


if __name__ == "__main__":
    raise SystemExit(main())
