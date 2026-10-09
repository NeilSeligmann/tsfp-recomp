# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Pad scripts for the front end menus (T845): FABRICATED input for `--synthetic-pad --pad-script`.

The T803/T821 recipe walks the title to the main menu with four fixed A presses (title screen at poll 1,500, the second A at 3,000 that
joins, the third at 3,200 that accepts the default name `Player 1`, the fourth at 3,300 that picks the storage device). A poll shows in the
title about 72 presents later (`POLL_TO_PRESENT`). From poll 3,400 the main menu is up and the steps given here press its pad.

A step is one of `A`, `B`, `UP`, `DOWN`, `LEFT`, `RIGHT` (any token the scripted pad knows). Every step is held `HOLD` polls and followed by
`GAP_MOVE` polls of rest after a D-pad step or `GAP_SELECT` after the others (a screen takes about 15 presents to build, the focus moves
on the next update). `DOWN*3` repeats a step. The focus order is `docs/boot-frontier.md` row 28: DOWN moves to the next enabled entry and
wraps, UP the previous one, entries flagged disabled (`[entry + 0x28] & 4`) are skipped.

    python3 -m tools.frontend_menu_pad --steps "DOWN*6,A,DOWN,A" > tmp/pad.txt
"""

import argparse
import re

# the fixed lead in of the T821 recipe: (poll of the press, button)
LEAD_IN = ((1500, "A"), (3000, "A"), (3200, "A"), (3300, "A"))
FIRST_STEP_POLL = 3400
HOLD = 4
GAP_MOVE = 26
GAP_SELECT = 96
POLL_TO_PRESENT = 72
DPAD = {"UP", "DOWN", "LEFT", "RIGHT"}


def expand(steps: str) -> list[str]:
    """`DOWN*3,A` -> `[DOWN, DOWN, DOWN, A]`."""
    buttons: list[str] = []
    for token in (part.strip() for part in steps.split(",")):
        if not token:
            continue
        match = re.fullmatch(r"([A-Z]+)(?:\*(\d+))?", token)
        if not match:
            raise ValueError(f"bad step {token!r}, expected NAME or NAME*COUNT")
        buttons.extend([match[1]] * int(match[2] or 1))
    return buttons


def presses(steps: str) -> list[tuple[int, str]]:
    """(poll of the press, button) for the lead in and every step."""
    result = list(LEAD_IN)
    poll = FIRST_STEP_POLL
    for button in expand(steps):
        result.append((poll, button))
        poll += HOLD + (GAP_MOVE if button in DPAD else GAP_SELECT)
    return result


def script(steps: str) -> str:
    """The pad script text: rest lines and `HOLD` poll press lines."""
    lines: list[str] = []
    position = 0
    for poll, button in presses(steps):
        lines.append(str(poll - position))
        lines.append(f"{HOLD} {button}")
        position = poll + HOLD
    return "\n".join(lines) + "\n"


def present_of(poll: int) -> int:
    """The present a poll shows at (measured: poll 1,500 is present 1,572)."""
    return poll + POLL_TO_PRESENT


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--steps",
        default="",
        help="comma separated buttons after the lead in, e.g. DOWN*6,A,DOWN,A (default: none, stops on the main menu)",
    )
    args = parser.parse_args()
    print(script(args.steps), end="")


if __name__ == "__main__":
    main()
