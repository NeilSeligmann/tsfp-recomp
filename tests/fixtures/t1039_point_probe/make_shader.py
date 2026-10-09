"""Generate synthetic point shaders through the actual live translator, no retail bytes."""

from __future__ import annotations

import argparse
import struct
import subprocess
from pathlib import Path

from tools.nv2a import translate

# Hand-derived MOV oPos,v1; MOV oD0,v2. Optional MOV oPts,v2 distinguishes
# the fixed-state override from merely preserving the program-written size.
POSITION = (0, 0x0020021B, 0x0836186C, 0x0000F800)
COLOUR = (0, 0x0020041B, 0x0836186C, 0x0000F818)
POINT = (0, 0x0020041B, 0x0836186C, 0x0000F831)


def program(with_point: bool) -> bytes:
    rows = (
        [POSITION, COLOUR, POINT]
        if with_point
        else [POSITION, tuple(COLOUR[:3]) + (COLOUR[3] | 1,)]
    )
    return b"".join(struct.pack("<4I", *row) for row in rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for with_point in (False, True):
        stem = "with_point" if with_point else "without_point"
        source = args.out / (stem + ".vert")
        source.write_text(
            translate.vertex_shader_source(
                translate.translate(program(with_point)), live_raster=True, output_init="nv"
            )
        )
        subprocess.run(
            [
                "glslangValidator",
                "-V",
                "--target-env",
                "vulkan1.1",
                "-S",
                "vert",
                "-o",
                str(args.out / (stem + ".spv")),
                str(source),
            ],
            check=True,
        )


if __name__ == "__main__":
    main()
