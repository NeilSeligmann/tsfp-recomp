#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One-command full verification (T260).

Runs, each with an explicit per-command timeout and a recorded wall time:

0. ``ruff_lint``: ``python -m ruff check .`` (T265; no counts, pass/fail only).
0b. ``ruff_format`` (T457): ``python -m ruff format --check .``. Advisory by default
   (``FORMAT_ENFORCED_DEFAULT``): an unformatted file prints a note but passes, until the last
   straggler is formatted. ``--format-enforce`` makes it FAIL, ``--no-format-enforce`` advisory.
1. ``lifted_ctest``: configure + build + ctest with the lifted tree (``-DTSFP_LIFTED_DIR``).
   Reported SKIPPED, with the reason, when the lifted directory is absent.
1b. ``shader_original`` (T367): checks the optional retained original shader-compiler chunk
   (``tools.install_shader_original``) against the inputs it was generated from. Not installed
   is SKIPPED with the install command, a stale or broken pair FAILS, and an installed pair
   gets one ``--native-shader-assembler`` boot of the lifted host that must get past
   XGAssembleShader. Read-only: it never installs anything.
2. ``plain_ctest``: configure + build + ctest with no lifted tree.
3. ``gpu_oracle``: the named GPU / oracle pytest files (``GPU_ORACLE_PATTERNS``).
4. ``full_pytest``: ``python -m pytest -q -p no:cacheprovider`` (``--full``, the default;
   ``--quick`` skips it). ``--fast`` runs it with ``-n 12 --dist loadscope`` (the T353
   measured 8x path), logged under the label ``pytest_fast`` so its timings are never
   mistaken for the serial invocation of record.

It prints one summary table with per-step pass / fail / skip counts parsed from the ctest and
pytest output, compares each step against ``tools/verify_baseline.json`` and exits nonzero on
any failure, any pass-count drop versus the baseline, or any skip-count rise. The baseline is
only rewritten deliberately, with ``--update-baseline``.

Why it exists: ctest alone stayed green while ~4,700 pytest tests failed for ~2 h (c926423).
Rule: run this before reporting any merge that touches ``src/``.

All paths are relative to the repository root (this file's grandparent), never hardcoded.
"""

from __future__ import annotations

import argparse
import contextlib
import glob
import importlib.util
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools import install_shader_original as shader_original

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = Path("tools") / "verify_baseline.json"
DEFAULT_LIFTED_DIR = Path("generated") / "lifted" / "gen"
LIFTED_MARKER = "recomp_dispatch.c"

# Named GPU / oracle pytest files, as globs relative to the repository root.
GPU_ORACLE_PATTERNS: tuple[str, ...] = (
    "tests/test_gpu_pgraph*.py",
    "tests/test_gpu_vsh_draw.py",
    "tests/test_nv2a_*device*.py",
    "tests/test_nv2a_*corner*.py",
    "tests/test_nv2a_*viewport*.py",
    "tests/test_oracle_build_sources.py",
    "tests/test_d3d8_real_harness.py",
)

# T353 fast path for the full pytest. Exactly -n 12: -n auto picks 32 workers and drove
# this machine into memory pressure that reaped the run (T353), so it is forbidden here.
FAST_PYTEST_ARGS: tuple[str, ...] = ("-n", "12", "--dist", "loadscope")
FAST_LABEL = "pytest_fast"

STEP_RUFF = "ruff_lint"
STEP_FORMAT = "ruff_format"
# T457: False while tools/harness/cli.py is still unformatted (task/t418 holds pending edits).
# Flip to True in the pass that formats it, so verify_all goes red on any unformatted file.
FORMAT_ENFORCED_DEFAULT = False
STEP_LIFTED = "lifted_ctest"
STEP_SHADER = "shader_original"
STEP_PLAIN = "plain_ctest"
STEP_GPU = "gpu_oracle"
STEP_FULL = "full_pytest"
ALL_STEPS = (STEP_RUFF, STEP_FORMAT, STEP_LIFTED, STEP_SHADER, STEP_PLAIN, STEP_GPU, STEP_FULL)

# T367: the opt-in boot that proves an installed shader chunk links and runs. The first six
# flags are the measured headless set whose boot stops at XGAssembleShader without the chunk.
SHADER_PROBE_FLAGS = (
    "--ac97-ready",
    "--headless-effects",
    "--headless-streams",
    "--headless-buffers",
    "--headless-listener",
    "--headless-first-vblank",
    "--native-shader-assembler",
)
ASSEMBLER_STOP = 0x003EE2B3
ASSEMBLER_STOP_CALLS = 797  # the same flags without the opt-in stop here after this many calls
DEFAULT_XBE_CANDIDATES = (Path("build/default.xbe"), Path("tmp/oxm-extract/retail/default.xbe"))
SURFACE_FILE = ROOT / "src/xbox" / shader_original.SURFACE_PAIR[0]

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"
TIMEOUT = "TIMEOUT"


class ParseError(ValueError):
    """The tool output did not contain a count summary we can trust."""


@dataclass(frozen=True)
class Counts:
    passed: int = 0
    failed: int = 0
    skipped: int = 0

    @property
    def total(self) -> int:
        return self.passed + self.failed + self.skipped


# ---------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------

_CTEST_LINE = re.compile(
    r"Test\s+#\d+:\s+\S+\s+\.+\s*(?:\*\*\*)?(Passed|Failed|Timeout|Skipped|Not Run|Exception)"
)
_CTEST_SUMMARY = re.compile(r"\d+% tests passed, (\d+) tests? failed out of (\d+)")


def parse_ctest(text: str) -> Counts:
    """Count per-test result lines, cross-checked against ctest's own ``out of N`` total.

    ctest's summary line counts a skipped test as passed, so the per-test lines are the
    source of truth for the pass / skip split. Anything that is not Passed or Skipped
    (Failed, Timeout, Exception, Not Run) is a failure.
    """
    statuses = _CTEST_LINE.findall(text)
    summary = _CTEST_SUMMARY.search(text)
    if not statuses:
        raise ParseError("no ctest per-test result lines found")
    if summary is None:
        raise ParseError("no ctest summary line found (ctest killed or never finished)")
    passed = statuses.count("Passed")
    skipped = statuses.count("Skipped")
    failed = len(statuses) - passed - skipped
    if len(statuses) != int(summary.group(2)):
        raise ParseError(
            f"ctest reported {summary.group(2)} tests but {len(statuses)} result lines parsed"
        )
    if failed < int(summary.group(1)):
        raise ParseError("ctest summary reports more failures than result lines")
    return Counts(passed=passed, failed=failed, skipped=skipped)


_PYTEST_TOKEN = re.compile(
    r"(\d+) (passed|failed|skipped|errors?|xfailed|xpassed|warnings?|deselected|rerun)"
)
_PYTEST_FINAL = re.compile(r"^=*\s*((?:\d+ \w+(?:, )?)+) in [\d.]+s(?: \([\d:]+\))?\s*=*\s*$")


def parse_pytest(text: str) -> Counts:
    """Parse the final ``N passed, M skipped in T s`` line of ``pytest -q``.

    ``failed`` folds in collection/setup errors. xfailed, xpassed, warnings and deselected
    are not tests that ran to a verdict and are ignored. The last matching line wins.
    """
    final: str | None = None
    for line in text.splitlines():
        match = _PYTEST_FINAL.match(line.strip())
        if match:
            final = match.group(1)
    if final is None:
        raise ParseError("no pytest summary line found (pytest killed, crashed or ran no tests)")
    tally: dict[str, int] = {}
    for number, word in _PYTEST_TOKEN.findall(final):
        tally[word] = tally.get(word, 0) + int(number)
    passed = tally.get("passed", 0)
    failed = tally.get("failed", 0) + tally.get("error", 0) + tally.get("errors", 0)
    skipped = tally.get("skipped", 0)
    if passed + failed + skipped == 0:
        raise ParseError("pytest summary has no passed/failed/skipped counts")
    return Counts(passed=passed, failed=failed, skipped=skipped)


# ---------------------------------------------------------------------------
# Retained shader compiler (T367)
# ---------------------------------------------------------------------------

_BOOT_TOTAL = re.compile(r"HLE calls reached, in call order \((\d+) recorded, (\d+) total")
_BOOT_STOP = re.compile(r"guest address\s+0x([0-9A-Fa-f]{1,8})\b")


@dataclass(frozen=True)
class BootProbe:
    total: int
    stops: tuple[int, ...]


def parse_boot_probe(text: str) -> BootProbe:
    """Read the HLE call total and the stop addresses off a ``tsfp_host`` run."""
    match = _BOOT_TOTAL.search(text)
    if match is None:
        raise ParseError("no 'HLE calls reached' summary (the host refused or died early)")
    stops = sorted({int(address, 16) for address in _BOOT_STOP.findall(text)})
    return BootProbe(total=int(match.group(2)), stops=tuple(stops))


def evaluate_boot_probe(probe: BootProbe) -> list[str]:
    """Problems that show the opt-in boot did not get past the retained compiler."""
    problems: list[str] = []
    if ASSEMBLER_STOP in probe.stops:
        problems.append(f"opt-in boot still stops at XGAssembleShader {ASSEMBLER_STOP:#010x}")
    if probe.total <= ASSEMBLER_STOP_CALLS:
        problems.append(
            f"opt-in boot made {probe.total} HLE calls, not beyond the {ASSEMBLER_STOP_CALLS} "
            "of the boot without the opt-in"
        )
    return problems


def find_xbe(explicit: Path | None) -> Path | None:
    """The XBE to hash and boot: the given path, else the first conventional location."""
    if explicit is not None:
        return explicit if explicit.is_absolute() else ROOT / explicit
    for candidate in DEFAULT_XBE_CANDIDATES:
        if (ROOT / candidate).is_file():
            return ROOT / candidate
    return None


def shader_original_step(
    name: str,
    *,
    gen_dir: Path,
    xbe: Path | None,
    surface: Path,
    host: Path | None,
    strict: bool,
    timeout: float,
    logs: Path,
    vendor: Path = shader_original.DEFAULT_VENDOR,
    entries: Path = shader_original.DEFAULT_ENTRIES,
) -> StepResult:
    """Static check of the installed chunk, then one opt-in boot when a host and XBE exist."""
    result = StepResult(name)
    check = shader_original.check_installed(
        gen_dir, entries=entries, surface=surface, vendor=vendor, xbe=xbe
    )
    if check.status == shader_original.ABSENT:
        result.note = (
            "SKIPPED: no retained shader compiler installed (optional): "
            "python -m tools.install_shader_original --xbe <default.xbe> --install"
        )
        if strict:
            result.problems.append("shader original not installed under --strict")
            result.status = FAIL
        return result
    if check.status != shader_original.CURRENT:
        result.status = FAIL
        result.problems.extend(check.problems)
        return result
    result.note = f"{check.profile} current" + ("" if xbe else ", XBE hash unchecked")
    if host is None or xbe is None or not host.is_file():
        result.note += ", static only (no lifted host build or XBE to boot)"
        result.status = FAIL if strict else PASS
        if strict:
            result.problems.append("no boot probe under --strict")
        result.clean_run = not strict
        return result
    log = logs / f"{name}.boot.log"
    with tempfile.TemporaryDirectory(prefix="verify_all_hdd_") as hdd:
        command = [host, xbe, "--hdd", hdd, "--trace", "1", *SHADER_PROBE_FLAGS]
        code, wall = run_command(f"{name} boot", command, ROOT, log, timeout)
    result.timings.append(("boot", wall))
    result.wall += wall
    if code is None:
        result.status = TIMEOUT
        result.problems.append("opt-in boot timed out")
        result.log_tail = tail(log)
        return result
    try:
        probe = parse_boot_probe(log.read_text(encoding="utf-8", errors="replace"))
    except ParseError as error:
        result.status = FAIL
        result.problems.append(f"opt-in boot: {error}")
        result.log_tail = tail(log)
        return result
    result.problems.extend(evaluate_boot_probe(probe))
    result.note += f", opt-in boot {probe.total} calls"
    result.status = FAIL if result.problems else PASS
    result.clean_run = not result.problems
    if result.problems:
        result.log_tail = tail(log)
    return result


# ---------------------------------------------------------------------------
# Baseline comparison
# ---------------------------------------------------------------------------


def compare_to_baseline(counts: Counts, baseline: dict[str, int] | None) -> list[str]:
    """Return the problems (empty means clean). Failures always count, baseline or not."""
    problems: list[str] = []
    if counts.failed > 0:
        problems.append(f"{counts.failed} failed")
    if baseline is None:
        return problems
    if counts.passed < baseline["passed"]:
        problems.append(f"pass-count drop {baseline['passed']} -> {counts.passed}")
    if counts.skipped > baseline["skipped"]:
        problems.append(f"skip-count rise {baseline['skipped']} -> {counts.skipped}")
    return problems


def load_baseline(path: Path) -> dict[str, dict[str, int]]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    steps = data.get("steps")
    if not isinstance(steps, dict) or not steps:
        raise SystemExit(f"{path}: baseline has no 'steps' entries")
    return {
        name: {"passed": int(entry["passed"]), "skipped": int(entry["skipped"])}
        for name, entry in steps.items()
    }


def write_baseline(path: Path, steps: dict[str, dict[str, int]]) -> None:
    payload = {
        "comment": (
            "Expected counts for tools/verify_all.py. A step fails on any failure, on passed "
            "below this, or on skipped above this. Rewrite only deliberately with "
            "--update-baseline and explain the change in the commit."
        ),
        "steps": {name: steps[name] for name in sorted(steps)},
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Running commands
# ---------------------------------------------------------------------------


@dataclass
class StepResult:
    name: str
    status: str = SKIP
    counts: Counts | None = None
    wall: float = 0.0
    note: str = ""
    problems: list[str] = field(default_factory=list)
    timings: list[tuple[str, float]] = field(default_factory=list)
    log_tail: str = ""
    clean_run: bool = False  # ran to completion with counts and no failure, ignoring the baseline


def run_command(
    label: str,
    command: Sequence[str | Path],
    cwd: Path,
    log_path: Path,
    timeout: float,
    env: dict[str, str] | None = None,
) -> tuple[int | None, float]:
    """Run to completion or kill the whole process group at the timeout. rc None = timed out."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"$ {' '.join(str(part) for part in command)}\n")
        log.flush()
        proc = subprocess.Popen(
            [str(part) for part in command],
            cwd=cwd,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        try:
            code: int | None = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            code = None
            log.write(f"\n[verify_all] {label} killed after {timeout:.0f}s timeout\n")
    return code, time.monotonic() - started


def tail(path: Path, lines: int = 25) -> str:
    return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])


def ctest_env() -> dict[str, str]:
    """ctest environment. The disc tests look for ``../discs/tsfp-xbox.iso`` relative to the build
    directory, which only holds for a build tree beside the checkout, so point them at it
    explicitly. Without this a temp --build-root silently turns disc tests into skips."""
    env = dict(os.environ)
    iso = ROOT / "discs" / "tsfp-xbox.iso"
    if "TSFP_XBOX_ISO" not in env and iso.exists():
        env["TSFP_XBOX_ISO"] = str(iso)
    return env


def ctest_step(
    name: str,
    build_dir: Path,
    cmake_defs: Sequence[str],
    args: argparse.Namespace,
    logs: Path,
) -> StepResult:
    result = StepResult(name)
    sequence: list[tuple[str, list[str | Path], float]] = [
        (
            "configure",
            [
                "cmake",
                "-S",
                ROOT,
                "-B",
                build_dir,
                f"-DCMAKE_BUILD_TYPE={args.build_type}",
                *cmake_defs,
                *args.cmake_arg,
            ],
            args.timeout_build,
        ),
        ("build", ["cmake", "--build", build_dir, "-j", str(args.jobs)], args.timeout_build),
    ]
    for label, command, timeout in sequence:
        log = logs / f"{name}.{label}.log"
        code, wall = run_command(f"{name} {label}", command, ROOT, log, timeout)
        result.timings.append((label, wall))
        result.wall += wall
        if code != 0:
            result.status = TIMEOUT if code is None else FAIL
            result.problems.append(f"{label} {'timed out' if code is None else f'exit {code}'}")
            result.log_tail = tail(log)
            return result
    log = logs / f"{name}.ctest.log"
    command = ["ctest", "--test-dir", build_dir, "-j", str(args.ctest_jobs), "--output-on-failure"]
    code, wall = run_command(f"{name} ctest", command, ROOT, log, args.timeout_ctest, ctest_env())
    result.timings.append(("ctest", wall))
    result.wall += wall
    return finish_counted(result, code, log, parse_ctest)


def ruff_step(name: str, python: str, timeout: float, logs: Path) -> StepResult:
    """``ruff check .`` over the repository. No counts: PASS on exit 0, FAIL otherwise."""
    result = StepResult(name)
    log = logs / f"{name}.ruff.log"
    command = [python, "-m", "ruff", "check", "."]
    code, wall = run_command(f"{name} ruff", command, ROOT, log, timeout)
    result.timings.append(("ruff", wall))
    result.wall += wall
    if code is None:
        result.status = TIMEOUT
        result.problems.append("timed out")
        result.log_tail = tail(log)
    elif code != 0:
        result.status = FAIL
        result.problems.append(f"exit {code}")
        result.log_tail = tail(log)
    else:
        result.status = PASS
        result.clean_run = True
    return result


def format_step(name: str, python: str, timeout: float, logs: Path, enforce: bool) -> StepResult:
    """``ruff format --check .``. Nonzero: unformatted files, FAIL if enforced else advisory."""
    result = StepResult(name)
    log = logs / f"{name}.ruff.log"
    command = [python, "-m", "ruff", "format", "--check", "."]
    code, wall = run_command(f"{name} ruff", command, ROOT, log, timeout)
    result.timings.append(("ruff", wall))
    result.wall += wall
    if code is None:
        result.status = TIMEOUT
        result.problems.append("timed out")
        result.log_tail = tail(log)
    elif code != 0 and enforce:
        result.status = FAIL
        result.problems.append(f"exit {code}")
        result.log_tail = tail(log)
    elif code != 0:
        result.status = PASS
        result.note = f"ADVISORY: ruff format --check exit {code}, see {log.name}"
    else:
        result.status = PASS
        result.clean_run = True
    return result


def pytest_step(
    name: str,
    python: str,
    extra: Sequence[str],
    timeout: float,
    logs: Path,
    label: str = "pytest",
) -> StepResult:
    result = StepResult(name)
    log = logs / f"{name}.{label}.log"
    command = [python, "-m", "pytest", "-q", "-p", "no:cacheprovider", *extra]
    code, wall = run_command(f"{name} {label}", command, ROOT, log, timeout)
    result.timings.append((label, wall))
    result.wall += wall
    return finish_counted(result, code, log, parse_pytest)


def finish_counted(
    result: StepResult, code: int | None, log: Path, parser: Callable[[str], Counts]
) -> StepResult:
    if code is None:
        result.status = TIMEOUT
        result.problems.append("timed out")
        result.log_tail = tail(log)
        # Counts of a killed run are unreliable; still try to show them.
    try:
        result.counts = parser(log.read_text(encoding="utf-8", errors="replace"))
    except ParseError as error:
        if result.status != TIMEOUT:
            result.status = FAIL
            result.problems.append(f"unparsable output: {error}")
            result.log_tail = tail(log)
        return result
    if result.status == TIMEOUT:
        return result
    if code != 0 and result.counts.failed == 0:
        result.problems.append(f"exit code {code} with no counted failure")
    result.status = PASS
    result.clean_run = result.counts.failed == 0 and not result.problems
    return result


def xdist_available() -> bool:
    """True when pytest-xdist is importable in this interpreter (the project venv)."""
    return importlib.util.find_spec("xdist") is not None


def resolve_gpu_files() -> list[str]:
    """Expand the named patterns relative to the repository root. An empty pattern is an error."""
    files: list[str] = []
    for pattern in GPU_ORACLE_PATTERNS:
        matches = sorted(
            Path(match).relative_to(ROOT).as_posix() for match in glob.glob(str(ROOT / pattern))
        )
        if not matches:
            raise SystemExit(f"GPU/oracle pattern matched no test file: {pattern}")
        files.extend(match for match in matches if match not in files)
    return files


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------


def apply_baseline(result: StepResult, baseline: dict[str, dict[str, int]]) -> None:
    if result.counts is None:
        return
    entry = baseline.get(result.name)
    result.problems.extend(compare_to_baseline(result.counts, entry))
    if entry is None:
        result.note = (result.note + " no baseline").strip()
    elif result.counts.passed > entry["passed"] and not result.problems:
        result.note = (
            result.note + f" passed rose from {entry['passed']}, consider --update-baseline"
        ).strip()
    if result.problems:
        result.status = FAIL if result.status == PASS else result.status


def render_table(results: Sequence[StepResult], baseline: dict[str, dict[str, int]]) -> str:
    header = ("step", "status", "passed", "failed", "skipped", "baseline", "wall", "note")
    rows: list[tuple[str, ...]] = [header]
    for result in results:
        counts = result.counts
        entry = baseline.get(result.name)
        timings = " ".join(f"{label} {seconds:.0f}s" for label, seconds in result.timings)
        note = "; ".join(
            filter(None, [result.note, *result.problems, f"({timings})" if timings else ""])
        )
        rows.append(
            (
                result.name,
                result.status,
                str(counts.passed) if counts else "-",
                str(counts.failed) if counts else "-",
                str(counts.skipped) if counts else "-",
                f"{entry['passed']}p/{entry['skipped']}s" if entry else "-",
                f"{result.wall:.0f}s",
                note,
            )
        )
    widths = [max(len(row[index]) for row in rows) for index in range(len(header) - 1)]
    lines = []
    for row in rows:
        cells = [row[index].ljust(widths[index]) for index in range(len(header) - 1)]
        lines.append("  ".join(cells) + "  " + row[-1])
    lines.insert(1, "  ".join("-" * width for width in widths))
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run lifted ctest, plain ctest, GPU/oracle pytests and the full pytest, "
        "print one table, and fail on any failure or baseline regression.",
    )
    parser.add_argument(
        "--full",
        dest="full",
        action="store_true",
        default=True,
        help="run the full pytest (default)",
    )
    parser.add_argument("--quick", dest="full", action="store_false", help="skip the full pytest")
    parser.add_argument(
        "--fast",
        action="store_true",
        help=f"run the full pytest with '{' '.join(FAST_PYTEST_ARGS)}' (the T353 8x path, "
        f"logged as {FAST_LABEL}). The serial step stays the invocation of record for "
        "timing entries. -n auto is forbidden on this machine (T353). Needs pytest-xdist "
        "and refuses to run without it",
    )
    parser.add_argument("--only", nargs="+", choices=ALL_STEPS, help="run just these steps")
    parser.add_argument(
        "--build-root",
        type=Path,
        help="directory for the build trees and logs (default: a temp dir)",
    )
    parser.add_argument(
        "--keep-build", action="store_true", help="keep a temp --build-root after a green run"
    )
    parser.add_argument(
        "--lifted-dir",
        type=Path,
        default=DEFAULT_LIFTED_DIR,
        help=f"lifted C dir, relative to the repo root or absolute (default {DEFAULT_LIFTED_DIR})",
    )
    parser.add_argument(
        "--xbe",
        type=Path,
        help="the extracted default.xbe for the shader_original step (default: build/default.xbe,"
        " then tmp/oxm-extract/retail/default.xbe)",
    )
    parser.add_argument(
        "--host",
        type=Path,
        help="tsfp_host for the shader_original boot probe (default: the lifted_ctest build)",
    )
    parser.add_argument(
        "--timeout-shader", type=float, default=300, help="seconds for the shader_original boot"
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        default=DEFAULT_BASELINE,
        help="baseline JSON (relative to the repo root)",
    )
    parser.add_argument(
        "--update-baseline", action="store_true", help="rewrite the baseline from this run's counts"
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="treat a SKIPPED lifted or shader_original step, or a missing generated "
        "src/xbox surface file, as a failure",
    )
    parser.add_argument(
        "--build-type", default="Release", help="CMAKE_BUILD_TYPE (default Release)"
    )
    parser.add_argument(
        "--cmake-arg", action="append", default=[], help="extra cmake argument, repeatable"
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="interpreter for the pytest steps (default: this one)",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="build parallelism")
    parser.add_argument("--ctest-jobs", type=int, default=1, help="ctest parallelism (default 1)")
    parser.add_argument(
        "--timeout-build", type=float, default=3600, help="seconds per configure/build command"
    )
    parser.add_argument("--timeout-ctest", type=float, default=1800, help="seconds per ctest run")
    parser.add_argument(
        "--timeout-ruff", type=float, default=300, help="seconds for the ruff lint step"
    )
    parser.add_argument(
        "--timeout-format", type=float, default=300, help="seconds for the ruff format step"
    )
    parser.add_argument(
        "--format-enforce",
        action=argparse.BooleanOptionalAction,
        default=FORMAT_ENFORCED_DEFAULT,
        help="ruff_format FAILS on unformatted files (default: FORMAT_ENFORCED_DEFAULT)",
    )
    parser.add_argument(
        "--timeout-gpu", type=float, default=1800, help="seconds for the GPU/oracle pytests"
    )
    parser.add_argument(
        "--timeout-full", type=float, default=3600, help="seconds for the full pytest"
    )
    return parser


def surface_preflight(steps: Sequence[str]) -> list[Path]:
    """Name the gitignored generated surface files a fresh worktree lacks (T367).

    Without them the plain and lifted builds silently drop the tests keyed by the surface,
    and the only symptom is a pass-count drop with no reason. Printed, not fatal here: the
    baseline comparison fails the run, and --strict fails it directly.
    """
    if not {STEP_LIFTED, STEP_SHADER, STEP_PLAIN} & set(steps):
        return []
    missing = shader_original.missing_surface_inputs(ROOT / "src/xbox")
    if missing:
        names = ", ".join(path.name for path in missing)
        print(
            f"[verify_all] WARNING: generated src/xbox input(s) absent: {names}. Tests keyed by "
            "them are not built. Generate them with: python -m tools.install_shader_original "
            "--xbe <default.xbe> --xtlid <xtlid.xml> (or see docs/shader-original-routes.md)",
            flush=True,
        )
    return missing


def selected_steps(args: argparse.Namespace) -> list[str]:
    steps = list(args.only) if args.only else list(ALL_STEPS)
    if not args.full and STEP_FULL in steps and not args.only:
        steps.remove(STEP_FULL)
    return steps


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.fast and not xdist_available():
        # Never fall back to serial silently: a serial run misrecorded as fast would be
        # trusted as the 8x path. Refuse up front, naming the missing dependency.
        raise SystemExit(
            "--fast requires pytest-xdist, which is not importable in this venv "
            "(install it, e.g. 'uv add --dev pytest-xdist'); refusing to fall back to serial"
        )
    baseline_path = args.baseline if args.baseline.is_absolute() else ROOT / args.baseline
    baseline = load_baseline(baseline_path)
    steps = selected_steps(args)
    temp_root = args.build_root is None
    build_root = (args.build_root or Path(tempfile.mkdtemp(prefix="verify_all_"))).resolve()
    build_root.mkdir(parents=True, exist_ok=True)
    logs = build_root / "logs"
    lifted = args.lifted_dir if args.lifted_dir.is_absolute() else ROOT / args.lifted_dir
    results: list[StepResult] = []

    missing_surface = surface_preflight(steps)
    for name in steps:
        if name == STEP_RUFF:
            result = ruff_step(name, args.python, args.timeout_ruff, logs)
        elif name == STEP_FORMAT:
            result = format_step(name, args.python, args.timeout_format, logs, args.format_enforce)
        elif name == STEP_LIFTED:
            if (lifted / LIFTED_MARKER).is_file():
                result = ctest_step(
                    name, build_root / "lifted", [f"-DTSFP_LIFTED_DIR={lifted}"], args, logs
                )
            else:
                result = StepResult(name, SKIP, note=f"SKIPPED: no {LIFTED_MARKER} in {lifted}")
                if args.strict:
                    result.problems.append("lifted step skipped under --strict")
                    result.status = FAIL
        elif name == STEP_SHADER:
            host = args.host or build_root / "lifted" / "tsfp_host"
            result = shader_original_step(
                name,
                gen_dir=lifted,
                xbe=find_xbe(args.xbe),
                surface=SURFACE_FILE,
                host=host if host.is_absolute() else ROOT / host,
                strict=args.strict,
                timeout=args.timeout_shader,
                logs=logs,
            )
        elif name == STEP_PLAIN:
            # Point at a directory that cannot hold lifted code, whatever the cache default is.
            result = ctest_step(
                name,
                build_root / "plain",
                [f"-DTSFP_LIFTED_DIR={build_root / 'no-lifted'}"],
                args,
                logs,
            )
        elif name == STEP_GPU:
            result = pytest_step(name, args.python, resolve_gpu_files(), args.timeout_gpu, logs)
        elif args.fast:
            result = pytest_step(
                name, args.python, list(FAST_PYTEST_ARGS), args.timeout_full, logs, FAST_LABEL
            )
        else:
            result = pytest_step(name, args.python, [], args.timeout_full, logs)
        apply_baseline(result, baseline)
        results.append(result)
        print(f"[verify_all] {name}: {result.status} in {result.wall:.0f}s", flush=True)

    print()
    print(render_table(results, baseline))
    failed = [result for result in results if result.status in (FAIL, TIMEOUT)]
    for result in failed:
        if result.log_tail:
            print(f"\n--- last log lines of {result.name} ---\n{result.log_tail}")

    if args.update_baseline:
        updated = dict(baseline)
        for result in results:
            if result.clean_run and result.counts is not None:
                updated[result.name] = {
                    "passed": result.counts.passed,
                    "skipped": result.counts.skipped,
                }
        refused = [
            result.name for result in results if result.status != SKIP and not result.clean_run
        ]
        if refused:
            print(
                f"\n--update-baseline refused: {', '.join(refused)} did not run clean; "
                "baseline untouched"
            )
        else:
            write_baseline(baseline_path, updated)
            print(f"\nbaseline rewritten: {baseline_path}")
            return 0

    if missing_surface and args.strict:
        failed.append(StepResult("surface_preflight", FAIL))
    if failed:
        print(
            f"\nVERIFY: FAILED ({', '.join(result.name for result in failed)}); "
            f"build root kept at {build_root}"
        )
        return 1
    if temp_root and not args.keep_build:
        shutil.rmtree(build_root, ignore_errors=True)
    print("\nVERIFY: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
