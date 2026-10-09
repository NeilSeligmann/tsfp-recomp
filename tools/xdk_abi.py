# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the calling convention and arity of each XDK surface ADDRESS.

WHY THIS EXISTS, AND WHY `tools/lift/callsites.py` CANNOT DO IT. That module is
ORDINAL-keyed. It finds a call site by recognising a read of the XBE's kernel
thunk table and mapping the slot to an ordinal. XDK library functions are not
imported: D3D8, DSOUND, XAPILIB and the rest are statically linked into the title
at known virtual addresses, so there is no thunk slot, no ordinal, and nothing for
that module to key on. Pointed at this problem it reports zero sites, not a wrong
answer. `src/host/xdk_thunk.c` consequently refuses all 236 measured addresses for
want of a convention, and that refusal is what this module exists to lift.

THE PRIMARY ROUTE IS THE CALLEE'S OWN TERMINATING INSTRUCTION, NOT A PUSH COUNT.
`ret` pops the return address and nothing else. `ret imm16` pops the return
address and `imm16` further bytes. That single byte pattern IS the cleanup
contract, it is read out of the user's own binary at the function's address, and
it is what the dispatcher needs: `pop_bytes` is `4 + imm16` and no inference
stands between the binary and that number.

That is a categorically stronger measurement than counting argument pushes, for
three reasons worth stating because the push count is the obvious thing to reach
for:

1. It is PER-FUNCTION. A push count is per-SITE, so it needs an estimator over
   sites, and `src/host/kernel_thunk.c` records what that cost on the kernel side:
   ordinal 219's per-site tallies were `{8,8,8,8,6,10}` and the minimum estimator
   picked the 6, which was the one wrong number in the set.
2. It is the CALLEE'S OWN STATEMENT of how much it pops. A push count is the
   caller's statement about how much it pushed, which coincides with the callee's
   cleanup only if the caller is correct and the lifter's bracket around it is
   correctly placed. Ordinal 219 failed on the second of those.
3. It cannot be inflated by a register save. The whole one-sided error that forces
   the minimum estimator in `callsites.py` -- a `push esi` landing inside the
   lifter's call-site bracket -- has no analogue here.

WHAT `ret` WITH NO IMMEDIATE DOES AND DOES NOT SETTLE. It settles `pop_bytes`
exactly: four, the return address alone. It does NOT settle the argument count,
because a `__cdecl` function with six stack arguments and a `__stdcall` function
with none both end in a bare `ret`. The distinction is invisible in the callee and
has to come from the CALLER, which is what `caller_cleanup_at_sites` is for: a
`__cdecl` caller must undo its own pushes, so it carries an `add esp, N` after the
call and a callee-cleanup caller cannot. That is the same test
`src/host/kernel_thunk.c` used to settle ordinal 277 by hand.

NOTE THE ASYMMETRY THIS CREATES, BECAUSE IT DECIDES WHERE THE RISK LIVES. For a
`ret imm16` row a wrong number desynchronises `esp` permanently. For a bare-`ret`
row the pop is four whatever the argument count turns out to be, so a wrong
argument count makes a handler misread its arguments and CANNOT desynchronise
`esp`. The two kinds of row are therefore not equally dangerous and this module
reports them separately rather than pooling them into one accuracy figure.

THE ESTIMATOR, DELIBERATELY. There is no estimator over call sites for
`stack_args`, because there is nothing to estimate: `ret imm16` is read once per
function. Where a function has SEVERAL `ret` instructions this module requires
them to be UNANIMOUS and REFUSES on conflict. Not the minimum, not the majority.
The reason is that the error models differ in kind. A per-site push count errs in
one direction, which is what justifies a minimum. A disagreement between two `ret`
instructions means the control-flow walk pulled in code belonging to a NEIGHBOURING
function, i.e. the extent is wrong, and a wrong extent is not biased in a knowable
direction -- it invalidates the row rather than skewing it. Collapsing it to a
number would launder an extent bug into an arity.

Push counts are still measured, and are reported as the FULL per-site
distribution, never collapsed to one number. A corroborator that collapses throws
away exactly the information that caught ordinal 219.

HOW THE EXTENT IS FOUND, AND WHY NOT FROM A FUNCTION DATABASE. By walking control
flow forward from the entry address: fall-through, both arms of every direct
conditional branch, and through direct unconditional jumps. Calls are not followed;
execution returns from them. This is deliberately independent of
`generated/retail/functions.csv` and of the lifter's own `functions.json`, so that
neither Ghidra's nor the lifter's idea of where a function ends can put a number
into this table. 11 of the 236 addresses are absent from `functions.csv` anyway.

WHY THE CAPSTONE RESYNC TRAP DOES NOT APPLY HERE, AND WHAT REPLACES IT.
`capstone.disasm` stops silently at the first undecodable byte and returns a short
list; an earlier scan in this tree reported ZERO call sites because of it and
`.text` alone needs 132 resynchronisations. That trap is specific to a LINEAR
sweep, which decodes bytes that are not instructions. A control-flow walk only ever
decodes an address that control flow reaches, so every decode starts on a genuine
instruction boundary and there is nothing to resynchronise onto. The failure mode
is replaced rather than removed: a walk can stop EARLY, so every decode is
performed one instruction at a time with `count=1`, an undecodable address is
recorded in `FunctionWalk.undecodable` rather than ending the walk silently, and
`unresolved_jumps` records every indirect jump the walk could not follow. A row
with either is reported, never quietly accepted.

REGISTER READS ARE NOT AUTOMATIC ARGUMENTS. MSVC also uses `push ecx` to
reserve a local or outgoing float slot. A bounded stack-slot walk suppresses that
read only when every reached path overwrites the value before consuming it.
An unresolved entry value whose only use is a push refuses the convention rather
than declaring `this`; a genuine register read still establishes the register
argument. Ret immediates settle cleanup independently of this distinction.

The call-site scan DOES sweep linearly, and it uses
`tools.codediff.normalise.normalise_text`, which resynchronises.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from hashlib import sha256
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

from tools.codediff.normalise import MAX_INSN_BYTES, normalise_text
from tools.xbe.parser import parse_xbe

#: XBE section header flag: this section is executable.
SECTION_EXECUTABLE = 0x4

#: One row of the generated surface table, as `tools/gen_d3d8_surface.py` writes it.
_SURFACE_ROW = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*\"(\w+)\"\s*,\s*(?:NULL|\"([^\"]*)\")\s*,\s*(\d+)\s*\}"
)

#: Mnemonics that return to the caller, i.e. terminate the callee and perform its
#: half of the stack cleanup. `retf` is included so that an unexpected far return
#: is CLASSIFIED rather than mistaken for a fall-through off the end of a function.
RETURN_MNEMONICS = frozenset({"ret", "retn", "retf"})

#: Mnemonics after which control does not reach the next instruction and there is
#: nothing further for the walk to do on this path.
HALT_MNEMONICS = frozenset({"hlt", "ud2", "int3"})

#: Hard ceiling on instructions decoded per function. A walk that needs more than
#: this has almost certainly escaped its function through a mis-decoded branch, and
#: an unbounded walk would silently absorb a whole section.
WALK_INSN_LIMIT = 20000

#: How far back from a call instruction the push scan looks. Argument setup for the
#: widest thing in either boundary measured so far is well inside this.
PUSH_SCAN_BYTES = 256

#: Mnemonics which, immediately after an `add esp, N`, prove that the `add` was the
#: caller's own FRAME TEARDOWN and not argument cleanup.
#:
#: THIS IS NOT A REFINEMENT, IT IS A CORRECTION OF A WRONG MEASUREMENT. The first
#: version of this module read any `add esp, N` after a call as caller cleanup, and
#: at `0x003D57D0` -- the busiest surface address in the image, whose callee ends in
#: `ret 4` -- that reported 8 of 94 sites as caller-cleaning, i.e. a contradiction
#: with the callee's own instruction. Every one of the 8 is `call X; add esp, N; ret`
#: with N of 0x10, 0x40, 0x48, 0x50 and 0x80 at different sites. Those are the
#: caller's FRAME sizes, which is why they vary wildly and why none of them is a
#: plausible argument count. A tail-position call followed by the epilogue looks
#: exactly like cdecl cleanup unless what follows the `add` is examined.
EPILOGUE_FOLLOWERS = frozenset({"ret", "retn", "retf", "pop", "leave", "jmp", "ljmp"})

#: Every capstone name for a part of each 32-bit register. A write to `cl` must be
#: recognised as touching `ecx`, or a partial write reads as leaving it untouched.
REGISTER_FAMILY: dict[str, str] = {
    name: parent
    for parent, names in {
        "eax": ("eax", "ax", "al", "ah"),
        "ebx": ("ebx", "bx", "bl", "bh"),
        "ecx": ("ecx", "cx", "cl", "ch"),
        "edx": ("edx", "dx", "dl", "dh"),
        "esi": ("esi", "si", "sil"),
        "edi": ("edi", "di", "dil"),
        "ebp": ("ebp", "bp", "bpl"),
        "esp": ("esp", "sp", "spl"),
    }.items()
    for name in names
}

#: Registers a called function is free to destroy under every convention in play, so
#: a `call` kills them for the purpose of liveness. Without this an `ecx` read AFTER
#: a call reads as evidence that `ecx` arrived as an argument, which it is not.
CALL_CLOBBERED = ("eax", "ecx", "edx")

#: The two registers a convention can pass an argument in, in order.
ARGUMENT_REGISTERS = ("ecx", "edx")

#: Conventions, named to match the `xdk_cc` enum in `src/host/xdk_thunk.h`.
CC_STDCALL = "XDK_CC_STDCALL"
CC_FASTCALL = "XDK_CC_FASTCALL"
CC_THISCALL = "XDK_CC_THISCALL"
CC_CDECL = "XDK_CC_CDECL"

#: Mirrors `XDK_ABI_MAX_STACK_ARGS` in `src/host/xdk_thunk.h`. A row above it would
#: be refused by the host anyway, so it is refused here where the reason is visible.
MAX_STACK_ARGS = 16

#: Mirrors `XDK_MEASURED_ARITY_MIN_SITES`. Only the push-count corroboration is a
#: vote, so this gates corroboration strength and never `stack_args` itself.
MEASURED_ARITY_MIN_SITES = 3

#: Stack-argument counts known INDEPENDENTLY of anything this module measures, used
#: to validate the measurement rather than to supply answers. Exactly the role
#: `KNOWN_STACK_ARGS` plays in `tools/lift/callsites.py`, and `emit_c` refuses to
#: write a table that cannot reproduce them -- because a measurement that fails its
#: own check is worse than none: the host stops cleanly without a table and
#: desynchronises `esp` silently with a wrong one.
#:
#: WHERE THESE NUMBERS COME FROM, BECAUSE PROVENANCE MATTERS HERE. They are the
#: BSD-sockets / Winsock API, whose signatures are public, documented by Microsoft
#: outside any XDK, and identical on every platform that implements sockets.
#: `recvfrom` has taken six arguments since 4.2BSD. NOTHING here is derived from a
#: leaked `.lib` archive, a FLIRT database or any XDK header, and `docs/provenance.md`
#: is not engaged by it. The keys are matched against the IMAGE'S OWN `.XTLID` names,
#: which is the image describing itself.
#:
#: NO XDK-PRIVATE NAME APPEARS HERE OR IN ANY OUTPUT THIS MODULE WRITES. A convention
#: is derived from the binary and is not a name.
KNOWN_STACK_ARGS: dict[str, int] = {
    "accept": 3,
    "bind": 3,
    "closesocket": 1,
    "connect": 3,
    "getpeername": 3,
    "getsockname": 3,
    "getsockopt": 5,
    "htonl": 1,
    "htons": 1,
    "inet_addr": 1,
    "ioctlsocket": 3,
    "listen": 2,
    "ntohl": 1,
    "ntohs": 1,
    "recv": 4,
    "recvfrom": 6,
    "select": 5,
    "send": 4,
    "sendto": 6,
    "setsockopt": 5,
    "shutdown": 2,
    "socket": 3,
    "WSACleanup": 0,
    "WSACloseEvent": 1,
    "WSACreateEvent": 0,
    "WSAGetLastError": 0,
    "WSASetLastError": 1,
}

#: Fewest independently-known arities that must be reproduced before `emit_c` will
#: write anything. Nine is what `tools/lift/callsites.py` manages on the kernel side,
#: and this module reaches twenty on this image, so the bar is set where a real
#: regression in coverage would trip it rather than where today's number happens to be.
MIN_VALIDATED = 9


@dataclass(frozen=True)
class Surface:
    """One row of the measured XDK surface.

    `name` is carried for diagnostics only and is deliberately never written to any
    output this module emits. 69 of the 80 D3D names are NULL because the clean-room
    boundary is working, the rest came from the image's own `.XTLID`, and
    `docs/provenance.md` governs where a name may travel. A convention is derived
    from the binary and is not a name, so nothing here needs one.
    """

    address: int
    section: str
    name: str | None
    sites: int


@dataclass(frozen=True)
class DecodedInsn:
    """One instruction of a function body, with the register facts liveness needs."""

    va: int
    size: int
    mnemonic: str
    op_str: str
    #: 32-bit register names this instruction reads, sub-register names folded in.
    reads: frozenset[str]
    #: 32-bit register names this instruction overwrites, in whole or in part.
    writes: frozenset[str]
    #: Registers this instruction writes only PART of, e.g. `mov ch, al`. Recorded
    #: because liveness here is register-granular rather than byte-granular, so a
    #: partial write is treated as a kill and that is an approximation worth naming.
    partial_writes: frozenset[str]
    #: Successor VAs reached by control flow from here, excluding call returns.
    successors: tuple[int, ...]
    #: True when control also reaches the following instruction.
    falls_through: bool
    #: True when this instruction calls, so liveness can kill the volatile registers.
    is_call: bool


@dataclass
class FunctionWalk:
    """What a control-flow walk from one entry address found.

    Every field that can make the row untrustworthy is recorded rather than folded
    into a verdict, so that `classify` refuses on the evidence instead of on a flag
    somebody has to remember to set.
    """

    entry: int
    section: str
    #: VA of each terminating instruction -> the immediate it pops, 0 for a bare `ret`.
    returns: dict[int, int] = field(default_factory=dict)
    #: VAs of indirect jumps the walk could not follow. A jump table lands here.
    unresolved_jumps: tuple[int, ...] = ()
    #: (VA, target) for each direct jump leaving the entry's section.
    external_jumps: tuple[tuple[int, int], ...] = ()
    #: Addresses where decoding failed. Never empty silently: see the module docstring.
    undecodable: tuple[int, ...] = ()
    #: Every instruction decoded, ascending by VA.
    insns: tuple[DecodedInsn, ...] = ()
    #: True when the walk hit WALK_INSN_LIMIT, which invalidates it.
    truncated: bool = False
    #: VAs of direct unconditional `jmp` instructions the walk followed through.
    #: Ordinary intra-function control flow: a `jmp` to a shared epilogue is one.
    followed_jumps: tuple[tuple[int, int], ...] = ()
    #: Target of the entry instruction when the ENTRY ITSELF is an unconditional
    #: jump, i.e. the surface address is a one-instruction jump stub and the
    #: terminating instruction found belongs to the target.
    #:
    #: Distinguished from `followed_jumps` because conflating the two was a reporting
    #: BUG: 0x003D57D0 is an ordinary aligned function with two internal `jmp`s, and
    #: the first version of this module annotated it "entered through 2 direct
    #: jump(s)", which is false and would have sent a reader looking for a stub that
    #: is not there.
    entry_jump_target: int | None = None

    @property
    def return_immediates(self) -> frozenset[int]:
        return frozenset(self.returns.values())

    @property
    def unanimous(self) -> bool:
        """True when every terminating instruction pops the same number of bytes."""
        return len(self.return_immediates) == 1

    @property
    def clean(self) -> bool:
        """True when nothing about the walk itself undermines what it found."""
        return not self.undecodable and not self.truncated


def _decoder() -> capstone.Cs:
    """A detail-enabled 32-bit x86 decoder. Built per call: no shared state."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def executable_sections(xbe_path: Path) -> list[tuple[str, int, bytes]]:
    """(name, base VA, bytes) for every executable section of an XBE.

    All of them. The 236 surface addresses live in seven library sections and not one
    of them is `.text`, so a scan of `.text` finds nothing at all here.
    """
    raw = xbe_path.read_bytes()
    xbe = parse_xbe(raw)
    out: list[tuple[str, int, bytes]] = []
    for section in xbe.sections:
        if not section.flags & SECTION_EXECUTABLE:
            continue
        body = raw[section.raw_addr : section.raw_addr + section.raw_size]
        if body:
            out.append((section.name, section.virtual_addr, body))
    return out


class SectionMap:
    """Executable sections, addressable by VA."""

    def __init__(self, sections: Iterable[tuple[str, int, bytes]]) -> None:
        self._sections = sorted(sections, key=lambda item: item[1])

    def __iter__(self) -> Iterable[tuple[str, int, bytes]]:
        return iter(self._sections)

    def find(self, va: int) -> tuple[str, int, bytes] | None:
        """The section containing `va`, or None."""
        for name, base, data in self._sections:
            if base <= va < base + len(data):
                return name, base, data
        return None

    def bounds(self) -> tuple[int, int]:
        """Lowest and highest VA covered, half-open. Used to mask absolute operands."""
        if not self._sections:
            return 0, 0
        lo = self._sections[0][1]
        hi = max(base + len(data) for _, base, data in self._sections)
        return lo, hi


def read_surface(path: Path) -> list[Surface]:
    """Parse the generated surface table.

    Reads the C rather than regenerating, because the generator reads the user's XBE
    and the table is the committed-by-nobody record of what it found. A row whose
    address is not 32-bit is a parse failure rather than a skip: silently dropping
    rows would shrink the denominator this whole report is a fraction of.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    rows: list[Surface] = []
    for match in _SURFACE_ROW.finditer(text):
        address = int(match.group(1), 16)
        if not 0 <= address < 1 << 32:
            raise ValueError(f"surface address out of range: {match.group(0)}")
        rows.append(
            Surface(
                address=address,
                section=match.group(2),
                name=match.group(3),
                sites=int(match.group(4)),
            )
        )
    if not rows:
        raise ValueError(f"no surface rows parsed from {path}; the table shape changed")
    return rows


def _register_access(
    insn: capstone.CsInsn,
) -> tuple[frozenset[str], frozenset[str], frozenset[str]]:
    """32-bit registers this instruction reads, writes, and writes only part of.

    A SUB-REGISTER WRITE IS A KILL, and the first version of this module had it the
    other way round. Treating `mov ch, al` as also READING `ecx` -- on the reasoning
    that it leaves the upper bits alive -- MANUFACTURED REGISTER ARGUMENTS. It made
    `ntohs` at 0x00432700 report `ecx` live-in and classify as THISCALL, when the
    function's whole body is

        mov ax, word ptr [esp + 4]   ; the one argument, off the STACK
        mov ch, al
        mov cl, ah
        mov ax, cx
        ret 4

    where `ecx` is scratch being built up, not an incoming value. The same false
    positive put a spurious `edx` argument on 0x003D7220 via `mov dl, byte ptr [...]`.
    A caller passing a 32-bit argument in a register does not expect the callee to
    overwrite half of it before reading the whole, so a kill is the right model for
    ARGUMENT detection. A genuine byte-wide argument in `cl` is still caught, because
    a sub-register READ is folded into `reads` above.

    `xor ecx, ecx` reads and writes `ecx` by capstone's account, and is special-cased
    to a pure write: the idiom is the standard way to prove a register is NOT an
    incoming argument, and counting its read would hide every such proof.
    """
    read_ids, write_ids = insn.regs_access()
    reads: set[str] = set()
    writes: set[str] = set()
    partial: set[str] = set()
    for reg in read_ids:
        parent = REGISTER_FAMILY.get(insn.reg_name(reg))
        if parent:
            reads.add(parent)
    for reg in write_ids:
        name = insn.reg_name(reg)
        parent = REGISTER_FAMILY.get(name)
        if not parent:
            continue
        writes.add(parent)
        if name != parent:
            partial.add(parent)

    if insn.mnemonic in ("xor", "sub") and len(insn.operands) == 2:
        first, second = insn.operands
        if (
            first.type == cs_x86.X86_OP_REG
            and second.type == cs_x86.X86_OP_REG
            and first.value.reg == second.value.reg
        ):
            zeroed = REGISTER_FAMILY.get(insn.reg_name(first.value.reg))
            if zeroed:
                reads.discard(zeroed)
                writes.add(zeroed)
                partial.discard(zeroed)
    return frozenset(reads), frozenset(writes), frozenset(partial)


def _control_flow(insn: capstone.CsInsn) -> tuple[tuple[int, ...], bool, bool]:
    """(successor VAs, falls through, is a call) for one instruction.

    A `call` falls through: it returns. Only DIRECT branch targets are reported, so
    an indirect jump produces no successors and the caller records it as unresolved
    rather than guessing.
    """
    mnemonic = insn.mnemonic
    if mnemonic in RETURN_MNEMONICS or mnemonic in HALT_MNEMONICS:
        return (), False, False
    if mnemonic == "call":
        return (), True, True
    is_jump = capstone.CS_GRP_JUMP in insn.groups
    if not is_jump:
        return (), True, False
    target = (
        insn.operands[0].value.imm
        if insn.operands and insn.operands[0].type == cs_x86.X86_OP_IMM
        else None
    )
    unconditional = mnemonic in ("jmp", "ljmp")
    if target is None:
        return (), not unconditional, False
    return (target,), not unconditional, False


def walk_function(sections: SectionMap, entry: int) -> FunctionWalk:
    """Decode a function by following control flow from `entry`.

    Follows fall-through, both arms of every direct conditional branch, and direct
    unconditional jumps. Does NOT follow calls. Does NOT follow a jump that leaves
    the entry's section, which is recorded as an external jump instead, because
    crossing a section boundary means the target belongs to a different library and
    its cleanup is its own.

    A direct `jmp` WITHIN the section is followed, which is what makes the 93
    unaligned surface addresses work: those are one-instruction jump stubs, and the
    bytes the caller's return address is popped by live in the target. A stub does
    not push anything of its own, so the target's `ret imm16` is the stub's cleanup
    contract unchanged.
    """
    located = sections.find(entry)
    if located is None:
        return FunctionWalk(entry=entry, section="", truncated=False)
    section_name, base, data = located

    decoder = _decoder()
    seen: dict[int, DecodedInsn] = {}
    pending = [entry]
    returns: dict[int, int] = {}
    unresolved: list[int] = []
    external: list[tuple[int, int]] = []
    undecodable: list[int] = []
    followed: list[tuple[int, int]] = []
    truncated = False
    entry_jump: int | None = None

    while pending:
        va = pending.pop()
        while True:
            if va in seen:
                break
            if len(seen) >= WALK_INSN_LIMIT:
                truncated = True
                break
            if not base <= va < base + len(data):
                external.append((va, va))
                break
            offset = va - base
            raw = next(decoder.disasm(data[offset : offset + MAX_INSN_BYTES], va, count=1), None)
            if raw is None:
                # Recorded, never silent. See the module docstring on the resync trap.
                undecodable.append(va)
                break

            reads, writes, partial = _register_access(raw)
            successors, falls_through, is_call = _control_flow(raw)
            decoded = DecodedInsn(
                va=va,
                size=raw.size,
                mnemonic=raw.mnemonic,
                op_str=raw.op_str,
                reads=reads,
                writes=writes,
                partial_writes=partial,
                successors=successors,
                falls_through=falls_through,
                is_call=is_call,
            )
            seen[va] = decoded

            if raw.mnemonic in RETURN_MNEMONICS:
                immediate = (
                    raw.operands[0].value.imm
                    if raw.operands and raw.operands[0].type == cs_x86.X86_OP_IMM
                    else 0
                )
                returns[va] = immediate
                break
            if raw.mnemonic in HALT_MNEMONICS:
                break

            is_jump = capstone.CS_GRP_JUMP in raw.groups
            unconditional = raw.mnemonic in ("jmp", "ljmp")
            if va == entry and unconditional and successors:
                entry_jump = successors[0]
            if is_jump and not successors:
                # An indirect jump: a jump table, or a tail call through a register.
                unresolved.append(va)
                if unconditional:
                    break
            # A jump out of the section is followed ONLY when it is the entry
            # instruction itself, i.e. a one-instruction stub. 0x00431D53 and
            # 0x00431D65 are XNET stubs jumping into the game's own `.text`, where
            # the linker placed the real `WSAGetLastError` and `WSACloseEvent`, and
            # the stub pushes nothing of its own, so the target's `ret` IS the stub's
            # cleanup contract. Anywhere else a cross-section jump is a tail call to
            # another library and that library's cleanup is its own.
            crossing = entry_jump is not None and va == entry
            for target in successors:
                inside_section = base <= target < base + len(data)
                if not inside_section and not crossing:
                    external.append((va, target))
                    continue
                if unconditional:
                    followed.append((va, target))
                    continue
                pending.append(target)
            if unconditional:
                reachable = [t for t in successors if crossing or base <= t < base + len(data)]
                if not reachable:
                    break
                va = reachable[0]
                if not base <= va < base + len(data):
                    relocated = sections.find(va)
                    if relocated is None:
                        external.append((entry, va))
                        break
                    section_name, base, data = relocated
                continue
            if not falls_through:
                break
            va = va + raw.size

    return FunctionWalk(
        entry=entry,
        section=section_name,
        returns=dict(sorted(returns.items())),
        unresolved_jumps=tuple(sorted(set(unresolved))),
        external_jumps=tuple(sorted(set(external))),
        undecodable=tuple(sorted(set(undecodable))),
        insns=tuple(sorted(seen.values(), key=lambda item: item.va)),
        truncated=truncated,
        followed_jumps=tuple(sorted(set(followed))),
        entry_jump_target=entry_jump,
    )


# T109 hand proofs, tied to exact original caller AND output callee bytes.
#
# 0x409635 saves ECX to [ebp-4], passes its address to 0x4095C8 (arg1), and
# reads it at0x409652 only if the returned status is nonnegative. The callee
# only writes the pointee (0x4095DE or0x409620), never reads it. Its failure
# paths may leave it untouched, but the caller does not consume it on failure.
#
# 0x444F71 saves ECX to [ebp-4], passes its address to0x37EA14 (arg3), and
# never reads it afterward. That callee loads the pointer at0x37EA5C and writes
# [ECX] at0x37EA60; it never reads the pointee. The saved entry ECX therefore
# cannot affect either caller's observable result. ECX live-in is a reservation,
# not `this`. These are narrow hand proofs, not a generic out-pointer heuristic.
#
# Fingerprints include every byte through the final reached ret, including all
# conditional arms. Changes to either body invalidate the proof and restore the
# conservative refusal; no caller-name or address-only exception is accepted.
_OUTPUT_SLOT_PROOFS: dict[int, tuple[int, tuple[tuple[int, int, str], ...]]] = {
    0x00409635: (
        0x00409638,
        (
            (0x00409635, 71, "5fd80999f9aaaba2a98b715fb5e44003841d86d320cdb871cdadbf61b63e1858"),
            (0x004095C8, 109, "e3f137d5607c926d154e21e026ce43d256e6903a835b54651269cf778801dc79"),
        ),
    ),
    0x00444F71: (
        0x00444F74,
        (
            (0x00444F71, 228, "2f4538a33707d4b132e8c4c378957e8fafaea5000d7891f692dad672e48392a2"),
            (0x0037EA14, 101, "3d1f9d7c3d7bc741daf5f314f7c61dccbb74ed8f0fabc9de8805fb0421e3680d"),
        ),
    ),
}


# T146 zero-argument hand proof. The wrapper saves/restores ESI, calls the
# FS:[0x24] reader, and uses only its AL result to choose a Leave call with the
# fixed CS address 0x4124B4. The reader consumes no incoming stack/register value:
# its passive arm passes that same constant to Enter. The table helper contains
# eleven absolute stores and ret, with no incoming argument reads. Complete
# three-body fingerprints cover both FS branches and the wrapper's sole ret.
# This is deliberately independent of the one caller's push/cleanup vote.
_ZERO_ARGUMENT_PROOFS: dict[int, tuple[tuple[int, int, str, tuple[int, ...]], ...]] = {
    0x00406AB6: (
        (
            0x00406AB6,
            31,
            "c780906eb27a71514b8f41a9bf4c8b839baf564b33e6618577bf09c68e766b53",
            (0x00406AD4,),
        ),
        (
            0x004069FE,
            30,
            "4450e54bc67fd406b06afb8292d1ba89d8686a6d7af1bdf7d94bfb18ebae3c9b",
            (0x00406A0C, 0x00406A1B),
        ),
        (
            0x00409FC2,
            111,
            "bc0b3a5c7be672f9c75dbf81ef7da624a754fb0e37d3d2900d9c7661601ad0d2",
            (0x0040A030,),
        ),
    ),
}


# T368 zero-argument hand proof for 0x003D3630, the InsertFence wrapper `push 0; call 0x003D67B0;
# ret`. All eight bytes are fingerprinted and the body reads no incoming stack or register value:
# its only stack write is the constant it pushes for the callee. The title's single caller
# (0x00018E20) pushes nothing before and cleans up nothing after the call, but one voter always
# agrees with itself, so the proof does not rest on it: whatever a caller pushed, the wrapper
# never reads it.
_ZERO_ARGUMENT_PROOFS[0x003D3630] = (
    (
        0x003D3630,
        8,
        "d203b43fd82a14e13266400cdc3be43bc19348ac958e44c8ed7c56faa3c28f7c",
        (0x003D3637,),
    ),
)


# T393 zero-argument hand proof for 0x003D97F0, D3DDevice_GetOverlayUpdateStatus. The 29 byte
# body loads the device global, compares device+0x2410 with device+0x1DE8, returns the setne
# result and reads no incoming stack or register value (esi is saved and restored around its
# own use). Its single caller (the XMV movie loop at 0x00030280) pushes nothing, and one voter
# always agrees with itself, so the proof rests on the fingerprinted body.
_ZERO_ARGUMENT_PROOFS[0x003D97F0] = (
    (
        0x003D97F0,
        29,
        "47a0dd1406ed38fc61e89f5a9fe4fb9440e6377caf666089cbda903efd53357f",
        (0x003D980C,),
    ),
)


# T549 zero-argument hand proof for 0x003D34A0, D3DDevice_BlockUntilIdle. The entry is a five
# byte `jmp 0x003D6B80` stub whose target is `mov eax,[0x3E3F58]; mov ecx,[eax+0x2C]; push 2;
# push ecx; call 0x003D6870; ret`: the device global and its fence word are the only inputs, no
# incoming stack or register value is read, and the callee it pushes two arguments for returns
# with `ret 8` at both exits (checked when the proof is applied). Both bodies are fingerprinted.
# The two callers (0x00023237 and a tail jump at 0x00023269) push nothing, but one voter always
# agrees with itself, so the proof rests on the target's bytes. The stub is the walk entry and
# its target the second item.
_STUB_ENTRY_PROOFS = frozenset({0x003D34A0})
_ZERO_ARGUMENT_PROOFS[0x003D34A0] = (
    (
        0x003D34A0,
        5,
        "ef5a6870891dd42ac485797ea71474e8418a719d319e2f35afb6ea4c9e360d52",
        (0x003D6B90,),
    ),
    (
        0x003D6B80,
        17,
        "8b94637cf7e89cbb042a1692ff07c0a6c34514a07044bbbd99a65a7a1a153401",
        (0x003D6B90,),
    ),
)


# T904: the 111-byte XOnlineCleanup body reads no incoming argument and ends in a bare ret.
# Its two title callers do not push stack arguments, fewer than the three independent caller
# votes required by the automatic path, so the complete body fingerprint establishes arity 0.
_ZERO_ARGUMENT_PROOFS[0x00413593] = (
    (
        0x00413593,
        111,
        "06e675f7bb9836d09a5f222cda93ccfc69cda6a0a6d2eac9a0bfa71ef2b5c731",
        (0x00413601,),
    ),
)


def _verified_fixed_vertex_upload(walk: FunctionWalk, sections: SectionMap | None) -> bool:
    """T539 complete fixed-upload body and both register-only caller witnesses.

    The body reads ECX/EDX, never an incoming stack slot; its only pushes save
    those registers around the no-argument refill. Both callers set ECX/EDX and
    neither pushes arguments nor cleans them up. Bare RET/site count alone is
    insufficient; any changed witness byte invalidates this finite proof.
    """
    if (
        walk.entry != 0x003D5720
        or sections is None
        or walk.returns != {0x003D57B3: 0}
        or live_in_registers(walk) != {"ecx": True, "edx": True}
    ):
        return False
    for va, size, digest in (
        (0x003D5720, 162, "8db3d23ff1321379ef0dad1db2e2b4ce800848f2e48e7a5ca623fe2f051ce1a5"),
        (0x003D6B20, 15, "b1ef85f0be222a94789503370115f70dda8128817a06cffb5bd65841827aef98"),
        (0x000230D8, 58, "483628ed443fe2945e5c70401555f5e3c888dbc0e789766351e10164b1e62584"),
        (0x0002D15F, 255, "afcfde5c56e59396b7fa81a2e44ce89a327e59fefe9735bd1b63c9cc590aec9f"),
    ):
        region = sections.find(va)
        if region is None:
            return False
        _, base, body = region
        actual = body[va - base : va - base + size]
        if len(actual) != size or sha256(actual).hexdigest() != digest:
            return False
    refill = walk_function(sections, 0x003D69E0)
    return (
        refill.clean
        and not refill.unresolved_jumps
        and not refill.external_jumps
        and bool(refill.returns)
        and set(refill.returns.values()) == {8}
    )


def _verified_zero_arguments(walk: FunctionWalk, sections: SectionMap | None) -> bool:
    proof = _ZERO_ARGUMENT_PROOFS.get(walk.entry)
    if proof is None or sections is None:
        return False
    # These fixed indirect calls are stdcall1 only while the original import
    # slots still designate measured Enter277/Leave294. Data is retained by
    # SectionMap; changing a thunk must invalidate the helper stack proof too.
    for slot, ordinal in ((0x0047581C, 277), (0x00475818, 294)):
        region = sections.find(slot)
        if region is None:
            return False
        _, base, body = region
        original = body[slot - base : slot - base + 4]
        if len(original) != 4 or int.from_bytes(original, "little") != 0x80000000 | ordinal:
            return False
    for va, size, expected, returns in proof:
        region = sections.find(va)
        if region is None:
            return False
        _, base, body = region
        original = body[va - base : va - base + size]
        if len(original) != size or sha256(original).hexdigest() != expected:
            return False
        child = walk if va == walk.entry else walk_function(sections, va)
        # T549: a stub entry is admitted only when it jumps to the NEXT fingerprinted body.
        stub_target = proof[1][0] if va == walk.entry and walk.entry in _STUB_ENTRY_PROOFS else None
        if (
            child.returns != dict.fromkeys(returns, 0)
            or child.truncated
            or child.undecodable
            or child.unresolved_jumps
            or child.external_jumps
            or child.entry_jump_target != stub_target
            or any(live_in_registers(child).values())
        ):
            return False
    if walk.entry in _STUB_ENTRY_PROOFS:
        # The stub's target pushes two arguments for 0x003D6870, which must clean them up itself.
        callee = walk_function(sections, 0x003D6870)
        if set(callee.returns.values()) != {8}:
            return False
    return True


def _verified_output_push(walk: FunctionWalk, sections: SectionMap | None) -> int | None:
    proof = _OUTPUT_SLOT_PROOFS.get(walk.entry)
    if proof is None or sections is None:
        return None
    pushed_va, fingerprints = proof
    for va, size, expected in fingerprints:
        region = sections.find(va)
        if region is None:
            return None
        _, base, body = region
        original = body[va - base : va - base + size]
        if len(original) != size or sha256(original).hexdigest() != expected:
            return None
    pushed = next((insn for insn in walk.insns if insn.va == pushed_va), None)
    if pushed is None or pushed.mnemonic != "push" or pushed.op_str != "ecx":
        return None
    return pushed_va


def _discarded_push(walk: FunctionWalk, pushed: DecodedInsn) -> bool:
    """Prove a pushed register value is overwritten before it can be consumed.

    Track its four-byte slot through a bounded CFG prefix. Calls, aliases,
    partial writes and unknown stack changes refuse the proof. This catches
    MSVC's one-byte stack reservation without discarding a genuine argument spill.
    """
    by_va = {insn.va: insn for insn in walk.insns}
    prefix = [insn for insn in walk.insns if insn.va < pushed.va]
    ebp_delta = None
    if len(prefix) >= 2 and prefix[-1].mnemonic == "mov" and prefix[-1].op_str == "ebp, esp":
        ebp_delta = 4
    pending = [(pushed.va + pushed.size, 0)]
    seen: set[tuple[int, int]] = set()
    while pending:
        va, delta = pending.pop()
        if (va, delta) in seen:
            continue
        if len(seen) >= 128:
            return False
        seen.add((va, delta))
        insn = by_va.get(va)
        if insn is None or insn.is_call or insn.mnemonic in RETURN_MNEMONICS:
            return False
        # An address of the slot escaping is not a dead spill proof.
        if insn.mnemonic == "lea" and ("[esp" in insn.op_str or "[ebp" in insn.op_str):
            return False
        if re.search(r", (?:esp|ebp)$", insn.op_str):
            return False
        memory = re.findall(r"\[(esp|ebp)(?: ([+-]) (0x[0-9a-f]+|[0-9]+))?\]", insn.op_str)
        if ("[esp" in insn.op_str or "[ebp" in insn.op_str) and not memory:
            return False
        overwritten = False
        for base, sign, number in memory:
            if base == "ebp" and ebp_delta is None:
                return False
            offset = int(number, 0) if number else 0
            if sign == "-":
                offset = -offset
            slot = (delta if base == "esp" else ebp_delta) + offset
            if slot == 0:
                destination = insn.op_str.split(",", 1)[0]
                overwritten = destination.startswith("dword ptr [") and (
                    insn.mnemonic in {"mov", "fst", "fstp"}
                    or (insn.mnemonic == "and" and insn.op_str.endswith(", 0"))
                )
                if not overwritten:
                    return False
            elif -3 <= slot <= 3:
                return False
        if overwritten:
            continue
        if insn.mnemonic == "push":
            delta -= 4
        elif insn.mnemonic == "pop":
            if delta == 0:
                return False
            delta += 4
        elif "esp" in insn.writes or "ebp" in insn.writes:
            return False
        nexts = list(insn.successors)
        if insn.falls_through:
            nexts.append(insn.va + insn.size)
        if not nexts:
            return False
        pending.extend((target, delta) for target in nexts)
    return True


def _incoming_reads(walk: FunctionWalk, register: str) -> tuple[DecodedInsn, ...]:
    """Reads of the entry value, stopping each path at a write or a call."""
    by_va = {insn.va: insn for insn in walk.insns}
    pending = [walk.entry]
    seen: set[int] = set()
    reads: list[DecodedInsn] = []
    while pending:
        va = pending.pop()
        if va in seen or va not in by_va:
            continue
        seen.add(va)
        insn = by_va[va]
        if register in insn.reads and not (insn.mnemonic == "push" and _discarded_push(walk, insn)):
            reads.append(insn)
        if register in insn.writes or insn.is_call:
            continue
        pending.extend(insn.successors)
        if insn.falls_through:
            pending.append(insn.va + insn.size)
    return tuple(reads)


def live_in_registers(walk: FunctionWalk) -> dict[str, bool]:
    """Which argument registers are LIVE-IN at the function's entry.

    A register is live-in exactly when some path from the entry READS it before
    writing it, which is the formal statement of "it arrived holding something". That
    is the measurement behind `register_args`, and it is a backward fixpoint over the
    walk's own instructions rather than a peek at the prologue, because
    `__fastcall`'s second argument is routinely consumed several blocks in.

    A `call` KILLS `eax`, `ecx` and `edx`. Without that, a read of `ecx` after a call
    makes `ecx` look like an incoming argument when it is holding a returned value or
    a reloaded temporary.
    """
    by_va = {insn.va: insn for insn in walk.insns}
    if walk.entry not in by_va:
        return dict.fromkeys(ARGUMENT_REGISTERS, False)

    successors: dict[int, tuple[int, ...]] = {}
    for insn in walk.insns:
        nexts = [target for target in insn.successors if target in by_va]
        if insn.falls_through and (insn.va + insn.size) in by_va:
            nexts.append(insn.va + insn.size)
        successors[insn.va] = tuple(nexts)

    discarded = {
        insn.va
        for insn in walk.insns
        if insn.mnemonic == "push"
        and insn.op_str in ARGUMENT_REGISTERS
        and _discarded_push(walk, insn)
    }
    live: dict[int, set[str]] = {insn.va: set() for insn in walk.insns}
    order = [insn.va for insn in reversed(walk.insns)]
    changed = True
    while changed:
        changed = False
        for va in order:
            insn = by_va[va]
            out: set[str] = set()
            for target in successors[va]:
                out |= live[target]
            kills = set(insn.writes)
            if insn.is_call:
                kills |= set(CALL_CLOBBERED)
            reads = insn.reads
            if insn.va in discarded:
                reads = frozenset()
            value = (out - kills) | {r for r in reads if r in ARGUMENT_REGISTERS}
            value &= set(ARGUMENT_REGISTERS)
            if value != live[va]:
                live[va] = value
                changed = True

    entry_live = live[walk.entry]
    return {register: register in entry_live for register in ARGUMENT_REGISTERS}


def prologue_push_only(walk: FunctionWalk, register: str) -> bool:
    """True when every read of `register` in the function is a `push`.

    MSVC emits `push ecx` as a one-byte way to make four bytes of stack space, which
    reads `ecx` without `ecx` meaning anything. A row whose only evidence for a
    register argument is such a push is flagged rather than believed, because the
    push idiom and a genuine `this` spill are the same instruction.
    """
    reads = [insn for insn in walk.insns if register in insn.reads]
    return bool(reads) and all(insn.mnemonic == "push" for insn in reads)


@dataclass(frozen=True)
class CallSite:
    """One direct call to a surface address, with what the caller does around it."""

    va: int
    section: str
    target: int
    #: Argument dwords immediately preceding the call, counted backwards from it and
    #: stopping at the first instruction that is not a push, a push setup, or a
    #: `sub esp, K` reserving argument space.
    pushes: int
    #: Bytes the CALLER removes from the stack after the call AS ARGUMENT CLEANUP:
    #: `add esp, N` that is not the caller's own epilogue. Zero when it removes
    #: nothing, which is what a callee-cleanup convention requires.
    caller_cleanup: int
    #: Bytes removed by an `add esp, N` that IS the caller's epilogue. Carried
    #: separately so a reader can see it was seen and discounted, rather than
    #: wondering whether it was missed. See EPILOGUE_FOLLOWERS.
    epilogue_adjust: int = 0


def scan_call_sites(sections: SectionMap, targets: Iterable[int]) -> list[CallSite]:
    """Every direct `call rel32` to one of `targets`, read from the binary.

    This is the CORROBORATING route and the decisive route for bare-`ret` rows. It
    sweeps linearly, so it goes through `normalise_text`, which resynchronises after
    a byte that is not an instruction. Calling `capstone.disasm` on a whole section
    here instead would stop at the first undecodable byte and silently report a
    prefix: that is how an earlier scan in this tree reported zero call sites.
    """
    wanted = set(targets)
    lo, hi = sections.bounds()
    decoder = _decoder()
    found: list[CallSite] = []

    for name, base, data in sections:
        normalised = normalise_text(data, base, lo, hi, section_name=name)
        insns = normalised.insns
        targets_in_section = _branch_targets(data, insns)
        for index, insn in enumerate(insns):
            if insn.length != 5 or data[insn.offset] != 0xE8:
                continue
            delta = int.from_bytes(data[insn.offset + 1 : insn.offset + 5], "little", signed=True)
            target = (insn.va + 5 + delta) & 0xFFFFFFFF
            if target not in wanted:
                continue
            pushes = _count_pushes(data, insns, index, decoder, targets_in_section)
            cleanup, epilogue = _caller_cleanup(data, insns, index, decoder, pushes)
            found.append(
                CallSite(
                    va=insn.va,
                    section=name,
                    target=target,
                    pushes=pushes,
                    caller_cleanup=cleanup,
                    epilogue_adjust=epilogue,
                )
            )
    return found


def _branch_targets(data: bytes, insns: Sequence[object]) -> frozenset[int]:
    """VAs that some direct branch in this section jumps to.

    Used to stop the backward push scan at a basic-block boundary. Without it the
    scan walks back through the instructions of a PRECEDING block and counts its
    pushes as this call's arguments, because the allowlist of argument-setup
    mnemonics is wide enough to step over almost anything.
    """

    def rel(offset: int, size: int) -> int:
        return int.from_bytes(data[offset : offset + size], "little", signed=True)

    out: set[int] = set()
    for insn in insns:
        offset, length, va = insn.offset, insn.length, insn.va  # type: ignore[attr-defined]
        opcode = data[offset]
        if length == 5 and opcode == 0xE9:
            out.add((va + 5 + rel(offset + 1, 4)) & 0xFFFFFFFF)
        elif length == 2 and (opcode == 0xEB or 0x70 <= opcode <= 0x7F):
            out.add((va + 2 + rel(offset + 1, 1)) & 0xFFFFFFFF)
        elif length == 6 and opcode == 0x0F and 0x80 <= data[offset + 1] <= 0x8F:
            out.add((va + 6 + rel(offset + 2, 4)) & 0xFFFFFFFF)
    return frozenset(out)


def _count_pushes(
    data: bytes,
    insns: Sequence[object],
    index: int,
    decoder: capstone.Cs,
    branch_targets: frozenset[int],
) -> int:
    """Argument dwords placed on the stack immediately before the call at `index`.

    Walks backwards from the call over CONTIGUOUS instructions. Counts a `push` as
    one dword and a `sub esp, K` as K/4, because an argument written with
    `mov [esp+N]` after one `sub esp, K` is otherwise invisible -- which is the KNOWN
    LIMIT `tools/lift/callsites.py` records, and it is the error that goes the
    dangerous way. Steps over the mnemonics a compiler puts between two pushes while
    computing an argument, and stops at:

      - any other instruction,
      - a non-contiguous predecessor,
      - PUSH_SCAN_BYTES of distance,
      - a BASIC-BLOCK BOUNDARY. Without this last one the scan walks out of the
        argument-setup run into a preceding block and counts its pushes too, because
        the step-over allowlist is wide enough to cross almost anything.

    THIS NUMBER IS CORROBORATION AND NOTHING MORE. It still over-counts whenever a
    callee-saved register save sits inside the run. `classify` never derives
    `stack_args` from it for a row whose `ret imm16` is known, and reports the full
    distribution rather than any collapse of it.
    """
    setup = {"lea", "mov", "movzx", "movsx", "add", "or", "and", "xor", "shl", "shr", "inc", "dec"}
    count = 0
    cursor = index - 1
    span = 0
    while cursor >= 0:
        insn = insns[cursor]
        offset, length, va = insn.offset, insn.length, insn.va  # type: ignore[attr-defined]
        if offset + length != insns[cursor + 1].offset:  # type: ignore[attr-defined]
            break
        span += length
        if span > PUSH_SCAN_BYTES:
            break
        decoded = next(decoder.disasm(data[offset : offset + length], va, count=1), None)
        if decoded is None:
            break
        mnemonic = decoded.mnemonic
        if mnemonic == "push":
            count += 1
        elif _esp_store_slot(decoded) is not None:
            # An argument STORED into the outgoing area rather than pushed, which is
            # the KNOWN LIMIT `tools/lift/callsites.py` records and which this title
            # uses heavily: `XInitDevices`'s only call site passes its arguments with
            # `mov dword ptr [esp+0x24]` and `mov dword ptr [esp+0x28]` and contains
            # no `push` at all.
            #
            # COUNTING THESE SLOTS WAS TRIED AND MEASURED WORSE, so it is deliberately
            # NOT done. Adding distinct `[esp+N]` store offsets to the count moved
            # agreement with the ret immediate from 143 unanimous / 14 over-counting
            # to 130 unanimous / 18 over-counting across the same rows. The cause is
            # that one `sub esp` serves a whole frame, so stores belonging to OTHER
            # calls sit in the same contiguous run and there is no local way to tell
            # which call a slot belongs to. Stepped over, not counted.
            pass
        elif mnemonic == "sub":
            reserved = _esp_adjust(decoded, "sub")
            if reserved and reserved % 4 == 0:
                count += reserved // 4
            elif reserved:
                break
        elif mnemonic not in setup:
            break
        if va in branch_targets:
            # This instruction starts a new basic block, so nothing before it is part
            # of the same argument-setup run.
            break
        cursor -= 1
    return count


def _esp_store_slot(decoded: capstone.CsInsn) -> int | None:
    """Byte offset of an `esp`-relative 32-bit store, or None.

    `mov dword ptr [esp + N], src`. Recognised so that `_count_pushes` can step over
    an argument store knowingly rather than by falling into its `mov` allowlist. See
    the measurement there for why the slots are not counted. An indexed or scaled
    address is rejected: `[esp + eax*4]` is an array write, not one argument.
    """
    if decoded.mnemonic != "mov" or len(decoded.operands) != 2:
        return None
    destination = decoded.operands[0]
    if destination.type != cs_x86.X86_OP_MEM or destination.size != 4:
        return None
    memory = destination.value.mem
    if memory.index or memory.segment:
        return None
    if not memory.base or decoded.reg_name(memory.base) != "esp":
        return None
    return memory.disp if memory.disp >= 0 else None


def _esp_adjust(decoded: capstone.CsInsn, mnemonic: str) -> int:
    """Bytes `decoded` adds to esp when it is `<mnemonic> esp, imm`, else 0."""
    if decoded.mnemonic != mnemonic or len(decoded.operands) != 2:
        return 0
    first, second = decoded.operands
    if first.type != cs_x86.X86_OP_REG or decoded.reg_name(first.value.reg) != "esp":
        return 0
    if second.type != cs_x86.X86_OP_IMM:
        return 0
    return max(0, second.value.imm if mnemonic == "add" else -second.value.imm)


def _caller_cleanup(
    data: bytes,
    insns: Sequence[object],
    index: int,
    decoder: capstone.Cs,
    pushes: int,
) -> tuple[int, int]:
    """(argument cleanup, epilogue adjustment) right after the call at `index`.

    An `add esp, N` after a call is the ONLY thing that separates a `__cdecl`
    function from a callee-cleanup one with no stack arguments: both end in a bare
    `ret`. `src/host/kernel_thunk.c` settled ordinal 277 with exactly this test.

    TWO CONDITIONS, BOTH MEASURED NECESSARY, because the naive version of this test
    is WRONG:

    1. The `add` must not be the caller's own FRAME TEARDOWN. A tail-position
       `call X; add esp, N; ret` looks identical to cdecl cleanup, and at 0x003D57D0
       -- whose callee ends in `ret 4`, so cdecl is excluded by the callee itself --
       8 of 94 sites are exactly that, with N of 0x10, 0x40, 0x48, 0x50 and 0x80.
       Those are frame sizes. An `add` followed by anything in EPILOGUE_FOLLOWERS is
       therefore booked as an epilogue and reported separately rather than counted.
    2. N must equal `4 * pushes`. Two independently noisy counts agreeing on the same
       number is the evidence; either alone is not. When they disagree the row is
       refused rather than resolved, which is the conservative direction: a refused
       row stops the run with its address in the message, and a wrong cdecl arity
       makes a handler misread arguments.

    Reads ONE instruction, the immediate successor, so a deferred or folded
    adjustment reads as zero. That is why `classify` requires a zero to be unanimous
    over at least MEASURED_ARITY_MIN_SITES sites before concluding anything from it.
    """
    if index + 1 >= len(insns):
        return 0, 0
    insn = insns[index + 1]
    offset, length, va = insn.offset, insn.length, insn.va  # type: ignore[attr-defined]
    if insns[index].offset + insns[index].length != offset:  # type: ignore[attr-defined]
        return 0, 0
    decoded = next(decoder.disasm(data[offset : offset + length], va, count=1), None)
    if decoded is None:
        return 0, 0
    removed = _esp_adjust(decoded, "add") or _esp_adjust(decoded, "sub")
    if not removed:
        return 0, 0

    if index + 2 < len(insns):
        following = insns[index + 2]
        if following.offset == offset + length:  # type: ignore[attr-defined]
            after = next(
                decoder.disasm(
                    data[following.offset : following.offset + following.length],  # type: ignore[attr-defined]
                    following.va,  # type: ignore[attr-defined]
                    count=1,
                ),
                None,
            )
            if after is not None and after.mnemonic in EPILOGUE_FOLLOWERS:
                return 0, removed
    if removed != 4 * pushes:
        # Neither an epilogue nor a match for what was pushed. Reported as an
        # epilogue-class observation so it cannot be mistaken for argument cleanup.
        return 0, removed
    return removed, 0


#: A row whose convention and arity are established.
VERDICT_ESTABLISHED = "established"
#: A row this module refuses. `reason` says why, and the refusal is the product.
VERDICT_REFUSED = "refused"


@dataclass(frozen=True)
class AbiRow:
    """What this module concluded about one surface address, and on what evidence."""

    address: int
    section: str
    verdict: str
    #: One of the `CC_*` names, or None when refused.
    cc: str | None = None
    stack_args: int | None = None
    register_args: int | None = None
    #: Bytes the dispatcher should pop, return address included.
    pop_bytes: int | None = None
    #: Why a refusal was issued. Empty for an established row.
    reason: str = ""
    #: Facts a reader needs in order to disagree with the verdict.
    notes: tuple[str, ...] = ()
    #: `ret imm16` values found, for the record.
    return_immediates: tuple[int, ...] = ()
    #: Push counts at the guest's own call sites -> how many sites gave that count.
    push_votes: dict[int, int] = field(default_factory=dict)
    #: `add esp, N` values after the call -> how many sites did that.
    cleanup_votes: dict[int, int] = field(default_factory=dict)
    #: True when the primary route needed no corroboration to reach its answer.
    primary_decisive: bool = False
    #: How many distinct `ret` instructions the walk reached. The C side refuses zero.
    terminators: int = 0

    @property
    def sites(self) -> int:
        return sum(self.push_votes.values())


def classify(
    surface: Surface,
    walk: FunctionWalk,
    sites: Sequence[CallSite],
    *,
    sections: SectionMap | None = None,
) -> AbiRow:
    """Decide one address's convention and arity, or refuse it.

    The order of the gates is the order of the evidence's strength, and each refusal
    names the thing that was missing rather than returning a default.
    """
    notes: list[str] = []
    push_votes: dict[int, int] = defaultdict(int)
    cleanup_votes: dict[int, int] = defaultdict(int)
    epilogues = 0
    for site in sites:
        push_votes[site.pushes] += 1
        cleanup_votes[site.caller_cleanup] += 1
        if site.epilogue_adjust:
            epilogues += 1

    def refuse(reason: str) -> AbiRow:
        return AbiRow(
            address=surface.address,
            section=surface.section,
            verdict=VERDICT_REFUSED,
            reason=reason,
            notes=tuple(notes),
            return_immediates=tuple(sorted(walk.return_immediates)),
            push_votes=dict(push_votes),
            cleanup_votes=dict(cleanup_votes),
        )

    if not walk.section:
        return refuse("the address is not inside any executable section of the XBE")
    if walk.truncated:
        return refuse(
            f"the control-flow walk hit its {WALK_INSN_LIMIT}-instruction ceiling, so the "
            "function extent is not established"
        )
    if walk.undecodable:
        places = ", ".join(f"{va:#010x}" for va in walk.undecodable[:4])
        return refuse(f"undecodable byte(s) inside the function at {places}")

    if walk.entry_jump_target is not None:
        notes.append(
            f"the entry is a one-instruction jump stub to {walk.entry_jump_target:#010x}; the "
            "terminating instruction is the target's, and a stub pushes nothing of its own, so "
            "the target's cleanup contract is the stub's unchanged"
        )
    if walk.unresolved_jumps:
        notes.append(
            f"{len(walk.unresolved_jumps)} indirect jump(s) not followed "
            f"({', '.join(f'{va:#010x}' for va in walk.unresolved_jumps[:3])})"
        )
    if walk.external_jumps:
        notes.append(f"{len(walk.external_jumps)} direct jump(s) leave the section")
    if epilogues:
        notes.append(
            f"{epilogues} site(s) carry an `add esp` that is the CALLER'S OWN EPILOGUE or does "
            "not match what was pushed; seen and discounted, not argument cleanup"
        )

    if not walk.returns:
        if walk.unresolved_jumps or walk.external_jumps:
            return refuse(
                "no terminating instruction reached: the function leaves through a jump "
                "this walk cannot follow, so its cleanup is another function's"
            )
        return refuse("no terminating instruction reached and no tail jump to explain it")
    if not walk.unanimous:
        found = ", ".join(str(value) for value in sorted(walk.return_immediates))
        return refuse(
            f"terminating instructions disagree: ret immediates {{{found}}}. Not resolved "
            "to a minimum or a majority, because a disagreement means the walk crossed a "
            "function boundary and a wrong extent is not biased in a knowable direction"
        )

    immediate = next(iter(walk.return_immediates))
    if immediate % 4:
        return refuse(f"ret immediate {immediate} is not a whole number of dwords")

    discarded_pushes = [
        insn.va
        for insn in walk.insns
        if insn.mnemonic == "push"
        and insn.op_str in ARGUMENT_REGISTERS
        and _discarded_push(walk, insn)
    ]
    if discarded_pushes:
        notes.append(
            "register pushes overwritten before consumption are stack reservations: "
            + ", ".join(f"{va:#010x}" for va in discarded_pushes)
        )
    live = live_in_registers(walk)
    verified_push = _verified_output_push(walk, sections)
    if verified_push is not None:
        witnesses = _incoming_reads(walk, "ecx")
        if witnesses and all(insn.va == verified_push for insn in witnesses):
            live["ecx"] = False
            notes.append(
                f"T109 original caller/callee fingerprints verified: ECX push at "
                f"{verified_push:#010x} is an output-slot reservation; "
                "entry value is never consumed"
            )
    for register in ARGUMENT_REGISTERS:
        witnesses = _incoming_reads(walk, register)
        if live[register] and witnesses and all(insn.mnemonic == "push" for insn in witnesses):
            return refuse(
                f"{register} entry value is only pushed; stack reservation versus "
                "argument spill is unresolved, so no register convention is established"
            )
    register_args = 0
    if live["ecx"]:
        register_args = 2 if live["edx"] else 1
    elif live["edx"]:
        notes.append("edx is live-in but ecx is not, which no convention in the enum describes")

    for register in ARGUMENT_REGISTERS:
        if live[register] and prologue_push_only(walk, register):
            notes.append(
                f"{register} is live-in only through a `push {register}`, which MSVC also "
                "emits to reserve four bytes, so the register argument is not certain"
            )
        if not live[register] and any(register in insn.partial_writes for insn in walk.insns):
            notes.append(
                f"{register} is not live-in, but the function writes part of it; liveness here "
                "is register-granular, not byte-granular"
            )

    if immediate > 0:
        stack_args = immediate // 4
        if stack_args > MAX_STACK_ARGS:
            return refuse(
                f"{stack_args} stack arguments exceeds XDK_ABI_MAX_STACK_ARGS "
                f"({MAX_STACK_ARGS}); raise the bound deliberately rather than here"
            )
        if register_args == 2:
            cc = CC_FASTCALL
        elif register_args == 1:
            # THISCALL and a one-register FASTCALL pop identically and both put the
            # value in ecx; the label is the only difference and the binary does not
            # carry it. THISCALL is chosen and the ambiguity is recorded.
            cc = CC_THISCALL
            notes.append(
                "ecx live-in, edx not: THISCALL and a 1-register FASTCALL are "
                "indistinguishable in the callee and pop identically"
            )
        else:
            cc = CC_STDCALL
        _corroborate_pushes(notes, stack_args, register_args, push_votes)
        _corroborate_cleanup(notes, cleanup_votes)
        return AbiRow(
            address=surface.address,
            section=surface.section,
            verdict=VERDICT_ESTABLISHED,
            cc=cc,
            stack_args=stack_args,
            register_args=register_args,
            pop_bytes=4 + immediate,
            notes=tuple(notes),
            return_immediates=(immediate,),
            push_votes=dict(push_votes),
            cleanup_votes=dict(cleanup_votes),
            primary_decisive=True,
            terminators=len(walk.returns),
        )

    # A bare `ret`. pop_bytes is 4 whatever the argument count is, so `esp` is safe
    # either way and only the handler's view of its arguments is at stake. Which it is
    # has to come from the CALLER.
    notes.append("bare `ret`: pop is 4 regardless, so no esp risk attaches to this row")
    cleaning = {value: count for value, count in cleanup_votes.items() if value}
    total = sum(cleanup_votes.values())
    if cleaning:
        if len(cleaning) > 1:
            return refuse(
                "bare `ret` with CALLER cleanup of differing sizes at different sites "
                f"({dict(sorted(cleaning.items()))}), so the cdecl argument count is not "
                "established"
            )
        bytes_cleaned = next(iter(cleaning))
        if bytes_cleaned % 4:
            return refuse(f"caller cleanup of {bytes_cleaned} is not a whole number of dwords")
        if cleanup_votes.get(0):
            return refuse(
                f"bare `ret` and {len(cleaning)} site(s) clean up {bytes_cleaned} bytes while "
                f"{cleanup_votes[0]} site(s) clean up none; caller cleanup cannot be optional, "
                "so one of the two readings is wrong"
            )
        stack_args = bytes_cleaned // 4
        if stack_args > MAX_STACK_ARGS:
            return refuse(f"{stack_args} cdecl stack arguments exceeds {MAX_STACK_ARGS}")
        notes.append(f"every one of {total} site(s) removes {bytes_cleaned} bytes after the call")
        return AbiRow(
            address=surface.address,
            section=surface.section,
            verdict=VERDICT_ESTABLISHED,
            cc=CC_CDECL,
            stack_args=stack_args,
            register_args=0,
            pop_bytes=4,
            notes=tuple(notes),
            return_immediates=(0,),
            push_votes=dict(push_votes),
            cleanup_votes=dict(cleanup_votes),
            primary_decisive=False,
            terminators=len(walk.returns),
        )

    zero_argument_proof = (
        register_args == 0 and immediate == 0 and _verified_zero_arguments(walk, sections)
    )
    fixed_upload_proof = register_args == 2 and _verified_fixed_vertex_upload(walk, sections)
    if total < MEASURED_ARITY_MIN_SITES and not (zero_argument_proof or fixed_upload_proof):
        return refuse(
            f"bare `ret` and only {total} call site(s) in the image: caller cleanup is the "
            f"only thing that separates cdecl-with-arguments from callee-cleanup-with-none, "
            f"and {MEASURED_ARITY_MIN_SITES} sites are needed before the ABSENCE of an "
            "`add esp` means anything. One voter always agrees with itself"
        )

    if zero_argument_proof:
        notes.append(
            "T146 complete wrapper/FS-reader/table-helper fingerprints "
            "prove zero incoming arguments"
        )
    if fixed_upload_proof:
        notes.append(
            "T539 complete fixed-upload body and both ECX/EDX caller fingerprints "
            "prove zero incoming stack arguments"
        )

    # No site cleans up, over enough sites to mean it. The callee pops nothing and the
    # caller pops nothing, so there are no stack arguments to pop.
    notes.append(
        f"no stack arguments: none of {total} site(s) carries an `add esp` after the call, "
        "which a cdecl caller would need"
    )
    if register_args == 2:
        cc = CC_FASTCALL
    elif register_args == 1:
        cc = CC_FASTCALL
        notes.append("1 register argument in ecx, 0 on the stack")
    else:
        cc = CC_STDCALL
    _corroborate_pushes(notes, 0, register_args, push_votes)
    return AbiRow(
        address=surface.address,
        section=surface.section,
        verdict=VERDICT_ESTABLISHED,
        cc=cc,
        stack_args=0,
        register_args=register_args,
        pop_bytes=4,
        notes=tuple(notes),
        return_immediates=(0,),
        push_votes=dict(push_votes),
        cleanup_votes=dict(cleanup_votes),
        primary_decisive=zero_argument_proof or fixed_upload_proof,
        terminators=len(walk.returns) if zero_argument_proof or fixed_upload_proof else 0,
    )


def _corroborate_pushes(
    notes: list[str],
    stack_args: int,
    register_args: int,
    push_votes: dict[int, int],
) -> None:
    """Record whether the guest's own pushes agree with the primary route.

    Reports the FULL distribution. It is never allowed to change `stack_args`: a
    corroborator that can overrule the thing it corroborates is not a corroborator.
    A push run includes the return address only for an indirect site, and these are
    direct `call rel32` sites where the hardware pushes it, so the expected count is
    `stack_args` exactly.
    """
    if not push_votes:
        notes.append("no direct call site in the image, so no push corroboration")
        return
    total = sum(push_votes.values())
    distribution = dict(sorted(push_votes.items()))
    agreeing = push_votes.get(stack_args, 0)
    if agreeing == total:
        notes.append(f"pushes agree at all {total} site(s): {stack_args}")
    elif agreeing:
        notes.append(
            f"pushes agree at {agreeing} of {total} site(s); full distribution {distribution}. "
            "Not collapsed to a minimum: that is what made ordinal 219 wrong"
        )
    elif all(count <= stack_args for count in push_votes):
        # Every site counted FEWER dwords than the callee pops. That is the documented
        # one-sided failure of a push count -- an argument stored rather than pushed,
        # or a bracket that opens late -- so it weakens the corroboration and says
        # nothing against the ret immediate.
        notes.append(
            f"pushes UNDER-count the ret immediate at every site: {distribution} against "
            f"{stack_args} expected. One-sided and expected; the callee's own pop stands"
        )
    else:
        notes.append(
            f"PUSHES EXCEED the ret immediate at some site: distribution {distribution} "
            f"against {stack_args} expected"
            + (f" plus {register_args} in register(s)" if register_args else "")
            + ". An over-count is a register save or a run crossing a block boundary, but it "
            "is the direction a push count is not supposed to be wrong in"
        )


def _corroborate_cleanup(notes: list[str], cleanup_votes: dict[int, int]) -> None:
    """Flag a callee-cleanup row at whose sites the caller ALSO cleans up."""
    cleaning = sum(count for value, count in cleanup_votes.items() if value)
    if cleaning:
        notes.append(
            f"CONTRADICTION: the callee pops with `ret imm16`, yet {cleaning} site(s) also "
            "carry an `add esp` after the call. Both cannot be the cleanup"
        )


@dataclass(frozen=True)
class AbiReport:
    """The measurement over a whole surface."""

    rows: tuple[AbiRow, ...]
    #: Addresses whose independently-known arity this measurement REPRODUCED.
    #: Addresses rather than names, so no name travels into any output.
    validated: tuple[int, ...] = ()
    #: (address, known, measured) for every independently-known arity it got WRONG.
    mismatches: tuple[tuple[int, int, int], ...] = ()
    #: Addresses with an independently-known arity that the measurement REFUSED. Not
    #: a failure: a refusal is the policy working, and these quantify its cost.
    validation_refused: tuple[int, ...] = ()

    @property
    def established(self) -> tuple[AbiRow, ...]:
        return tuple(row for row in self.rows if row.verdict == VERDICT_ESTABLISHED)

    @property
    def refused(self) -> tuple[AbiRow, ...]:
        return tuple(row for row in self.rows if row.verdict == VERDICT_REFUSED)

    def refusal_reasons(self) -> dict[str, int]:
        """Refusal reason -> how many rows gave it, commonest first."""
        tally: dict[str, int] = defaultdict(int)
        for row in self.refused:
            tally[row.reason.split(":")[0].split(";")[0].strip()] += 1
        return dict(sorted(tally.items(), key=lambda item: (-item[1], item[0])))

    def by_convention(self) -> dict[str, int]:
        tally: dict[str, int] = defaultdict(int)
        for row in self.established:
            tally[row.cc or "?"] += 1
        return dict(sorted(tally.items()))

    def flagged(self) -> tuple[AbiRow, ...]:
        """Established rows carrying a note a reader must see before trusting them."""
        loud = ("CONTRADICTION", "DISAGREE", "no convention in the enum")
        return tuple(
            row
            for row in self.established
            if any(marker in note for note in row.notes for marker in loud)
        )


def measure(xbe_path: Path, surface_path: Path) -> AbiReport:
    """Measure every surface address in `surface_path` against `xbe_path`."""
    sections = SectionMap(executable_sections(xbe_path))
    surface = read_surface(surface_path)
    sites_by_target: dict[int, list[CallSite]] = defaultdict(list)
    for site in scan_call_sites(sections, (row.address for row in surface)):
        sites_by_target[site.target].append(site)
    rows = [
        classify(
            row,
            walk_function(sections, row.address),
            sites_by_target.get(row.address, []),
            sections=sections,
        )
        for row in surface
    ]
    by_address = {row.address: row for row in rows}
    validated: list[int] = []
    mismatches: list[tuple[int, int, int]] = []
    refused: list[int] = []
    for entry in surface:
        known = KNOWN_STACK_ARGS.get(entry.name or "")
        if known is None:
            continue
        measured_row = by_address[entry.address]
        if measured_row.verdict != VERDICT_ESTABLISHED:
            refused.append(entry.address)
        elif measured_row.stack_args == known:
            validated.append(entry.address)
        else:
            mismatches.append((entry.address, known, measured_row.stack_args or 0))
    return AbiReport(
        rows=tuple(rows),
        validated=tuple(sorted(validated)),
        mismatches=tuple(sorted(mismatches)),
        validation_refused=tuple(sorted(refused)),
    )


BANNER = """\
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED by tools/xdk_abi.py. Do not edit: regenerate instead.
 *
 * Calling convention and argument counts for the XDK surface addresses this title
 * statically links, MEASURED from the user's own executable.
 *
 * THE PRIMARY EVIDENCE IS THE CALLEE'S OWN TERMINATING INSTRUCTION. `ret imm16`
 * pops the return address plus imm16 bytes, so `pop_bytes` is `4 + imm16` and the
 * stack-argument count is `imm16 / 4`. That is the function's own statement of its
 * cleanup contract, read at its address, and it is not a vote over call sites --
 * which is why the minimum-over-sites failure that made kernel ordinal 219 wrong
 * cannot occur here. Where a function has several `ret` instructions they are
 * required to AGREE; a disagreement is refused rather than resolved, because it
 * means the control-flow walk crossed a function boundary.
 *
 * A bare `ret` settles `pop_bytes` at 4 and does NOT settle the argument count:
 * cdecl-with-arguments and callee-cleanup-with-none both end in one. Those rows are
 * settled from the CALLER, by whether an `add esp, N` follows the call.
 *
 * `register_args` is a live-in analysis over the callee's own control flow: a
 * register is an argument exactly when some path reads it before writing it.
 *
 * This is derived from the user's binary and is therefore never committed.
 */
"""


def emit_c(report: AbiReport, path: Path) -> int:
    """Write the established rows as a C table. Returns how many were written.

    Only established rows. A refused row must keep stopping the run, and emitting it
    with a placeholder would turn a loud refusal into a silent wrong pop, which is
    the failure `src/host/xdk_thunk.c` exists to prevent.
    """
    if report.mismatches:
        raise ValueError(
            "the measurement disagrees with independently-known arities at "
            + ", ".join(
                f"{address:#010x} (known {known}, measured {got})"
                for address, known, got in report.mismatches
            )
            + "; refusing to emit a table that fails its own validation"
        )
    if len(report.validated) < MIN_VALIDATED:
        raise ValueError(
            f"only {len(report.validated)} independently-known aritie(s) were reproduced, "
            f"below the {MIN_VALIDATED} required; a measurement that was never put to the "
            "test must not be used. Expected some of KNOWN_STACK_ARGS to be in the surface"
        )

    established = report.established
    lines = [
        BANNER,
        "",
        f"/* {len(established)} of {len(report.rows)} surface addresses established."
        f" {len(report.refused)} refused; see tools/xdk_abi.py --verbose for each reason. */",
        "",
        f"/* Validated: reproduced {len(report.validated)} independently-known stack-argument",
        " * counts from the public BSD-sockets API, spanning 0 to 6 arguments, 0 mismatches.",
        f" * {len(report.validation_refused)} further known row(s) were refused rather than",
        " * answered, which is the quorum working rather than a failure. */",
        "",
        "/* `xdk_abi_row` and the two evidence classes are declared in xdk_thunk.h, which the",
        " * includer has already seen. `count` is the number of `ret` instructions read for",
        " * XDK_ABI_FROM_CALLEE_RET and the number of call sites voting for",
        " * XDK_ABI_FROM_CALLER_VOTES, so each class reaches the entry point that gates it. */",
        "static const xdk_abi_row XDK_MEASURED_ABIS[] = {",
    ]
    for row in sorted(established, key=lambda item: item.address):
        evidence = (
            "XDK_ABI_FROM_CALLEE_RET" if row.primary_decisive else "XDK_ABI_FROM_CALLER_VOTES"
        )
        count = row.terminators if row.primary_decisive else row.sites
        lines.append(
            f"    {{0x{row.address:08X}u, {row.cc}, {row.stack_args}u, "
            f"{row.register_args}u, {evidence}, {count}u}},"
        )
    lines.append("};")
    lines.append("")
    lines.append(
        "#define XDK_MEASURED_ABI_COUNT (sizeof(XDK_MEASURED_ABIS) / sizeof(XDK_MEASURED_ABIS[0]))"
    )
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(established)


def render(report: AbiReport, *, verbose: bool = False) -> str:
    """A human-readable summary. Refusals are the product, so they are itemised."""
    lines = [
        f"xdk surface      {len(report.rows)} address(es)",
        f"  established    {len(report.established)}",
        f"  refused        {len(report.refused)}",
        "  by convention  "
        + (", ".join(f"{cc} {count}" for cc, count in report.by_convention().items()) or "(none)"),
    ]
    decisive = sum(1 for row in report.established if row.primary_decisive)
    lines.append(
        f"  evidence       {decisive} from the callee's own `ret imm16`, "
        f"{len(report.established) - decisive} from caller cleanup at a bare `ret`"
    )
    flagged = report.flagged()
    lines.append(f"  flagged        {len(flagged)} established row(s) carry a loud note")
    lines.append(
        f"  validated      {len(report.validated)} independently-known aritie(s) reproduced, "
        f"{len(report.mismatches)} mismatch(es), {len(report.validation_refused)} refused"
    )
    if report.mismatches:
        for address, known, got in report.mismatches:
            lines.append(f"    MISMATCH {address:#010x} known {known}, measured {got}")
    lines.append("")
    lines.append("  refusals, by reason:")
    for reason, count in report.refusal_reasons().items():
        lines.append(f"    {count:>4}  {reason}")

    if flagged:
        lines.append("")
        lines.append("  flagged rows:")
        for row in flagged:
            lines.append(f"    {row.address:#010x} {row.section:<8} {row.cc}")
            for note in row.notes:
                lines.append(f"            {note}")

    if verbose:
        lines.append("")
        lines.append(f"  {'address':<12}{'section':<9}{'verdict':<13}cc / reason")
        for row in sorted(report.rows, key=lambda item: item.address):
            if row.verdict == VERDICT_ESTABLISHED:
                tail = (
                    f"{row.cc} stack={row.stack_args} reg={row.register_args} pop={row.pop_bytes}"
                )
            else:
                tail = row.reason
            lines.append(f"  {row.address:#010x}  {row.section:<9}{row.verdict:<13}{tail}")
            for note in row.notes:
                lines.append(f"      - {note}")
    return "\n".join(lines)


def to_json(report: AbiReport) -> list[dict[str, object]]:
    """The whole report as plain data, for a test or a diff to read.

    Deliberately carries no `.XTLID` name. A convention comes from the binary and is
    not a name, and `docs/provenance.md` governs where a name may travel.
    """
    return [
        {
            "address": f"{row.address:#010x}",
            "section": row.section,
            "verdict": row.verdict,
            "cc": row.cc,
            "stack_args": row.stack_args,
            "register_args": row.register_args,
            "pop_bytes": row.pop_bytes,
            "primary_decisive": row.primary_decisive,
            "terminators": row.terminators,
            "reason": row.reason,
            "notes": list(row.notes),
            "return_immediates": list(row.return_immediates),
            "push_votes": {str(k): v for k, v in sorted(row.push_votes.items())},
            "cleanup_votes": {str(k): v for k, v in sorted(row.cleanup_votes.items())},
        }
        for row in sorted(report.rows, key=lambda item: item.address)
    ]


def main(argv: list[str] | None = None) -> int:
    """Measure the XDK surface's calling conventions and report what was established."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--xbe", required=True, type=Path, help="the XBE to measure")
    parser.add_argument(
        "--surface",
        type=Path,
        default=Path("src/xbox/xdk_surface.c"),
        help="the generated surface table to take addresses from",
    )
    parser.add_argument("--json", type=Path, default=None, help="write the full report here")
    parser.add_argument("--emit-c", type=Path, default=None, help="write the C table here")
    parser.add_argument(
        "--verbose", action="store_true", help="list every row, established and refused"
    )
    args = parser.parse_args(argv)

    report = measure(args.xbe, args.surface)
    print(render(report, verbose=args.verbose))

    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(to_json(report), indent=1) + "\n", encoding="utf-8")
        print(f"\nwrote {args.json}")
    if args.emit_c is not None:
        args.emit_c.parent.mkdir(parents=True, exist_ok=True)
        written = emit_c(report, args.emit_c)
        print(f"wrote {args.emit_c} ({written} row(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
