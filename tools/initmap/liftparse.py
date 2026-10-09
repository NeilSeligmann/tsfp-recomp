# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the lifted C into per-function control-flow graphs of call events.

The lifter writes one `void sub_XXXXXXXX(void)` per guest function. Inside it a
guest basic block is a `loc_XXXXXXXX: ;` label, a call is one of a few macro forms,
and a branch is a `goto`. That is regular enough to rebuild a graph from text without
touching the user's executable, which is the point: the graph is the lifter's own
reading, so a mistake here is a mistake about the LIFT, not about the binary.

The call forms, as measured on the shipped tree:

    PUSH32(esp, RET); RECOMP_ABI_CALL(0xTARGET, sub_TARGET);      direct call
    PUSH32(esp, RET); RECOMP_ICALL_SAFE(0xTARGET, _icall_esp);    call to a suppressed XDK body
    { uint32_t _icall_target = EXPR; ... RECOMP_ICALL_SAFE_AT(_icall_target, ...); }
                                                                  call through a pointer
    g_seh_ebp = ebp; sub_TARGET(); return;                        direct tail jump
    g_seh_ebp = ebp; RECOMP_ITAIL(0xTARGET); return;              tail jump to an XDK body
    g_seh_ebp = ebp; RECOMP_ITAIL(EXPR); return;                  tail jump through a pointer

A switch is `{ uint32_t _jt = ...; if (_jt == X) goto L; ... RECOMP_ITAIL(_jt); }`, whose
last statement is the lifter's own "no case matched" arm and is not a call.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field, replace
from pathlib import Path

#: Event kinds. `direct` and `tail` go to a lifted body, `manual` and `manual_tail` to a
#: suppressed XDK body, `indirect` and `indirect_tail` through a computed address.
KIND_DIRECT = "direct"
KIND_TAIL = "tail"
KIND_MANUAL = "manual"
KIND_MANUAL_TAIL = "manual_tail"
KIND_INDIRECT = "indirect"
KIND_INDIRECT_TAIL = "indirect_tail"

#: Kinds that end the function after the transfer.
TAIL_KINDS = frozenset({KIND_TAIL, KIND_MANUAL_TAIL, KIND_INDIRECT_TAIL})

#: The doc comment names the function and gives its first guest address. The name is
#: usually `sub_<address>` but the entry point is `xbe_entry_point`, so the address is read
#: from the comment and not from the name.
_HEADER = re.compile(
    r"^ \* (\w+)\n \* Original: 0x([0-9A-F]{8}) - 0x([0-9A-F]{8})(?: \((\d+) bytes, (\d+) insns\))?"
    r"[^\n]*\n(?: \*[^\n]*\n)*? \*/\n"
    r"void \1\(void\)\n\{\n",
    re.MULTILINE,
)
_LABEL = re.compile(r"^loc_([0-9A-F]{8}): ;")
_GOTO = re.compile(r"goto loc_([0-9A-F]{8});")
_PUSH_RET = re.compile(r"PUSH32\(esp, 0x([0-9A-F]+)u?\); RECOMP_ABI_CALL")
_ABI_CALL = re.compile(r"RECOMP_ABI_CALL\(0x([0-9A-F]+)u, sub_([0-9A-F]+)\)")
_MANUAL_CALL = re.compile(r"RECOMP_ICALL_SAFE\(0x([0-9A-F]+)u, _icall_esp\)")
_MANUAL_RET = re.compile(r"PUSH32\(esp, 0x([0-9A-F]+)u?\); RECOMP_ICALL_SAFE\(")
_INDIRECT = re.compile(
    r"\{ uint32_t _icall_target = (.*?); PUSH32\(esp, 0x([0-9A-F]+)u?\); "
    r"RECOMP_ICALL_SAFE_AT\(_icall_target, _icall_esp, 0x([0-9A-F]+)u?\); \}"
)
_MANUAL_TAIL = re.compile(r"RECOMP_ITAIL\(0x([0-9A-F]+)u\)")
_INDIRECT_TAIL = re.compile(r"RECOMP_ITAIL\((MEM32\(.*?\)|e[a-ds][xip]|e[sd]i|ebp)\); return;")
_DIRECT_TAIL = re.compile(r"g_seh_ebp = ebp; sub_([0-9A-F]{8})\(\); return;")
_IF_ARM = re.compile(r"^if \(.*?\) \{ (.*)\}")
_IF_GOTO = re.compile(r"^if \(.*\) goto loc_([0-9A-F]{8});")
_JT_OPEN = re.compile(r"^\{ uint32_t _jt =")
_SLOT_CONSTANT = re.compile(r"^MEM32\(0x([0-9A-Fa-f]+)\)$")
_VTABLE = re.compile(r"^MEM32\((e[a-ds][xip]|e[sd]i|ebp)(?: \+ (0x[0-9A-Fa-f]+|\d+))?\)$")


@dataclass(frozen=True)
class CallEvent:
    """One transfer of control out of a function, in text order within its block."""

    kind: str
    #: Guest VA for direct, tail, manual and manual_tail. None when computed.
    target: int | None = None
    #: The pointer expression for indirect kinds, as the lifter wrote it.
    expr: str | None = None
    #: Guest VA of the call instruction's return address, when the line carried one.
    return_va: int | None = None
    #: For a call through a register, the constant slot that register was last loaded from.
    via_slot: int | None = None
    #: The last few data-section constants seen before an indirect call, in text order. A
    #: table walk (`call [esi]` over a range) leaves the table's bounds here.
    nearby: tuple[int, ...] = ()

    @property
    def slot(self) -> int | None:
        """The constant memory slot an indirect call reads its target from, if any.

        Either written in the call itself, or inherited from the load that filled the
        register it calls through (`mov esi, [import]` ... `call esi`).
        """
        if self.expr is None:
            return None
        found = _SLOT_CONSTANT.match(self.expr)
        if found:
            return int(found.group(1), 16)
        return self.via_slot

    @property
    def is_register_pointer(self) -> bool:
        """True for a call through a register or a register-relative slot (a vtable)."""
        return self.expr is not None and (
            self.slot is None and (_VTABLE.match(self.expr) is not None or "MEM32" not in self.expr)
        )


@dataclass
class Node:
    """A run of statements with one entry. Edges are node ids, fall-through first."""

    ident: int
    events: list[CallEvent] = field(default_factory=list)
    successors: list[int] = field(default_factory=list)
    #: True when control leaves the function here (return, tail jump, noreturn call).
    is_exit: bool = False
    #: The guest address of the label that opened this node, when it had one.
    label: int | None = None
    #: Code-address immediates seen in this node, for the address-taken analysis.
    immediates: list[int] = field(default_factory=list)
    #: Kernel thunk-table slots touched other than by a call, i.e. data imports.
    slot_reads: list[int] = field(default_factory=list)
    #: Constants inside a memory-mapped hardware window.
    mmio: list[int] = field(default_factory=list)
    #: Instructions the lift could not translate or that read a clock: `unimpl:<mnemonic>`,
    #: `rdtsc`, `cpuid`.
    hazards: list[str] = field(default_factory=list)


@dataclass
class Function:
    entry: int
    nodes: list[Node]
    #: Lines the parser could not classify but that mention control flow.
    unparsed: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class FunctionIndex:
    """Where each function's text lives, without holding every body parsed."""

    texts: dict[str, str]
    #: entry VA -> (chunk name, start offset of the body, end offset)
    spans: dict[int, tuple[str, int, int]]
    #: entry VA -> (end VA, instruction count the lifter recorded, or None)
    extents: dict[int, tuple[int, int | None]] = field(default_factory=dict)

    def has_body(self, entry: int) -> bool:
        return entry in self.spans

    def body(self, entry: int) -> str:
        chunk, start, end = self.spans[entry]
        return self.texts[chunk][start:end]


def index_directory(directory: Path) -> FunctionIndex:
    """Index every `recomp_NNNN.c` chunk in `directory` by function entry."""
    texts: dict[str, str] = {}
    spans: dict[int, tuple[str, int, int]] = {}
    extents: dict[int, tuple[int, int | None]] = {}
    for path in sorted(directory.glob("recomp_[0-9][0-9][0-9][0-9].c")):
        texts[path.name] = path.read_text(encoding="utf-8", errors="replace")
    for name, text in texts.items():
        for match in _HEADER.finditer(text):
            end = text.find("\n}\n", match.end())
            if end < 0:
                raise ValueError(f"unterminated function {match.group(1)} in {name}")
            entry = int(match.group(2), 16)
            if entry in spans:
                raise ValueError(f"function at {entry:#010x} defined twice")
            spans[entry] = (name, match.end(), end)
            extents[entry] = (
                int(match.group(3), 16),
                int(match.group(5)) if match.group(5) else None,
            )
    return FunctionIndex(texts=texts, spans=spans, extents=extents)


def _events_in(statement: str) -> list[CallEvent]:
    """Every call or tail transfer on one source line, in left-to-right order."""
    found: list[tuple[int, CallEvent]] = []
    for match in _ABI_CALL.finditer(statement):
        ret = _PUSH_RET.search(statement)
        found.append(
            (
                match.start(),
                CallEvent(
                    KIND_DIRECT,
                    target=int(match.group(1), 16),
                    return_va=int(ret.group(1), 16) if ret else None,
                ),
            )
        )
    for match in _MANUAL_CALL.finditer(statement):
        ret = _MANUAL_RET.search(statement)
        found.append(
            (
                match.start(),
                CallEvent(
                    KIND_MANUAL,
                    target=int(match.group(1), 16),
                    return_va=int(ret.group(1), 16) if ret else None,
                ),
            )
        )
    for match in _INDIRECT.finditer(statement):
        found.append(
            (
                match.start(),
                CallEvent(KIND_INDIRECT, expr=match.group(1), return_va=int(match.group(2), 16)),
            )
        )
    for match in _MANUAL_TAIL.finditer(statement):
        found.append((match.start(), CallEvent(KIND_MANUAL_TAIL, target=int(match.group(1), 16))))
    for match in _INDIRECT_TAIL.finditer(statement):
        found.append((match.start(), CallEvent(KIND_INDIRECT_TAIL, expr=match.group(1))))
    for match in _DIRECT_TAIL.finditer(statement):
        found.append((match.start(), CallEvent(KIND_TAIL, target=int(match.group(1), 16))))
    found.sort(key=lambda item: item[0])
    return [event for _, event in found]


_COMMENT = re.compile(r"/\*.*?\*/")
_IMMEDIATE = re.compile(r"\b0x([0-9A-Fa-f]{5,8})u?\b")
_SLOT_LOAD = re.compile(r"^(e[a-ds][xip]|e[sd]i|ebp) = MEM32\(0x([0-9A-Fa-f]+)\);")
_REGISTER_WRITE = re.compile(r"^(e[a-ds][xip]|e[sd]i|ebp) (?:=|\+=|-=|\^=|&=|\|=) ")
_REGISTER_POP = re.compile(r"^POP32\(esp, (e[a-ds][xip]|e[sd]i|ebp)\);")
_INDEXED = re.compile(r"\* [1248] \+ $")
_UNIMPL = re.compile(r'RECOMP_UNIMPL\("(\w+)')
_DEAD_JUMP = re.compile(r"\(void\)0; /\* goto loc_[0-9A-F]{8} - dead code")


@dataclass(frozen=True)
class Probe:
    """Which constants on a call-free line are worth remembering.

    - `code_addresses`: function entries. One that appears as a bare immediate is an
      address taken, which is how a callback or a thread start routine is registered
      without a call edge to it.
    - `thunk_slots`: the kernel import thunk table. A constant in it that is not a call
      target is a read of a kernel DATA export.
    - `mmio_windows`: hardware register windows. Register bases that arrive from a
      kernel call are invisible to this and are reported as such by the caller.
    """

    code_addresses: frozenset[int] = frozenset()
    thunk_slots: range = range(0)
    #: Thunk slots whose export is DATA. Loading one is a read of that data. Loading any
    #: other is the start of a register-held call to a kernel function.
    data_slots: frozenset[int] = frozenset()
    mmio_windows: tuple[range, ...] = ()
    #: Initialised data, where a table of routines could live.
    data_windows: tuple[range, ...] = ()


NO_PROBE = Probe()


def parse_function(entry: int, body: str, probe: Probe = NO_PROBE) -> Function:
    """Rebuild a function's graph from its lifted text.

    Conditional exits written as `if (c) { ...; return; }` become an arm node so the
    call inside is not mistaken for an unconditional one. A `goto` to a label this
    function does not define is kept as an unparsed line, not guessed at.
    """
    nodes: list[Node] = [Node(0)]
    labels: dict[int, int] = {}
    pending: list[tuple[int, int]] = []
    unparsed: list[str] = []
    current = nodes[0]
    #: register -> thunk slot it was last loaded from, in text order.
    held: dict[str, int] = {}
    recent: list[int] = []

    def start_node(label: int | None = None) -> Node:
        node = Node(len(nodes), label=label)
        nodes.append(node)
        return node

    def close_into(node: Node) -> None:
        if not current.is_exit and node.ident not in current.successors:
            current.successors.append(node.ident)

    for raw in body.split("\n"):
        line = raw.strip()
        if not line or line.startswith(("/*", "*", "//")):
            continue
        label = _LABEL.match(line)
        if label:
            target = int(label.group(1), 16)
            if not current.events and not current.successors and not current.is_exit:
                if current.label is None and current.ident == len(nodes) - 1:
                    current.label = target
                    labels[target] = current.ident
                    continue
            node = start_node(target)
            close_into(node)
            labels[target] = node.ident
            current = node
            continue
        if _JT_OPEN.match(line):
            continue
        arm = _IF_ARM.match(line)
        if arm:
            inner = arm.group(1)
            arm_events = _events_in(inner)
            goes = _GOTO.search(inner)
            leaves = "return;" in inner
            if arm_events or leaves or goes:
                arm_node = start_node()
                current.successors.append(arm_node.ident)
                arm_node.events.extend(arm_events)
                if goes:
                    pending.append((arm_node.ident, int(goes.group(1), 16)))
                elif leaves:
                    arm_node.is_exit = True
                after = start_node()
                current.successors.append(after.ident)
                if not goes and not leaves:
                    arm_node.successors.append(after.ident)
                current = after
                continue
        conditional = _IF_GOTO.match(line)
        if conditional:
            after = start_node()
            current.successors.append(after.ident)
            pending.append((current.ident, int(conditional.group(1), 16)))
            current = after
            continue
        line_events = [
            replace(event, via_slot=held.get(event.expr or ""), nearby=tuple(recent[-4:]))
            if event.kind in (KIND_INDIRECT, KIND_INDIRECT_TAIL)
            else event
            for event in _events_in(line)
        ]
        current.events.extend(line_events)
        load = _SLOT_LOAD.match(line)
        written = _REGISTER_WRITE.match(line) or _REGISTER_POP.match(line)
        if load:
            slot = int(load.group(2), 16)
            if slot not in probe.data_slots:
                held[load.group(1)] = slot
                written = None
        if written:
            held.pop(written.group(1), None)
        if line_events:
            for clobbered in ("eax", "ecx", "edx"):
                held.pop(clobbered, None)
        unimplemented = _UNIMPL.search(line)
        if unimplemented:
            current.hazards.append(f"unimpl:{unimplemented.group(1)}")
        if "/* rdtsc */" in line:
            current.hazards.append("rdtsc")
        if not line_events:
            bare = _COMMENT.sub("", line)
            for hit in _IMMEDIATE.finditer(bare):
                value = int(hit.group(1), 16)
                if value in probe.thunk_slots and _INDEXED.search(bare[: hit.start()]):
                    # `[reg * 4 + slot]` reads a run of imports by index, not one export.
                    current.hazards.append("indexed-import-table")
                    continue
                # A page-aligned value is far more often a size or a mask than a routine.
                if value in probe.code_addresses and value & 0xFFF:
                    current.immediates.append(value)
                if value in probe.thunk_slots and (
                    value in probe.data_slots or not (load and int(load.group(2), 16) == value)
                ):
                    current.slot_reads.append(value)
                if any(value in window for window in probe.mmio_windows):
                    current.mmio.append(value)
                if (
                    any(value in window for window in probe.data_windows)
                    and value not in probe.thunk_slots
                    and (not recent or recent[-1] != value)
                ):
                    recent.append(value)
        if line.startswith("goto "):
            found = _GOTO.search(line)
            if found:
                pending.append((current.ident, int(found.group(1), 16)))
                # The block after an unconditional jump has no edge into it unless a label
                # opens it.
                current = start_node()
                continue
        if "return;" in line:
            current.is_exit = True
            after = start_node()
            current = after
            continue
        if "goto" in line and not _DEAD_JUMP.search(line):
            unparsed.append(line)

    for source, label_va in pending:
        destination = labels.get(label_va)
        if destination is None:
            unparsed.append(f"goto to undefined label loc_{label_va:08X}")
            continue
        if destination not in nodes[source].successors:
            nodes[source].successors.append(destination)
    # Drop the empty trailing nodes the splitting leaves behind, keeping ids stable
    # by leaving them in place: they have no events and no way in.
    return Function(entry=entry, nodes=nodes, unparsed=unparsed)


_BARE_PUSH = re.compile(r"^PUSH32\(esp, [^;]*\);$")


def call_argument_words(body: str) -> list[tuple[int, int]]:
    """For a straight-line function, each direct call and the stack words pushed before it.

    Words are counted since the previous call, so a register save or a local counts as an
    argument. That is acceptable for the one use this has, a flat list of initialisers where
    nothing else is pushed, and wrong for general code. The call's own return-address push
    shares its line with the call and is not counted.
    """
    result: list[tuple[int, int]] = []
    pushes = 0
    for raw in body.split("\n"):
        line = raw.strip()
        if _BARE_PUSH.match(line):
            pushes += 1
            continue
        call = _ABI_CALL.search(line)
        if call:
            result.append((int(call.group(1), 16), pushes))
            pushes = 0
    return result
