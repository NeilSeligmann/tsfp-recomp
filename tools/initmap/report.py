# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn one walk into the tables the init-sequence document is built from.

Phases. The walk is a single tick counter, so a position is a tick. Two marker functions
cut it: `main` and the frame loop. A tier-1 hit before the first is `pre-main` (process and
thread start), between them is `pre-frame` (the init list, the session set-up), and after
the frame loop is first entered it is `frame-loop`, which a static reading cannot split into
"the first frame", a menu and a level. A tier-2 hit is reached only through an address that
was stored, so it has no honest position and is reported as `by-address`.
"""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field

from tools.initmap.image import AbiRow, Image, SurfaceRow, ordinal_name
from tools.initmap.liftparse import KIND_DIRECT, call_argument_words
from tools.initmap.walk import (
    CERTAIN,
    CONDITIONAL,
    HIT_DATA_IMPORT,
    HIT_HAZARD,
    HIT_KERNEL,
    HIT_MMIO,
    HIT_XDK,
    LOOPED,
    Hit,
    Walk,
    Walker,
)

PHASE_PRE_MAIN = "pre-main"
PHASE_PRE_FRAME = "pre-frame"
PHASE_FRAME = "frame-loop"
PHASE_BY_ADDRESS = "by-address"

#: Strongest first. A site that always runs beats one inside a loop beats a conditional one.
_CERTAINTY_RANK = {CERTAIN: 0, LOOPED: 1, CONDITIONAL: 2}

#: Kernel exports grouped by what they put the title at the mercy of. Names, not ordinals,
#: because the ordinal table drifts between XDK builds and a name survives that.
RISK_CATEGORIES: dict[str, frozenset[str]] = {
    "timing": frozenset(
        {
            "KeQueryPerformanceCounter",
            "KeQueryPerformanceFrequency",
            "KeStallExecutionProcessor",
            "KeDelayExecutionThread",
            "NtYieldExecution",
            "KeQuerySystemTime",
            "KeTickCount",
            "KeSetTimer",
            "KeSetTimerEx",
            "KeWaitForSingleObject",
            "KeWaitForMultipleObjects",
            "NtWaitForSingleObject",
            "NtWaitForSingleObjectEx",
            "NtWaitForMultipleObjectsEx",
        }
    ),
    "hardware": frozenset(
        {
            "HalReadSMBusValue",
            "HalWriteSMBusValue",
            "HalReadSMCTrayState",
            "HalReadWritePCISpace",
            "HalGetInterruptVector",
            "HalEnableSystemInterrupt",
            "HalDisableSystemInterrupt",
            "KeConnectInterrupt",
            "KeInitializeInterrupt",
            "KeDisconnectInterrupt",
            "MmMapIoSpace",
            "MmClaimGpuInstanceMemory",
            "MmLockUnlockBufferPages",
            "ExQueryNonVolatileSetting",
            "ExSaveNonVolatileSetting",
            "XboxHardwareInfo",
            "HalBootSMCVideoMode",
            "HalDiskModelNumber",
            "HalDiskSerialNumber",
            "HalDiskCachePartitionCount",
            "AvGetSavedDataAddress",
            "AvSetDisplayMode",
            "AvSendTVEncoderOption",
        }
    ),
    "network": frozenset({"PhyInitialize", "PhyGetLinkState"}),
    "identity and media": frozenset(
        {
            "XboxKrnlVersion",
            "XeImageFileName",
            "LaunchDataPage",
            "XboxHDKey",
            "XboxSignatureKey",
            "XboxLANKey",
            "XboxAlternateSignatureKeys",
            "XePublicKeyData",
            "XcVerifyPKCS1Signature",
            "XcModExp",
        }
    ),
}


@dataclass(frozen=True)
class Boundaries:
    """Ticks at which the marker functions were first expanded. None when never reached."""

    main: int | None
    frame_loop: int | None

    def phase(self, hit: Hit) -> str:
        if hit.tier == 2:
            return PHASE_BY_ADDRESS
        if self.main is not None and hit.tick < self.main:
            return PHASE_PRE_MAIN
        if self.frame_loop is not None and hit.tick >= self.frame_loop:
            return PHASE_FRAME
        return PHASE_PRE_FRAME


@dataclass
class ModuleRow:
    address: int
    section: str
    name: str | None
    convention: str | None
    stack_args: int | None
    register_args: int | None
    evidence: str | None
    first_tick: int
    first_site: int | None
    first_function: int
    phase: str
    certainty: str
    hits: int
    sites: int
    #: Functions from the root down to the first hit's caller, for "where does this come from".
    chain: tuple[int, ...] = ()


@dataclass
class OrdinalRow:
    ordinal: int
    name: str
    implemented: bool
    first_tick: int
    first_site: int | None
    first_function: int
    phase: str
    certainty: str
    sites: int
    categories: tuple[str, ...]


@dataclass
class InitEntry:
    index: int
    target: int
    return_va: int | None
    argument_words: int
    functions: int
    xdk: dict[str, list[int]]
    kernel: list[int]
    unimplemented: list[int]
    data_imports: list[int]
    unresolved: int
    no_body: int
    mmio: int
    #: XDK addresses whose FIRST reach in the ordered walk lies under this entry.
    first_reached: dict[str, list[int]] = field(default_factory=dict)


@dataclass
class Report:
    walk: Walk
    boundaries: Boundaries
    init_entries: list[InitEntry]
    modules: dict[str, list[ModuleRow]]
    ordinals: list[OrdinalRow]
    surface_totals: dict[str, int]
    always: frozenset[int]
    hit_counts: dict[str, int]
    phase_counts: dict[str, dict[str, int]]
    risks: dict[str, list[OrdinalRow]]
    risk_hits: dict[str, list[tuple[Hit, str]]]
    distinct_reached: dict[str, dict[str, int]]


def _best(certainties: list[str]) -> str:
    return min(certainties, key=lambda value: _CERTAINTY_RANK[value])


def boundaries_of(walk: Walk, main: int, frame_loop: int) -> Boundaries:
    return Boundaries(
        main=walk.marker_ticks.get(main),
        frame_loop=walk.marker_ticks.get(frame_loop),
    )


def frame_chain(walk: Walk, frame_ident: int) -> tuple[int, ...]:
    """The functions on the way down to a frame, outermost first."""
    chain: list[int] = []
    while frame_ident >= 0:
        frame = walk.frames[frame_ident]
        chain.append(frame.function)
        frame_ident = frame.parent
    return tuple(reversed(chain))


def module_rows(
    walk: Walk,
    surface: dict[int, SurfaceRow],
    abi: dict[int, AbiRow],
    boundaries: Boundaries,
    always: frozenset[int],
) -> dict[str, list[ModuleRow]]:
    """Per section, the reached XDK addresses in first-reach order (tier 1 before tier 2)."""
    by_address: dict[int, list[Hit]] = defaultdict(list)
    for hit in walk.hits:
        if hit.kind == HIT_XDK and hit.value is not None:
            by_address[hit.value].append(hit)
    rows: dict[str, list[ModuleRow]] = defaultdict(list)
    for address, hits in by_address.items():
        row = surface[address]
        first = min(hits, key=lambda hit: (hit.tier, hit.tick))
        convention = abi.get(address)
        rows[row.section].append(
            ModuleRow(
                address=address,
                section=row.section,
                name=row.name,
                convention=convention.convention if convention else None,
                stack_args=convention.stack_args if convention else None,
                register_args=convention.register_args if convention else None,
                evidence=convention.evidence if convention else None,
                first_tick=first.tick,
                first_site=first.return_va,
                first_function=first.function,
                phase=boundaries.phase(first),
                certainty=_best([walk.certainty(hit, always) for hit in hits]),
                hits=len(hits),
                sites=len({hit.return_va for hit in hits}),
                chain=frame_chain(walk, first.frame),
            )
        )
    for section_rows in rows.values():
        section_rows.sort(key=lambda item: (item.phase == PHASE_BY_ADDRESS, item.first_tick))
    return dict(rows)


def ordinal_rows(
    walk: Walk,
    implemented: frozenset[int],
    boundaries: Boundaries,
    always: frozenset[int],
) -> list[OrdinalRow]:
    """Every kernel ordinal reached by a call, in first-reach order."""
    by_ordinal: dict[int, list[Hit]] = defaultdict(list)
    for hit in walk.hits:
        if hit.kind == HIT_KERNEL and hit.value is not None:
            by_ordinal[hit.value].append(hit)
    rows: list[OrdinalRow] = []
    for ordinal, hits in by_ordinal.items():
        first = min(hits, key=lambda hit: (hit.tier, hit.tick))
        name = ordinal_name(ordinal)
        rows.append(
            OrdinalRow(
                ordinal=ordinal,
                name=name,
                implemented=ordinal in implemented,
                first_tick=first.tick,
                first_site=first.return_va,
                first_function=first.function,
                phase=boundaries.phase(first),
                certainty=_best([walk.certainty(hit, always) for hit in hits]),
                sites=len({hit.return_va for hit in hits}),
                categories=tuple(
                    category for category, names in RISK_CATEGORIES.items() if name in names
                ),
            )
        )
    rows.sort(key=lambda item: (item.phase == PHASE_BY_ADDRESS, item.first_tick))
    return rows


def init_entries(
    walker: Walker,
    walk: Walk,
    init_list: int,
    surface: dict[int, SurfaceRow],
    implemented: frozenset[int],
) -> list[InitEntry]:
    """One record per call in the init list: its reach, and what it first reaches in order."""
    calls = call_argument_words(walker.index.body(init_list))
    function = walker.function(init_list)
    events = [
        event for node in function.nodes for event in node.events if event.kind == KIND_DIRECT
    ]
    if len(calls) != len(events):
        raise ValueError(
            f"init list {init_list:#x}: {len(calls)} calls by text, {len(events)} by graph"
        )
    entries: list[InitEntry] = []
    by_return: dict[int, int] = {}
    for index, (event, (_, words)) in enumerate(zip(events, calls, strict=True), start=1):
        assert event.target is not None
        closure = walker.closure(event.target)
        by_section: dict[str, list[int]] = defaultdict(list)
        for address in sorted(closure.xdk):
            by_section[surface[address].section].append(address)
        kernel = sorted(closure.kernel)
        if event.return_va is not None:
            by_return[event.return_va] = index - 1
        entries.append(
            InitEntry(
                index=index,
                target=event.target,
                return_va=event.return_va,
                argument_words=words,
                functions=len(closure.functions),
                xdk=dict(by_section),
                kernel=kernel,
                unimplemented=[ordinal for ordinal in kernel if ordinal not in implemented],
                data_imports=sorted(closure.data_imports),
                unresolved=closure.unresolved,
                no_body=len(closure.no_body),
                mmio=len(closure.mmio),
            )
        )
    init_frames = [frame for frame in walk.frames if frame.function == init_list]
    if not init_frames:
        return entries
    init_ident = init_frames[0].ident
    owner: dict[int, int | None] = {}

    def entry_of(frame_ident: int) -> int | None:
        """Index of the init-list call whose subtree contains this frame."""
        if frame_ident in owner:
            return owner[frame_ident]
        frame = walk.frames[frame_ident]
        result: int | None
        if frame.parent == init_ident:
            result = by_return.get(frame.via_return) if frame.via_return is not None else None
        elif frame.parent < 0:
            result = None
        else:
            result = entry_of(frame.parent)
        owner[frame_ident] = result
        return result

    seen: set[int] = set()
    for hit in walk.hits:
        if hit.kind != HIT_XDK or hit.value is None or hit.value in seen or hit.tier != 1:
            continue
        seen.add(hit.value)
        index = entry_of(hit.frame)
        if index is not None:
            entries[index].first_reached.setdefault(surface[hit.value].section, []).append(
                hit.value
            )
    return entries


def build(
    walker: Walker,
    walk: Walk,
    image: Image,
    surface: dict[int, SurfaceRow],
    abi: dict[int, AbiRow],
    implemented: frozenset[int],
    main: int,
    frame_loop: int,
    init_list: int,
) -> Report:
    """Assemble every table from one walk."""
    boundaries = boundaries_of(walk, main, frame_loop)
    always = walk.always_functions()
    modules = module_rows(walk, surface, abi, boundaries, always)
    ordinals = ordinal_rows(walk, implemented, boundaries, always)
    totals: dict[str, int] = defaultdict(int)
    for row in surface.values():
        totals[row.section] += 1
    counts: dict[str, int] = defaultdict(int)
    phases: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for hit in walk.hits:
        counts[hit.kind] += 1
        phases[hit.kind][boundaries.phase(hit)] += 1
    risks: dict[str, list[OrdinalRow]] = defaultdict(list)
    for row in ordinals:
        for category in row.categories:
            risks[category].append(row)
    risk_hits: dict[str, list[tuple[Hit, str]]] = defaultdict(list)
    for hit in walk.hits:
        if hit.kind == HIT_MMIO:
            risk_hits["mmio"].append((hit, f"{hit.value:#010x}"))
        elif hit.kind == HIT_HAZARD:
            risk_hits["untranslated or clock"].append((hit, hit.expr or ""))
        elif hit.kind == HIT_DATA_IMPORT and hit.value is not None:
            ordinal = image.slot_ordinal(hit.value)
            label = ordinal_name(ordinal) if ordinal is not None else f"slot {hit.value:#x}"
            risk_hits["kernel data import"].append((hit, label))
    distinct: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for section, section_rows in modules.items():
        for row in section_rows:
            distinct[section][row.phase] += 1
    return Report(
        walk=walk,
        boundaries=boundaries,
        init_entries=init_entries(walker, walk, init_list, surface, implemented),
        modules=modules,
        ordinals=ordinals,
        surface_totals=dict(totals),
        always=always,
        hit_counts=dict(counts),
        phase_counts={kind: dict(values) for kind, values in phases.items()},
        risks=dict(risks),
        risk_hits=dict(risk_hits),
        distinct_reached={section: dict(values) for section, values in distinct.items()},
    )
