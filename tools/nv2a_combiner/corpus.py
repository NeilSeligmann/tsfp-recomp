# SPDX-License-Identifier: GPL-3.0-or-later
"""The title's own combiner configurations: collect, translate, count.

Two populations, both read out of the user's executable and never committed:

  STATIC     the pixel-shader definitions the title installs by pointer (35 addresses).
  GENERATED  what the title's source builder emits for every key, run through the title's
             own assembler (161 distinct blocks).

Only counts and failure causes leave this module. The translated shaders go to the
gitignored `generated/shaders/combiner/`, named by address or digest prefix.
"""

from __future__ import annotations

import hashlib
import random
import shutil
import subprocess
from argparse import Namespace
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import glsl, reference
from tools.shaderscan import builders as shader_builders
from tools.shaderscan import callargs
from tools.shaderscan import combiners as shader_combiners
from tools.shaderscan.assemble import AssemblerEmulator, AssemblerSpec, Outcome
from tools.shaderscan.cli import _decode, _reachable
from tools.shaderscan.image import Image
from tools.xdk_abi import SectionMap, executable_sections, walk_function


@dataclass(frozen=True)
class Addresses:
    """Where things are in ONE build of the title. They are arguments, never baked in."""

    assembler: int
    heap_alloc: int
    heap_free: int
    setter: int
    builder: int
    builder_register: str
    builder_mask: int
    key_target: int
    key_argument: int
    key_global: int
    wrapper_literals: tuple[tuple[int, str, int], ...]


def static_definitions(image: Image, setter: int) -> dict[int, bytes]:
    """Every distinct definition address passed to the `SetPixelShader` wrapper."""
    text = _decode(image, ".text")
    leaders = callargs.branch_leaders(text)
    by_address = {insn.address: index for index, insn in enumerate(text)}
    transfers = callargs.find_sites(text, {setter}, 1, leaders)
    arguments = [
        shader_combiners.tail_call_argument(text, by_address[site.va])
        if site.kind == "jmp"
        else site.args[0]
        for site in transfers
    ]
    addresses = sorted(
        {a.value for a in arguments if a.kind == callargs.KIND_IMM and a.value is not None}
    )
    result = {}
    for address in addresses:
        offset = image.xbe.va_to_offset(address)
        block = image.raw[offset : offset + cfg.BLOCK_BYTES]
        if len(block) == cfg.BLOCK_BYTES:
            result[address] = block
    return result


@dataclass
class Generated:
    """The builder's distinct sources, assembled."""

    #: digest of the source -> (source length, definition)
    blocks: dict[str, bytes] = field(default_factory=dict)
    rejected: int = 0
    #: Budget expiry, fault or bad return: no HRESULT, so neither assembled nor rejected.
    unfinished: int = 0
    reachable: set[str] = field(default_factory=set)


def generated_definitions(xbe: Path, image: Image, addresses: Addresses) -> Generated:
    """Run the pixel builder over every effective key, assemble each distinct source."""
    sections = SectionMap(executable_sections(xbe))
    walk = walk_function(sections, addresses.builder)
    if not walk.clean:
        raise RuntimeError("the builder's control-flow walk is not clean")
    tested = shader_builders.tested_bits(
        [(insn.mnemonic, insn.op_str) for insn in walk.insns], addresses.builder_register
    )
    effective = tested & addresses.builder_mask
    spec = shader_builders.BuilderSpec(
        "pixel", addresses.builder, addresses.builder_register, addresses.assembler
    )
    emulator = shader_builders.BuilderEmulator(image, spec)
    everything = shader_builders.enumerate_keys(
        emulator, shader_builders.subsets(effective), keep_sources=True
    )
    assembler = AssemblerEmulator(
        image, AssemblerSpec(addresses.assembler, addresses.heap_alloc, addresses.heap_free)
    )
    result = Generated()
    for digest, (source, flags) in everything.sources.items():
        run = assembler.run(source, flags)
        assembled = run.result
        if run.outcome not in (Outcome.RETURNED, Outcome.KERNEL_CALL) or assembled is None:
            result.unfinished += 1
            continue
        if assembled.kernel_call is not None or not assembled.data:
            result.rejected += 1
            continue
        result.blocks[digest] = assembled.data
    namespace = Namespace(
        section=".text",
        reachable=(addresses.key_target, addresses.key_argument),
        key_global=addresses.key_global,
        wrapper_literals=[f"{e:#x}:{r}:{end:#x}" for e, r, end in addresses.wrapper_literals],
    )
    _, reachable = _reachable(namespace, image, emulator, addresses.builder_mask)
    result.reachable = set(reachable.by_digest) & set(result.blocks)
    return result


def structure_key(block: bytes) -> bytes:
    """The block with every runtime-supplied constant and host-only word zeroed.

    Two blocks that differ only in the constant colours are ONE shader once the constants
    are uniforms, which is what the translator does.
    """
    dwords = cfg.words(block)
    for index in (
        *range(cfg.IDX_FACTOR0, cfg.IDX_FACTOR0 + 16),
        cfg.IDX_FINAL_C0,
        cfg.IDX_FINAL_C1,
    ):
        dwords[index] = 0
    for index in (cfg.IDX_FACTOR0_MAP, cfg.IDX_FACTOR1_MAP, cfg.IDX_FINAL_MAP):
        dwords[index] = 0
    return b"".join(value.to_bytes(4, "little") for value in dwords)


def glsl_text_key(config: cfg.Config) -> str:
    return hashlib.sha256(glsl.combine_function(config).encode()).hexdigest()


@dataclass
class Translation:
    name: str
    origin: str
    block: bytes
    config: cfg.Config | None = None
    cause: str | None = None
    detail: str = ""
    compiled: bool | None = None


def translate_blocks(blocks: dict[str, tuple[str, bytes]], out: Path) -> list[Translation]:
    """Decode and translate each block, write the fragment shader, compile it."""
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for name, (origin, block) in blocks.items():
        entry = Translation(name, origin, block)
        try:
            entry.config = cfg.decode(block)
        except cfg.UnsupportedConfig as error:
            entry.cause = error.cause
            entry.detail = str(error)
            results.append(entry)
            continue
        path = out / f"{name}.frag"
        path.write_text(glsl.fragment_shader(entry.config))
        completed = subprocess.run(
            ["glslangValidator", "-V", str(path), "-o", str(out / f"{name}.spv")],
            capture_output=True,
            text=True,
            timeout=60,
        )
        entry.compiled = completed.returncode == 0
        if not entry.compiled:
            entry.cause = "glslang"
            entry.detail = completed.stdout.strip().splitlines()[-1] if completed.stdout else ""
        elif shutil.which("spirv-val") is not None:
            checked = subprocess.run(
                ["spirv-val", "--target-env", "vulkan1.1", str(out / f"{name}.spv")],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if checked.returncode != 0:
                entry.cause = "spirv_val"
                entry.detail = checked.stderr.strip().splitlines()[0] if checked.stderr else ""
        results.append(entry)
    return results


@dataclass
class InitialStateSensitivity:
    """Configurations whose output changes when an UNDEFINED initial register changes."""

    spare0_colour: int = 0
    spare0_alpha_without_texture: int = 0
    spare1: int = 0
    untextured_register: int = 0
    checked: int = 0


def initial_state_sensitivity(
    configs: list[cfg.Config], *, trials: int = 40, seed: int = 5
) -> InitialStateSensitivity:
    """Which configurations read a register the hardware leaves undefined.

    Each undefined initial value is replaced by two different values and the fragment
    evaluated both ways. A difference on any of `trials` random fragments marks the
    configuration. A configuration that never reads the register passes every trial.
    """
    from tools.nv2a_combiner.validate import make_case

    rng = random.Random(seed)
    cases = [make_case(rng).fragment for _ in range(trials)]
    result = InitialStateSensitivity(checked=len(configs))
    for config in configs:
        probes = {
            "spare0_colour": ({cfg.REG_SPARE0: [0.2, 0.6, 0.9, None]}, 0.0),
            "spare1": ({cfg.REG_SPARE1: [0.3, 0.7, 0.1, 0.8]}, 0.0),
        }
        for attribute, (override, _) in probes.items():
            if _differs(config, cases, override):
                setattr(result, attribute, getattr(result, attribute) + 1)
        if config.texture_modes[0] == cfg.TEX_NONE and _differs(
            config, cases, {cfg.REG_SPARE0: [None, None, None, 0.0]}
        ):
            result.spare0_alpha_without_texture += 1
        for stage, mode in enumerate(config.texture_modes):
            if mode == cfg.TEX_NONE and _differs(
                config, cases, {cfg.REG_T0 + stage: [0.4, 0.5, 0.6, 0.3]}
            ):
                result.untextured_register += 1
                break
    return result


def _differs(
    config: cfg.Config, cases: list[reference.Fragment], override: dict[int, list[float | None]]
) -> bool:
    for fragment in cases:
        base = reference.evaluate(config, fragment)
        changed = reference.evaluate(config, fragment, initial=override)
        if any(abs(a - b) > 1e-9 for a, b in zip(base, changed, strict=True)):
            return True
    return False


def summarise(results: list[Translation]) -> Counter[str]:
    """Outcome counts: translated, or the cause of each failure."""
    counts: Counter[str] = Counter()
    for entry in results:
        counts["translated" if entry.cause is None else entry.cause] += 1
    return counts


def feature_census(configs: list[cfg.Config]) -> Counter[str]:
    """How many configurations use each feature at least once."""
    census: Counter[str] = Counter()
    for config in configs:
        seen: set[str] = set()
        for stage in config.stages:
            for name, half in (("rgb", stage.rgb), ("alpha", stage.alpha)):
                out = half.output
                if out.mux:
                    seen.add("mux")
                if out.ab_dot or out.cd_dot:
                    seen.add("dot product")
                if out.blue_to_alpha_ab or out.blue_to_alpha_cd:
                    seen.add("blue to alpha")
                if out.bias:
                    seen.add("output bias")
                seen.add(f"output scale {out.scale:g}")
                for operand in half.inputs:
                    seen.add(f"{name} input mapping {operand.mapping}")
                for destination in (out.ab_dst, out.cd_dst, out.sum_dst):
                    if destination not in (cfg.REG_ZERO, cfg.REG_SPARE0, cfg.REG_SPARE1):
                        seen.add("writes a register other than spare0/spare1")
            if stage.factor0 or stage.factor1:
                seen.add("per-stage constant beyond stage 0")
        if not config.mux_msb:
            seen.add("LSB mux select")
        final = config.final
        for operand in (final.a, final.b, final.c, final.d, final.e, final.f, final.g):
            seen.add(f"final input mapping {operand.mapping}")
            if operand.register == cfg.REG_SUM:
                seen.add("final reads spare0+secondary sum")
            if operand.register == cfg.REG_PROD:
                seen.add("final reads E times F")
        if final.complement_v1 or final.complement_r0:
            seen.add("final sum complement flag")
        seen.add("final sum clamp" if final.clamp_sum else "final sum unclamped")
        for mode in config.texture_modes:
            seen.add(f"texture stage program {cfg.TEXTURE_MODE_NAMES[mode]}")
        seen.add(f"stages {len(config.stages)}")
        census.update(seen)
    return census
