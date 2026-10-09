# ruff: noqa: E501
"""T1490: synthetic texture mode fixtures for test_t1490_texture_modes, no retail bytes.

Writes into DIR: `vertex.spv` (v1 to oPos, v2 to oD0, v9..v12 to oT0..oT3, through the actual live vertex translator) and one
`<combiner_name>.spv` per case through `tools.nv2a_combiner.replay_modules` (the module the live host would make), plus
`manifest.txt` with a `vertex <program name>` line and one `case name` line per case, so the C test proves its own plan names equal these (the key parity).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
from pathlib import Path

from tools.nv2a import translate
from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import replay_modules

PASS = {0: 0xD4300000, 45: 0xC0, 26: 0xC0, 53: 0x11101, 8: 0x0C, 9: 0x1C80}


def read_stage(stage: int) -> dict[int, int]:
    """Spare0 = t<stage> rgb, final D = spare0, G = v0 alpha (the T791 `texture_words` with another register)."""
    return PASS | {34: ((8 + stage) << 24) | 0x00200000}


#: case -> (combiner words, stage program 0x1E70, dot mapping 0x1E74, other stage input 0x1E78)
CASES: dict[str, tuple[dict[int, int], int, int, int]] = {
    "control2d": (read_stage(0), 1, 0, 0),
    "cube": (read_stage(1), 3 << 5, 0, 0),
    # the title's texm3x2 pair (stages 2 and 3, mapping 0x110, normal map in stage 1): the program is
    # 0x4C421 = (1, 1, 17, 9), stage 0 is not read and stage 1 only as the input of the dot stages
    "dot_pair": (read_stage(3), 0x4C421, 0x110, 0x00110000),
    "dot_zero_to_one": (read_stage(3), 0x4C421, 0x000, 0x00110000),
    "dot_sign2": (read_stage(3), 0x4C421, 0x220, 0x00110000),
    "dot_sign3": (read_stage(3), 0x4C421, 0x330, 0x00110000),
    # stage 0 instead of stage 1 is the normal map (the title's other definition, 0x4C401)
    "dot_from_stage0": (read_stage(3), 0x4C401, 0x110, 0x00000000),
}


def vertex_program() -> bytes:
    def row(source: int, address: int, final: bool) -> tuple[int, int, int, int]:
        return (
            0,
            0x00200000 | (source << 9) | 0x1B,
            0x0836186C,
            0xF800 | (address << 3) | int(final),
        )

    rows = [row(1, 0, False), row(2, 3, False)]
    rows += [row(9 + n, 9 + n, n == 3) for n in range(4)]
    return b"".join(struct.pack("<4I", *r) for r in rows)


def block(words: dict[int, int], program: int, mapping: int, other: int) -> bytes:
    dwords = [0] * cfg.BLOCK_DWORDS
    for index, value in (words | {54: program, 55: mapping, 56: other}).items():
        dwords[index] = value
    return struct.pack(f"<{cfg.BLOCK_DWORDS}I", *dwords)


def compile_spirv(source: Path, out: Path, stage: str) -> None:
    subprocess.run(
        [
            "glslangValidator",
            "-V",
            "--target-env",
            "vulkan1.1",
            "-S",
            stage,
            "-o",
            str(out),
            str(source),
        ],
        check=True,
        capture_output=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    vertex = args.out / "vertex.vert"
    vertex.write_text(
        translate.vertex_shader_source(
            translate.translate(vertex_program()), live_raster=True, output_init="nv"
        )
    )
    compile_spirv(vertex, args.out / "vertex.spv", "vert")
    program = vertex_program()
    # the live program name: sha256 of (GPU_PGRAPH_PROGRAM_HEADER_VERSION 0x2078, slot count, the rows), src/gpu/gpu_pgraph_replay.c
    digest = hashlib.sha256(struct.pack("<HH", 0x2078, len(program) // 16) + program).hexdigest()
    manifest = [f"vertex static_{digest}"]
    for case, (words, program, mapping, other) in CASES.items():
        _, _, key = replay_modules.replay_form(block(words, program, mapping, other))
        name = replay_modules.module_name(key)
        source = args.out / f"{name}.frag"
        source.write_text(replay_modules.module_source(key))
        compile_spirv(source, args.out / f"{name}.spv", "frag")
        manifest.append(f"{case} {name}")
    (args.out / "manifest.txt").write_text("\n".join(manifest) + "\n")


if __name__ == "__main__":
    main()
