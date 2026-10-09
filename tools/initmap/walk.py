# SPDX-License-Identifier: GPL-3.0-or-later
"""Depth-first walk of the lifted call graph, in an order that approximates execution.

WHAT THE ORDER MEANS. A function is expanded the first time a call to it is met, at that
call. Its blocks are visited in reverse postorder with the fall-through arm of each branch
first. That is a reading of the control flow and not a trace: a branch can go either way, a
loop can run zero or many times, and a function reached again is not expanded again. Each
hit therefore carries a certainty, and the order is only as good as the "always" ones.

WHAT IS NOT SEEN. A call through a register or a vtable has no static target and is
recorded as unresolved. A call through a constant memory slot is resolved from the image's
initial bytes when that slot holds the address of a lifted function, which is an INFERENCE
because the slot may be rewritten before the call runs. A routine registered by address
(a thread start, a callback) has no call edge. The `follow_address_taken` pass walks those
as a second tier, tagged so a reader can drop them.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass, field

from tools.initmap.cfg import Shape, analyse
from tools.initmap.image import Image, SurfaceRow
from tools.initmap.liftparse import (
    KIND_DIRECT,
    KIND_INDIRECT,
    KIND_INDIRECT_TAIL,
    KIND_MANUAL,
    KIND_MANUAL_TAIL,
    KIND_TAIL,
    CallEvent,
    Function,
    FunctionIndex,
    Node,
    Probe,
    parse_function,
)

#: Sections that hold initialised data. The XBE flags cannot say: this image marks even
#: `.rdata` and `.data` executable.
DATA_SECTION_NAMES = (".rdata", ".data", ".data1")
#: The longest table a looped register call may be resolved against.
MAX_TABLE_ENTRIES = 512
_BARE_REGISTER = re.compile(r"^e[a-ds][xip]$|^e[sd]i$|^ebp$")

#: Hit kinds.
HIT_XDK = "xdk"
HIT_KERNEL = "kernel"
HIT_UNRESOLVED = "unresolved_indirect"
HIT_NO_BODY = "no_body"
HIT_DATA_IMPORT = "data_import"
HIT_MMIO = "mmio"
HIT_ADDRESS_TAKEN = "address_taken"
HIT_HAZARD = "hazard"

CERTAIN = "always"
CONDITIONAL = "conditional"
LOOPED = "loop"


@dataclass(frozen=True)
class Frame:
    """One expansion of a function. A function has at most one frame in a given tier."""

    ident: int
    function: int
    parent: int
    #: Return address of the call that opened this frame, when the lifter recorded one.
    via_return: int | None
    depth: int
    #: Cumulative over the chain from the root: some site on the way is conditional or looped.
    conditional: bool
    looped: bool
    #: 1 for the direct-call walk, 2 for routines found only by address.
    tier: int
    #: How the frame was reached: a direct call, a resolved slot, or a registered address.
    via_kind: str
    #: Tick at which the frame opened.
    tick: int
    #: For a tier-2 frame, the tick at which its address was registered. Equal to `tick`
    #: otherwise. Tier-2 frames are expanded after the whole tier-1 walk, so only this tells
    #: whether the registration happened before the first frame.
    registered: int = 0


@dataclass(frozen=True)
class Hit:
    """One thing the walk met at a site: an XDK call, a kernel call, an unknown target."""

    tick: int
    frame: int
    function: int
    kind: str
    #: Guest address for xdk, no_body, mmio, address_taken. Ordinal for kernel. Slot for
    #: data_import. None for unresolved.
    value: int | None
    return_va: int | None
    #: Cumulative flags (chain plus this site).
    conditional: bool
    looped: bool
    #: This site alone, for the "always" fixpoint.
    local_conditional: bool
    local_looped: bool
    tail: bool = False
    #: The pointer expression of an unresolved call, or the tag of a hazard.
    expr: str | None = None
    tier: int = 1


@dataclass
class Walk:
    """Result of one walk."""

    roots: tuple[int, ...]
    frames: list[Frame] = field(default_factory=list)
    hits: list[Hit] = field(default_factory=list)
    #: function -> [(callee, local_conditional, local_looped)] for the always fixpoint.
    edges: dict[int, list[tuple[int, bool, bool]]] = field(default_factory=dict)
    #: First tick at which each requested marker function was expanded.
    marker_ticks: dict[int, int] = field(default_factory=dict)
    #: Calls to a function already expanded, per target.
    repeats: dict[int, int] = field(default_factory=dict)
    shapes_without_exit: set[int] = field(default_factory=set)
    unparsed: dict[int, list[str]] = field(default_factory=dict)
    ticks: int = 0

    def always_functions(self) -> frozenset[int]:
        """Functions reached from the root through unconditional, non-looped sites only."""
        reached = set(self.roots)
        pending = list(self.roots)
        while pending:
            current = pending.pop()
            for callee, conditional, looped in self.edges.get(current, ()):
                if not conditional and not looped and callee not in reached:
                    reached.add(callee)
                    pending.append(callee)
        return frozenset(reached)

    def certainty(self, hit: Hit, always: frozenset[int]) -> str:
        """`always` only if the site is unconditional in a function that always runs."""
        if hit.function in always and not hit.local_conditional and not hit.local_looped:
            return CERTAIN
        if hit.looped and not hit.conditional:
            return LOOPED
        return CONDITIONAL


@dataclass
class _Cursor:
    frame: Frame
    function: Function
    shape: Shape
    node_position: int = 0
    event_position: int = -1
    #: Callees still to open for the event just handled: (target, kind, return, cond, loop).
    queue: list[tuple[int, str, int | None, bool, bool]] = field(default_factory=list)


class Walker:
    """Holds the parse cache and the image so several walks share the expensive part."""

    def __init__(
        self,
        index: FunctionIndex,
        image: Image,
        surface: dict[int, SurfaceRow],
        data_ordinals: frozenset[int] = frozenset(),
    ) -> None:
        self.index = index
        self.image = image
        self.surface = surface
        self.probe = Probe(
            code_addresses=frozenset(index.spans),
            thunk_slots=image.thunk_slots,
            data_slots=frozenset(
                slot for slot in image.thunk_slots if image.slot_ordinal(slot) in data_ordinals
            ),
            data_windows=tuple(
                range(section.virtual_addr, section.virtual_addr + section.virtual_size)
                for section in image.xbe.sections
                if section.name in DATA_SECTION_NAMES
            ),
            mmio_windows=(
                range(0xFD000000, 0xFDFFFFF0),
                range(0xFE800000, 0xFEA00000),
                range(0xFEC00000, 0xFEC10000),
                range(0xFED00000, 0xFED10000),
                range(0xFEF00000, 0xFEF10000),
            ),
        )
        self._functions: dict[int, Function] = {}
        self._shapes: dict[int, Shape] = {}
        self._overrides: dict[int, int] = {}
        self._overridden: set[int] = set()

    def function(self, entry: int) -> Function:
        cached = self._functions.get(entry)
        if cached is None:
            cached = parse_function(entry, self.index.body(entry), self.probe)
            self._functions[entry] = cached
        return cached

    def shape(self, entry: int) -> Shape:
        cached = self._shapes.get(entry)
        if cached is None:
            cached = analyse(self.function(entry))
            self._shapes[entry] = cached
        return cached

    def resolve_slot(self, slot: int) -> int | None:
        """Initial value of a constant slot if it is a lifted function. INFERRED, not proven."""
        value = self.image.read_u32(slot)
        if value is not None and self.index.has_body(value):
            return value
        return None

    def run(
        self,
        roots: tuple[int, ...],
        markers: frozenset[int] = frozenset(),
        follow_address_taken: bool = False,
        resolve_indirect: dict[int, int] | None = None,
    ) -> Walk:
        """Walk each root in order, sharing one expanded set, then the address-taken tier.

        `resolve_indirect` maps a function to the target of its first unresolved register
        call. It exists for the one case the lift cannot see through, a thread bootstrap that
        calls the start routine it was handed, and every use is an INFERENCE the caller owns.
        """
        walk = Walk(roots=roots)
        self._overrides = dict(resolve_indirect or {})
        self._overridden = set()
        expanded: set[int] = set()
        taken: list[tuple[int, int, int]] = []  # (function, registering frame, registering tick)
        for root in roots:
            if root in expanded:
                continue
            self._descend(walk, expanded, markers, root, -1, None, 1, "root", taken)
        if follow_address_taken:
            queue_position = 0
            while queue_position < len(taken):
                target, parent, registered = taken[queue_position]
                queue_position += 1
                if target in expanded or not self.index.has_body(target):
                    continue
                self._descend(
                    walk,
                    expanded,
                    markers,
                    target,
                    parent,
                    None,
                    2,
                    HIT_ADDRESS_TAKEN,
                    taken,
                    registered=registered,
                )
        return walk

    def _descend(
        self,
        walk: Walk,
        expanded: set[int],
        markers: frozenset[int],
        entry: int,
        parent: int,
        via_return: int | None,
        tier: int,
        via_kind: str,
        taken: list[tuple[int, int, int]],
        site_conditional: bool = False,
        site_looped: bool = False,
        registered: int = 0,
    ) -> None:
        stack: list[_Cursor] = []

        def open_frame(
            target: int,
            parent_id: int,
            ret: int | None,
            kind: str,
            conditional: bool,
            looped: bool,
        ) -> None:
            expanded.add(target)
            walk.ticks += 1
            parent_frame = walk.frames[parent_id] if parent_id >= 0 else None
            frame = Frame(
                ident=len(walk.frames),
                function=target,
                parent=parent_id,
                via_return=ret,
                depth=(parent_frame.depth + 1) if parent_frame else 0,
                conditional=conditional or (parent_frame.conditional if parent_frame else False),
                looped=looped or (parent_frame.looped if parent_frame else False),
                tier=tier,
                via_kind=kind,
                tick=walk.ticks,
                registered=registered if tier == 2 else walk.ticks,
            )
            walk.frames.append(frame)
            if target in markers and target not in walk.marker_ticks:
                walk.marker_ticks[target] = walk.ticks
            function = self.function(target)
            if function.unparsed:
                walk.unparsed[target] = function.unparsed
            shape = self.shape(target)
            if not shape.has_real_exit:
                walk.shapes_without_exit.add(target)
            stack.append(_Cursor(frame=frame, function=function, shape=shape))

        open_frame(entry, parent, via_return, via_kind, site_conditional, site_looped)

        while stack:
            cursor = stack[-1]
            if cursor.queue:
                target, kind, ret, conditional, looped = cursor.queue.pop(0)
                if target not in expanded:
                    open_frame(target, cursor.frame.ident, ret, kind, conditional, looped)
                continue
            order = cursor.shape.order
            if cursor.node_position >= len(order):
                stack.pop()
                continue
            node_id = order[cursor.node_position]
            node = cursor.function.nodes[node_id]
            frame = cursor.frame
            local_conditional = node_id not in cursor.shape.unconditional
            local_looped = node_id in cursor.shape.cyclic

            if cursor.event_position < 0:
                cursor.event_position = 0
                self._note_node(walk, frame, node, local_conditional, local_looped, taken)
            if cursor.event_position >= len(node.events):
                cursor.node_position += 1
                cursor.event_position = -1
                continue
            event = node.events[cursor.event_position]
            cursor.event_position += 1
            for target, kind in self._handle_event(
                walk, frame, event, local_conditional, local_looped, expanded
            ):
                cursor.queue.append(
                    (target, kind, event.return_va, local_conditional, local_looped)
                )

    def _note_node(
        self,
        walk: Walk,
        frame: Frame,
        node: Node,
        local_conditional: bool,
        local_looped: bool,
        taken: list[tuple[int, int, int]],
    ) -> None:
        for slot in node.slot_reads:
            self._add_hit(walk, frame, HIT_DATA_IMPORT, slot, None, local_conditional, local_looped)
        for address in node.mmio:
            self._add_hit(walk, frame, HIT_MMIO, address, None, local_conditional, local_looped)
        for address in node.immediates:
            if address == frame.function:
                continue
            walk.ticks += 1
            self._add_hit(
                walk, frame, HIT_ADDRESS_TAKEN, address, None, local_conditional, local_looped
            )
            taken.append((address, frame.ident, walk.ticks))
        for tag in node.hazards:
            self._add_hit(
                walk, frame, HIT_HAZARD, None, None, local_conditional, local_looped, expr=tag
            )

    def _add_hit(
        self,
        walk: Walk,
        frame: Frame,
        kind: str,
        value: int | None,
        return_va: int | None,
        local_conditional: bool,
        local_looped: bool,
        tail: bool = False,
        expr: str | None = None,
    ) -> None:
        walk.ticks += 1
        walk.hits.append(
            Hit(
                tick=walk.ticks,
                frame=frame.ident,
                function=frame.function,
                kind=kind,
                value=value,
                return_va=return_va,
                conditional=frame.conditional or local_conditional,
                looped=frame.looped or local_looped,
                local_conditional=local_conditional,
                local_looped=local_looped,
                tail=tail,
                expr=expr,
                tier=frame.tier,
            )
        )

    def _handle_event(
        self,
        walk: Walk,
        frame: Frame,
        event: CallEvent,
        local_conditional: bool,
        local_looped: bool,
        expanded: set[int],
    ) -> list[tuple[int, str]]:
        """Record the event and return the callees to expand, in order."""
        tail = event.kind in (KIND_TAIL, KIND_MANUAL_TAIL, KIND_INDIRECT_TAIL)
        edge = walk.edges.setdefault(frame.function, [])

        def hit(kind: str, value: int | None, expr: str | None = None) -> None:
            self._add_hit(
                walk,
                frame,
                kind,
                value,
                event.return_va,
                local_conditional,
                local_looped,
                tail=tail,
                expr=expr,
            )

        def callee(target: int, kind: str) -> list[tuple[int, str]]:
            return self._call_function(
                walk, target, edge, local_conditional, local_looped, expanded, hit, kind
            )

        if event.kind in (KIND_MANUAL, KIND_MANUAL_TAIL):
            hit(HIT_XDK, event.target)
            return []
        if event.kind in (KIND_DIRECT, KIND_TAIL):
            target = event.target
            assert target is not None
            if target in self.surface:
                hit(HIT_XDK, target)
                return []
            return callee(target, "call")
        assert event.kind in (KIND_INDIRECT, KIND_INDIRECT_TAIL)
        slot = event.slot
        if slot is not None:
            ordinal = self.image.slot_ordinal(slot)
            if ordinal is not None:
                hit(HIT_KERNEL, ordinal)
                return []
            resolved = self.resolve_slot(slot)
            if resolved is not None:
                return callee(resolved, "slot")
        override = self._overrides.get(frame.function)
        if override is not None and frame.function not in self._overridden and slot is None:
            self._overridden.add(frame.function)
            return callee(override, "override")
        if local_looped:
            children: list[tuple[int, str]] = []
            for target in self.table_targets(event):
                children += callee(target, "table")
            if children:
                return children
        hit(HIT_UNRESOLVED, slot, event.expr)
        return []

    def table_targets(self, event: CallEvent) -> list[int]:
        """Routines a looped call through a bare register would reach, read from a table.

        The shape is `esi = START; edi = END; loop: eax = [esi]; call eax; esi += 4`, which
        is how the C runtime runs its static constructors. The lift leaves the loop's two
        bounds as the last two data constants before the call, in `event.nearby`. A table
        is read only when they ascend and the first holds a routine pointer, or a null
        sentinel directly before one. This is a pattern match, so every use is an
        INFERENCE, and a table that is rewritten before the loop runs is not seen.
        """
        if event.expr is None or not _BARE_REGISTER.match(event.expr) or len(event.nearby) < 2:
            return []
        start, end = event.nearby[-2:]
        if not start < end <= start + 4 * MAX_TABLE_ENTRIES or not self._is_table_start(start):
            return []
        targets: list[int] = []
        for address in range(start, end, 4):
            word = self.image.read_u32(address)
            if word is None:
                break
            if word and self.index.has_body(word):
                targets.append(word)
            elif word:
                break
        return targets

    def _is_table_start(self, address: int) -> bool:
        """A routine pointer at `address`, or a null sentinel directly before one."""
        if address % 4:
            return False
        first = self.image.read_u32(address)
        if first is None:
            return False
        if first and self.index.has_body(first):
            return True
        second = self.image.read_u32(address + 4)
        return first == 0 and second is not None and bool(second) and self.index.has_body(second)

    def _call_function(
        self,
        walk: Walk,
        target: int,
        edge: list[tuple[int, bool, bool]],
        local_conditional: bool,
        local_looped: bool,
        expanded: set[int],
        record: Callable[[str, int | None], None],
        kind: str,
    ) -> list[tuple[int, str]]:
        if not self.index.has_body(target):
            record(HIT_NO_BODY, target)
            return []
        edge.append((target, local_conditional, local_looped))
        if target in expanded:
            walk.repeats[target] = walk.repeats.get(target, 0) + 1
            return []
        return [(target, kind)]

    def closure(self, entry: int) -> Closure:
        """Everything reachable from `entry` by direct calls, resolved slots and tables.

        Unordered, and reachable-blocks only. An override or an address taken is NOT
        followed: the closure answers "what does this call statically need".
        """
        result = Closure()
        pending = [entry]
        result.functions.add(entry)
        while pending:
            current = pending.pop()
            function = self.function(current)
            shape = self.shape(current)
            for node_id in shape.order:
                node = function.nodes[node_id]
                for slot in node.slot_reads:
                    ordinal = self.image.slot_ordinal(slot)
                    if ordinal is not None:
                        result.data_imports.add(ordinal)
                result.mmio.update(node.mmio)
                for event in node.events:
                    self._closure_event(result, event, node_id in shape.cyclic, pending)
        return result

    def _closure_event(
        self, result: Closure, event: CallEvent, looped: bool, pending: list[int]
    ) -> None:
        targets: list[int] = []
        if event.kind in (KIND_MANUAL, KIND_MANUAL_TAIL):
            assert event.target is not None
            result.xdk.add(event.target)
            return
        if event.kind in (KIND_DIRECT, KIND_TAIL):
            assert event.target is not None
            if event.target in self.surface:
                result.xdk.add(event.target)
                return
            if not self.index.has_body(event.target):
                result.no_body.add(event.target)
                return
            targets = [event.target]
        else:
            slot = event.slot
            ordinal = self.image.slot_ordinal(slot) if slot is not None else None
            if ordinal is not None:
                result.kernel.add(ordinal)
                return
            resolved = self.resolve_slot(slot) if slot is not None else None
            if resolved is not None:
                targets = [resolved]
            elif looped:
                targets = [t for t in self.table_targets(event) if self.index.has_body(t)]
            if not targets:
                result.unresolved += 1
                return
        for target in targets:
            if target not in result.functions:
                result.functions.add(target)
                pending.append(target)


@dataclass
class Closure:
    """The unordered reach of one function. Used for per-entry summaries."""

    functions: set[int] = field(default_factory=set)
    xdk: set[int] = field(default_factory=set)
    kernel: set[int] = field(default_factory=set)
    data_imports: set[int] = field(default_factory=set)
    mmio: set[int] = field(default_factory=set)
    no_body: set[int] = field(default_factory=set)
    unresolved: int = 0
