# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Part (b): what can reach the key argument at the vertex-key writers' non-literal sites.

THE CLAIM BEING CLOSED (T51, T97, T84c). The draw batcher `0x18EA0` stores its third
argument to the key global `0x4B85E4`, and a second writer `0x18FF0` stores its second.
Of 60 batcher sites, 56 push a literal. Four do not (`0x1CFD0`, `0x1D2BF`, `0x200F6`,
`0x20165`) and `0x18FF0` has one more (`0x18F716`). Until now "the 4 non-literal sites
select among the same finite variants" was an inference. This module derives, for each
site, the finite set of keys that can reach it, or states exactly what leaves it open.

HOW EACH SITE IS DERIVED (all by dataflow over the decoded image, no boot):

  switch wrapper sites `0x1CFD0` (wrapper `0x1CD90`) and `0x1D2BF` (wrapper `0x1D0B0`)
      The key is `esi`, set by a switch over a kind code to one of 14 literals, with a
      default arm that passes the wrapper's FIRST ARGUMENT through. Every direct caller of
      the wrapper is found in every executable section, its arguments are resolved over all
      paths, and when all of them are finite the title's own wrapper is RUN on each
      combination (`tools.tracegaps.emulate`) and `esi` read where the switch joins.

  record replay sites `0x200F6` and `0x20165`
      The key is `[record + 8]`, a field of a recorded draw. Every writer of the field goes
      through the global current-record pointer `0x55FB74` and is a "recorder". Each writes
      the key together with a TYPE (`[record + 0x14]`), and the replay function dispatches on
      the type to choose the site. The dispatch is run in the emulator over a range of
      types to find which type reaches which site, then the keys are the union over the
      recorders that write those types.

  flag word site `0x18F716` (second writer `0x18FF0`)
      The key is a frame slot of the enclosing function, a flags word built from literals
      by `or`/`add`/`and`/`neg; sbb`. The resolver enumerates it exactly over every path.

THE ANSWER IS A SUPERSET WHERE CONDITIONS ARE CORRELATED. Value sets are path-insensitive
about conditions that are not a simple compare of the same register with a literal, so a
set can include a combination the real branches never produce. It is never smaller than the
truth for the paths it models, and each finding below says which.
"""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field
from itertools import product

from capstone import x86 as cs_x86

from tools.tracegaps.code import Code
from tools.tracegaps.emulate import SCRATCH_BASE, Runner
from tools.tracegaps.flow import (
    FAMILY_OF,
    Flow,
    FlowCache,
    ValueSet,
    exact,
    substitute_arguments,
)

#: The global key and the globals of the record mechanism, all read from the image.
KEY_GLOBAL = 0x4B85E4
RECORD_POINTER = 0x55FB74
RECORD_MODE = 0x55FB6C
#: Record field offsets, as the recorders store them.
FIELD_KEY = 0x08
FIELD_FLAGS = 0x0C
FIELD_PRIM = 0x10
FIELD_TYPE = 0x14
#: A fake record in scratch RAM for the dispatch run.
RECORD_ADDRESS = SCRATCH_BASE + 0x100
#: Types the dispatch is run over: a window around every compare constant, plus extremes.
TYPE_PROBE = (*range(-2, 0x41), 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
EXTREMES = {0x7FFFFFFF, 0x80000000, 0xFFFFFFFF}


@dataclass(frozen=True)
class Batcher:
    entry: int
    arguments: int
    #: 1-based argument holding the key
    key_argument: int


BATCHERS = (Batcher(0x18EA0, 4, 3), Batcher(0x18FF0, 3, 2))
#: The record initialiser `0x1E6C0` zeroes the fields; its type store is read for the initial type.
RECORD_INITIALISER = 0x1E6C0


@dataclass(frozen=True)
class SiteSpec:
    """A non-literal site from T263 and the strategy that derives it."""

    site: int
    batcher: int
    strategy: str


SITES = (
    SiteSpec(0x1CFD0, 0x18EA0, "switch_wrapper"),
    SiteSpec(0x1D2BF, 0x18EA0, "switch_wrapper"),
    SiteSpec(0x200F6, 0x18EA0, "record_replay"),
    SiteSpec(0x20165, 0x18EA0, "record_replay"),
    SiteSpec(0x18F716, 0x18FF0, "flag_slot"),
)
WRAPPER_ENTRIES = (0x1CD90, 0x1D0B0)


@dataclass
class SiteKey:
    """One batcher call site and what reaches its key argument."""

    site: int
    section: str
    entry: int
    arguments: list[ValueSet]

    def key(self, batcher: Batcher) -> ValueSet:
        return self.arguments[batcher.key_argument - 1]

    def is_literal(self, batcher: Batcher) -> bool:
        key = self.key(batcher)
        return key.bounded and len(key.values) == 1


@dataclass
class Census:
    batcher: Batcher
    sites: list[SiteKey]
    undecodable_note: str = ""

    def literal_values(self) -> list[int]:
        return sorted(
            {
                next(iter(s.key(self.batcher).values))
                for s in self.sites
                if s.is_literal(self.batcher)
            }
        )

    def literal_sites(self) -> list[SiteKey]:
        return [s for s in self.sites if s.is_literal(self.batcher)]

    def non_literal_sites(self) -> list[SiteKey]:
        return [s for s in self.sites if not s.is_literal(self.batcher)]


def census_batcher(code: Code, flows: FlowCache, batcher: Batcher) -> Census:
    sites: list[SiteKey] = []
    for transfer in code.callers(batcher.entry):
        flow = flows.flow_for(transfer.address)
        sites.append(
            SiteKey(
                site=transfer.address,
                section=transfer.section,
                entry=flow.entry,
                arguments=flow.call_arguments(transfer.address, batcher.arguments),
            )
        )
    sites.sort(key=lambda item: item.site)
    return Census(batcher, sites)


# --------------------------------------------------------------------------- switch wrappers


@dataclass
class WrapperCaller:
    call: int
    section: str
    entry: int
    arguments: list[ValueSet]
    #: "exact" when the wrapper was run on every combination, else "bound"
    mode: str
    keys: ValueSet
    flags: ValueSet


@dataclass
class WrapperAnalysis:
    entry: int
    site: int
    join: int
    argument_count: int
    relevant: set[int]
    structural_key: ValueSet
    structural_flags: ValueSet
    callers: list[WrapperCaller] = field(default_factory=list)

    def key_union(self) -> ValueSet:
        result = ValueSet()
        for caller in self.callers:
            result = result.union(caller.keys)
        return result

    def flags_union(self) -> ValueSet:
        result = ValueSet()
        for caller in self.callers:
            result = result.union(caller.flags)
        return result

    def exact_union(
        self, *, skip: frozenset[int] = frozenset()
    ) -> tuple[ValueSet | None, ValueSet]:
        """Key and flag unions over callers outside `skip`, or (None, empty) if there are none."""
        keys, flags, seen = ValueSet(), ValueSet(), False
        for caller in self.callers:
            if caller.entry not in skip:
                keys, flags, seen = keys.union(caller.keys), flags.union(caller.flags), True
        return (keys if seen else None), flags

    def argument_union(self, number: int, *, skip: frozenset[int] = frozenset()) -> ValueSet:
        result = ValueSet()
        for caller in self.callers:
            if caller.entry not in skip:
                result = result.union(caller.arguments[number - 1])
        return result


def analyze_wrapper(
    code: Code, flows: FlowCache, runner: Runner, entry: int, site: int, batcher: Batcher
) -> WrapperAnalysis:
    flow = flows.flow_for(site, entry=entry)
    mode_refs = [
        i.address for i in code.absolute_operand_refs(RECORD_MODE) if i.address in flow.insns
    ]
    if not mode_refs:
        raise LookupError(f"wrapper {entry:#x} never tests the record-mode flag {RECORD_MODE:#x}")
    join = min(mode_refs)
    argument_count = max(flow.incoming_arguments_read())
    call_arguments = flow.call_arguments(site, batcher.arguments)
    analysis = WrapperAnalysis(
        entry=entry,
        site=site,
        join=join,
        argument_count=argument_count,
        relevant=flow.incoming_arguments_read(before=join),
        structural_key=call_arguments[batcher.key_argument - 1],
        structural_flags=call_arguments[1],
    )
    for transfer in code.callers(entry):
        caller_flow = flows.flow_for(transfer.address)
        arguments = caller_flow.call_arguments(transfer.address, argument_count)
        analysis.callers.append(
            _wrapper_caller(
                runner, analysis, transfer.address, transfer.section, caller_flow.entry, arguments
            )
        )
    analysis.callers.sort(key=lambda item: item.call)
    return analysis


def _wrapper_caller(
    runner: Runner,
    analysis: WrapperAnalysis,
    call: int,
    section: str,
    entry: int,
    arguments: list[ValueSet],
) -> WrapperCaller:
    needed = [arguments[n - 1] for n in sorted(analysis.relevant)]
    if all(value.bounded and value.values for value in needed):
        keys, flags = ValueSet(), ValueSet()
        domains = [
            sorted(arguments[n - 1].values) if n in analysis.relevant else [0]
            for n in range(1, analysis.argument_count + 1)
        ]
        for combination in product(*domains):
            capture = runner.run(analysis.entry, combination, [analysis.join], words=1)
            if capture is None:
                raise RuntimeError(f"wrapper {analysis.entry:#x} did not reach {analysis.join:#x}")
            keys = keys.union(exact(capture.registers["esi"]))
            flags = flags.union(exact(capture.registers["ebx"]))
        return WrapperCaller(call, section, entry, arguments, "exact", keys, flags)
    mapping = {n: arguments[n - 1] for n in range(1, analysis.argument_count + 1)}
    return WrapperCaller(
        call,
        section,
        entry,
        arguments,
        "bound",
        substitute_arguments(analysis.structural_key, analysis.entry, mapping),
        substitute_arguments(analysis.structural_flags, analysis.entry, mapping),
    )


# --------------------------------------------------------------------------- recorders


@dataclass(frozen=True)
class RecorderStore:
    function: int
    load: int
    store: int
    field: int
    value: ValueSet


@dataclass
class RecorderCensus:
    stores: list[RecorderStore]
    #: instructions that load the current-record pointer but are followed by no matched store
    bare_loads: list[int]
    #: instructions that overwrite the current-record pointer (initialisers, resets)
    setters: list[int]

    def functions(self) -> list[int]:
        return sorted({store.function for store in self.stores})

    def field_values(self, function: int, field_offset: int) -> ValueSet:
        result = ValueSet()
        for store in self.stores:
            if store.function == function and store.field == field_offset:
                result = result.union(store.value)
        return result


def census_recorders(code: Code, flows: FlowCache) -> RecorderCensus:
    stores: list[RecorderStore] = []
    bare: list[int] = []
    setters: list[int] = []
    for insn in code.absolute_operand_refs(RECORD_POINTER):
        ops = insn.operands
        is_load = (
            insn.mnemonic == "mov"
            and len(ops) == 2
            and ops[0].type == cs_x86.X86_OP_REG
            and ops[1].type == cs_x86.X86_OP_MEM
        )
        if not is_load:
            setters.append(insn.address)
            continue
        flow = flows.flow_for(insn.address)
        register = ops[0].reg
        matched = False
        cursor = code.next_insn(insn)
        for _ in range(4):
            if cursor is None:
                break
            cops = cursor.operands
            if (
                cursor.mnemonic == "mov"
                and len(cops) == 2
                and cops[0].type == cs_x86.X86_OP_MEM
                and cops[0].mem.base == register
                and cops[0].mem.index == 0
            ):
                stores.append(
                    RecorderStore(
                        flow.entry,
                        insn.address,
                        cursor.address,
                        cops[0].mem.disp,
                        flow.operand_value(cursor, cops[1]),
                    )
                )
                matched = True
            elif Flow.writes_family(cursor, FAMILY_OF.get(cursor.reg_name(register), "")):
                break
            cursor = code.next_insn(cursor)
        if not matched:
            bare.append(insn.address)
    stores.sort(key=lambda item: (item.function, item.store))
    return RecorderCensus(stores, sorted(bare), sorted(setters))


# --------------------------------------------------------------------------- replay dispatch


@dataclass
class Dispatch:
    """The replay function's type dispatch, run over `TYPE_PROBE`."""

    function: int
    start: int
    #: arm entry (the stop address) -> types that reach it
    arms: dict[int, list[int]]
    else_arm: int

    def arm_of(self, flow: Flow, site: int) -> int | None:
        stops = set(self.arms)
        for stop in stops:
            if flow.reaches(stop, site) and not flow.reaches(flow.entry, site, avoiding={stop}):
                return stop
        return None


def run_dispatch(code: Code, flows: FlowCache, runner: Runner, site: int) -> Dispatch:
    flow = flows.flow_for(site)
    loads = sorted(
        address
        for address, insn in flow.insns.items()
        for operand in insn.operands
        if operand.type == cs_x86.X86_OP_MEM
        and operand.mem.disp == FIELD_TYPE
        and operand.mem.base != 0
        and insn.reg_name(operand.mem.base) != "esp"
        and operand.access & 1
    )
    if not loads:
        raise LookupError(f"replay function {flow.entry:#x} never reads the type field")
    start = loads[0]
    base_register = flow.insns[start].reg_name(
        next(o.mem.base for o in flow.insns[start].operands if o.type == cs_x86.X86_OP_MEM)
    )
    chain_end = min(
        address
        for address, insn in flow.insns.items()
        if address > start and insn.mnemonic == "call"
    )
    arm_entries = {
        flow.insns[a].operands[0].imm & 0xFFFFFFFF
        for a in flow.insns
        if start <= a < chain_end
        and flow.insns[a].mnemonic.startswith("j")
        and flow.insns[a].mnemonic != "jmp"
        and flow.insns[a].operands[0].type == cs_x86.X86_OP_IMM
    }
    stops = sorted({*arm_entries, chain_end})
    arms: dict[int, list[int]] = defaultdict(list)
    for probe in TYPE_PROBE:
        capture = runner.run(
            start,
            [],
            stops,
            words=1,
            registers={base_register: RECORD_ADDRESS},
            memory={RECORD_ADDRESS + FIELD_TYPE: probe & 0xFFFFFFFF, RECORD_ADDRESS + 0x18: 0},
        )
        if capture is None:
            raise RuntimeError(f"dispatch at {start:#x} reached no arm for type {probe:#x}")
        arms[capture.stop].append(probe & 0xFFFFFFFF)
    return Dispatch(flow.entry, start, dict(arms), chain_end)


# --------------------------------------------------------------------------- the report


@dataclass
class ReplayClosure:
    """The wrapper run on every record the replay's else arm can hand it."""

    types: list[int]
    #: domains as the recorders and the initialiser leave them
    flags: list[int]
    keys: list[int]
    #: what the wrapper returns on that domain alone
    plain_keys: list[int]
    #: what it returns once its own outputs may be recorded and replayed again
    closed_keys: list[int]
    closed_flags: list[int]
    iterations: int
    runs: int


@dataclass
class SiteResult:
    spec: SiteSpec
    key: ValueSet
    flags: ValueSet
    lines: list[str]


@dataclass(frozen=True)
class FlowCheck:
    """Health of the control-flow graph of one analysed function."""

    instructions: int
    #: indirect jumps no table could be read for (code behind them is missing from the graph)
    unresolved_switches: int
    #: instructions reached at two different stack depths (slot results are untrusted if non-zero)
    stack_conflicts: int
    #: instructions where the stack model was lost
    lost_deltas: int


@dataclass
class Emissions:
    """Distinct vertex and pixel sources the title's builders emit for nested key sets."""

    #: one label per key set, smallest first (each set contains the one before it)
    labels: list[str]
    key_counts: list[int]
    #: builder name -> per key set `(distinct masked keys, distinct sources)`
    by_builder: dict[str, list[tuple[int, int]]]


@dataclass
class Report:
    censuses: dict[int, Census]
    wrappers: dict[int, WrapperAnalysis]
    recorders: RecorderCensus
    dispatch: Dispatch
    record_types: dict[int, ValueSet]
    record_keys: dict[int, ValueSet]
    record_flags: dict[int, ValueSet]
    initial: dict[int, ValueSet]
    else_types: ValueSet
    closure: ReplayClosure | None
    results: list[SiteResult]
    unexplained: list[int]
    literal_keys: list[int]
    new_keys: list[int]
    masked_counts: dict[str, int]
    replay_callers: list[int]
    initialiser_callers: list[int]
    emissions: Emissions | None = None
    #: function entry -> occurrences of its address outside decoded code (a pointer table would show)
    pointer_hits: dict[int, list[tuple[str, int]]] = field(default_factory=dict)
    #: every function the analysis read, with the health of its graph
    flow_checks: dict[int, FlowCheck] = field(default_factory=dict)


def measure_emissions(code: Code, key_sets: list[tuple[str, set[int]]]) -> Emissions:
    """Run the title's two source builders on `key & mask` for each key set and count distinct output."""
    from tools.shaderprobe.profile import RETAIL
    from tools.shaderscan.builders import BuilderEmulator, BuilderSpec, enumerate_keys

    by_builder: dict[str, list[tuple[int, int]]] = {}
    for builder in (RETAIL.vertex_builder, RETAIL.pixel_builder):
        spec = BuilderSpec(builder.name, builder.entry, builder.key_register, RETAIL.assembler)
        emulator = BuilderEmulator(code.image, spec)
        counts: list[tuple[int, int]] = []
        for _, keys in key_sets:
            masked = {key & builder.mask for key in keys}
            result = enumerate_keys(emulator, sorted(masked))
            if result.failures or result.no_call:
                raise RuntimeError(
                    f"{builder.name} builder: {result.failures} faults, {result.no_call} runs without an assembler call"
                )
            counts.append((len(masked), result.distinct))
        by_builder[builder.name] = counts
    return Emissions(
        [label for label, _ in key_sets], [len(keys) for _, keys in key_sets], by_builder
    )


def analyze(code: Code, flows: FlowCache, runner: Runner, *, with_emissions: bool = True) -> Report:
    from tools.shaderprobe.profile import RETAIL

    censuses = {b.entry: census_batcher(code, flows, b) for b in BATCHERS}
    batchers = {b.entry: b for b in BATCHERS}

    wrappers: dict[int, WrapperAnalysis] = {}
    for spec in SITES:
        if spec.strategy == "switch_wrapper":
            entry = flows.flow_for(spec.site).entry
            wrappers[spec.site] = analyze_wrapper(
                code, flows, runner, entry, spec.site, batchers[spec.batcher]
            )

    record_sites = [spec.site for spec in SITES if spec.strategy == "record_replay"]
    dispatch = run_dispatch(code, flows, runner, record_sites[0])
    replay_flow = flows.at_entry(dispatch.function)
    explicit = {stop: set(v) for stop, v in dispatch.arms.items() if stop != dispatch.else_arm}

    recorders = census_recorders(code, flows)
    initial = _initial_fields(code, flows)
    wrapper_by_entry = {w.entry: w for w in wrappers.values()}
    replay_only = frozenset({dispatch.function})
    record_types: dict[int, ValueSet] = {}
    record_keys: dict[int, ValueSet] = {}
    record_flags: dict[int, ValueSet] = {}
    for function in recorders.functions():
        types = recorders.field_values(function, FIELD_TYPE)
        keys = recorders.field_values(function, FIELD_KEY)
        flags = recorders.field_values(function, FIELD_FLAGS)
        if function in wrapper_by_entry:
            wrapper = wrapper_by_entry[function]
            args = {
                n: wrapper.argument_union(n, skip=replay_only)
                for n in range(1, wrapper.argument_count + 1)
            }
            types = substitute_arguments(types, function, args)
            keys = substitute_arguments(keys, function, args)
            flags = substitute_arguments(flags, function, args)
            exact_keys, exact_flags = wrapper.exact_union(skip=replay_only)
            if exact_keys is not None:
                keys, flags = exact_keys, exact_flags
        record_types[function], record_keys[function], record_flags[function] = types, keys, flags

    explicit_union = set().union(*explicit.values()) if explicit else set()
    all_types = initial.get(FIELD_TYPE, ValueSet())
    for types in record_types.values():
        all_types = all_types.union(types)
    else_types = ValueSet(
        frozenset(v for v in all_types.values if v not in explicit_union), all_types.unbounded
    )
    closure = _close_replay(
        runner, wrappers, dispatch, record_types, record_keys, record_flags, initial, else_types
    )

    results = [
        _site_result(
            spec,
            batchers[spec.batcher],
            flows,
            wrappers,
            dispatch,
            replay_flow,
            record_types,
            record_keys,
            record_flags,
            explicit,
        )
        for spec in SITES
    ]
    named = {spec.site for spec in SITES}
    unexplained = sorted(
        site.site
        for census in censuses.values()
        for site in census.non_literal_sites()
        if site.site not in named
    )
    literal = sorted(
        set(censuses[0x18EA0].literal_values()) | set(censuses[0x18FF0].literal_values())
    )
    reached: set[int] = set()
    for result in results:
        reached.update(result.key.values)
    masks = {"vertex": RETAIL.vertex_builder.mask, "pixel": RETAIL.pixel_builder.mask}
    table_literals = set()
    for wrapper in wrappers.values():
        table_literals.update(wrapper.structural_key.values)
    base_keys = set(literal) | table_literals
    four_sites: set[int] = set()
    for result in results:
        if result.spec.strategy != "flag_slot":
            four_sites.update(result.key.values)
    emissions = (
        measure_emissions(
            code,
            [
                (
                    "the 23 base keys (12 batcher literals plus the 14 wrapper table literals)",
                    base_keys,
                ),
                (
                    "base plus the keys at 0x1cfd0, 0x1d2bf, 0x200f6, 0x20165",
                    base_keys | four_sites,
                ),
                ("base plus all five sites (adds 0x18f716)", base_keys | reached),
            ],
        )
        if with_emissions
        else None
    )
    analysed = {w.entry for w in wrappers.values()} | {dispatch.function, RECORD_INITIALISER}
    analysed.update(recorders.functions())
    analysed.add(
        flows.flow_for(next(r.spec.site for r in results if r.spec.strategy == "flag_slot")).entry
    )
    flow_checks = {}
    for entry in sorted(analysed):
        graph = flows.at_entry(entry)
        flow_checks[entry] = FlowCheck(
            len(graph.insns),
            len(graph.unresolved_indirect),
            len(graph.delta_conflicts),
            len(graph.delta_lost),
        )
    return Report(
        censuses=censuses,
        wrappers=wrappers,
        recorders=recorders,
        dispatch=dispatch,
        record_types=record_types,
        record_keys=record_keys,
        record_flags=record_flags,
        initial=initial,
        else_types=else_types,
        closure=closure,
        results=results,
        unexplained=unexplained,
        literal_keys=literal,
        new_keys=sorted(reached - set(literal)),
        masked_counts={
            name: len({key & mask for key in reached | set(literal)})
            for name, mask in masks.items()
        },
        replay_callers=[t.address for t in code.callers(dispatch.function)],
        initialiser_callers=[t.address for t in code.callers(RECORD_INITIALISER)],
        emissions=emissions,
        flow_checks=flow_checks,
        pointer_hits={
            entry: [(n, a) for n, a, in_code in code.dword_hits(entry) if not in_code]
            for entry in sorted(
                {b.entry for b in BATCHERS}
                | {w.entry for w in wrappers.values()}
                | {dispatch.function, RECORD_INITIALISER}
            )
        },
    )


def _initial_fields(code: Code, flows: FlowCache) -> dict[int, ValueSet]:
    """What the record initialiser stores in each field, so a fresh record has known contents."""
    flow = flows.at_entry(RECORD_INITIALISER)
    fields: dict[int, ValueSet] = {}
    for _, insn in sorted(flow.insns.items()):
        ops = insn.operands
        if insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != cs_x86.X86_OP_MEM:
            continue
        offset = ops[0].mem.disp
        if (
            offset in (FIELD_KEY, FIELD_FLAGS, FIELD_PRIM, FIELD_TYPE, 0x18)
            and insn.reg_name(ops[0].mem.base) != "esp"
        ):
            fields[offset] = fields.get(offset, ValueSet()).union(flow.operand_value(insn, ops[1]))
    return fields


def _close_replay(
    runner: Runner,
    wrappers: dict[int, WrapperAnalysis],
    dispatch: Dispatch,
    record_types: dict[int, ValueSet],
    record_keys: dict[int, ValueSet],
    record_flags: dict[int, ValueSet],
    initial: dict[int, ValueSet],
    else_types: ValueSet,
) -> ReplayClosure | None:
    """Run the wrapper on every (type, flags, key) a record on the else arm can hold.

    The replay's else arm calls the wrapper with `(record type, record flags, record key)`.
    Those fields were written by the recorders whose types are not on a dispatch arm, or
    by the initialiser. Their union is the domain; the wrapper's own outputs are added back
    (a replay that records again) until nothing new appears.
    """
    replay_callers = [
        (wrapper, caller)
        for wrapper in wrappers.values()
        for caller in wrapper.callers
        if caller.entry == dispatch.function
    ]
    if not replay_callers or not else_types.bounded:
        return None
    types = sorted(else_types.values)
    flags = ValueSet(
        initial.get(FIELD_FLAGS, ValueSet()).values, initial.get(FIELD_FLAGS, ValueSet()).unbounded
    )
    keys = ValueSet(
        initial.get(FIELD_KEY, ValueSet()).values, initial.get(FIELD_KEY, ValueSet()).unbounded
    )
    for function, written in record_types.items():
        if written.bounded and not written.values & set(types):
            continue
        flags = flags.union(record_flags[function])
        keys = keys.union(record_keys[function])
    if not (flags.bounded and keys.bounded):
        return None
    wrapper, _ = replay_callers[0]
    domain_flags, domain_keys = sorted(flags.values), sorted(keys.values)
    plain_keys: set[int] = set()
    iterations = runs = 0
    while True:
        iterations += 1
        produced_keys: set[int] = set()
        produced_flags: set[int] = set()
        for combination in product(types, sorted(flags.values), sorted(keys.values)):
            padded = [*combination, *([0] * (wrapper.argument_count - 3))]
            capture = runner.run(wrapper.entry, padded, [wrapper.join], words=1)
            runs += 1
            if capture is None:
                raise RuntimeError(f"wrapper {wrapper.entry:#x} did not reach {wrapper.join:#x}")
            produced_keys.add(capture.registers["esi"])
            produced_flags.add(capture.registers["ebx"])
        if iterations == 1:
            plain_keys = set(produced_keys)
        if produced_keys <= keys.values and produced_flags <= flags.values:
            break
        keys = keys.union(exact(*produced_keys))
        flags = flags.union(exact(*produced_flags))
    for wrapper_item, caller in replay_callers:
        if wrapper_item.entry == wrapper.entry:
            caller.mode = "exact over the else-arm record domain"
            caller.keys = exact(*produced_keys)
            caller.flags = exact(*produced_flags)
    return ReplayClosure(
        types,
        domain_flags,
        domain_keys,
        sorted(plain_keys),
        sorted(produced_keys),
        sorted(produced_flags),
        iterations,
        runs,
    )


def _site_result(
    spec: SiteSpec,
    batcher: Batcher,
    flows: FlowCache,
    wrappers: dict[int, WrapperAnalysis],
    dispatch: Dispatch,
    replay_flow: Flow,
    record_types: dict[int, ValueSet],
    record_keys: dict[int, ValueSet],
    record_flags: dict[int, ValueSet],
    explicit: dict[int, set[int]],
) -> SiteResult:
    lines: list[str] = []
    if spec.strategy == "switch_wrapper":
        wrapper = wrappers[spec.site]
        lines.append(
            f"wrapper {wrapper.entry:#x}, key formed at the join {wrapper.join:#x}, arguments read "
            f"before it {sorted(wrapper.relevant)} of {wrapper.argument_count}"
        )
        lines.append(f"structural key set {wrapper.structural_key.describe()}")
        return SiteResult(spec, wrapper.key_union(), wrapper.flags_union(), lines)
    if spec.strategy == "record_replay":
        arm = dispatch.arm_of(replay_flow, spec.site)
        if arm is None:
            raise LookupError(f"site {spec.site:#x} is not dominated by one dispatch arm")
        types = sorted(explicit.get(arm, []))
        lines.append(
            f"replay function {dispatch.function:#x}, dispatch at {dispatch.start:#x}: site is dominated by arm "
            f"{arm:#x}, reached by types {[hex(t) for t in types]}"
        )
        keys, flags = ValueSet(), ValueSet()
        for function, written in sorted(record_types.items()):
            if written.bounded and not (written.values & set(types)):
                continue
            keys = keys.union(record_keys[function])
            flags = flags.union(record_flags[function])
            lines.append(
                f"recorder {function:#x} writes types {written.describe()} keys {record_keys[function].describe()}"
            )
        return SiteResult(spec, keys, flags, lines)
    flow = flows.flow_for(spec.site)
    arguments = flow.call_arguments(spec.site, batcher.arguments)
    lines.append(
        f"enclosing function {flow.entry:#x}, {len(flow.insns)} instructions, stack conflicts "
        f"{len(flow.delta_conflicts)}, unresolved switches {len(flow.unresolved_indirect)}"
    )
    return SiteResult(spec, arguments[batcher.key_argument - 1], arguments[0], lines)


# --------------------------------------------------------------------------- rendering


def _hex_list(values: object) -> str:
    return ", ".join(f"{v:#x}" for v in sorted(values))  # type: ignore[attr-defined]


def _summary(value: ValueSet) -> str:
    values = sorted(value.values)
    if not values:
        return value.describe()
    always, anyset = 0xFFFFFFFF, 0
    for item in values:
        always &= item
        anyset |= item
    return (
        f"{len(values)} values, lowest {values[0]:#x}, highest {values[-1]:#x}, bits always set {always:#x}, "
        f"bits ever set {anyset:#x}" + "".join(f"; UNBOUNDED({r})" for r in value.unbounded)
    )


def render(report: Report, *, verbose: bool = False) -> str:
    out: list[str] = []
    add = out.append
    for entry, census in report.censuses.items():
        add(
            f"batcher {entry:#x}: {len(census.sites)} direct call sites in all executable sections; key is "
            f"argument {census.batcher.key_argument}: {len(census.literal_sites())} literal "
            f"({len(census.literal_values())} distinct), {len(census.non_literal_sites())} not a single literal"
        )
        add(f"  literal key values: {_hex_list(census.literal_values())}")
        for site in census.non_literal_sites():
            add(
                f"  non-literal {site.site:#x} [{site.section}] in {site.entry:#x}: {_summary(site.key(census.batcher))[:200]}"
            )
    add("")
    for result in report.results:
        shown = result.key.describe() if len(result.key.values) <= 40 else _summary(result.key)
        add(
            f"SITE {result.spec.site:#x} ({result.spec.strategy}): {len(result.key.values)} key value(s) {shown}"
        )
        add(
            f"  flags/kind argument: {result.flags.describe() if len(result.flags.values) <= 40 else _summary(result.flags)}"
        )
        for line in result.lines:
            add(f"  {line}")
        wrapper = report.wrappers.get(result.spec.site)
        if wrapper is not None:
            groups: dict[tuple[str, str, str], list[int]] = defaultdict(list)
            for caller in wrapper.callers:
                shown_args = ", ".join(
                    a.describe() for a in caller.arguments[: max(wrapper.relevant)]
                )
                groups[(shown_args, caller.mode, caller.keys.describe())].append(caller.call)
            add(
                f"  callers of {wrapper.entry:#x}: {len(wrapper.callers)} in {len(groups)} distinct (arguments, result) shapes"
            )
            for (arguments, mode, keys), calls in sorted(
                groups.items(), key=lambda item: min(item[1])
            ):
                listing = ", ".join(f"{c:#x}" for c in (calls if verbose else calls[:4]))
                more = "" if verbose or len(calls) <= 4 else f" +{len(calls) - 4}"
                add(f"    {len(calls):3d}x ({arguments}) [{mode}] -> {keys}   at {listing}{more}")
    add("")
    add(
        f"replay dispatch ({report.dispatch.function:#x}, reads the type at {report.dispatch.start:#x}): "
        + "; ".join(
            f"arm {stop:#x}{' (else)' if stop == report.dispatch.else_arm else ''} <- {_types(values)}"
            for stop, values in sorted(report.dispatch.arms.items())
        )
    )
    add(
        f"record initialiser {RECORD_INITIALISER:#x} stores "
        + ", ".join(f"+{o:#x}={v.describe()}" for o, v in sorted(report.initial.items()))
    )
    add(f"else-arm record types {report.else_types.describe()}")
    if report.closure is not None:
        add(
            f"replay closure: wrapper run on {len(report.closure.types)} types x {len(report.closure.flags)} flags x "
            f"{len(report.closure.keys)} keys as the recorders leave them, {report.closure.runs} runs, "
            f"{report.closure.iterations} pass(es)"
        )
        add(f"  keys it returns on that domain alone: {_hex_list(report.closure.plain_keys)}")
        add(
            "  keys once its own outputs may be recorded and replayed (sound bound, used for the site): "
            f"{_hex_list(report.closure.closed_keys)}"
        )
    add(f"recorders (writers through the global record pointer {RECORD_POINTER:#x}):")
    for function in report.recorders.functions():
        add(
            f"  {function:#x}: types {report.record_types[function].describe()} keys {report.record_keys[function].describe()} "
            f"flags {report.record_flags[function].describe()}"
        )
    add(
        f"  pointer setters {_hex_list(report.recorders.setters)}; loads with no matched store "
        f"{_hex_list(report.recorders.bare_loads) or 'none'}"
    )
    add(
        f"  initialiser called from {_hex_list(report.initialiser_callers)}; "
        f"replay function called from {_hex_list(report.replay_callers)}"
    )
    add(
        "occurrences of each function address outside decoded code (a pointer table would show): "
        + ", ".join(f"{entry:#x}: {len(hits)}" for entry, hits in report.pointer_hits.items())
    )
    add("graphs read (instructions, unresolved switches, stack conflicts, lost stack models):")
    for entry, check in report.flow_checks.items():
        add(
            f"  {entry:#x}: {check.instructions}, {check.unresolved_switches}, "
            f"{check.stack_conflicts}, {check.lost_deltas}"
        )
    add(f"unexplained non-literal sites: {_hex_list(report.unexplained) or 'none'}")
    add(
        f"literal keys {len(report.literal_keys)}; keys at the five sites that are not among them: {len(report.new_keys)}; "
        f"distinct after the vertex mask {report.masked_counts['vertex']}, pixel mask {report.masked_counts['pixel']} "
        "(literals plus the five sites, before the 0x22be0 OR/AND variants)"
    )
    if report.emissions is not None:
        emissions = report.emissions
        add(
            "title builders run on key & mask, no 0x22be0 OR/AND variants (distinct masked keys -> distinct sources):"
        )
        for position, label in enumerate(emissions.labels):
            parts = ", ".join(
                f"{name} {counts[position][0]} -> {counts[position][1]}"
                for name, counts in emissions.by_builder.items()
            )
            add(f"  {emissions.key_counts[position]:4d} keys, {label}: {parts}")
    add("")
    out.extend(verdict(report))
    return "\n".join(out)


def _types(values: list[int]) -> str:
    signed = sorted(v - (1 << 32) if v >= 0xFFFFFFF0 else v for v in values if v not in EXTREMES)
    if len(signed) <= 4:
        return f"types {signed}"
    return f"{len(values)} probed types such as {signed[:6]}"


# --------------------------------------------------------------------------- json and verdict


def value_json(value: ValueSet) -> dict[str, object]:
    return {"values": sorted(value.values), "unbounded": list(value.unbounded)}


def to_json(report: Report) -> dict[str, object]:
    return {
        "census": {
            f"{entry:#x}": {
                "sites": len(census.sites),
                "literal_sites": len(census.literal_sites()),
                "literal_values": census.literal_values(),
                "non_literal": [site.site for site in census.non_literal_sites()],
            }
            for entry, census in report.censuses.items()
        },
        "sites": {
            f"{result.spec.site:#x}": {
                "strategy": result.spec.strategy,
                "key": value_json(result.key),
                "flags": value_json(result.flags),
            }
            for result in report.results
        },
        "dispatch": {f"{stop:#x}": values for stop, values in sorted(report.dispatch.arms.items())},
        "recorders": {
            f"{function:#x}": {
                "types": value_json(report.record_types[function]),
                "keys": value_json(report.record_keys[function]),
                "flags": value_json(report.record_flags[function]),
            }
            for function in report.recorders.functions()
        },
        "else_types": value_json(report.else_types),
        "replay_closure": None
        if report.closure is None
        else {"plain_keys": report.closure.plain_keys, "closed_keys": report.closure.closed_keys},
        "unexplained": report.unexplained,
        "new_keys": report.new_keys,
        "masked_counts": report.masked_counts,
        "emissions": None
        if report.emissions is None
        else {
            "labels": report.emissions.labels,
            "key_counts": report.emissions.key_counts,
            "by_builder": {
                name: [list(c) for c in counts]
                for name, counts in report.emissions.by_builder.items()
            },
        },
        "pointer_hits": {f"{entry:#x}": len(hits) for entry, hits in report.pointer_hits.items()},
        "flow_checks": {
            f"{entry:#x}": [c.instructions, c.unresolved_switches, c.stack_conflicts, c.lost_deltas]
            for entry, c in report.flow_checks.items()
        },
    }


def verdict(report: Report) -> list[str]:
    """MEASURED and NOT DERIVABLE statements, generated from the data above."""
    lines: list[str] = []
    for result in report.results:
        count = len(result.key.values)
        state = (
            "a finite set (an upper bound, see below)"
            if result.key.bounded
            else "finite with open parts"
        )
        lines.append(
            f"MEASURED site {result.spec.site:#x}: {count} key value(s), {state} ({result.spec.strategy})"
        )
    lines.append(
        f"MEASURED non-literal sites outside the five named: {len(report.unexplained)}"
        + (f" ({_hex_list(report.unexplained)})" if report.unexplained else "")
    )
    lines.append(
        f"MEASURED keys at the five sites outside the {len(report.literal_keys)} batcher literals: {len(report.new_keys)}"
    )
    if report.emissions is not None:
        for name, counts in report.emissions.by_builder.items():
            lines.append(
                f"MEASURED {name} sources (no 0x22be0 variants): "
                + ", then ".join(str(sources) for _, sources in counts)
                + " for the three nested key sets above"
            )
    unresolved = [f"{e:#x}" for e, c in report.flow_checks.items() if c.unresolved_switches]
    if unresolved:
        lines.append(
            "NOT DERIVABLE the code behind an unresolved switch in " + ", ".join(unresolved)
        )
    lines.append(
        "NOT DERIVABLE which subset of a finite set the real branches form: value sets ignore conditions that are not a compare of the same register with a literal, "
        "so correlated or data-dependent branches (for example `0x18F716`'s flag bits) make each set an upper bound"
    )
    lines.append(
        "NOT DERIVABLE whether any site runs in a boot, which callers run, or in what order (no execution)"
    )
    lines.append(
        "NOT DERIVABLE a store into a record field through a pointer other than the global current-record pointer "
        f"({RECORD_POINTER:#x}): that needs alias analysis of the records' owners "
        f"(initialiser callers {_hex_list(report.initialiser_callers)}, replay callers {_hex_list(report.replay_callers)})"
    )
    lines.append(
        "NOT DERIVABLE whether a replay can record again: the replay-wrapper's extra keys are reported both without and with that feedback"
    )
    lines.append(
        "NOT DERIVABLE a caller reached only through a register or memory call: the census sees direct calls and tail jumps, "
        "and the pointer-table check above covers stored addresses"
    )
    return lines
