# SPDX-License-Identifier: GPL-3.0-or-later
"""T100c: compare the `--undo-viewport` vertex stage with a float32 twin on the `corner` vectors.

`vertex_device --undo-viewport` compares `gl_Position` with `viewport.clip_from_window`, a
float64 twin that overflows, loses NaN and inf and treats denormals differently from the
device, so T100 left the `corner` kind out. `viewport.clip_from_window_f32` runs the same
GLSL operations in float32 (and a variant that reads denormal operands as zero, the T100b
cause). This module runs the vertex stage with the inverse on the corner vectors of every
program that has one (`full` and `z-only`), feeds the twin the window position the SAME
device's compute form produces (isolating the inverse) and, for context, the interpreter's
position, and classifies every residual.

A residual float is attributed, in order, to: `input_differs` (the window position fed to the twin
already differs from the one the same vertex stage produces, so the cause is upstream of the
inverse), `denormal_operand` (a source operand of one of the inverse's operations at that lane
is denormal, the T100b known cause), `rounding` (finite, a
divide a few ULP from correctly rounded, beyond the 7.1 tolerance) or `unexplained`.

MEASURED facts name the device (RADV, llvmpipe), never NV2A. INFERRED model: viewport.py.
`python -m tools.nv2a.corner_viewport generated/shaders/corpus/static --device hardware`.
"""

from __future__ import annotations

import argparse
import math
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.nv2a import corner_classify, interp, translate, validate, vertex_device, viewport
from tools.nv2a.validate import TestVector, VkRunner

TWIN_FLOAT64 = viewport.TWIN_FLOAT64
TWIN_FLOAT32 = viewport.TWIN_FLOAT32
TWIN_FLOAT32_FTZ = viewport.TWIN_FLOAT32_FTZ
TWIN_FLOAT32_RCP = viewport.TWIN_FLOAT32_RCP
TWINS = viewport.TWINS
twin_position = viewport.twin_position

SOURCE_VERTEX = "vertex"
SOURCE_COMPUTE = "compute"
SOURCE_INTERPRETER = "interpreter"
SOURCES = (SOURCE_VERTEX, SOURCE_COMPUTE, SOURCE_INTERPRETER)

CAUSE_DENORMAL = "denormal_operand"
CAUSE_UPSTREAM = "input_differs"
CAUSE_ROUNDING = "rounding"
CAUSE_UNEXPLAINED = "unexplained"

#: a finite residual this close (relative) is a divide or reciprocal a few ULP off, not a rule
ROUNDING_BOUND = 2.0**-10
POSITION = vertex_device.POSITION_ADDRESS


@dataclass
class ProgramEvidence:
    """One program's corner vectors: what the device returned and the two window positions."""

    name: str
    shape: str
    vectors: Sequence[TestVector]
    #: `gl_Position` of the vertex stage with the inverse, per vector
    device: Sequence[Sequence[float]]
    #: window position from the same vertex stage without the inverse / from the compute form on
    #: the same device / from the IEEE interpreter
    windows: dict[str, Sequence[Sequence[float]]]


def collect(
    programs: dict[str, bytes],
    *,
    device: str,
    work: Path,
    count: int = 16,
    seed: int = 0,
    kinds: Sequence[str] = (validate.KIND_CORNER,),
    float_controls: bool = True,
    source_hook: Callable[[str], str] | None = None,
) -> list[ProgramEvidence]:
    """Run every program that has a viewport inverse (full or z-only) on the device."""
    vertex_runner = VkRunner(entry=vertex_device.VKVERT_SOURCE, device=device)
    compute_runner = VkRunner(device=device)
    oracle = validate.ModelInterpreter(interp, "ieee")
    evidence = []
    for name, data in programs.items():
        shape = viewport.classify(data)
        if shape not in viewport.GLSL_BODY:
            continue
        translation = translate.translate(data, name=name)
        vectors, _ = validate.build_vectors(data, seed, count, list(kinds))
        source = vertex_device.capture_source(
            translation, undo_viewport=True, float_controls=float_controls
        )
        if source_hook is not None:
            source = source_hook(source)
        raw = vertex_device.run_vertex(
            source, vectors, runner=vertex_runner, work=work, name="corner_viewport"
        )
        plain = vertex_device.run_vertex(
            vertex_device.capture_source(translation, float_controls=float_controls),
            vectors,
            runner=vertex_runner,
            work=work,
            name="corner_viewport_plain",
        )
        compute = validate.run_glsl_vectors(
            translate.compute_test_source(translation),
            vectors,
            work=work,
            runner=compute_runner,
            name="corner_viewport_compute",
        )
        reference = [
            oracle.run(data, vector.inputs, vector.constants, a0=0).outputs for vector in vectors
        ]
        evidence.append(
            ProgramEvidence(
                name,
                shape,
                vectors,
                [list(slots[POSITION][:4]) for slots in raw],
                {
                    SOURCE_VERTEX: [list(slots[POSITION][:4]) for slots in plain],
                    SOURCE_COMPUTE: [list(slots[POSITION][:4]) for slots in compute],
                    SOURCE_INTERPRETER: [list(slots[POSITION][:4]) for slots in reference],
                },
            )
        )
    return evidence


@dataclass(frozen=True)
class Residual:
    program: str
    shape: str
    vector: int
    component: int
    twin: str
    source: str
    expected: float
    device: float
    effect: str
    cause: str


@dataclass
class TwinScore:
    compared: int = 0
    residuals: list[Residual] = field(default_factory=list)
    #: bit census of every compared float against the twin (corner_classify.bit_class)
    bits: Counter[str] = field(default_factory=Counter)


def _denormal(value: float) -> bool:
    return 0.0 < abs(value) < interp.MIN_NORMAL


def denormal_operand(
    shape: str,
    vector: TestVector,
    window: Sequence[float],
    component: int,
) -> bool:
    """True when a source operand of an inverse operation at this lane is denormal. Lanes w
    (and x and y of z-only) are copied through, no operation reads them as a source."""
    if component >= 3 or (shape == viewport.Z_ONLY and component != 2):
        return False
    scale = vector.constants[viewport.SCALE][component]
    offset = vector.constants[viewport.OFFSET][component]
    operands = [window[component], scale, offset]
    if shape == viewport.FULL:
        operands.append(window[3])
    if any(_denormal(value) for value in operands):
        return True
    # the difference (window - offset) feeds the divide, the quotient feeds the multiply
    if scale == 0.0 or not all(math.isfinite(value) for value in operands):
        return False
    difference = validate.round_f32(window[component] - offset)
    quotient = validate.round_f32(difference / scale)
    return _denormal(difference) or (shape == viewport.FULL and _denormal(quotient))


def _cause(
    item: ProgramEvidence,
    index: int,
    component: int,
    source: str,
    expected: float,
    device: float,
) -> str:
    vector = item.vectors[index]
    own = item.windows[SOURCE_VERTEX][index]
    if any(
        validate.classify(item.windows[source][index][lane], own[lane]) is not None
        for lane in range(4)
    ):
        return CAUSE_UPSTREAM
    if denormal_operand(item.shape, vector, item.windows[source][index], component):
        return CAUSE_DENORMAL
    if (
        math.isfinite(expected)
        and math.isfinite(device)
        and abs(expected - device) <= ROUNDING_BOUND * max(abs(expected), abs(device))
    ):
        return CAUSE_ROUNDING
    return CAUSE_UNEXPLAINED


def score(evidence: Sequence[ProgramEvidence], twin: str, source: str) -> TwinScore:
    """Compare the device with `twin` fed by `source`, over every float of every vector."""
    result = TwinScore()
    for item in evidence:
        for index, vector in enumerate(item.vectors):
            expected = twin_position(
                twin,
                item.shape,
                item.windows[source][index],
                vector.constants[viewport.SCALE],
                vector.constants[viewport.OFFSET],
            )
            for component in range(4):
                want, got = expected[component], item.device[index][component]
                result.compared += 1
                result.bits[corner_classify.bit_class(want, got)] += 1
                if validate.classify(want, got) is not None:
                    result.residuals.append(
                        Residual(
                            item.name,
                            item.shape,
                            index,
                            component,
                            twin,
                            source,
                            want,
                            got,
                            corner_classify.effect_class(want, got),
                            _cause(item, index, component, source, want, got),
                        )
                    )
    return result


def render(evidence: Sequence[ProgramEvidence], device: str) -> str:
    shapes = Counter(item.shape for item in evidence)
    vectors = sum(len(item.vectors) for item in evidence)
    lines = [
        f"device: {device}",
        f"programs: {len(evidence)} {dict(shapes)}   corner vectors: {vectors}   "
        f"floats per twin and source: {4 * vectors}",
        "",
        f"{'twin':<16}{'source':<13}{'residual':>9}  by cause   |   bit census",
    ]
    for source in SOURCES:
        for twin in TWINS:
            result = score(evidence, twin, source)
            causes = Counter(residual.cause for residual in result.residuals)
            lines.append(
                f"{twin:<16}{source:<13}{len(result.residuals):>9}  {dict(causes)}   |   "
                f"{dict(result.bits)}"
            )
    for source in SOURCES:
        result = score(evidence, TWIN_FLOAT32_FTZ, source)
        effects = Counter(residual.effect for residual in result.residuals)
        programs = sorted({residual.program[:12] for residual in result.residuals})
        lines.append(
            f"\nfloat32-ftz residuals fed by {source}: effects {dict(effects)}, "
            f"{len(programs)} programs {programs}"
        )
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.corner_viewport",
        description=(
            "Compare the undo-viewport vertex stage with the float32 twin on corner vectors."
        ),
    )
    parser.add_argument("programs", nargs="+", help="microcode .bin files or directories")
    parser.add_argument("--device", default="hardware")
    parser.add_argument("--count", type=int, default=16, help="corner vectors per program")
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args(argv)
    reason = vertex_device.device_unavailable_reason(args.device)
    if reason is not None:
        print(f"unavailable: {reason}", file=sys.stderr)
        return 3
    programs = vertex_device._programs(args.programs)
    with tempfile.TemporaryDirectory(prefix="nv2a-corner-viewport-") as scratch:
        evidence = collect(
            programs,
            device=args.device,
            work=Path(scratch),
            count=args.count,
            seed=args.seed,
        )
    print(render(evidence, vertex_device.device_name(args.device)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
