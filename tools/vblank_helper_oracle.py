# SPDX-License-Identifier: GPL-3.0-or-later
"""T372: what the ORIGINAL vblank helper 0x003DC2E0 does, measured under the Unicorn oracle.

The helper runs from the vblank DPC (0x003DCA90) with ECX = context = device + 0x1C28. The C port
only has its event half (d3d8_gpu_wait_vblank). This module runs the retail bytes from the state
CreateDevice leaves, over device states with NON-EMPTY swap, threshold and flip fields, and
reports every observable effect, so each effect can be modelled or refused by evidence:

  - dwords changed in the D3D region (named as context offsets),
  - NV2A register writes, in order, and register reads,
  - port reads (the field status port 0x80C0),
  - rdtsc reads (the timestamp, 0x003DC0D0),
  - kernel calls (KeSetEvent on the vblank event),
  - every callback invocation with the three dword record it receives.

The model is the one tools.d3dscan.oracle documents: no hardware, a dictionary of registers, one
extra seam here for the port read and for rdtsc (both are inputs the case chooses, never
measurements of hardware).

    python -m tools.vblank_helper_oracle build/default.xbe
    python -m tools.vblank_helper_oracle build/default.xbe --case flip-due

Needs `unicorn`. Read-only: it opens the image and prints.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.d3dscan import oracle as oracle_module
from tools.d3dscan.oracle import D3D_HIGH, D3D_LOW, DEVICE_BASE, MMIO_BASE, Oracle

HELPER = 0x003DC2E0
RDTSC_FUNCTION = 0x003DC0D0
FLIP_GLOBAL = 0x003E6408
# A guest address inside the mapped image that the case turns into the callback.
CALLBACK_ENTRY = 0x00890000
CONTEXT = DEVICE_BASE + 0x1C28

# Context offsets (the doc in docs/vblank-delivery.md "T183c results", plus the flip slots).
CALLBACK = 0x190
EVENT = 0x194
MODE_WORD = 0x1B4
DISPLAY_START_DISABLED = 0x1B8
FLIP_INDEX = 0x1BC
COUNT = 0x1C0
THRESHOLD = 0x1C4
FIELD_STATUS = 0x1D0
TIMESTAMP_DELTA = 0x1D4
TIMESTAMP_LAST = 0x1D8
SLOT_BASE = 0x174
SLOT_BYTES = 12
GAMMA_PENDING = 0x7DC
GAMMA_RAMPS = 0x1DC

FIELD_NAMES = {
    CALLBACK: "callback",
    EVENT + 4: "event.SignalState",
    MODE_WORD: "mode",
    DISPLAY_START_DISABLED: "display_start_disabled",
    FLIP_INDEX: "flip_index",
    COUNT: "count",
    THRESHOLD: "threshold",
    FIELD_STATUS: "field_status",
    TIMESTAMP_DELTA: "timestamp_delta",
    TIMESTAMP_LAST: "timestamp_last",
    SLOT_BASE: "slot0.pending",
    SLOT_BASE + 4: "slot0.target",
    SLOT_BASE + 8: "slot0.value",
    SLOT_BASE + SLOT_BYTES: "slot1.pending",
    SLOT_BASE + SLOT_BYTES + 4: "slot1.target",
    SLOT_BASE + SLOT_BYTES + 8: "slot1.value",
    GAMMA_PENDING: "gamma0.pending",
    GAMMA_PENDING + 4: "gamma1.pending",
}


class Seams:
    """The two inputs the helper takes from hardware, installed on any Oracle: the field status
    port read and rdtsc (0x003DC0D0). Both are chosen by the case, never measured. Also the
    callback address, which records the three dword record it is called with."""

    def __init__(self, oracle: Oracle) -> None:
        from unicorn import UC_HOOK_INSN
        from unicorn.x86_const import UC_X86_INS_IN

        self.port_reads: list[int] = []
        self.rdtsc_reads: list[int] = []
        self.callbacks: list[tuple[int, int, int]] = []
        self.port_value = 0
        self.tsc = 0
        oracle.emulator.hook_add(UC_HOOK_INSN, self._port_in, None, 1, 0, UC_X86_INS_IN)
        oracle.hook_function(RDTSC_FUNCTION, 0, self._rdtsc)
        oracle.hook_function(CALLBACK_ENTRY, 0, self._callback)

    def _port_in(self, _emulator: object, port: int, _size: int, _user: object) -> int:
        self.port_reads.append(port)
        return self.port_value

    def _rdtsc(self, oracle: Oracle, _esp: int) -> int:
        from unicorn.x86_const import UC_X86_REG_EDX

        self.rdtsc_reads.append(self.tsc)
        oracle.emulator.reg_write(UC_X86_REG_EDX, (self.tsc >> 32) & 0xFFFFFFFF)
        return self.tsc & 0xFFFFFFFF

    def _callback(self, oracle: Oracle, esp: int) -> int:
        record = oracle.read32(esp + 4)
        self.callbacks.append(struct.unpack("<3I", oracle.read_bytes(record, 12)))
        return 0

    def clear(self) -> None:
        self.port_reads.clear()
        self.rdtsc_reads.clear()
        self.callbacks.clear()


class HelperOracle(Oracle):
    """The oracle plus a register access log and the hardware seams."""

    def __init__(self, xbe: Path) -> None:
        self.register_writes: list[tuple[int, int, int]] = []
        self.register_reads: list[tuple[int, int]] = []
        super().__init__(xbe, instant_gpu=True)
        self.seams = Seams(self)

    def _register_write(
        self, emulator: object, offset: int, size: int, value: int, user: object
    ) -> None:
        self.register_writes.append((MMIO_BASE + offset, size, value))
        super()._register_write(emulator, offset, size, value, user)

    def _register_read(self, emulator: object, offset: int, size: int, user: object) -> int:
        self.register_reads.append((MMIO_BASE + offset, size))
        return super()._register_read(emulator, offset, size, user)


@dataclass
class Effects:
    """Everything one helper run did, relative to the state it started from."""

    changed: dict[str, tuple[int, int]] = field(default_factory=dict)
    flip_global: tuple[int, int] = (0, 0)
    register_writes: list[tuple[int, int, int]] = field(default_factory=list)
    register_reads: int = 0
    port_reads: list[int] = field(default_factory=list)
    rdtsc_reads: list[int] = field(default_factory=list)
    kernel_calls: list[str] = field(default_factory=list)
    callbacks: list[tuple[int, int, int]] = field(default_factory=list)
    return_value: int = 0


def new_oracle(xbe: Path) -> HelperOracle:
    """A fresh oracle past CreateDevice, the state every case starts from."""
    oracle = HelperOracle(xbe)
    created = oracle_module.create_device(oracle)
    if not created.succeeded:
        raise oracle_module.OracleError("the oracle's CreateDevice failed")
    # CreateDevice does not leave a vblank to register for: the device has no callback.
    oracle.register_writes.clear()
    oracle.register_reads.clear()
    oracle.kernel_calls.clear()
    oracle.kernel_log.clear()
    return oracle


def name_of(offset: int) -> str:
    return FIELD_NAMES.get(offset, f"ctx+{offset:#x}")


def run_helper(oracle: HelperOracle, fields: dict[int, int], tsc: int = 0x100000) -> Effects:
    """Seed context dwords (ctx offset -> value), run the original helper once, diff."""
    for offset, value in fields.items():
        oracle.write32(CONTEXT + offset, value)
    oracle.seams.tsc = tsc
    before = oracle.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)
    oracle.register_writes.clear()
    oracle.register_reads.clear()
    oracle.seams.clear()
    oracle.kernel_calls.clear()
    returned = oracle.run(HELPER, [], ecx=CONTEXT)
    after = oracle.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)
    effects = Effects(
        register_writes=list(oracle.register_writes),
        register_reads=len(oracle.register_reads),
        port_reads=list(oracle.seams.port_reads),
        rdtsc_reads=list(oracle.seams.rdtsc_reads),
        kernel_calls=list(oracle.kernel_calls),
        callbacks=list(oracle.seams.callbacks),
        return_value=returned,
    )
    for offset in range(0, len(before), 4):
        old = struct.unpack_from("<I", before, offset)[0]
        new = struct.unpack_from("<I", after, offset)[0]
        if old == new:
            continue
        address = D3D_LOW + offset
        if address == FLIP_GLOBAL:
            effects.flip_global = (old, new)
        elif CONTEXT <= address < CONTEXT + 0x1000:
            effects.changed[name_of(address - CONTEXT)] = (old, new)
        else:
            effects.changed[f"{address:#010x}"] = (old, new)
    return effects


def slot(index: int, pending: int, target: int, value: int) -> dict[int, int]:
    base = SLOT_BASE + SLOT_BYTES * index
    return {base: pending, base + 4: target, base + 8: value}


@dataclass(frozen=True)
class Case:
    name: str
    note: str
    fields: Callable[[], dict[int, int]]
    port_value: int = 0
    tsc: int = 0x100000


def _empty() -> dict[int, int]:
    return {COUNT: 1, THRESHOLD: 0}


def _callback_set() -> dict[int, int]:
    return {COUNT: 1, THRESHOLD: 0, CALLBACK: CALLBACK_ENTRY, FLIP_INDEX: 5}


CASES: tuple[Case, ...] = (
    Case("empty", "count 1, threshold 0, no flips, no callback", _empty),
    Case("timestamp-first", "previous timestamp 0: delta is NOT written", _empty, tsc=0x2000),
    Case(
        "timestamp-second",
        "previous timestamp set: delta = tsc low32 - previous",
        lambda: {COUNT: 1, THRESHOLD: 0, TIMESTAMP_LAST: 0x1000},
        tsc=0x1BA5E3F,
    ),
    Case(
        "timestamp-wrap",
        "low 32 bits only: the 32-bit subtraction wraps",
        lambda: {COUNT: 1, THRESHOLD: 0, TIMESTAMP_LAST: 0xFFFFFF00},
        tsc=0x1_0000_0100,
    ),
    Case(
        "threshold-hit",
        "count + 1 == threshold, no flip: threshold++",
        lambda: {COUNT: 4, THRESHOLD: 5},
    ),
    Case(
        "threshold-behind",
        "threshold already behind the new count: unchanged",
        lambda: {COUNT: 4, THRESHOLD: 2},
    ),
    Case(
        "callback",
        "callback set: record [count, flip_index, flags] by pointer",
        _callback_set,
    ),
    Case(
        "callback-threshold",
        "callback set and the threshold hit: flags 2",
        lambda: {**_callback_set(), COUNT: 4, THRESHOLD: 5},
    ),
    Case(
        "mode-skips-port",
        "mode bit 0x1000000 set: the field status port is not read",
        lambda: {COUNT: 1, THRESHOLD: 0, MODE_WORD: 0x03480104},
        port_value=0x20,
    ),
    Case(
        "port-field-clear",
        "mode bit clear: port 0x80C0 read, bit 5 clear gives field_status 1",
        lambda: {COUNT: 1, THRESHOLD: 0, FIELD_STATUS: 7},
        port_value=0x00,
    ),
    Case(
        "port-field-set",
        "port bit 5 set gives field_status 0",
        lambda: {COUNT: 1, THRESHOLD: 0, FIELD_STATUS: 7},
        port_value=0x20,
    ),
    Case(
        "port-field-flipped",
        "ctx+8 top bits 01 inverts the port result",
        lambda: {COUNT: 1, THRESHOLD: 0, FIELD_STATUS: 7, 8: 0x40000000},
        port_value=0x00,
    ),
    Case(
        "flip-not-due",
        "slot 0 pending for a later count: no flip, threshold untouched",
        lambda: {COUNT: 1, THRESHOLD: 0, **slot(0, 1, 9, 0x01234000)},
    ),
    Case(
        "flip-due",
        "slot 0 pending for count + 1: flip, consumer index +1, flags 1",
        lambda: {
            **_callback_set(),
            FLIP_INDEX: 4,
            COUNT: 3,
            THRESHOLD: 4,
            **slot(0, 1, 4, 0x01234000),
        },
    ),
    Case(
        "flip-due-display-disabled",
        "as flip-due with +0x1B8 clear: the display start register is written",
        lambda: {
            COUNT: 3,
            THRESHOLD: 0,
            DISPLAY_START_DISABLED: 0,
            **slot(0, 1, 4, 0x01234000),
        },
    ),
    Case(
        "flip-due-gamma",
        "flip with a pending gamma ramp: the ramp is uploaded and cleared",
        lambda: {
            COUNT: 3,
            THRESHOLD: 0,
            GAMMA_PENDING: 1,
            **slot(0, 1, 4, 0x01234000),
        },
    ),
    Case(
        "flip-chain",
        "both slots due the same vblank: both flip, consumer index +2",
        lambda: {
            COUNT: 3,
            THRESHOLD: 0,
            **slot(0, 1, 4, 0x01234000),
            **slot(1, 1, 4, 0x01235000),
        },
    ),
    Case(
        "flip-chain-second-later",
        "second slot due later: only the first flips",
        lambda: {
            COUNT: 3,
            THRESHOLD: 0,
            **slot(0, 1, 4, 0x01234000),
            **slot(1, 1, 9, 0x01235000),
        },
    ),
    Case(
        "flip-second-slot-only",
        "consumer index 1 with slot 1 pending and due: slot 1 flips",
        lambda: {
            COUNT: 3,
            THRESHOLD: 0,
            FLIP_INDEX: 1,
            **slot(1, 1, 4, 0x01235000),
        },
    ),
    Case(
        "other-slot-pending",
        "only the NON-current slot pending: nothing flips",
        lambda: {COUNT: 1, THRESHOLD: 0, FLIP_INDEX: 0, **slot(1, 1, 2, 0x01235000)},
    ),
)


def run_case(xbe: Path, case: Case) -> Effects:
    oracle = new_oracle(xbe)
    oracle.seams.port_value = case.port_value
    return run_helper(oracle, case.fields(), case.tsc)


def grouped_writes(
    writes: list[tuple[int, int, int]],
) -> list[tuple[int, int, int, int]]:
    """Collapse the ramp upload: consecutive writes to one register become one line per value
    run, and a long run of distinct values to the same register is summarised as its first value
    and its length."""
    grouped: list[tuple[int, int, int, int]] = []
    for address, size, value in writes:
        if grouped and grouped[-1][:3] == (address, size, value):
            grouped[-1] = (address, size, value, grouped[-1][3] + 1)
        else:
            grouped.append((address, size, value, 1))
    return grouped


def format_effects(case: Case, effects: Effects) -> str:
    lines = [f"[{case.name}] {case.note}"]
    for name, (old, new) in sorted(effects.changed.items()):
        lines.append(f"    {name}: {old:#x} -> {new:#x}")
    if effects.flip_global != (0, 0):
        lines.append(
            f"    DAT_003e6408: {effects.flip_global[0]:#x} -> {effects.flip_global[1]:#x}"
        )
    runs = grouped_writes(effects.register_writes)
    shown = runs if len(runs) <= 12 else [*runs[:6], None, *runs[-3:]]
    for item in shown:
        if item is None:
            total = len(effects.register_writes)
            lines.append(f"    ... {len(runs) - 9} more write runs, {total} writes in all")
            continue
        address, size, value, repeat = item
        times = f" x{repeat}" if repeat > 1 else ""
        lines.append(f"    mmio write{size * 8} {address:#010x} <- {value:#x}{times}")
    lines.append(f"    mmio reads {effects.register_reads}, port reads {effects.port_reads}")
    lines.append(f"    rdtsc reads {len(effects.rdtsc_reads)}, kernel {effects.kernel_calls}")
    for record in effects.callbacks:
        lines.append(f"    callback record [{record[0]}, {record[1]}, {record[2]}]")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.vblank_helper_oracle",
        description="Run the retail vblank helper 0x3DC2E0 under the oracle over chosen states.",
    )
    parser.add_argument("xbe", type=Path, help="path to the retail XBE")
    parser.add_argument("--case", help="run one named case instead of all")
    parser.add_argument("--list", action="store_true", help="list the case names and exit")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.list:
        for case in CASES:
            print(f"{case.name}: {case.note}")
        return 0
    chosen = [case for case in CASES if args.case in (None, case.name)]
    if not chosen:
        print(f"no case named {args.case}", file=sys.stderr)
        return 2
    for case in chosen:
        print(format_effects(case, run_case(args.xbe, case)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
