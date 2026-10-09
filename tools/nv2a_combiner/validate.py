# SPDX-License-Identifier: GPL-3.0-or-later
"""Evaluate translated shaders on lavapipe and compare them with the reference model.

WHY LAVAPIPE. `src/gpu/gpu_device.c` is the project's Vulkan device and it renders
triangles. Validating a combiner needs thousands of arbitrary fragment inputs and the
exact output floats back. Rasterising that would mean one pixel per input and a colour
buffer that rounds to 8 bits, which hides the error being measured. A COMPUTE shader on the
same software Vulkan implementation (lavapipe, `llvmpipe` in the device name) runs the same
GLSL front end and the same float32 arithmetic class with no rounding on the way out, so
`vkeval.c` is a small standalone compute runner and not a use of `gpu_device.c`. It shares
its implementation (`tools/nv2a/vkrun_core.c`) with the vertex-program runner
`tools/nv2a/vkrun.c`. Both select the software device (CPU type or llvmpipe/lavapipe name)
and never fall back to a GPU.

THE INPUTS. Random fragments mix three populations because uniform floats alone almost
never land on the places combiners are decided (the mux threshold, a clamp edge, an exact
zero): uniform floats, 8-bit grid values k/255 (what a real texel or vertex colour is),
and corner values. Everything is rounded to float32 before BOTH evaluators see it, so the
comparison measures arithmetic, not input rounding.
"""

from __future__ import annotations

import math
import os
import random
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import glsl, models, reference

#: Floats per fragment in the compute shader's input buffer, 45 vec4s.
IN_FLOATS = 45 * 4
OUT_FLOATS = 8
#: Absolute tolerance, about 2.5% of one 8-bit step. float32 error over 8 chained stages
#: is measured well below this, see `docs/combiner-translator.md`.
TOLERANCE = 1e-4

CORNERS = (0.0, 1.0, 0.5, 1.0 / 255.0, 127.0 / 255.0, 128.0 / 255.0, 254.0 / 255.0)

SOURCE_DIRECTORY = Path(__file__).resolve().parent


def to_float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


@dataclass
class Case:
    """One fragment and the procedural texture coefficients it samples."""

    fragment: reference.Fragment
    #: [stage][term 0..4 = base, du, dv, dw, duv][channel]
    coefficients: list[list[list[float]]]


def _sampler(coefficients: list[list[list[float]]]) -> reference.Sampler:
    def sample(stage: int, coord: reference.Vec3) -> reference.Vec4:
        base, du, dv, dw, duv = coefficients[stage]
        u, v, w = coord
        return tuple(  # type: ignore[return-value]
            reference.clamp(
                base[c] + du[c] * u + dv[c] * v + dw[c] * w + duv[c] * (u * v), 0.0, 1.0
            )
            for c in range(4)
        )

    return sample


class _Draw:
    """Draws one value from the three populations."""

    def __init__(self, rng: random.Random) -> None:
        self.rng = rng

    def unit(self) -> float:
        roll = self.rng.random()
        if roll < 0.3:
            return self.rng.choice(CORNERS)
        if roll < 0.65:
            return self.rng.randrange(256) / 255.0
        return self.rng.random()

    def wide(self) -> float:
        roll = self.rng.random()
        if roll < 0.2:
            return self.rng.choice((-1.0, 0.0, 1.0, 0.5, -0.5))
        return self.rng.uniform(-1.5, 1.5)

    def vec(self, count: int = 4) -> tuple[float, ...]:
        return tuple(to_float32(self.unit()) for _ in range(count))


def make_case(rng: random.Random) -> Case:
    draw = _Draw(rng)
    texcoords = []
    for _ in range(4):
        x, y, z = (to_float32(draw.wide()) for _ in range(3))
        w = to_float32(rng.choice((1.0, 0.5, 2.0, rng.uniform(0.25, 2.0))))
        texcoords.append((x, y, z, w))
    coefficients = [
        [[to_float32(rng.uniform(-1.0, 1.0)) for _ in range(4)] for _ in range(5)] for _ in range(4)
    ]
    fog = draw.vec(4)
    fragment = reference.Fragment(
        v0=draw.vec(),  # type: ignore[arg-type]
        v1=draw.vec(),  # type: ignore[arg-type]
        texcoord=tuple(texcoords),  # type: ignore[arg-type]
        fog_color=fog[:3],  # type: ignore[arg-type]
        fog_factor=fog[3],
        factor0=tuple(draw.vec() for _ in range(8)),  # type: ignore[misc]
        factor1=tuple(draw.vec() for _ in range(8)),  # type: ignore[misc]
        final_c0=draw.vec(),  # type: ignore[arg-type]
        final_c1=draw.vec(),  # type: ignore[arg-type]
        sampler=_sampler(coefficients),
    )
    return Case(fragment, coefficients)


def make_cases(seed: int, count: int) -> list[Case]:
    rng = random.Random(seed)
    return [make_case(rng) for _ in range(count)]


def pack_cases(cases: list[Case]) -> bytes:
    """The input buffer layout documented on `glsl.compute_shader`."""
    floats: list[float] = []
    for case in cases:
        f = case.fragment
        floats += [*f.v0, *f.v1]
        for coord in f.texcoord:
            floats += coord
        floats += [*f.fog_color, f.fog_factor]
        for vector in f.factor0:
            floats += vector
        for vector in f.factor1:
            floats += vector
        floats += [*f.final_c0, *f.final_c1]
        for stage in case.coefficients:
            for term in stage:
                floats += term
    assert len(floats) == IN_FLOATS * len(cases)
    return struct.pack(f"<{len(floats)}f", *floats)


def runner_path(directory: Path) -> Path:
    return directory / "bin" / "vkeval"


#: The entry file plus the core shared with tools/nv2a/vkrun.c. The header is listed
#: so a change to the profile struct also triggers a rebuild.
RUNNER_SOURCES = (
    SOURCE_DIRECTORY / "vkeval.c",
    SOURCE_DIRECTORY.parent / "nv2a" / "vkrun_core.c",
    SOURCE_DIRECTORY.parent / "nv2a" / "vkrun_core.h",
)


def build_runner(directory: Path) -> Path:
    """Compile `vkeval.c` into `directory/bin`, only when missing or older than a source."""
    target = runner_path(directory)
    if target.exists() and all(
        target.stat().st_mtime >= source.stat().st_mtime for source in RUNNER_SOURCES
    ):
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("gcc") or shutil.which("cc")
    if compiler is None:
        raise RuntimeError("no C compiler on PATH")
    compile_units = [str(source) for source in RUNNER_SOURCES if source.suffix == ".c"]
    subprocess.run(
        [compiler, "-std=c11", "-O2", "-Wall", "-o", str(target), *compile_units, "-lvulkan"],
        check=True,
        timeout=120,
    )
    return target


def compile_spirv(glsl_text: str, directory: Path, name: str) -> Path:
    """glslangValidator to SPIR-V. The source stays next to it for inspection."""
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / f"{name}.comp"
    output = directory / f"{name}.spv"
    source.write_text(glsl_text)
    subprocess.run(
        ["glslangValidator", "-V", "--target-env", "vulkan1.1", str(source), "-o", str(output)],
        check=True,
        capture_output=True,
        timeout=60,
    )
    return output


def available() -> bool:
    """Whether glslangValidator, a C compiler and the Vulkan loader are all present."""
    if shutil.which("glslangValidator") is None:
        return False
    if shutil.which("gcc") is None and shutil.which("cc") is None:
        return False
    return Path("/usr/include/vulkan/vulkan.h").exists()


@dataclass
class Comparison:
    fragments: int = 0
    compared: int = 0
    skipped_boundary: int = 0
    killed_mismatch: int = 0
    nan_outputs: int = 0
    over_tolerance: int = 0
    max_error: float = 0.0
    worst_index: int = -1


def compare(
    config: cfg.Config,
    cases: list[Case],
    raw: bytes,
    tolerance: float = TOLERANCE,
    model: models.Model | None = None,
) -> Comparison:
    """Reference model against the GPU's output bytes, fragment by fragment."""
    numbers = struct.unpack(f"<{len(raw) // 4}f", raw)
    result = Comparison(fragments=len(cases))
    for index, case in enumerate(cases):
        expected = reference.evaluate_detailed(config, case.fragment, model=model)
        base = index * OUT_FLOATS
        got = numbers[base : base + 4]
        got_killed = numbers[base + 4] > 0.5
        if any(math.isnan(value) for value in numbers[base : base + 5]):
            result.nan_outputs += 1
            continue
        if expected.near_mux_boundary:
            result.skipped_boundary += 1
            continue
        result.compared += 1
        if got_killed != expected.killed:
            result.killed_mismatch += 1
            continue
        error = max(abs(g - e) for g, e in zip(got, expected.colour, strict=True))
        if error > result.max_error:
            result.max_error = error
            result.worst_index = index
        if error > tolerance:
            result.over_tolerance += 1
    return result


def run_manifest(
    directory: Path, jobs: list[tuple[Path, Path, Path, int]], device: str | None = None
) -> str:
    """Run `vkeval` on (spirv, input, output, count) jobs and return its stdout.

    `device` sets VKRUN_DEVICE for the run (T102/T103: "hardware" or a device-name substring,
    see `tools/nv2a/vkrun_core.c`). None leaves the environment alone."""
    manifest = directory / "manifest.txt"
    manifest.write_text(
        "".join(
            f"{spirv} {inp} {out} {count} {IN_FLOATS} {OUT_FLOATS}\n"
            for spirv, inp, out, count in jobs
        )
    )
    completed = subprocess.run(
        [str(runner_path(directory)), str(manifest)],
        capture_output=True,
        text=True,
        timeout=1500,
        env={**os.environ, "VKRUN_DEVICE": device} if device else None,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"vkeval failed: {completed.stdout}{completed.stderr}")
    return completed.stdout


def validate_configs(
    configs: dict[str, cfg.Config],
    directory: Path,
    *,
    count: int,
    seed: int,
    tolerance: float = TOLERANCE,
    model: models.Model | None = None,
    device: str | None = None,
) -> dict[str, Comparison]:
    """Translate, compile, run and compare every configuration. One batch, one device."""
    build_runner(directory)
    cases = make_cases(seed, count)
    input_path = directory / "fragments.bin"
    input_path.write_bytes(pack_cases(cases))
    jobs = []
    outputs = {}
    for name, config in configs.items():
        spirv = compile_spirv(glsl.compute_shader(config, model), directory, name)
        output = directory / f"{name}.out"
        jobs.append((spirv, input_path, output, count))
        outputs[name] = output
    run_manifest(directory, jobs, device)
    return {
        name: compare(configs[name], cases, outputs[name].read_bytes(), tolerance, model)
        for name in configs
    }
