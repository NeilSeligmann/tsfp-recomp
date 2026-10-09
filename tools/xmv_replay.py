# SPDX-License-Identifier: GPL-3.0-or-later
"""T462: the first XMV movie frames' recorded stream through the strict swap replay.

Boots the retail disc through both logo movies (`tests/test_movie_loop_boot.py`'s flags, cut by
guest progress with `--stop-after-calls 0x3D8E50:N`) and reports, from the host's own output:

* the frame profile's list of methods the decoder does not interpret (the lenient decoder with the
  combiner and the immediate group on) for the cut, so what a strict replay would still refuse is a
  table;
* a STRICT replay (`--gpu-replay` with every output group, the combiner and the announced stand-in
  texture): how many presents it replayed, the first refusal when it stops, the picture of the last
  presented frame;
* a lenient replay, listing every method still logged UNHANDLED and the first frame that carried it.

The vertex-program modules come from the user's corpus (`--corpus`) plus the library programs the
corpus build adds (the copy composition's two instructions, `tools/nv2a/corpus.py LIBRARY_PROGRAMS`,
made here when the corpus predates them), the combiner modules from the title's own definitions and
from the census of the cut. Frames are written under `--work-dir` and never committed. The copy
composition samples the render target as a texture, which no replay provides (T510), so the stand-in
texture (NOT the title's, T497) draws it: the PICTURE IS NOT THE MOVIE.

Two more boots follow, with the combiner and the output groups and NO stand-in (T510): a render
target texture CENSUS (`--gpu-replay-rt-texture-census`, census only: it replays no pixel, so no
picture comes from it) and a STRICT render target texture replay (`--gpu-replay-rt-texture`), whose
first refusal is printed verbatim. That refusal is a measurement, not a failure, and does not change
the exit status.

Exit status: 0 the strict replay replayed every present of the cut, 1 it refused one, 2 an input is
missing, 3 no Vulkan device could be opened.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from tools import steady_replay as steady
from tools.nv2a import corpus as corpus_tool
from tools.shaderscan.image import Image

ROOT = Path(__file__).resolve().parents[1]
SWAP_ADDRESS = steady.SWAP_ADDRESS
# tests/test_movie_loop_boot.py: the T394 boot that plays both logo movies, minus the
# movie-specific budget below.
MOVIE_FLAGS = (
    "--ac97-ready", "--headless-streams", "--headless-buffers", "--headless-listener",
    "--headless-second-vblank", "--native-shader-assembler", "--native-xmv",
    "--headless-movie-audio",
    "--couple-vblank-effects", "--check-vblank-quiescence", "--overlay-consume",
    "--vblank-owner-waits", "429",
)  # fmt: skip
STRICT_OPTIONS = (steady.OUTPUT_STATE, steady.COMBINER, "--gpu-replay-viewport-from-target")
DEFAULT_STANDIN = steady.DEFAULT_STANDIN


@dataclass
class Boot:
    output: str
    returncode: int
    directory: Path | None


def run(args: argparse.Namespace, work: Path, label: str, swaps: int, replay: Sequence[str] | None,
        spv: Path | None) -> Boot:  # fmt: skip
    """One cut boot of the movie loop. `replay` None is the frame profile run, else the swap replay
    with those flags."""
    hdd = Path(tempfile.mkdtemp(prefix=f"{label}-hdd-", dir=work))
    command = [str(args.host), str(args.xbe), "--hdd", str(hdd), *MOVIE_FLAGS, "--thread-timeout",
               str(args.thread_timeout), "--profile-calls", "--stop-after-calls",
               f"0x{SWAP_ADDRESS:X}:{swaps}", "--disc", str(args.disc)]  # fmt: skip
    dump = None
    if replay is not None:
        assert spv is not None
        dump = work / f"{label}-frames"
        shutil.rmtree(dump, ignore_errors=True)
        dump.mkdir(parents=True)
        command += ["--gpu-replay", str(spv), "--gpu-replay-dump", str(dump), *replay]
        if args.window_to_clip:
            command.append(steady.WINDOW_TO_CLIP)
    environment = {**os.environ, "VKRUN_DEVICE": args.device} if args.device else None
    try:
        done = subprocess.run(  # noqa: S603
            command, capture_output=True, text=True, errors="replace", timeout=args.timeout,
            env=environment, check=False,
        )  # fmt: skip
    finally:
        shutil.rmtree(hdd, ignore_errors=True)
    output = done.stdout + done.stderr
    if "requires the compiled retained XMV profile" in output:
        raise steady.Unavailable(
            "the host has no retained XMV chunk (the native XMV flags are refused)"
        )
    return Boot(output, done.returncode, dump)


def prepare_library_modules(
    xbe: Path, directory: Path, work: Path, glslang: str, *, window_to_clip: bool = False
) -> list[str]:
    """Make the vertex modules of `corpus.LIBRARY_PROGRAMS` that `directory` lacks and copy them in.

    The user's corpus may predate them: the digest the replay looks for is the SHA-256 of the
    program with its header, so each is written into a scratch corpus as a static program and run
    through the same `vsh_modules` as the rest."""
    import hashlib

    from tools.nv2a import vsh_modules

    if shutil.which(glslang) is None:
        raise steady.Unavailable(f"{glslang} not found (sudo apt-get install -y glslang-tools)")
    programs = corpus_tool.library_programs(Image.load(xbe))
    missing = {
        hashlib.sha256(data).hexdigest(): (data, address)
        for _, address, data in programs
        if not (directory / f"static_{hashlib.sha256(data).hexdigest()}.spv").exists()
    }
    if not missing:
        return []
    scratch = work / "library-corpus"
    for kind in ("static", "generated"):
        (scratch / kind).mkdir(parents=True, exist_ok=True)
    for digest, (data, _) in missing.items():
        (scratch / "static" / f"{digest}.bin").write_bytes(data)
    static = {digest: [hex(address)] for digest, (_, address) in missing.items()}
    (scratch / "manifest.json").write_text(
        json.dumps({"static": static, "key_mask": 0xFFFF, "generated_keys": {}})
    )
    options = {"window_to_clip": True} if window_to_clip else {}
    vsh_modules.generate(scratch, work / "library-vsh", spirv=True, glslang=glslang, **options)
    made = []
    for digest in missing:
        shutil.copy(work / "library-vsh" / "spv" / f"static_{digest}.spv", directory)
        made.append(f"static_{digest}")
    return made


def render_picture(summary: dict[str, object]) -> str:
    top = ", ".join(f"{colour} x{count}" for colour, count in summary["top"])  # type: ignore[attr-defined]
    return f"{summary['width']}x{summary['height']}, {summary['distinct_colours']} colour(s): {top}"


def texture_json(
    census: steady.TextureCensus | None,
    census_stats: steady.ReplayStats | None,
    strict_stats: steady.ReplayStats | None,
) -> dict[str, object]:
    """The T510 boots as JSON. Census only: the census replays no pixel."""
    refusal = steady.parse_texture_refusal(strict_stats.stopped) if strict_stats else None
    return {
        "census_only": True,
        "census": (
            {
                "frames": census.frames,
                "classified": census.classified,
                "lines": [
                    {
                        "category": steady.texture_census_category(line.text),
                        **dataclasses.asdict(line),
                    }
                    for line in census.lines
                ],
                "by_category": steady.texture_census_by_category(census),
            }
            if census
            else None
        ),
        "census_counters": dataclasses.asdict(census_stats) if census_stats else None,
        "strict": (
            dataclasses.asdict(strict_stats) | {"first_refusal": refusal} if strict_stats else None
        ),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--host", default="build/tsfp_host", help="a tsfp_host with the shader and XMV chunks"
    )
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument("--disc", default="discs/tsfp-xbox.iso", help="the user's Xbox disc image")
    parser.add_argument(
        "--swaps", type=int, default=12, help="cut after N Swap dispatches (the first XMV frames)"
    )
    parser.add_argument("--corpus", default="generated/shaders/corpus")
    parser.add_argument("--spv-dir", help="a ready directory of module files (skips generating)")
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument(
        "--device", default="", help="$VKRUN_DEVICE for the replay (hardware, llvmpipe)"
    )
    parser.add_argument(
        "--work-dir", help="where frames and modules go (default: a temp directory)"
    )
    parser.add_argument("--timeout", type=int, default=600, help="seconds per boot")
    parser.add_argument(
        "--thread-timeout", type=int, default=120000, help="the host's watchdog in ms"
    )
    parser.add_argument(
        "--standin", default=DEFAULT_STANDIN, help="WxH:RRGGBBAA, NOT the title's texture"
    )
    parser.add_argument(
        "--window-to-clip", action="store_true",
        help="T560, INFERRED, opt-in: build the modules with --window-to-clip and replay with "
        "--gpu-replay-window-to-clip, so the composition's window coordinates cover the target",
    )  # fmt: skip
    parser.add_argument(
        "--surface-model", action="store_true",
        help="T633/T596, opt-in (xemu-level rules of T736): three more boots, the census with "
        "--gpu-replay-surface-source, a strict boot with it and one with "
        "--gpu-replay-target-persist as well, reporting the first refusal each reaches",
    )  # fmt: skip
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    args = parser.parse_args(argv)
    try:
        return measure(args)
    except steady.Unavailable as error:
        print(f"xmv_replay: {error}", file=sys.stderr)
        return 2
    except steady.NoDevice as error:
        print(f"xmv_replay: no Vulkan device: {error}", file=sys.stderr)
        return 3


def measure(args: argparse.Namespace) -> int:
    for path in (Path(args.host), Path(args.xbe), Path(args.disc)):
        if not path.exists():
            raise steady.Unavailable(f"{path} does not exist")
    work = Path(args.work_dir) if args.work_dir else Path(tempfile.mkdtemp(prefix="xmv-replay-"))
    work.mkdir(parents=True, exist_ok=True)
    with contextlib.redirect_stdout(
        sys.stderr
    ):  # the module tools print their progress, --json owns stdout
        if args.spv_dir:
            spv = Path(args.spv_dir)
        else:
            spv = steady.prepare_modules(
                Path(args.corpus), work, args.glslang, window_to_clip=args.window_to_clip
            )
            steady.prepare_combiner_modules(Path(args.xbe), spv, work, args.glslang)
        library = prepare_library_modules(
            Path(args.xbe), spv, work, args.glslang, window_to_clip=args.window_to_clip
        )
    standin = ("--gpu-replay-standin-texture", args.standin)
    profile = run(args, work, "profile", args.swaps, None, None)
    unhandled = steady.parse_profile_unhandled(profile.output)
    definitions = steady.parse_standin_definitions(profile.output)
    with contextlib.redirect_stdout(sys.stderr):
        made = (
            steady.prepare_standin_modules(definitions, spv, work, args.glslang)
            if definitions
            else []
        )
    strict = run(args, work, "strict", args.swaps, (*STRICT_OPTIONS, *standin), spv)
    stats = steady.check_device(steady.parse_replay_stats(strict.output), strict.output)
    lenient = run(
        args, work, "lenient", args.swaps, (*STRICT_OPTIONS, *standin, "--gpu-replay-lenient"), spv
    )
    lenient_stats = steady.check_device(steady.parse_replay_stats(lenient.output), lenient.output)
    still = steady.parse_replay_unhandled_frames(lenient.output)
    census_boot = run(
        args, work, "rt-census", args.swaps,
        (*STRICT_OPTIONS, "--gpu-replay-lenient", steady.RT_TEXTURE_CENSUS), spv,
    )  # fmt: skip
    rt_boot = run(args, work, "rt-strict", args.swaps, (*STRICT_OPTIONS, steady.RT_TEXTURE), spv)
    texture_census = steady.parse_texture_census(census_boot.output)
    rt_stats = steady.parse_replay_stats(rt_boot.output)
    surface = None
    if args.surface_model:
        # T633/T596, INFERRED, opt-in: the census is lenient like the one above (it never latches)
        surface = steady.run_surface_boots(
            STRICT_OPTIONS,
            lambda label, flags: run(
                args, work, label, args.swaps,
                (*flags, "--gpu-replay-lenient") if steady.RT_TEXTURE_CENSUS in flags else flags,
                spv,
            ),
        )  # fmt: skip
    pictures = {}
    if strict.directory is not None:
        for path in sorted(strict.directory.glob("frame_??????_target_*.png"))[-2:]:
            pictures[path.name] = steady.png_summary(path)
    result = {
        "swaps": args.swaps,
        "profile_unhandled": {f"{method:04X}": count for method, count in unhandled.items()},
        "library_modules": library,
        "standin_modules": made,
        "window_to_clip_announced": "INFERRED (T560)" in strict.output,
        "strict": dataclasses.asdict(stats),
        "lenient": dataclasses.asdict(lenient_stats),
        "still_unhandled_first_frame": {
            f"{method:04X}": frame for method, frame in sorted(still.items())
        },
        "pictures": pictures,
        "render_target_texture": texture_json(
            texture_census, steady.parse_replay_stats(census_boot.output), rt_stats
        ),
        "surface_model": steady.surface_json(surface),
        "work_dir": str(work),
    }
    if args.json:
        print(json.dumps(result, indent=1))
    else:
        print(f"cut after {args.swaps} Swap dispatches, work directory {work}")
        print(
            f"modules made here: {len(library)} library program(s), "
            f"{len(made)} stand-in combiner(s)"
        )
        print(f"frame profile: {len(unhandled)} method(s) the lenient decoder does not interpret: "
              + " ".join(f"{method:04X}" for method in unhandled))  # fmt: skip
        print(
            f"strict replay: presents {stats.presents}, replayed {stats.replayed}, "
            f"empty {stats.empty}, refused {stats.refused}, skipped {stats.skipped}, "
            f"draws {stats.draws}, dumps {stats.dumps}"
        )
        print(
            f"strict replay stopped: {stats.stopped}"
            if stats.stopped
            else "strict replay did not refuse a frame"
        )
        print(f"lenient replay: {len(still)} method(s) still logged UNHANDLED: "
              + " ".join(f"{method:04X}@{frame}"
                         for method, frame in sorted(still.items())))  # fmt: skip
        for name, summary in pictures.items():
            print(f"picture {name}: {render_picture(summary)}")
        print("\n".join(steady.render_texture_census(texture_census)))
        print("\n".join(steady.render_texture_strict(rt_stats)))
        if surface is not None:
            print("\n".join(steady.render_surface_model(surface)))
    return 1 if stats.refused else 0


if __name__ == "__main__":
    raise SystemExit(main())
