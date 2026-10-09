#!/usr/bin/env python3
# ruff: noqa: E501
"""T1709: append a scripted walk of pad presses to a route (headless regression of the settings dump session).

The route is normally the output of `python -m tools.settings_route` (it ends on the Settings > Controls page). The walk file
lists one press per line, `BUTTON [xN] [rest=POLLS] [# label]`, `//` lines are comments, BUTTON is one of UP DOWN LEFT RIGHT A B X Y
START BACK. Each press is HOLD polls down (default 6) then REST polls of rest (default 60), written before the `# polls:` trailer,
which is rewritten. `OUT.labels.json` lists the route poll of every press so a report can be matched to the plan.

    python -m tools.settings_walk BASE_ROUTE --walk tools/data/settings_walk.txt --out OUT [--final-rest 150] [--hold 6] [--rest 60]

The walk replays headless with `--dump-on-button` (no `--dump-button-after-replay`), see docs/t1709-settings-dump.md.
Exit codes: 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

DIRECTIONS = ("UP", "DOWN", "LEFT", "RIGHT")
BUTTONS = DIRECTIONS + ("A", "B", "X", "Y", "START", "BACK")
LINE_RE = re.compile(
    r"^(?P<button>[A-Z]+)(?:\s+x(?P<count>[0-9]+))?(?:\s+rest=(?P<rest>[0-9]+))?(?:\s*#\s*(?P<label>.*))?$"
)
POLLS_PREFIX = "# polls:"


class WalkError(ValueError):
    pass


def route_polls(lines: list[str]) -> int:
    """Sum of the poll counts of the pad lines (comment and blank lines count nothing)."""
    total = 0
    for line in lines:
        if not line.strip() or line.startswith("#"):
            continue
        total += int(line.split()[0])
    return total


def parse_walk(text: str, default_rest: int) -> list[tuple[str, int, int, str]]:
    """(button, count, rest polls, label) per walk line."""
    entries: list[tuple[str, int, int, str]] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        match = LINE_RE.match(line)
        if match is None or match.group("button") not in BUTTONS:
            raise WalkError(f"walk line {number}: cannot read {line!r}")
        count = int(match.group("count") or 1)
        if count < 1:
            raise WalkError(f"walk line {number}: count must be at least 1")
        rest = int(match.group("rest")) if match.group("rest") is not None else default_rest
        entries.append((match.group("button"), count, rest, match.group("label") or ""))
    if not entries:
        raise WalkError("the walk has no presses")
    return entries


def build(
    base: str, walk: str, hold: int, rest: int, final_rest: int
) -> tuple[str, list[dict[str, object]]]:
    lines = base.splitlines()
    if not lines or not lines[-1].startswith(POLLS_PREFIX):
        raise WalkError("the base route has no '# polls:' trailer as its last line")
    body = lines[:-1]
    declared = int(lines[-1][len(POLLS_PREFIX) :].strip())
    if declared != route_polls(body):
        raise WalkError(
            f"the base route trailer says {declared} polls but its lines sum to {route_polls(body)}"
        )
    total = declared
    labels: list[dict[str, object]] = []
    for button, count, rest_polls, label in parse_walk(walk, rest):
        token = button if button in DIRECTIONS else f"{button}=255"
        for _ in range(count):
            labels.append({"route_poll": total, "button": button, "label": label})
            body.append(f"{hold} {token}")
            body.append(str(rest_polls))
            total += hold + rest_polls
    body.append(str(final_rest))
    total += final_rest
    body.append(f"{POLLS_PREFIX} {route_polls(body)}")
    if route_polls(body[:-1]) != total:
        raise WalkError("internal poll count mismatch")
    return "\n".join(body) + "\n", labels


def positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "base", type=Path, help="route that ends on the Controls page (tools.settings_route output)"
    )
    parser.add_argument(
        "--walk", type=Path, default=Path("tools/data/settings_walk.txt"), help="walk file"
    )
    parser.add_argument(
        "--out", type=Path, required=True, help="route to write (OUT.labels.json too)"
    )
    parser.add_argument(
        "--hold", type=positive, default=6, help="polls a button is held (default 6)"
    )
    parser.add_argument(
        "--rest", type=positive, default=60, help="default rest polls after a press (default 60)"
    )
    parser.add_argument(
        "--final-rest", type=positive, default=150, help="rest polls at the very end (default 150)"
    )
    parser.add_argument("--force", action="store_true", help="replace an existing output")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.out.exists() and not args.force:
        print(f"settings_walk: {args.out} exists (use --force)", file=sys.stderr)
        return 2
    if args.out.resolve() == args.base.resolve():
        print("settings_walk: --out must not be the base route", file=sys.stderr)
        return 2
    try:
        text, labels = build(
            args.base.read_text(encoding="utf-8"),
            args.walk.read_text(encoding="utf-8"),
            args.hold,
            args.rest,
            args.final_rest,
        )
    except (OSError, WalkError, ValueError) as error:
        print(f"settings_walk: {error}", file=sys.stderr)
        return 2
    args.out.write_text(text, encoding="utf-8")
    Path(f"{args.out}.labels.json").write_text(
        json.dumps(labels, indent=0) + "\n", encoding="utf-8"
    )
    print(
        f"settings_walk: {args.out}: {len(labels)} presses, {route_polls(text.splitlines()[:-1])} route polls"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
