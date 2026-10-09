#!/usr/bin/env python3
# ruff: noqa: E501
"""T1087 xemu Story input probe: per input phase, a before, a during (key held) and an after frame.

Drives a RUNNING xemu launched with `ctl start --keyboard-pad` (digital keys: sticks and triggers are
full deflection only).  Phases are `name=KEY[+KEY...]` joined by commas, `name=-` is the no-key control.

    python -m tools.xemu.story_input_probe --out-dir tmp/t1087/xemu --hold 4 --phases idle=-,move_fwd=e,fire=o
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

# xemu's default keyboard map (tools/xemu/ctl.py KEYBOARD_PAD), phase name -> xdotool keys.
DEFAULT_PHASES = (
    "idle=-,lstick_up=e,lstick_down=d,lstick_left=s,lstick_right=f,rstick_right=l,rstick_left=j,"
    "rstick_up=i,rstick_down=k,rtrigger=o,ltrigger=w,btn_a=a,btn_b=b,btn_x=x,btn_y=y,white=1,black=2"
)


# The same key in xemu's keyboard map and in our pad script: full deflection both sides.
KEY_TO_TOKEN = {
    "e": "LY=32767",
    "d": "LY=-32768",
    "s": "LX=-32768",
    "f": "LX=32767",
    "i": "RY=32767",
    "k": "RY=-32768",
    "j": "RX=-32768",
    "l": "RX=32767",
    "o": "RT=255",
    "w": "LT=255",
    "a": "A=255",
    "b": "B=255",
    "x": "X=255",
    "y": "Y=255",
    "1": "WHITE=255",
    "2": "BLACK=255",
    "Return": "START",
    "BackSpace": "BACK",
}


def pad_script(
    phases: list[tuple[str, list[str]]], on: int, off: int, record_body: str = ""
) -> str:
    """Pad script (`--pad-script` syntax): per phase `on` polls with the tokens, then `off` polls at rest."""
    lines = [
        "# T1087 phase script (FABRICATED scripted pad): same phases as the xemu keyboard probe"
    ]
    if record_body:
        lines.append(record_body.rstrip("\n"))
    for name, keys in phases:
        tokens = " ".join(KEY_TO_TOKEN[key] for key in keys)
        lines.append(f"{on} {tokens}".rstrip() + f"  # {name}")
        lines.append(f"{off}")
    return "\n".join(lines) + "\n"


def parse_phases(text: str) -> list[tuple[str, list[str]]]:
    phases = []
    for item in text.split(","):
        name, _, keys = item.partition("=")
        if not name or not keys:
            raise ValueError(f"bad phase {item!r}, expected name=KEY[+KEY] or name=-")
        phases.append((name, [] if keys == "-" else keys.split("+")))
    names = [name for name, _ in phases]
    if len(set(names)) != len(names):
        raise ValueError("duplicate phase name")
    return phases


def ctl(*arguments: str) -> None:
    subprocess.run([sys.executable, "-m", "tools.xemu.ctl", *arguments], check=True, timeout=60)


def run(out_dir: Path, phases: list[tuple[str, list[str]]], hold: float, settle: float) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for index, (name, keys) in enumerate(phases):
        stem = f"{index:02d}-{name}"
        ctl("shot", str(out_dir / f"{stem}-pre.png"))
        during = str(out_dir / f"{stem}-during.png")
        if keys:
            ctl("key", *keys, "--chord", "--hold", str(hold), "--gap", "0", "--shot", during)
        else:
            time.sleep(hold)
            ctl("shot", during)
        time.sleep(settle)
        ctl("shot", str(out_dir / f"{stem}-post.png"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--out-dir", required=True, help="frame directory (gitignored, retail frames)"
    )
    parser.add_argument(
        "--phases", default=DEFAULT_PHASES, help="name=KEY[+KEY],..., name=- is no key"
    )
    parser.add_argument("--hold", type=float, default=4.0, help="seconds each key is held")
    parser.add_argument(
        "--settle", type=float, default=3.0, help="seconds after release before the post frame"
    )
    parser.add_argument("--pad-out", default=None, help="write the matching pad script and exit")
    parser.add_argument("--pad-on", type=int, default=150, help="polls per phase with input")
    parser.add_argument("--pad-off", type=int, default=150, help="polls of rest after each phase")
    parser.add_argument("--pad-lead", type=int, default=0, help="rest polls before the first phase")
    parser.add_argument("--pad-record", default=None, help="recorded input file to play first")
    args = parser.parse_args()
    if args.pad_out:
        body = ""
        if args.pad_record:
            lines = Path(args.pad_record).read_text().splitlines()
            body = "\n".join(line for line in lines if line.strip() and not line.startswith("#"))
        if args.pad_lead:
            body += f"\n{args.pad_lead}"
        out = Path(args.pad_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            pad_script(parse_phases(args.phases), args.pad_on, args.pad_off, body.strip())
        )
        return 0
    run(Path(args.out_dir), parse_phases(args.phases), args.hold, args.settle)
    return 0


if __name__ == "__main__":
    sys.exit(main())
