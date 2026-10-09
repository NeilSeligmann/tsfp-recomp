# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1618: the flag identity of an input record (route), checked BEFORE the game launches.

The host refuses `--replay-input FILE` when the record was made against another XBE or another game
affecting flag set (`xinput_record_check_header`, `src/input/xinput_record.c`), but only after it
started, so the window flashes and closes. This module repeats exactly that comparison in Python,
from the record header and the host command line `tools.play` is about to run, so the launcher (and
the owner's fish scripts, through `python -m tools.play --check-route FILE ...`) can say so first.

The host stays the authority. `IGNORED` and `PATH_VALUED` mirror `ignored[]` and `path_valued[]` of
`src/input/xinput_record.c`, a test (`tests/test_t1618_route_flags.py`) parses the C source and fails
when the two drift. `route_game_flags` is the ONE place that names the game affecting `tools.play`
flags both the route recording and the replay (`tools.action_profile`) run with.

  python -m tools.play.route_check flags [--no-skip-intro]   one tools.play flag per line
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

# Flags that do not change what the title sees, with the number of values that follow (ignored[] in C).
IGNORED: dict[str, int] = {
    "--record-input": 1,
    "--replay-input": 1,
    "--pad-source": 1,
    "--pad-feed": 1,
    "--pad-script": 1,
    "--pad-script-live": 1,
    "--disc": 1,
    "--hdd": 1,
    "--present": 1,
    "--present-capture": 1,
    "--present-timeline": 1,
    "--present-no-pace": 0,
    "--present-hold-ms": 1,
    "--audio-sink": 1,
    "--audio-output": 1,
    "--audio-mute": 0,
    "--audio-latency-ms": 1,
    "--audio-min-rate": 1,
    "--audio-stretch": 1,
    "--audio-stretch-legacy": 0,
    "--av-sync-offset-ms": 1,
    "--audio-hold-ms": 1,
    "--no-audio-pump": 0,
    "--live-pipeline": 1,
    "--live-present-sync": 0,
    "--live-readback": 0,
    "--live-blit-verify": 1,
    "--gpu-live-blit": 0,
    "--live-frame-hash": 1,
    "--live-pipeline-cache": 1,
    "--cpu-profile": 1,
    "--cpu-profile-wall": 1,
    "--interactive": 0,
    "--thread-timeout": 1,
    "--vblank-owner-waits": 1,
    "--vblank-worker-blanks": 1,
    "--stop-after-calls": 1,
    "--trace": 1,
    "--profile-calls": 0,
    "--profile-calls-top": 1,
    "--profile-watch": 1,
    "--trace-xmv": 0,
    "--synthetic-pad": 0,
    "--controllers": 0,
    "--controller-config": 1,
    "--controller-mappings": 1,
    "--controller-save": 1,
    "--dump-overlay": 1,
    "--dump-xmv-frames": 1,
    "--capture-xmv-entries": 1,
    "--gpu-replay-dump": 1,
    "--gpu-replay-draw-dump": 1,
    "--snapshot-at-poll": 1,
    "--stop-at-poll": 1,
    "--forced-state": 0,
    "--dump-guest-range": 1,
    "--dump-guest-dir": 1,
    "--dump-guest-max-bytes": 1,
    "--poke-at-poll": 1,
    "--route-wait": 1,
    "--replay-handover": 0,
    # T1618: the read-only indirect call census (src/host/function_census.c)
    "--census-icalls": 0,
    "--census-phases": 1,
    "--census-window": 1,
    # T1627: host hotkeys (src/input/xinput_hotkey.c) are tooling: they write files and keep a chord from the game
    "--hotkey": 1,
    "--hotkey-dir": 1,
    # T1720b: the shot hotkey writes pictures only
    "--shot-dir": 1,
    "--shot-max": 1,
    "--shot-max-bytes": 1,
    # T1629: the passive button dump observer (src/host/button_dump.c)
    "--dump-on-button": 0,
    "--dump-after-frames": 1,
    "--dump-button-threshold": 1,
    "--dump-button-coalesce": 1,
    "--dump-button-max-pending": 1,
    "--dump-button-max-dumps": 1,
    "--dump-button-max-bytes": 1,
    "--dump-button-start-poll": 1,
    "--dump-button-after-replay": 0,
    "--dump-button-idle-every": 1,
    "--dump-button-max-idle": 1,
    # T1633: event waits and the event log only observe the guest (file I/O counters, calls, memory reads) and pace the pad
    "--route-wait-event": 1,
    "--route-event-log": 1,
    "--route-log-mem": 1,
    # T1640: closed loop menu navigation only changes how the pad inputs of a nav range are produced, not the recorded identity
    "--route-nav": 1,
    "--route-nav-menus": 1,
    "--route-nav-timing": 1,
    "--route-nav-record": 1,
}
# Flags whose value is an input path: the flag stays in the identity, the value becomes "<path>".
PATH_VALUED = (
    "--gpu-replay",
    "--gpu-replay-undo-viewport",
    "--gpu-replay-window-to-clip",
    "--mount",
    "--seed-xmv-result",
)
PATH_TOKEN = "<path>"
RECORD_VERSION = 1
RE_RECORD_ADVICE = "record it again with tmp/record_mapmaker_route.fish"


def canonicalise(tokens: Sequence[str]) -> list[str]:
    """The identity tokens of an argument list (`canonicalise` of src/input/xinput_record.c)."""
    out: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        skip = IGNORED.get(token)
        if skip is not None:
            index += skip + 1
            continue
        out.append(token)
        if token in PATH_VALUED and index + 1 < len(tokens):
            index += 1
            out.append(PATH_TOKEN)
        index += 1
    return out


def identity_flags(host_command: Sequence[str]) -> list[str]:
    """The identity of a host command line: argv minus the host binary and the XBE path."""
    return canonicalise(list(host_command)[2:])


def canonical_line(line: str) -> list[str]:
    """The identity of an already recorded `# flags:` line, as the host re-canonicalises it."""
    return canonicalise([token for token in line.split(" ") if token])


def flags_sha256(identity: Sequence[str]) -> str:
    return hashlib.sha256(" ".join(identity).encode()).hexdigest()


def units(tokens: Sequence[str]) -> list[tuple[str, ...]]:
    """A flag with its values is one unit: a `--x` token opens it, tokens after it that are not flags are its values."""
    grouped: list[list[str]] = []
    for token in tokens:
        if not grouped or token.startswith("--"):
            grouped.append([token])
        else:
            grouped[-1].append(token)
    return [tuple(unit) for unit in grouped]


def diff_flags(
    recorded: Sequence[str], current: Sequence[str]
) -> tuple[list[tuple[str, ...]], list[tuple[str, ...]]]:
    """(units only in the recording, units only in this run), repeated units matched pairwise."""
    remaining = units(current)
    only_recorded: list[tuple[str, ...]] = []
    for unit in units(recorded):
        if unit in remaining:
            remaining.remove(unit)
        else:
            only_recorded.append(unit)
    return only_recorded, remaining


def format_units(found: Sequence[tuple[str, ...]]) -> str:
    return ", ".join(" ".join(unit) for unit in found) or "(nothing)"


@dataclass
class RouteHeader:
    first_line: str = ""
    values: dict[str, str] = field(default_factory=dict)


def read_header(path: Path) -> RouteHeader:
    """The leading `# key: value` comment lines of a record (the first line is `# tsfp-input vN`)."""
    header = RouteHeader()
    with path.open(encoding="utf-8", errors="replace") as handle:
        for number, raw in enumerate(handle):
            line = raw.rstrip("\n")
            if not line.startswith("#"):
                break
            if number == 0:
                header.first_line = line
                continue
            key, separator, value = line[2:].partition(":")
            if separator and line.startswith("# "):
                header.values.setdefault(key, value.strip())
    return header


@dataclass
class Verdict:
    ok: bool
    kind: str = "match"  # match, unreadable, version, xbe, flags
    lines: list[str] = field(default_factory=list)

    def message(self) -> str:
        return "\n".join(self.lines)


def check_route(route: Path, host_command: Sequence[str], xbe_sha256: str | None = None) -> Verdict:
    """Would the host accept `route` for this command line? The checks of `xinput_record_check_header`."""
    try:
        header = read_header(route)
    except OSError as error:
        return Verdict(False, "unreadable", [f"route {route}: cannot read it ({error})"])
    if not header.first_line.startswith("# "):
        return Verdict(
            False,
            "version",
            [f"route {route} is not an input record (no '# tsfp-input vN' header)"],
        )
    if header.first_line != f"# tsfp-input v{RECORD_VERSION}":
        return Verdict(
            False, "version", [f"route {route}: unsupported record version ({header.first_line!r})"]
        )
    recorded_xbe = header.values.get("xbe-sha256", "(missing)")
    if xbe_sha256 is not None and recorded_xbe != xbe_sha256:
        return Verdict(
            False,
            "xbe",
            [
                f"route {route} was recorded against a different XBE ({recorded_xbe[:16]}..., this run "
                f"{xbe_sha256[:16]}...), {RE_RECORD_ADVICE}"
            ],
        )
    current = identity_flags(host_command)
    if header.values.get("flags-sha256") == flags_sha256(current):
        return Verdict(True)
    recorded_line = header.values.get("flags")
    if recorded_line is None:
        return Verdict(
            False,
            "flags",
            [
                f"route {route} has no '# flags:' line, so its flag set is unknown, {RE_RECORD_ADVICE}"
            ],
        )
    recorded = canonical_line(recorded_line)
    if recorded == current:
        return Verdict(True)
    only_recorded, only_current = diff_flags(recorded, current)
    lines = [f"route {route} was recorded with a different flag set, {RE_RECORD_ADVICE}"]
    if only_recorded or only_current:
        lines += [
            f"  only in the recording: {format_units(only_recorded)}",
            f"  only in this run:      {format_units(only_current)}",
        ]
    else:
        lines += ["  the same flags in a different order"]
    lines += [
        "  Record and replay must run with identical game flags (tmp/record_mapmaker_route.fish builds the same flags "
        "as the replay)."
    ]
    return Verdict(False, "flags", lines)


def route_game_flags(skip_intro: bool = True) -> list[str]:
    """The `tools.play` flags the route recording and the replay share: every game affecting one, built here only.

    `--skip-intro` is part of it on purpose: it terminates the startup logos, so every later menu appears at an
    earlier poll (docs/t1139-intro-skip.md) and the poll indexed inputs of a route recorded without it land on
    the wrong screens. The census observers (`--census-icalls`, `--census-phases`) and the sampler flags are NOT in
    it: they are read-only and ignored by the identity (`IGNORED`).
    """
    return [
        "--interactive",
        "--window",
        "--gpu-live",
        "--gpu-live-inferred",
        "--xonline-offline",
        *(["--skip-intro"] if skip_intro else []),
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.play.route_check", description="T1618: route flag identity helpers"
    )
    sub = parser.add_subparsers(dest="command", required=True)
    flags_parser = sub.add_parser(
        "flags", help="print the shared tools.play route flags, one per line"
    )
    flags_parser.add_argument(
        "--no-skip-intro",
        action="store_true",
        help="leave --skip-intro out (record AND replay must agree)",
    )
    args = parser.parse_args(argv)
    if args.command == "flags":
        print("\n".join(route_game_flags(skip_intro=not args.no_skip_intro)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
