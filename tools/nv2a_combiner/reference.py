# SPDX-License-Identifier: GPL-3.0-or-later
"""A software model of the NV2A pixel pipeline on ONE fragment, written from the semantics.

CLEAN ROOM. Every formula below comes from the OpenGL NV_register_combiners specification
(the 8 input mappings, scale and bias, dot product, mux, the final combiner equation) or
from the title's own assembler (`probe.py` shows which source construct sets which bit).
Nothing is taken from the translator in `glsl.py`, and the two are written differently on
purpose: this module works on one scalar channel at a time with Python lists, the
translator emits GLSL vector code. A defect they share would be a defect in `config.py`,
which is why `probe.py` checks that module against the assembler and not against this one.

FLOAT VERSUS HARDWARE. The hardware holds register values as 9-bit signed fixed point
(the specification says so in a note, and says the spec is written as if the maths were
float). `evaluate(..., quantum=1/255)` rounds every register write to that grid so the
float model's distance from a fixed-point model can be measured. The 9-bit model is an
INFERENCE about rounding points, not a hardware measurement, see the docs.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field, replace

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import models

Vec4 = tuple[float, float, float, float]
Vec3 = tuple[float, float, float]
#: `sampler(stage, (a, b, c))` returns an RGBA texel. Stands in for texture fetches.
Sampler = Callable[[int, Vec3], Vec4]

#: Alpha of spare0 before stage 0 when texture stage 0 is off. INFERRED, see the docs.
INITIAL_SPARE0_ALPHA_UNTEXTURED = 1.0
#: What a texture register reads as when its stage program is NONE. INFERRED.
UNTEXTURED = (0.0, 0.0, 0.0, 1.0)


def _zero_sampler(stage: int, coord: Vec3) -> Vec4:
    return (0.0, 0.0, 0.0, 0.0)


@dataclass
class Fragment:
    """Every input the pixel pipeline reads, outside the combiner configuration."""

    v0: Vec4 = (0.0, 0.0, 0.0, 0.0)
    v1: Vec4 = (0.0, 0.0, 0.0, 0.0)
    texcoord: tuple[Vec4, Vec4, Vec4, Vec4] = ((0.0, 0.0, 0.0, 1.0),) * 4
    fog_color: Vec3 = (0.0, 0.0, 0.0)
    fog_factor: float = 0.0
    factor0: tuple[Vec4, ...] = ((0.0,) * 4,) * 8
    factor1: tuple[Vec4, ...] = ((0.0,) * 4,) * 8
    final_c0: Vec4 = (0.0, 0.0, 0.0, 0.0)
    final_c1: Vec4 = (0.0, 0.0, 0.0, 0.0)
    sampler: Sampler = field(default=_zero_sampler)


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def map_input(value: float, mapping: int) -> float:
    """The eight input mappings, one scalar channel. NV_register_combiners table 4."""
    if mapping == cfg.MAP_UNSIGNED_IDENTITY:
        return max(0.0, value)
    if mapping == cfg.MAP_UNSIGNED_INVERT:
        return 1.0 - clamp(value, 0.0, 1.0)
    if mapping == cfg.MAP_EXPAND_NORMAL:
        return 2.0 * max(0.0, value) - 1.0
    if mapping == cfg.MAP_EXPAND_NEGATE:
        return -2.0 * max(0.0, value) + 1.0
    if mapping == cfg.MAP_HALF_BIAS_NORMAL:
        return max(0.0, value) - 0.5
    if mapping == cfg.MAP_HALF_BIAS_NEGATE:
        return -max(0.0, value) + 0.5
    if mapping == cfg.MAP_SIGNED_IDENTITY:
        return value
    if mapping == cfg.MAP_SIGNED_NEGATE:
        return -value
    raise ValueError(f"input mapping {mapping}")


def map_output(value: float, bias: bool, scale: float) -> float:
    """Bias first, then scale, then clamp to [-1, 1], as in the specification."""
    return clamp((value - (0.5 if bias else 0.0)) * scale, -1.0, 1.0)


def _round_to(value: float, quantum: float | None) -> float:
    if quantum is None:
        return value
    return round(value / quantum) * quantum


def dot_mapping(
    value: float, mode: int, mapping_one: Callable[[float], float] | None = None
) -> float:
    """Per-channel mapping of a dot-product stage's input texel. INFERRED, see the docs.

    `mapping_one` replaces xemu's formula for mode 1 (T102 candidates, `models.DOT_MAPPINGS`)."""
    if mode == 0:
        return value
    if mode == 1:
        if mapping_one is not None:
            return mapping_one(value)
        return (value * 255.0 - 128.0) / 127.0
    raise ValueError(f"dot mapping {mode}")


def texture_registers(
    config: cfg.Config, frag: Fragment, mapping_one: Callable[[float], float] | None = None
) -> list[Vec4]:
    """The four texture registers after the texture stage programs ran."""
    modes = config.texture_modes
    inputs = [cfg.input_stage(config.other_input, stage) for stage in range(4)]
    result: list[Vec4] = [UNTEXTURED] * 4
    dots: list[float] = [0.0] * 4
    for stage in range(4):
        mode = modes[stage]
        coord = frag.texcoord[stage]
        source = result[inputs[stage]] if stage else UNTEXTURED
        if mode == cfg.TEX_NONE:
            result[stage] = UNTEXTURED
        elif mode == cfg.TEX_2D_PROJECTIVE:
            result[stage] = frag.sampler(stage, (coord[0] / coord[3], coord[1] / coord[3], 0.0))
        elif mode == cfg.TEX_3D_PROJECTIVE:
            w = coord[3]
            result[stage] = frag.sampler(stage, (coord[0] / w, coord[1] / w, coord[2] / w))
        elif mode == cfg.TEX_CUBE_MAP:
            result[stage] = frag.sampler(stage, (coord[0], coord[1], coord[2]))
        elif mode == cfg.TEX_PASS_THROUGH:
            result[stage] = coord
        elif mode == cfg.TEX_CLIP_PLANE:
            result[stage] = (0.0, 0.0, 0.0, 0.0)
        elif mode in (cfg.TEX_DEPENDENT_AR, cfg.TEX_DEPENDENT_GB):
            pair = (
                (source[3], source[0]) if mode == cfg.TEX_DEPENDENT_AR else (source[1], source[2])
            )
            result[stage] = frag.sampler(stage, (pair[0], pair[1], 0.0))
        elif mode == cfg.TEX_DOT_PRODUCT:
            shift = 4 * (stage - 1)
            mapper = (config.dot_mapping >> shift) & 0xF
            dots[stage] = sum(
                coord[axis] * dot_mapping(source[axis], mapper, mapping_one) for axis in range(3)
            )
            result[stage] = (0.0, 0.0, 0.0, 0.0)
        elif mode == cfg.TEX_DOT_ST:
            shift = 4 * (stage - 1)
            mapper = (config.dot_mapping >> shift) & 0xF
            dots[stage] = sum(
                coord[axis] * dot_mapping(source[axis], mapper, mapping_one) for axis in range(3)
            )
            result[stage] = frag.sampler(stage, (dots[stage - 1], dots[stage], 0.0))
        else:
            raise ValueError(f"texture mode {mode}")
    return result


def clip_killed(config: cfg.Config, frag: Fragment, compare_word: int = 0) -> bool:
    """True when a clip-plane texture stage discards the fragment. INFERRED rule."""
    for stage, mode in enumerate(config.texture_modes):
        if mode != cfg.TEX_CLIP_PLANE:
            continue
        for axis in range(4):
            flag = (compare_word >> (4 * stage + axis)) & 1
            value = frag.texcoord[stage][axis]
            if (value >= 0.0) if flag else (value < 0.0):
                return True
    return False


#: A mux select value this close to its threshold may flip between float32 and float64, so a
#: comparison against the GPU cannot call a disagreement there a defect.
MUX_BOUNDARY_MARGIN = 1e-4


class Evaluator:
    """Runs the stages. One instance per fragment."""

    def __init__(
        self,
        config: cfg.Config,
        frag: Fragment,
        quantum: float | None,
        initial: dict[int, list[float | None]] | None = None,
        model: models.Model | None = None,
    ) -> None:
        self.config = config
        self.frag = frag
        self.quantum = quantum
        self.model = model
        self.near_mux_boundary = False
        mapping_one = models.DOT_MAPPINGS[model.dot][0] if model else None
        textures = texture_registers(config, frag, mapping_one)
        self.registers: dict[int, list[float]] = {
            cfg.REG_ZERO: [0.0] * 4,
            cfg.REG_FOG: [*frag.fog_color, frag.fog_factor],
            cfg.REG_V0: list(frag.v0),
            cfg.REG_V1: list(frag.v1),
            cfg.REG_SPARE0: [0.0] * 4,
            cfg.REG_SPARE1: [0.0] * 4,
        }
        for stage in range(4):
            self.registers[cfg.REG_T0 + stage] = list(textures[stage])
        if config.texture_modes[0] != cfg.TEX_NONE:
            self.registers[cfg.REG_SPARE0][3] = textures[0][3]
        else:
            self.registers[cfg.REG_SPARE0][3] = INITIAL_SPARE0_ALPHA_UNTEXTURED
        # Overrides replace the registers the hardware leaves undefined. None keeps a channel.
        for register, values in (initial or {}).items():
            for channel, value in enumerate(values):
                if value is not None:
                    self.registers[register][channel] = value

    def _read(self, snapshot: dict[int, list[float]], operand: cfg.Input, *, rgb: bool) -> list:
        """One mapped operand. RGB half: 3 channels. Alpha half: 1 channel."""
        values = snapshot[operand.register]
        if rgb:
            picked = [values[3]] * 3 if operand.alpha else values[:3]
        else:
            picked = [values[3] if operand.alpha else values[2]]
        return [map_input(value, operand.mapping) for value in picked]

    def _half(
        self, snapshot: dict[int, list[float]], half: cfg.Half, *, rgb: bool
    ) -> dict[str, list[float]]:
        a, b, c, d = (self._read(snapshot, operand, rgb=rgb) for operand in half.inputs)
        out = half.output
        if out.ab_dot:
            ab = [sum(x * y for x, y in zip(a, b, strict=True))] * 3
        else:
            ab = [x * y for x, y in zip(a, b, strict=True)]
        if out.cd_dot:
            cd = [sum(x * y for x, y in zip(c, d, strict=True))] * 3
        else:
            cd = [x * y for x, y in zip(c, d, strict=True)]
        if out.mux:
            alpha = snapshot[cfg.REG_SPARE0][3]
            if self.config.mux_msb:
                take_cd = alpha >= 0.5
                self.near_mux_boundary |= abs(alpha - 0.5) < MUX_BOUNDARY_MARGIN
            else:
                # The low bit of the 8-bit value. A negative select reads as 0, an INFERENCE.
                level = max(alpha, 0.0) * 255.0
                take_cd = (int(level + 0.5) & 1) == 1
                self.near_mux_boundary |= abs(level % 1.0 - 0.5) < 100 * MUX_BOUNDARY_MARGIN
            total = list(cd if take_cd else ab)
        else:
            total = [x + y for x, y in zip(ab, cd, strict=True)]
        shaped = {
            "ab": ab,
            "cd": cd,
            "sum": total,
        }
        return {
            key: [self._written(map_output(value, out.bias, out.scale)) for value in values]
            for key, values in shaped.items()
        }

    def _written(self, value: float) -> float:
        if self.model is not None and self.model.rounds_writes():
            return models.grid_round(value, self.model.rounding)
        return _round_to(value, self.quantum)

    def stage(self, stage: cfg.Stage) -> None:
        snapshot = {register: list(values) for register, values in self.registers.items()}
        snapshot[cfg.REG_C0] = list(self.frag.factor0[stage.factor0])
        snapshot[cfg.REG_C1] = list(self.frag.factor1[stage.factor1])
        rgb = self._half(snapshot, stage.rgb, rgb=True)
        alpha = self._half(snapshot, stage.alpha, rgb=False)
        for half, results, is_rgb in ((stage.rgb, rgb, True), (stage.alpha, alpha, False)):
            out = half.output
            for key, destination in (("ab", out.ab_dst), ("cd", out.cd_dst), ("sum", out.sum_dst)):
                if destination == cfg.REG_ZERO:
                    continue
                target = self.registers[destination]
                if is_rgb:
                    target[0:3] = results[key]
                    blue_to_alpha = (
                        out.blue_to_alpha_ab if key == "ab" else out.blue_to_alpha_cd
                    ) and key != "sum"
                    if blue_to_alpha:
                        target[3] = results[key][2]
                else:
                    target[3] = results[key][0]

    def _final_input(self, operand: cfg.Input, *, alpha_only: bool = False) -> list[float]:
        values = self.registers[operand.register]
        if alpha_only:
            return [map_input(values[3] if operand.alpha else values[2], operand.mapping)]
        picked = [values[3]] * 3 if operand.alpha else values[:3]
        return [map_input(value, operand.mapping) for value in picked]

    def final(self) -> Vec4:
        final = self.config.final
        self.registers[cfg.REG_C0] = list(self.frag.final_c0)
        self.registers[cfg.REG_C1] = list(self.frag.final_c1)
        spare0 = self.registers[cfg.REG_SPARE0]
        secondary = self.registers[cfg.REG_V1]
        first = [1.0 - clamp(x, 0.0, 1.0) if final.complement_r0 else max(0.0, x) for x in spare0]
        second = [
            1.0 - clamp(x, 0.0, 1.0) if final.complement_v1 else max(0.0, x) for x in secondary
        ]
        total = [first[axis] + second[axis] for axis in range(3)]
        if final.clamp_sum:
            total = [clamp(x, 0.0, 1.0) for x in total]
        self.registers[cfg.REG_SUM] = [*total, 0.0]
        product = [
            x * y
            for x, y in zip(self._final_input(final.e), self._final_input(final.f), strict=True)
        ]
        self.registers[cfg.REG_PROD] = [*product, 0.0]
        a, b, c, d = (self._final_input(op) for op in (final.a, final.b, final.c, final.d))
        rgb = [clamp(d[i] + (1.0 - a[i]) * c[i] + a[i] * b[i], 0.0, 1.0) for i in range(3)]
        green = self._final_input(final.g, alpha_only=True)[0]
        return (rgb[0], rgb[1], rgb[2], clamp(green, 0.0, 1.0))


@dataclass(frozen=True)
class Result:
    colour: Vec4
    #: A clip-plane texture stage discarded the fragment.
    killed: bool
    near_mux_boundary: bool


def evaluate_detailed(
    config: cfg.Config,
    frag: Fragment,
    *,
    quantum: float | None = None,
    initial: dict[int, list[float | None]] | None = None,
    model: models.Model | None = None,
) -> Result:
    """Run every stage and the final combiner. `quantum` rounds register writes to a grid.

    `model` (T102, T103) selects a candidate rounding and dot mapping, see `models.py`. None is
    the shipped model and every existing call keeps its behaviour."""
    if model is not None and model.texels8:
        inner = frag.sampler
        frag = replace(
            frag,
            sampler=lambda stage, coord: tuple(  # type: ignore[arg-type,return-value]
                models.grid_round(value, "half_up") for value in inner(stage, coord)
            ),
        )
    evaluator = Evaluator(config, frag, quantum, initial, model)
    for stage in config.stages:
        evaluator.stage(stage)
    colour = evaluator.final()
    if model is not None and model.rounds_output():
        colour = tuple(models.grid_round(value, model.rounding) for value in colour)  # type: ignore[assignment]
    return Result(colour, clip_killed(config, frag), evaluator.near_mux_boundary)


def evaluate(
    config: cfg.Config,
    frag: Fragment,
    *,
    quantum: float | None = None,
    initial: dict[int, list[float | None]] | None = None,
    model: models.Model | None = None,
) -> Vec4:
    return evaluate_detailed(config, frag, quantum=quantum, initial=initial, model=model).colour
