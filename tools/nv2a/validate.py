# SPDX-License-Identifier: GPL-3.0-or-later
"""Run translated NV2A vertex programs on a software Vulkan device and compare them with
the reference interpreter, so a translator bug shows up as a mismatch.

Path of one program: translate to a GLSL compute shader, compile it with glslang, run it
over a set of generated vectors through `vkrun` (lavapipe, one process for a whole batch),
run the interpreter on the SAME float32 vectors, compare per float.

Compute shader layout (the translator emits it, this module runs it): local_size_x 64,
push constant `uint count`, binding 0 readonly float `data[]` with input record
`gid * 832` (v0..v15 as 64 floats then c0..c191 as 768 floats), binding 1 writeonly float
`result[]` with output record `gid * 64` (o0..o15 as 16 x 4 floats).

Tolerance (`RTOL`, `ATOL`, the denormal rule) is a fixed documented constant set, see
`compare`. A mismatch is never dropped: the report keeps exact counts per category and
status, and only the stored example records are capped.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_SOURCE = Path(__file__).resolve().with_name("vkrun.c")
#: The entry file plus the core shared with tools/nv2a_combiner/vkeval.c. The header
#: is listed so a change to the profile struct also triggers a rebuild.
RUNNER_SOURCES = (
    RUNNER_SOURCE,
    RUNNER_SOURCE.with_name("vkrun_core.c"),
    RUNNER_SOURCE.with_name("vkrun_core.h"),
)
DEFAULT_BIN_DIR = REPO_ROOT / "generated" / "shaders" / "bin"
DEFAULT_WORK_DIR = REPO_ROOT / "generated" / "shaders" / "validate"

INPUT_REGISTERS = 16
CONSTANT_REGISTERS = 192
OUTPUT_REGISTERS = 16
IN_FLOATS = (INPUT_REGISTERS + CONSTANT_REGISTERS) * 4  # 832
OUT_FLOATS = OUTPUT_REGISTERS * 4  # 64
CONSTANT_LIMIT = CONSTANT_REGISTERS - 1

# Comparison tolerance. The Vulkan spec lets rcp, rsq, exp2, log2 and pow differ from
# the exact result by a few ulp (2 to 3 ulp, plus a small absolute term near 1 for
# exp2/log2), a fused multiply-add may skip one rounding, and LIT chains two of these.
# 2^-16 is 128 ulp of float32 (2^-23). It was 2^-18 first, and a MEASURED random program
# (an EXP/LIT chain near overflow) differed from the exact reference by 5.3e-6, which
# the NV spec allows (EXP and LOG z need only 2^-11). It still catches a wrong operand,
# sign, swizzle or opcode, whose error is O(1) relative. The
# absolute term 1e-6 covers results that cancel to near zero from O(1) operands, where
# relative error is meaningless (one rounding of an O(4) product is about 5e-7).
# Operands near 1e30 that cancel are NOT covered, by design: that is reported.
RTOL = 2.0**-16
ATOL = 1e-6
# Below the smallest normal float32. Whether the device flushes denormals is not
# specified (lavapipe reports neither flush nor preserve), so both sides count as zero.
DENORMAL_LIMIT = 2.0**-126

KIND_ORDINARY = "ordinary"
KIND_MATRIX = "matrix"
KIND_CORNER = "corner"
KIND_RELATIVE_EDGES = "relative_edges"
KINDS = (KIND_ORDINARY, KIND_MATRIX, KIND_CORNER, KIND_RELATIVE_EDGES)

CATEGORY_ARITHMETIC = "arithmetic"
CATEGORY_SPECIAL = "special"

DEFAULT_MAX_RECORDS = 20
DEFAULT_CHUNK = 64


class ValidateError(Exception):
    """A harness-level problem (as opposed to a program mismatch)."""


class RunnerUnavailable(ValidateError):
    """No usable vkrun, glslang or lavapipe on this machine."""


class CompileError(ValidateError):
    """glslang rejected a shader."""


class RunnerError(ValidateError):
    """The runner failed a job."""


# --------------------------------------------------------------------------- float32


def round_f32(value: float) -> float:
    """Round to float32 and back. ctypes casts in C, so overflow gives inf, no exception."""
    return ctypes.c_float(value).value


def f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", round_f32(value)))[0]


def _from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


SMALLEST_NORMAL = _from_bits(0x00800000)
LARGEST_DENORMAL = _from_bits(0x007FFFFF)
SMALLEST_DENORMAL = _from_bits(0x00000001)
LARGEST_FINITE = _from_bits(0x7F7FFFFF)

#: Values drawn by the `corner` kind. NaN is the quiet NaN every float32 path produces.
SPECIAL_VALUES: tuple[float, ...] = tuple(
    round_f32(value)
    for value in (
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        -0.5,
        2.0,
        -2.0,
        math.inf,
        -math.inf,
        math.nan,
        SMALLEST_NORMAL,
        SMALLEST_DENORMAL,
        LARGEST_DENORMAL,
        LARGEST_FINITE,
        -LARGEST_FINITE,
        round_f32(1e-30),
        round_f32(-1e-30),
        round_f32(1e30),
        round_f32(-1e30),
        16777216.0,
        -0.25,
        -3.0,
        -0.001,
    )
)


# ------------------------------------------------------------------------- vectors

Row = tuple[float, float, float, float]


@dataclass(frozen=True)
class TestVector:
    """One execution's inputs: v0..v15 and c0..c191, all exactly float32 values."""

    __test__ = False  # not a pytest class despite the name

    kind: str
    inputs: tuple[Row, ...]
    constants: tuple[Row, ...]

    def __post_init__(self) -> None:
        if len(self.inputs) != INPUT_REGISTERS or len(self.constants) != CONSTANT_REGISTERS:
            raise ValueError("a vector is 16 input rows and 192 constant rows")


def pack_vectors(vectors: Sequence[TestVector]) -> bytes:
    """The runner's IN file: per vector 832 little-endian float32, inputs then constants."""
    chunks = []
    for vector in vectors:
        flat = [x for row in vector.inputs for x in row]
        flat += [x for row in vector.constants for x in row]
        chunks.append(struct.pack(f"<{IN_FLOATS}f", *flat))
    return b"".join(chunks)


def _rows(rng: random.Random, count: int, element: Callable[[], float]) -> list[Row]:
    return [tuple(element() for _ in range(4)) for _ in range(count)]  # type: ignore[misc]


def _ordinary_element(rng: random.Random, a0_range: tuple[int, int] | None) -> float:
    if a0_range is not None and rng.random() < 0.5:
        # ARL floors its source, so [lo, hi + 0.999] reaches every A0 in range.
        return round_f32(rng.uniform(a0_range[0], a0_range[1] + 0.999))
    roll = rng.random()
    if roll < 0.15:
        return rng.choice((0.0, -0.0))
    if roll < 0.45:
        return float(rng.randint(-4, 4))
    return round_f32(rng.uniform(-4.0, 4.0))


def _matrix_rows(rng: random.Random) -> list[Row]:
    style = rng.choice(("rotation", "projection", "scale", "random"))
    if style == "rotation":
        axis = [rng.uniform(-1, 1) for _ in range(3)]
        length = math.sqrt(sum(a * a for a in axis)) or 1.0
        x, y, z = (a / length for a in axis)
        angle = rng.uniform(-math.pi, math.pi)
        c, s = math.cos(angle), math.sin(angle)
        t = 1 - c
        scale = rng.uniform(0.5, 2.0)
        rot = (
            (t * x * x + c, t * x * y + s * z, t * x * z - s * y),
            (t * x * y - s * z, t * y * y + c, t * y * z + s * x),
            (t * x * z + s * y, t * y * z - s * x, t * z * z + c),
        )
        rows = [(*(scale * v for v in row), 0.0) for row in rot]
        rows.append((rng.uniform(-10, 10), rng.uniform(-10, 10), rng.uniform(-10, 10), 1.0))
    elif style == "projection":
        near, far = rng.uniform(0.1, 1.0), rng.uniform(50.0, 500.0)
        q = far / (far - near)
        sx, sy = rng.uniform(0.5, 3.0), rng.uniform(0.5, 3.0)
        rows = [
            (sx, 0.0, 0.0, 0.0),
            (0.0, sy, 0.0, 0.0),
            (0.0, 0.0, q, 1.0),
            (0.0, 0.0, -q * near, 0.0),
        ]
    elif style == "scale":
        rows = [
            (rng.uniform(0.1, 4), 0.0, 0.0, 0.0),
            (0.0, rng.uniform(0.1, 4), 0.0, 0.0),
            (0.0, 0.0, rng.uniform(0.1, 4), 0.0),
            (0.0, 0.0, 0.0, 1.0),
        ]
    else:
        rows = [tuple(rng.uniform(-2, 2) for _ in range(4)) for _ in range(4)]  # type: ignore[misc]
    return [tuple(round_f32(v) for v in row) for row in rows]  # type: ignore[misc]


def _matrix_vector(rng: random.Random, a0_range: tuple[int, int] | None) -> TestVector:
    constants: list[Row] = []
    while len(constants) < CONSTANT_REGISTERS:
        constants.extend(_matrix_rows(rng))
    inputs: list[Row] = []
    for register in range(INPUT_REGISTERS):
        if register == 0:  # position
            row = (*(rng.uniform(-10, 10) for _ in range(3)), 1.0)
        elif register == 2:  # normal
            raw = [rng.uniform(-1, 1) for _ in range(3)]
            length = math.sqrt(sum(a * a for a in raw)) or 1.0
            row = (*(a / length for a in raw), 0.0)
        else:  # colours and texture coordinates
            row = tuple(rng.uniform(0, 1) for _ in range(4))
        inputs.append(tuple(round_f32(v) for v in row))  # type: ignore[arg-type]
    if a0_range is not None:
        for index in range(0, INPUT_REGISTERS, 3):
            inputs[index] = tuple(  # type: ignore[assignment]
                _ordinary_element(rng, a0_range) for _ in range(4)
            )
    return TestVector(KIND_MATRIX, tuple(inputs), tuple(constants[:CONSTANT_REGISTERS]))


def _relative_edge_values(bases: Sequence[int]) -> list[float]:
    """Inputs whose floor, added to a base constant index, lands on or past a bound."""
    targets = (0, CONSTANT_LIMIT, -1, CONSTANT_REGISTERS, 1, CONSTANT_LIMIT - 1, 95, -2, 193)
    values: list[float] = [-0.5, 191.5, 1e9, -1e9, math.nan, math.inf, -math.inf, 0.0, 0.999]
    for base in bases:
        for target in targets:
            a0 = target - base
            values.extend((float(a0), a0 + 0.5))
    unique: list[float] = []
    seen: set[int] = set()
    for value in values:
        bits = f32_bits(value)
        if bits not in seen:
            seen.add(bits)
            unique.append(round_f32(value))
    return unique


def make_vectors(
    seed: int,
    count: int,
    kind: str,
    *,
    a0_range: tuple[int, int] | None = None,
    relative_base: int | Sequence[int] = 0,
) -> list[TestVector]:
    """`count` deterministic float32 vectors of one kind.

    `a0_range` (inclusive A0 values) biases half the input elements of `ordinary` and
    `matrix` vectors so a program that does ARL then a relative read lands in range.
    `relative_base` (one or several base constant indices) steers `relative_edges`.
    """
    if kind not in KINDS:
        raise ValueError(f"unknown vector kind {kind!r}, expected one of {KINDS}")
    rng = random.Random(f"nv2a-validate:{seed}:{kind}")  # str seeds are stable across runs
    vectors: list[TestVector] = []
    bases = [relative_base] if isinstance(relative_base, int) else list(relative_base)
    edges = _relative_edge_values(bases or [0])
    for index in range(count):
        if kind == KIND_MATRIX:
            vectors.append(_matrix_vector(rng, a0_range))
        elif kind == KIND_CORNER:
            inputs = _rows(rng, INPUT_REGISTERS, lambda: rng.choice(SPECIAL_VALUES))
            constants = _rows(rng, CONSTANT_REGISTERS, lambda: rng.choice(SPECIAL_VALUES))
            vectors.append(TestVector(kind, tuple(inputs), tuple(constants)))
        elif kind == KIND_RELATIVE_EDGES:
            uniform = index % 2 == 0
            inputs = [
                tuple(
                    edges[(index // 2) % len(edges)]
                    if uniform
                    else edges[(index // 2 + register * 4 + lane) % len(edges)]
                    for lane in range(4)
                )
                for register in range(INPUT_REGISTERS)
            ]  # type: ignore[misc]
            constants = _rows(rng, CONSTANT_REGISTERS, lambda: _ordinary_element(rng, None))
            vectors.append(TestVector(kind, tuple(inputs), tuple(constants)))  # type: ignore[arg-type]
        else:
            inputs = _rows(rng, INPUT_REGISTERS, lambda: _ordinary_element(rng, a0_range))
            constants = _rows(rng, CONSTANT_REGISTERS, lambda: _ordinary_element(rng, None))
            vectors.append(TestVector(kind, tuple(inputs), tuple(constants)))
    return vectors


# ------------------------------------------------------------------------- compare


@dataclass(frozen=True)
class Mismatch:
    """One differing float. `vector` indexes the program's whole vector list."""

    program: str
    vector: int
    slot: int
    component: int
    expected: float
    actual: float
    category: str
    kind: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "program": self.program,
            "kind": self.kind,
            "vector": self.vector,
            "slot": self.slot,
            "component": self.component,
            "expected": _json_float(self.expected),
            "actual": _json_float(self.actual),
            "expected_bits": f"0x{f32_bits(self.expected):08x}",
            "actual_bits": f"0x{f32_bits(self.actual):08x}",
            "category": self.category,
        }


def _json_float(value: float) -> float | str:
    return value if math.isfinite(value) else repr(value)


def classify(expected: float, actual: float, rtol: float = RTOL, atol: float = ATOL) -> str | None:
    """None when equal under the tolerance, else CATEGORY_SPECIAL when a NaN or an
    infinity is involved, else CATEGORY_ARITHMETIC.

    Equal means: both NaN (payload and sign ignored), or both infinities of the same
    sign, or both finite with magnitudes below 2^-126 flushed to zero and then
    `abs(a - b) <= atol + rtol * max(abs(a), abs(b))`. +0.0 equals -0.0.
    """
    expected_nan, actual_nan = math.isnan(expected), math.isnan(actual)
    if expected_nan or actual_nan:
        return None if expected_nan and actual_nan else CATEGORY_SPECIAL
    expected_inf, actual_inf = math.isinf(expected), math.isinf(actual)
    if expected_inf or actual_inf:
        return None if expected == actual else CATEGORY_SPECIAL
    if abs(expected) < DENORMAL_LIMIT:
        expected = 0.0
    if abs(actual) < DENORMAL_LIMIT:
        actual = 0.0
    if abs(expected - actual) <= atol + rtol * max(abs(expected), abs(actual)):
        return None
    return CATEGORY_ARITHMETIC


def floats_equal(expected: float, actual: float, rtol: float = RTOL, atol: float = ATOL) -> bool:
    return classify(expected, actual, rtol, atol) is None


Outputs = Sequence[Sequence[float]]  # 16 slots of 4 floats


def compare(
    expected: Sequence[Outputs],
    actual: Sequence[Outputs],
    rtol: float = RTOL,
    atol: float = ATOL,
    *,
    program: str = "program",
    kinds: Sequence[str] | None = None,
) -> list[Mismatch]:
    """Compare per-vector outputs (each 16 slots x 4 floats). Every differing float is
    returned. A shape difference raises, it is never treated as a pass."""
    if len(expected) != len(actual):
        raise ValueError(f"{len(expected)} expected vectors but {len(actual)} actual")
    found: list[Mismatch] = []
    for vector, (want_slots, got_slots) in enumerate(zip(expected, actual, strict=True)):
        if len(want_slots) != OUTPUT_REGISTERS or len(got_slots) != OUTPUT_REGISTERS:
            raise ValueError(f"vector {vector}: outputs are not {OUTPUT_REGISTERS} slots")
        for slot, (want_row, got_row) in enumerate(zip(want_slots, got_slots, strict=True)):
            if len(want_row) != 4 or len(got_row) != 4:
                raise ValueError(f"vector {vector} slot {slot}: not 4 components")
            for component, (want, got) in enumerate(zip(want_row, got_row, strict=True)):
                category = classify(want, got, rtol, atol)
                if category is not None:
                    found.append(
                        Mismatch(
                            program,
                            vector,
                            slot,
                            component,
                            want,
                            got,
                            category,
                            kinds[vector] if kinds is not None else "",
                        )
                    )
    return found


# -------------------------------------------------------------------------- runner


@dataclass(frozen=True)
class Job:
    spirv: Path
    inputs: Path
    output: Path
    count: int


@dataclass(frozen=True)
class JobResult:
    index: int
    ok: bool
    reason: str = ""


def _runner_sources(entry: Path) -> tuple[Path, ...]:
    """`entry` (vkrun.c by default, vkvert.c for the vertex stage) plus the shared core."""
    return (entry, *RUNNER_SOURCES[1:])


def _needs_build(binary: Path, sources: Sequence[Path] = RUNNER_SOURCES) -> bool:
    if not binary.exists():
        return True
    built = binary.stat().st_mtime
    return any(built < source.stat().st_mtime for source in sources)


def build_runner(bin_dir: Path = DEFAULT_BIN_DIR, entry: Path = RUNNER_SOURCE) -> Path:
    """Compile the runner entry (vkrun.c by default) if the binary is missing or older than
    its sources. The binary is named after the entry file."""
    binary = bin_dir / entry.stem
    sources = _runner_sources(entry)
    if not _needs_build(binary, sources):
        return binary
    if shutil.which("gcc") is None:
        raise RunnerUnavailable("gcc not found")
    bin_dir.mkdir(parents=True, exist_ok=True)
    # Build under a private name then rename, so concurrent users never see a half file.
    handle, temp_name = tempfile.mkstemp(prefix=f"{entry.stem}.", dir=bin_dir)
    os.close(handle)
    compile_units = [str(source) for source in sources if source.suffix == ".c"]
    command = ["gcc", "-O2", "-Wall", "-Wextra", "-o", temp_name, *compile_units, "-lvulkan"]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=300, check=False)
        if done.returncode != 0:
            raise RunnerUnavailable(f"{entry.stem} build failed: {done.stderr.strip()[:500]}")
        os.replace(temp_name, binary)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)
    return binary


class VkRunner:
    """Drives vkrun. Anything with `run_batch(jobs) -> list[JobResult]` can stand in.

    `entry` selects another runner built on the same core (T100: vkvert.c, the vertex stage)
    and `device` sets VKRUN_DEVICE so a run can be pointed at another device on purpose
    ("hardware" or a name substring). The default stays the software device."""

    def __init__(
        self,
        *,
        bin_dir: Path = DEFAULT_BIN_DIR,
        zero_out: bool = False,
        timeout: float = 900.0,
        entry: Path = RUNNER_SOURCE,
        device: str | None = None,
    ) -> None:
        self.bin_dir = bin_dir
        self.zero_out = zero_out
        self.timeout = timeout
        self.entry = entry
        #: Device name text for VKRUN_DEVICE (T98, any device type; T100 adds "hardware" for
        #: the first non-software device). None keeps the software-only default, which is what
        #: every other user of the harness wants.
        self.device = device
        self._binary: Path | None = None

    def _environment(self) -> dict[str, str]:
        environment = dict(os.environ)
        if self.device:
            environment["VKRUN_DEVICE"] = self.device
        else:
            environment.pop("VKRUN_DEVICE", None)
        return environment

    @property
    def binary(self) -> Path:
        if self._binary is None:
            self._binary = build_runner(self.bin_dir, self.entry)
        return self._binary

    def info(self) -> dict[str, str]:
        done = subprocess.run(
            [str(self.binary), "--info"],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            env=self._environment(),
        )
        if done.returncode != 0:
            raise RunnerUnavailable(f"vkrun --info failed: {done.stderr.strip()[:300]}")
        info = {}
        for line in done.stdout.splitlines():
            key, _, value = line.partition(":")
            info[key.strip()] = value.strip()
        return info

    def run_batch(self, jobs: Sequence[Job]) -> list[JobResult]:
        """Run every job. A job the runner never reached (crash, timeout) is a failure
        with the reason, and a crash only costs the job that caused it."""
        results: dict[int, JobResult] = {}
        pending = list(range(len(jobs)))
        with tempfile.TemporaryDirectory(prefix="vkrun-manifest-") as scratch:
            while pending:
                manifest = Path(scratch) / "manifest.txt"
                manifest.write_text(
                    "".join(
                        f"{jobs[i].spirv} {jobs[i].inputs} {jobs[i].output} {jobs[i].count}\n"
                        for i in pending
                    )
                )
                command = [str(self.binary)] + (["--zero-out"] if self.zero_out else [])
                try:
                    done = subprocess.run(
                        [*command, str(manifest)],
                        capture_output=True,
                        text=True,
                        timeout=self.timeout,
                        check=False,
                        env=self._environment(),
                    )
                except subprocess.TimeoutExpired:
                    for index in pending:
                        results[index] = JobResult(index, False, "runner timed out")
                    break
                handled = self._parse(done.stdout, pending, results)
                remaining = pending[handled:]
                if remaining:
                    # The runner died before reaching this job: blame it, retry the rest.
                    culprit = remaining[0]
                    results[culprit] = JobResult(
                        culprit,
                        False,
                        f"runner exited with {done.returncode}: {done.stderr[-200:]}",
                    )
                    remaining = remaining[1:]
                pending = remaining
        return [results[i] for i in range(len(jobs))]

    @staticmethod
    def _parse(stdout: str, pending: list[int], results: dict[int, JobResult]) -> int:
        handled = 0
        for line in stdout.splitlines():
            parts = line.split(" ", 2)
            if len(parts) < 2 or parts[0] not in ("OK", "FAIL") or not parts[1].isdigit():
                continue
            position = int(parts[1])
            if position != handled or position >= len(pending):
                continue
            index = pending[position]
            ok = parts[0] == "OK"
            results[index] = JobResult(
                index, ok, "" if ok else (parts[2] if len(parts) > 2 else "")
            )
            handled += 1
        return handled


def runner_unavailable_reason(runner: VkRunner | None = None) -> str | None:
    """None when glslang, gcc and a lavapipe device are all usable, else the reason."""
    if shutil.which("glslangValidator") is None:
        return "glslangValidator not found"
    try:
        info = (runner or VkRunner()).info()
    except (RunnerUnavailable, OSError, subprocess.SubprocessError) as error:
        return str(error)
    if not info.get("device"):
        return "no software Vulkan device"
    return None


def read_outputs(path: Path, count: int) -> list[tuple[Row, ...]]:
    """Parse the runner's OUT file into per-vector 16 x 4 floats."""
    data = path.read_bytes()
    if len(data) != count * OUT_FLOATS * 4:
        raise RunnerError(f"{path.name}: {len(data)} bytes, expected {count * OUT_FLOATS * 4}")
    flat = struct.unpack(f"<{count * OUT_FLOATS}f", data)
    vectors = []
    for vector in range(count):
        base = vector * OUT_FLOATS
        vectors.append(
            tuple(tuple(flat[base + slot * 4 : base + slot * 4 + 4]) for slot in range(16))
        )
    return vectors


def compile_glsl(source_path: Path, spirv_path: Path, stage: str = "comp") -> None:
    """glslang to SPIR-V for Vulkan 1.1 (`stage` comp, or vert for T100). Raises
    CompileError with the compiler's words."""
    if shutil.which("glslangValidator") is None:
        raise RunnerUnavailable("glslangValidator not found")
    done = subprocess.run(
        [
            "glslangValidator",
            "-V",
            "--target-env",
            "vulkan1.1",
            "-S",
            stage,
            "-o",
            str(spirv_path),
            str(source_path),
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    if done.returncode != 0:
        raise CompileError((done.stdout + done.stderr).strip()[:1500])


def run_glsl_vectors(
    glsl_source: str,
    vectors: Sequence[TestVector],
    *,
    work: Path | None = None,
    runner: Any = None,
    name: str = "shader",
) -> list[tuple[Row, ...]]:
    """Run ANY compute shader that respects the layout contract over `vectors` and return
    the per-vector 16 x 4 outputs. Raises CompileError or RunnerError on failure."""
    runner = runner or VkRunner()
    with tempfile.TemporaryDirectory(prefix="nv2a-run-", dir=_ensure(work)) as scratch:
        directory = Path(scratch)
        source = directory / f"{name}.comp"
        spirv = directory / f"{name}.spv"
        source.write_text(glsl_source)
        compile_glsl(source, spirv)
        inputs = directory / f"{name}.in"
        output = directory / f"{name}.out"
        inputs.write_bytes(pack_vectors(vectors))
        (result,) = runner.run_batch([Job(spirv, inputs, output, len(vectors))])
        if not result.ok:
            raise RunnerError(result.reason)
        return read_outputs(output, len(vectors))


def _ensure(work: Path | None) -> Path | None:
    if work is not None:
        work.mkdir(parents=True, exist_ok=True)
    return work


# ------------------------------------------------------------------------- reports

STATUS_PASS = "pass"
STATUS_MISMATCH = "mismatch"
STATUS_COMPILE_ERROR = "compile-error"
STATUS_RUNNER_FAILURE = "runner-failure"
STATUS_INTERPRETER_ERROR = "interpreter-error"
STATUS_TRANSLATOR_ERROR = "translator-error"


@dataclass
class ProgramReport:
    name: str
    status: str
    detail: str = ""
    vectors: int = 0
    mismatch_total: int = 0
    mismatch_by_category: dict[str, int] = field(default_factory=dict)
    mismatch_by_kind: dict[str, int] = field(default_factory=dict)
    records: list[Mismatch] = field(default_factory=list)
    interpreter_errors: int = 0
    executed: Counter[str] = field(default_factory=Counter)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
            "vectors": self.vectors,
            "mismatch_total": self.mismatch_total,
            "mismatch_by_category": dict(self.mismatch_by_category),
            "mismatch_by_kind": dict(self.mismatch_by_kind),
            "interpreter_errors": self.interpreter_errors,
            "records": [record.to_dict() for record in self.records],
        }


@dataclass
class ValidationReport:
    programs: dict[str, ProgramReport] = field(default_factory=dict)
    rtol: float = RTOL
    atol: float = ATOL
    seed: int = 0
    kinds: tuple[str, ...] = ()
    vectors_per_kind: int = 0

    def status_counts(self) -> dict[str, int]:
        counts: Counter[str] = Counter()
        for program in self.programs.values():
            counts[
                "unsupported" if program.status.startswith("unsupported:") else program.status
            ] += 1
        return dict(counts)

    def unsupported_codes(self) -> dict[str, int]:
        codes: Counter[str] = Counter()
        for program in self.programs.values():
            if program.status.startswith("unsupported:"):
                codes[program.status.split(":", 1)[1]] += 1
        return dict(codes)

    def mismatch_counts(self) -> dict[str, Any]:
        by_category: Counter[str] = Counter()
        by_kind: Counter[str] = Counter()
        special_only = 0
        for program in self.programs.values():
            by_category.update(program.mismatch_by_category)
            by_kind.update(program.mismatch_by_kind)
            if program.mismatch_total and set(program.mismatch_by_category) == {CATEGORY_SPECIAL}:
                special_only += 1
        return {
            "floats_by_category": dict(by_category),
            "floats_by_kind": dict(by_kind),
            "programs_special_only": special_only,
            "programs_with_arithmetic": sum(
                1 for p in self.programs.values() if p.mismatch_by_category.get(CATEGORY_ARITHMETIC)
            ),
        }

    def opcode_coverage(self) -> dict[str, dict[str, int]]:
        """Opcode executions behind passing comparisons, and behind every run."""
        passing: Counter[str] = Counter()
        everything: Counter[str] = Counter()
        for program in self.programs.values():
            everything.update(program.executed)
            if program.status == STATUS_PASS:
                passing.update(program.executed)
        return {"passing": dict(sorted(passing.items())), "all": dict(sorted(everything.items()))}

    def examples(self, limit: int = DEFAULT_MAX_RECORDS) -> list[Mismatch]:
        found: list[Mismatch] = []
        for program in self.programs.values():
            found.extend(program.records)
            if len(found) >= limit:
                break
        return found[:limit]

    @property
    def failed(self) -> bool:
        """True when anything other than pass or unsupported happened."""
        return any(
            p.status != STATUS_PASS and not p.status.startswith("unsupported")
            for p in self.programs.values()
        )

    def to_dict(self, examples: int = DEFAULT_MAX_RECORDS) -> dict[str, Any]:
        return {
            "programs_total": len(self.programs),
            "tolerance": {"rtol": self.rtol, "atol": self.atol, "denormal_limit": DENORMAL_LIMIT},
            "seed": self.seed,
            "kinds": list(self.kinds),
            "vectors_per_kind": self.vectors_per_kind,
            "status_counts": self.status_counts(),
            "unsupported_codes": self.unsupported_codes(),
            "mismatches": self.mismatch_counts(),
            "opcode_coverage": self.opcode_coverage(),
            "examples": [record.to_dict() for record in self.examples(examples)],
            "programs": [program.to_dict() for program in self.programs.values()],
        }

    def summary(self, examples: int = DEFAULT_MAX_RECORDS) -> str:
        lines = [f"programs: {len(self.programs)}"]
        for status, count in sorted(self.status_counts().items()):
            lines.append(f"  {status}: {count}")
        for code, count in sorted(self.unsupported_codes().items()):
            lines.append(f"  unsupported:{code}: {count}")
        counts = self.mismatch_counts()
        lines.append(f"mismatched floats by category: {counts['floats_by_category']}")
        lines.append(f"mismatched floats by vector kind: {counts['floats_by_kind']}")
        lines.append(
            f"programs with special-value-only mismatches: {counts['programs_special_only']}"
        )
        for record in self.examples(examples):
            lines.append(
                f"  {record.program} [{record.kind}] vec {record.vector} o{record.slot}."
                f"{'xyzw'[record.component]} expected {record.expected!r} actual {record.actual!r}"
                f" ({record.category})"
            )
        return "\n".join(lines)


# --------------------------------------------------------------------- validation


class ModelInterpreter:
    """The interpreter module with its arithmetic model fixed (`spec` or `ieee`).

    A stand-in module without models (the tests inject some) is called without one.
    """

    def __init__(self, module: ModuleType, model: str, output_init: str = "zero") -> None:
        self._module = module
        self._models = {"spec": "SPEC_MODEL", "ieee": "IEEE_MODEL"}
        self._model = getattr(module, self._models[model], None)
        self._output_init = output_init

    def run(self, program: bytes, inputs: Any, constants: Any, *, a0: int = 0) -> Any:
        # The default is not passed, so a stand-in module without the option still works.
        extra = {} if self._output_init == "zero" else {"output_init": self._output_init}
        if self._model is None:
            return self._module.run(program, inputs, constants, a0=a0, **extra)
        return self._module.run(program, inputs, constants, a0=a0, model=self._model, **extra)


def _load(name: str) -> ModuleType:
    return importlib.import_module(name)


def relative_profile(data: bytes) -> tuple[bool, list[int]]:
    """Whether the program reads constants relative to A0 (or writes A0), and the base
    constant indices of its relative reads. Undecodable input counts as not relative."""
    try:
        isa = _load("tools.nv2a.isa")
        decoded = isa.decode_program(data)
    except Exception:
        return False, []
    bases = sorted({d.const for d in decoded if d.relative})
    uses_arl = any(d.mac == isa.MAC_ARL for d in decoded)
    return (bool(bases) or uses_arl), bases


def a0_range_for(bases: Sequence[int]) -> tuple[int, int]:
    """A0 values keeping `base + A0` inside 0..191 for every relative base."""
    if not bases:
        return (0, CONSTANT_LIMIT)
    low, high = -min(bases), CONSTANT_LIMIT - max(bases)
    return (low, high) if low <= high else (0, 0)


def build_vectors(
    data: bytes, seed: int, count: int, kinds: Sequence[str]
) -> tuple[list[TestVector], list[str]]:
    """The program's vectors (`count` per kind) and the kind of each, with relative
    addressing steering applied when the program uses it."""
    uses_relative, bases = relative_profile(data)
    a0_range = a0_range_for(bases) if uses_relative else None
    vectors: list[TestVector] = []
    for kind in kinds:
        vectors.extend(
            make_vectors(seed, count, kind, a0_range=a0_range, relative_base=bases or [0])
        )
    return vectors, [v.kind for v in vectors]


def _safe_name(index: int, name: str) -> str:
    cleaned = "".join(c if c.isalnum() or c in "._-" else "_" for c in name)[:80]
    return f"{index:05d}_{cleaned}"


@dataclass
class _Pending:
    index: int
    name: str
    data: bytes
    vectors: list[TestVector]
    kinds: list[str]
    report: ProgramReport
    spirv: Path | None = None
    inputs: Path | None = None
    output: Path | None = None


def _finish(
    pending: _Pending,
    interpreter: Any,
    rtol: float,
    atol: float,
    max_records: int,
) -> None:
    """Run the interpreter over the program's vectors, compare, and set the status."""
    report = pending.report
    assert pending.output is not None
    actual = read_outputs(pending.output, len(pending.vectors))
    expected: list[Outputs] = []
    kept_actual: list[Outputs] = []
    kept_index: list[int] = []
    first_error = ""
    for index, vector in enumerate(pending.vectors):
        try:
            result = interpreter.run(pending.data, vector.inputs, vector.constants, a0=0)
        except Exception as error:  # an interpreter bug must be a reported outcome
            report.interpreter_errors += 1
            first_error = first_error or f"vector {index}: {type(error).__name__}: {error}"
            continue
        expected.append(result.outputs)
        kept_actual.append(actual[index])
        kept_index.append(index)
        executed = getattr(result, "executed", None)
        if executed:
            report.executed.update(executed)
    kinds = [pending.kinds[i] for i in kept_index]
    mismatches = compare(expected, kept_actual, rtol, atol, program=pending.name, kinds=kinds)
    # compare() indexes within the kept list, map back to the program's vector numbers.
    mismatches = [
        Mismatch(
            m.program,
            kept_index[m.vector],
            m.slot,
            m.component,
            m.expected,
            m.actual,
            m.category,
            m.kind,
        )
        for m in mismatches
    ]
    report.mismatch_total = len(mismatches)
    report.mismatch_by_category = dict(Counter(m.category for m in mismatches))
    report.mismatch_by_kind = dict(Counter(m.kind for m in mismatches))
    report.records = mismatches[:max_records]
    if mismatches:
        report.status = STATUS_MISMATCH
        if set(report.mismatch_by_category) == {CATEGORY_SPECIAL}:
            report.detail = "special-value only"
    elif report.interpreter_errors:
        report.status = STATUS_INTERPRETER_ERROR
        report.detail = f"{report.interpreter_errors} vectors failed, first: {first_error}"
    else:
        report.status = STATUS_PASS
    if mismatches and report.interpreter_errors:
        report.detail = (
            report.detail + f"; {report.interpreter_errors} interpreter errors"
        ).lstrip("; ")


def validate_many(
    programs: dict[str, bytes],
    *,
    count: int = 64,
    seed: int = 0,
    kinds: Sequence[str] = KINDS,
    vectors: Sequence[TestVector] | None = None,
    work: Path | None = None,
    runner: Any = None,
    translator: Any = None,
    interpreter: Any = None,
    rtol: float = RTOL,
    atol: float = ATOL,
    max_records: int = DEFAULT_MAX_RECORDS,
    chunk: int = DEFAULT_CHUNK,
    jobs: int | None = None,
    keep_data: bool = False,
    output_init: str = "zero",
) -> ValidationReport:
    """Translate, compile, run and compare every program. `count` vectors per kind, or an
    explicit `vectors` list used for every program. `translator` and `interpreter` default
    to `tools.nv2a.translate` and `tools.nv2a.interp` (any object with the same
    functions works, which is how the tests inject stand-ins).

    `output_init` (T561) is the translator's output register start value ("zero" or "nv",
    `translate.OUTPUT_INITS`). It is passed to `compute_test_source` only when not the
    default. The interpreter must be built with the same value (`ModelInterpreter`), the
    CLI does both."""
    translator = translator or _load("tools.nv2a.translate")
    interpreter = interpreter or _load("tools.nv2a.interp")
    runner = runner or VkRunner()
    directory = work or DEFAULT_WORK_DIR
    directory.mkdir(parents=True, exist_ok=True)
    report = ValidationReport(
        rtol=rtol, atol=atol, seed=seed, kinds=tuple(kinds), vectors_per_kind=count
    )
    unsupported_type = getattr(translator, "Unsupported", ())
    source_options = {} if output_init == "zero" else {"output_init": output_init}
    items = list(programs.items())
    for start in range(0, len(items), chunk):
        batch: list[_Pending] = []
        for index, (name, data) in enumerate(items[start : start + chunk], start):
            program_report = ProgramReport(name, "")
            report.programs[name] = program_report
            try:
                translation = translator.translate(data, name=name)
                source = translator.compute_test_source(translation, **source_options)
            except unsupported_type as error:
                program_report.status = f"unsupported:{getattr(error, 'code', 'unknown')}"
                program_report.detail = str(error)[:300]
                continue
            except Exception as error:
                program_report.status = STATUS_TRANSLATOR_ERROR
                program_report.detail = f"{type(error).__name__}: {error}"[:300]
                continue
            if vectors is not None:
                chosen, chosen_kinds = list(vectors), [v.kind for v in vectors]
            else:
                chosen, chosen_kinds = build_vectors(data, seed, count, kinds)
            program_report.vectors = len(chosen)
            stem = _safe_name(index, name)
            pending = _Pending(index, name, data, chosen, chosen_kinds, program_report)
            pending.spirv = directory / f"{stem}.spv"
            pending.inputs = directory / f"{stem}.in"
            pending.output = directory / f"{stem}.out"
            (directory / f"{stem}.comp").write_text(source)
            batch.append(pending)
        _run_chunk(
            batch,
            directory,
            runner,
            interpreter,
            rtol,
            atol,
            max_records,
            jobs,
            keep_data,
        )
    return report


def _run_chunk(
    batch: list[_Pending],
    directory: Path,
    runner: Any,
    interpreter: Any,
    rtol: float,
    atol: float,
    max_records: int,
    jobs: int | None,
    keep_data: bool,
) -> None:
    def compile_one(pending: _Pending) -> str | None:
        assert pending.spirv is not None
        try:
            compile_glsl(pending.spirv.with_suffix(".comp"), pending.spirv)
        except CompileError as error:
            return str(error)
        return None

    workers = jobs or min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        errors = list(pool.map(compile_one, batch))
    runnable: list[_Pending] = []
    for pending, error in zip(batch, errors, strict=True):
        if error is not None:
            pending.report.status = STATUS_COMPILE_ERROR
            pending.report.detail = error
        else:
            runnable.append(pending)
    for pending in runnable:
        assert pending.inputs is not None
        pending.inputs.write_bytes(pack_vectors(pending.vectors))
    if not runnable:
        return
    job_list = [
        Job(p.spirv, p.inputs, p.output, len(p.vectors))  # type: ignore[arg-type]
        for p in runnable
    ]
    results = runner.run_batch(job_list)
    for pending, result in zip(runnable, results, strict=True):
        if not result.ok:
            pending.report.status = STATUS_RUNNER_FAILURE
            pending.report.detail = result.reason[:300]
            continue
        try:
            _finish(pending, interpreter, rtol, atol, max_records)
        except (RunnerError, ValueError) as error:
            pending.report.status = STATUS_RUNNER_FAILURE
            pending.report.detail = str(error)[:300]
    if not keep_data:
        for pending in runnable:
            for path in (pending.inputs, pending.output):
                if path is not None:
                    path.unlink(missing_ok=True)


def validate_program(
    data: bytes,
    name: str = "program",
    *,
    vectors: Sequence[TestVector] | None = None,
    count: int = 64,
    seed: int = 0,
    kinds: Sequence[str] = KINDS,
    runner: Any = None,
    **options: Any,
) -> ProgramReport:
    """`validate_many` for one program, returning its ProgramReport."""
    report = validate_many(
        {name: data}, vectors=vectors, count=count, seed=seed, kinds=kinds, runner=runner, **options
    )
    return report.programs[name]


# ------------------------------------------------------------------------------ CLI


def _collect_programs(paths: Iterable[str], limit: int | None) -> dict[str, bytes]:
    files: list[Path] = []
    for entry in paths:
        path = Path(entry)
        if path.is_dir():
            files.extend(sorted(path.glob("*.bin")))
        elif path.is_file():
            files.append(path)
        else:
            raise SystemExit(f"validate: {entry} is not a file or directory")
    if limit is not None:
        files = files[:limit]
    return {path.stem: path.read_bytes() for path in files}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.validate",
        description="Compare translated NV2A vertex programs on lavapipe with the interpreter.",
    )
    parser.add_argument(
        "--programs", nargs="*", default=[], help="program files or directories of *.bin"
    )
    parser.add_argument("--vectors", type=int, default=64, help="vectors per kind per program")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--kinds", default=",".join(KINDS), help="comma separated vector kinds")
    parser.add_argument("--work", type=Path, default=DEFAULT_WORK_DIR, help="scratch directory")
    parser.add_argument("--report", type=Path, default=None, help="write the JSON report here")
    parser.add_argument("--limit", type=int, default=None, help="only the first K programs")
    parser.add_argument("--rtol", type=float, default=RTOL)
    parser.add_argument("--atol", type=float, default=ATOL)
    parser.add_argument("--examples", type=int, default=DEFAULT_MAX_RECORDS)
    parser.add_argument(
        "--model",
        choices=("spec", "ieee"),
        default="ieee",
        help="interpreter arithmetic: plain IEEE (what a GPU computes) or the NV spec departures",
    )
    parser.add_argument(
        "--output-init",
        choices=("zero", "nv"),
        default="zero",
        help="output register start value for BOTH the translator and the interpreter: "
        "zero, or nv = (0, 0, 0, 1) (T561, INFERRED)",
    )
    parser.add_argument("--zero-out", action="store_true", help="zero (not NaN) the output buffer")
    parser.add_argument("--info", action="store_true", help="print the runner device info and exit")
    args = parser.parse_args(argv)

    runner = VkRunner(zero_out=args.zero_out)
    if args.info:
        try:
            for key, value in runner.info().items():
                print(f"{key}: {value}")
        except RunnerUnavailable as error:
            print(f"validate: {error}", file=sys.stderr)
            return 2
        return 0
    kinds = [kind for kind in args.kinds.split(",") if kind]
    unknown = [kind for kind in kinds if kind not in KINDS]
    if unknown or not kinds:
        parser.error(f"--kinds must be a non-empty subset of {','.join(KINDS)}")
    if not args.programs:
        parser.error("--programs is required")
    programs = _collect_programs(args.programs, args.limit)
    if not programs:
        print("validate: no programs found", file=sys.stderr)
        return 2
    try:
        result = validate_many(
            programs,
            count=args.vectors,
            seed=args.seed,
            kinds=kinds,
            work=args.work,
            runner=runner,
            interpreter=ModelInterpreter(_load("tools.nv2a.interp"), args.model, args.output_init),
            rtol=args.rtol,
            atol=args.atol,
            max_records=args.examples,
            output_init=args.output_init,
        )
    except (RunnerUnavailable, ModuleNotFoundError) as error:
        print(f"validate: {error}", file=sys.stderr)
        return 2
    print(result.summary(args.examples))
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result.to_dict(args.examples), indent=1) + "\n")
    return 1 if result.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
