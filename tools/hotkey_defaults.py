#!/usr/bin/env python3
# ruff: noqa: E501
"""T1632: the ONE Python source of truth for the default host hotkey chords.

The strings are the macros XINPUT_HOTKEY_DEFAULT_{PAD,KB}_{ADVANCE,MARK,DUMP,STOP} of src/input/xinput_hotkey.h
(tests/test_t1632_hotkey_defaults.py parses that header and fails when the two differ). The fish scripts under tmp/ ask
this tool for the `--hotkey` flags and for the words that announce the chords, so no script hardcodes a chord.

    python -m tools.hotkey_defaults flags --labels advance,mark,stop --dir DIR [--device pad|kb|both]
        one argv token per line: --hotkey SPEC ... --hotkey-dir DIR   (fish: set -l hk (python -m tools.hotkey_defaults ...))
    python -m tools.hotkey_defaults describe --labels mark,stop [--device pad|kb|both]
        a human table: label, what the host does, the chord on the pad and on the keyboard
    python -m tools.hotkey_defaults hint --label advance [--device pad|kb|both]
        ONE line for a prompt: 'hold Back+... for 0.5 s (press Back first), or press Ctrl+Shift+N in the game window'

Labels: advance and dump only write DIR/hotkey.N (a script reacts), mark and stop are acted on by the host itself.
Exit 2 on an unknown label, a repeated label or more than 8 flags.

The LEAD of the pad chords (the first key, the only one that reaches the game while the chord is made) is BACK. If Back does
something in the game at the screen you are on, set TSFP_HOTKEY_PAD_LEAD to another pad key before running a script, for
example `env TSFP_HOTKEY_PAD_LEAD=GUIDE fish tmp/record_mapmaker_route.fish`. GUIDE is not mapped to any game input, so it
never reaches the game (and never the recording), but many desktops and pads do not deliver it to the game window. The
keyboard chords already lead with Ctrl, which is unmapped.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

MAX_FLAGS = 8
KEYS_MIN = 2
KEYS_MAX = 5
HOLD_MAX_MS = 10000
PAD_DEFAULT_HOLD_MS = 500
KB_DEFAULT_HOLD_MS = 0
LABEL_RE = re.compile(r"[A-Za-z0-9_-]{1,31}")
DEVICES = ("pad", "kb")

PAD_KEYS = frozenset(
    "A B X Y LB RB START BACK GUIDE LSTICK RSTICK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT".split()
)
KB_KEYS = frozenset(
    [chr(code) for code in range(ord("A"), ord("Z") + 1)]
    + [str(digit) for digit in range(10)]
    + "UP DOWN LEFT RIGHT ENTER BACKSPACE SPACE TAB F1".split()
    + "CTRL SHIFT ALT LCTRL RCTRL LSHIFT RSHIFT LALT RALT".split()
)
DEVICE_KEYS = {"pad": PAD_KEYS, "kb": KB_KEYS}

# label -> device -> chord spec. Mirrors src/input/xinput_hotkey.h, keep the order of labels (it is the order of the flags).
DEFAULT_CHORDS: dict[str, dict[str, str]] = {
    "advance": {
        "pad": "pad=BACK+START+LB+RB@500:advance",
        "kb": "kb=CTRL+SHIFT+N:advance",
    },
    "mark": {
        "pad": "pad=BACK+START+LB+X@500:mark",
        "kb": "kb=CTRL+SHIFT+M:mark",
    },
    "dump": {
        "pad": "pad=BACK+START+LB+B@500:dump",
        "kb": "kb=CTRL+SHIFT+D:dump",
    },
    "stop": {
        "pad": "pad=BACK+START+LB+Y@1500:stop",
        "kb": "kb=CTRL+SHIFT+O@1000:stop",
    },
    "shot": {
        "pad": "pad=BACK+START+LB+A@500:shot",
        "kb": "kb=CTRL+SHIFT+S:shot",
    },
}

# What the label does. The host acts on mark and stop (src/host/hotkey_actions.c), every label also writes DIR/hotkey.N.
LABEL_EFFECT = {
    "advance": "continue the current prompt (file only, tools.action_profile reacts)",
    "mark": "place a record mark (the host does it, like SIGUSR2, only while recording)",
    "dump": "take a labelled guest dump (file only, tools.hotkey_watch dump-daemon reacts)",
    "stop": "end the run cleanly (the host does it, like closing the window)",
    "shot": "write shot-NNN.png of the presented frame and a shots.manifest line (the host does it, T1720)",
}
HOST_ACTION_LABELS = frozenset({"mark", "stop", "shot"})

KEY_WORDS = {
    "CTRL": "Ctrl",
    "SHIFT": "Shift",
    "ALT": "Alt",
    "LCTRL": "Left Ctrl",
    "RCTRL": "Right Ctrl",
    "LSHIFT": "Left Shift",
    "RSHIFT": "Right Shift",
    "LALT": "Left Alt",
    "RALT": "Right Alt",
    "START": "Start",
    "BACK": "Back",
    "GUIDE": "Guide",
    "LSTICK": "Left stick click",
    "RSTICK": "Right stick click",
    "DPAD_UP": "D-pad up",
    "DPAD_DOWN": "D-pad down",
    "DPAD_LEFT": "D-pad left",
    "DPAD_RIGHT": "D-pad right",
}


LEAD_ENV = "TSFP_HOTKEY_PAD_LEAD"
DEFAULT_PAD_LEAD = "BACK"


class HotkeyError(ValueError):
    """A bad hotkey spec, label or flag set."""


def pad_lead() -> str:
    """The first key of every pad chord: BACK, or TSFP_HOTKEY_PAD_LEAD (a pad key none of the chords already uses)."""
    lead = os.environ.get(LEAD_ENV, "").strip().upper() or DEFAULT_PAD_LEAD
    if lead not in PAD_KEYS:
        raise HotkeyError(f"{LEAD_ENV}={lead!r} is not a pad key ({' '.join(sorted(PAD_KEYS))})")
    return lead


def chord_spec(label: str, device: str) -> str:
    """The --hotkey value of `label` on `device`: the default chord with the pad lead substituted."""
    spec = DEFAULT_CHORDS[label][device]
    if device != "pad":
        return spec
    lead = pad_lead()
    head, _, rest = spec.partition("=")
    keys, sep, tail = rest.partition("@")
    parts = keys.split("+")
    if lead != parts[0] and lead in parts:
        raise HotkeyError(f"{LEAD_ENV}={lead}: {lead} is already a key of the {label} chord")
    parts[0] = lead
    return f"{head}={'+'.join(parts)}{sep}{tail}"


def parse_spec(text: str) -> dict:
    """Python mirror of the host grammar (xinput_hotkey.h): DEVICE=KEY+KEY[+KEY...][@HOLDMS]:LABEL.

    Returns {device, keys, hold_ms, label}. Raises HotkeyError on the first malformed part."""
    device, sep, rest = text.partition("=")
    if not sep or device not in DEVICES:
        raise HotkeyError(f"{text!r}: DEVICE must be pad or kb followed by '='")
    body, sep, label = rest.rpartition(":")
    if not sep or not LABEL_RE.fullmatch(label):
        raise HotkeyError(f"{text!r}: LABEL must be 1..31 of [A-Za-z0-9_-] after the last ':'")
    keytext, sep, holdtext = body.partition("@")
    if sep:
        if not holdtext.isdecimal():
            raise HotkeyError(f"{text!r}: HOLDMS must be a number")
        hold_ms = int(holdtext)
        if hold_ms > HOLD_MAX_MS:
            raise HotkeyError(f"{text!r}: HOLDMS {hold_ms} is above {HOLD_MAX_MS}")
    else:
        hold_ms = PAD_DEFAULT_HOLD_MS if device == "pad" else KB_DEFAULT_HOLD_MS
    keys = [key.upper() for key in keytext.split("+")]
    if not KEYS_MIN <= len(keys) <= KEYS_MAX:
        raise HotkeyError(f"{text!r}: {len(keys)} keys, want {KEYS_MIN}..{KEYS_MAX}")
    if len(set(keys)) != len(keys):
        raise HotkeyError(f"{text!r}: a key is listed twice")
    for key in keys:
        if key not in DEVICE_KEYS[device]:
            raise HotkeyError(f"{text!r}: {key!r} is not a {device} key")
    return {"device": device, "keys": keys, "hold_ms": hold_ms, "label": label}


def parse_labels(text: str) -> list[str]:
    """`advance,mark,stop` -> list, validated: known labels only, no repeat, at least one."""
    labels = [item.strip() for item in text.split(",") if item.strip()]
    if not labels:
        raise HotkeyError("no label given")
    for label in labels:
        if label not in DEFAULT_CHORDS:
            raise HotkeyError(f"unknown label {label!r} (known: {', '.join(DEFAULT_CHORDS)})")
    if len(set(labels)) != len(labels):
        raise HotkeyError(f"a label is listed twice: {','.join(labels)}")
    return labels


def devices_of(choice: str) -> list[str]:
    return list(DEVICES) if choice == "both" else [choice]


def specs_for(labels: list[str], device: str = "both") -> list[str]:
    """The --hotkey values, label by label, pad before kb. HotkeyError beyond MAX_FLAGS."""
    specs = [chord_spec(label, dev) for label in labels for dev in devices_of(device)]
    if len(specs) > MAX_FLAGS:
        raise HotkeyError(f"{len(specs)} --hotkey flags, the host takes at most {MAX_FLAGS}")
    return specs


def flag_tokens(labels: list[str], directory: str, device: str = "both") -> list[str]:
    if not directory or "\n" in directory:
        raise HotkeyError("--dir must be a non-empty single line path")
    tokens: list[str] = []
    for spec in specs_for(labels, device):
        tokens += ["--hotkey", spec]
    return tokens + ["--hotkey-dir", directory]


def _seconds(milliseconds: int) -> str:
    seconds = milliseconds / 1000
    return f"{seconds:g}"


def chord_text(label: str, device: str) -> str:
    """The words that tell the owner how to press the chord of `label` on `device` (pad or kb)."""
    parsed = parse_spec(chord_spec(label, device))
    names = [KEY_WORDS.get(key, key) for key in parsed["keys"]]
    chord = "+".join(names)
    hold = parsed["hold_ms"]
    if device == "pad":
        return f"hold {chord} for {_seconds(hold)} s (press {names[0]} first)"
    window = "in the game window"
    if hold:
        return f"hold {chord} for {_seconds(hold)} s {window}"
    return f"press {chord} {window}"


def hint_line(label: str, device: str = "both") -> str:
    return ", or ".join(chord_text(label, dev) for dev in devices_of(device))


def describe_lines(labels: list[str], device: str = "both") -> list[str]:
    lines: list[str] = []
    for label in labels:
        who = "host" if label in HOST_ACTION_LABELS else "file"
        lines.append(f"{label}: {LABEL_EFFECT[label]} [{who}]")
        for dev in devices_of(device):
            lines.append(f"    {dev:<3} {chord_text(label, dev)}")
    return lines


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.hotkey_defaults",
        description="T1632: the default host hotkey chords as --hotkey flags or as announcement text.",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("flags", "print one argv token per line: --hotkey SPEC ... --hotkey-dir DIR"),
        ("describe", "print a human table of the chords"),
    ):
        command = sub.add_parser(name, help=help_text)
        command.add_argument(
            "--labels", required=True, help="comma list of advance, mark, dump, stop, shot"
        )
        command.add_argument("--device", choices=("pad", "kb", "both"), default="both")
        if name == "flags":
            command.add_argument("--dir", required=True, help="the hotkey directory (--hotkey-dir)")
    hint = sub.add_parser("hint", help="print one line telling how to press the chord of one label")
    hint.add_argument("--label", required=True, help="one of advance, mark, dump, stop, shot")
    hint.add_argument("--device", choices=("pad", "kb", "both"), default="both")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "hint":
            lines = [hint_line(parse_labels(args.label)[0], args.device)]
            if "," in args.label:
                raise HotkeyError("hint takes ONE label")
        else:
            labels = parse_labels(args.labels)
            if args.command == "flags":
                lines = flag_tokens(labels, args.dir, args.device)
            else:
                lines = describe_lines(labels, args.device)
    except HotkeyError as error:
        print(f"hotkey_defaults: {error}", file=sys.stderr)
        return 2
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
