# SPDX-License-Identifier: GPL-3.0-or-later
"""T98: run the NV2A arithmetic corner cases through the translated SPIR-V on every Vulkan
device available (RADV and llvmpipe here) and record what each does against what the
reference interpreter assumes.

An AMD GPU is NOT NV2A hardware. This is a SECOND SOURCE, not ground truth: it can show
where the interpreter's NV rules (`interp.SPEC_MODEL`, from the GL_NV_vertex_program text)
differ from what a shipping IEEE-754 GPU does with the translator's output, and where two
devices disagree with each other (so the translator's output is not a portable oracle for
that corner). It cannot say what NV2A silicon does.

Corners, one probe program each (built from the same microcode encoder the directed set uses):
  * zero_times        `0 * x = +0` for any x, including inf and NaN (MUL, MAD, DP4)
  * denormal          denormal inputs read as zero (ADD, MUL, MOV, RSQ, LOG)
  * signed_zero_order `-0 < +0` and NaN beyond the infinities in SLT/SGE; MIN/MAX of +-0
  * arl_rounding      ARL float to integer: floor (NV) vs nearest (Microsoft pages)
  * log_zero          LOG(0): z is -inf (NV) or -FLT_MAX (D3D page)
  * zero_special      RCP/RCC/RSQ of +-0 and LIT with a zero power

Each case is compared with five interpreter references (SPEC, IEEE, and each single NV rule
switched on alone) plus per-corner alternatives (D3D LOG, other ARL roundings), and the result
is named by every reference it matches. The denormal corner is also run with the SPIR-V
DenormFlushToZero and DenormPreserve execution modes added, where the device reports them,
because the translator sets neither and the default is then device defined.

`--corpus DIR` additionally finds which of the title's static programs depend on `0 * x`
(interpreter only, same vectors as docs/vertex-translator.md 7.4) and runs those on the
devices, so the effect of the corner on the real programs is measured per device.

    python -m tools.nv2a.corners --report generated/shaders/corners/report.json
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from tools.nv2a import directed, interp, isa, translate
from tools.nv2a.validate import (
    DEFAULT_BIN_DIR,
    DEFAULT_WORK_DIR,
    LARGEST_DENORMAL,
    RTOL,
    SMALLEST_NORMAL,
    Job,
    RunnerUnavailable,
    TestVector,
    ValidateError,
    VkRunner,
    build_vectors,
    compile_glsl,
    floats_equal,
    pack_vectors,
    read_outputs,
    round_f32,
)

Row = tuple[float, float, float, float]

DEFAULT_DEVICES = ("llvmpipe", "RADV")
VARIANT_DEFAULT = "default"
VARIANT_FLUSH = "ftz"
VARIANT_PRESERVE = "preserve"
VARIANTS = (VARIANT_DEFAULT, VARIANT_FLUSH, VARIANT_PRESERVE)
#: SPIR-V (SPV_KHR_float_controls) capability and execution mode numbers, 32-bit float.
_DENORM_LINES = {
    VARIANT_FLUSH: 'spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], '
    "capabilities = [4465], 4460, 32);",
    VARIANT_PRESERVE: 'spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], '
    "capabilities = [4464], 4459, 32);",
}
_DENORM_SUPPORT_KEY = {
    VARIANT_FLUSH: "shaderDenormFlushToZeroFloat32",
    VARIANT_PRESERVE: "shaderDenormPreserveFloat32",
}
_EXECUTION_MODE_ANCHOR = "capabilities = [4466], 4461, 32);"

CORNER_ZERO_TIMES = "zero_times"
CORNER_DENORMAL = "denormal"
CORNER_ORDER = "signed_zero_order"
CORNER_ARL = "arl_rounding"
CORNER_LOG = "log_zero"
CORNER_SPECIAL = "zero_special"
CORNERS = (
    CORNER_ZERO_TIMES,
    CORNER_DENORMAL,
    CORNER_ORDER,
    CORNER_ARL,
    CORNER_LOG,
    CORNER_SPECIAL,
)

#: The interpreter references every result is named against. Order is the report order.
REFERENCE_MODELS: dict[str, interp.Model] = {
    "spec": interp.SPEC_MODEL,
    "ieee": interp.IEEE_MODEL,
    "zero_only": interp.Model(True, False, False),
    "flush_only": interp.Model(False, True, False),
    "order_only": interp.Model(False, False, True),
}

INF = math.inf
NAN = math.nan
DENORMAL = round_f32(2.0**-130)
LARGEST_FLOAT = round_f32(3.4028234663852886e38)
RELATIVE_BASE = directed.RELATIVE_BASE
ARL_WINDOW = range(80, 113)
INT32_LIMIT = 2**31

MUL_INPUT = 1
CONSTANT_B = 96
CONSTANT_C = 97
INPUT_ALL_LANES = (0, 1, 2, 3)


class CornersError(ValidateError):
    """A harness-level problem with the corner probe."""


# ----------------------------------------------------------------------------- floats


def format_float(value: float) -> str:
    """Stable text for a float: `repr` keeps the sign of zero and spells inf and nan."""
    return repr(float(value))


def same(expected: float, actual: float) -> bool:
    """Float equality for this probe. NaN equals NaN (the payload is not compared), signed
    zeros differ, a denormal and a zero differ (that is the question), finite normals match
    within the translator's documented tolerance because GPU log2 and exp2 are approximate."""
    if math.isnan(expected) or math.isnan(actual):
        return math.isnan(expected) and math.isnan(actual)
    if expected == 0.0 and actual == 0.0:
        return math.copysign(1.0, expected) == math.copysign(1.0, actual)
    if math.isinf(expected) or math.isinf(actual) or expected == 0.0 or actual == 0.0:
        return expected == actual
    if abs(expected) < SMALLEST_NORMAL or abs(actual) < SMALLEST_NORMAL:
        return expected == actual
    return abs(expected - actual) <= RTOL * max(abs(expected), abs(actual))


def rows_same(expected: Sequence[float], actual: Sequence[float], lanes: Sequence[int]) -> bool:
    return all(same(expected[lane], actual[lane]) for lane in lanes)


# ------------------------------------------------------------------------------ probes


@dataclass(frozen=True)
class Case:
    """One execution of a probe program. `lanes` are the o0 components that are compared."""

    corner: str
    label: str
    inputs: Mapping[int, Row]
    constants: Mapping[int, Row]
    lanes: tuple[int, ...] = (0,)
    #: extra named references, each a full o0 row (D3D LOG, other ARL roundings)
    alternatives: Mapping[str, Row] = field(default_factory=dict)

    def vector(self) -> TestVector:
        zero: Row = (0.0, 0.0, 0.0, 0.0)
        inputs = tuple(self.inputs.get(index, zero) for index in range(16))
        constants = tuple(self.constants.get(index, zero) for index in range(192))
        return TestVector("corner", inputs, constants)


@dataclass(frozen=True)
class Probe:
    name: str
    program: bytes
    cases: tuple[Case, ...]


def _splat(value: float) -> Row:
    return (value, value, value, value)


def _binary_case(corner: str, a: float, b: float, label: str | None = None) -> Case:
    text = label or f"{format_float(a)} , {format_float(b)}"
    return Case(
        corner,
        text,
        {MUL_INPUT: _splat(a)},
        {CONSTANT_B: _splat(b), CONSTANT_C: _splat(b)},
    )


def _output_of(mac: int = 0, ilu: int = 0, **fields: Any) -> tuple[int, int, int, int]:
    """A one-instruction body writing o0. A MAC result by default, the ILU one when `ilu`."""
    return directed._instruction(
        mac=mac, ilu=ilu, out_mask=0xF, out_is_ilu=bool(ilu and not mac), **fields
    )


def _mac_program(mac: int, *, third_from_temp: bool = False) -> bytes:
    """`o0 = MAC(v1, c96, ...)`. ADD reads A and C, so its C comes from the constant file."""
    if mac == 3:
        body = _output_of(
            mac=mac,
            a=(isa.MUX_INPUT, 0, 0x1B, 0),
            c=(isa.MUX_CONST, 0, 0x1B, 0),
            input_index=MUL_INPUT,
            const=CONSTANT_B,
        )
        return directed.pack([body])
    if third_from_temp:
        load = directed._instruction(
            mac=1,
            a=(isa.MUX_CONST, 0, 0x1B, 0),
            const=CONSTANT_C,
            temp_out=3,
            mac_mask=0xF,
        )
        body = _output_of(
            mac=mac,
            a=(isa.MUX_INPUT, 0, 0x1B, 0),
            b=(isa.MUX_CONST, 0, 0x1B, 0),
            c=(isa.MUX_TEMP, 3, 0x1B, 0),
            input_index=MUL_INPUT,
            const=CONSTANT_B,
        )
        return directed.pack([load, body])
    body = _output_of(
        mac=mac,
        a=(isa.MUX_INPUT, 0, 0x1B, 0),
        b=(isa.MUX_CONST, 0, 0x1B, 0),
        input_index=MUL_INPUT,
        const=CONSTANT_B,
    )
    return directed.pack([body])


def _ilu_program(ilu: int, swizzle: int = 0x00) -> bytes:
    """`o0 = ILU(v1.<swizzle>)`: the scalar unit reads the C operand's x after swizzling."""
    body = _output_of(
        ilu=ilu,
        c=(isa.MUX_INPUT, 0, swizzle, 0),
        input_index=MUL_INPUT,
    )
    return directed.pack([body])


def _arl_program() -> bytes:
    load = directed._instruction(mac=isa.MAC_ARL, a=(isa.MUX_INPUT, 0, 0x00, 0), input_index=0)
    read = directed._instruction(
        ilu=1,
        c=(isa.MUX_CONST, 0, 0x1B, 0),
        const=RELATIVE_BASE,
        relative=True,
        out_mask=0xF,
        out_is_ilu=True,
    )
    return directed.pack([load, read])


def _mul_cases() -> list[Case]:
    pairs = [
        (0.0, INF),
        (INF, 0.0),
        (-0.0, INF),
        (0.0, -INF),
        (0.0, NAN),
        (NAN, 0.0),
        (0.0, -5.0),
        (-0.0, 5.0),
        (0.0, LARGEST_FLOAT),
        (0.0, 0.0),
        (2.0, 3.0),
        (0.0, 7.0),
        (INF, 2.0),
    ]
    return [_binary_case(CORNER_ZERO_TIMES, a, b) for a, b in pairs]


def _mad_cases() -> list[Case]:
    pairs = [(0.0, INF), (INF, 0.0), (0.0, NAN), (2.0, 3.0)]
    return [
        Case(
            CORNER_ZERO_TIMES,
            f"{format_float(a)} * {format_float(b)} + 1.0",
            {MUL_INPUT: _splat(a)},
            {CONSTANT_B: _splat(b), CONSTANT_C: _splat(1.0)},
        )
        for a, b in pairs
    ]


def _dp4_cases() -> list[Case]:
    rows = [
        ((0.0, 1.0, 2.0, 3.0), (INF, 1.0, 1.0, 1.0)),
        ((0.0, 1.0, 2.0, 3.0), (NAN, 1.0, 1.0, 1.0)),
        ((1.0, 2.0, 3.0, 4.0), (1.0, 1.0, 1.0, 1.0)),
    ]
    return [
        Case(
            CORNER_ZERO_TIMES,
            f"dp4 {tuple(map(format_float, a))} . {tuple(map(format_float, b))}",
            {MUL_INPUT: a},
            {CONSTANT_B: b},
            lanes=INPUT_ALL_LANES,
        )
        for a, b in rows
    ]


def _denormal_binary_cases() -> list[Case]:
    return [
        _binary_case(CORNER_DENORMAL, DENORMAL, 2.0**100, "mul d * 2^100"),
        _binary_case(CORNER_DENORMAL, DENORMAL, 1.0, "mul d * 1"),
        _binary_case(CORNER_DENORMAL, -DENORMAL, 1.0, "mul -d * 1"),
        _binary_case(
            CORNER_DENORMAL, SMALLEST_NORMAL, 0.5, "mul min_normal * 0.5 (denormal result)"
        ),
        _binary_case(CORNER_DENORMAL, LARGEST_DENORMAL, 2.0**100, "mul largest_denormal * 2^100"),
    ]


def _denormal_add_cases() -> list[Case]:
    return [
        _binary_case(CORNER_DENORMAL, DENORMAL, 0.0, "add d + 0"),
        _binary_case(CORNER_DENORMAL, DENORMAL, DENORMAL, "add d + d"),
        _binary_case(CORNER_DENORMAL, DENORMAL, 1.0, "add d + 1"),
        _binary_case(
            CORNER_DENORMAL, LARGEST_DENORMAL, SMALLEST_NORMAL, "add largest_denormal + min_normal"
        ),
    ]


def _denormal_unary_cases(label: str) -> list[Case]:
    return [
        Case(
            CORNER_DENORMAL,
            f"{label}({name})",
            {MUL_INPUT: _splat(value)},
            {},
            lanes=INPUT_ALL_LANES if label == "log" else (0,),
        )
        for name, value in (("d", DENORMAL), ("largest_denormal", LARGEST_DENORMAL))
    ]


def _compare_cases(pairs: Sequence[tuple[float, float]]) -> list[Case]:
    return [_binary_case(CORNER_ORDER, a, b) for a, b in pairs]


def _order_pairs() -> list[tuple[float, float]]:
    return [
        (-0.0, 0.0),
        (0.0, -0.0),
        (-0.0, -0.0),
        (0.0, 0.0),
        (NAN, INF),
        (INF, NAN),
        (-INF, NAN),
        (NAN, -INF),
        (1.0, 2.0),
        (2.0, 1.0),
    ]


def _arl_cases() -> list[Case]:
    values = [
        0.5,
        1.5,
        2.5,
        3.5,
        -0.5,
        -1.5,
        -2.5,
        round_f32(0.49999999),
        round_f32(2.9999998),
        round_f32(-0.0000001),
        0.75,
        -0.75,
        1.0,
        -1.0,
        0.0,
        -0.0,
    ]
    constants = {index: _splat(float(index)) for index in ARL_WINDOW}
    cases = []
    for value in values:
        alternatives = {
            name: _splat(float(RELATIVE_BASE + rounder(value)))
            for name, rounder in ARL_ROUNDERS.items()
            if name != "floor"
        }
        cases.append(
            Case(
                CORNER_ARL,
                f"arl {format_float(value)}",
                {0: _splat(value)},
                constants,
                alternatives=alternatives,
            )
        )
    return cases


def _round_half_up(value: float) -> int:
    return math.floor(value + 0.5)


#: ARL conversions to try against the observed constant index. `floor` is the NV text.
ARL_ROUNDERS: dict[str, Callable[[float], int]] = {
    "floor": math.floor,
    "nearest_even": lambda value: round(value),
    "nearest_half_up": _round_half_up,
    "truncate": math.trunc,
    "ceil": math.ceil,
}


def _log_cases() -> list[Case]:
    cases = []
    for name, value in (
        ("0", 0.0),
        ("-0", -0.0),
        ("1", 1.0),
        ("8", 8.0),
        ("0.5", 0.5),
        ("-4", -4.0),
        ("3", 3.0),
        ("inf", INF),
        ("nan", NAN),
    ):
        spec = interp.run(
            _ilu_program(6), _inputs_with(value), _empty_constants(), model=interp.SPEC_MODEL
        ).outputs[0]
        d3d = [spec[0], spec[1], spec[2], spec[3]]
        if d3d[2] == -INF:
            d3d[2] = -LARGEST_FLOAT
        cases.append(
            Case(
                CORNER_LOG,
                f"log({name})",
                {MUL_INPUT: _splat(value)},
                {},
                lanes=INPUT_ALL_LANES,
                alternatives={"d3d": (d3d[0], d3d[1], d3d[2], d3d[3])},
            )
        )
    return cases


def _special_cases(
    label: str, program: bytes, values: Sequence[tuple[str, float]], *, d3d_flt_max: bool = False
) -> list[Case]:
    """Scalar-unit cases. With `d3d_flt_max` the D3D pseudo-code answer (the largest finite
    float where NV gives an infinity) is recorded as an alternative reference."""
    cases = []
    for name, value in values:
        alternatives: dict[str, Row] = {}
        if d3d_flt_max:
            spec = interp.run(
                program, _inputs_with(value), _empty_constants(), model=interp.SPEC_MODEL
            ).outputs[0]
            alternatives["d3d_flt_max"] = tuple(  # type: ignore[assignment]
                math.copysign(LARGEST_FLOAT, x) if math.isinf(x) else x for x in spec
            )
        cases.append(
            Case(
                CORNER_SPECIAL,
                f"{label}({name})",
                {MUL_INPUT: _splat(value)},
                {},
                alternatives=alternatives,
            )
        )
    return cases


def _lit_cases() -> list[Case]:
    rows = [
        ("diffuse 1, specular 0, power 0", (1.0, 0.0, 0.0, 0.0)),
        ("diffuse 1, specular 0, power 2", (1.0, 0.0, 0.0, 2.0)),
        ("diffuse 1, specular 0.5, power 0", (1.0, 0.5, 0.0, 0.0)),
        ("diffuse 1, specular 0.5, power 2", (1.0, 0.5, 0.0, 2.0)),
        ("diffuse 0, specular 0.5, power 2", (0.0, 0.5, 0.0, 2.0)),
    ]
    return [
        Case(
            CORNER_SPECIAL,
            f"lit({text})",
            {MUL_INPUT: row},
            {},
            lanes=INPUT_ALL_LANES,
        )
        for text, row in rows
    ]


def _inputs_with(value: float) -> list[Row]:
    inputs: list[Row] = [(0.0, 0.0, 0.0, 0.0)] * 16
    inputs[MUL_INPUT] = _splat(value)
    return inputs


def _empty_constants() -> list[Row]:
    return [(0.0, 0.0, 0.0, 0.0)] * 192


_RCP = _ilu_program(2)
_RCC = _ilu_program(3)
_RSQ = _ilu_program(4)


def build_probes() -> list[Probe]:
    """Every probe, in a fixed order. Programs are plain NV2A microcode."""
    return [
        Probe("mul", _mac_program(2), tuple(_mul_cases() + _denormal_binary_cases())),
        Probe("mad", _mac_program(4, third_from_temp=True), tuple(_mad_cases())),
        Probe("dp4", _mac_program(7), tuple(_dp4_cases())),
        Probe("add", _mac_program(3), tuple(_denormal_add_cases())),
        Probe("mov", _mac_program(1), tuple(_denormal_unary_cases("mov"))),
        Probe("rsq", _RSQ, tuple(_denormal_unary_cases("rsq"))),
        Probe("slt", _mac_program(11), tuple(_compare_cases(_order_pairs()))),
        Probe("sge", _mac_program(12), tuple(_compare_cases(_order_pairs()))),
        Probe("min", _mac_program(9), tuple(_compare_cases(_order_pairs()[:4]))),
        Probe("max", _mac_program(10), tuple(_compare_cases(_order_pairs()[:4]))),
        Probe("arl", _arl_program(), tuple(_arl_cases())),
        Probe("log", _ilu_program(6), tuple(_log_cases() + _denormal_unary_cases("log"))),
        Probe(
            "rcp",
            _RCP,
            tuple(_special_cases("rcp", _RCP, (("0", 0.0), ("-0", -0.0)), d3d_flt_max=True)),
        ),
        Probe("rcc", _RCC, tuple(_special_cases("rcc", _RCC, (("0", 0.0), ("-0", -0.0))))),
        Probe(
            "rsq_zero",
            _RSQ,
            tuple(
                _special_cases(
                    "rsq", _RSQ, (("0", 0.0), ("-0", -0.0), ("-4", -4.0)), d3d_flt_max=True
                )
            ),
        ),
        Probe("lit", _ilu_program(7, 0x1B), tuple(_lit_cases())),
    ]


# --------------------------------------------------------------------------- execution


class Executor(Protocol):
    """Runs one probe program over test vectors in one denormal variant and returns the
    16 x 4 outputs per vector. Raises `CornersError` for a program the device refused."""

    def run(
        self, name: str, source: str, vectors: Sequence[TestVector], variant: str
    ) -> list[tuple[Row, ...]]: ...


def variant_source(source: str, variant: str) -> str:
    """The translator's compute shader with a denormal execution mode added."""
    if variant == VARIANT_DEFAULT:
        return source
    if _EXECUTION_MODE_ANCHOR not in source:
        raise CornersError("the translator's execution-mode line moved, update corners.py")
    return source.replace(
        _EXECUTION_MODE_ANCHOR, _EXECUTION_MODE_ANCHOR + "\n" + _DENORM_LINES[variant], 1
    )


class DeviceExecutor:
    """The real thing: glslang then `vkrun` on the device named by `VKRUN_DEVICE`."""

    def __init__(self, runner: VkRunner, work: Path, spirv_cache: dict[str, Path]) -> None:
        self.runner = runner
        self.work = work
        self.spirv_cache = spirv_cache

    def run(
        self, name: str, source: str, vectors: Sequence[TestVector], variant: str
    ) -> list[tuple[Row, ...]]:
        stem = f"{name}.{variant}"
        spirv = self.spirv_cache.get(stem)
        if spirv is None:
            spirv = self.work / f"{stem}.spv"
            glsl = self.work / f"{stem}.comp"
            glsl.write_text(variant_source(source, variant))
            compile_glsl(glsl, spirv)
            self.spirv_cache[stem] = spirv
        inputs = self.work / f"{stem}.{self.runner.device}.in"
        output = self.work / f"{stem}.{self.runner.device}.out"
        inputs.write_bytes(pack_vectors(vectors))
        (result,) = self.runner.run_batch([Job(spirv, inputs, output, len(vectors))])
        if not result.ok:
            raise CornersError(result.reason or "the runner failed the job")
        return read_outputs(output, len(vectors))


def device_info(runner: VkRunner) -> dict[str, str]:
    """`vkrun --info` as a dict. `RunnerUnavailable` when the device is not there."""
    info = runner.info()
    if not info.get("device"):
        raise RunnerUnavailable("the runner reported no device")
    return info


def unavailable_reason(device: str, bin_dir: Path = DEFAULT_BIN_DIR) -> str | None:
    """None when glslang, a C compiler and the named Vulkan device all work, else why not."""
    if shutil.which("glslangValidator") is None:
        return "glslangValidator not found"
    try:
        device_info(VkRunner(bin_dir=bin_dir, device=device))
    except (RunnerUnavailable, OSError) as error:
        return f"no Vulkan device matching {device!r}: {error}"
    except Exception as error:  # a timeout or a crash of vkrun: skip with the reason
        return f"Vulkan device {device!r} could not be probed: {error}"
    return None


# ------------------------------------------------------------------------ the analysis


def _flush_denormal(value: float) -> float:
    return math.copysign(0.0, value) if 0.0 < abs(value) < SMALLEST_NORMAL else value


def references_for(case: Case, program: bytes) -> dict[str, Row]:
    """The interpreter's o0 row under each model, plus the case's own alternatives."""
    vector = case.vector()
    references: dict[str, Row] = {}
    for name, model in REFERENCE_MODELS.items():
        outputs = interp.run(program, vector.inputs, vector.constants, model=model).outputs
        references[name] = tuple(outputs[0])  # type: ignore[assignment]
    references.update(case.alternatives)
    flushed = tuple(_flush_denormal(x) for x in references["spec"])
    if any(not same(a, b) for a, b in zip(references["spec"], flushed, strict=True)):
        references["ftz_result"] = flushed  # type: ignore[assignment]
    return references


def matches(references: Mapping[str, Row], row: Sequence[float], lanes: Sequence[int]) -> list[str]:
    return [name for name, reference in references.items() if rows_same(reference, row, lanes)]


def is_discriminating(references: Mapping[str, Row], lanes: Sequence[int]) -> bool:
    """True when at least two references disagree, so the case can tell them apart."""
    rows = list(references.values())
    return any(not rows_same(rows[0], other, lanes) for other in rows[1:])


def class_name(names: Sequence[str]) -> str:
    return "+".join(names) if names else "neither"


def arl_fit(offsets: Sequence[int | None], values: Sequence[float]) -> list[str]:
    """Which ARL conversions explain EVERY observed index offset. Empty when none do."""
    if not offsets or len(offsets) != len(values):
        return []
    return [
        name
        for name, rounder in ARL_ROUNDERS.items()
        if all(
            offset is not None and offset == rounder(value)
            for offset, value in zip(offsets, values, strict=True)
        )
    ]


@dataclass
class DeviceRun:
    """One device (or a stand-in) with its identity and the variants it supports."""

    label: str
    info: dict[str, str]
    executor: Executor
    variants: tuple[str, ...] = (VARIANT_DEFAULT,)


def supported_variants(info: Mapping[str, str], requested: Sequence[str]) -> tuple[str, ...]:
    """The requested variants the device claims to support. `default` is always run."""
    chosen = [VARIANT_DEFAULT]
    for variant in requested:
        key = _DENORM_SUPPORT_KEY.get(variant)
        if key is not None and info.get(key) == "1" and variant not in chosen:
            chosen.append(variant)
    return tuple(chosen)


def run_probes(
    devices: Sequence[DeviceRun], probes: Sequence[Probe] | None = None
) -> dict[str, Any]:
    """Run every probe on every device and variant and build the report."""
    probes = list(probes if probes is not None else build_probes())
    keys = [(device.label, variant) for device in devices for variant in device.variants]
    cases_report: list[dict[str, Any]] = []
    failures: dict[str, str] = {}
    for probe in probes:
        translation = translate.translate(probe.program, name=probe.name)
        source = translate.compute_test_source(translation)
        vectors = [case.vector() for case in probe.cases]
        rows_by_key: dict[str, list[tuple[Row, ...]]] = {}
        for device in devices:
            for variant in device.variants:
                key = f"{device.label}:{variant}"
                try:
                    rows_by_key[key] = device.executor.run(probe.name, source, vectors, variant)
                except ValidateError as error:
                    failures[f"{key}:{probe.name}"] = str(error)[:300]
        for index, case in enumerate(probe.cases):
            references = references_for(case, probe.program)
            discriminating = is_discriminating(references, case.lanes)
            results: dict[str, Any] = {}
            for key, rows in rows_by_key.items():
                row = rows[index][0]
                results[key] = {
                    "row": [format_float(x) for x in row],
                    "matches": class_name(matches(references, row, case.lanes)),
                }
            cases_report.append(
                {
                    "corner": case.corner,
                    "probe": probe.name,
                    "label": case.label,
                    "lanes": list(case.lanes),
                    "discriminating": discriminating,
                    "references": {
                        name: [format_float(x) for x in row] for name, row in references.items()
                    },
                    "results": results,
                    "_row_floats": {
                        key: tuple(rows[index][0]) for key, rows in rows_by_key.items()
                    },
                }
            )
    report = {
        "devices": {
            device.label: {"info": device.info, "variants": list(device.variants)}
            for device in devices
        },
        "keys": [f"{label}:{variant}" for label, variant in keys],
        "failures": failures,
        "cases": cases_report,
    }
    report["summary"] = summarise(report)
    report["disagreements"] = disagreements(report)
    report["arl_fit"] = arl_fits(report, probes)
    for case in cases_report:
        del case["_row_floats"]
    return report


def summarise(report: Mapping[str, Any]) -> dict[str, dict[str, dict[str, int]]]:
    """corner -> device:variant -> how many DISCRIMINATING cases fell in each class, plus
    `cases` (all) and `discriminating` totals under the pseudo key `_totals`."""
    summary: dict[str, dict[str, dict[str, int]]] = {}
    for corner in CORNERS:
        in_corner = [c for c in report["cases"] if c["corner"] == corner]
        entry: dict[str, dict[str, int]] = {
            "_totals": {
                "cases": len(in_corner),
                "discriminating": sum(1 for c in in_corner if c["discriminating"]),
            }
        }
        for key in report["keys"]:
            counts: Counter[str] = Counter()
            for case in in_corner:
                if case["discriminating"] and key in case["results"]:
                    counts[case["results"][key]["matches"]] += 1
            entry[key] = dict(counts)
        summary[corner] = entry
    return summary


def disagreements(report: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Cases where two devices (default variant) differ, and where a variant differs from the
    default of the same device. Each entry names the keys and shows the rows."""
    found: list[dict[str, Any]] = []
    for case in report["cases"]:
        rows = case["_row_floats"]
        lanes = case["lanes"]
        defaults = [key for key in rows if key.endswith(f":{VARIANT_DEFAULT}")]
        for position, first in enumerate(defaults):
            for second in defaults[position + 1 :]:
                if not rows_same(rows[first], rows[second], lanes):
                    found.append(_disagreement("device", case, first, second))
        for key in rows:
            device, _, variant = key.rpartition(":")
            default_key = f"{device}:{VARIANT_DEFAULT}"
            if variant != VARIANT_DEFAULT and default_key in rows:
                if not rows_same(rows[default_key], rows[key], lanes):
                    found.append(_disagreement("variant", case, default_key, key))
    return found


def _disagreement(kind: str, case: Mapping[str, Any], first: str, second: str) -> dict[str, Any]:
    return {
        "kind": kind,
        "corner": case["corner"],
        "probe": case["probe"],
        "label": case["label"],
        "between": [first, second],
        "rows": [case["results"][first]["row"], case["results"][second]["row"]],
    }


def arl_fits(report: Mapping[str, Any], probes: Sequence[Probe]) -> dict[str, list[str]]:
    """Per key, the ARL conversions that explain every observed relative-read index."""
    probe = next((p for p in probes if p.name == "arl"), None)
    if probe is None:
        return {}
    values = [case.inputs[0][0] for case in probe.cases]
    arl_cases = [c for c in report["cases"] if c["probe"] == "arl"]
    fits: dict[str, list[str]] = {}
    for key in report["keys"]:
        offsets: list[int | None] = []
        for case in arl_cases:
            observed = case["_row_floats"].get(key)
            value = observed[0] if observed is not None else NAN
            offsets.append(int(value) - RELATIVE_BASE if math.isfinite(value) else None)
        fits[key] = arl_fit(offsets, values) if len(offsets) == len(values) else []
    return fits


# -------------------------------------------------------------------------- the corpus


def zero_rule_dependents(
    programs: Mapping[str, bytes],
    *,
    count: int = 16,
    seed: int = 0,
    kinds: Sequence[str] = ("ordinary", "matrix"),
) -> dict[str, dict[str, Any]]:
    """Per program, what changes when ONLY the `0 * x = +0` rule is switched on, on the same
    vector kinds and counts as docs/vertex-translator.md 7.4 (interpreter only).

    `value_floats` counts floats that differ numerically (what `validate.floats_equal`
    calls different: a NaN where a number is expected, a different magnitude), and
    `sign_only_floats` those that differ only in the sign of a zero. A program DEPENDS on
    the rule when `value_floats` is nonzero, which is the docs' "6 of 46"."""
    found: dict[str, dict[str, Any]] = {}
    for name, data in programs.items():
        vectors, _ = build_vectors(data, seed, count, kinds)
        value: Counter[int] = Counter()
        sign_only = 0
        for vector in vectors:
            plain = interp.run(data, vector.inputs, vector.constants, model=interp.IEEE_MODEL)
            zero = interp.run(
                data, vector.inputs, vector.constants, model=REFERENCE_MODELS["zero_only"]
            )
            for slot in range(16):
                for lane in INPUT_ALL_LANES:
                    a, b = plain.outputs[slot][lane], zero.outputs[slot][lane]
                    if not floats_equal(a, b):
                        value[slot] += 1
                    elif not same(a, b):
                        sign_only += 1
        found[name] = {
            "vectors": len(vectors),
            "value_floats": sum(value.values()),
            "sign_only_floats": sign_only,
            "value_output_slots": {
                isa.OUTPUT_NAMES.get(slot, f"o{slot}"): n for slot, n in sorted(value.items())
            },
        }
    return found


def run_dependents(
    programs: Mapping[str, bytes],
    names: Sequence[str],
    devices: Sequence[DeviceRun],
    *,
    count: int = 16,
    seed: int = 0,
) -> dict[str, Any]:
    """The named programs on every device (default variant): per device, how many floats
    where SPEC and IEEE differ numerically fall on SPEC, on IEEE, or on neither."""
    report: dict[str, Any] = {}
    for name in names:
        data = programs[name]
        vectors, _ = build_vectors(data, seed, count, ("ordinary", "matrix"))
        source = translate.compute_test_source(translate.translate(data, name=name[:12]))
        spec = [
            interp.run(data, v.inputs, v.constants, model=interp.SPEC_MODEL).outputs
            for v in vectors
        ]
        ieee = [
            interp.run(data, v.inputs, v.constants, model=interp.IEEE_MODEL).outputs
            for v in vectors
        ]
        entry: dict[str, Any] = {"floats_where_models_differ": 0, "devices": {}}
        for device in devices:
            counts: Counter[str] = Counter()
            try:
                rows = device.executor.run(f"corpus_{name[:12]}", source, vectors, VARIANT_DEFAULT)
            except ValidateError as error:
                entry["devices"][device.label] = {"error": str(error)[:300]}
                continue
            total = 0
            for vector_index in range(len(vectors)):
                for slot in range(16):
                    for lane in INPUT_ALL_LANES:
                        a, b = spec[vector_index][slot][lane], ieee[vector_index][slot][lane]
                        if floats_equal(a, b):
                            continue
                        total += 1
                        actual = rows[vector_index][slot][lane]
                        hit = [
                            label
                            for label, expected in (("spec", a), ("ieee", b))
                            if floats_equal(expected, actual)
                        ]
                        counts[class_name(hit)] += 1
            entry["floats_where_models_differ"] = total
            entry["devices"][device.label] = dict(counts)
        report[name] = entry
    return report


# ------------------------------------------------------------------------------- output


def render_text(report: Mapping[str, Any]) -> str:
    lines: list[str] = []
    for label, device in report["devices"].items():
        info = device["info"]
        lines.append(
            f"device {label}: {info.get('device', '?')} driver={info.get('driver', '?')} "
            f"denorm_ftz={info.get('shaderDenormFlushToZeroFloat32', '?')} "
            f"denorm_preserve={info.get('shaderDenormPreserveFloat32', '?')} "
            f"variants={','.join(device['variants'])}"
        )
    for key, reason in report["failures"].items():
        lines.append(f"FAILED {key}: {reason}")
    lines.append("")
    lines.append("discriminating cases per corner, by what each device result matches:")
    for corner in CORNERS:
        entry = report["summary"][corner]
        totals = entry["_totals"]
        lines.append(
            f"  {corner}: {totals['discriminating']} of {totals['cases']} cases discriminate"
        )
        for key in report["keys"]:
            lines.append(f"    {key:<22} {entry[key]}")
    lines.append("")
    if report["arl_fit"]:
        lines.append("ARL conversion explaining every case: " + str(report["arl_fit"]))
    lines.append(f"{len(report['disagreements'])} disagreement(s):")
    for item in report["disagreements"]:
        lines.append(
            f"  [{item['kind']}] {item['corner']}/{item['probe']} {item['label']}: "
            f"{item['between'][0]} {item['rows'][0]} vs {item['between'][1]} {item['rows'][1]}"
        )
    return "\n".join(lines)


def _json_ready(report: Mapping[str, Any]) -> str:
    return json.dumps(report, indent=1, sort_keys=False) + "\n"


def _open_devices(
    labels: Sequence[str], requested_variants: Sequence[str], bin_dir: Path, work: Path
) -> tuple[list[DeviceRun], dict[str, str]]:
    devices: list[DeviceRun] = []
    skipped: dict[str, str] = {}
    for label in labels:
        runner = VkRunner(bin_dir=bin_dir, device=label)
        try:
            info = device_info(runner)
        except (RunnerUnavailable, OSError) as error:
            skipped[label] = str(error)
            continue
        scratch = work / label
        scratch.mkdir(parents=True, exist_ok=True)
        devices.append(
            DeviceRun(
                label,
                info,
                DeviceExecutor(runner, scratch, {}),
                supported_variants(info, requested_variants),
            )
        )
    return devices, skipped


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.corners",
        description="Run the NV2A arithmetic corners on every Vulkan device and compare them "
        "with the interpreter's NV rules.",
    )
    parser.add_argument(
        "--devices",
        default=",".join(DEFAULT_DEVICES),
        help="comma separated device-name substrings (VKRUN_DEVICE), default %(default)s",
    )
    parser.add_argument(
        "--variants",
        default=",".join(VARIANTS),
        help="denormal variants to try where the device reports support (default %(default)s)",
    )
    parser.add_argument(
        "--bin-dir", type=Path, default=DEFAULT_BIN_DIR, help="where vkrun is built"
    )
    parser.add_argument("--work", type=Path, default=DEFAULT_WORK_DIR / "corners")
    parser.add_argument("--report", type=Path, default=None, help="write the JSON report here")
    parser.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="directory of the title's static *.bin programs (gitignored): find and run "
        "the ones that depend on 0 * x",
    )
    parser.add_argument("--list", action="store_true", help="list the probes and cases, no device")
    args = parser.parse_args(argv)

    probes = build_probes()
    if args.list:
        for probe in probes:
            for case in probe.cases:
                print(f"{case.corner:<18} {probe.name:<9} {case.label}")
        return 0
    variants = [v for v in args.variants.split(",") if v]
    unknown = [v for v in variants if v not in VARIANTS]
    if unknown:
        parser.error(f"--variants must be a subset of {','.join(VARIANTS)}")
    if shutil.which("glslangValidator") is None:
        print("corners: glslangValidator not found", file=sys.stderr)
        return 2
    args.work.mkdir(parents=True, exist_ok=True)
    labels = [d for d in args.devices.split(",") if d]
    with tempfile.TemporaryDirectory(prefix="corners-", dir=args.work) as scratch:
        try:
            devices, skipped = _open_devices(labels, variants, args.bin_dir, Path(scratch))
        except RunnerUnavailable as error:
            print(f"corners: {error}", file=sys.stderr)
            return 2
        for label, reason in skipped.items():
            print(f"corners: skipped device {label!r}: {reason}", file=sys.stderr)
        if not devices:
            print("corners: no requested Vulkan device is available", file=sys.stderr)
            return 2
        report = run_probes(devices, probes)
        report["skipped_devices"] = skipped
        if args.corpus is not None:
            programs = {p.stem: p.read_bytes() for p in sorted(args.corpus.glob("*.bin"))}
            if not programs:
                print(f"corners: no *.bin programs in {args.corpus}", file=sys.stderr)
                return 2
            measured = zero_rule_dependents(programs)
            dependents = {k: v for k, v in measured.items() if v["value_floats"]}
            report["zero_rule_dependents"] = {
                "programs_total": len(programs),
                "programs_dependent": len(dependents),
                "programs_sign_of_zero_only": sum(
                    1 for v in measured.values() if not v["value_floats"] and v["sign_only_floats"]
                ),
                "programs": dependents,
                "on_devices": run_dependents(programs, sorted(dependents), devices),
            }
    print(render_text(report))
    if "zero_rule_dependents" in report:
        section = report["zero_rule_dependents"]
        print(
            f"\n{section['programs_dependent']} of {section['programs_total']} programs depend on "
            f"0 * x numerically ({section['programs_sign_of_zero_only']} more differ only in the "
            "sign of a zero)"
        )
        for name in sorted(section["programs"]):
            item = section["programs"][name]
            print(
                f"  {name[:12]} floats={item['value_floats']} slots={item['value_output_slots']} "
                f"devices={section['on_devices'][name]['devices']}"
            )
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(_json_ready(report))
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
