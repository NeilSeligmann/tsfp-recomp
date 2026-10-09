# SPDX-License-Identifier: GPL-3.0-or-later
"""`python -m tools.play --disc DISC`: build if needed, run the title on the host, report the stop.

The run is headless unless `--window` is given, which asks the host for `--present window` (T760)
and `--audio-sink sdl` (T761), SDL3 builds only (the DirectSound HLE emits no PCM yet, T759,
so the device plays silence today). The host itself refuses with the named reason when built
without SDL3. Overlay pictures also go to PNG files in the per-run directory. The tool only
builds a command line for the host, it never changes a host default.
"""

import argparse
import hashlib
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import IO

from tools.play import flags, route_check, route_nav, route_wait, stop
from tools.play.flags import Flag
from tools.snapshot import options as snapshot_options

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = Path("build/play")
DEFAULT_XBE = Path("build/default.xbe")
RUNS = Path("tmp/play")
SNAPSHOTS = Path("tmp/snapshots")
# T847: the live renderer's shader modules are derived from the user's own disc, so they live in a
# gitignored cache that outlives the run directories (a module is named by the digest of its input,
# so a module made once serves every later run).
LIVE_MODULES = RUNS / "modules"
GLSLANG = "glslangValidator"


def _tee(
    source: IO[bytes], path: Path, log: IO[bytes], lock: threading.Lock, mirror: IO[str] | None
) -> None:
    with path.open("wb") as sink:
        for line in iter(source.readline, b""):
            sink.write(line)
            sink.flush()
            with lock:
                log.write(line)
                log.flush()
                if mirror is not None:
                    mirror.write(line.decode(errors="replace"))
                    mirror.flush()


def run_host(
    command: list[str],
    timeout: int | None,
    run_dir: Path,
    console: bool,
    log_file: Path | None = None,
) -> int:
    """Run the host under a hard timeout, writing host.out, host.err and the interleaved run.log.

    `console` also mirrors both streams to the terminal live.
    """
    full = (
        ["timeout", "--kill-after=5s", f"{timeout}s", *command] if timeout is not None else command
    )
    # TSFP_PYTHON: the interpreter the host runs the on-demand translator with (T847)
    env = {**os.environ, "TSFP_PYTHON": sys.executable}
    process = subprocess.Popen(  # noqa: S603
        full,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    lock = threading.Lock()
    log_path = log_file if log_file is not None else run_dir / "run.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("wb") as log:
        pumps = [
            threading.Thread(
                target=_tee,
                args=(
                    process.stdout,
                    run_dir / "host.out",
                    log,
                    lock,
                    sys.stdout if console else None,
                ),
            ),
            threading.Thread(
                target=_tee,
                args=(
                    process.stderr,
                    run_dir / "host.err",
                    log,
                    lock,
                    sys.stderr if console else None,
                ),
            ),
        ]
        for pump in pumps:
            pump.start()
        try:
            code = process.wait()
        except KeyboardInterrupt:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            code = 130
        for pump in pumps:
            pump.join()
    return code


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        allow_abbrev=False,
        prog="python -m tools.play",
        description="Run the title on the host (T741).",
    )
    parser.add_argument(
        "--disc", type=Path, help="user-owned Xbox disc image (or extracted dir, T742)"
    )
    parser.add_argument("--xbe", type=Path, default=DEFAULT_XBE, help="boot default.xbe")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD, help="cmake build dir")
    parser.add_argument("--host", type=Path, help="use this tsfp_host binary, skip the build")
    parser.add_argument(
        "--skip-intro",
        action="store_true",
        help="FABRICATED shortcut: terminate startup logos through original cleanup; default off",
    )
    parser.add_argument(
        "--xonline-offline",
        action="store_true",
        help="INFERRED offline XONLINE policy: no live accounts or services; default off",
    )
    parser.add_argument("--no-build", action="store_true", help="never run cmake/ninja")
    parser.add_argument("--run-dir", type=Path, help="per-run output dir (default tmp/play/<time>)")
    parser.add_argument(
        "--timeout",
        type=int,
        default=None,
        help="hard kill after N seconds (evidence default 1800; interactive unlimited)",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="live window play past callback evidence budgets (FABRICATED wall timing), "
        "keyboard by default",
    )
    parser.add_argument(
        "--vblank-owner-waits", type=int, help="owner callback evidence budget (1..1000000)"
    )
    parser.add_argument(
        "--vblank-worker-blanks", type=int, help="worker callback evidence budget (1..1000000)"
    )
    parser.add_argument("--strict", action="store_true", help="drop every opt-in flag")
    parser.add_argument("--enable", action="append", default=[], help="turn a table flag on")
    parser.add_argument("--disable", action="append", default=[], help="turn an opt-in flag off")
    parser.add_argument("--list-flags", action="store_true", help="print the flag table and exit")
    parser.add_argument("--dry-run", action="store_true", help="print the command, do not run it")
    parser.add_argument("--debug", action="store_true", help="longer trace and profile, keep state")
    parser.add_argument(
        "--log-file",
        type=Path,
        help="write the interleaved run log here (default <run-dir>/run.log)",
    )
    parser.add_argument(
        "--console", action="store_true", help="also stream host stdout/stderr to the terminal live"
    )
    parser.add_argument("--no-overlay", action="store_true", help="do not dump overlay pictures")
    parser.add_argument(
        "--present", metavar="SINK", help="host video sink: null, png-dir, window (T744, T760)"
    )
    parser.add_argument(
        "--audio-sink", metavar="SINK", help="host audio sink: null, wav-file, sdl (T744, T761)"
    )
    parser.add_argument(
        "--mute",
        action="store_true",
        help="play silence on the SDL3 audio device (host --audio-mute): the sound clock and the "
        "window pacing are unchanged, needs --window or --audio-sink sdl",
    )
    parser.add_argument(
        "--window", action="store_true", help="live SDL3 window (T760), same as --present window"
    )
    parser.add_argument(
        "--gpu-live",
        action="store_true",
        help="T838 (M9, OPT-IN, INFERRED): the live Vulkan renderer draws the title in the window "
        "(needs --window), same as --enable gpu-live; refused draws are named in the stop report",
    )
    parser.add_argument(
        "--gpu-live-blit",
        action="store_true",
        help="with --gpu-live, T849: present with the swapchain pre-pass blit instead of the "
        "front buffer readback",
    )
    parser.add_argument(
        "--gpu-live-inferred",
        action="store_true",
        help="with --gpu-live, also admit the INFERRED texture formats and sampler states",
    )
    parser.add_argument(
        "--gpu-live-modules",
        type=Path,
        metavar="DIR",
        help="the shader module directory (static_/generated_/combiner_ <sha256>.spv) the live "
        f"renderer draws with and writes the modules it translates on demand into (default: "
        f"{LIVE_MODULES}, a gitignored cache that grows as the title binds new programs)",
    )
    parser.add_argument(
        "--no-gpu-live-translate",
        action="store_true",
        help="T847: do not translate a vertex program or combiner configuration that is in no "
        "module table on demand (every such draw is then refused by name)",
    )
    parser.add_argument(
        "--pad-source",
        choices=("keyboard", "gamepad"),
        help="host pad source from the window (T751, FABRICATED mapping, needs the window, "
        "implies --synthetic-pad)",
    )
    parser.add_argument(
        "--record-input",
        type=Path,
        metavar="FILE",
        help="T1074: record the port 0 pad by poll index to FILE (FABRICATED, implies "
        "--synthetic-pad; with --interactive or --pad-source it records the session, see "
        "docs/input-replay.md)",
    )
    parser.add_argument(
        "--replay-interactive",
        dest="force_replay_interactive",
        action="store_true",
        help="T1126: run the replay in the host's --interactive mode (needs --window). Automatic "
        "for a record whose '# budgets:' line says interactive=1. A record made before T1126 has "
        "no such line, pass this for an interactive recording or the replay stops at the "
        "owner-wait budget",
    )
    parser.add_argument(
        "--replay-input",
        type=Path,
        metavar="FILE",
        help="T1074: replay a --record-input file (FABRICATED, implies --synthetic-pad); the host "
        "refuses a record made against another XBE or flag set",
    )
    parser.add_argument(
        "--replay-handover",
        action="store_true",
        help="T1616: with --replay-input, the live window pad (keyboard, or --pad-source gamepad) "
        "takes over when the recorded inputs end. Implies --interactive and --window. FABRICATED",
    )
    parser.add_argument(
        "--check-route",
        nargs="?",
        const="-",
        metavar="FILE",
        help="T1618: only check the route FILE (default: the --replay-input one) was recorded with "
        "the flags and the XBE of THIS command line, print the differing flags, exit 0 (same) or 2 "
        "(different). Nothing is built or launched. Give the same arguments as the real replay",
    )
    parser.add_argument(
        "--skip-route-check",
        action="store_true",
        help="T1618: do not compare the route's flag identity before launching a replay (the host "
        "still refuses a record made against another XBE or flag set)",
    )
    parser.add_argument(
        "--route-wait",
        action="append",
        default=[],
        metavar="SPEC",
        help="T1616: hold the replay at a recorded mark until a guest memory condition holds, "
        "markK:mem=ADDR[:W]==VALUE[&MASK],min=N,max=N (repeatable, needs --replay-input, "
        "docs/input-replay.md)",
    )
    parser.add_argument(
        "--route-wait-event",
        action="append",
        default=[],
        metavar="SPEC",
        help="T1633: hold the replay at a recorded mark until host observed events hold, "
        "markK:file-open=SUBSTR,file-idle=MS,mem=*ADDR+OFF>=N,call=VA,frame-stable=MS,timeout=MS "
        "(repeatable, needs --replay-input; the record may also carry '# wait:' lines; "
        "docs/input-replay.md 'Event driven route')",
    )
    parser.add_argument(
        "--route-event-log",
        type=Path,
        metavar="FILE",
        help="T1633: the host writes a timestamped log of guest file opens/reads, watched calls, "
        "sampled memory and route events (python -m tools.route_events reads it)",
    )
    parser.add_argument(
        "--route-log-mem",
        action="append",
        default=[],
        metavar="SPEC",
        help="T1633: sample guest memory [*]ADDR[+OFF][:W] into --route-event-log (changes only, "
        "repeatable)",
    )
    parser.add_argument(
        "--route-nav",
        choices=route_nav.MODES,
        metavar="MODE",
        help="T1640: host --route-nav MODE (on, off or strict) for the '# nav:' steps of the "
        "--replay-input record. Default: on, passed only when the record has nav steps and the "
        "host --help shows the marker T1640 (an older host ignores nav steps and plays the "
        "recorded presses)",
    )
    parser.add_argument(
        "--no-route-nav",
        action="store_true",
        help="T1640: opt out of closed loop menu navigation (--route-nav off), the recorded "
        "presses of every nav range play as recorded",
    )
    parser.add_argument(
        "--route-nav-menus",
        type=Path,
        default=Path(route_nav.DEFAULT_MENU_FILE),
        metavar="FILE",
        help="T1640: the menu table (default %(default)s, resolved against the repository root); "
        "passed to the host as an absolute path when it exists",
    )
    parser.add_argument(
        "--route-nav-timing",
        metavar="HOLD:GAP",
        help="T1640: d-pad hold and release polls of a nav step, passed to the host unchanged",
    )
    parser.add_argument(
        "--poke-at-poll",
        action="append",
        default=[],
        metavar="WHERE:LABEL",
        help="T1616: serve the guarded poke request guestpoke.LABEL in the --dump-guest-dir at "
        "poll N, at mark K (markK) or when the replay ends (replay-end). Needs --replay-input and "
        "the host --forced-state (FABRICATED-STATE, T1613 guards)",
    )
    parser.add_argument(
        "--hotkey",
        action="append",
        default=[],
        metavar="SPEC",
        help="T1627/T1632: host hotkey chord DEVICE=KEY+KEY[@MS]:LABEL, passed unchanged to "
        "the host (repeatable, at most 8, needs the window pad: --pad-source or --interactive, "
        "and --hotkey-dir). Labels 'mark' (record mark) and 'stop' (clean shutdown) act in "
        "the host; "
        "python -m tools.hotkey_defaults prints the default chords",
    )
    parser.add_argument(
        "--hotkey-dir",
        type=Path,
        metavar="DIR",
        help="T1627: where the host writes hotkey.<n> (required with --hotkey)",
    )
    parser.add_argument(
        "--shot-dir",
        type=Path,
        metavar="DIR",
        help="T1720b: where the 'shot' hotkey writes shot-NNN.png and shots.manifest "
        "(default: --hotkey-dir)",
    )
    parser.add_argument(
        "--shot-max",
        type=int,
        metavar="N",
        help="T1720b: at most N pictures per run (host default 200)",
    )
    parser.add_argument(
        "--shot-max-bytes",
        type=int,
        metavar="N",
        help="T1720b: at most N PNG bytes per run (host default 512 MiB)",
    )
    parser.add_argument(
        "--controllers",
        action="store_true",
        help="four-port SDL gamepads (T942, opt-in, needs the window); keyboard reserves port 0",
    )
    for option, description in (
        ("--controller-config", "load port assignments, remapping and calibration"),
        ("--controller-mappings", "load local SDL mappings for compatible devices"),
        ("--controller-save", "save the effective controller configuration atomically"),
    ):
        parser.add_argument(
            option, type=Path, metavar="FILE", help=description + " (needs --controllers)"
        )
    snapshot_options.add_arguments(parser)
    args, args.host_passthrough = parser.parse_known_args(argv)
    for name in ("vblank_owner_waits", "vblank_worker_blanks"):
        value = getattr(args, name)
        if value is not None and not 1 <= value <= 1000000:
            parser.error(f"--{name.replace(chr(95), chr(45))} must be 1..1000000")
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.check_route is not None:
        if args.check_route != "-":
            args.replay_input = Path(args.check_route)
        if args.replay_input is None:
            parser.error("--check-route needs a FILE (or --replay-input FILE)")
    if args.replay_handover:
        args.interactive = True
    if args.interactive:
        args.window = True
        if args.pad_source is None:
            args.pad_source = "keyboard"
    args.replay_interactive = False
    args.replay_thread_timeout = 2147483647
    if args.replay_input is not None and args.replay_input.is_file():
        adopt_recorded_budgets(args)
    if args.replay_input is not None and args.force_replay_interactive and not args.interactive:
        args.replay_interactive = True
    return args


BUDGETS_LINE = re.compile(
    r"# budgets: owner-waits=(\d+) worker-blanks=(\d+) thread-timeout=(\d+) interactive=([01])\s*"
)


def recorded_budgets(path: Path) -> dict[str, int] | None:
    """The `# budgets:` header line of an input record (T1126), None for a record made before it."""
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("#"):
                return None
            match = BUDGETS_LINE.fullmatch(line)
            if match:
                keys = ("owner", "worker", "timeout", "interactive")
                return dict(zip(keys, map(int, match.groups()), strict=True))
    return None


def adopt_recorded_budgets(args: argparse.Namespace) -> None:
    """A replay runs under the recording's budgets unless the command line gives its own."""
    recorded = recorded_budgets(args.replay_input)
    if recorded is None:
        return
    if args.vblank_owner_waits is None and 1 <= recorded["owner"] <= 1000000:
        args.vblank_owner_waits = recorded["owner"]
    if args.vblank_worker_blanks is None and 1 <= recorded["worker"] <= 1000000:
        args.vblank_worker_blanks = recorded["worker"]
    args.replay_thread_timeout = recorded["timeout"]
    args.replay_interactive = bool(recorded["interactive"]) and not args.interactive


# The window stays open this long after the stop report (close it or press Escape to end it sooner).
WINDOW_HOLD_MS = 600000


def audio_sink_of(args: argparse.Namespace) -> str | None:
    """The audio sink asked of the host: a named one, else sdl under --window (T761), else none."""
    return args.audio_sink if args.audio_sink is not None else ("sdl" if args.window else None)


def presentation_refusal(args: argparse.Namespace) -> str | None:
    """A reason to refuse the presentation options before anything runs, else None."""
    if args.window and args.present not in (None, "window"):
        return f"--window means --present window, it conflicts with --present {args.present}"
    if args.mute and audio_sink_of(args) != "sdl":
        return "--mute silences the SDL3 audio device, add --window or --audio-sink sdl"
    if args.pad_source is not None and not (args.window or args.present == "window"):
        return "--pad-source reads the window's keyboard or gamepad, add --window"
    if args.hotkey and args.hotkey_dir is None:
        return "--hotkey needs --hotkey-dir (the host writes hotkey.<n> there)"
    if args.hotkey_dir is not None and not args.hotkey:
        return "--hotkey-dir alone does nothing, add --hotkey SPEC"
    shot_flags = (args.shot_dir, args.shot_max, args.shot_max_bytes)
    if any(flag is not None for flag in shot_flags) and not args.hotkey:
        return "--shot-dir/--shot-max/--shot-max-bytes need --hotkey (the host refuses them alone)"
    if args.hotkey and len(args.hotkey) > 8:
        return "at most 8 --hotkey chords"
    if args.hotkey and args.pad_source is None:
        return "--hotkey reads the window's keyboard or gamepad, add --pad-source or --interactive"
    if args.replay_input is None and (
        args.replay_handover or args.route_wait or args.route_wait_event or args.poke_at_poll
    ):
        return (
            "--replay-handover, --route-wait, --route-wait-event and --poke-at-poll act on a "
            "replay, add --replay-input"
        )
    if args.route_log_mem and args.route_event_log is None:
        return "--route-log-mem writes into --route-event-log, add it"
    if args.replay_input is None and (args.route_nav or args.route_nav_timing):
        return "--route-nav and --route-nav-timing act on a replay, add --replay-input"
    if args.replay_input is None and args.record_input is None and args.no_route_nav:
        return (
            "--no-route-nav acts on a replay or a recording, add --replay-input or --record-input"
        )
    if args.no_route_nav and args.route_nav not in (None, "off"):
        return f"--no-route-nav conflicts with --route-nav {args.route_nav}"
    if args.route_nav_timing is not None:
        try:
            route_nav.parse_timing(args.route_nav_timing)
        except route_nav.NavError as error:
            return str(error)
    if args.replay_input is not None:
        if args.record_input is not None or args.controllers:
            return (
                "--replay-input is the pad itself, it conflicts with --record-input "
                "and --controllers"
            )
        if args.pad_source is not None and not args.replay_handover:
            return (
                "--replay-input is the pad itself, it conflicts with --pad-source and "
                "--interactive (add --replay-handover to hand the live pad over after the record)"
            )
        if not args.replay_input.is_file():
            return f"--replay-input {args.replay_input}: no such file"
        if args.replay_interactive and not (args.window or args.present == "window"):
            return (
                "--replay-input: the recording ran --interactive, which waives the owner-wait "
                "budget stop; replay it with --window (headless under Xvfb works) or the replay "
                "stops at a budget the recording never hit"
            )
    if args.record_input is not None and args.controllers:
        return "--record-input records the legacy port 0 pad, it conflicts with --controllers"
    if args.controllers:
        if not (args.window or args.present == "window"):
            return "--controllers needs --window or --present window"
        if args.pad_source == "gamepad":
            return (
                "--controllers conflicts with legacy --pad-source gamepad; "
                "use keyboard or omit --pad-source"
            )
    for name in ("controller_config", "controller_mappings", "controller_save"):
        if getattr(args, name) is not None and not args.controllers:
            return "--" + name.replace("_", "-") + " needs --controllers"
    if args.present == "png-dir" and args.no_overlay:
        return "--present png-dir needs the overlay dump, drop --no-overlay"
    if wants_live(args) and not (args.window or args.present == "window"):
        return "--gpu-live draws in the Vulkan window, add --window"
    if (args.gpu_live_inferred or "gpu-live-inferred" in args.enable) and not wants_live(args):
        return "--gpu-live-inferred needs --gpu-live"
    if (args.gpu_live_blit or "gpu-live-blit" in args.enable) and not wants_live(args):
        return "--gpu-live-blit needs --gpu-live"
    return None


PLACEHOLDER_MODULES = ("static_" + "0" * 64 + ".spv", "combiner_" + "0" * 64 + ".spv")


def ensure_modules(directory: Path) -> None:
    """Make `directory` a module directory the host accepts: with no module at all it refuses,
    so an empty one gets a one vertex and one combiner placeholder (SPIR-V magic only, named for
    a digest no draw has, never selected)."""
    directory.mkdir(parents=True, exist_ok=True)
    if not any(directory.glob("*.spv")):
        for name in PLACEHOLDER_MODULES:
            (directory / name).write_bytes(bytes.fromhex("03022307") + bytes(16))


def wants_live(args: argparse.Namespace) -> bool:
    """--gpu-live or --enable gpu-live (T838)."""
    return bool(args.gpu_live or "gpu-live" in args.enable)


def translate_refusal(args: argparse.Namespace) -> str | None:
    """Why on-demand translation (T847) cannot be asked of the host, else None."""
    if args.no_gpu_live_translate:
        return "--no-gpu-live-translate"
    if shutil.which(GLSLANG) is None:
        return f"{GLSLANG} not found (sudo apt-get install -y glslang-tools)"
    return None


def presentation_flags(args: argparse.Namespace, modules: Path | None = None) -> list[str]:
    """Opt-in host flags, only when asked for. The host validates the sink names itself."""
    extra: list[str] = []
    if args.interactive:
        extra += ["--interactive", "--thread-timeout", "2147483647"]
    elif args.replay_interactive:
        # The recording ran --interactive (owner and worker budget stops waived), so match it
        # without a keyboard pad, the recorded file is the pad (T1126).
        extra += ["--interactive", "--thread-timeout", str(args.replay_thread_timeout)]
    for name in ("vblank_owner_waits", "vblank_worker_blanks"):
        value = getattr(args, name)
        if value is not None:
            extra += ["--" + name.replace("_", "-"), str(value)]
    sink = "window" if args.window else args.present
    if sink is not None:
        extra += ["--present", sink]
    if sink == "window":
        extra += [
            "--present-hold-ms",
            str(0 if args.interactive or args.replay_interactive else WINDOW_HOLD_MS),
        ]
    # --window is the watch-and-listen mode: it also opens the SDL3 audio device (T761) unless a
    # sink was named. The device pulls from the modelled timeline, silence until the HLE mixer
    # (T759) exists.
    audio = audio_sink_of(args)
    if audio is not None:
        extra += ["--audio-sink", audio]
    if args.mute:
        extra += ["--audio-mute"]
    if args.pad_source is not None:
        extra += ["--synthetic-pad", "--pad-source", args.pad_source]
    if args.controllers:
        extra += ["--controllers"]
    for spec in args.hotkey:
        extra += ["--hotkey", spec]
    if args.hotkey_dir is not None:
        extra += ["--hotkey-dir", str(args.hotkey_dir.resolve())]
    if args.shot_dir is not None:
        extra += ["--shot-dir", str(args.shot_dir.resolve())]
    if args.shot_max is not None:
        extra += ["--shot-max", str(args.shot_max)]
    if args.shot_max_bytes is not None:
        extra += ["--shot-max-bytes", str(args.shot_max_bytes)]
    if args.record_input is not None:
        extra += nav_record_flags(args)
    if args.record_input is not None or args.replay_input is not None:
        extra += ["--synthetic-pad"] if args.pad_source is None else []
    # The host runs from ROOT; the file belongs to the launcher's cwd.
    if args.route_event_log is not None:
        extra += ["--route-event-log", str(args.route_event_log.resolve())]
        for spec in args.route_log_mem:
            extra += ["--route-log-mem", spec]
    if args.record_input is not None:
        extra += ["--record-input", str(args.record_input.resolve())]
    if args.replay_input is not None:
        extra += ["--replay-input", str(args.replay_input.resolve())]
        if args.replay_handover:
            extra += ["--replay-handover"]
        for spec in args.route_wait:
            extra += ["--route-wait", spec]
        for spec in args.route_wait_event:
            extra += ["--route-wait-event", spec]
        for spec in args.poke_at_poll:
            extra += ["--poke-at-poll", spec]
        extra += nav_flags(args)
    for name in ("controller_config", "controller_mappings", "controller_save"):
        path = getattr(args, name)
        if path is not None:
            # The host runs from ROOT; command-line controller paths belong to the launcher's cwd.
            extra += ["--" + name.replace("_", "-"), str(path.resolve())]
    if wants_live(args):
        # the replay's directory and model are the live renderer's inputs, lenient so one unhandled
        # method does not stop the frames
        extra += ["--gpu-replay", str(modules or LIVE_MODULES), "--gpu-replay-lenient"]
        if translate_refusal(args) is None:
            extra += ["--gpu-live-translate"]
    return extra


def nav_menus_file(args: argparse.Namespace) -> Path:
    """T1640: the menu table, a relative path is the repository's (the host runs from ROOT)."""
    path = args.route_nav_menus
    return path if path.is_absolute() else ROOT / path


def nav_record_flags(args: argparse.Namespace) -> list[str]:
    """T1640: the flags that make the host write `# nav:` lines into a recording."""
    found, notes = route_nav.nav_record_flags(
        nav_menus_file(args), args.no_route_nav, list(args.host_passthrough)
    )
    for note in notes:
        print(f"play: {note}", file=sys.stderr)
    return found


def nav_flags(args: argparse.Namespace) -> list[str]:
    """T1640: the --route-nav flags for a record with nav steps."""
    try:
        text = args.replay_input.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    found, notes = route_nav.nav_host_flags(
        text, args.route_nav, nav_menus_file(args), args.route_nav_timing, args.no_route_nav
    )
    for note in notes:
        print(f"play: {note}", file=sys.stderr)
    return found


def _cmake_cache(path: Path) -> dict[str, str]:
    """Read generated cache entries, including CMake's PATH/FILEPATH/INTERNAL types."""
    if not path.is_file():
        return {}
    entries = {}
    for line in path.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        key, value = line.split("=", 1)
        entries[key.split(":", 1)[0]] = value
    return entries


def build_host(build_dir: Path, xbe: Path = DEFAULT_XBE) -> Path:
    """Build against the verified private menu seed profile, preserving canonical code."""
    from tools.play.seed_profile import prepare

    compiler = shutil.which("clang")
    if compiler is None:
        raise RuntimeError("clang is required to build the host")
    # Use one stable absolute spelling. CMake resets a compiler-changed cache even
    # when two differently spelled paths ultimately name the same executable.
    compiler = str(Path(compiler).resolve())
    lifted = prepare(ROOT, ROOT / xbe)
    cache = ROOT / build_dir / "CMakeCache.txt"
    previous = _cmake_cache(cache)
    fresh = []
    if cache.exists() and (
        previous.get("CMAKE_HOME_DIRECTORY") != str(ROOT.resolve())
        or previous.get("CMAKE_C_COMPILER") != compiler
        or previous.get("CMAKE_GENERATOR") != "Ninja"
    ):
        # Clear configuration before passing required options: CMake's automatic
        # compiler reset re-runs without them and can silently select canonical C.
        fresh = ["--fresh"]
    subprocess.run(  # noqa: S603
        [
            "cmake",
            *fresh,
            "-S",
            ".",
            "-B",
            str(build_dir),
            "-G",
            "Ninja",  # noqa: S607
            f"-DCMAKE_C_COMPILER={compiler}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DTSFP_LIFTED_OPT=-O0",
            f"-DTSFP_LIFTED_DIR={lifted}",
        ],
        cwd=ROOT,
        check=True,
    )
    effective = _cmake_cache(cache)
    selected = effective.get("TSFP_LIFTED_DIR")
    if selected is None or Path(selected).resolve() != lifted.resolve():
        raise RuntimeError("CMake did not retain the verified lifted profile; refusing host build")
    if effective.get("CMAKE_C_COMPILER") != compiler:
        raise RuntimeError("CMake did not retain the selected compiler; refusing host build")
    subprocess.run(["ninja", "-C", str(build_dir), "tsfp_host"], cwd=ROOT, check=True)  # noqa: S603, S607
    return build_dir / "tsfp_host"


def host_command(
    host: Path, xbe: Path, hdd: Path, disc: Path, active: list[Flag], extra: list[str]
) -> list[str]:
    """The host command line, all paths as given (relative to the repository root)."""
    return [
        str(host),
        str(xbe),
        "--hdd",
        str(hdd),
        *flags.argv_of(active),
        *extra,
        "--disc",
        str(disc),
    ]


def host_extra(args: argparse.Namespace, overlays: Path, modules: Path) -> list[str]:
    """The host flags outside the flag table: debug, overlay dump, presentation, passthrough."""
    extra = ["--trace", "400", "--profile-calls", "--profile-calls-top", "40"] if args.debug else []
    if not args.no_overlay:
        extra += ["--dump-overlay", str(overlays)]
    extra += presentation_flags(args, modules)
    extra += args.host_passthrough
    if args.snapshot_at_poll is not None:
        extra += ["--snapshot-at-poll", str(args.snapshot_at_poll)]
    return extra


def xbe_digest(xbe: Path) -> str | None:
    """sha256 of the XBE the host will load (a record is bound to it), None when it is missing."""
    path = ROOT / xbe
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def route_refusal(args: argparse.Namespace, command: list[str]) -> str | None:
    """T1618: why the host would refuse `--replay-input` (record header vs `command`), else None."""
    if args.replay_input is None or args.skip_route_check:
        return None
    verdict = route_check.check_route(args.replay_input, command, xbe_digest(args.xbe))
    if not verdict.ok:
        return verdict.message()
    navs = nav_refusal(args)
    if navs is not None:
        waits = wait_refusal(args, command)
        return navs if waits is None else waits + "\n" + navs
    return wait_refusal(args, command)


def wait_refusal(args: argparse.Namespace, command: list[str]) -> str | None:
    """T1633: the waits (record '# wait:' lines and command line) the host would refuse."""
    try:
        record = route_wait.read_record(args.replay_input)
    except OSError:
        return None
    waits = [("--route-wait", spec) for spec in args.route_wait]
    waits += [("--route-wait-event", spec) for spec in args.route_wait_event]
    problems = route_wait.validate(record, waits, command, args.poke_at_poll)
    if not problems:
        return None
    head = f"route {args.replay_input}: the waits would be refused by the host:"
    return head + "\n  " + "\n  ".join(problems)


def nav_refusal(args: argparse.Namespace) -> str | None:
    """T1640: the '# nav:' lines of the record the host would refuse."""
    try:
        text = args.replay_input.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    if not route_nav.has_nav_lines(text):
        return None
    table = nav_menus_file(args)
    menus = None
    if table.is_file():
        try:
            menus = route_nav.load_menu_table(table)
        except route_nav.NavError as error:
            return f"menu table {table}: {error}"
    problems = route_nav.validate_route(text, menus)
    if not problems:
        return None
    head = f"route {args.replay_input}: the nav steps would be refused by the host:"
    return head + "\n  " + "\n  ".join(problems)


def check_route_only(
    args: argparse.Namespace, active: list[Flag], hdd: Path, extra: list[str]
) -> int:
    """T1618 `--check-route`: compare the route with this command line, build and launch nothing."""
    command = host_command(Path("tsfp_host"), args.xbe, hdd, args.disc, active, extra)
    refusal = route_refusal(args, command)
    if refusal is not None:
        print(f"play: {refusal}", file=sys.stderr)
        return 2
    print(f"play: route {args.replay_input} matches the flags and the XBE of this command line")
    return 0


def write_repro(run_dir: Path, command: list[str]) -> Path:
    """One shell line, run from the repository root, with a fresh HDD dir of the same name."""
    path = ROOT / run_dir / "repro.sh"
    hdd = shlex.quote(str(run_dir / "hdd"))
    path.write_text(
        "#!/bin/sh\n# run from the repository root; recreates an empty HDD dir first\n"
        f"rm -rf {hdd} && mkdir -p {hdd}\n"
        f"{shlex.join(command)}\n"
    )
    return path


def start_snapshot(args: argparse.Namespace, raw: list[str]) -> int:
    """T1153: re-run this command under the DMTCP layer and snapshot it at a port 0 poll."""
    from tools.snapshot import session

    poll = snapshot_options.resolve_poll(args)
    if args.host is not None:
        host = args.host
    elif args.no_build:
        host = args.build_dir / "tsfp_host"
    else:
        print(
            "play: --snapshot-at-poll pins the host binary: give --host PATH or --no-build",
            file=sys.stderr,
        )
        return 2
    host = (ROOT / host).resolve() if not host.is_absolute() else host
    if not host.is_file():
        print(f"play: missing input: host binary {host}", file=sys.stderr)
        return 2
    run_dir = args.run_dir or RUNS / datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = (ROOT / run_dir).resolve() if not run_dir.is_absolute() else run_dir
    snapshot_dir = args.snapshot_dir or SNAPSHOTS / run_dir.name
    if not snapshot_dir.is_absolute():
        snapshot_dir = (ROOT / snapshot_dir).resolve()
    play_argv = [
        *snapshot_options.strip_flags(raw),
        "--host",
        str(host),
        "--run-dir",
        str(run_dir),
        "--snapshot-at-poll",
        str(poll),
    ]
    return session.take(
        play_argv,
        poll=poll,
        host=host,
        xbe=(ROOT / args.xbe).resolve(),
        record=args.replay_input.resolve() if args.replay_input else None,
        run_dir=run_dir,
        snapshot_dir=snapshot_dir,
        keep_running=args.snapshot_continue,
        gzip=args.snapshot_gzip,
    )


def main(argv: list[str] | None = None) -> int:
    raw = sys.argv[1:] if argv is None else argv
    args = parse_args(raw)
    if args.resume_snapshot is not None:
        from tools.snapshot import session

        return session.resume(
            args.resume_snapshot,
            host_override=args.host,
            timeout=args.resume_timeout,
            fast_verify=args.snapshot_fast_verify,
        )
    if snapshot_options.wants_snapshot(args) and not os.environ.get(snapshot_options.INNER_ENV):
        return start_snapshot(args, raw)
    enable = [
        *args.enable,
        *(["skip-intro"] if args.skip_intro else []),
        *(["xonline-offline"] if args.xonline_offline else []),
        *(["gpu-live"] if args.gpu_live else []),
        *(["gpu-live-inferred"] if args.gpu_live_inferred else []),
        *(["gpu-live-blit"] if args.gpu_live_blit else []),
    ]
    try:
        active = flags.select(tuple(enable), tuple(args.disable), args.strict)
    except flags.UnknownFlagError as error:
        print(f"play: {error}", file=sys.stderr)
        return 2
    overrides = {
        "vblank-owner-waits": args.vblank_owner_waits,
        "vblank-worker-blanks": args.vblank_worker_blanks,
        "thread-timeout": 2147483647
        if args.interactive
        else (args.replay_thread_timeout if args.replay_interactive else None),
    }
    active = [
        Flag(flag.name, (flag.args[0], str(overrides[flag.name])), flag.tier, flag.label, flag.why)
        if overrides.get(flag.name) is not None
        else flag
        for flag in active
    ]
    if args.list_flags:
        print("\n".join(flags.announce(list(flags.FLAG_TABLE))))
        return 0
    refusal = presentation_refusal(args)
    if refusal is not None:
        print(f"play: {refusal}", file=sys.stderr)
        return 2
    if args.disc is None:
        print("play: --disc is required (the user's own disc image)", file=sys.stderr)
        return 2
    missing = [str(p) for p in (args.disc, args.xbe) if not (ROOT / p).exists()]
    if missing:
        print(f"play: missing input: {', '.join(missing)}", file=sys.stderr)
        return 2
    run_dir = args.run_dir or RUNS / datetime.now().strftime("%Y%m%d-%H%M%S")
    hdd, overlays = run_dir / "hdd", run_dir / "overlay"
    modules = args.gpu_live_modules or LIVE_MODULES
    if args.check_route is not None:
        return check_route_only(args, active, hdd, host_extra(args, overlays, modules))
    print("play: flags (every OPT-IN and FABRICATED item is listed, the host labels its own too):")
    print("\n".join(flags.announce(active)))
    if wants_live(args):
        ensure_modules(ROOT / modules)
        reason = translate_refusal(args)
        profile_modules = (
            modules / "live-raster-v5"
            if args.gpu_live_inferred or "gpu-live-inferred" in args.enable
            else modules
        )
        print(
            f"play: on-demand shader translation (INFERRED translators, cache {profile_modules}): "
            + (
                "ON"
                if reason is None
                else f"OFF, {reason}; draws with a program in no table are refused"
            )
        )
    extra = host_extra(args, overlays, modules)
    # T1618: a route recorded with other flags is refused HERE, before the build and the window
    refusal = route_refusal(
        args, host_command(Path("tsfp_host"), args.xbe, hdd, args.disc, active, extra)
    )
    if refusal is not None and not args.dry_run:
        print(f"play: {refusal}", file=sys.stderr)
        return 2
    if args.host is not None:
        host = args.host
    elif args.no_build:
        print("play: --no-build uses the existing host; no private menu seed preparation")
        host = args.build_dir / "tsfp_host"
    else:
        try:
            host = build_host(args.build_dir, args.xbe)
        except (RuntimeError, OSError, subprocess.SubprocessError) as error:
            print(f"play: private build preparation failed: {error}", file=sys.stderr)
            return 2
    if not (ROOT / host).exists():
        print(f"play: missing input: host binary {host}", file=sys.stderr)
        return 2
    command = host_command(host, args.xbe, hdd, args.disc, active, extra)
    if args.dry_run:
        print(shlex.join(command))
        return 0
    if route_nav.has_nav_flags(command) and not route_nav.host_supports_nav(ROOT / host):
        print(
            "play: the host predates T1640 (--help has no T1640 marker): no --route-nav flags, "
            "nav steps play as recorded and a recording gets no nav lines. "
            "Rebuild: python -m tools.private_host build",
            file=sys.stderr,
        )
        command = route_nav.strip_nav_flags(command)
    for directory in (hdd, overlays):
        (ROOT / directory).mkdir(parents=True, exist_ok=True)
    repro = write_repro(run_dir, command)
    print(f"play: run dir {run_dir} (run.log, host.out, host.err, overlay/, repro.sh)")
    if args.log_file:
        print(f"play: run log: {args.log_file}")
    window = args.window or args.present == "window"
    print(
        "play: interactive window play (FABRICATED wall timing), Z=A, Enter=START; "
        "Escape/window close ends execution; running..."
        if args.interactive
        else "play: SDL3 window and audio device requested, the window stays open after the stop "
        "report (Escape closes it); running..."
        if window
        else "play: headless, no window and no audio device (docs/play.md); running..."
    )
    started = time.monotonic()
    try:
        code = run_host(
            command,
            args.timeout if args.timeout is not None else (None if args.interactive else 1800),
            ROOT / run_dir,
            args.console,
            args.log_file,
        )
    except KeyboardInterrupt:
        code = 130
    text = (ROOT / run_dir / "host.out").read_text(errors="replace")
    report = stop.parse(text)
    summary = stop.render(report)
    (ROOT / run_dir / "stop.txt").write_text("\n".join(summary) + "\n")
    print("\n".join(summary))
    shots = len(list((ROOT / overlays).glob("*.png"))) if not args.no_overlay else 0
    print(
        f"play: host exit {code} after {time.monotonic() - started:.0f}s, "
        f"{shots} overlay picture(s)"
    )
    print(f"play: repro: sh {repro.relative_to(ROOT)}")
    if args.debug:
        print(f"play: --debug: no live process remains, state kept in {run_dir}/hdd (docs/play.md)")
    if not report.stops:
        refused = (ROOT / run_dir / "host.err").read_text(errors="replace").strip()
        if refused:
            print(f"play: host said: {refused.splitlines()[0]}", file=sys.stderr)
    return 0 if report.stops else 1
