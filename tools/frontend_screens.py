# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Read a front end screen's widgets, entries and screen pushes out of the retail bytes (T845), no emulation.

A front end SCREEN RECORD is four dwords copied to a widget (`0x78F30`): `+0` the init (called once, at create, from `0x78F74`), `+4`
the update (every present from the draw walker `0x7AD78`), `+8` and `+C` hooks. The init builds the widget with a handful of calls, all
cdecl and recognised here from their pushed arguments:

- `0x75B70(flags, title, width, select, back, ...)`: the widget header. `title` is a `0x3D7B0(label id)` result, `select` the widget
  level select callback (A on any entry, caller `0x7B17B`), `back` the B callback;
- `0x79D60(label, index, flags, width, select, draw)`: one entry. `select` is the entry's own select callback (caller `0x7B0F8`);
- `0x74A00(slot, label)`: a button prompt line (Select, Back);
- `0x79B70(parent, record)`: push the screen `record` (a select callback does this to move on), `0x32AFE0` is the same with a wrapper;
- `mov [0x78AD2C], imm`: the front end MODE word (`docs/boot-frontier.md` row 28).

This is static: it follows the instruction stream linearly and cannot evaluate a register argument (printed as its register name), a
conditional entry shows once. `python3 -m tools.frontend_screens --xbe tmp/oxm-extract/retail/default.xbe 0x2D0BE0`.
"""

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

import capstone

from tools.xppscan.image import Image

LABEL = 0x0003D7B0
WIDGET, ENTRY, BUTTON, PUSH, PUSH_WRAPPED = 0x75B70, 0x79D60, 0x74A00, 0x79B70, 0x32AFE0
MODE_WORD = 0x78AD2C
CALLS = {WIDGET: "widget", ENTRY: "entry", BUTTON: "button", PUSH: "push", PUSH_WRAPPED: "push"}
MAX_BYTES = 0x1800
TAIL_JUMP_SPAN = 0x400  # a jmp beyond this, or before the start, leaves the function


@dataclass(frozen=True)
class Event:
    address: int
    kind: str
    args: tuple[str, ...]

    def ints(self) -> list[int | None]:
        """Each argument as an integer, None for a register or an unresolved label."""
        return [
            int(arg, 0) if re.fullmatch(r"-?(0x[0-9a-f]+|\d+)", arg) else None for arg in self.args
        ]


def decode(image: Image, start: int, limit: int = MAX_BYTES) -> list[Event]:
    """The widget, entry, button, push and mode events of the function at `start`, in address order.

    Stops at the first int3 (the padding after a function) and after a tail jump out of the function (a `tail` event). A `0x3D7B0(imm)` argument is kept as `L<id>` so a later call that takes its result shows
    which label it carries."""
    code = image.read(start, limit)
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(disassembler.disasm(code, start))
    events: list[Event] = []
    pending: list[str] = []
    result = ""
    for instruction in instructions:
        mnemonic, operands = instruction.mnemonic, instruction.op_str
        if mnemonic == "int3":
            break
        if mnemonic == "jmp" and operands.startswith("0x"):
            target = int(operands, 16)
            if not start <= target <= instruction.address + TAIL_JUMP_SPAN:
                events.append(Event(instruction.address, "tail", (operands,)))
                break
        if mnemonic == "push":
            pending.append(result if operands == "eax" and result else operands)
        elif mnemonic == "call" and operands.startswith("0x"):
            target = int(operands, 16)
            if target == LABEL:
                result = f"L{pending.pop()}" if pending else "L?"
            else:
                arguments = tuple(reversed(pending))
                pending = []
                result = ""
                if target in CALLS:
                    events.append(Event(instruction.address, CALLS[target], arguments))
        elif mnemonic == "mov" and (
            match := re.fullmatch(rf"dword ptr \[{MODE_WORD:#x}\], (0x[0-9a-f]+|\d+)", operands)
        ):
            events.append(Event(instruction.address, "mode", (match[1],)))
    return events


def label_id(argument: str) -> int | None:
    """`L0x366` -> 0x366, anything else (a register, a computed label) -> None."""
    match = re.fullmatch(r"L(0x[0-9a-f]+|\d+)", argument)
    return int(match[1], 0) if match else None


def records(image: Image, address: int, count: int = 4) -> list[int]:
    """The dwords of a screen record."""
    return [image.u32(address + 4 * index) for index in range(count)]


def jump_table(image: Image, table: int, count: int) -> list[int]:
    return [image.u32(table + 4 * index) for index in range(count)]


def describe(image: Image, start: int, labels: object | None = None) -> str:
    lines = [f"screen init {start:#010x}"]
    for event in decode(image, start):
        shown = []
        for argument in event.args:
            identifier = label_id(argument)
            if identifier is not None and labels is not None:
                shown.append(f"{argument} {labels.text(identifier)!r}")  # type: ignore[attr-defined]
            else:
                shown.append(argument)
        lines.append(f"  {event.address:08X} {event.kind:6} {' '.join(shown)}")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--xbe", type=Path, default=Path("tmp/oxm-extract/retail/default.xbe"))
    parser.add_argument("--iso", type=Path, help="also print the label texts from this disc image")
    parser.add_argument("starts", nargs="+", help="init addresses, for example 0x2D0BE0")
    args = parser.parse_args()
    image = Image(args.xbe)
    labels = None
    if args.iso:
        from tools.frontend_labels import load

        labels = load(args.iso)
    for start in args.starts:
        print(describe(image, int(start, 0), labels))


if __name__ == "__main__":
    main()
