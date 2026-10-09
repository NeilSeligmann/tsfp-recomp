# SPDX-License-Identifier: GPL-3.0-or-later
"""List the XDK surface functions that have no registered HLE handler.

DEFINITION (method INFERRED, not proven). A surface function (one of the addresses in
the generated src/xbox/xdk_surface.c) is MISSING when the registration snapshot written
by the `xdk_report` executable says `registered_handler:false`. That snapshot asks the
d3d8, dsound, xinput and xgrph registries whether a handler is registered for the
address. An unregistered address is trampolined to `xdk_thunk`, which stops the run with
"XDK function with no implementation". Not proven: whether an unregistered function has
a faithfully lifted body that would be reached without a stop (the dispatch design in
docs/xdk-dispatch.md routes every surface address through the trampoline, so no
unregistered address runs its lifted body). A registered handler is also not proof of
completeness: PARTIAL below lists registered entries that still refuse on a measured
input. Section XNET has the self-contained registry src/xbox/xnet_hle.c (T1070, three
functions). XONLINE is registered by the opt-in --xonline-offline policy (T1071,
docs/t1071-xonline-xmv.md), which xdk_report applies, so those rows read "registered". XMV is
served by the retained original route (default with --disc, T1093) that no HLE module registers.

Reached evidence comes from tmp/play/*/stop.txt (the STOP address and the "LAST GUEST
CALLS" list, the last dozen indirect targets). The call census there is a total, not
per address, so "not seen" means "not in any stop report", never "never called".
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROW = re.compile(r'\{0x([0-9a-fA-F]+), "(\w+)", (NULL|"[^"]*"), (\d+)')
STOP = re.compile(r"STOP .* at 0x([0-9A-Fa-f]+)")
LAST = re.compile(r"LAST GUEST CALLS[^:]*:\s*(.*)")

# Registered entries known to stop on a measured input. INFERRED from source reading
# and a stop report; each names its evidence.
PARTIAL = {
    0x003D59E0: "registered (src/gpu/d3d8_vertex_program.c); the bulk upload stop (sized "
    "reservation 0x003D6B30, stop in tmp/play/20261005-171938) is resolved by T1078, the "
    "title now passes it (docs/t1078-vertex-program-bulk-upload.md)",
    0x004754FD: "registered (src/input/xvoice_media.c), but only the measured title-shaped "
    "absent-voice case returns "
    "HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED); unsupported shapes stop by name, "
    "and connected media-object creation/async lifetime remain unimplemented (T1086)",
}

# Controller APIs registered by xinput_devices_register_pad(), called by src/host/main.c only
# with --controllers or --synthetic-pad (T1069). The default xdk_report snapshot omits them, so
# they stay listed but are labelled with the mode instead of plain "unregistered".
OPT_IN_PAD = frozenset((0x0046E133, 0x0046E189, 0x0046E195, 0x0046E36D, 0x0046E3E0))

# Shader assembler profile lifted bodies (tools/gen_xdk_original_bodies.py REQUIRED_V2),
# reached only with the opt-in --native-shader-assembler host flag. Not registered handlers.
RETAINED = frozenset(
    (0x003EE2B3, 0x003E6714, 0x003F1791, 0x003F42A0, 0x003EA301, 0x003FC8A8, 0x00402BCF, 0x00402C5C)
)

# The seven XMV exports are routed to the retained original bodies by src/host/xmv_original.c
# (lifted by tools.gen_xmv_original_bodies; docs/xmv-contracts.md). No HLE module registers them,
# so the snapshot cannot see that route (T1071). Since T1093 the route is the DEFAULT whenever a
# disc is mounted (--no-native-xmv opts out), so these are retained-default, not opt-in.
RETAINED_DEFAULT = frozenset(
    (0x00444A2D, 0x00444F71, 0x00445055, 0x004450C2, 0x00445241, 0x00445252, 0x0044525D)
)


def parse_surface(text: str) -> list[tuple[int, str, str | None, int]]:
    rows = []
    for match in ROW.finditer(text):
        name = None if match.group(3) == "NULL" else match.group(3).strip('"')
        rows.append((int(match.group(1), 16), match.group(2), name, int(match.group(4))))
    return rows


def parse_registration(text: str) -> dict[int, bool]:
    data = json.loads(text)
    return {int(e["address"], 16): bool(e["registered_handler"]) for e in data["entries"]}


def parse_census(play_dir: Path) -> tuple[dict[int, set[str]], dict[int, set[str]]]:
    """Return (stops, last_calls): address to the run names that showed it."""
    stops: dict[int, set[str]] = defaultdict(set)
    last: dict[int, set[str]] = defaultdict(set)
    for stop_file in sorted(play_dir.glob("*/stop.txt")):
        run = stop_file.parent.name
        for line in stop_file.read_text(errors="replace").splitlines():
            if (m := STOP.search(line)) is not None:
                stops[int(m.group(1), 16)].add(run)
            if (m := LAST.search(line)) is not None:
                for token in m.group(1).split():
                    last[int(token, 16)].add(run)
    return stops, last


Row = tuple[int, str, str | None, int]
Missing = tuple[int, str, str | None, int, bool, str]


def classify(
    rows: list[Row],
    registered: dict[int, bool],
    stops: dict[int, set[str]],
    last: dict[int, set[str]],
) -> list[Missing]:
    """Missing rows ranked by sites, with PARTIAL rows first."""
    out: list[Missing] = []
    for address, section, name, sites in rows:
        if registered.get(address) is None:
            raise ValueError(
                f"surface address {address:#010x} absent from the registration snapshot"
            )
        partial = address in PARTIAL
        if registered[address] and not partial:
            continue
        reached = "STOP" if address in stops else ("last-calls" if address in last else "not seen")
        out.append((address, section, name, sites, partial, reached))
    out.sort(key=lambda r: (not r[4], -r[3], r[0]))
    return out


def render(rows: list[Row], registered: dict[int, bool], missing: list[Missing]) -> str:
    per = defaultdict(lambda: [0, 0, 0, 0])
    for (
        address,
        section,
        _name,
        sites,
    ) in [(r[0], r[1], r[2], r[3]) for r in rows]:
        per[section][0] += 1
        per[section][1] += sites
        if not registered[address]:
            per[section][2] += 1
            per[section][3] += sites
    lines = [
        "# XDK missing functions (T1065)",
        "",
        "Generated by `python3 -m tools.xdk_missing`. Method: INFERRED (see the tool docstring).",
        "Missing = no registered HLE handler in the `xdk_report` registration snapshot, plus",
        "registered entries flagged PARTIAL. Not a proof of implementation either way.",
        "Sites are game `.text` direct calls and tail jumps (the surface generator count).",
        "Reached = a STOP address or in a `LAST GUEST CALLS` list of `tmp/play/*/stop.txt`;",
        "that census is only the last dozen calls, so `not seen` is not `never called`.",
        "",
        "| section | functions | sites | missing | missing sites |",
        "|---|---|---|---|---|",
    ]
    for section in sorted(per):
        f, s, mf, ms = per[section]
        lines.append(f"| {section} | {f} | {s} | {mf} | {ms} |")
    lines += [
        "",
        "| rank | address | section | name | sites | status | reached |",
        "|---|---|---|---|---|---|---|",
    ]
    for rank, (address, section, name, sites, partial, reached) in enumerate(missing, 1):
        status = (
            "PARTIAL"
            if partial
            else "registered only with --controllers/--synthetic-pad (not in default snapshot)"
            if address in OPT_IN_PAD
            else (
                "unregistered, original retained (default with --disc)"
                if address in RETAINED_DEFAULT
                else "unregistered, original retained (opt-in)"
                if address in RETAINED
                else "unregistered"
            )
        )
        lines.append(
            f"| {rank} | `0x{address:08X}` | {section} | {name or ''} | {sites} | "
            f"{status} | {reached} |"
        )
    total_registered = sum(1 for r in rows if registered[r[0]])
    unregistered = [m for m in missing if not registered[m[0]]]
    mode_labelled = [
        m
        for m in unregistered
        if m[0] in OPT_IN_PAD or m[0] in RETAINED_DEFAULT or m[0] in RETAINED
    ]
    lines += [
        "",
        "## Summary (computed, method INFERRED)",
        "",
        f"- {len(rows)} surface functions, {total_registered} registered in the snapshot,",
        f"  {len(rows) - total_registered} unregistered, {len(missing)} listed (unregistered plus",
        f"  {sum(1 for m in missing if m[4])} PARTIAL registered).",
        f"- {len(mode_labelled)} of the unregistered rows carry a mode label: opt-in pad",
        "  (`--controllers`/`--synthetic-pad`), retained XMV (default with `--disc`) or retained",
        "  shader-assembler bodies (`--native-shader-assembler`). They are not plain gaps and not",
        "  proof of completeness. Counts are the default `xdk_report` policy only (see",
        "  `docs/t1110-mode-report.md`: it is a mixed policy, not default host nor full coverage).",
    ]
    lines += ["", "PARTIAL notes:", ""]
    lines += [f"- `0x{a:08X}`: {note}" for a, note in sorted(PARTIAL.items())]
    lines += NOTES
    return "\n".join(lines) + "\n"


NOTES = [
    "",
    "## Findings and limits",
    "",
    "- The former stop at `0x003D59E0` was not an unregistered function. A handler IS registered",
    "  (`handler_upload_constants` in `src/gpu/d3d8_shader.c` and `d3d8_upload_vertex_program`",
    "  in `src/gpu/d3d8_vertex_program.c`) and refused the bulk upload until T1078 ported the",
    "  sized reservation `0x003D6B30`, which is NOT a surface address (internal, see",
    "  `docs/d3d8-draw-cascade.md`). Plain registration counting therefore under-reports gaps.",
    "- A committed or cached `build/xdk_report` can be stale (it once showed 78 registered).",
    "  Always rebuild the target from the checkout being reported (T1065 reconcile build: Ninja,",
    "  clang, Release). The counts in the Summary section are computed from the snapshot.",
    "- XNET registers only its three self-contained functions (T1070, see docs/xnet-offline.md).",
    "  XONLINE (T1071): 29 of 30 are registered by the opt-in `--xonline-offline` policy that",
    "  `xdk_report` applies (no-state wrappers measured under Unicorn, refused by name after",
    "  Startup), only `XOnlineTitleIdIsSameTitle` `0x00413005` stays a stop (the original",
    "  dereferences NULL with no state). XMV (7) is the retained original route, default",
    "  with `--disc` since T1093 (`--no-native-xmv` opts out).",
    "  Details: `docs/t1071-xonline-xmv.md`.",
    "- Not measured: whether any unregistered function is dead code for this title, and the",
    "  per-address call census (the play stop reports hold only the last dozen calls).",
    "- Reproduce: build target `xdk_report` (needs the gitignored `src/xbox/xdk_surface.*` and",
    "  `xdk_abi.inc`), run it to a JSON file, then",
    "  `python3 -m tools.xdk_missing --registration <json> --play-dir tmp/play`.",
]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--surface", type=Path, default=Path("src/xbox/xdk_surface.c"))
    p.add_argument("--registration", type=Path, default=None, help="xdk_report JSON output")
    p.add_argument(
        "--xdk-report",
        type=Path,
        default=Path("build/xdk_report"),
        help="run when no --registration",
    )
    p.add_argument("--play-dir", type=Path, default=Path("tmp/play"))
    p.add_argument("--out", type=Path, default=Path("docs/xdk-missing-functions.md"))
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rows = parse_surface(args.surface.read_text())
    if args.registration is not None:
        reg_text = args.registration.read_text()
    else:
        reg_text = subprocess.run(
            [str(args.xdk_report)], capture_output=True, text=True, timeout=60, check=True
        ).stdout
    registered = parse_registration(reg_text)
    stops, last = parse_census(args.play_dir)
    missing = classify(rows, registered, stops, last)
    args.out.write_text(render(rows, registered, missing))
    print(f"{len(rows)} surface functions, {len(missing)} missing or partial, wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
