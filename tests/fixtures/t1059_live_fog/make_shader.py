"""Synthetic original/xemu-discriminated fog profile; no title bytes."""

import struct
import subprocess
import sys
from pathlib import Path

from tests.fixtures.t1039_point_probe.make_shader import program
from tools.nv2a import translate
from tools.nv2a_combiner import replay_modules


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    body = program(False)
    # MOV oFog,v3: v3.x is independently supplied distance.
    explicit_body = (
        body[:-16]
        + struct.pack("<4I", 0, 0x0020041B, 0x0836186C, 0x0000F818)
        + struct.pack("<4I", 0, 0x0020061B, 0x0836186C, 0x0000F829)
    )
    words = [0] * 60
    words[53] = 1
    words[8], words[9] = 0x13040300, 0x00001400
    block = struct.pack("<60I", *words)
    _, _, key = replay_modules.replay_form(block, live_fog=True)
    name = replay_modules.module_name(key)
    alpha_words = words.copy()
    alpha_words[8] = 0x00000013  # final D=fog.alpha, no fog RGB read
    _, _, alpha_key = replay_modules.replay_form(struct.pack("<60I", *alpha_words), live_fog=True)
    alpha_name = replay_modules.module_name(alpha_key)
    sources = {
        "fog.vert": translate.vertex_shader_source(
            translate.translate(body), live_raster=True, live_fog=True, output_init="nv"
        ),
        "explicit.vert": translate.vertex_shader_source(
            translate.translate(explicit_body), live_raster=True, live_fog=True, output_init="nv"
        ),
        "alpha.frag": replay_modules.module_source(alpha_key, live_fog=True),
        "fog.frag": replay_modules.module_source(key, live_fog=True),
    }
    for filename, source in sources.items():
        path = out / filename
        path.write_text(source)
        subprocess.run(
            [
                "glslangValidator",
                "-V",
                "--target-env",
                "vulkan1.1",
                "-o",
                str(out / (filename + ".spv")),
                str(path),
            ],
            check=True,
        )
    import hashlib

    vertex_name = (
        "static_" + hashlib.sha256(struct.pack("<2H", 0x2078, len(body) // 16) + body).hexdigest()
    )
    explicit_name = (
        "static_" + hashlib.sha256(struct.pack("<2H", 0x2078, 3) + explicit_body).hexdigest()
    )
    (out / "names.h").write_text(
        f'#define FOG_VERTEX_NAME "{vertex_name}"\n'
        f'#define FOG_EXPLICIT_NAME "{explicit_name}"\n'
        f'#define FOG_FRAGMENT_NAME "{name}"\n'
        f'#define FOG_ALPHA_NAME "{alpha_name}"\n'
    )


if __name__ == "__main__":
    main()
