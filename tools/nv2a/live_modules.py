# SPDX-License-Identifier: GPL-3.0-or-later
"""T847: make ONE shader module on demand for the live renderer (`--gpu-live-translate`).

The host (`src/gpu/live_module_maker.c`) calls this as a subprocess when a draw binds a vertex
program or a register combiner configuration that is in no module table, and loads the module
from the directory afterwards:

    python -m tools.nv2a.live_modules vertex   --out DIR --expect generated_<sha256> FILE
    python -m tools.nv2a.live_modules combiner --out DIR --expect combiner_<sha256> FILE

`vertex` FILE is the program as the replay digests it (header 0x2078, count, instructions), the
translator is `tools/nv2a/translate.py` through `vsh_modules.make_program_module` (raw viewport,
zero output init, the defaults of the T95 corpus modules). `combiner` FILE is one 240 byte
definition block (the 57 state words and three zeros), the translator is
`tools/nv2a_combiner/replay_modules.py`. Both write `<name>.spv` into DIR atomically (a reader never
sees half a module) and refuse by name when the module the translator makes is not the one the host
asked for. Output is derived from the user's own data: DIR must be gitignored.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

from tools.nv2a import translate, vsh_modules
from tools.nv2a_combiner import config as combiner_config
from tools.nv2a_combiner import replay_modules


class MakeError(Exception):
    pass


def make_vertex(
    program: bytes, out: Path, expect: str, glslang: str, *, live_raster: bool = False
) -> Path:
    try:
        made = vsh_modules.make_program_module(
            program,
            out,
            glslang=glslang,
            live_raster=live_raster,
            live_fog=live_raster,
            output_init="nv" if live_raster else "zero",
        )
    except translate.Unsupported as error:
        raise MakeError(f"the translator refuses this program: {error}") from error
    except vsh_modules.TableError as error:
        raise MakeError(str(error)) from error
    if made.stem != expect:
        made.unlink(missing_ok=True)
        raise MakeError(f"the program's digest names {made.stem}, the host asked for {expect}")
    return made


def make_combiner(
    block: bytes, out: Path, expect: str, glslang: str, *, live_fog: bool = False
) -> Path:
    if len(block) != combiner_config.BLOCK_BYTES:
        raise MakeError(
            f"a definition block is {combiner_config.BLOCK_BYTES} bytes, got {len(block)}"
        )
    keys, refused = replay_modules.keys_for_blocks([block], live_fog=live_fog)
    if not keys:
        causes = ", ".join(sorted(refused)) or "unknown"
        raise MakeError(f"the combiner translator refuses this configuration: {causes}")
    # T860: `<name>_alpha` is the variant that runs the alpha test in the module
    alpha = expect.endswith(replay_modules.ALPHA_SUFFIX)
    base = expect.removesuffix(replay_modules.ALPHA_SUFFIX)
    if base not in keys:
        raise MakeError(f"the definition names {', '.join(keys)}, the host planned {expect}")
    out.mkdir(parents=True, exist_ok=True)
    final = out / f"{expect}.spv"
    if final.exists():
        return final
    with tempfile.TemporaryDirectory(dir=out, prefix=f".{expect}.") as work:
        try:
            replay_modules.write_modules(
                {base: keys[base]},
                Path(work),
                glslang=glslang,
                with_alpha=frozenset({expect}) if alpha else frozenset(),
                live_fog=live_fog,
            )
        except replay_modules.GenerationError as error:
            raise MakeError(str(error)) from error
        (Path(work) / "spv" / f"{expect}.spv").replace(final)
    return final


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("vertex", "combiner"))
    parser.add_argument("file", type=Path)
    parser.add_argument("--out", type=Path, required=True, help="the module directory")
    parser.add_argument("--expect", required=True, help="the module name the host asked for")
    parser.add_argument("--glslang", default=os.environ.get("TSFP_GLSLANG", "glslangValidator"))
    parser.add_argument(
        "--live-raster",
        action="store_true",
        help="INFERRED xemu-derived programmable raster map; versioned live cache only",
    )
    args = parser.parse_args(argv)
    try:
        data = args.file.read_bytes()
        make = make_vertex if args.kind == "vertex" else make_combiner
        made = make(
            data,
            args.out,
            args.expect,
            args.glslang,
            **(
                {"live_raster": True}
                if args.kind == "vertex" and args.live_raster
                else {"live_fog": True}
                if args.kind == "combiner" and args.live_raster
                else {}
            ),
        )
    except (MakeError, OSError) as error:
        print(f"live_modules: {error}", file=sys.stderr)
        return 1
    print(made)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
