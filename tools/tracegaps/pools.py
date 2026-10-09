# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T606: can the saturated closures' memory sites be resolved with pools? (MEASURED verdict: no.)

The saturated closure of each thread start (`saturate.py`) keeps `call [reg + disp]` and `call reg`
sites no rule resolves. The only sound candidates for ANY such site are the taken addresses of the
running flag's callers (`addresstaking.classify` residue, `Report.running_*`): a target that is not a
member of the callers' closure cannot reach a writer. This module asks, per site, whether a pool
bounds those candidates, with the two checks T606 named.

(1) ARITY. A candidate's `ret N` against the bytes a caller pushes. Measured: of the member entries
    taken, all but a few `ret` with 0 (cdecl or thiscall without stack arguments), so no site is
    separated by arity. `arity_classes` reports the distribution.
(2) VTABLE POOLS. A candidate stored in a run of code addresses (a vtable or callback array in
    `.rdata`/`.data`) sits at a slot index. A site `call [reg + disp]` with `disp % 4 == 0` can reach
    the candidates stored at slot `disp / 4` of some run. `slot_pool` lists them per site. A pool is a
    NECESSARY condition to rule out: an empty pool would let the site be dropped, a non-empty one
    (a member entry that the site's displacement can read) keeps it open. Without a writer analysis
    (which object holds which vtable pointer, `mov [obj], offset table`) a non-empty pool cannot be
    narrowed, and object pointers arrive through arguments, so the writers are not bounded either.

The site classes with no displacement pool at all (`call reg`, `jmp reg`, frame slots `[esp + d]`)
stay open for a different reason: their value is an argument. ASSUMPTION (as `saturate`): a callback
is only reached through an address the image names, so candidates are the taken addresses.
"""

from __future__ import annotations

import collections
from dataclasses import dataclass, field

from capstone import x86 as cs_x86

from tools.tracegaps import addresstaking, gateregion, saturate, threadreach
from tools.tracegaps.code import Code
from tools.tracegaps.reach import ReachGraph, Site


@dataclass
class SiteVerdict:
    site: Site
    #: `vtable` (`call [reg + disp]`), `frame` (`[esp + d]`, `[ebp + d]`), `indexed` (`[reg*4 + base]`), `register`, `jump`
    shape: str
    #: member entries stored at slot `disp / 4` of a run, empty when none
    pool: tuple[int, ...]


@dataclass
class Report:
    members: int
    entries: int
    interiors: int
    arity_classes: dict[str, int]
    #: run start -> member slot indices (data-pointer runs holding a candidate)
    runs: dict[int, tuple[int, ...]]
    starts: dict[str, list[SiteVerdict]] = field(default_factory=dict)
    #: Reused by follow-up analyses over the same retail decode.
    _graph: ReachGraph | None = field(default=None, repr=False, compare=False)

    def open_sites(self, name: str) -> list[SiteVerdict]:
        """Sites no pool rules out (every site with a candidate, or no displacement to bound)."""
        return [
            verdict for verdict in self.starts[name] if verdict.pool or verdict.shape != "vtable"
        ]

    @property
    def closable(self) -> bool:
        """True only when every site of every start has an empty displacement pool."""
        return all(not self.open_sites(name) for name in self.starts)


def _return_sizes(graph: ReachGraph, entry: int) -> tuple[int, ...]:
    sizes: set[int] = set()
    for address in graph.body(entry):
        insn = graph.code.insn_at(address)
        if insn is not None and insn.mnemonic in ("ret", "retn"):
            sizes.add(insn.operands[0].imm if insn.operands else 0)
    return tuple(sorted(sizes))


def _run_slots(
    graph: ReachGraph, candidates: dict[int, list[addresstaking.Role]]
) -> dict[int, list[int]]:
    """Run start -> slot indices holding a candidate, over contiguous runs of code addresses."""
    image = graph.code.image
    runs: dict[int, list[int]] = collections.defaultdict(list)
    for _address, hits in graph.address_hits(sorted(candidates)).items():
        for hit in hits:
            if hit.kind != "data":
                continue
            start = hit.where
            while graph.is_code(image.u32(start - 4) or 0):
                start -= 4
            runs[start].append((hit.where - start) // 4)
    return runs


def shape_of(graph: ReachGraph, site: Site) -> tuple[str, int | None]:
    """The class of a site and its displacement when a slot index can bound it."""
    insn = graph.code.insn_at(site.address)
    if site.kind == "jump":
        return "jump", None
    if site.kind == "register" or insn is None or insn.operands[0].type != cs_x86.X86_OP_MEM:
        return "register", None
    mem = insn.operands[0].mem
    if mem.base in (cs_x86.X86_REG_ESP, cs_x86.X86_REG_EBP):
        return "frame", None
    if mem.index != 0:
        return "indexed", None
    return "vtable", mem.disp & 0xFFFFFFFF


def analyse(code: Code) -> Report:
    graph = gateregion.new_graph(code)
    threads = gateregion.register_thread_starts(graph)
    closures = {
        name: graph.closure([start]) for name, start in gateregion.OTHER_THREAD_STARTS.items()
    }
    writers: set[int] = set()
    for insn in code.absolute_operand_refs(gateregion.RUNNING):
        if any(op.type == 3 and op.access & 2 for op in insn.operands):
            owners = graph.real_owners(insn.address)
            if owners:
                writers.add(owners[0])
    running = threadreach.barrier(
        graph, tuple(sorted(writers)), next(iter(closures.values())), threads
    )
    classification = addresstaking.classify(graph, running.backward.members, running.unexplained)
    arity = collections.Counter(
        "ret " + "/".join(map(str, sizes)) if sizes else "no ret"
        for sizes in (_return_sizes(graph, entry) for entry in classification.entries)
    )
    runs = _run_slots(graph, classification.entries)
    report = Report(
        members=len(running.backward.members),
        entries=len(classification.entries),
        interiors=len(classification.interiors),
        arity_classes=dict(sorted(arity.items())),
        runs={start: tuple(sorted(slots)) for start, slots in sorted(runs.items())},
        _graph=graph,
    )
    resolver = saturate.SiteResolver(graph)
    claims = {"network poll": threadreach.poll_claims(graph)}
    for name, start in gateregion.OTHER_THREAD_STARTS.items():
        saturated = saturate.saturate(graph, start, resolver, claims.get(name))
        verdicts = []
        for site in saturated.unresolved:
            shape, disp = shape_of(graph, site)
            pool: tuple[int, ...] = ()
            if disp is not None and disp % 4 == 0:
                pool = tuple(
                    start_of_run + 4 * (disp // 4)
                    for start_of_run, slots in report.runs.items()
                    if disp // 4 in slots
                )
            verdicts.append(SiteVerdict(site, shape, pool))
        report.starts[name] = verdicts
    return report


def render(report: Report) -> str:
    lines = [
        "T606 pools for the memory sites of the saturated closures",
        f"  callers closure {report.members} functions, taken candidates {report.entries} entries + {report.interiors} interiors",
        f"  arity of the candidate entries: {report.arity_classes}",
        f"  runs holding a candidate entry: {len(report.runs)}",
    ]
    for name, verdicts in report.starts.items():
        shapes = collections.Counter(verdict.shape for verdict in verdicts)
        with_pool = sum(1 for verdict in verdicts if verdict.pool)
        lines.append(
            f"  {name}: {len(verdicts)} unresolved sites {dict(sorted(shapes.items()))}, "
            f"{with_pool} with a candidate at the displacement's slot, open {len(report.open_sites(name))}"
        )
    lines.append(f"CLOSABLE: {report.closable}")
    return "\n".join(lines)
