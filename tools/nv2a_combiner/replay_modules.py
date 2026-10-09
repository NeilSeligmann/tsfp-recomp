# SPDX-License-Identifier: GPL-3.0-or-later
"""Build-time generator and key for the combiner fragment modules of the pushbuffer replay (T75).

`src/gpu/gpu_combiner.c` decides a draw's combiner stage from the 57-dword shadow the decoder
kept and names the module `combiner_<sha256>` over 36 STRUCTURE dwords. This file is the same
function in Python (`structure_key`, `module_name`) plus the generator that writes those modules:

    python -m tools.nv2a_combiner.replay_modules --out generated/shaders/combiner/replay corpus XBE
    python -m tools.nv2a_combiner.replay_modules --out DIR blocks FILE   # 240-byte blocks, joined

Output, GITIGNORED (nothing derived from the user's executable is committed):

  * `spv/combiner_<digest>.spv`  one fragment-stage SPIR-V module per distinct configuration;
  * `fsh_table.inc`              C initialisers for `struct gpu_vsh_table` (names only, no keys).

What is NOT keyed, because it changes no translation: the factors (10..25) and the final constants
(43, 44) are uniforms, and words 42, 55, 56 only matter to texture modes the replay refuses. The
unused stages are zeroed and the bits the translator does not read are masked (the OUTPUT, CONTROL
and FINAL1 masks below), so two configurations that translate alike share a module.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import glsl

#: Dword indices hashed into a module name, ascending. `gpu_combiner.c` key_indices.
STRUCTURE_INDICES = (
    *range(0, 10),
    *range(26, 42),
    *range(45, 55),
    # T1490: the dot mapping (55) and the other stage input (56), zero unless a dot stage is kept
    55,
    56,
)
#: Words hashed by `module_name` for a key whose last two words are zero: the 36 words of every
#: module that existed before T1490, so their names did not change.
LEGACY_KEY_WORDS = 36
DOT_MAPPING_KEY_MASK = 0x00000FFF
OTHER_INPUT_KEY_MASK = 0x00FF0000
DOT_MODES = (cfg.TEX_DOT_ST, cfg.TEX_DOT_PRODUCT)
#: Modes refused even when the stage is unread: a clip plane (5) discards, DOT_ZW (10) writes depth.
SIDE_EFFECT_MODES = (cfg.TEX_CLIP_PLANE, 10)
KEY_WORDS = len(STRUCTURE_INDICES)
CONTROL_KEY_MASK = (
    0xFF | cfg.CONTROL_MUX_MSB_BIT | cfg.CONTROL_FACTOR0_EACH_BIT | (cfg.CONTROL_FACTOR1_EACH_BIT)
)
OUTPUT_KEY_MASK = 0x000FFFFF
FINAL1_KEY_MASK = 0xFFFFFFE0
REPLAY_MODES = glsl.REPLAY_TEXTURE_MODES

_SPARE0 = cfg.REG_SPARE0
_V1 = cfg.REG_V1


class ReplayRefusal(ValueError):
    """The configuration is one the replay never draws. `cause` is a short stable name."""

    def __init__(self, cause: str, detail: str = "") -> None:
        super().__init__(f"{cause}{': ' + detail if detail else ''}")
        self.cause = cause


def config_reads(config: cfg.Config) -> int:
    """Bit r is set when source register r is read. The same meaning as `gpu_combiner.c`.

    The sum register reads spare0 and the secondary colour, the product reads E and F, a mux
    reads spare0 alpha. The E and F operands are read only through a product."""
    mask = 0
    for stage in config.stages:
        for half in (stage.rgb, stage.alpha):
            for operand in half.inputs:
                mask |= 1 << operand.register
            if half.output.mux:
                mask |= 1 << _SPARE0
    final = config.final

    def through_sum(register: int) -> int:
        return (1 << _SPARE0) | (1 << _V1) if register == cfg.REG_SUM else 0

    for operand in (final.a, final.b, final.c, final.d, final.g):
        register = operand.register
        mask |= (1 << register) | through_sum(register)
        if register == cfg.REG_PROD:
            for source in (final.e, final.f):
                mask |= (1 << source.register) | through_sum(source.register)
    return mask


def initial_spare0_alpha_read(config: cfg.Config) -> bool:
    """True when spare0 alpha is read before any stage wrote it (it then starts as t0.a when stage
    0 samples a texture and as 1.0 otherwise). The same walk as `gpu_combiner.c`: the reads of a
    stage see the state at its start, the writes land at its end."""
    written = False
    for stage in config.stages:
        read = any(
            operand.register == _SPARE0 and operand.alpha
            for half in (stage.rgb, stage.alpha)
            for operand in half.inputs
        ) or any(half.output.mux for half in (stage.rgb, stage.alpha))
        if read and not written:
            return True
        colour, alpha = stage.rgb.output, stage.alpha.output
        written = written or (
            (colour.ab_dst == _SPARE0 and colour.blue_to_alpha_ab)
            or (colour.cd_dst == _SPARE0 and colour.blue_to_alpha_cd)
            or _SPARE0 in (alpha.ab_dst, alpha.cd_dst, alpha.sum_dst)
        )
    if written:
        return False
    final = config.final
    operands = [final.a, final.b, final.c, final.d, final.g]
    if any(operand.register == cfg.REG_PROD for operand in operands):
        operands += [final.e, final.f]
    return any(operand.register == _SPARE0 and operand.alpha for operand in operands)


def kept_stages(config: cfg.Config, reads: int, program: int) -> list[int]:
    """The mode kept per stage (0 for a stage the draw does not need), T1490.

    A stage is needed when the combiner reads its register (stage 0 also when spare0 alpha starts
    as t0.a). A needed dot stage (9, 17) also needs the stage its input comes from, and DOT_ST
    (9) the dot of the stage before it. The same walk as `gpu_combiner.c`."""
    modes = [(program >> (5 * stage)) & 0x1F for stage in range(4)]
    needed = [bool(reads & (1 << (cfg.REG_T0 + stage))) for stage in range(4)]
    if initial_spare0_alpha_read(config):
        needed[0] = True
    for stage in range(3, 0, -1):
        if needed[stage] and modes[stage] in DOT_MODES:
            needed[cfg.input_stage(config.other_input, stage)] = True
            if modes[stage] == cfg.TEX_DOT_ST:
                needed[stage - 1] = True
    return [modes[stage] if needed[stage] else 0 for stage in range(4)]


def effective_program(config: cfg.Config, reads: int, program: int) -> int:
    """Word 54 as the replay keys it: the modes of `kept_stages`. Raises ReplayRefusal for a mode
    the replay does not draw, or a dot stage in an order xemu does not generate (a read of a NONE
    stage is defined, T1207)."""
    for stage in range(4):
        mode = (program >> (5 * stage)) & 0x1F
        if mode in SIDE_EFFECT_MODES or mode > 18:
            raise ReplayRefusal("texture_mode", f"stage {stage} mode {mode}")
    kept = kept_stages(config, reads, program)
    result = 0
    for stage, mode in enumerate(kept):
        if mode not in REPLAY_MODES:
            raise ReplayRefusal("texture_mode", f"stage {stage} mode {mode}")
        # xemu psh.c asserts: DOTPRODUCT at stage 1 or 2, DOT_ST at stage 2 or 3 after a dot stage
        if mode == cfg.TEX_DOT_PRODUCT and stage not in (1, 2):
            raise ReplayRefusal("stage_order", f"dot product at stage {stage}")
        if mode == cfg.TEX_DOT_ST and (stage < 2 or kept[stage - 1] not in DOT_MODES):
            raise ReplayRefusal(
                "stage_order", f"dot st at stage {stage} after mode {kept[stage - 1]}"
            )
        result |= mode << (5 * stage)
    return result


def structure_key(words: Sequence[int], program: int) -> tuple[int, ...]:
    """The 36 canonical dwords of a configuration, `program` being the effective word 54."""
    if len(words) < cfg.STATE_DWORDS:
        raise ValueError(f"need {cfg.STATE_DWORDS} dwords, got {len(words)}")
    canonical = list(words[: cfg.STATE_DWORDS])
    count = canonical[cfg.IDX_CONTROL] & 0xFF
    for stage in range(8):
        slots = (
            stage,
            cfg.IDX_ALPHA_OCW + stage,
            cfg.IDX_COLOR_ICW + stage,
            cfg.IDX_COLOR_OCW + stage,
        )
        if stage >= count:
            for slot in slots:
                canonical[slot] = 0
        else:
            canonical[cfg.IDX_ALPHA_OCW + stage] &= OUTPUT_KEY_MASK
            canonical[cfg.IDX_COLOR_OCW + stage] &= OUTPUT_KEY_MASK
    canonical[cfg.IDX_FINAL1] &= FINAL1_KEY_MASK
    canonical[cfg.IDX_CONTROL] &= CONTROL_KEY_MASK
    canonical[cfg.IDX_STAGE_PROGRAM] = program
    dot = any(((program >> (5 * stage)) & 0x1F) in DOT_MODES for stage in range(4))
    canonical[cfg.IDX_DOT_MAPPING] = (
        canonical[cfg.IDX_DOT_MAPPING] & DOT_MAPPING_KEY_MASK if dot else 0
    )
    canonical[cfg.IDX_OTHER_INPUT] = (
        canonical[cfg.IDX_OTHER_INPUT] & OTHER_INPUT_KEY_MASK if dot else 0
    )
    return tuple(canonical[index] for index in STRUCTURE_INDICES)


def module_name(key: Sequence[int]) -> str:
    """`combiner_<sha256>`. A key with no dot stage (the two last words zero) hashes only its first
    36 words, so every module name that existed before T1490 is unchanged."""
    words = tuple(key)
    if len(words) > LEGACY_KEY_WORDS and not any(words[LEGACY_KEY_WORDS:]):
        words = words[:LEGACY_KEY_WORDS]
    return "combiner_" + hashlib.sha256(struct.pack(f"<{len(words)}I", *words)).hexdigest()


def block_for_key(key: Sequence[int]) -> bytes:
    """A 60-dword block holding only the key words, for `config.decode`."""
    dwords = [0] * cfg.BLOCK_DWORDS
    for index, value in zip(STRUCTURE_INDICES, key, strict=True):
        dwords[index] = value
    return struct.pack(f"<{cfg.BLOCK_DWORDS}I", *dwords)


def replay_form(block: bytes, *, live_fog: bool = False) -> tuple[cfg.Config, int, tuple[int, ...]]:
    """(canonical config, register reads, key) of a 240-byte definition as the replay would run it.

    Raises `cfg.UnsupportedConfig` (the translator refuses it) or `ReplayRefusal`. The program
    word is taken from the block itself, as a stream that wrote 0x1E70 would carry it."""
    dwords = cfg.words(block)
    first = cfg.decode(block)
    reads = config_reads(first)
    if reads & (1 << cfg.REG_FOG) and not live_fog:
        raise ReplayRefusal("fog_register")
    program = effective_program(first, reads, dwords[cfg.IDX_STAGE_PROGRAM])
    key = structure_key(dwords, program)
    config = cfg.decode(block_for_key(key))
    return config, reads, key


ALPHA_SUFFIX = "_alpha"


def module_source(key: Sequence[int], *, alpha_test: bool = False, live_fog: bool = False) -> str:
    """The GLSL of the module `module_name(key)` (T860: `alpha_test` is the `_alpha` variant)."""
    config = cfg.decode(block_for_key(key))
    reads = config_reads(config)
    name = module_name(key) + (ALPHA_SUFFIX if alpha_test else "")
    return glsl.replay_fragment_shader(
        config, reads=reads, digest=name, alpha_test=alpha_test, live_fog=live_fog
    )


def render_inc(names: Sequence[str]) -> str:
    ordered = sorted(names)
    lines = [
        "/* GENERATED by tools/nv2a_combiner/replay_modules.py, do not edit, do not commit. */"
    ]
    lines.append("static const char *const fsh_module_names[] = {")
    lines += [f'    "{name}",' for name in ordered] or ["    0"]
    lines.append("};")
    lines.append("static const struct gpu_vsh_table fsh_table = {")
    lines.append("    0u,")
    lines.append("    0u, 0,")
    lines.append("    0u, 0,")
    lines.append(f"    {len(ordered)}u, fsh_module_names,")
    lines.append("};")
    return "\n".join(lines) + "\n"


class GenerationError(Exception):
    pass


def compile_fragment(glsl_path: Path, spirv: Path, glslang: str) -> None:
    done = subprocess.run(
        [
            glslang,
            "-V",
            "--target-env",
            "vulkan1.1",
            "-S",
            "frag",
            "-o",
            str(spirv),
            str(glsl_path),
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    if done.returncode != 0:
        raise GenerationError(
            f"glslang failed on {glsl_path.name}: {(done.stdout + done.stderr)[:500]}"
        )


def write_modules(
    keys: dict[str, tuple[int, ...]],
    out: Path,
    *,
    glslang: str = "glslangValidator",
    live_fog: bool = False,
    with_alpha: frozenset[str] = frozenset(),
) -> list[str]:
    """Write `spv/<name>.spv` for every key and `fsh_table.inc`. Returns the module names."""
    if shutil.which(glslang) is None:
        raise GenerationError(f"{glslang} not found")
    (out / "spv").mkdir(parents=True, exist_ok=True)
    names = []
    for name, key in sorted(keys.items()):
        source = out / "spv" / f"{name}.frag"
        source.write_text(module_source(key, live_fog=live_fog))
        if (alpha_name := name + ALPHA_SUFFIX) in with_alpha:
            alpha_source = out / "spv" / f"{alpha_name}.frag"
            alpha_source.write_text(module_source(key, alpha_test=True, live_fog=live_fog))
            compile_fragment(alpha_source, out / "spv" / f"{alpha_name}.spv", glslang)
            alpha_source.unlink()
            names.append(alpha_name)
        compile_fragment(source, out / "spv" / f"{name}.spv", glslang)
        source.unlink()
        names.append(name)
    (out / "fsh_table.inc").write_text(render_inc(names))
    return names


def keys_for_blocks(
    blocks: Sequence[bytes], *, live_fog: bool = False
) -> tuple[dict[str, tuple[int, ...]], dict[str, int]]:
    """Module name to key for every block the replay can draw, and refusal counts by cause."""
    keys: dict[str, tuple[int, ...]] = {}
    refused: dict[str, int] = {}
    for block in blocks:
        try:
            _, _, key = replay_form(block, live_fog=live_fog)
        except cfg.UnsupportedConfig as error:
            refused[error.cause] = refused.get(error.cause, 0) + 1
        except ReplayRefusal as error:
            refused[error.cause] = refused.get(error.cause, 0) + 1
        else:
            keys[module_name(key)] = key
    return keys, refused


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a_combiner.replay_modules",
        description="Generate the combiner fragment modules of the pushbuffer replay.",
    )
    parser.add_argument("--out", type=Path, default=Path("generated/shaders/combiner/replay"))
    parser.add_argument("--glslang", default="glslangValidator")
    sub = parser.add_subparsers(dest="mode", required=True)
    corpus_parser = sub.add_parser("corpus", help="every definition the title can set")
    corpus_parser.add_argument("xbe", type=Path)
    blocks_parser = sub.add_parser("blocks", help="concatenated 240-byte definitions")
    blocks_parser.add_argument("file", type=Path)
    args = parser.parse_args(argv)
    if args.mode == "blocks":
        raw = args.file.read_bytes()
        blocks = [
            raw[offset : offset + cfg.BLOCK_BYTES] for offset in range(0, len(raw), cfg.BLOCK_BYTES)
        ]
    else:
        from tools.nv2a_combiner import cli, corpus
        from tools.shaderscan.image import Image

        addresses = cli.default_addresses()
        image = Image.load(args.xbe)
        static = corpus.static_definitions(image, addresses.setter)
        generated = corpus.generated_definitions(args.xbe, image, addresses)
        blocks = [*static.values(), *generated.blocks.values()]
    keys, refused = keys_for_blocks(blocks)
    try:
        names = write_modules(keys, args.out, glslang=args.glslang)
    except GenerationError as error:
        print(error, file=sys.stderr)
        return 1
    print(f"{len(blocks)} definitions, {len(names)} modules written to {args.out}")
    for cause, count in sorted(refused.items()):
        print(f"  refused {cause}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
