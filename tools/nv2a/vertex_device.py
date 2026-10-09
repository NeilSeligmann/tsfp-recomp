# SPDX-License-Identifier: GPL-3.0-or-later
"""T100: run the translated VERTEX shader form on a Vulkan device and compare it with the
compute form and the interpreter.

Until now only `translate.compute_test_source` was executed (docs/vertex-translator.md
section 7). The renderer uses `translate.vertex_shader_source`, a different wrapper around the
same `nv2a_program()` body, so its varying assignments, `gl_Position`, `gl_PointSize` and the
std140 constant block were only compiled. This module captures the vertex stage's outputs
exactly, with VK_EXT_transform_feedback and rasterizer discard, through `tools/nv2a/vkvert.c`
(a second stage of the shared runner core `vkrun_core.c`, T104).

The device-side shader is the renderer's vertex source with layout qualifiers added and
nothing else: each output gets `xfb_buffer = 0, xfb_offset = 16 * NV2A output address,
xfb_stride = 256`, `gl_Position` and `gl_PointSize` are redeclared in `gl_PerVertex` with the
same qualifiers. The captured record therefore has the compute shader's 16 x vec4 layout and
`validate.compare` applies unchanged. A float output (`oFog`, `oPts`) is one float at the
start of its slot, and only captured components are compared.

MEASURED facts are about the device named in the report (RADV on this machine, not NV2A):
`python -m tools.nv2a.vertex_device --device hardware`. INFERRED items are marked.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools.nv2a import interp, isa, translate, validate, viewport
from tools.nv2a.validate import (
    ATOL,
    KINDS,
    RTOL,
    Job,
    Mismatch,
    RunnerError,
    TestVector,
    VkRunner,
    compare,
    pack_vectors,
    read_outputs,
)

VKVERT_SOURCE = Path(__file__).resolve().with_name("vkvert.c")
#: Bytes between consecutive records in the capture buffer (64 floats, `xfb_stride`).
CAPTURE_STRIDE = 256
SLOT_BYTES = 16
POSITION_ADDRESS = 0
POINT_SIZE_ADDRESS = 6
#: The runner's NaN sentinel (vkrun_core.c SENTINEL_BITS): a captured float still holding it was
#: never written by the vertex stage.
SENTINEL_BITS = 0x7FC0DEAD
CATEGORY_UNWRITTEN = "unwritten"
#: Float controls the compute form requests (SignedZeroInfNanPreserve, width 32).
FLOAT_CONTROLS = translate.FLOAT_CONTROLS_LINES
#: The viewport-inverse twin that explains each device's `--undo-viewport` corner vectors
#: (T100c, T266, docs/vertex-translator.md 7.6.2), matched case-sensitively as a substring of the
#: Vulkan device name. MEASURED on those two devices only: no single twin fits both (llvmpipe
#: divides exactly, RADV computes `a / b` as `a * rcp(b)`), and none is claimed for NV2A. A device
#: not listed has no default, the twin must then be named explicitly.
DEVICE_TWINS = {
    "RADV": viewport.TWIN_FLOAT32_RCP,
    "llvmpipe": viewport.TWIN_FLOAT32_FTZ,
}

_VARYING_LINE = re.compile(r"^layout\(location = (\d+)\) out (vec4|float) (\w+);$")
_VARYING_ADDRESS = {name: address for address, (name, _, _) in translate.VARYINGS.items()}


@dataclass(frozen=True)
class Capture:
    """One captured output: its NV2A address and how many leading components it has."""

    address: int
    width: int


def captures(translation: translate.Translation) -> tuple[Capture, ...]:
    """What the vertex form writes, by output address: oPos 4, oPts 1, a vec4 varying 4, a
    float varying (oFog) 1."""
    found = []
    for address in sorted(translation.outputs_written):
        if address == POSITION_ADDRESS:
            found.append(Capture(address, 4))
        elif address == POINT_SIZE_ADDRESS:
            found.append(Capture(address, 1))
        elif address in translate.VARYINGS:
            found.append(Capture(address, 1 if translate.VARYINGS[address][2] == "float" else 4))
    return tuple(found)


def _xfb(address: int) -> str:
    return f"xfb_buffer = 0, xfb_offset = {address * SLOT_BYTES}, xfb_stride = {CAPTURE_STRIDE}"


def capture_source(
    translation: translate.Translation,
    *,
    undo_viewport: bool = False,
    float_controls: bool = False,
    output_init: str = translate.DEFAULT_OUTPUT_INIT,
) -> str:
    """The renderer's vertex shader with transform feedback layout qualifiers added.

    Nothing else changes: the `nv2a_program()` body, the input and uniform declarations and
    `main()` are the renderer's own text. `float_controls` is passed to
    `translate.vertex_shader_source`: the compute form's SignedZeroInfNanPreserve execution mode,
    which the renderer's default source lacks (T100a, docs/vertex-translator.md 7.6).
    `output_init` is the output register start value (T561, `translate.OUTPUT_INITS`)."""
    source = translate.vertex_shader_source(
        translation,
        undo_viewport=undo_viewport,
        float_controls=float_controls,
        output_init=output_init,
    )
    wanted = {capture.address for capture in captures(translation)}
    lines: list[str] = []
    seen: set[int] = set()
    block_at = None
    for line in source.splitlines():
        match = _VARYING_LINE.match(line)
        if match:
            address = _VARYING_ADDRESS[match.group(3)]
            if address not in wanted:
                raise ValueError(f"varying {match.group(3)} is declared but not captured")
            line = (
                f"layout(location = {match.group(1)}, {_xfb(address)}) "
                f"out {match.group(2)} {match.group(3)};"
            )
            seen.add(address)
        elif line == f"vec4 v[{isa.INPUT_COUNT}];":
            block_at = len(lines)
        lines.append(line)
    if block_at is None:
        raise ValueError("vertex source has no `vec4 v[...]` anchor line to place gl_PerVertex")
    builtin = ["out gl_PerVertex {"]
    if POSITION_ADDRESS in wanted:
        builtin.append(f"    layout({_xfb(POSITION_ADDRESS)}) vec4 gl_Position;")
        seen.add(POSITION_ADDRESS)
    if POINT_SIZE_ADDRESS in wanted:
        builtin.append(f"    layout({_xfb(POINT_SIZE_ADDRESS)}) float gl_PointSize;")
        seen.add(POINT_SIZE_ADDRESS)
    builtin.append("};")
    if seen != wanted:
        raise ValueError(f"captured {sorted(seen)} but the program writes {sorted(wanted)}")
    lines[block_at:block_at] = builtin
    return "\n".join(lines) + "\n"


# ----------------------------------------------------------------------------- running


def device_unavailable_reason(device: str = "hardware") -> str | None:
    """None when glslang, gcc, libvulkan and a transform-feedback device matching `device` are
    all usable, else the reason a test should be skipped for."""
    if shutil.which("glslangValidator") is None:
        return "glslangValidator not found"
    try:
        info = VkRunner(entry=VKVERT_SOURCE, device=device).info()
    except (validate.RunnerUnavailable, OSError, subprocess.SubprocessError) as error:
        return f"no usable Vulkan {device} device with VK_EXT_transform_feedback: {error}"
    if not info.get("device"):
        return f"no Vulkan device matches {device!r}"
    return None


def device_name(device: str = "hardware") -> str:
    return VkRunner(entry=VKVERT_SOURCE, device=device).info()["device"]


def run_vertex(
    source: str,
    vectors: Sequence[TestVector],
    *,
    runner: VkRunner,
    work: Path,
    name: str = "shader",
) -> list[tuple[tuple[float, ...], ...]]:
    """Run a vertex shader (any text `glslangValidator -S vert` accepts, already xfb-decorated)
    over `vectors` and return the raw per-vector 16 x 4 capture, uncaptured floats at the
    runner's sentinel. Raises CompileError or RunnerError."""
    shader = work / f"{name}.vert"
    spirv = work / f"{name}.spv"
    shader.write_text(source)
    validate.compile_glsl(shader, spirv, "vert")
    inputs = work / f"{name}.in"
    output = work / f"{name}.out"
    inputs.write_bytes(pack_vectors(vectors))
    (result,) = runner.run_batch([Job(spirv, inputs, output, len(vectors))])
    if not result.ok:
        raise RunnerError(result.reason)
    return read_outputs(output, len(vectors))


def _bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def project(outputs: Sequence[Sequence[float]], wanted: Sequence[Capture]) -> list[list[float]]:
    """16 x 4 with every uncaptured component zero, so `compare` sees only what was captured."""
    slots = [[0.0] * 4 for _ in range(16)]
    for capture in wanted:
        for component in range(capture.width):
            slots[capture.address][component] = outputs[capture.address][component]
    return slots


def unwritten(
    raw: Sequence[Sequence[Sequence[float]]],
    wanted: Sequence[Capture],
    kinds: Sequence[str],
    program: str,
) -> list[Mismatch]:
    """Captured floats the vertex stage never wrote (still the runner's sentinel bits)."""
    found = []
    for vector, slots in enumerate(raw):
        for capture in wanted:
            for component in range(capture.width):
                value = slots[capture.address][component]
                if _bits(value) == SENTINEL_BITS:
                    found.append(
                        Mismatch(
                            program,
                            vector,
                            capture.address,
                            component,
                            0.0,
                            value,
                            CATEGORY_UNWRITTEN,
                            kinds[vector],
                        )
                    )
    return found


def twin_for_device(name: str) -> str | None:
    """The T100c twin measured to explain the device named `name`, None when not measured."""
    for fragment, twin in DEVICE_TWINS.items():
        if fragment in name:
            return twin
    return None


def expected_position(
    kind: str,
    vector: TestVector,
    compute_position: Sequence[float],
    twin: str = viewport.TWIN_FLOAT64,
) -> list[float]:
    """The `oPos` the undo-viewport vertex form should produce: a raw `o0` through a Python twin
    of the inverse (INFERRED, tools/nv2a/viewport.py). The float64 twin is the T96 one, which
    differs from the device on corner vectors, the float32 twins are T100c."""
    return viewport.twin_position(
        twin,
        kind,
        compute_position,
        vector.constants[viewport.SCALE],
        vector.constants[viewport.OFFSET],
    )


@dataclass
class ProgramResult:
    name: str
    status: str = ""
    detail: str = ""
    vectors: int = 0
    captured: list[tuple[int, int]] = field(default_factory=list)
    compared_floats: int = 0
    #: vertex form on the device vs the compute form on the same device
    vs_compute: list[Mismatch] = field(default_factory=list)
    #: vertex form on the device vs the interpreter
    vs_interpreter: list[Mismatch] = field(default_factory=list)
    #: the compute form on the same device vs the interpreter (context, not a T100 check)
    compute_vs_interpreter: list[Mismatch] = field(default_factory=list)
    unwritten: list[Mismatch] = field(default_factory=list)
    #: the twin that the inverse was compared with ("" when the program has no inverse)
    twin: str = ""
    #: per window source (corner_viewport.SOURCES), the device's `gl_Position` against the twin
    #: fed that source: residuals with their corner_classify effect and cause. The "vertex"
    #: source is the same stage's own window without the inverse (isolates the inverse).
    twin_residuals: dict[str, list[Any]] = field(default_factory=dict)

    def counts(self) -> dict[str, int]:
        return {
            "vs_compute": len(self.vs_compute),
            "vs_interpreter": len(self.vs_interpreter),
            "compute_vs_interpreter": len(self.compute_vs_interpreter),
            "unwritten": len(self.unwritten),
            **{f"twin_{source}": len(found) for source, found in self.twin_residuals.items()},
        }


STATUS_PASS = "pass"
STATUS_MISMATCH = "mismatch"
STATUS_UNSUPPORTED = "unsupported"


def _twin_residuals(
    translation: translate.Translation,
    vectors: Sequence[TestVector],
    *,
    name: str,
    twin: str,
    raw: Sequence[Sequence[Sequence[float]]],
    compute: Sequence[Sequence[Sequence[float]]],
    oracle: Sequence[Sequence[Sequence[float]]],
    vertex_runner: VkRunner,
    work: Path,
    float_controls: bool,
    output_init: str = translate.DEFAULT_OUTPUT_INIT,
) -> dict[str, list[Any]]:
    """Residuals of the inverse against `twin` for each window source (T100c method). Imported
    late: corner_viewport imports this module."""
    from tools.nv2a import corner_viewport

    plain = run_vertex(
        capture_source(translation, float_controls=float_controls, output_init=output_init),
        vectors,
        runner=vertex_runner,
        work=work,
        name="vertex_plain",
    )

    def positions(slots_per_vector: Sequence[Sequence[Sequence[float]]]) -> list[list[float]]:
        return [list(slots[POSITION_ADDRESS][:4]) for slots in slots_per_vector]

    evidence = corner_viewport.ProgramEvidence(
        name,
        translation.viewport,
        vectors,
        positions(raw),
        {
            corner_viewport.SOURCE_VERTEX: positions(plain),
            corner_viewport.SOURCE_COMPUTE: positions(compute),
            corner_viewport.SOURCE_INTERPRETER: positions(oracle),
        },
    )
    return {
        source: corner_viewport.score([evidence], twin, source).residuals
        for source in corner_viewport.SOURCES
    }


def run_program(
    data: bytes,
    name: str,
    vectors: Sequence[TestVector],
    *,
    vertex_runner: VkRunner,
    compute_runner: VkRunner,
    work: Path,
    undo_viewport: bool = False,
    float_controls: bool = False,
    output_init: str = translate.DEFAULT_OUTPUT_INIT,
    source_hook: Any = None,
    rtol: float = RTOL,
    atol: float = ATOL,
    model: str = "ieee",
    twin: str = viewport.TWIN_FLOAT64,
) -> ProgramResult:
    """Translate, run the vertex form and the compute form on the device, run the interpreter,
    and compare. `source_hook(source) -> source` lets a test inject a deliberate fault.

    `twin` (viewport.TWINS) is the model of the viewport inverse the `undo_viewport` form is
    compared with. The default float64 twin is T96's and does not fit `corner` vectors. With a
    float32 twin the vertex form is also run WITHOUT the inverse, and the inverse is scored
    against the twin fed that window (`twin_residuals["vertex"]`, the T100c method), so a
    difference already present before the inverse is not blamed on it. Every residual keeps its
    cause (`corner_viewport.CAUSE_*`) and `corner_classify` effect, none is hidden.

    `output_init` (T561) is the output register start value of the vertex form, the compute form
    AND the interpreter, so the three stay comparable (`translate.OUTPUT_INITS`)."""
    if twin not in viewport.TWINS:
        raise ValueError(f"unknown twin {twin!r}, expected one of {viewport.TWINS}")
    result = ProgramResult(name, vectors=len(vectors))
    try:
        translation = translate.translate(data, name=name)
    except translate.Unsupported as error:
        result.status, result.detail = STATUS_UNSUPPORTED, str(error)[:200]
        return result
    wanted = captures(translation)
    result.captured = [(c.address, c.width) for c in wanted]
    vertex_source = capture_source(
        translation,
        undo_viewport=undo_viewport,
        float_controls=float_controls,
        output_init=output_init,
    )
    if source_hook is not None:
        vertex_source = source_hook(vertex_source)
    kinds = [vector.kind for vector in vectors]
    raw = run_vertex(vertex_source, vectors, runner=vertex_runner, work=work, name="vertex")
    compute = validate.run_glsl_vectors(
        translate.compute_test_source(translation, output_init=output_init),
        vectors,
        work=work,
        runner=compute_runner,
        name="compute",
    )
    interpreter = validate.ModelInterpreter(interp, model, output_init)
    oracle = [
        interpreter.run(data, vector.inputs, vector.constants, a0=0).outputs for vector in vectors
    ]
    inverse = undo_viewport and translation.viewport in viewport.GLSL_BODY
    expected_compute = [[list(row) for row in slots] for slots in compute]
    expected_oracle = [[list(row) for row in slots] for slots in oracle]
    if inverse:
        for index, vector in enumerate(vectors):
            for table in (expected_compute, expected_oracle):
                table[index][POSITION_ADDRESS] = expected_position(
                    translation.viewport, vector, table[index][POSITION_ADDRESS], twin
                )
    actual = [project(slots, wanted) for slots in raw]
    result.compared_floats = len(vectors) * sum(c.width for c in wanted)
    result.vs_compute = compare(
        [project(slots, wanted) for slots in expected_compute],
        actual,
        rtol,
        atol,
        program=name,
        kinds=kinds,
    )
    result.vs_interpreter = compare(
        [project(slots, wanted) for slots in expected_oracle],
        actual,
        rtol,
        atol,
        program=name,
        kinds=kinds,
    )
    result.compute_vs_interpreter = compare(
        [project(slots, wanted) for slots in compute],
        [project(slots, wanted) for slots in oracle],
        rtol,
        atol,
        program=name,
        kinds=kinds,
    )
    result.unwritten = unwritten(raw, wanted, kinds, name)
    if inverse:
        result.twin = twin
        result.twin_residuals = _twin_residuals(
            translation,
            vectors,
            name=name,
            twin=twin,
            raw=raw,
            compute=compute,
            oracle=oracle,
            vertex_runner=vertex_runner,
            work=work,
            float_controls=float_controls,
            output_init=output_init,
        )
    failed = (
        result.vs_compute
        or result.vs_interpreter
        or result.unwritten
        or result.twin_residuals.get("vertex")
    )
    result.status = STATUS_MISMATCH if failed else STATUS_PASS
    return result


# ---------------------------------------------------------------------------------- CLI


def _programs(paths: Sequence[str]) -> dict[str, bytes]:
    files: list[Path] = []
    for entry in paths:
        path = Path(entry)
        files.extend(sorted(path.glob("*.bin")) if path.is_dir() else [path])
    return {file.stem: file.read_bytes() for file in files}


def print_twin_residuals(results: Sequence[ProgramResult]) -> None:
    """Per window source, how many floats of `gl_Position` the twin does not reproduce, by cause
    (corner_viewport.CAUSE_*) and by corner_classify effect."""
    sources: list[str] = []
    for result in results:
        sources.extend(source for source in result.twin_residuals if source not in sources)
    programs = sum(1 for result in results if result.twin_residuals)
    print(f"inverse programs: {programs}")
    for source in sources:
        found = [r for result in results for r in result.twin_residuals.get(source, [])]
        print(
            f"  twin fed by {source:<11} residual floats: {len(found)} "
            f"causes {dict(Counter(r.cause for r in found))} "
            f"effects {dict(Counter(r.effect for r in found))}"
        )


def _twin_json(result: ProgramResult) -> dict[str, Any]:
    return {
        "twin": result.twin,
        "twin_residual_records": {
            source: [
                {
                    "vector": r.vector,
                    "component": r.component,
                    "expected": r.expected,
                    "device": r.device,
                    "effect": r.effect,
                    "cause": r.cause,
                }
                for r in found[:20]
            ]
            for source, found in result.twin_residuals.items()
        },
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.vertex_device",
        description=(
            "Run the translated vertex shader form on a Vulkan device (T100) and compare it "
            "with the compute form on the same device and with the interpreter."
        ),
    )
    parser.add_argument("programs", nargs="*", help="microcode .bin files or directories")
    parser.add_argument(
        "--device",
        default="hardware",
        help='"hardware" (first non-software device), or a device-name substring such as '
        "radv or llvmpipe (default: hardware)",
    )
    parser.add_argument("--count", type=int, default=16, help="vectors per kind (default 16)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--kinds", nargs="+", choices=KINDS, default=list(KINDS))
    parser.add_argument(
        "--undo-viewport",
        action="store_true",
        help="use the T96 viewport inverse in the vertex form (INFERRED)",
    )
    parser.add_argument(
        "--float-controls",
        action="store_true",
        help="ask the vertex form for SignedZeroInfNanPreserve like the compute form",
    )
    parser.add_argument(
        "--output-init",
        choices=sorted(translate.OUTPUT_INITS),
        default=translate.DEFAULT_OUTPUT_INIT,
        help="output register start value of the vertex form, the compute form and the "
        "interpreter: zero, or nv = (0, 0, 0, 1) (T561, INFERRED)",
    )
    parser.add_argument(
        "--twin",
        choices=("auto", *viewport.TWINS),
        default="auto",
        help="model of the viewport inverse the --undo-viewport form is compared with. auto "
        f"picks the T100c twin measured for the device ({DEVICE_TWINS}) and is refused for "
        "any other device. float64 is T96's, which does not fit corner vectors",
    )
    parser.add_argument("--model", choices=("spec", "ieee"), default="ieee")
    parser.add_argument("--json", type=Path, help="write the per-program report here")
    parser.add_argument("--info", action="store_true", help="print the device and exit")
    args = parser.parse_args(argv)
    reason = device_unavailable_reason(args.device)
    if reason is not None:
        print(f"unavailable: {reason}", file=sys.stderr)
        return 3
    if args.info:
        print(device_name(args.device))
        return 0
    programs = _programs(args.programs)
    if not programs:
        parser.error("no programs given")
    twin = args.twin
    if args.undo_viewport and twin == "auto":
        twin = twin_for_device(device_name(args.device)) or ""
        if not twin:
            parser.error(
                f"no viewport twin is measured for device {device_name(args.device)!r} "
                f"(known: {sorted(DEVICE_TWINS)}), pass --twin {{{','.join(viewport.TWINS)}}}"
            )
    elif twin == "auto":
        twin = viewport.TWIN_FLOAT64
    vertex_runner = VkRunner(entry=VKVERT_SOURCE, device=args.device)
    compute_runner = VkRunner(device=args.device)
    results = []
    with tempfile.TemporaryDirectory(prefix="nv2a-vertex-") as scratch:
        for name, data in programs.items():
            vectors, _ = validate.build_vectors(data, args.seed, args.count, args.kinds)
            results.append(
                run_program(
                    data,
                    name,
                    vectors,
                    vertex_runner=vertex_runner,
                    compute_runner=compute_runner,
                    work=Path(scratch),
                    undo_viewport=args.undo_viewport,
                    float_controls=args.float_controls,
                    output_init=args.output_init,
                    model=args.model,
                    twin=twin,
                )
            )
    summary: Counter[str] = Counter(result.status for result in results)
    by_kind: Counter[str] = Counter()
    for result in results:
        for found in result.vs_compute + result.vs_interpreter:
            by_kind[f"{found.kind}/{found.category}"] += 1
    print(f"device: {device_name(args.device)}")
    print(f"programs: {len(results)} {dict(summary)}")
    print(f"floats compared: {sum(result.compared_floats for result in results)}")
    print(f"mismatches by kind/category: {dict(by_kind)}")
    if args.undo_viewport:
        print(f"viewport twin: {twin}")
        print_twin_residuals(results)
    for result in results:
        if result.status == STATUS_MISMATCH:
            print(f"  {result.name[:16]} {result.counts()}")
    if args.json:
        args.json.write_text(
            json.dumps(
                [
                    {
                        "name": r.name,
                        "status": r.status,
                        "captured": r.captured,
                        "compared_floats": r.compared_floats,
                        **r.counts(),
                        **_twin_json(r),
                        "records": [m.to_dict() for m in (r.vs_compute + r.vs_interpreter)[:20]],
                    }
                    for r in results
                ],
                indent=1,
            )
        )
    return 0 if summary.get(STATUS_MISMATCH, 0) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
