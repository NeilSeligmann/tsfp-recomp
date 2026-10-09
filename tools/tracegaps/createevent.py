# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Part (a): every caller of the CreateEventA and CreateMutexA wrappers, and what each asks the kernel for.

THE CLAIM BEING CLOSED. T8g found five callers of the CreateEvent wrapper `0x37FF30`; the
T142 note says exactly one, `0x291FD`. T142 swept `.text`. This module decodes EVERY
executable section and lists each direct caller with the four pushed arguments resolved
over every path (`tools.tracegaps.flow`), then runs the title's own wrapper on those
arguments and captures the `NtCreateEvent` call it makes. The ObjectAttributes, Type and
InitialState columns are what the wrapper really passes, not a reading of its source.

WRAPPER SHAPE (read from the image, asserted by the tests, printed here):
    CreateEventA(lpEventAttributes, bManualReset, bInitialState, lpName)   stdcall, 4 args
    NtCreateEvent(&handle, OA, Type, InitialState)
        OA = NULL when lpName is NULL, else built by `0x381B7C` from the name
        Type = (bManualReset == 0): 1 SynchronizationEvent, 0 NotificationEvent
The same census runs for the mutex wrapper `0x37FFB1`.

WHAT ELSE IS CHECKED, so absence is a measurement and not a hope: tail `jmp`s to the
wrapper, every 4-byte occurrence of the wrapper address in any section (a pointer table
would show), every code reference to the kernel thunk slot (only the wrapper may call
`NtCreateEvent`), and the thunk slot's ordinal read from the image.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from tools.tracegaps.code import Code
from tools.tracegaps.emulate import Runner, symbol
from tools.tracegaps.flow import FlowCache, ValueSet

#: The high bit marks a kernel import in an XBE thunk slot.
ORDINAL_FLAG = 0x80000000


@dataclass(frozen=True)
class WrapperSpec:
    """One XAPI wrapper over one `Nt*Create*` kernel call."""

    name: str
    entry: int
    #: The IAT slot the wrapper calls through, and the ordinal expected in it
    thunk_slot: int
    nt_name: str
    expected_ordinal: int
    #: The wrapper's own arguments, argument 1 first
    arguments: tuple[str, ...]
    #: The kernel call's arguments after the handle pointer
    kernel_arguments: tuple[str, ...]
    #: Which wrapper argument is the name, and which decides each kernel argument
    name_argument: int
    #: The helper that turns a name into an ObjectAttributes
    name_builder: int


EVENT = WrapperSpec(
    name="CreateEventA",
    entry=0x37FF30,
    thunk_slot=0x475898,
    nt_name="NtCreateEvent",
    expected_ordinal=189,
    arguments=("lpEventAttributes", "bManualReset", "bInitialState", "lpName"),
    kernel_arguments=("ObjectAttributes", "EventType", "InitialState"),
    name_argument=4,
    name_builder=0x381B7C,
)
MUTEX = WrapperSpec(
    name="CreateMutexA",
    entry=0x37FFB1,
    thunk_slot=0x4758A4,
    nt_name="NtCreateMutant",
    expected_ordinal=192,
    arguments=("lpMutexAttributes", "bInitialOwner", "lpName"),
    kernel_arguments=("ObjectAttributes", "InitialOwner"),
    name_argument=3,
    name_builder=0x381B7C,
)


@dataclass(frozen=True)
class Outcome:
    """What the wrapper passed to the kernel for one combination of argument values."""

    arguments: tuple[int, ...]
    #: "NULL", "built from the name", or "unknown"
    object_attributes: str
    #: kernel argument label -> value
    kernel: tuple[tuple[str, int], ...]


@dataclass
class Caller:
    call: int
    section: str
    entry: int
    #: value sets of the wrapper's arguments, argument 1 first
    arguments: list[ValueSet]
    outcomes: list[Outcome] = field(default_factory=list)
    #: addresses of direct calls to the enclosing function
    entry_callers: list[int] = field(default_factory=list)
    stack_conflicts: int = 0

    @property
    def return_address(self) -> int:
        return self.call + 5


@dataclass
class WrapperReport:
    spec: WrapperSpec
    thunk_ordinal: int | None
    callers: list[Caller]
    tail_jumps: list[int]
    #: occurrences of the wrapper address that are not inside a decoded instruction
    pointer_hits: list[tuple[str, int]]
    #: code references to the kernel thunk slot outside the wrapper
    thunk_refs_elsewhere: list[int]
    #: instructions that read the slot inside the wrapper (the kernel call sites)
    thunk_refs_inside: list[int]
    indirect_transfers: int


def trace_wrapper(
    code: Code, runner: Runner, spec: WrapperSpec, flows: FlowCache | None = None
) -> WrapperReport:
    flows = flows or FlowCache(code)
    wrapper = flows.at_entry(spec.entry)
    slot_refs = code.absolute_operand_refs(spec.thunk_slot)
    inside = [insn.address for insn in slot_refs if insn.address in wrapper.insns]
    outside = [insn.address for insn in slot_refs if insn.address not in wrapper.insns]
    builder_calls = [
        t.address for t in code.callers(spec.name_builder) if t.address in wrapper.insns
    ]
    callers: list[Caller] = []
    for transfer in code.callers(spec.entry):
        flow = flows.flow_for(transfer.address)
        arguments = flow.call_arguments(transfer.address, len(spec.arguments))
        caller = Caller(
            call=transfer.address,
            section=transfer.section,
            entry=flow.entry,
            arguments=arguments,
            entry_callers=[t.address for t in code.callers(flow.entry)],
            stack_conflicts=len(flow.delta_conflicts),
        )
        caller.outcomes = _outcomes(runner, spec, arguments, inside, builder_calls)
        callers.append(caller)
    callers.sort(key=lambda item: item.call)
    hits = code.dword_hits(spec.entry)
    return WrapperReport(
        spec=spec,
        thunk_ordinal=_thunk_ordinal(code, spec.thunk_slot),
        callers=callers,
        tail_jumps=[t.address for t in code.tail_jumps(spec.entry)],
        pointer_hits=[(name, address) for name, address, in_code in hits if not in_code],
        thunk_refs_elsewhere=outside,
        thunk_refs_inside=inside,
        indirect_transfers=code.indirect_transfer_count(),
    )


def _thunk_ordinal(code: Code, slot: int) -> int | None:
    value = code.image.u32(slot)
    if value is None or not value & ORDINAL_FLAG:
        return None
    return value & ~ORDINAL_FLAG


def _domain(values: ValueSet, number: int) -> list[int]:
    """The concrete values to run for one argument: its set, plus zero and a symbol when open.

    Zero and "anything else" are the two cases every wrapper test distinguishes (NULL or
    not, FALSE or TRUE), so an open argument is run as both.
    """
    domain = set(values.values)
    if values.unbounded or not domain:
        domain |= {0, symbol(number)}
    return sorted(domain)


def _outcomes(
    runner: Runner,
    spec: WrapperSpec,
    arguments: list[ValueSet],
    kernel_calls: list[int],
    builder_calls: list[int],
) -> list[Outcome]:
    """Run the wrapper on every combination of argument values and capture the kernel call."""
    combinations: list[tuple[int, ...]] = [()]
    for number, values in enumerate(arguments, start=1):
        domain = _domain(values, number)
        if number == spec.name_argument:
            # Only NULL or not matters: the wrapper tests the name pointer against zero.
            domain = sorted({0 if value == 0 else symbol(number) for value in domain})
        combinations = [(*prefix, value) for prefix in combinations for value in domain]
    found: list[Outcome] = []
    for combination in combinations:
        capture = runner.run(spec.entry, combination, [*kernel_calls, *builder_calls], words=6)
        if capture is None:
            found.append(Outcome(combination, "unknown", ()))
            continue
        if capture.stop in builder_calls:
            found.append(Outcome(combination, "built from the name", ()))
            continue
        oa = capture.words[1]
        kernel = tuple(
            (label, capture.words[2 + position])
            for position, label in enumerate(spec.kernel_arguments[1:])
        )
        found.append(Outcome(combination, "NULL" if oa == 0 else "non-NULL", kernel))
    return found


# --------------------------------------------------------------------------- rendering


def _shown(value: int) -> str:
    return f"open#{value - symbol(0)}" if symbol(0) < value < symbol(0) + 16 else f"{value:#x}"


def render(report: WrapperReport) -> str:
    spec = report.spec
    lines = [
        f"{spec.name} wrapper {spec.entry:#x}: {len(report.callers)} direct caller(s), all executable sections",
        f"  kernel call {spec.nt_name} through thunk slot {spec.thunk_slot:#x}, "
        f"ordinal in image {report.thunk_ordinal} (expected {spec.expected_ordinal})",
        f"  wrapper arguments, argument 1 first: ({', '.join(spec.arguments)})",
    ]
    for caller in report.callers:
        pushed = ", ".join(value.describe() for value in caller.arguments)
        lines.append(
            f"  call {caller.call:#x} [{caller.section}] return {caller.return_address:#x} "
            f"in function {caller.entry:#x} (called from "
            f"{', '.join(f'{a:#x}' for a in caller.entry_callers) or 'no direct caller'})"
        )
        lines.append(f"      pushed ({pushed})")
        for outcome in caller.outcomes:
            kernel = ", ".join(f"{label}={_shown(value)}" for label, value in outcome.kernel)
            lines.append(
                f"      -> {spec.nt_name}: ObjectAttributes {outcome.object_attributes}"
                + (f", {kernel}" if kernel else "")
            )
    lines.append(f"  tail jmp to the wrapper: {_list(report.tail_jumps)}")
    lines.append(
        f"  occurrences of the wrapper address outside decoded code: {len(report.pointer_hits)}"
    )
    lines.append(
        f"  code references to {spec.thunk_slot:#x}: inside the wrapper {_list(report.thunk_refs_inside)}, "
        f"elsewhere {_list(report.thunk_refs_elsewhere)}"
    )
    lines.append(
        f"  indirect call/jmp instructions in the image (denominator): {report.indirect_transfers}"
    )
    lines.extend(f"  {line}" for line in verdict(report))
    return "\n".join(lines)


def _list(addresses: list[int]) -> str:
    return ", ".join(f"{a:#x}" for a in addresses) if addresses else "none"


def value_json(value: ValueSet) -> dict[str, object]:
    return {"values": sorted(value.values), "unbounded": list(value.unbounded)}


def to_json(report: WrapperReport) -> dict[str, object]:
    return {
        "wrapper": report.spec.entry,
        "name": report.spec.name,
        "thunk_ordinal": report.thunk_ordinal,
        "callers": [
            {
                "call": caller.call,
                "section": caller.section,
                "entry": caller.entry,
                "entry_callers": caller.entry_callers,
                "arguments": [value_json(value) for value in caller.arguments],
                "outcomes": [
                    {
                        "arguments": list(outcome.arguments),
                        "object_attributes": outcome.object_attributes,
                        "kernel": dict(outcome.kernel),
                    }
                    for outcome in caller.outcomes
                ],
            }
            for caller in report.callers
        ],
        "tail_jumps": report.tail_jumps,
        "pointer_hits": [list(hit) for hit in report.pointer_hits],
        "thunk_refs_elsewhere": report.thunk_refs_elsewhere,
        "thunk_refs_inside": report.thunk_refs_inside,
        "indirect_transfers": report.indirect_transfers,
    }


def verdict(report: WrapperReport) -> list[str]:
    spec = report.spec
    table = type_table(report)
    lines = [
        f"MEASURED {spec.name}: {len(report.callers)} direct caller(s) across all executable sections "
        f"({', '.join(sorted({c.section for c in report.callers}))})",
        f"MEASURED every caller's {spec.nt_name} ObjectAttributes: "
        + ", ".join(sorted({o.object_attributes for c in report.callers for o in c.outcomes})),
    ]
    for label, values in table.items():
        lines.append(
            f"MEASURED {spec.nt_name} {label} values over all callers: {[hex(v) for v in values]}"
        )
    lines.append(
        f"MEASURED the kernel thunk slot {spec.thunk_slot:#x} holds ordinal {report.thunk_ordinal} and is referenced "
        f"outside the wrapper by {len(report.thunk_refs_elsewhere)} instruction(s)"
    )
    lines.append(
        f"NOT DERIVABLE whether any caller runs in a boot (functions with no direct caller: "
        f"{', '.join(f'{c.entry:#x}' for c in report.callers if not c.entry_callers) or 'none'}), "
        f"or a caller reached through one of the {report.indirect_transfers} indirect call/jmp instructions"
    )
    return lines


def type_table(report: WrapperReport) -> dict[str, list[int]]:
    """kernel argument label -> every distinct concrete value any caller can produce."""
    table: dict[str, set[int]] = {}
    for caller in report.callers:
        for outcome in caller.outcomes:
            for label, value in outcome.kernel:
                table.setdefault(label, set()).add(value)
    return {label: sorted(values) for label, values in table.items()}
