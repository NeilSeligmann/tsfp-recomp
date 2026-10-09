#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1709: reproducible HEADLESS end-to-end run of the settings dump pipeline on a container-runnable host.

    python -m tools.settings_headless --host PATH/tsfp_host [--route R] [--hdd-source D] [--xbe X] [--disc ISO]
                                     [--out DIR] [--stop-at-poll N] [--keep] [--force] [--dry-run]

Steps (the commands proven in docs/t1709-settings-dump.md, MEASURED with the container host):
  1. a headless copy of a recorded owner route (its `# flags:` header line replaced by `headless_flags()`, every other byte kept),
  2. `tools.settings_route` cuts it to the Settings > Controls page, 3. `tools.settings_walk` appends the scripted walk,
  4. the pristine HDD is copied to OUT/hdd, 5. the host replays the route with `--dump-on-button` and the guest ranges of
  `tools.settings_dump_report ranges` (no window, no gpu, SDL dummy audio),
  6. `tools.button_dump_report --validate` and `tools.settings_dump_report --write --json`,
  7. `evaluate()` turns the JSON report into PASS/FAIL assertion lines.
The recorded route, HDD, XBE and ISO are owner files found in the MAIN checkout (`git rev-parse --git-common-dir`, or --main-root /
TSFP_MAIN_ROOT), they are only read or copied under OUT and never printed. OUT defaults to `tmp/t1709-headless` of the main checkout
and a non-empty OUT is refused without --force. After a PASS the 170 MB `OUT/session/buttons` folder is deleted unless --keep, the
manifest `buttons.jsonl` and the reports stay.

Exit codes: 0 every assertion passed, 1 a failed assertion (or the host failed), 2 missing or bad inputs.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path

from tools import button_dump_report, settings_dump_report, settings_route, settings_walk
from tools.private_host import locate_disc, main_checkout

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = Path("tmp/t1709-headless")
DEFAULT_HDD = Path("tmp/hdds/route-hdd-pristine")
DEFAULT_XBE = Path("build/default.xbe")
ROUTES_DIR = Path("tmp/owner-profiles/routes")
MENUS = "tools/data/menu_nav.txt"
WALK = "tools/data/settings_walk.txt"
FLAGS_PREFIX = "# flags:"
POLLS_PREFIX = "# polls:"
HOST_TIMEOUT_SECONDS = 900
KILL_AFTER = "10s"
#: Host polls are shifted by about +1470 (jitter +-40) by the waits of mark 1 over the route polls.
MARK_WAIT_SHIFT = 1500
STOP_SLACK = 300
OWNER_WAITS = 1000000
WORKER_BLANKS = 1000000
DUMP_AFTER_FRAMES = "2,30"
MAX_DUMPS = 3000
#: The nine Controls settings the default walk exercises.
WALK_SETTINGS = (
    "crosshair",
    "inverse_look",
    "auto_aim",
    "auto_lookahead",
    "aim_mode",
    "crouch",
    "weapon_change",
    "vibration",
    "turn_speed",
)
MIN_WALK_PRESSES = 100
#: Profile block change of the Accept press (MEASURED): Weapon Change 4 -> 0 at +0x68.
ACCEPT_BLOCK = "profile_head"
ACCEPT_OFFSET = 0x68
ACCEPT_OLD = "4"
ACCEPT_NEW = "0"
FORBIDDEN_FLAGS = ("UNLABELLED-BYTE", "VALUE-MISMATCH")
MIN_AV_ROWS = 4
MIN_AV_PAIRS = 3
PRESS_EDGES = ("press", "mixed")

Check = tuple[str, bool, str]


class InputError(ValueError):
    """A missing or unusable input (exit 2)."""


# --------------------------------------------------------------------------------------------------------------------
# pure parts
# --------------------------------------------------------------------------------------------------------------------


def headless_flags() -> list[str]:
    """The host flags of a run with no window, no gpu and dummy audio (the set of route.headless.txt)."""
    return [
        "--xonline-offline",
        "--skip-intro",
        "--ac97-ready",
        "--headless-streams",
        "--headless-buffers",
        "--headless-listener",
        "--headless-second-vblank",
        "--native-shader-assembler",
        "--native-xmv",
        "--headless-movie-audio",
        "--couple-vblank-effects",
        "--check-vblank-quiescence",
        "--overlay-consume",
    ]


def rewrite_flags_header(text: str) -> str:
    """`text` with its header `# flags:` line replaced by the headless set, every other byte unchanged."""
    lines = text.splitlines(keepends=True)
    for index, raw in enumerate(lines):
        stripped = raw.strip()
        if stripped and not stripped.startswith("#"):
            break
        if raw.startswith(FLAGS_PREFIX):
            ending = raw[len(raw.rstrip("\r\n")) :]
            lines[index] = f"{FLAGS_PREFIX} {' '.join(headless_flags())}{ending}"
            return "".join(lines)
    raise InputError("the route has no '# flags:' header line")


def stop_poll(route_text: str, mark_shift: int = MARK_WAIT_SHIFT, slack: int = STOP_SLACK) -> int:
    """Host poll to stop at: the route polls of its trailer, the mark wait shift and a slack for the last rest."""
    trailers = [line for line in route_text.splitlines() if line.startswith(POLLS_PREFIX)]
    if not trailers:
        raise InputError("the route has no '# polls:' trailer")
    try:
        polls = int(trailers[-1][len(POLLS_PREFIX) :].strip())
    except ValueError:
        raise InputError(f"unreadable trailer {trailers[-1]!r}") from None
    return polls + mark_shift + slack


def host_command(
    host: str,
    xbe: str,
    hdd: str,
    route: str,
    event_log: str,
    disc: str,
    menus: str,
    stop: int,
    ranges: str,
    dump_dir: str,
) -> list[str]:
    """The host argv of the dump run (without the timeout wrapper)."""
    return [
        host,
        xbe,
        "--hdd",
        hdd,
        *headless_flags(),
        "--vblank-owner-waits",
        str(OWNER_WAITS),
        "--vblank-worker-blanks",
        str(WORKER_BLANKS),
        "--synthetic-pad",
        "--replay-input",
        route,
        "--route-event-log",
        event_log,
        "--route-nav",
        "strict",
        "--route-nav-menus",
        menus,
        "--stop-at-poll",
        str(stop),
        "--disc",
        disc,
        "--dump-on-button",
        "--dump-after-frames",
        DUMP_AFTER_FRAMES,
        "--dump-button-max-dumps",
        str(MAX_DUMPS),
        "--dump-guest-range",
        ranges,
        "--dump-guest-dir",
        dump_dir,
    ]


def with_timeout(command: Sequence[str], seconds: int = HOST_TIMEOUT_SECONDS) -> list[str]:
    """`command` under a hard timeout (SIGKILL ten seconds after the SIGTERM)."""
    return ["timeout", f"--kill-after={KILL_AFTER}", str(seconds), *command]


def host_env(runtime_dir: str) -> dict[str, str]:
    """The environment of the host run: dummy SDL audio and a private runtime dir."""
    env = dict(os.environ)
    env["SDL_AUDIODRIVER"] = "dummy"
    env["XDG_RUNTIME_DIR"] = runtime_dir
    return env


def _walk_press(report: dict) -> bool:
    return report.get("kind") is not None and report.get("edge") in PRESS_EDGES


def _check_walk_presses(report: dict) -> Check:
    count = sum(1 for item in report.get("reports", []) if _walk_press(item))
    return (
        "plan end state: walk presses recorded",
        count >= MIN_WALK_PRESSES,
        f"{count} press events on the Settings pages (need >= {MIN_WALK_PRESSES})",
    )


def _check_coverage(report: dict) -> Check:
    status = {item.get("key"): item.get("status") for item in report.get("coverage", [])}
    bad = [
        f"{key}={status.get(key, 'missing')}" for key in WALK_SETTINGS if status.get(key) != "OK"
    ]
    return (
        "controls coverage OK",
        not bad,
        f"{len(WALK_SETTINGS) - len(bad)}/{len(WALK_SETTINGS)} settings OK"
        + (f", not OK: {', '.join(bad)}" if bad else ""),
    )


def _check_accept(report: dict) -> Check:
    hits = [
        item
        for entry in report.get("reports", [])
        if entry.get("accept")
        for item in entry.get("changes", [])
        if item.get("block") == ACCEPT_BLOCK
        and item.get("start") == ACCEPT_OFFSET
        and item.get("old") == ACCEPT_OLD
        and item.get("new") == ACCEPT_NEW
    ]
    return (
        "profile block changed at Accept",
        bool(hits),
        f"{ACCEPT_BLOCK} +0x{ACCEPT_OFFSET:X} {ACCEPT_OLD} -> {ACCEPT_NEW}: {len(hits)} change(s) on an Accept press",
    )


def _check_flag(report: dict, tag: str) -> Check:
    count = report.get("flags", {}).get(tag, 0)
    return (f"no {tag} flags", count == 0, f"{tag} count {count}")


def _check_av(report: dict) -> Check:
    rows = report.get("av_rows", [])
    pairs = report.get("av_storage", {}).get("pairs", [])
    unpaired = sum(1 for pair in pairs if not pair.get("rows"))
    ok = len(rows) >= MIN_AV_ROWS and len(pairs) >= MIN_AV_PAIRS and unpaired == 0
    return (
        "A/V rows with a storage pairing",
        ok,
        f"{len(rows)} rows (need >= {MIN_AV_ROWS}), {len(pairs)} audio/profile pairs (need >= {MIN_AV_PAIRS}), {unpaired} pairs without a row",
    )


def evaluate(report: dict) -> list[Check]:
    """(name, ok, detail) per assertion over the JSON of `tools.settings_dump_report --json`."""
    return [
        _check_walk_presses(report),
        _check_coverage(report),
        _check_accept(report),
        *(_check_flag(report, tag) for tag in FORBIDDEN_FLAGS),
        _check_av(report),
    ]


def newest_route(routes_dir: Path) -> Path | None:
    """The newest recorded route that has a `# wait: mark1:` line (the one `tools.settings_route` accepts)."""
    found: list[tuple[float, str, Path]] = []
    for path in routes_dir.glob("*.txt"):
        if not path.is_file():
            continue
        text = path.read_bytes().decode("utf-8", errors="replace")
        if re.search(r"^# wait: mark1:", text, re.MULTILINE):
            found.append((path.stat().st_mtime, path.name, path))
    return max(found)[2] if found else None


def plan_routes(source_text: str, walk_text: str) -> tuple[str, str]:
    """(settings route, walk route) of a source route in memory, the stop poll of a dry run comes from the second."""
    generated = settings_route.generate(
        rewrite_flags_header(source_text), "controls", settings_route.DEFAULT_REST_POLLS
    )
    text, _ = settings_walk.build(generated.text, walk_text, 6, 60, 150)
    return generated.text, text


def tree_size(path: Path) -> int:
    """Bytes of the files below `path` (0 when it does not exist)."""
    return (
        sum(
            item.stat().st_size
            for item in path.rglob("*")
            if item.is_file() and not item.is_symlink()
        )
        if path.exists()
        else 0
    )


# --------------------------------------------------------------------------------------------------------------------
# pipeline
# --------------------------------------------------------------------------------------------------------------------


def say(text: str) -> None:
    print(f"settings_headless: {text}", flush=True)


def call_tool(main: Callable[[list[str]], int], argv: list[str]) -> int:
    """Run a sibling tool's main() in process, a SystemExit becomes its code."""
    try:
        return main(argv)
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else 2


def resolve_inputs(args: argparse.Namespace, main: Path) -> dict[str, Path]:
    """Absolute paths of every input, InputError (exit 2) naming what is missing."""
    host = args.host.resolve()
    xbe = (args.xbe or main / DEFAULT_XBE).resolve()
    hdd = (args.hdd_source or main / DEFAULT_HDD).resolve()
    disc = args.disc or locate_disc(ROOT, main)
    route = args.route or newest_route(main / ROUTES_DIR)
    out = (args.out or main / DEFAULT_OUT).resolve()
    missing = []
    if not host.is_file() or not os.access(host, os.X_OK):
        missing.append(f"host {host} (an executable tsfp_host, --host)")
    if not xbe.is_file():
        missing.append(f"XBE {xbe} (--xbe)")
    if disc is None or not Path(disc).is_file():
        missing.append(f"disc image {disc} (--disc)")
    if not hdd.is_dir():
        missing.append(f"pristine HDD {hdd} (--hdd-source)")
    if route is None or not Path(route).is_file():
        missing.append(
            f"recorded route {route} (--route, none with a '# wait: mark1:' line under {main / ROUTES_DIR})"
        )
    if missing:
        raise InputError("missing inputs: " + "; ".join(missing))
    if out.is_dir() and any(out.iterdir()) and not args.force:
        raise InputError(f"{out} is not empty (use --force to replace it)")
    return {
        "host": host,
        "xbe": xbe,
        "hdd": hdd,
        "disc": Path(disc).resolve(),
        "route": Path(route).resolve(),
        "out": out,
    }


def prepare_out(out: Path, protected: Sequence[Path]) -> None:
    """An empty OUT (an existing one is removed first, never one that holds a checkout)."""
    if out.exists():
        if any(out == item or out in item.parents for item in protected):
            raise InputError(f"refusing to remove {out}, it holds a checkout")
        shutil.rmtree(out)
    out.mkdir(parents=True)


def commands(paths: dict[str, Path], stop: int, ranges: str) -> dict[str, list[str]]:
    out = paths["out"]
    session = out / "session"
    return {
        "headless route": [
            "write",
            str(out / "route.headless.txt"),
            "(rewrite_flags_header of the recorded route)",
        ],
        "settings route": [
            sys.executable,
            "-m",
            "tools.settings_route",
            str(out / "route.headless.txt"),
            "--out",
            str(out / "settings.route.txt"),
            "--page",
            "controls",
            "--waits",
            str(out / "route.headless.waits"),
        ],
        "walk": [
            sys.executable,
            "-m",
            "tools.settings_walk",
            str(out / "settings.route.txt"),
            "--walk",
            str(ROOT / WALK),
            "--out",
            str(out / "walk.route.txt"),
        ],
        "hdd": ["copy", str(paths["hdd"]), str(out / "hdd")],
        "host": with_timeout(
            host_command(
                str(paths["host"]),
                str(paths["xbe"]),
                str(out / "hdd"),
                str(out / "walk.route.txt"),
                str(out / "events.log"),
                str(paths["disc"]),
                MENUS,
                stop,
                ranges,
                str(session),
            )
        ),
        "validate": [sys.executable, "-m", "tools.button_dump_report", str(session), "--validate"],
        "report": [
            sys.executable,
            "-m",
            "tools.settings_dump_report",
            str(session),
            "--write",
            "--json",
        ],
    }


def run_host(paths: dict[str, Path], command: list[str]) -> tuple[int, float]:
    out = paths["out"]
    runtime = out / "xdg-runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    started = time.monotonic()
    with (out / "host.out").open("wb") as stdout, (out / "host.err").open("wb") as stderr:
        done = subprocess.run(
            command, cwd=ROOT, env=host_env(str(runtime)), stdout=stdout, stderr=stderr, check=False
        )  # noqa: S603
    return done.returncode, time.monotonic() - started


def report_lines(checks: list[Check]) -> bool:
    for name, ok, detail in checks:
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)
    return all(ok for _, ok, _ in checks)


def pipeline(args: argparse.Namespace, paths: dict[str, Path], main: Path) -> int:
    out = paths["out"]
    session = out / "session"
    prepare_out(out, (ROOT, main))
    source_text = paths["route"].read_bytes().decode("utf-8", errors="surrogateescape")
    (out / "route.headless.txt").write_bytes(
        rewrite_flags_header(source_text).encode("utf-8", errors="surrogateescape")
    )
    sibling = paths["route"].with_suffix(".waits")
    if sibling.is_file():
        shutil.copyfile(sibling, out / "route.headless.waits")
    steps = commands(paths, 0, "")
    for name, tool in (("settings route", settings_route), ("walk", settings_walk)):
        if call_tool(tool.main, steps[name][3:]) != 0:
            say(f"{tool.__name__} failed")
            return 2
    stop = args.stop_at_poll or stop_poll((out / "walk.route.txt").read_text(encoding="utf-8"))
    ranges = settings_dump_report.ranges_argument()
    shutil.copytree(paths["hdd"], out / "hdd", symlinks=True)
    command = commands(paths, stop, ranges)["host"]
    say(f"host replay, stop at poll {stop}, hard timeout {HOST_TIMEOUT_SECONDS} s")
    code, wall = run_host(paths, command)
    checks: list[Check] = [
        (
            "host run",
            code == 0,
            f"exit status {code}, wall {wall:.1f} s (stdout/stderr in host.out, host.err)",
        )
    ]
    if (session / "buttons.jsonl").is_file():
        valid = call_tool(button_dump_report.main, [str(session), "--validate"]) == 0
        checks.append(
            (
                "manifest validates (button_dump_report --validate)",
                valid,
                str(session / "buttons.jsonl"),
            )
        )
        if call_tool(settings_dump_report.main, [str(session), "--write", "--json"]) == 0:
            report = json.loads(
                (session / settings_dump_report.JSON_NAME).read_text(encoding="utf-8")
            )
            checks += evaluate(report)
        else:
            checks.append(("settings_dump_report", False, "the report tool failed"))
    else:
        checks.append(("manifest written", False, f"{session / 'buttons.jsonl'} is missing"))
    passed = report_lines(checks)
    before = tree_size(out)
    if passed and not args.keep:
        shutil.rmtree(session / "buttons", ignore_errors=True)
    say(
        f"disk used by {out}: {before / 1e6:.1f} MB before cleanup, {tree_size(out) / 1e6:.1f} MB after, wall {wall:.1f} s for the host"
    )
    say("ALL PASS" if passed else "FAILED")
    return 0 if passed else 1


def dry_run(args: argparse.Namespace, paths: dict[str, Path]) -> int:
    source_text = paths["route"].read_bytes().decode("utf-8", errors="surrogateescape")
    try:
        _, walk_route = plan_routes(source_text, (ROOT / WALK).read_text(encoding="utf-8"))
    except (settings_route.ToolError, settings_walk.WalkError, InputError) as error:
        raise InputError(f"the route cannot be turned into the walk route: {error}") from None
    stop = args.stop_at_poll or stop_poll(walk_route)
    for name, command in commands(paths, stop, settings_dump_report.ranges_argument()).items():
        print(f"[{name}] {shlex.join(command)}")
    say("dry run, nothing executed")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.settings_headless", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument("--host", type=Path, required=True, help="container-runnable tsfp_host")
    parser.add_argument(
        "--route",
        type=Path,
        help="recorded owner route (default: newest with a mark 1 wait in the main checkout)",
    )
    parser.add_argument(
        "--hdd-source",
        type=Path,
        help=f"pristine HDD folder (default {DEFAULT_HDD} of the main checkout)",
    )
    parser.add_argument(
        "--xbe", type=Path, help=f"XBE (default {DEFAULT_XBE} of the main checkout)"
    )
    parser.add_argument(
        "--disc",
        type=Path,
        help="disc image (default: the USA XBOX iso under tmp of the main checkout)",
    )
    parser.add_argument(
        "--out", type=Path, help=f"output folder (default {DEFAULT_OUT} of the main checkout)"
    )
    parser.add_argument(
        "--main-root",
        type=Path,
        help="main checkout holding the owner inputs (default: found via git, or TSFP_MAIN_ROOT)",
    )
    parser.add_argument(
        "--stop-at-poll",
        type=int,
        help="host poll to stop at (default: walk route polls + 1500 + 300)",
    )
    parser.add_argument("--keep", action="store_true", help="keep the dump folder after a PASS")
    parser.add_argument("--force", action="store_true", help="replace a non-empty --out")
    parser.add_argument("--dry-run", action="store_true", help="print the commands, run nothing")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    main_root = main_checkout(ROOT, args.main_root)
    try:
        paths = resolve_inputs(args, main_root)
        if args.stop_at_poll is not None and args.stop_at_poll < 1:
            raise InputError("--stop-at-poll must be at least 1")
        return dry_run(args, paths) if args.dry_run else pipeline(args, paths, main_root)
    except InputError as error:
        print(f"settings_headless: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
