# SPDX-License-Identifier: GPL-3.0-or-later
"""Decide which functions this harness is entitled to claim a verdict on.

Scope is restricted so the ORACLE stays trustworthy and the measurement isolates what
it is actually testing: integer, flag, addressing and memory semantics. Everything
excluded is excluded for a stated reason and that reason is carried into the results
CSV as a `SKIPPED-UNSUPPORTED` row. Nothing is dropped quietly.

The exclusions are not arbitrary:

* **x87 and SSE** are a divergence BY DESIGN, not a bug to be found. The lifter
  computes the x87 stack in C `double` where the hardware uses 80-bit extended
  precision, so `fld`/`fcos`/`ret` lowering to `cos()` on a `double` will never be
  bit-exact. Reporting that as DISAGREE thousands of times would bury real findings.
* **Privileged, BCD and string-port instructions** are almost always data being decoded
  as code. Running them teaches nothing about the lifter.
* **Calls** leave the function under test, so a verdict would be about the callee too.
  This is the one exclusion that is now liftable: with `allow_calls=True` the callee is
  replaced by one synthetic behaviour on BOTH sides, so the comparison stays about the
  caller. See `tools/harness/callstub.py` for the contract, for the three narrower
  reasons that replace the blanket `call` skip, and for what such a verdict does not
  establish. It stays opt-in, because it needs a subject built with the stub shim.
* **Unlifted stack ops** (`pushad`/`popad`/`pushfd`/`popfd`/`enter`) are known no-ops in
  the lifter that silently desync `esp`. They are a KNOWN defect, already enumerated;
  the harness is not the place to rediscover them 148 times.

Widening any of these is a deliberate act, not a default.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import capstone

from .callstub import CalleeConventions, StubPlan, plan_calls
from .jumps import direct_tail_jump as _is_direct_tail_jump
from .jumps import inbody_direct_jump as _is_inbody_direct_jump
from .model import SkippedFunction
from .provenance import SHORT_SHA_CHARS, tree_digest
from .static_tables import StaticJumpTable, prove_static_tables

#: The size window. The floor keeps out one- and two-instruction fragments; the ceiling
#: was 160 and is now 512, which was measured rather than guessed.
#:
#: The ceiling is NOT a performance guard. Oracle cost per case is 0.097 ms mean in the
#: 8-160 band against 0.189 ms in the 256-512 band, executed instruction counts stay three
#: orders of magnitude below the 200,000-instruction budget, and no COUNT-LIMIT fault was
#: observed in any size band across 985 sampled cases. Raising it to 512 costs single-digit
#: seconds across a full sweep.
#:
#: What it is, is subject to brutal diminishing returns, because size is almost never the
#: ONLY reason a function is out of scope. Of the 5,076 functions the 160-byte cap
#: excluded, raising the cap to infinity brings just 58 (1.1%) into scope on its own: the
#: other 98.9% move straight into `call`, `jump`, `sse-mmx`, `partial-decode`,
#: `no-ret-terminator` or `x87`. `partial-decode` eats more of them than the cap yields --
#: 469 at an unlimited cap against 58 admitted -- because a larger body is likelier to
#: contain an embedded jump table, which defeats the linear decode. `memmove` is the
#: documented example: at 672 bytes it reclassifies from `size-out-of-range` straight to
#: `partial-decode`, since capstone walks into the jump-table bytes after each
#: `jmp dword ptr [reg*4+addr]` and the decoded lengths no longer sum to the declared size.
#:
#: 512 is chosen because it captures 53 of those 58 (91%); 1,024 adds 3 more and removing
#: the cap entirely adds 2. The real yield from raising it comes from COMBINING it with
#: call stubbing, which shares the same functions: with `allow_calls` the cap going
#: 160 -> 512 is worth +311 candidates rather than +53.
DEFAULT_MIN_SIZE = 8
DEFAULT_MAX_SIZE = 512

#: Instructions the lifter does not implement and that silently desync the guest stack.
_UNLIFTED_STACK = frozenset(
    {"pushal", "popal", "pusha", "popa", "pushfd", "popfd", "pushf", "popf", "enter"}
)
_PRIVILEGED = frozenset(
    {
        "in",
        "out",
        "insb",
        "insw",
        "insd",
        "outsb",
        "outsw",
        "outsd",
        "hlt",
        "cli",
        "sti",
        "iret",
        "iretd",
        "int",
        "int1",
        "int3",
        "into",
        "lgdt",
        "sgdt",
        "lidt",
        "sidt",
        "lldt",
        "sldt",
        "ltr",
        "str",
        "arpl",
        "lar",
        "lsl",
        "clts",
        "invd",
        "wbinvd",
        "invlpg",
        "rdmsr",
        "wrmsr",
        "rdpmc",
        "rdtsc",
        "cpuid",
        "ud2",
        "ud0",
        "ud1",
        "sysenter",
        "sysexit",
        "verr",
        "verw",
        "smsw",
        "lmsw",
        "bound",
    }
)
_BCD = frozenset({"aaa", "aad", "aam", "aas", "daa", "das", "salc"})
_SEGMENT_LOAD = frozenset({"lds", "les", "lfs", "lgs", "lss"})
_ATOMIC = frozenset({"xchg", "cmpxchg", "cmpxchg8b", "xadd"})
#: SSE/MMX mnemonics whose operands do not name an xmm/mm register, so the operand
#: sniff below would miss them.
_VECTOR_EXTRA = frozenset({"emms", "femms", "ldmxcsr", "stmxcsr"})
#: Barriers. Architecturally no-ops for a single-threaded differential test, but the
#: lifter's handling of them is untested, so they get their own skip reason rather than
#: being hidden inside the SSE count.
_SERIALISING = frozenset({"sfence", "lfence", "mfence"})

_MM_OPERAND = re.compile(r"\b(x?mm\d+)\b")
_RET_MNEMONICS = frozenset({"ret", "retn"})


@dataclass(frozen=True)
class Candidate:
    """A function the harness will execute, with its decoded body for diagnostics."""

    va: int
    size: int
    mnemonics: tuple[str, ...]
    #: Present only for a function admitted with `allow_calls=True`. Carries the
    #: `(callee_va, pop_bytes)` table both sides must stub identically, so the oracle
    #: and the subject cannot each decide for themselves what a callee does.
    stub_plan: StubPlan | None = None
    #: The body uses x87 and was admitted with `allow_x87=True`, so its cases carry an
    #: x87 entry stack (see `seeding.attach_fp_stack`).
    x87: bool = False
    static_tables: tuple[StaticJumpTable, ...] = ()

    @property
    def body_insns(self) -> int:
        return len(self.mnemonics)


@dataclass(frozen=True)
class Selection:
    """The full partition of the input function set. Counts on both sides always add up."""

    candidates: tuple[Candidate, ...]
    skipped: tuple[SkippedFunction, ...]

    @property
    def total(self) -> int:
        return len(self.candidates) + len(self.skipped)

    def skip_histogram(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for item in self.skipped:
            counts[item.reason] = counts.get(item.reason, 0) + 1
        return dict(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))


def disassembler(*, detail: bool = False) -> capstone.Cs:
    """A 32-bit x86 decoder configured the way `classify_function` expects.

    `detail` is required for call stubbing, which resolves branch and call targets
    through capstone's operand model rather than by parsing operand text. It is off by
    default because it costs real time across twelve thousand functions and the
    call-free path never looks at an operand.
    """
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = detail
    return decoder


def instruction_reason(mnemonic: str, operands: str) -> str | None:
    """Why this instruction disqualifies its function, or None if it is in scope."""
    low = mnemonic.lower()
    ops = operands.lower()

    # Any mnemonic beginning with `f` is x87 on x86-32. The fences are `mfence`,
    # `lfence` and `sfence`, so this prefix does not catch them by accident.
    if low in _SERIALISING:
        return "serialising"
    if low.startswith("f"):
        return "x87"
    if _MM_OPERAND.search(ops) or low in _VECTOR_EXTRA:
        return "sse-mmx"
    if low.startswith("call") or low.startswith("lcall"):
        return "call"
    if low in {"jmp", "ljmp"} or low.startswith("jmp ") or low.startswith("ljmp"):
        return "jump"
    if low in {"retf", "lret"}:
        return "far-return"
    if low in _UNLIFTED_STACK:
        return "unlifted-stack-op"
    if low in _PRIVILEGED:
        return "privileged"
    if low in _BCD:
        return "bcd"
    if low in _SEGMENT_LOAD:
        return "segment-load"
    if low in _ATOMIC or low.startswith("lock"):
        return "atomic"
    return None


def classify_function(
    va: int,
    size: int,
    code: bytes,
    decoder: capstone.Cs,
    *,
    min_size: int = DEFAULT_MIN_SIZE,
    max_size: int = DEFAULT_MAX_SIZE,
    conventions: CalleeConventions | None = None,
    allow_inbody_jumps: bool = False,
    allow_x87: bool = False,
    allow_vector: bool = False,
    vector_mode: str = "",
    allow_static_tables: bool = False,
    read_code: Callable[[int, int], bytes] | None = None,
    allow_guarded_jumps: bool = False,
) -> Candidate | SkippedFunction:
    """Decide whether one function is in scope, or why it is not.

    Passing `conventions` enables call stubbing: a `call` stops being an automatic skip
    and the function is admitted only if every call site in it can be stubbed
    identically on both sides. The blanket `call` reason is then replaced by the three
    narrower, separately-counted reasons `callstub.plan_calls` returns.
    """
    if not min_size <= size <= max_size:
        return SkippedFunction(va, size, "size-out-of-range")
    if len(code) != size:
        return SkippedFunction(va, size, "body-unreadable")

    instructions = list(decoder.disasm(code, va))
    if not instructions:
        return SkippedFunction(va, size, "undecodable")
    if sum(i.size for i in instructions) != size:
        # A partial decode means the tail is data, or the bounds are wrong. Either way
        # the oracle would be executing something the subject never saw.
        return SkippedFunction(va, size, "partial-decode")
    if instructions[-1].mnemonic.lower() not in _RET_MNEMONICS:
        # Without a `ret` the oracle has no way to recognise a normal return, and the
        # function is a tail-call or a fallthrough into its neighbour.
        return SkippedFunction(va, size, "no-ret-terminator")

    static_tables: tuple[StaticJumpTable, ...] = ()
    if allow_static_tables and read_code is not None:
        proved = prove_static_tables(va, size, code, read_code)
        if proved is not None:
            static_tables = proved
    if allow_guarded_jumps and read_code is not None:
        # T1576 opt-in (guarded-jump-schema3): the shared guarded prover, tails left to the
        # exact-entry check of the closure/root plan. Never reached by default.
        from .guarded_tables import prove_guarded_tables

        guarded = prove_guarded_tables(va, size, code, read_code, allow_tails=True)
        if guarded is not None:
            static_tables = guarded
    static_sites = {table.site for table in static_tables}

    mnemonics: list[str] = []
    uses_x87 = False
    for insn in instructions:
        text = f"{insn.mnemonic} {insn.op_str}".strip()
        mnemonics.append(text)
        reason = instruction_reason(insn.mnemonic, insn.op_str)
        if allow_vector:
            from .vector import instruction_supported, vector_state_instruction

            if vector_state_instruction(insn):
                if instruction_supported(insn, vector_mode):
                    continue
                return SkippedFunction(va, size, "unsupported-vector-state")
        if (
            reason == "jump"
            and allow_inbody_jumps
            and (_is_inbody_direct_jump(insn, va, size) or _is_direct_tail_jump(insn, va, size))
        ):
            reason = None
        if reason == "x87" and allow_x87:
            # Admitted: the case generator seeds an x87 entry stack and both sides report
            # the exit stack. SSE and MMX stay excluded.
            uses_x87 = True
            continue
        if reason == "jump" and insn.address in static_sites:
            reason = None
        if reason == "call" and conventions is not None:
            # Deferred to plan_calls, which either produces a stub plan or names a
            # narrower reason than "contains a call".
            continue
        if reason is not None:
            return SkippedFunction(va, size, reason)

    if conventions is None:
        return Candidate(
            va=va,
            size=size,
            mnemonics=tuple(mnemonics),
            x87=uses_x87,
            static_tables=static_tables,
        )

    planned = plan_calls(
        va, size, instructions, conventions, allow_tail_branches=allow_inbody_jumps
    )
    if isinstance(planned, str):
        return SkippedFunction(va, size, planned)
    return Candidate(
        va=va,
        size=size,
        mnemonics=tuple(mnemonics),
        stub_plan=planned,
        x87=uses_x87,
        static_tables=static_tables if allow_guarded_jumps else (),
    )


def select(
    functions: list[tuple[int, int]],
    read_code: object,
    *,
    min_size: int = DEFAULT_MIN_SIZE,
    max_size: int = DEFAULT_MAX_SIZE,
    allow_calls: bool = False,
    allow_x87: bool = False,
    suppressed_entries: frozenset[int] = frozenset(),
    vector_entries: frozenset[int] = frozenset(),
    vector_mode: str = "",
    allow_inbody_jumps: bool = False,
    allow_static_tables: bool = False,
    allow_guarded_jumps: bool = False,
    call_functions: list[tuple[int, int]] | None = None,
) -> Selection:
    """Partition `functions` into candidates and skips.

    `read_code` is any callable `(va, size) -> bytes`, which keeps this module free of
    the XBE parser and testable on synthetic byte strings.

    `allow_calls` brings call-bearing functions into scope by stubbing their callees
    identically on both sides. The callee sizes come from `functions` itself, so a call
    to something that is not a known function entry is correctly unstubbable rather than
    quietly assumed to pop nothing.

    `allow_x87` admits x87 bodies. Only a hand-written replacement run uses it: the
    subject is then native C and the question is whether it matches the hardware.

    `call_functions` supplies known callee bounds independently of the selected root
    catalogue. Explicit live closure uses this to retain verified thunk signatures
    without admitting those thunks as roots. Ordinary selection keeps its old catalogue.

    `vector_entries` explicitly opts individual roots into the same fail-closed
    movement/register-bitwise whitelist as live-call closure inspection. It does
    not broaden ordinary selection or permit unsupported descendants.

    `suppressed_entries` must come from verified subject-tree evidence. Only those entry
    bodies are skipped; their signatures remain available for planning calls from callers.
    """
    reader = read_code
    assert callable(reader)
    decoder = disassembler(detail=allow_calls or bool(vector_entries))
    conventions = (
        CalleeConventions(
            dict(functions if call_functions is None else call_functions),
            reader,
            disassembler(detail=True),
        )
        if allow_calls
        else None
    )
    candidates: list[Candidate] = []
    skipped: list[SkippedFunction] = []
    for va, size in functions:
        if va in suppressed_entries:
            skipped.append(SkippedFunction(va, size, "manual-body-suppressed"))
            continue
        verdict = classify_function(
            va,
            size,
            reader(va, size),
            decoder,
            min_size=min_size,
            max_size=max_size,
            conventions=conventions,
            allow_inbody_jumps=allow_inbody_jumps,
            allow_x87=allow_x87,
            allow_vector=va in vector_entries,
            vector_mode=vector_mode,
            allow_static_tables=allow_static_tables,
            read_code=reader,
            allow_guarded_jumps=allow_guarded_jumps,
        )
        if isinstance(verdict, Candidate):
            candidates.append(verdict)
        else:
            skipped.append(verdict)
    return Selection(candidates=tuple(candidates), skipped=tuple(skipped))


def suppressed_manual_entries(gen_dir: Path | str, subject_tree_sha: str) -> frozenset[int]:
    """Read exact host-dispatch/stop trampolines from the measured subject tree.

    This is entry-body scope only. Calls to these addresses remain eligible for symmetric
    call stubbing. Never infer suppression from a section, a requested manual list, or ABORT.
    A changed/unverified tree cannot provide exclusions for an already-built subject.
    """
    digest = tree_digest(gen_dir)
    if len(subject_tree_sha) not in {SHORT_SHA_CHARS, len(digest)} or not digest.startswith(
        subject_tree_sha
    ):
        raise ValueError("subject tree digest does not match manual-body evidence")
    source = Path(gen_dir) / "recomp_xdk_manual.c"
    if not source.exists():
        return frozenset()
    # Preserve quoted metadata while discarding comments: reasons may contain
    # literal // or /*, which cannot be treated as C comment delimiters.
    token = re.compile(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*', re.DOTALL)
    code = token.sub(
        lambda match: match.group() if match.group().startswith('"') else " ",
        source.read_text(),
    )
    literal = r'"(?:[\x20-\x21\x23-\x5b\x5d-\x7e]|\\["\\?])+"'
    pattern = re.compile(
        r"^\s*void\s+sub_([0-9A-Fa-f]{8})\(void\)\s*\{\s*"
        r"(?:xdk_thunk_dispatch_at\(0x([0-9A-Fa-f]{8})u\)"
        r"|xdk_thunk_stop_at\(0x([0-9A-Fa-f]{8})u,\s*(" + literal + r"),\s*(" + literal + r")\))"
        r";\s*\}",
        re.MULTILINE,
    )
    addresses: set[int] = set()
    for symbol, dispatch, stop, label, reason in pattern.findall(code):
        if int(symbol, 16) != int(dispatch or stop, 16):
            continue
        if stop:
            address = int(stop, 16)
            if address == 0 or 0xFE000000 <= address < 0xFE001000:
                continue
            # Metadata bounds match the generator, including escaped quote/backslash.
            decoded = [re.sub(r'\\(["\\?])', r"\1", value[1:-1]) for value in (label, reason)]
            if not (0 < len(decoded[0]) < 128 and 0 < len(decoded[1]) < 256):
                continue
        addresses.add(int(symbol, 16))
    return frozenset(addresses)
