# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit scratch registers and arithmetic EFLAGS at every direct call site.

A hand-written replacement does NOT reproduce the values the original leaves in the
caller-saved scratch registers (ecx, edx, and eax when the function returns nothing).
That is sound only if no caller READS such a register after the call and before
overwriting it. This module checks exactly that over the original machine code, at the
DIRECT call sites the lifter found.

WHAT IS AUDITED
---------------
`find_call_sites` reads the lifted C and collects, per function, the RETURN ADDRESS of
every direct call (the lifter emits `PUSH32(esp, <return address>); RECOMP_ABI_CALL(<va>, ..)`).
`audit_site` then walks the ORIGINAL code forward from that return address and decides, per
scratch register, whether anything reads it before it is rewritten.

WHAT IS NOT AUDITED, and is reported rather than guessed
--------------------------------------------------------
* Tail jumps (`sub_X(); return;`) carry no return address. The reader would be the
  caller's caller, so they are only counted (`tail_jumps`).
* Indirect calls (through a pointer, a vtable or the dispatch table). `data_references`
  is an UPPER BOUND on how many such paths could exist: every place the image holds the
  function's address as a 4 byte little-endian value.

THE DATAFLOW RULE
-----------------
A forward walk over a small control-flow graph, both arms of every conditional branch,
direct `jmp` followed, a visited set so loops end, and a budget of `max_insns` decoded
instructions. For one register family (eax/ax/ah/al and so on) each instruction is:

* a READ (explicit, implicit, or as part of an address) before a full write: UNSAFE.
* a FULL 32 bit write with no earlier read: the path is SAFE (the value is killed).
* a PARTIAL write (`mov cl, 1`): the upper bits stay live, keep walking.
* `ret`: SAFE for ecx and edx, UNRESOLVED for eax (the caller may return it).
* `call` with eax live: SAFE (a call produces a return value there).
* a DIRECT `call` with ecx or edx live: the callee may take a register argument, so the
  callee is walked from its entry with the same rules, up to MAX_CALL_DEPTH calls deep. If
  the callee reads the register first the site is UNSAFE, if it writes it first or returns
  without reading it the path is SAFE (a caller cannot rely on ecx or edx surviving a call),
  and the walk after the call returns is not continued. A recursive call adds nothing the
  walk already in progress does not decide. An INDIRECT call stays UNRESOLVED.
* an indirect jump, code that cannot be read or a trap: UNRESOLVED.
* (T1252 round7) EDX callee instruction-budget exhaustion may use an independently bounded
  complete original-byte/CSV closure summary, within the remaining call-depth limit. A
  preserved/mixed summary resumes the caller with strict live-return tracking through later
  calls. Missing original provenance or unknown paths remain UNRESOLVED; no budget grows.

Paths combine as UNSAFE over UNRESOLVED over SAFE.

CONSERVATIVE CHOICES, each of which can only turn a SAFE into an UNSAFE or UNRESOLVED
-----------------------------------------------------------------------------------
* `xor r, r` and `sub r, r` are pure writes, but every other instruction capstone reports
  as reading any part of the family counts as a read, including a read of `cl` after
  `mov cl, 1` wrote it.
* A write that may not happen (`cmovcc`, `bsf`, `bsr`, and anything under a `rep` prefix
  that reads ecx) is not a kill.
"""

from __future__ import annotations

import bisect
import json
import re
from collections import deque
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from tools.harness.image import GuestImage

    from .bridge import BridgeExemption
    from .manifest import ManifestEntry

# Schema 2 adds mandatory EFLAGS site verdicts and fail-closed flag eligibility.
SCHEMA = 2
AUDITED_REGISTERS = ("eax", "ecx", "edx")
MAX_INSNS = 64
MAX_CALL_DEPTH = 3
# The flag walk tracks (address, live flags, return stack) states, which outnumber instructions.
MAX_FLAG_STATES = 4096
MAX_FLAG_CALL_DEPTH = 8
# How many callers-of-callers the flag walk may climb through, one per empty-stack `ret`.
MAX_FLAG_CLIMB_DEPTH = 4
# Largest state set one stack cleanliness analysis (T757) explores before it gives up.
MAX_CLEAN_STATES = 2048
MAX_TAIL_CLIMB_DEPTH = 4  # (T1252 R17) functions climbed through tail jumps
# Largest ESP offset (bytes, either direction) from a callee entry the analysis tracks.
MAX_CLEAN_OFFSET = 0x400
_WINDOW = 16
MAX_PROOF_LOOKBACK = 32  # (T768) instructions searched back from a call for the write of a register

#: Register names (as capstone spells them) that belong to each family.
_FAMILY = {
    "eax": frozenset({"eax", "ax", "ah", "al"}),
    "ecx": frozenset({"ecx", "cx", "ch", "cl"}),
    "edx": frozenset({"edx", "dx", "dh", "dl"}),
}
#: Instructions whose destination is only conditionally written.
_CONDITIONAL_WRITERS = ("cmov", "bsf", "bsr")
_TRAPS = frozenset({"hlt", "ud2", "syscall", "sysenter", "int", "int3", "into", "iretd", "iret"})

CodeAt = Callable[[int, int], bytes]

# `PUSH32(esp, 0x003D21F5u); RECOMP_ABI_CALL(0x003BA830u, sub_003BA830); /* call 0x003BA830 */`
# The lifter also emits `RECOMP_ICALL_SAFE(0x..u, ..)` for a call whose target it did not
# lift, and the target there is still a literal address.
_CALL = re.compile(
    r"PUSH32\(esp,\s*0x([0-9A-Fa-f]+)u\);\s*RECOMP_(?:ABI_CALL|ICALL_SAFE)\(\s*0x([0-9A-Fa-f]+)u,"
)
# `sub_X(); return; /* tail jmp 0x003CCCD0 */` and
# `RECOMP_ITAIL(0x..u); return; /* manual tail jmp 0x.. */`.
# An indirect tail jump has no address in its comment and is correctly not matched.
_TAIL = re.compile(r"tail jmp 0x([0-9A-Fa-f]+)")


@dataclass(frozen=True)
class SiteVerdict:
    """The verdict for one register at one direct call site."""

    return_va: int
    register: str
    verdict: str
    reason: str
    reason_code: str = ""
    proof_path: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.verdict == "unresolved" and not self.reason_code:
            object.__setattr__(self, "reason_code", unresolved_reason_code(self.reason))


@dataclass(frozen=True)
class FunctionAudit:
    """Original caller resources; flags are separate from manifest scratch masks."""

    va: int
    direct_sites: int
    tail_jumps: int
    data_references: int
    verdicts: tuple[SiteVerdict, ...]
    flags: tuple[SiteVerdict, ...] = ()

    def _site_states(self) -> dict[int, str]:
        by_site: dict[int, list[str]] = {}
        for verdict in (*self.verdicts, *self.flags):
            by_site.setdefault(verdict.return_va, []).append(verdict.verdict)
        states = {}
        for return_va, kinds in by_site.items():
            if "unsafe" in kinds:
                states[return_va] = "unsafe"
            elif "unresolved" in kinds:
                states[return_va] = "unresolved"
            else:
                states[return_va] = "safe"
        return states

    def _count(self, state: str) -> int:
        return sum(1 for value in self._site_states().values() if value == state)

    @property
    def safe(self) -> int:
        # With no ignored registers, no scratch-register liveness proof is needed.
        return self.direct_sites - self.unsafe - self.unresolved

    @property
    def unsafe(self) -> int:
        return self._count("unsafe")

    @property
    def unresolved(self) -> int:
        return self._count("unresolved")

    @property
    def eligible(self) -> bool:
        """No unsafe resource and no unaudited arithmetic flags at a direct site.

        Historical unresolved scratch-register eligibility is unchanged. EFLAGS has no
        manifest preservation promise, so its unresolved paths fail closed. A separately
        adapted flag-bridge consumer is not an exemption by itself: only `tools.replace.bridge`
        (T1252) answers a site, and only after proving the whole bridge shape.
        """
        return self.unsafe == 0 and all(item.verdict == "safe" for item in self.flags)


# ---------------------------------------------------------------------------------------
# Reading the lifted tree
# ---------------------------------------------------------------------------------------


def find_call_sites(gen_dir: Path, vas: set[int]) -> dict[int, tuple[set[int], int]]:
    """For each wanted function: (its direct call return addresses, its tail jump count).

    Only the numbered chunks `recomp_0*.c` are read. The dispatch table and the manual XDK
    file mention every function by address without calling it from original code.
    """
    sites: dict[int, set[int]] = {va: set() for va in vas}
    tails: dict[int, int] = dict.fromkeys(vas, 0)
    for chunk in sorted(gen_dir.glob("recomp_0*.c")):
        text = chunk.read_text(encoding="utf-8", errors="replace")
        for match in _CALL.finditer(text):
            target = int(match.group(2), 16)
            if target in sites:
                sites[target].add(int(match.group(1), 16))
        for match in _TAIL.finditer(text):
            target = int(match.group(1), 16)
            if target in tails:
                tails[target] += 1
    return {va: (sites[va], tails[va]) for va in vas}


def count_data_references(
    image: GuestImage, va: int, *, code_ranges: Iterable[tuple[int, int]] = ()
) -> int:
    """Occurrences of `va` as a 4 byte little-endian value in the image.

    An UPPER bound on indirect reachability. The flat image carries no section flags, so
    with no `code_ranges` (half open `(start, end)` guest addresses) every byte of it is
    searched, which also counts an immediate such as `push <va>` in code. That is still
    evidence the address escapes, so it belongs in an upper bound.
    """
    needle = (va & 0xFFFFFFFF).to_bytes(4, "little")
    skipped = tuple(code_ranges)
    count = 0
    start = image.data.find(needle)
    while start != -1:
        guest = image.base + start
        if not any(low <= guest < high for low, high in skipped):
            count += 1
        start = image.data.find(needle, start + 1)
    return count


# ---------------------------------------------------------------------------------------
# Who calls the function a walk is inside (the caller-of-caller climb)
# ---------------------------------------------------------------------------------------

_GOTO = re.compile(r"goto loc_([0-9A-Fa-f]{8})\b")
# A top level function definition line, `void sub_003D2000(void)`.
_DEFINITION = re.compile(r"^[A-Za-z_][^\n;{}]*?\b(\w+)\([^\n;{}]*\)[ \t]*$", re.MULTILINE)
_SUB_NAME = re.compile(r"sub_([0-9A-Fa-f]{8})$")


@dataclass
class CallerIndex:
    """Every direct call, tail jump and goto in the lifted tree, plus who owns each call site.

    `sites[target]` is the set of return addresses of direct calls to `target`, `owner[return]`
    the entry of the lifted function whose body holds that call (absent when the definition is
    not a `sub_XXXXXXXX`), `tails[target]` the tail jumps to it and `gotos` every address some
    `goto loc_X` jumps to (a function entered by a jump is entered without a call).
    """

    sites: dict[int, set[int]]
    tails: dict[int, int]
    gotos: set[int]
    owner: dict[int, int]
    entries: list[int] = field(default_factory=list)  # sorted entry of every `sub_XXXXXXXX`
    # (T757) every tail jump as (target, entry of the function holding it, -1 when unknown)
    tail_edges: list[tuple[int, int]] = field(default_factory=list)


def build_caller_index(gen_dir: Path) -> CallerIndex:
    """Scan the numbered lifted chunks once (same files and patterns as `find_call_sites`)."""
    index = CallerIndex({}, {}, set(), {})
    for chunk in sorted(gen_dir.glob("recomp_0*.c")):
        text = chunk.read_text(encoding="utf-8", errors="replace")
        starts: list[int] = []
        entries: list[int] = []
        for match in _DEFINITION.finditer(text):
            name = _SUB_NAME.fullmatch(match.group(1))
            starts.append(match.start())
            entries.append(int(name.group(1), 16) if name else -1)
            if name:
                index.entries.append(int(name.group(1), 16))
        for match in _CALL.finditer(text):
            return_va = int(match.group(1), 16)
            index.sites.setdefault(int(match.group(2), 16), set()).add(return_va)
            slot = bisect.bisect_right(starts, match.start()) - 1
            if slot >= 0 and entries[slot] != -1:
                index.owner[return_va] = entries[slot]
        for match in _TAIL.finditer(text):
            target = int(match.group(1), 16)
            index.tails[target] = index.tails.get(target, 0) + 1
            slot = bisect.bisect_right(starts, match.start()) - 1
            index.tail_edges.append((target, entries[slot] if slot >= 0 else -1))
        index.gotos.update(int(match.group(1), 16) for match in _GOTO.finditer(text))
    index.entries.sort()
    index.tail_edges.sort()
    return index


# The CRT `_initterm` style loop that calls every non-null, non -1 dword of a table:
#   head; L: mov eax,[esi]; test eax,eax; je N; cmp eax,-1; je N; call eax;
#   N: add esi,4; cmp esi,edi; jb L
# Matched byte for byte. The instruction after `call eax` (`add esi, 4`) is what the flag walk
# decodes from the image to see that every arithmetic flag is killed before any read.
_LOOP_BODY = bytes.fromhex("8B0685C0740783F8FF7402FFD083C6043BF772EC")
_CALL_EAX_END = _LOOP_BODY.index(bytes.fromhex("FFD0")) + 2
_EXIT = bytes([len(_LOOP_BODY)])  # `jae` over the body: the loop is skipped when start >= end
# (before start, between start and end, after end), the loop heads seen in the retail image:
#   mov eax,start; mov edi,end; cmp eax,edi; mov esi,eax; jae out
#   mov esi,start; mov eax,esi; mov edi,end; cmp eax,edi; jae out
_LOOP_HEADS = (
    (b"\xb8", b"\xbf", bytes.fromhex("3BC78BF073") + _EXIT),
    (b"\xbe", bytes.fromhex("8BC6BF"), bytes.fromhex("3BC773") + _EXIT),
)


# The XBE header page is data only: its values (section bounds) are not read to reach a table.
HEADER_SIZE = 0x1000


def _dword_positions(data: bytes, value: int) -> list[int]:
    """Every byte offset (aligned or not) where `data` holds `value` as a little-endian dword."""
    needle = (value & 0xFFFFFFFF).to_bytes(4, "little")
    found = []
    position = data.find(needle)
    while position != -1:
        found.append(position)
        position = data.find(needle, position + 1)
    return found


def _address_escapes(data: bytes, address: int) -> bool:
    """True when the 4 byte little-endian value of `address` occurs anywhere in `data` (T1497):
    some operand, table slot or relocation like pointer may hold the address."""
    return data.find((address & 0xFFFFFFFF).to_bytes(4, "little")) != -1


def _dwords_in_range(data: bytes, low: int, high: int) -> list[int]:
    """Every byte offset whose little-endian dword is in `[low, high]` (all four alignments)."""
    found = []
    for prefix in range(low >> 16, (high >> 16) + 1):
        # the two high bytes of the dword, then every possible low half filtered below
        pattern = re.compile(
            b"(?=(..)" + re.escape((prefix & 0xFFFF).to_bytes(2, "little")) + b")", re.DOTALL
        )
        for match in pattern.finditer(data):
            value = int.from_bytes(match.group(1), "little") | (prefix << 16)
            if low <= value <= high:
                found.append(match.start())
    return sorted(found)


@dataclass(frozen=True)
class TableWalker:
    """A verified loop that calls each slot of the dword table `[start, end)`.

    `return_site` is the address after its `call eax`, where the flag walk continues.
    """

    start: int
    end: int
    return_site: int
    immediates: tuple[int, int] = ()  # addresses of the start and end dwords in the loop head


def find_table_walkers(image: GuestImage) -> tuple[TableWalker, ...]:
    """Every instance of the walker loop in the image, with its table bounds from the head."""
    found: list[TableWalker] = []
    position = image.data.find(_LOOP_BODY)
    while position != -1:
        body = image.base + position
        for before, between, after in _LOOP_HEADS:
            size = len(before) + 4 + len(between) + 4 + len(after)
            raw = image.code_at(body - size, size)
            middle = len(before) + 4
            if (
                len(raw) == size
                and raw.startswith(before)
                and raw[middle : middle + len(between)] == between
                and raw.endswith(after)
            ):
                start = int.from_bytes(raw[len(before) : middle], "little")
                end_at = middle + len(between)
                end = int.from_bytes(raw[end_at : end_at + 4], "little")
                head = body - size
                found.append(
                    TableWalker(
                        start,
                        end,
                        body + _CALL_EAX_END,
                        (head + len(before), head + end_at),
                    )
                )
        position = image.data.find(_LOOP_BODY, position + 1)
    return tuple(found)


def _spans(code: dict[int, Any]) -> list[tuple[int, int]]:
    """Maximal contiguous `[first, end)` runs of decoded instructions."""
    spans: list[tuple[int, int]] = []
    for address in sorted(code):
        end = address + code[address].size
        if spans and spans[-1][1] == address:
            spans[-1] = (spans[-1][0], end)
        else:
            spans.append((address, end))
    return spans


def _in_spans(spans: list[tuple[int, int]], address: int) -> bool:
    return any(low <= address < high for low, high in spans)


class CallerClimb:
    """Decides when the callers of a function are provably ALL its direct call sites.

    A walk that reaches a `ret` with an empty return stack continues at the return site of
    every caller of the function it is inside. That is sound only if no other way into the
    function exists, so `callers` refuses (returns None and a reason) unless the function has
    at least one direct call site AND no tail jump to it, no `goto` to its entry, and no 4 byte
    copy of its address anywhere in the image (a pointer, vtable slot or push immediate).
    The last test is the same upper bound `count_data_references` documents for indirect
    reachability, so a function whose address escapes is never climbed through.
    """

    def __init__(
        self,
        index: CallerIndex,
        image: GuestImage,
        *,
        code_ranges: Iterable[tuple[int, int]] = (),
        data_ranges: Iterable[tuple[int, int]] = (),
        function_starts: Iterable[int] = (),
    ) -> None:
        self._index = index
        self._image = image
        # (T1252 R17) entries of `generated/retail/functions.csv`, extra body boundaries for the
        # per owner instruction map (empty: only the lifted `sub_XXXXXXXX` entries bound a body).
        self._function_starts = tuple(sorted(set(function_starts)))
        self._deciding: list[int] = []
        # (T1497) `[low, high)` guest ranges of DATA sections (.rdata, .data, .data1). Empty
        # (the default) switches the aligned data pointer rule off.
        self._data_ranges = tuple(data_ranges)
        self._ranges = tuple(code_ranges)
        self._cache: dict[int, tuple[tuple[int, ...] | None, str]] = {}
        self._table_walkers: tuple[TableWalker, ...] | None = None
        self._decoder: Any = None
        self._proofs: dict[tuple[int, tuple[Need, ...]], str] = {}
        self._sweeps: dict[int, tuple[list[Any], set[int]] | str] = {}
        self._copies: dict[int, list[int]] = {}
        self._plain_audit: StackAudit | None = None

    def owner(self, return_va: int) -> int | None:
        return self._index.owner.get(return_va)

    def callers(self, frame: int) -> tuple[tuple[int, ...] | None, str]:
        if frame not in self._cache:
            decided = self._decide(frame)
            if decided[0] is None and "cyclic" in decided[1] and self._deciding:
                return decided  # refused only because an outer tail climb is in progress
            self._cache[frame] = decided
        return self._cache[frame]

    def _decide(self, frame: int) -> tuple[tuple[int, ...] | None, str]:
        name = _hex(frame)
        sites = self._index.sites.get(frame, set())
        tail_callers: tuple[int, ...] = ()
        if self._index.tails.get(frame, 0):
            # (T1252 R17) a tail jump target is climbed only when EVERY jumping function is proven
            # to forward the call, see `_tail_callers`.
            climbed = self._tail_callers(frame)
            if isinstance(climbed, str):
                return None, f"{name} is a tail jump target ({climbed})"
            tail_callers = climbed
        if frame in self._index.gotos:
            return None, f"{name} is entered by a jump"
        outside = self._interior_entry(frame)
        if outside:
            return None, f"{name} has an interior entry from outside its body: {outside}"
        if not sites and not tail_callers:
            return self._table_callers(frame)
        references = count_data_references(self._image, frame, code_ranges=self._ranges)
        if references:
            return None, f"{name} address appears {references} times in the image"
        return tuple(sorted(set(sites) | set(tail_callers))), "complete"

    # (T1252 R17) Climbing through tail jumps.
    #
    # `F` is entered by `jmp F` from the lifted functions S1..Sn (`index.tail_edges`) and by direct
    # calls. When F returns, it returns through the slot of whoever called S (the jump does not
    # push), so the return sites of F are the direct sites of F plus the return sites of every
    # caller of every Si. That is complete only if ALL hold, else the old refusal stands:
    #  1. every tail edge of F has a known source S != F (a lifted entry) and `callers(S)` is
    #     itself proven complete (recursion, bounded depth, a cycle refuses);
    #  2. the decoded code of S holds exactly as many direct `jmp F` instructions as the lifter
    #     reported edges (no jump the proof did not see);
    #  3. S is clean up to those jumps: StackAudit on S with each `jmp F` replaced by `ret` finds
    #     every path stack balanced (offset 0 at the jump), no store reaching the return slot, no
    #     unmodelled stack effect. The `ret` stand in is also what makes the jump equal a return
    #     through S's own slot.
    # F's own body is not examined here (the flag walk checks `clean(F)` at its `ret`). Arguments
    # and registers S passes to F are NOT proven: `pointer_proof` refuses any F with tail
    # sources, so a store through an argument pointer forwarded by S stays unresolved.

    def tail_sources(self, frame: int) -> tuple[int, ...]:
        """Sources (lifted entry, -1 unknown) of every tail jump to `frame`, one per jump."""
        edges = self._index.tail_edges
        position = bisect.bisect_left(edges, (frame, -1))
        found = []
        while position < len(edges) and edges[position][0] == frame:
            found.append(edges[position][1])
            position += 1
        return tuple(found)

    def _tail_callers(self, frame: int) -> tuple[int, ...] | str:
        sources = self.tail_sources(frame)
        if not sources:
            return "no tail jump source known"
        if len(self._deciding) >= MAX_TAIL_CLIMB_DEPTH or frame in self._deciding:
            return "tail jump chain too deep or cyclic"
        found: set[int] = set()
        self._deciding.append(frame)
        try:
            for source in sorted(set(sources)):
                if source < 0 or source == frame:
                    return (
                        f"tail jump from {_hex(source) if source >= 0 else 'an unknown function'}"
                    )
                reason = self._tail_source_forwards(source, frame, sources.count(source))
                if reason:
                    return f"tail jump from {_hex(source)}: {reason}"
                callers, why = self.callers(source)
                if callers is None:
                    return f"caller set of tail jumping {_hex(source)} unproven: {why}"
                found.update(callers)
        finally:
            self._deciding.pop()
        return tuple(sorted(found))

    def _tail_source_forwards(self, source: int, frame: int, edges: int) -> str:
        """Why `source`'s `jmp frame` instructions are not proven plain tail forwards, else ''."""
        import capstone

        code = self._code_map(source)
        if code is None:
            return "its code is not fully decoded"
        jumps = [
            address
            for address, insn in code.items()
            if insn.mnemonic == "jmp"
            and insn.operands
            and insn.operands[0].type == capstone.CS_OP_IMM
            and insn.operands[0].imm & 0xFFFFFFFF == frame
        ]
        if len(jumps) != edges:
            return f"decoded {len(jumps)} direct jumps but the lifted tree reports {edges}"
        patched = {
            address + offset: byte
            for address in jumps
            for offset, byte in enumerate(b"\xc3" + b"\x90" * (code[address].size - 1))
        }
        base = self._image.code_at

        def overlay(address: int, size: int) -> bytes:
            raw = bytearray(base(address, size))
            for offset in range(len(raw)):
                if address + offset in patched:
                    raw[offset] = patched[address + offset]
            return bytes(raw)

        if self._decoder is None:
            self._decoder = _make_decoder()
        result = StackAudit(overlay, self._decoder).clean(source, False)
        if not result.clean:
            return f"not clean up to the jump ({result.reason})"
        return ""

    def _interior_entry(self, frame: int) -> str:
        """Why some jump or call from outside `frame` enters its interior, else "" (T757).

        The body is `[frame, next lifted entry)`. A tail jump to an address inside it from a
        function other than `frame` (or from an unknown owner), or a direct call to such an
        address, reaches the body's `ret` without passing the entry the climb guards.
        `goto` is always inside its own function, so a goto to an inner label is not external.
        """
        entries = self._index.entries
        slot = bisect.bisect_right(entries, frame)
        end = entries[slot] if slot < len(entries) else 1 << 32
        edges = self._index.tail_edges
        for position in range(bisect.bisect_right(edges, (frame, 1 << 32)), len(edges)):
            target, source = edges[position]
            if target >= end:
                break
            if source != frame:
                return f"tail jump to {_hex(target)} from {_hex(source) if source >= 0 else '?'}"
        for target in self._index.sites:
            if frame < target < end:
                return f"direct call to {_hex(target)}"
        return self._interior_copy(frame, end)

    def _interior_copy(self, frame: int, end: int) -> str:
        """Why the image holds a 4 byte copy of an interior address of `[frame, end)` (T779).

        An indirect `jmp`/`call` (`push imm32; jmp [cell]`, a vtable slot, a `mov reg, imm32`
        jump target) can enter the body past the entry the climb guards and reach its `ret`
        with the caller's flags unproven. Every dword of the image (data, code immediate or
        relocation material, every byte alignment) equal to ANY address in `(frame, end)` is
        therefore refused, the same upper bound `count_data_references` applies to the entry.
        REMAINING ASSUMPTION, not closed: an interior target computed arithmetically at run time
        (`mov eax, base; add eax, offset; jmp eax`, no dword equal to the address) is not
        excluded. The strict xfail witness is
        `tests/test_t779_interior_dword.py::test_arithmetic_interior_entry_is_not_excluded`.
        """
        top = min(end, self._image.base + len(self._image.data))
        if frame + 1 >= top:
            return ""
        if frame not in self._copies:
            self._copies[frame] = _dwords_in_range(self._image.data, frame + 1, top - 1)
        # (T1252) a dword proven to be opcode or non-operand bytes inside a decoded instruction
        # is not an address any code can load as an immediate or displacement, so it is no copy.
        found = [
            at
            for at in self._copies[frame]
            if not self._inside_reachable_code(self._image.base + at)
            and not self._unaligned_data_straddle(self._image.base + at)
            and not self._mid_instruction(
                frame, int.from_bytes(self._image.data[at : at + 4], "little")
            )
        ]
        if not found:
            return ""
        value = int.from_bytes(self._image.data[found[0] : found[0] + 4], "little")
        return f"a 4 byte copy of interior address {_hex(value)} at image offset {found[0]:#x}"

    # (T768) Does every caller pass a non-stack pointer in the registers a callee stores through?
    #
    # `pointer_proof(entry, needs)` returns "" only when ALL hold, else the first failing reason:
    #  1. `entry` is the entry of a lifted function and `callers(entry)` is the complete direct
    #     caller set (at least one direct call, no tail jump, goto, interior entry or 4 byte copy
    #     of the address), never the table walker route (its callers are not direct calls).
    #  2. For every call site the register holds, at the call, a `mov reg, imm32` value that is
    #     an address inside the image such that every byte the callee stores
    #     (`[value+low, value+high)`) is inside the image. The guest stack is not in the image, so
    #     the store cannot reach any stack slot, let alone the return slot.
    #  3. That `mov` is proven the value on EVERY path to the call: the call is reached from the
    #     mov only by falling through (no jump, goto or outside tail jump targets any address after
    #     the mov up to the call, the function holds no indirect jump that could land there), no
    #     call, return, unconditional jump, trap or other write of the register lies between, and
    #     the instruction run is decoded linearly from the owning function entry through the call.
    # By construction the register at the callee's entry is then that image address in all
    # executions that enter through a call, and the entry is reachable by no other means, so a
    # store through an unmodified copy of it (tracked by `StackAudit`) cannot hit the stack.
    # Anything else (an argument or stack-derived value, an unknown register, a value loaded from
    # memory such as an absolute cell, an unproven site) is unclean. (R15, T1252/T1672) A pushed
    # ARGUMENT pointer (`mov reg,[esp+K]` in the callee) is accepted when every caller pushes an
    # in-image immediate or `lea reg,[esp+N]` above the callee's argument area, see
    # `_arg_pointer_at`; T768 had measured no body that needed it, the T1672 math roots do.

    def pointer_proof(self, entry: int, needs: tuple[Need, ...]) -> str:
        key = (entry, needs)
        if key not in self._proofs:
            self._proofs[key] = self._pointer_proof(entry, needs)
        return self._proofs[key]

    def _pointer_proof(self, entry: int, needs: tuple[Need, ...]) -> str:
        name = _hex(entry)
        entries = self._index.entries
        slot = bisect.bisect_left(entries, entry)
        if slot >= len(entries) or entries[slot] != entry:
            return f"{name} is not a lifted function entry"
        callers, why = self.callers(entry)
        if callers is None:
            return f"caller set of {name} unproven: {why}"
        if self._index.tails.get(entry, 0):
            sources = ", ".join(_hex(item) for item in sorted(set(self.tail_sources(entry))))
            return (
                f"{name} is also entered by tail jumps from {sources}: "
                "the arguments they forward are not proven"
            )
        if not self._index.sites.get(entry):
            return f"{name} has no direct caller set ({why})"
        for return_va in callers:
            reason = self._site_pointer(return_va, entry, needs)
            if reason:
                return f"call returning to {_hex(return_va)}: {reason}"
        return ""

    def _sweep(self, owner: int) -> tuple[list[Any], set[int]] | str:
        """Linear decode of `[owner, next lifted entry)` and every address control can jump to."""
        if owner in self._sweeps:
            return self._sweeps[owner]
        self._sweeps[owner] = self._decode_body(owner)
        return self._sweeps[owner]

    def _decode_body(self, owner: int) -> tuple[list[Any], set[int]] | str:
        outside = self._interior_entry(owner)
        if outside:
            return f"{_hex(owner)} has an interior entry from outside its body: {outside}"
        entries = self._index.entries
        slot = bisect.bisect_right(entries, owner)
        end = entries[slot] if slot < len(entries) else 1 << 32
        if self._decoder is None:
            self._decoder = _make_decoder()
        found: list[Any] = []
        labels = {goto for goto in self._index.gotos if owner <= goto < end}
        address = owner
        while address < end:
            insn = _decode_at(self._decoder, self._image.code_at, address)
            if insn is None:
                return f"{_hex(owner)} does not decode linearly at {_hex(address)}"
            groups = _group_names(insn)
            if "jump" in groups or "branch_relative" in groups:
                first = insn.operands[0] if insn.operands else None
                if first is None or first.type != 2:  # capstone.CS_OP_IMM
                    return f"{_hex(owner)} holds an indirect jump at {_hex(address)}"
                labels.add(first.imm & 0xFFFFFFFF)
            found.append(insn)
            address += insn.size
        if address != end and end != 1 << 32:
            return f"{_hex(owner)} does not decode linearly up to {_hex(end)}"
        return found, labels

    def _site_pointer(self, return_va: int, entry: int, needs: tuple[Need, ...]) -> str:
        owner = self._index.owner.get(return_va)
        if owner is None:
            return "owner of the call is unknown"
        body = self._sweep(owner)
        if isinstance(body, str):
            return body
        insns, labels = body
        call = next(
            (n for n, insn in enumerate(insns) if insn.address + insn.size == return_va), -1
        )
        if call < 0 or insns[call].mnemonic != "call":
            return "no decoded call instruction ends at the return address"
        operand = insns[call].operands[0] if insns[call].operands else None
        if operand is None or operand.type != 2 or operand.imm & 0xFFFFFFFF != entry:
            return "the decoded call does not target the callee"
        if insns[call].address in labels:
            return "the call is a jump target"
        for register, low, high in needs:
            if register == _ARGS:
                reason = self._args_at(insns, labels, call, high)
            elif register.startswith("arg:"):
                _, slot, end = register.split(":")
                reason = self._arg_pointer_at(insns, labels, call, int(slot), int(end), low, high)
            else:
                reason = self._register_at(insns, labels, call, register, low, high)
            if reason:
                return reason
        return ""

    def _neutral_call(self, insn: Any) -> bool:
        """A direct call to a function `StackAudit` proves clean (own return slot) with `ret`."""
        first = insn.operands[0] if insn.operands else None
        if insn.mnemonic != "call" or first is None or first.type != 2:
            return False
        if self._plain_audit is None:
            self._plain_audit = StackAudit(self._image.code_at, self._decoder or _make_decoder())
        result = self._plain_audit.clean(first.imm & 0xFFFFFFFF, False)
        return result.clean and result.pop == 0

    def _args_at(self, insns: list[Any], labels: set[int], call: int, high: int) -> str:
        """Why the call is not proven to push every byte of `[4, high)` above its return slot.

        (T1252) The callee writes its own argument area, offsets `[4, high)` from its entry ESP.
        That is the caller's frame only as far as the caller pushed arguments. Walking back from
        the call, `ceil((high - 4) / 4)` plain 4 byte `push`es must be found with only
        stack-neutral instructions between them (`mov`, arithmetic on other registers, loads and
        stores: nothing that writes ESP, pops, calls, jumps or returns), and no jump target may
        lie after the earliest counted push (a jump into the middle would skip a push). Anything
        else (arguments stored with `mov [esp+N]`, an unbalanced `pop`, a call in between, fewer
        pushes) is not proven.
        """
        wanted = (high - 4 + 3) // 4
        if wanted <= 0:
            return "no argument bytes to prove"
        found = 0
        for position in range(call - 1, max(call - 1 - MAX_PROOF_LOOKBACK, -1), -1):
            insn = insns[position]
            groups = _group_names(insn)
            if insn.mnemonic == "push" and insn.prefix[2] != 0x66:
                found += 1
            elif self._neutral_call(insn):
                pass  # a direct call to a clean callee that pops nothing leaves the pushes alone
            elif (
                "call" in groups
                or "jump" in groups
                or "branch_relative" in groups
                or "ret" in groups
                or "int" in groups
                or insn.mnemonic in _TRAPS
                or insn.mnemonic.startswith(("push", "pop", "leave", "enter"))
                or any((insn.reg_name(reg) or "") in ("esp", "sp") for reg in insn.regs_access()[1])
            ):
                return (
                    f"only {found} of {wanted} argument pushes precede the call "
                    f"(`{insn.mnemonic}` at {_hex(insn.address)} may change the stack depth)"
                )
            if found >= wanted:
                return ""
            if insn.address in labels:
                return (
                    f"only {found} of {wanted} argument pushes follow the jump target "
                    f"at {_hex(insn.address)}"
                )
        return f"only {found} of {wanted} argument pushes within {MAX_PROOF_LOOKBACK} instructions"

    def _arg_pointer_at(
        self,
        insns: list[Any],
        labels: set[int],
        call: int,
        slot: int,
        end: int,
        low: int,
        high: int,
    ) -> str:
        """Why the call is not proven to pass, in argument slot `slot`, a pointer whose stored
        bytes `[low, high)` lie wholly at or above the end `end` of the callee's argument area.

        (R15) The slot is the `(slot - 4) / 4`-th 4 byte `push` counted back from the call, with
        only stack-neutral instructions and neutral clean calls after it and no jump target
        after it. Its operand must be an in-image `imm32` or a register whose last write is
        `lea reg, [esp+N]`, N >= 0, with only plain 4 byte pushes (w of them, the argument push
        included) between the lea and the call and nothing that may rewrite the register or ESP.
        The pointer is then ESP_call + 4*w + N and `4*w + N + low >= end - 4` puts every stored
        byte above the argument area `[ESP_call, ESP_call + end - 4)`.
        """
        wanted = (slot - 4) // 4
        found = 0
        target = -1
        neutral_call = self._neutral_call
        for position in range(call - 1, max(call - 1 - MAX_PROOF_LOOKBACK, -1), -1):
            insn = insns[position]
            if insn.mnemonic == "push" and insn.prefix[2] != 0x66:
                if found == wanted:
                    target = position
                    break
                found += 1
            elif neutral_call(insn):
                pass
            elif self._changes_stack(insn):
                return (
                    f"only {found} of {wanted + 1} argument pushes precede the call "
                    f"(`{insn.mnemonic}` at {_hex(insn.address)} may change the stack depth)"
                )
            if insn.address in labels:
                return f"jump target at {_hex(insn.address)} after the pointer argument push"
        if target < 0:
            return f"argument slot {slot} is not one of the pushes before the call"
        push = insns[target]
        operand = push.operands[0] if push.operands else None
        if operand is None:
            return f"argument push at {_hex(push.address)} has no operand"
        if operand.type == 2:  # capstone.CS_OP_IMM
            value = operand.imm & 0xFFFFFFFF
            if value + low < self._image.base or value + high > self._image.base + len(
                self._image.data
            ):
                return f"pushed {_hex(value)} is not an in-image address for [{low}, {high})"
            return ""
        if operand.type != 1:  # capstone.CS_OP_REG
            return f"argument push at {_hex(push.address)} is not a register or immediate"
        register = _FULL_REGISTER.get(push.reg_name(operand.reg) or "", "")
        if not register or register in ("esp", "ebp"):
            return f"argument push at {_hex(push.address)} pushes {push.reg_name(operand.reg)}"
        if push.address in labels:
            return f"jump target at {_hex(push.address)} skips the write of {register}"
        pushes = found + 1
        for position in range(target - 1, max(target - 1 - MAX_PROOF_LOOKBACK, -1), -1):
            insn = insns[position]
            if insn.address in labels and not self._lea_of(insn, register):
                return f"jump target at {_hex(insn.address)} after the last write of {register}"
            written = {
                _FULL_REGISTER.get(insn.reg_name(reg) or "", "") for reg in insn.regs_access()[1]
            }
            if register in written:
                offset = self._lea_of(insn, register)
                if offset is None:
                    return f"{register} is not `lea {register}, [esp+N]` at {_hex(insn.address)}"
                if 4 * pushes + offset + low < end - 4:
                    return (
                        f"{register} = esp+{offset} with {pushes} pushes reaches into the "
                        f"callee's argument area (needs 4*{pushes}+{offset}+{low} >= {end - 4})"
                    )
                return ""
            if insn.mnemonic == "push" and insn.prefix[2] != 0x66:
                pushes += 1
            elif self._changes_stack(insn) or "call" in _group_names(insn):
                return (
                    f"`{insn.mnemonic}` at {_hex(insn.address)} may change the stack depth "
                    f"or {register} before the pointer is pushed"
                )
        return f"no write of {register} within {MAX_PROOF_LOOKBACK} instructions"

    @staticmethod
    def _lea_of(insn: Any, register: str) -> int | None:
        """N of `lea register, [esp+N]` (N >= 0, no index or segment), else None."""
        operands = insn.operands
        if insn.mnemonic != "lea" or len(operands) != 2 or operands[0].size != 4:
            return None
        if _FULL_REGISTER.get(insn.reg_name(operands[0].reg) or "") != register:
            return None
        mem = operands[1]
        if mem.type != 3 or mem.mem.index or mem.mem.segment:  # capstone.CS_OP_MEM
            return None
        if (insn.reg_name(mem.mem.base) or "") != "esp" or mem.mem.disp < 0:
            return None
        return mem.mem.disp

    @staticmethod
    def _changes_stack(insn: Any) -> bool:
        """Whether the instruction may move ESP other than by a plain 4 byte push, or leave."""
        groups = _group_names(insn)
        return bool(
            "call" in groups
            or "jump" in groups
            or "branch_relative" in groups
            or "ret" in groups
            or "int" in groups
            or insn.mnemonic in _TRAPS
            or insn.mnemonic.startswith(("push", "pop", "leave", "enter"))
            or any((insn.reg_name(reg) or "") in ("esp", "sp") for reg in insn.regs_access()[1])
        )

    def _register_at(
        self, insns: list[Any], labels: set[int], call: int, register: str, low: int, high: int
    ) -> str:
        """Why `register` is not proven an in-image address (stored `[low, high)`) at `call`."""
        for position in range(call - 1, max(call - 1 - MAX_PROOF_LOOKBACK, -1), -1):
            insn = insns[position]
            groups = _group_names(insn)
            if "call" in groups or "int" in groups or insn.mnemonic in _TRAPS:
                return f"control flow at {_hex(insn.address)} before any write of {register}"
            written = {
                _FULL_REGISTER.get(insn.reg_name(reg) or "", "") for reg in insn.regs_access()[1]
            }
            if (
                register in written
                or insn.mnemonic.startswith("popa")
                or insn.mnemonic in _ORIGIN_RESETTERS
            ):
                return self._immediate(insn, register, low, high)
            if insn.address in labels:
                return f"{_hex(insn.address)} is a jump target after the last visible write"
        return f"no write of {register} within {MAX_PROOF_LOOKBACK} instructions"

    def _immediate(self, insn: Any, register: str, low: int, high: int) -> str:
        operands = insn.operands
        if (
            insn.mnemonic != "mov"
            or len(operands) != 2
            or operands[0].size != 4
            or operands[1].type != 2  # capstone.CS_OP_IMM
        ):
            return f"{register} is not a `mov {register}, imm` at {_hex(insn.address)}"
        value = operands[1].imm & 0xFFFFFFFF
        if value + low < self._image.base or value + high > self._image.base + len(
            self._image.data
        ):
            return f"{register} = {_hex(value)} is not an in-image address for [{low}, {high})"
        return ""

    def _walkers(self) -> tuple[TableWalker, ...]:
        if self._table_walkers is None:
            self._table_walkers = find_table_walkers(self._image)
        return self._table_walkers

    def _table_callers(self, frame: int) -> tuple[tuple[int, ...] | None, str]:
        """The callers of a function with no direct call that sits in a walked pointer table.

        Proven complete only when ALL hold: its address is a 4 byte value exactly once in the
        image (no tail jump or goto, checked by the caller), that dword is a slot of the table
        of at least one verified walker loop (`find_table_walkers`), and nothing outside those
        walkers' own loop heads holds a value pointing into any such table (or at its end) (so
        no other code indexes it). The callers are then the instruction after each walker's
        `call eax`, which the flag walk decodes from the image like any other return site.
        """
        name = _hex(frame)
        references = [self._image.base + at for at in _dword_positions(self._image.data, frame)]
        if len(references) != 1:
            return None, f"{name} has no direct call site and {len(references)} data references"
        slot = references[0]
        covering = [
            walker
            for walker in self._walkers()
            if walker.start <= slot and slot + 4 <= walker.end and (slot - walker.start) % 4 == 0
        ]
        if not covering:
            return (
                None,
                f"{name} has no direct call site and is in no walked table slot",
            )
        for walker in covering:
            other = self._other_table_readers(walker)
            if other:
                return (
                    None,
                    f"{name} table {_hex(walker.start)} is also addressed at {_hex(other[0])}",
                )
        return tuple(sorted({walker.return_site for walker in covering})), "table walker"

    def _is_coincidence(self, at: int) -> bool:
        """True only when `at` is proven to be bytes inside an instruction, not its operand.

        An unaligned 4 byte value inside code (`mov ecx,[eax+0x4b84]` is `8B 88 84 4B 00 00`,
        which holds 0x004B8488) is not an address. This sweeps forward from the entry of the
        lifted function holding `at` and answers True only if every instruction up to and
        including the one covering `at` decodes, none is an `int3` (padding, so the sweep left
        the function), and `at` is not that instruction's own 4 byte immediate or displacement.
        Anything else is reported as a possible reader.
        """
        entries = self._index.entries
        slot = bisect.bisect_right(entries, at) - 1
        if slot < 0:
            return False
        if self._decoder is None:
            self._decoder = _make_decoder()
        address = entries[slot]
        while address <= at:
            insn = _decode_at(self._decoder, self._image.code_at, address)
            if insn is None or insn.mnemonic == "int3":
                return False
            if address + insn.size > at:
                operands = set()
                if insn.imm_size == 4:
                    operands.add(address + insn.imm_offset)
                if insn.disp_size == 4:
                    operands.add(address + insn.disp_offset)
                return address != at and at not in operands
            address += insn.size
        return False

    def _reachable_code(self, entry: int) -> dict[int, Any] | None:
        """Every instruction control can reach from `entry` by fallthrough and direct jumps.

        Maps instruction address to the decoded instruction. `ret`, `jmp`, `int3`, traps and
        indirect jumps end a path (an indirect jump may reach code this misses, which only makes
        the caller's answer more conservative). Returns None if any reached byte does not decode.
        """
        cache: dict[int, dict[int, Any] | None] = self.__dict__.setdefault("_reach", {})
        if entry in cache:
            return cache[entry]
        import capstone

        if self._decoder is None:
            self._decoder = _make_decoder()
        found: dict[int, Any] = {}
        result: dict[int, Any] | None = found
        entries = self._index.entries
        slot = bisect.bisect_right(entries, entry)
        end = entries[slot] if slot < len(entries) else 1 << 32
        pending = [entry]
        data, base = self._image.data, self._image.base
        while pending:
            address = pending.pop()
            if address in found or not entry <= address < end:
                continue  # a tail jump into another lifted function is not this body
            insn = _decode_at(self._decoder, self._image.code_at, address)
            if insn is None:
                result = None
                break
            found[address] = insn
            if len(found) > 20000:
                result = None
                break
            groups = _group_names(insn)
            following = address + insn.size
            first = insn.operands[0] if insn.operands else None
            direct = first is not None and first.type == capstone.CS_OP_IMM
            if "ret" in groups or insn.mnemonic in _TRAPS or insn.mnemonic == "int3":
                continue
            if "jump" in groups or "branch_relative" in groups:
                if direct:
                    pending.append(first.imm & 0xFFFFFFFF)
                elif (
                    first is not None
                    and first.type == capstone.CS_OP_MEM
                    and first.mem.base == 0
                    and first.mem.index != 0
                    and first.mem.scale == 4
                ):
                    # `jmp [index*4 + table]`: take each following dword that points into the
                    # body as a target. A table read short only leaves bytes unreached, and an
                    # unreached byte is never called a coincidence.
                    cell = first.mem.disp & 0xFFFFFFFF
                    while base <= cell and cell + 4 <= base + len(data):
                        target = int.from_bytes(data[cell - base : cell - base + 4], "little")
                        if not entry <= target < end:
                            break
                        pending.append(target)
                        cell += 4
                if insn.mnemonic != "jmp":
                    pending.append(following)
                continue
            pending.append(following)
        cache[entry] = result
        return result

    # (T1252 R17) A complete per owner instruction map.
    #
    # `_reachable_code` (recursive descent) misses code reached only through computed jumps
    # (switch arms behind `jmp reg` or table forms it does not follow), so a dword that is just
    # opcode plus rel32 bytes of such an arm was taken for an interior pointer. The map adds the
    # LINEAR decode of the body, which is what the lifter itself decodes (`Original: A - B (N
    # bytes, M insns)` in the generated C), bounded by the next lifted entry and the next
    # `functions.csv` start, in SEGMENTS separated by holes. Bytes inside a hole are never mapped.
    # A hole starts at
    #  * the byte after an instruction that never falls through (`ret`, `jmp`, a trap) unless that
    #    byte is an anchor (unanchored bytes after a terminator may be a table or a constant),
    #  * a byte that does not decode,
    #  * an `int3` (alignment padding, which MSVC also puts between the blocks of one function), or
    #  * an address decoded code uses as DATA (a displacement or immediate, or an indirect
    #    `jmp [table+i*4]`; never a direct branch or call target) that no reached instruction
    #    starts at: jump tables, byte and word index tables,
    # and ends at the first ANCHOR after it, an address known to hold code: an instruction
    # recursive descent reached, a direct branch or call target of a mapped instruction, or a
    # lifter `goto` label. With no anchor after the hole the rest of the body stays unmapped.
    # The map is accepted only if it agrees with every anchor: each anchor that falls inside a
    # mapped segment (this includes every recursive descent instruction) is a linear instruction
    # start. A desynchronised sweep (data decoded as code) would break one of these and then the
    # map is the recursive descent alone, exactly the old behaviour. Recursive descent
    # instructions win on any overlap, so the map is a superset of the old one.
    # ASSUMPTION, stated: bytes inside a segment (between two agreeing anchors, outside holes) are
    # code. Compilers place switch tables after the last instruction or after a pad; a table or
    # constant pool inline inside a segment that no instruction references would defeat it. The
    # map is only ever used to PROVE a dword is NOT an operand (so the interior copy rule stops
    # refusing it), never to prove a pointer. Witnesses: `tests/test_t1252_r17_tail_and_map.py`
    # (stray bytes after padding stay refused, tables, desynchronised sweeps).

    def _body_end(self, entry: int) -> int:
        entries = self._index.entries
        slot = bisect.bisect_right(entries, entry)
        end = entries[slot] if slot < len(entries) else self._image.base + len(self._image.data)
        position = bisect.bisect_right(self._function_starts, entry)
        if position < len(self._function_starts):
            end = min(end, self._function_starts[position])
        return min(end, self._image.base + len(self._image.data))

    def _sorted_starts(self, entry: int, code: dict[int, Any]) -> list[int]:
        cache: dict[int, list[int]] = self.__dict__.setdefault("_starts", {})
        if entry not in cache or len(cache[entry]) != len(code):
            cache[entry] = sorted(code)
        return cache[entry]

    def _code_map(self, entry: int) -> dict[int, Any] | None:
        """Instruction address to decoded instruction for the body of `entry`, else None."""
        cache: dict[int, dict[int, Any] | None] = self.__dict__.setdefault("_maps", {})
        if entry not in cache:
            reached = self._reachable_code(entry)
            result = reached
            if reached is not None:
                linear = self._linear_prefix(entry, reached)
                if linear:
                    result = {**linear, **reached}
            cache[entry] = result
        return cache[entry]

    def _linear_prefix(self, entry: int, reached: dict[int, Any]) -> dict[int, Any] | None:
        """Linear decode of the body in segments separated by holes (see the block comment).

        A hole starts at a byte that does not decode, at an `int3` (alignment padding, which
        MSVC also puts between the blocks of one function) and at an address decoded code uses
        as DATA (a displacement or immediate, never a branch or call target). It ends at the
        first ANCHOR after it: an address known to hold code, namely an instruction recursive
        descent reached, a direct branch or call target of a mapped instruction, or a lifter
        `goto` label. Bytes inside a hole are never mapped.
        """
        import capstone

        if self._decoder is None:
            self._decoder = _make_decoder()
        end = self._body_end(entry)
        data_refs: set[int] = set()
        for insn in reached.values():
            groups = _group_names(insn)
            first = insn.operands[0] if insn.operands else None
            if (
                ("jump" in groups or "branch_relative" in groups or "call" in groups)
                and first is not None
                and first.type == capstone.CS_OP_IMM
            ):
                continue  # a direct branch target is code, `jmp [tbl+i*4]` names data
            for operand in insn.operands:
                value = None
                if operand.type == capstone.CS_OP_MEM:
                    value = operand.mem.disp & 0xFFFFFFFF
                elif operand.type == capstone.CS_OP_IMM:
                    value = operand.imm & 0xFFFFFFFF
                if value is not None and entry < value < end and value not in reached:
                    data_refs.add(value)
        anchors = {goto for goto in self._index.gotos if entry <= goto < end} | set(reached)
        anchors |= self._direct_targets(reached.values(), entry, end)
        mapped: dict[int, Any] = {}
        decoded: dict[int, Any] = {}
        for _ in range(4):  # direct targets of newly mapped code can add anchors
            mapped = {}
            address = entry
            ended = False  # the previous instruction never falls through
            while address < end:
                if address not in decoded:
                    decoded[address] = _decode_at(self._decoder, self._image.code_at, address)
                insn = decoded[address]
                if (
                    insn is None
                    or insn.mnemonic == "int3"
                    or address in data_refs
                    or (ended and address not in anchors)
                ):
                    resume = [a for a in anchors if a > address and a not in data_refs]
                    if address == entry or not resume:
                        break
                    address = min(resume)
                    ended = False
                    continue
                mapped[address] = insn
                address += insn.size
                groups = _group_names(insn)
                ended = "ret" in groups or insn.mnemonic in {"jmp", "hlt", *_TRAPS}
            more = anchors | self._direct_targets(mapped.values(), entry, end)
            if more == anchors:
                break
            anchors = more
        if not mapped:
            return None
        spans = _spans(mapped)
        # every recursive descent instruction is an anchor, so a disagreement with it is caught here
        if any(_in_spans(spans, anchor) and anchor not in mapped for anchor in anchors):
            return None
        return mapped

    @staticmethod
    def _direct_targets(code: Iterable[Any], entry: int, end: int) -> set[int]:
        """In-body targets of the direct jumps, branches and calls among `code`."""
        import capstone

        found = set()
        for insn in code:
            groups = _group_names(insn)
            first = insn.operands[0] if insn.operands else None
            if (
                ("jump" in groups or "branch_relative" in groups or "call" in groups)
                and first is not None
                and first.type == capstone.CS_OP_IMM
                and entry <= first.imm & 0xFFFFFFFF < end
            ):
                found.add(first.imm & 0xFFFFFFFF)
        return found

    def _inside_reachable_code(self, at: int) -> bool:
        """True only when the 4 bytes at `at` lie in code reached from the owning entry and are
        not one instruction's own 4 byte immediate or displacement (T1252).

        Stronger than `_is_coincidence`: that sweeps linearly and so can be fooled by a data
        table decoded as code, and it refuses a dword that begins at an instruction start. Here
        every byte of the dword must belong to an instruction control flow reaches (fallthrough
        or direct jump from the lifted entry), so the bytes are real code, never table data.
        Such bytes are an address only as one instruction's 4 byte operand, which is tested.
        """
        entries = self._index.entries
        slot = bisect.bisect_right(entries, at) - 1
        if slot < 0:
            return False
        code = self._code_map(entries[slot])
        if code is None:
            return False
        starts = self._sorted_starts(entries[slot], code)
        index = bisect.bisect_right(starts, at) - 1
        if index < 0:
            return False
        covered = at
        while covered < at + 4:
            insn = code.get(starts[index]) if index < len(starts) else None
            if insn is None or insn.address > covered or insn.address + insn.size <= covered:
                return False
            if insn.address <= at and at + 4 <= insn.address + insn.size:
                operands = set()
                if insn.imm_size == 4 and not _not_a_pointer_source(insn, "imm"):
                    operands.add(insn.address + insn.imm_offset)
                if insn.disp_size == 4 and not _not_a_pointer_source(insn, "disp"):
                    operands.add(insn.address + insn.disp_offset)
                return at not in operands
            covered = insn.address + insn.size
            index += 1
        return True

    def _unaligned_data_straddle(self, at: int) -> bool:
        """True for the aligned data pointer rule (T1497): the dword at guest address `at` starts
        at a non multiple of 4 inside a DATA section (`data_ranges`) and nothing in the image can
        read it as a pointer.

        DECISION (owner delegated 2026-10-07): MSVC aligns pointer members and pointer tables to 4
        bytes, so an unaligned pointer shaped dword in .rdata/.data is a straddle of neighbouring
        constants (a float pool read at an odd offset), not a code pointer. Never applied in code
        sections (an operand there is handled by `_inside_reachable_code`). RESIDUAL RISK: a
        packed struct holding a code pointer, or a pointer computed arithmetically. The corpus
        check `python -m tools.replace.straddle_corpus` found no decoded instruction or aligned
        slot referencing the cases below, and this test enforces the same bound cheaply: the
        rule is withheld for any address whose own 4 byte value occurs ANYWHERE in the image
        (an operand, a pointer table slot, a relocation like pointer), see `_address_escapes`.
        """
        if at % 4 == 0 or not any(low <= at and at + 4 <= high for low, high in self._data_ranges):
            return False
        return not _address_escapes(self._image.data, at)

    def _mid_instruction(self, frame: int, target: int) -> bool:
        """True when `target` falls strictly inside an instruction reachable from `frame` and no
        reachable instruction starts there (T1252).

        A dword equal to such an address (typically the unaligned straddle of a float constant
        pool) can only enter the body mid instruction. ASSUMPTION, stated and not provable from
        bytes alone: compiled code does not jump into the interior of an instruction (no
        overlapping instruction streams), the same assumption the lifter itself makes by
        discovering code from entries. The strict xfail witness is
        `tests/test_t1252_mid_instruction.py::test_overlapping_instruction_stream_is_not_excluded`.
        """
        code = self._code_map(frame)
        if code is None or target in code:
            return False
        if self._padding_tail(frame, target):
            return True
        starts = self._sorted_starts(frame, code)
        index = bisect.bisect_right(starts, target) - 1
        if index < 0:
            return False
        insn = code[starts[index]]
        return insn.address < target < insn.address + insn.size

    def _padding_tail(self, frame: int, target: int) -> bool:
        """True when every byte from `target` to the end of the body `[frame, next entry)` is an
        `int3` (0xCC) padding byte: entering there traps before any `ret` (T1252)."""
        entries = self._index.entries
        slot = bisect.bisect_right(entries, frame)
        end = entries[slot] if slot < len(entries) else self._image.base + len(self._image.data)
        end = min(end, self._image.base + len(self._image.data))
        if not frame < target < end:
            return False
        start = target - self._image.base
        return all(byte == 0xCC for byte in self._image.data[start : end - self._image.base])

    def _other_table_readers(self, walker: TableWalker) -> list[int]:
        """Image addresses outside the walker loops' heads holding a value in the table's reach."""
        own = {
            at + offset for each in self._walkers() for at in each.immediates for offset in range(4)
        }
        hits = []
        for position in _dwords_in_range(self._image.data, walker.start, walker.end):
            at = self._image.base + position
            if walker.start <= at < walker.end or at in own:
                continue  # a slot of the table itself, or a verified loop head loading bounds
            if at < self._image.base + HEADER_SIZE:
                continue  # the XBE header page
            if self._is_coincidence(at):
                continue
            hits.append(at)
        return hits


# ---------------------------------------------------------------------------------------
# Is a callee's return address slot untouched (T757)
# ---------------------------------------------------------------------------------------

# (T768) (entry register, lowest byte offset stored, one past the highest) a callee stores through.
Need = tuple[str, int, int]
Discharge = Callable[[int, tuple[Need, ...]], str]
Origins = frozenset[tuple[str, str]]  # (register, entry register it is an unmodified copy of)

# (T1252) the entry value of EBP, and the pseudo register of an argument area need.
_ENTRY_EBP = "entry"
_ARGS = "args"
# Largest `and esp, -N` realignment the stack analysis models.
MAX_REALIGN = 64


def _clobbered(saved: int | None, low: int, high: int) -> int | None:
    """The saved-EBP slot offset, or None when a write of `[low, high)` may overwrite it."""
    if saved is not None and low < saved + 4 and high > saved:
        return None
    return saved


_FULL_REGISTER = {
    name: full
    for full, names in {
        "eax": ("eax", "ax", "al", "ah"),
        "ecx": ("ecx", "cx", "cl", "ch"),
        "edx": ("edx", "dx", "dl", "dh"),
        "ebx": ("ebx", "bx", "bl", "bh"),
        "esi": ("esi", "si"),
        "edi": ("edi", "di"),
        "ebp": ("ebp", "bp"),
    }.items()
    for name in names
}
# Entry registers whose value the caller proof can establish (`mov reg, imm` before the call).
_PROVABLE_ENTRY = ("eax", "ecx", "edx")
# Writers whose register effect capstone may not list: forget every tracked origin.
_ORIGIN_RESETTERS = frozenset({"cmpxchg", "xadd", "xchg", "bswap", "cmpxchg8b"})

# Instructions with a memory or stack effect Capstone does not describe by operands.
_UNMODELLED_WRITERS = frozenset(
    {"enter", "fnstenv", "fstenv", "fnsave", "fsave", "fxsave", "xsave", "cmpxchg8b"}
)


@dataclass(frozen=True)
class StackClean:
    """Whether a body provably returns through its own, unmodified return address slot."""

    clean: bool
    reason: str
    pop: int = 0  # bytes a `ret N` drops after the return address (0 for a plain `ret`)
    # Entries still being analysed whose cycle this result assumed clean with pop 0.
    open: frozenset[int] = frozenset()
    # (T1252) every `ret` is reached with EBP holding its value at entry (`push ebp ... pop ebp`
    # or `leave` of the saved slot). Only computed without `prune` (a pruned path stops early).
    keeps_ebp: bool = False


class StackAudit:
    """Conservative proof that a callee returns through the slot its caller's `call` pushed.

    The flag walk follows a direct call by pushing the return site and resuming there at the
    callee's `ret`. That is only true if the callee never rewrote its return address, so
    `clean(entry)` explores every path from `entry` (a state is address, ESP offset from
    entry, EBP offset or None) and answers clean only when ALL hold:

    - every `ret` is reached with ESP offset 0 and all `ret N` agree on N (push/pop balance);
    - ESP changes only by push, pop, call, ret, `add/sub esp,imm`, `mov ebp,esp`, `leave` and
      `mov esp,ebp` with EBP a known copy of ESP, so the offset is always known;
    - no store goes through a register other than ESP or an EBP that is a known copy of ESP
      (a pointer in any other register may alias the stack), and a store through ESP or such
      an EBP lands wholly below the return slot (it never reaches the slot, the arguments or
      any caller frame above it); absolute stores (no base or index register) are allowed;
    - there is no indirect call or jump, trap or unmodelled stack writer, every call target is
      itself clean, and the exploration stays within MAX_CLEAN_STATES.

    With `prune`, a path stops once definite overwrites cumulatively kill every live flag:
    after it no flag verdict depends on where the code returns. A recursive call assumes the
    cycle clean with pop 0 and the result is only kept (and only trusted) when the recursive
    entry's own pop is 0 as well.
    """

    def __init__(
        self,
        code_at: CodeAt,
        decoder: Any,
        discharge: Discharge | None = None,
        *,
        prune_flags: frozenset[str] | None = None,
        call_depth_budget: int | None = None,
        closure_states: list[int] | None = None,
    ) -> None:
        flags = frozenset(_ARITHMETIC_FLAGS if prune_flags is None else prune_flags)
        if not flags or not flags <= frozenset(_ARITHMETIC_FLAGS):
            raise ValueError("prune_flags must be a nonempty arithmetic-flag subset")
        # R9 only scopes where a pruned flag-observation walk may stop. It
        # never alters an unpruned return-slot or EBP-preservation obligation.
        self._prune_flags = flags
        self._call_depth_budget = call_depth_budget
        self._closure_states = closure_states
        self._code_at = code_at
        self._decoder = decoder
        # (T768) `discharge(entry, needs)` returns "" when every caller of `entry` provably passes
        # a non-stack pointer in each entry register of `needs` (see `CallerClimb.pointer_proof`),
        # else why not. Without it a store through any register but ESP/EBP is unclean as before.
        self._discharge = discharge
        self._memo: dict[tuple[int, bool], StackClean] = {}

    def clean(self, entry: int, prune: bool, active: frozenset[int] = frozenset()) -> StackClean:
        key = (entry, prune)
        if self._call_depth_budget is not None and len(active) > self._call_depth_budget:
            return StackClean(False, "unpruned stack proof exceeds remaining call depth")
        if not prune and self._call_depth_budget is None and key in self._memo:
            return self._memo[key]
        if entry in active:
            return StackClean(True, "recursion assumed", 0, frozenset({entry}), True)
        result = self._analyse(entry, prune, active | {entry})
        if entry in result.open:
            if result.clean and result.pop != 0:
                result = StackClean(False, f"recursive {_hex(entry)} pops {result.pop}")
            result = StackClean(
                result.clean, result.reason, result.pop, result.open - {entry}, result.keeps_ebp
            )
        if not prune and self._call_depth_budget is None and not result.open:
            self._memo[key] = result
        return result

    def _analyse(
        self,
        entry: int,
        prune: bool,
        active: frozenset[int],
        args_ok: bool = True,
        arg_pointers: bool = True,
    ) -> StackClean:
        import capstone
        from capstone import x86

        stack_regs = {x86.X86_REG_ESP, x86.X86_REG_SP}
        frame_regs = {x86.X86_REG_EBP, x86.X86_REG_BP}
        all_flags = self._prune_flags
        # (T768) with a `discharge`, `origins` maps a register to the entry register its value is an
        # unmodified copy of ("ecx" for `mov eax,ecx` as the first instruction). Any other write to
        # the register, any call and the unmodelled writers forget it. A store through such a
        # register becomes a need `discharge` must prove, instead of being unclean at once.
        tracked = self._discharge is not None
        start: Origins = (
            frozenset((reg, reg) for reg in _PROVABLE_ENTRY) if tracked else frozenset()
        )
        needs: dict[str, tuple[int, int, int]] = {}
        # (T1252) bytes written above the return slot (the callee's own argument area): the
        # (lowest offset, one past the highest, first site) a `discharge` must prove the callers
        # pushed.
        arg_need: tuple[int, int, int] | None = None
        # (R15) exclusive end offset of the callee's argument area (None: unknown, the stack address
        # escapes) and whether any store is not provably below the return slot (path-insensitive).
        arg_end: int | None = 4
        arg_stored = False
        # state includes path-local original flags; CFG joins never erase another arm's mask.
        # address, ESP upper offset, EBP, origins, taint, slack, saved EBP, live flags
        pending: deque[Any] = deque([(entry, 0, _ENTRY_EBP, start, "", 0, None, all_flags)])
        visited: set[Any] = set()
        pops: set[int] = set()
        open_entries: set[int] = set()
        keeps = True

        def dirty(reason: str) -> StackClean:
            return StackClean(False, reason)

        while pending:
            state = pending.popleft()
            if state in visited:
                continue
            if len(visited) >= MAX_CLEAN_STATES:
                return dirty(f"more than {MAX_CLEAN_STATES} stack states")
            if self._closure_states is not None:
                if self._closure_states[0] <= 0:
                    return dirty("exact cleanup exceeds total closure-state budget")
                self._closure_states[0] -= 1
            visited.add(state)
            address, delta, frame, origin_pairs, taint, slack, saved, remaining = state
            state_delta, state_frame = delta, frame
            origins = dict(origin_pairs)
            if abs(delta) > MAX_CLEAN_OFFSET:
                return dirty(f"stack offset unbounded at {_hex(address)}")
            insn = _decode_at(self._decoder, self._code_at, address)
            if insn is None:
                return dirty(f"no readable instruction at {_hex(address)}")
            if prune:
                # R10: retain only original flags not definitely overwritten on this
                # path. Undefined/may-write bits and callee effects are not kills.
                remaining -= _flag_effect(insn)[1]
                if not remaining:
                    continue
            name = insn.mnemonic
            groups = _group_names(insn)
            following = address + insn.size
            operands = insn.operands
            where = _hex(address)
            if name in _UNMODELLED_WRITERS or insn.mnemonic in _TRAPS or "int" in groups:
                return dirty(f"unmodelled stack effect {name} at {where}")
            direct = bool(operands) and operands[0].type == capstone.CS_OP_IMM
            if tracked and arg_end is not None:
                touched = self._argument_area_end(insn, delta, frame, slack, stack_regs, frame_regs)
                arg_end = None if touched is None else max(arg_end, touched)
            if "ret" in groups:
                if taint:
                    return dirty(taint)
                if slack:
                    return dirty(f"ret at {where} with an unrestored realigned stack")
                if delta != 0:
                    return dirty(f"ret at {where} with stack offset {delta}")
                keeps = keeps and frame == _ENTRY_EBP
                pops.add(operands[0].imm & 0xFFFF if operands else 0)
                if len(pops) > 1:
                    return dirty(f"ret N values disagree at {where}")
                continue
            if "call" in groups:
                if delta > 0:
                    return dirty(f"call pushes over the return slot or arguments at {where}")
                if not direct:
                    return dirty(f"indirect call at {where}")
                target = operands[0].imm & 0xFFFFFFFF
                sub = self.clean(target, prune, active)
                if not sub.clean:
                    return dirty(f"{sub.reason} in callee {_hex(target)} called at {where}")
                pop = sub.pop
                if prune:
                    exact = ExactCleanupProof(self._code_at, self._discharge).prove(
                        target, MAX_FLAG_CALL_DEPTH - len(active)
                    )
                    if exact.pop is None:
                        return dirty(f"{exact.reason} in callee {_hex(target)} called at {where}")
                    pop = exact.pop
                open_entries |= sub.open
                saved = _clobbered(saved, delta - 4 - slack, delta)
                kept = exact.keeps_ebp if prune else self._keeps_ebp(target, False, active, sub)
                after = frame if kept else None
                pending.append(
                    (following, delta + pop, after, frozenset(), taint, slack, saved, remaining)
                )
                continue
            if "jump" in groups or "branch_relative" in groups:
                if not direct:
                    return dirty(f"indirect jump at {where}")
                target = operands[0].imm & 0xFFFFFFFF
                pending.append((target, delta, frame, origin_pairs, taint, slack, saved, remaining))
                if name != "jmp":
                    pending.append(
                        (following, delta, frame, origin_pairs, taint, slack, saved, remaining)
                    )
                continue
            width = 2 if insn.prefix[2] == 0x66 else 4
            if (
                name == "push" or name.startswith("pusha") or name.startswith("pushf")
            ) and delta > 0:
                # the pushed bytes would land on the return slot or an argument (after a pop of it)
                return dirty(f"push over the return slot or arguments at {where}")
            if name == "push" or name.startswith("pushf"):
                saved = _clobbered(saved, delta - width - slack, delta)
                delta -= width
                if (
                    name == "push"
                    and not slack
                    and width == 4
                    and frame == _ENTRY_EBP
                    and operands[0].type == capstone.CS_OP_REG
                    and operands[0].reg in frame_regs
                ):
                    saved = delta  # the slot now holds the entry EBP
            elif name.startswith("pusha"):
                saved = _clobbered(saved, delta - 8 * width - slack, delta)
                delta -= 8 * width
            elif name == "pop":
                if operands[0].type != capstone.CS_OP_REG:
                    return dirty(f"pop to memory at {where}")
                if operands[0].reg in stack_regs:
                    return dirty(f"pop esp at {where}")
                if operands[0].reg in frame_regs:
                    frame = _ENTRY_EBP if saved == delta and not slack and width == 4 else None
                delta += width
            elif name.startswith("popf"):
                delta += width
            elif name.startswith("popa"):
                delta += 8 * width
                frame = None
            elif name == "leave":
                if not isinstance(frame, int):
                    return dirty(f"leave with unknown ebp at {where}")
                delta, slack = frame + width, 0
                frame = _ENTRY_EBP if saved == frame and width == 4 else None
            elif name in ("add", "sub") and operands[0].reg in stack_regs and insn.size > 0:
                if len(operands) != 2 or operands[1].type != capstone.CS_OP_IMM:
                    return dirty(f"esp arithmetic at {where}")
                amount = operands[1].imm
                delta += amount if name == "add" else -amount
            elif (
                name == "and"
                and operands[0].type == capstone.CS_OP_REG
                and (operands[0].reg in stack_regs)
            ):
                # (T1252) `and esp, -N` realigns downwards by at most N - 1 bytes: the offset
                # becomes an upper bound and only a `mov esp, ebp`, `lea esp, [ebp+d]` or `leave`
                # (EBP a known copy of ESP) makes it exact again. A `ret` before that is unclean.
                step = 0
                if operands[1].type == capstone.CS_OP_IMM:
                    step = (1 << 32) - (operands[1].imm & 0xFFFFFFFF)
                if step < 2 or step > MAX_REALIGN or step & (step - 1):
                    return dirty(f"esp written by and at {where}")
                slack += step - 1
            elif name == "lea" and operands[0].reg in stack_regs:
                mem = operands[1]
                if mem.type != capstone.CS_OP_MEM or mem.mem.index or mem.mem.segment:
                    return dirty(f"esp written by lea at {where}")
                if mem.mem.base in stack_regs:
                    delta += mem.mem.disp
                elif mem.mem.base in frame_regs and isinstance(frame, int):
                    delta, slack = frame + mem.mem.disp, 0
                else:
                    return dirty(f"esp written by lea at {where}")
            elif name == "mov" and operands[0].type == capstone.CS_OP_REG:
                reg, source = operands[0].reg, operands[1]
                if reg in frame_regs:
                    isesp = source.type == capstone.CS_OP_REG and source.reg in stack_regs
                    frame = delta if isesp and not slack else None
                elif reg in stack_regs:
                    if not isinstance(frame, int) or source.type != capstone.CS_OP_REG:
                        return dirty(f"mov esp at {where}")
                    if source.reg not in frame_regs:
                        return dirty(f"mov esp at {where}")
                    delta, slack = frame, 0
            else:
                written = {reg for reg in insn.regs_access()[1]}
                if written & stack_regs:
                    return dirty(f"esp written by {name} at {where}")
                if written & frame_regs:
                    frame = None
            for operand in operands:
                # Capstone 5 reports FNSTCW m16 as READ, despite its store
                # semantics. Preserve the architectural memory-write gate.
                stores = bool(operand.access & 2) or name == "fnstcw"
                if operand.type != capstone.CS_OP_MEM or not stores:
                    continue
                label = origins.get(_FULL_REGISTER.get(insn.reg_name(operand.mem.base) or "", ""))
                # (ESP is never a label, and an EBP copy of ESP is cleared on its write)
                if tracked and label is not None and not operand.mem.index:
                    low = operand.mem.disp
                    old = needs.get(label, (low, low + operand.size, address))
                    needs[label] = (min(old[0], low), max(old[1], low + operand.size), old[2])
                    continue
                reason = self._store_reason(operand, delta, frame, stack_regs, frame_regs)
                offset = self._store_offset(operand, delta, frame, stack_regs, frame_regs)
                arg_stored = arg_stored or bool(reason)
                if (
                    reason
                    and tracked
                    and args_ok
                    and not slack
                    and offset is not None
                    and offset >= 4
                ):
                    # (T1252) a store wholly inside the callee's own argument area: not unclean
                    # when `discharge` proves every caller pushed that many bytes before its call.
                    high = offset + operand.size
                    old_arg = arg_need or (offset, high, address)
                    arg_need = (min(old_arg[0], offset), max(old_arg[1], high), old_arg[2])
                    continue
                if reason:
                    saved = None  # the store may hit the saved EBP slot
                elif offset is not None:
                    saved = _clobbered(saved, offset - slack, offset + operand.size)
                if reason and not taint:
                    # (T1252) not unclean yet: only a `ret` this store can still reach is
                    # unsound. After a flag kill (`prune`) no path is explored, so a store that
                    # every path follows with a kill before any `ret` costs nothing.
                    taint = f"{reason} at {where}"
            if tracked:
                copy = None
                if name == "mov" and len(operands) == 2 and operands[0].size == 4:
                    if operands[0].type == capstone.CS_OP_REG == operands[1].type:
                        source = _FULL_REGISTER.get(insn.reg_name(operands[1].reg) or "", "")
                        copy = (insn.reg_name(operands[0].reg), origins.get(source))
                if name.startswith("popa") or name in _ORIGIN_RESETTERS:
                    origins.clear()
                for written in insn.regs_access()[1]:
                    origins.pop(_FULL_REGISTER.get(insn.reg_name(written) or "", ""), None)
                if copy is not None and copy[1] is not None and copy[0] in _FULL_REGISTER:
                    origins[_FULL_REGISTER[copy[0]]] = copy[1]
                loaded = self._argument_load(insn, state_delta, state_frame, stack_regs, frame_regs)
                if loaded is not None and not slack and arg_pointers:
                    origins[loaded[0]] = f"arg:{loaded[1]}"
            pending.append(
                (
                    following,
                    delta,
                    frame,
                    frozenset(origins.items()),
                    taint,
                    slack,
                    saved,
                    remaining,
                )
            )
        if needs:
            # Only reachable with a `discharge` (an untracked store was unclean at once).
            assert self._discharge is not None
            first = min(site for _, _, site in needs.values())
            pointer_needs = any(label.startswith("arg:") for label in needs)
            if pointer_needs and (arg_end is None or arg_stored):
                why_not = "the stack address escapes" if arg_end is None else "a store may reach it"
                return self._without_argument_pointers(
                    entry,
                    prune,
                    active,
                    args_ok,
                    f"store through an argument pointer at {_hex(first)}: the callee's argument "
                    f"area is not provably intact ({why_not})",
                    dirty,
                )
            ordered = tuple(
                sorted(
                    (f"{reg}:{arg_end}" if reg.startswith("arg:") else reg, low, high)
                    for reg, (low, high, _) in needs.items()
                )
            )
            why = self._discharge(entry, ordered)
            if why:
                message = (
                    f"store through a register that may alias the stack at {_hex(first)} ({why})"
                )
                if pointer_needs:
                    return self._without_argument_pointers(
                        entry, prune, active, args_ok, message, dirty
                    )
                return dirty(message)
        if arg_need is not None:
            assert self._discharge is not None
            why = self._discharge(entry, ((_ARGS, arg_need[0], arg_need[1]),))
            if why:
                # not proven: the stores are ordinary tainting stores again (only a `ret` they
                # can still reach is unclean, T1252 taint), so redo the walk without the need
                redone = self._analyse(
                    entry, prune, active, args_ok=False, arg_pointers=arg_pointers
                )
                if redone.clean:
                    return redone
                return dirty(
                    f"{redone.reason} (store to the callee's argument slots at "
                    f"{_hex(arg_need[2])} not proven: {why})"
                )
        keeps_ebp = keeps and not prune
        if len(pops) == 1:
            return StackClean(True, "clean", next(iter(pops)), frozenset(open_entries), keeps_ebp)
        return StackClean(True, "clean", 0, frozenset(open_entries), keeps_ebp)

    def _without_argument_pointers(
        self,
        entry: int,
        prune: bool,
        active: frozenset[int],
        args_ok: bool,
        why: str,
        dirty: Callable[[str], StackClean],
    ) -> StackClean:
        """Redo the walk with argument pointer labels off (R15 fallback).

        An argument pointer that is not proven makes its stores ordinary tainting stores again,
        exactly the behaviour before R15 (only a `ret` they can still reach is unclean, so a
        function whose flags die first is still handled as it was): never worse than before.
        """
        redone = self._analyse(entry, prune, active, args_ok=args_ok, arg_pointers=False)
        if redone.clean:
            return redone
        return dirty(f"{redone.reason} (argument pointer not proven: {why})")

    @staticmethod
    def _argument_load(
        insn: Any, delta: int, frame: int | str | None, stack_regs: set[int], frame_regs: set[int]
    ) -> tuple[str, int] | None:
        """(full register, slot offset) of `mov reg32, [esp+K]` with K >= 4 a multiple of 4."""
        import capstone

        operands = insn.operands
        if insn.mnemonic != "mov" or len(operands) != 2:
            return None
        target, source = operands
        if target.type != capstone.CS_OP_REG or target.size != 4 or source.size != 4:
            return None
        if source.type != capstone.CS_OP_MEM or source.mem.segment:
            return None
        if target.reg in stack_regs or target.reg in frame_regs:
            return None
        offset = StackAudit._store_offset(source, delta, frame, stack_regs, frame_regs)
        full = _FULL_REGISTER.get(insn.reg_name(target.reg) or "")
        if offset is None or offset < 4 or offset % 4 or full is None:
            return None
        return full, offset

    @staticmethod
    def _argument_area_end(
        insn: Any,
        delta: int,
        frame: int | str | None,
        slack: int,
        stack_regs: set[int],
        frame_regs: set[int],
    ) -> int | None:
        """Highest end offset the instruction touches above the return slot (0: none).

        None when the stack address may escape into a register, or the offset is unknown, so
        an argument word could be read or written through an alias the walk does not see.
        """
        import capstone

        name = insn.mnemonic
        end = 0
        frame_known = isinstance(frame, int)
        for position, operand in enumerate(insn.operands):
            if operand.type == capstone.CS_OP_MEM:
                base, index = operand.mem.base, operand.mem.index
                based = base in stack_regs or (base in frame_regs and frame_known)
                if index in frame_regs and frame_known:
                    return None
                if based and (index or slack or name == "lea"):
                    return None
                if based:
                    offset = delta if base in stack_regs else frame
                    assert isinstance(offset, int)
                    end = max(end, offset + operand.mem.disp + operand.size)
            elif operand.type == capstone.CS_OP_REG:
                if operand.reg in stack_regs:
                    modelled = (name in ("add", "sub", "and", "lea", "mov") and position == 0) or (
                        name == "mov" and insn.operands[0].reg in frame_regs
                    )
                    if not modelled:
                        return None
                elif operand.reg in frame_regs and frame_known:
                    modelled = (
                        name in ("push", "pop")
                        or (name == "mov" and position == 0)
                        or (name == "mov" and insn.operands[0].reg in stack_regs)
                    )
                    if not modelled:
                        return None
        return end if end > 4 else 0

    def _keeps_ebp(self, target: int, prune: bool, active: frozenset[int], sub: StackClean) -> bool:
        """Whether a clean callee returns with EBP unchanged (always asked unpruned)."""
        if not prune:
            return sub.keeps_ebp
        full = self.clean(target, False, active)
        return full.clean and full.keeps_ebp and not full.open

    @staticmethod
    def _store_offset(
        operand: Any,
        delta: int,
        frame: int | str | None,
        stack_regs: set[int],
        frame_regs: set[int],
    ) -> int | None:
        """Offset from the entry ESP of a store through ESP or an EBP copy, else None."""
        base = operand.mem.base
        if operand.mem.index:
            return None
        if base in stack_regs:
            return delta + operand.mem.disp
        if base in frame_regs and isinstance(frame, int):
            return frame + operand.mem.disp
        return None

    @staticmethod
    def _store_reason(
        operand: Any,
        delta: int,
        frame: int | str | None,
        stack_regs: set[int],
        frame_regs: set[int],
    ) -> str:
        """Why a store to `operand` may hit the return slot, or "" when it provably cannot."""
        base, index = operand.mem.base, operand.mem.index
        if index:
            return "indexed store may alias the stack"
        if base == 0:
            return ""
        if base in stack_regs:
            offset = delta + operand.mem.disp
        elif base in frame_regs and isinstance(frame, int):
            offset = frame + operand.mem.disp
        else:
            return "store through a register that may alias the stack"
        if offset + operand.size > 0:
            return f"store to stack offset {offset} reaches the return slot or above"
        return ""


@dataclass(frozen=True)
class ExactCleanup:
    """An actual all-path RET cleanup, independently checked for slot integrity."""

    pop: int | None
    reason: str
    source_sha256: str = ""
    keeps_ebp: bool = False


@dataclass(frozen=True)
class ObservationRead:
    address: int
    flags: frozenset[str]
    path: tuple[tuple[int, bytes], ...]


@dataclass(frozen=True)
class ObservationExit:
    """Bounded observed-path outcome; carries no return-cleanup assertion."""

    kind: str
    reason: str
    source_sha256: str = ""
    terminal_live_masks: tuple[frozenset[str], ...] = ()
    read: ObservationRead | None = None


class ObservationExitProof:
    def __init__(self, code_at: CodeAt) -> None:
        self._code_at = code_at

    def prove(
        self, entry: int, live: frozenset[str], remaining_depth: int = MAX_FLAG_CALL_DEPTH
    ) -> ObservationExit:
        import hashlib

        import capstone

        if not live or not live <= frozenset(_ARITHMETIC_FLAGS):
            return ObservationExit("refused", "invalid requested flag mask")
        if not 0 <= remaining_depth <= MAX_FLAG_CALL_DEPTH:
            return ObservationExit("refused", "invalid remaining call depth")
        windows: dict[int, bytes] = {}
        byte_values: dict[int, int] = {}
        owners: dict[int, int] = {}
        active: set[tuple[int, frozenset[str], int]] = set()
        memo: dict[tuple[int, frozenset[str], int], str] = {}
        terminal_masks: set[frozenset[str]] = set()
        path: list[tuple[int, bytes]] = []
        witness: ObservationRead | None = None

        class FoundRead(Exception):
            pass

        decoder = _make_decoder()
        # Only explicitly understood ordinary instruction effects. Unknown opcodes
        # cannot inherit an empty Capstone flag-effect description as permission.
        ordinary = {
            "nop",
            "mov",
            "movzx",
            "movsx",
            "lea",
            "push",
            "pop",
            "leave",
            "add",
            "adc",
            "sub",
            "sbb",
            "and",
            "or",
            "xor",
            "cmp",
            "test",
            "inc",
            "dec",
            "neg",
            "not",
            "mul",
            "imul",
            "div",
            "idiv",
            "shl",
            "sal",
            "shr",
            "sar",
            "shld",
            "shrd",
            "rol",
            "ror",
            "rcl",
            "rcr",
            "xchg",
            "bswap",
            "clc",
            "stc",
            "cmc",
            "cld",
            "std",
            "lahf",
            "sahf",
            "cwd",
            "cdq",
            "cbw",
            "cwde",
        }

        allowed_branches = {
            "jmp",
            "ja",
            "jae",
            "jb",
            "jbe",
            "je",
            "jg",
            "jge",
            "jl",
            "jle",
            "jne",
            "jno",
            "jnp",
            "jns",
            "jo",
            "jp",
            "js",
            "jecxz",
            "jcxz",
            "loop",
            "loope",
            "loopne",
        }

        def refuse(reason: str) -> None:
            raise _ExactCleanupRefusal("observation exit " + reason)

        def walk(address: int, mask: frozenset[str], depth: int) -> str:
            nonlocal witness
            if depth < 0:
                refuse("exceeds remaining call depth")
            key = (address, mask, depth)
            if key in active:
                refuse("has a CFG cycle or recursion")
            if key in memo:
                return memo[key]
            if len(memo) + len(active) >= MAX_CLEAN_STATES:
                refuse("exceeds total structural-state budget")
            if not 0 <= address < 1 << 32:
                refuse("address outside guest range")
            raw = self._code_at(address, _WINDOW)
            if type(raw) is not bytes or not raw:
                refuse("missing immutable bytes")
            if address in windows and windows[address] != raw:
                refuse("source changed during decode")
            windows[address] = raw
            for offset, value in enumerate(raw):
                pos = address + offset
                if pos in byte_values and byte_values[pos] != value:
                    refuse("inconsistent source bytes")
                byte_values[pos] = value
            insn = next(decoder.disasm(raw, address, count=1), None)
            if insn is None:
                refuse("incomplete instruction")
            for pos in range(address, address + insn.size):
                if pos in owners and owners[pos] != address:
                    refuse("overlapping instruction")
                owners[pos] = address
            active.add(key)
            name = insn.mnemonic
            groups = _group_names(insn)
            reads, writes = _flag_effect(insn)
            path.append((address, bytes(insn.bytes)))
            if reads & mask:
                if (
                    name not in ordinary | allowed_branches
                    or any(v in insn.prefix for v in (0xF0, 0xF2, 0xF3))
                    or (
                        name in allowed_branches
                        and (
                            insn.prefix[2] == 0x66
                            or len(insn.operands) != 1
                            or insn.operands[0].type != capstone.CS_OP_IMM
                        )
                    )
                ):
                    refuse("unsupported reading instruction")
                witness = ObservationRead(address, reads & mask, tuple(path))
                raise FoundRead
            if name in _TRAPS or "int" in groups:
                refuse("trap or unknown control")
            direct = bool(insn.operands) and insn.operands[0].type == capstone.CS_OP_IMM
            following = address + insn.size
            if "ret" in groups:
                if name != "ret" or insn.prefix[2] == 0x66:
                    refuse("unsupported return control")
                terminal_masks.add(mask)
                outcome = "live-return"
            elif "call" in groups:
                if (
                    name != "call"
                    or len(insn.operands) != 1
                    or not direct
                    or insn.prefix[2] == 0x66
                ):
                    refuse("indirect or unsupported call")
                outcome = walk(insn.operands[0].imm & 0xFFFFFFFF, mask, depth - 1)
                # A live-return child needs the existing exact slot/cleanup proof.
                # This bounded observation helper does not assume a continuation.
            elif "jump" in groups or "branch_relative" in groups:
                if (
                    name not in allowed_branches
                    or len(insn.operands) != 1
                    or not direct
                    or insn.prefix[2] == 0x66
                ):
                    refuse("indirect or unsupported jump")
                outcomes = {walk(insn.operands[0].imm & 0xFFFFFFFF, mask, depth)}
                if name != "jmp":
                    outcomes.add(walk(following, mask, depth))
                outcome = outcomes.pop() if len(outcomes) == 1 else "mixed"
            else:
                # Original ACOS/control-word prefixes: WAIT, FNSTCW and FLDCW.
                # Both preserve integer flags; FNSTCW writes memory, so this
                # observation permission is never a return-slot/cleanup proof.
                # Match complete bytes, not an x87 mnemonic/prefix family.
                x87_observation = bytes(insn.bytes) in {
                    b"\x9b",
                    b"\xd9\x3c\x24",
                    b"\xd9\x7d\xfc",
                    b"\xd9\x6d\x0c",
                }
                if (name not in ordinary and not x87_observation) or any(
                    v in insn.prefix for v in (0xF0, 0xF2, 0xF3)
                ):
                    refuse("unsupported instruction effect")
                remaining = mask - writes
                outcome = walk(following, remaining, depth) if remaining else "all-dead"
            path.pop()
            active.remove(key)
            memo[key] = outcome
            return outcome

        try:
            try:
                kind = walk(entry, live, remaining_depth)
            except FoundRead:
                kind = "read"
            for address, raw in windows.items():
                if self._code_at(address, _WINDOW) != raw:
                    refuse("source changed before consumption")
            digest = hashlib.sha256()
            for address, raw in sorted(windows.items()):
                digest.update(address.to_bytes(4, "little"))
                digest.update(raw)
            return ObservationExit(
                kind,
                "bounded observed paths only",
                digest.hexdigest(),
                tuple(sorted(terminal_masks, key=lambda mask: tuple(sorted(mask)))),
                witness,
            )
        except (_ExactCleanupRefusal, RecursionError) as exc:
            return ObservationExit("refused", str(exc) or "observation exit host recursion bound")


class ExactCleanupProof:
    """Fresh byte-snapshot proof: no observation-only result or cache is trusted."""

    def __init__(self, code_at: CodeAt, discharge: Discharge | None = None) -> None:
        self._code_at = code_at
        self._discharge = discharge

    def prove(self, entry: int, remaining_depth: int = MAX_FLAG_CALL_DEPTH) -> ExactCleanup:
        import hashlib

        import capstone

        windows: dict[int, bytes] = {}
        byte_values: dict[int, int] = {}
        instruction_owners: dict[int, int] = {}
        active: set[int] = set()
        memo: dict[tuple[int, int], frozenset[int]] = {}
        visited: set[tuple[int, int]] = set()
        decoder = _make_decoder()
        ordinary = {
            "nop",
            "mov",
            "movsx",
            "movzx",
            "lea",
            "add",
            "adc",
            "sub",
            "sbb",
            "and",
            "or",
            "xor",
            "cmp",
            "test",
            "inc",
            "dec",
            "neg",
            "not",
            "mul",
            "imul",
            "div",
            "idiv",
            "shl",
            "sal",
            "shr",
            "sar",
            "shld",
            "shrd",
            "rol",
            "ror",
            "rcl",
            "rcr",
            "push",
            "pop",
            "pushfd",
            "popfd",
            "pushf",
            "popf",
            "pushal",
            "popal",
            "pushaw",
            "popaw",
            "leave",
            "xchg",
            "bswap",
            "bsf",
            "bsr",
            "bt",
            "btc",
            "btr",
            "bts",
            "clc",
            "stc",
            "cmc",
            "cld",
            "std",
            "sahf",
            "lahf",
            "cwd",
            "cdq",
            "cbw",
            "cwde",
        }

        ordinary |= {
            stem + suffix
            for stem in ("set", "cmov")
            for suffix in (
                "a",
                "ae",
                "b",
                "be",
                "e",
                "g",
                "ge",
                "l",
                "le",
                "ne",
                "no",
                "np",
                "ns",
                "o",
                "p",
                "s",
            )
        }

        def refuse(reason: str) -> None:
            raise _ExactCleanupRefusal(reason)

        def walk(address: int, remaining_depth: int) -> frozenset[int]:
            if remaining_depth < 0:
                refuse("exact cleanup exceeds remaining call depth")
            if address in active:
                refuse("exact cleanup has a CFG cycle or recursion")
            key = (address, remaining_depth)
            if key in memo:
                return memo[key]
            if len(visited) >= MAX_CLEAN_STATES:
                refuse("exact cleanup exceeds total structural-state budget")
            if not 0 <= address < 1 << 32:
                refuse("exact cleanup address outside guest range")
            visited.add(key)
            raw = self._code_at(address, _WINDOW)
            if type(raw) is not bytes or not raw:
                refuse("exact cleanup has missing immutable instruction bytes")
            for offset, value in enumerate(raw):
                at = address + offset
                if at in byte_values and byte_values[at] != value:
                    refuse("exact cleanup source changed during decode")
                byte_values[at] = value
            windows[address] = raw
            insn = next(iter(decoder.disasm(raw, address, 1)), None)
            if insn is None:
                refuse("exact cleanup has incomplete instruction bytes")
            for at in range(address, address + insn.size):
                if at in instruction_owners and instruction_owners[at] != address:
                    refuse("exact cleanup has overlapping instruction boundaries")
                instruction_owners[at] = address
            active.add(address)
            groups = _group_names(insn)
            name = insn.mnemonic
            following = address + insn.size
            operands = insn.operands
            control = groups & {"call", "jump", "branch_relative", "ret"}
            if name in _TRAPS or "int" in groups:
                refuse("exact cleanup has a trap/nonreturn path")
            if control and insn.prefix[2] == 0x66:
                refuse("exact cleanup has unsupported control width")
            if "ret" in groups:
                if (
                    name != "ret"
                    or len(operands) > 1
                    or (operands and operands[0].type != capstone.CS_OP_IMM)
                ):
                    refuse("exact cleanup requires an actual near RET")
                outcomes = frozenset({operands[0].imm & 0xFFFF if operands else 0})
            elif "call" in groups:
                if name != "call" or len(operands) != 1 or operands[0].type != capstone.CS_OP_IMM:
                    refuse("exact cleanup has unknown/indirect/far call")
                sub = walk(operands[0].imm & 0xFFFFFFFF, remaining_depth - 1)
                if len(sub) != 1:
                    refuse("exact cleanup callee RET values disagree")
                outcomes = walk(following, remaining_depth)
            elif groups & {"jump", "branch_relative"}:
                if (
                    not name.startswith(("j", "loop"))
                    or len(operands) != 1
                    or operands[0].type != capstone.CS_OP_IMM
                ):
                    refuse("exact cleanup has unknown/indirect/far jump")
                outcomes = walk(operands[0].imm & 0xFFFFFFFF, remaining_depth)
                if name != "jmp":
                    outcomes |= walk(following, remaining_depth)
            else:
                # Exact observed x87 control-word forms; this proves only
                # structural RET cleanup. StackAudit independently checks the
                # FNSTCW memory write and saved frame/return-slot preservation.
                x87_control = bytes(insn.bytes) in {
                    b"\x9b",
                    b"\xd9\x3c\x24",
                    b"\xd9\x7d\xfc",
                    b"\xd9\x6d\x0c",
                }
                if name not in ordinary and not x87_control:
                    refuse("exact cleanup has unsupported instruction effect")
                if insn.prefix[0] in (0xF2, 0xF3):
                    refuse("exact cleanup has unsupported repeated instruction")
                outcomes = walk(following, remaining_depth)
            active.remove(address)
            memo[key] = outcomes
            return outcomes

        try:
            if not 0 <= remaining_depth <= MAX_FLAG_CALL_DEPTH:
                refuse("exact cleanup has no remaining call-depth contract")
            pops = walk(entry, remaining_depth)
            if len(pops) != 1:
                refuse("exact cleanup RET values disagree")

            def frozen_code_at(address: int, size: int) -> bytes:
                return windows.get(address, b"")[:size]

            full = StackAudit(
                frozen_code_at,
                _make_decoder(),
                self._discharge,
                call_depth_budget=remaining_depth,
                closure_states=[MAX_CLEAN_STATES - len(visited)],
            ).clean(entry, False)
            if not full.clean or full.open:
                refuse(f"exact cleanup return-slot proof refused: {full.reason}")
            pop = next(iter(pops))
            if full.pop != pop:
                refuse("exact cleanup witness disagrees with unpruned stack proof")
            if any(self._code_at(at, len(raw)) != raw for at, raw in windows.items()):
                refuse("exact cleanup source changed before consumption")
            provenance = hashlib.sha256()
            for at, raw in sorted(windows.items()):
                provenance.update(at.to_bytes(4, "little"))
                provenance.update(len(raw).to_bytes(4, "little"))
                provenance.update(raw)
            return ExactCleanup(
                pop,
                "actual uniform RET and unpruned slot proof",
                provenance.hexdigest(),
                full.keeps_ebp,
            )
        except (_ExactCleanupRefusal, RecursionError) as exc:
            return ExactCleanup(None, str(exc) or "exact cleanup host recursion bound")


class _ExactCleanupRefusal(Exception):
    """Internal fail-closed exit, never a guest fault."""


# ---------------------------------------------------------------------------------------
# One site, one register
# ---------------------------------------------------------------------------------------


def _not_a_pointer_source(insn: Any, kind: str) -> bool:
    """True when the 4 byte `imm` or `disp` of `insn` can never become a code pointer (T1252).

    - a direct `call`/`jmp`/`jcc` immediate is a displacement (`target - next`), not an address;
    - a `cmp` or `test` immediate only sets flags, the value flows nowhere;
    - the displacement of a memory operand of any instruction but `lea` addresses data (a jump
      table `jmp [i*4 + table]`, a `movzx` byte table): the value that flows on is the cell's
      content, and the address itself reaches no register.
    Only code that reads instruction bytes as data, or computes a target arithmetically, could
    still use the dword, the residual T779 already documents.
    """
    groups = _group_names(insn)
    first = insn.operands[0] if insn.operands else None
    if kind == "imm":
        if (
            first is not None
            and first.type == 2  # capstone.CS_OP_IMM
            and ("call" in groups or "jump" in groups or "branch_relative" in groups)
        ):
            return True
        return insn.mnemonic in ("cmp", "test")
    return insn.mnemonic != "lea"


def _make_decoder() -> Any:
    import capstone

    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def _decode_at(decoder: Any, code_at: CodeAt, address: int) -> Any | None:
    raw = code_at(address, _WINDOW)
    if not raw:
        return None
    return next(iter(decoder.disasm(raw, address, 1)), None)


def _same_register_idiom(insn: Any) -> bool:
    """`xor r, r` and `sub r, r`: capstone reports a read, but the value does not matter."""
    import capstone

    if insn.mnemonic not in ("xor", "sub") or len(insn.operands) != 2:
        return False
    first, second = insn.operands
    return (
        first.type == capstone.CS_OP_REG
        and second.type == capstone.CS_OP_REG
        and first.reg == second.reg
    )


def _effect(insn: Any, register: str) -> str:
    """How `insn` treats the register family: "read", "kill", "partial" or "none"."""
    family = _FAMILY[register]
    reads_ids, writes_ids = insn.regs_access()
    reads = {insn.reg_name(reg) for reg in reads_ids} & family
    writes = {insn.reg_name(reg) for reg in writes_ids} & family
    if _same_register_idiom(insn):
        reads = set()
    if reads:
        return "read"
    if not writes:
        return "none"
    # A write that may not happen leaves the old value live.
    if insn.mnemonic.startswith(_CONDITIONAL_WRITERS):
        return "partial"
    if insn.prefix[0] in (0xF2, 0xF3) and "ecx" in {insn.reg_name(reg) for reg in reads_ids}:
        return "partial"
    return "kill" if register in writes else "partial"


def _group_names(insn: Any) -> set[str]:
    return {insn.group_name(group) for group in insn.groups}


def _hex(address: int) -> str:
    return f"0x{address:08X}"


@dataclass(frozen=True)
class _Walk:
    """The outcome of one walk. `capped` and `cuts` say the answer leaned on a shortcut."""

    kind: str
    reason: str
    # A call chain deeper than MAX_CALL_DEPTH was not followed somewhere below.
    capped: bool = False
    # Walks still in progress whose recursive call was skipped somewhere below.
    cuts: frozenset[tuple[int, str]] = frozenset()
    summarized: bool = False
    preserved: bool = False
    proof_path: tuple[str, ...] = ()


Memo = dict[tuple[int, str], _Walk]


@dataclass(frozen=True)
class IncomingRegisterSummary:
    """Independent bounded original-CFG result; production walk does not consume it yet."""

    kind: str  # preserved, killed, mixed, unsafe, unresolved
    reason: str


class RegisterSummaryProof:
    """Memoized proof over an immutable exact byte closure, never an ABI clobber shortcut."""

    def __init__(self, bodies: dict[int, bytes], register: str) -> None:
        if register not in _FAMILY or not bodies or len(bodies) > 64:
            raise ValueError("invalid register/closure")
        if any(type(data) is not bytes or not data for data in bodies.values()):
            raise ValueError("exact nonempty immutable bytes required")
        if sum(map(len, bodies.values())) > 65536:
            raise ValueError("summary closure too large")
        spans = sorted((entry, entry + len(data)) for entry, data in bodies.items())
        if any(type(start) is not int or start < 0 or end > 1 << 32 for start, end in spans):
            raise ValueError("invalid guest address span")
        if any(end > following for (_, end), (following, _) in zip(spans, spans[1:], strict=False)):
            raise ValueError("overlapping byte contracts")
        self._bodies = dict(bodies)
        self._register = register
        self._memo: dict[int, IncomingRegisterSummary] = {}
        self._active: set[int] = set()

    def summarize(self, entry: int) -> IncomingRegisterSummary:
        if entry in self._memo:
            return self._memo[entry]
        if entry not in self._bodies:
            return IncomingRegisterSummary("unresolved", "missing callee contract")
        if entry in self._active:
            return IncomingRegisterSummary("unresolved", "recursive callee contract")
        self._active.add(entry)
        try:
            result = self._analyse(entry)
        except RecursionError:
            result = IncomingRegisterSummary("unresolved", "host recursion resource bound")
        finally:
            self._active.remove(entry)
        self._memo[entry] = result
        return result

    def _analyse(self, entry: int) -> IncomingRegisterSummary:
        import capstone

        raw = self._bodies[entry]
        insns = list(_make_decoder().disasm(raw, entry))
        if sum(i.size for i in insns) != len(raw):
            return IncomingRegisterSummary("unresolved", "incomplete instruction bytes")
        instructions = {i.address: i for i in insns}
        allowed = {
            "add",
            "and",
            "call",
            "cmp",
            "dec",
            "inc",
            "lea",
            "leave",
            "mov",
            "or",
            "pop",
            "push",
            "ret",
            "sete",
            "shl",
            "shr",
            "sub",
            "test",
            "xor",
            "nop",
            "fchs",
            "fcomp",
            "fdivrp",
            "fistp",
            "fld",
            "fld1",
            "fldpi",
            "fldz",
            "fmul",
            "fnstsw",
            "fst",
            "fstp",
            "fucompp",
            "wait",
        }
        for insn in insns:
            groups = _group_names(insn)
            if insn.mnemonic not in allowed and "jump" not in groups:
                return IncomingRegisterSummary("unresolved", "unknown opcode effect")
            if insn.prefix[0] in (0xF2, 0xF3):
                return IncomingRegisterSummary("unresolved", "conditional repeat effect")
            if "call" in groups or "jump" in groups:
                if not insn.operands or insn.operands[0].type != capstone.CS_OP_IMM:
                    return IncomingRegisterSummary("unresolved", "indirect control")
                target = insn.operands[0].imm & 0xFFFFFFFF
                if "call" in groups and target not in self._bodies:
                    return IncomingRegisterSummary("unresolved", "missing callee contract")
                if "jump" in groups and target not in instructions:
                    return IncomingRegisterSummary("unresolved", "outside/interior branch target")
        memo: dict[int, frozenset[str]] = {}
        active: set[int] = set()

        def walk(address: int) -> frozenset[str]:
            if address in active:
                return frozenset({"unresolved"})
            if address in memo:
                return memo[address]
            insn = instructions.get(address)
            if insn is None:
                return frozenset({"unresolved"})
            active.add(address)
            effect = _effect(insn, self._register)
            groups = _group_names(insn)
            following = address + insn.size
            if effect == "read":
                outcomes = frozenset({"unsafe"})
            elif effect == "kill":
                outcomes = frozenset({"killed"})
            elif effect == "partial":
                outcomes = frozenset({"unresolved"})
            elif "ret" in groups:
                outcomes = frozenset({"preserved"})
            elif "call" in groups:
                sub = self.summarize(insn.operands[0].imm & 0xFFFFFFFF)
                if sub.kind in ("unsafe", "unresolved", "killed"):
                    outcomes = frozenset({sub.kind})
                else:
                    # Preserve both the dead and live paths of a mixed callee.
                    outcomes = walk(following)
                    if sub.kind == "mixed":
                        outcomes |= frozenset({"killed"})
            elif "jump" in groups:
                outcomes = walk(insn.operands[0].imm & 0xFFFFFFFF)
                if insn.mnemonic != "jmp":
                    outcomes |= walk(following)
            else:
                outcomes = walk(following)
            active.remove(address)
            memo[address] = outcomes
            return outcomes

        outcomes = walk(entry)
        if "unsafe" in outcomes:
            return IncomingRegisterSummary("unsafe", "incoming register read before overwrite")
        if "unresolved" in outcomes:
            return IncomingRegisterSummary("unresolved", "unknown/live cyclic path")
        kind = next(iter(outcomes)) if len(outcomes) == 1 else "mixed"
        return IncomingRegisterSummary(kind, "all live original CFG paths accounted for")


class RegisterSummarySource:
    """Resolve byte-complete original closures; generated C never supplies extents."""

    def __init__(self, image: GuestImage, functions: Path) -> None:
        import csv
        import hashlib
        import io

        from tools.codediff.boundaries import FUNCTION_CSV_COLUMNS, _parse_row
        from tools.harness.image import GuestImage

        if type(image) is not GuestImage or type(image.data) is not bytes:
            raise ValueError("immutable original GuestImage required")
        self._image = image
        self._image_data = image.data
        self._functions = functions
        index_bytes = functions.read_bytes()
        self._index_sha = hashlib.sha256(index_bytes).hexdigest()
        reader = csv.reader(io.StringIO(index_bytes.decode("utf-8"), newline=""))
        if tuple(next(reader, ())) != FUNCTION_CSV_COLUMNS:
            raise ValueError("invalid original function index header")
        rows = [_parse_row(row, functions, line) for line, row in enumerate(reader, start=2)]
        self._spans = {row.entry_va: row for row in rows}
        if len(self._spans) != len(rows):
            raise ValueError("duplicate original function entry")
        self._entries = sorted(self._spans)
        self._image_sha = hashlib.sha256(image.data).hexdigest()
        self._cache: dict[
            tuple[int, str, int], tuple[dict[int, bytes], IncomingRegisterSummary]
        ] = {}

    @classmethod
    def open(cls, image: GuestImage, functions: Path | None) -> RegisterSummarySource | None:
        import csv

        if functions is None:
            return None
        try:
            return cls(image, functions)
        except (OSError, ValueError, csv.Error):
            return None

    def authenticates(self, code_at: CodeAt) -> bool:
        from tools.harness.image import GuestImage

        return (
            getattr(code_at, "__self__", None) is self._image
            and getattr(code_at, "__func__", None) is GuestImage.code_at
        )

    def summarize(
        self, entry: int, register: str, depth_budget: int = MAX_CALL_DEPTH
    ) -> IncomingRegisterSummary:
        import hashlib

        import capstone

        unresolved = IncomingRegisterSummary("unresolved", "unauthenticated original closure")
        if self._image.data is not self._image_data:
            return unresolved
        try:
            if hashlib.sha256(self._functions.read_bytes()).hexdigest() != self._index_sha:
                return unresolved
        except OSError:
            return unresolved
        key = (entry, register, depth_budget)
        if key in self._cache:
            bodies, result = self._cache[key]
            if any(self._image.code_at(at, len(raw)) != raw for at, raw in bodies.items()):
                return unresolved
            return result
        bodies: dict[int, bytes] = {}
        calls: dict[int, list[int]] = {}
        pending = [entry]
        total = 0
        while pending:
            at = pending.pop()
            if at in bodies:
                continue
            row = self._spans.get(at)
            if row is None or len(bodies) >= 64:
                return unresolved
            end = at + row.size_bytes
            if at < 0 or row.size_bytes <= 0 or end > 1 << 32 or row.body_max_va != end - 1:
                return unresolved
            slot = bisect.bisect_left(self._entries, at)
            if slot + 1 < len(self._entries) and self._entries[slot + 1] < end:
                return unresolved
            if slot:
                previous = self._spans[self._entries[slot - 1]]
                if previous.body_max_va >= at:
                    return unresolved
            total += row.size_bytes
            if total > 65536:
                return unresolved
            raw = self._image.code_at(at, row.size_bytes)
            if len(raw) != row.size_bytes:
                return unresolved
            insns = list(_make_decoder().disasm(raw, at))
            if sum(i.size for i in insns) != len(raw):
                return unresolved
            bodies[at] = raw
            calls[at] = []
            for insn in insns:
                if "call" not in _group_names(insn):
                    continue
                if not insn.operands or insn.operands[0].type != capstone.CS_OP_IMM:
                    return unresolved
                target = insn.operands[0].imm & 0xFFFFFFFF
                calls[at].append(target)
                pending.append(target)

        def within_depth(at: int, remaining: int) -> bool:
            if remaining < 0:
                return False
            return all(within_depth(target, remaining - 1) for target in calls[at])

        if not 0 <= depth_budget <= MAX_CALL_DEPTH or not within_depth(entry, depth_budget):
            return IncomingRegisterSummary(
                "unresolved", "original closure exceeds remaining call depth"
            )
        try:
            result = RegisterSummaryProof(bodies, register).summarize(entry)
        except ValueError:
            return unresolved
        provenance = hashlib.sha256()
        provenance.update(self._image_sha.encode())
        provenance.update(self._index_sha.encode())
        for at, raw in sorted(bodies.items()):
            provenance.update(at.to_bytes(4, "little"))
            provenance.update(len(raw).to_bytes(4, "little"))
            provenance.update(raw)
        result = IncomingRegisterSummary(
            result.kind, f"{result.reason}; original closure SHA256 {provenance.hexdigest()}"
        )
        self._cache[key] = (bodies, result)
        return result

    def summarize_flags(
        self, entry: int, live: frozenset[str]
    ) -> tuple[tuple[frozenset[str], ...], str] | None:
        """Call-free, complete original body; exact masks and independent live-return proof.

        No generated extent, partial body, indirect transfer, cycle or nested call is
        summarized. No terminal masks means every live path killed all flags.
        """
        import hashlib

        if self._image.data is not self._image_data:
            return None
        try:
            if self._index_sha != hashlib.sha256(self._functions.read_bytes()).hexdigest():
                return None
        except OSError:
            return None
        row = self._spans.get(entry)
        if row is None or not 0 < row.size_bytes <= 65536:
            return None
        end = entry + row.size_bytes
        if end > 1 << 32 or row.body_max_va != end - 1:
            return None
        slot = bisect.bisect_left(self._entries, entry)
        if slot + 1 < len(self._entries) and self._entries[slot + 1] < end:
            return None
        if slot and self._spans[self._entries[slot - 1]].body_max_va >= entry:
            return None
        raw = self._image.code_at(entry, row.size_bytes)
        insns = list(_make_decoder().disasm(raw, entry))
        if len(raw) != row.size_bytes or sum(i.size for i in insns) != len(raw):
            return None
        if any("call" in _group_names(i) for i in insns):
            return None

        def frozen(address: int, size: int) -> bytes:
            return (
                raw[address - entry : min(address - entry + size, len(raw))]
                if entry <= address < end
                else b""
            )

        observation = ObservationExitProof(frozen).prove(entry, live)
        if observation.kind not in {"all-dead", "live-return", "mixed"}:
            return None
        masks = observation.terminal_live_masks
        if masks:
            cleanup = ExactCleanupProof(frozen).prove(entry)
            if cleanup.pop is None:
                return None
        else:
            cleanup = ExactCleanup(None, "no live return")
        if self._image.code_at(entry, len(raw)) != raw:
            return None
        try:
            if self._index_sha != hashlib.sha256(self._functions.read_bytes()).hexdigest():
                return None
        except OSError:
            return None
        digest = hashlib.sha256()
        digest.update(self._image_sha.encode())
        digest.update(self._index_sha.encode())
        digest.update(entry.to_bytes(4, "little"))
        digest.update(raw)
        proof = (
            f"flag-summary entry {_hex(entry)} live {','.join(sorted(live))}; "
            f"bytes {len(raw)} structural-state-bound {MAX_CLEAN_STATES}; "
            f"original closure SHA256 {digest.hexdigest()}; "
            f"observation SHA256 {observation.source_sha256}; "
            f"return-slot SHA256 {cleanup.source_sha256}"
        )
        # Mixed/all-dead paths are already discharged; only live-return masks
        # continue. A summary with no live masks terminates this path.
        return masks, proof


def _walk(
    code_at: CodeAt,
    decoder: Any,
    start: int,
    register: str,
    max_insns: int,
    depth: int,
    memo: Memo,
    active: frozenset[tuple[int, str]],
    summaries: RegisterSummarySource | None = None,
    strict_returns: bool = False,
) -> _Walk:
    """Walk forward from `start`. `depth` 0 is the caller, 1 and up are callees."""
    import capstone

    own = (start, register)
    pending: deque[tuple[int, bool]] = deque([(start, strict_returns)])
    visited: set[tuple[int, bool]] = set()
    safe_notes: list[str] = []
    unresolved_reason: str | None = None
    capped = False
    cuts: set[tuple[int, str]] = set()
    summarized = strict_returns
    unread_return = False

    def note_unresolved(reason: str) -> None:
        nonlocal unresolved_reason
        if unresolved_reason is None:
            unresolved_reason = reason

    def callee_walk(
        target: int, call_address: int, following: int, strict_live: bool
    ) -> _Walk | None:
        """Walk a direct callee. Returns an UNSAFE result to propagate, else None."""
        nonlocal capped, summarized
        key = (target, register)
        where = f"in callee {_hex(target)} called at {_hex(call_address)}"
        if depth >= MAX_CALL_DEPTH:
            capped = True
            note_unresolved(f"call chain deeper than {MAX_CALL_DEPTH} at {_hex(call_address)}")
            return None
        if key in active:
            # A recursive call. The walk already in progress decides what this path would.
            cuts.add(key)
            return None
        sub = None if strict_live else memo.get(key)
        if sub is None:
            sub = _walk(
                code_at,
                decoder,
                target,
                register,
                max_insns,
                depth + 1,
                memo,
                active | {key},
                summaries,
                strict_live,
            )
        summarized = summarized or sub.summarized
        cuts.update(sub.cuts)
        capped = capped or sub.capped
        if sub.kind == "unsafe":
            return _Walk("unsafe", f"{sub.reason} {where}")
        if sub.kind == "unresolved":
            note_unresolved(f"{sub.reason} {where}")
        else:
            safe_notes.append(f"{sub.reason} {where}")
            if sub.summarized and sub.preserved:
                pending.append((following, True))
        return None

    def finish(result: _Walk) -> _Walk:
        open_cuts = frozenset(cuts - {own})
        result = _Walk(
            result.kind,
            result.reason,
            capped,
            open_cuts,
            result.summarized or summarized,
            result.preserved or unread_return,
        )
        if (
            depth >= 1
            and not result.summarized
            and (result.kind == "unsafe" or not (capped or open_cuts))
        ):
            memo[own] = _Walk(
                result.kind, result.reason, summarized=result.summarized, preserved=result.preserved
            )
        return result

    while pending:
        address, strict_live = pending.popleft()
        state = (address, strict_live)
        if state in visited:
            continue
        if len(visited) >= max_insns:
            if (
                register == "edx"
                and depth >= 1
                and summaries is not None
                and summaries.authenticates(code_at)
                and unresolved_reason is None
                and not capped
                and not cuts
            ):
                summary = summaries.summarize(start, register, MAX_CALL_DEPTH - depth)
                if summary.kind == "unsafe":
                    return finish(_Walk("unsafe", summary.reason, summarized=True))
                if summary.kind in {"killed", "preserved", "mixed"}:
                    return finish(
                        _Walk(
                            "safe",
                            summary.reason,
                            summarized=True,
                            preserved=summary.kind != "killed",
                        )
                    )
                note_unresolved(summary.reason)
            note_unresolved(f"more than {max_insns} instructions without a verdict")
            break
        insn = _decode_at(decoder, code_at, address)
        if insn is None:
            note_unresolved(f"no readable instruction at {_hex(address)}")
            continue
        visited.add(state)

        effect = _effect(insn, register)
        if effect == "read":
            return finish(_Walk("unsafe", f"read by {_hex(address)} first"))
        if effect == "kill":
            safe_notes.append(f"written by {_hex(address)} before any read")
            continue

        groups = _group_names(insn)
        following = address + insn.size
        operands = insn.operands
        direct = len(operands) >= 1 and operands[0].type == capstone.CS_OP_IMM
        if "ret" in groups:
            unread_return = True
            if register == "eax":
                note_unresolved(f"returns with eax live at {_hex(address)}")
            else:
                safe_notes.append(f"returns at {_hex(address)} with {register} unread")
        elif "call" in groups:
            if register == "eax":
                safe_notes.append(f"call at {_hex(address)} overwrites eax")
            elif not direct:
                note_unresolved(f"indirect call at {_hex(address)} before {register} is rewritten")
            else:
                unsafe = callee_walk(operands[0].imm & 0xFFFFFFFF, address, following, strict_live)
                if unsafe is not None:
                    return finish(unsafe)
        elif insn.mnemonic in _TRAPS or "int" in groups:
            note_unresolved(f"trap at {_hex(address)}")
        elif "jump" in groups or "branch_relative" in groups:
            if not direct:
                note_unresolved(f"indirect jump at {_hex(address)}")
                continue
            pending.append((operands[0].imm & 0xFFFFFFFF, strict_live))
            if insn.mnemonic != "jmp":
                pending.append((following, strict_live))
        else:
            pending.append((following, strict_live))

    if unresolved_reason is not None:
        return finish(_Walk("unresolved", unresolved_reason))
    if safe_notes:
        return finish(_Walk("safe", " and ".join(dict.fromkeys(safe_notes[:3]))))
    return finish(_Walk("safe", "no path reads it"))


# Arithmetic flags are independent resources: INC preserves CF; SAHF preserves OF.
# DF remains the existing calling-convention precondition, not an arithmetic result.
_ARITHMETIC_FLAGS = ("CF", "PF", "AF", "ZF", "SF", "OF")


def _flag_effect(insn: Any) -> tuple[frozenset[str], frozenset[str]]:
    from capstone import x86

    all_flags = frozenset(_ARITHMETIC_FLAGS)
    if insn.mnemonic in {"fcomi", "fcomip", "fucomi", "fucomip"}:
        return frozenset(), all_flags
    # FCMOV consumes integer flags despite the FPU/eflags metadata union.
    fcmov = {
        "fcmovb": {"CF"},
        "fcmovnb": {"CF"},
        "fcmove": {"ZF"},
        "fcmovne": {"ZF"},
        "fcmovbe": {"CF", "ZF"},
        "fcmovnbe": {"CF", "ZF"},
        "fcmovu": {"PF"},
        "fcmovnu": {"PF"},
    }
    if insn.mnemonic in fcmov:
        return frozenset(fcmov[insn.mnemonic]), frozenset()
    # Capstone overlays FPU status metadata on eflags. Ordinary x87 is flag-neutral.
    if insn.mnemonic.startswith("f") or insn.mnemonic in {"wait", "emms"}:
        return frozenset(), frozenset()
    reads = {flag for flag in all_flags if insn.eflags & getattr(x86, "X86_EFLAGS_TEST_" + flag, 0)}
    writes = {
        flag
        for flag in all_flags
        if any(
            insn.eflags & getattr(x86, "X86_EFLAGS_" + action + "_" + flag, 0)
            for action in ("MODIFY", "RESET", "SET")
        )
    }
    # A zero shift count and a zero REP count can preserve every old flag.
    # Do not infer unconditional flag death from Capstone's may-write metadata.
    if insn.mnemonic.split()[-1] in {
        "shl",
        "sal",
        "shr",
        "sar",
        "shld",
        "shrd",
        "rol",
        "ror",
        "rcl",
        "rcr",
    }:
        from capstone import CS_OP_IMM

        count = insn.operands[-1]
        # OF is undefined for shifts of more than one bit, even when Capstone
        # reports MODIFY_OF without distinguishing the operand count.
        if count.type == CS_OP_IMM and count.imm & 31 > 1:
            writes.discard("OF")
        if count.type == CS_OP_IMM and count.imm & 31 >= insn.operands[0].size * 8:
            # Narrow shifts can leave CF undefined; oversized double shifts have
            # wholly undefined results. Neither is a definite old-flag kill.
            writes.discard("CF")
            if insn.mnemonic in {"shld", "shrd"}:
                writes.clear()
        if (
            count.type != CS_OP_IMM
            or count.imm & 31 == 0
            or insn.mnemonic in {"rol", "ror", "rcl", "rcr"}
        ):
            writes.clear()
    # R11: exact one-bit non-carry rotates define CF/OF without reading old
    # flags. All other counts remain conservative, including width multiples.
    if insn.mnemonic in {"rol", "ror"}:
        from capstone import CS_OP_IMM

        count = insn.operands[-1]
        if count.type == CS_OP_IMM and count.imm & 31 == 1 and insn.operands[0].size in (1, 2, 4):
            writes = {"CF", "OF"}
    if insn.prefix[0] in (0xF2, 0xF3):
        writes.clear()
    # T1585: AND/OR/XOR/TEST leave AF architecturally undefined, but it is a
    # deterministic function of their operands in shared code on both sides, so the old
    # AF is unobservable once the op runs. Exact mnemonic allowlist ("lock and" is not in
    # it), after the REP/REPNE clear above. Capstone drops a meaningless F2/F3 on AND.
    elif insn.mnemonic in {"and", "or", "xor", "test"}:
        writes.add("AF")
    # Implicit reads absent from Capstone's TEST metadata.
    if insn.mnemonic in {"pushf", "pushfd"}:
        reads.update(all_flags)
    elif insn.mnemonic == "lahf":
        reads.update(all_flags - {"OF"})
    elif insn.mnemonic == "cmc":
        reads.add("CF")
    elif insn.mnemonic in {"daa", "das"}:
        reads.update({"CF", "AF"})
    elif insn.mnemonic in {"aaa", "aas"}:
        reads.add("AF")
    return frozenset(reads), frozenset(writes)


def _push_return(stack: tuple[Any, ...], following: int) -> tuple[Any, ...]:
    """Push a return address, folding a recursive cycle into one summary entry.

    A return address already on the stack means a call cycle (self or mutual recursion).
    The frames from its first occurrence up are replaced by a frozenset of their return
    sites, standing for one or more frames returning to any of them in any order. That is
    a superset of every real return order, so a flag verdict over it stays sound, and the
    stack no longer grows with the recursion depth.

    Soundness (T712): every real stack is a path through the folded one, because a fold
    only ever widens (more frames, any order, any site of the cycle), so no verdict that is
    SAFE here is unsafe for some real return order. The variants "keep only", "pop only",
    "one site" and "smaller site set" are NOT equivalent to this and are pinned apart by
    tests/test_replace_recursion.py (fuzzed shapes, some give SAFE where this gives
    UNRESOLVED or UNSAFE).
    """
    for index, entry in enumerate(stack):
        if entry == following or (isinstance(entry, frozenset) and following in entry):
            sites: set[int] = set()
            for member in stack[index:]:
                sites.update(member if isinstance(member, frozenset) else {member})
            return (*stack[:index], frozenset(sites))
    return (*stack, following)


def _walk_flags(
    code_at: CodeAt,
    decoder: Any,
    start: int,
    max_insns: int,
    live: frozenset[str] = frozenset(_ARITHMETIC_FLAGS),
    climb: CallerClimb | None = None,
    summaries: RegisterSummarySource | None = None,
) -> _Walk:
    """Walk the original code from `start` tracking which original arithmetic flags are live.

    The walk keeps the stack of return addresses of the direct calls it followed, so a `ret`
    inside a followed callee resumes after that call with the flags still live (the usual
    balanced call/ret assumption the lifter makes too). A `ret` with an empty stack leaves
    the function the walk started in. With a `climb` whose caller set for that function is
    proven complete the walk continues at every caller's return site (up to
    MAX_FLAG_CLIMB_DEPTH functions up), otherwise it stays UNRESOLVED. A state already
    visited adds no new outcome, but a walk in which no path ever killed the flags is not SAFE.
    """
    import hashlib

    import capstone

    # A state is (address, live flags, return stack, frame entry, functions climbed).
    first_frame = climb.owner(start) if climb is not None else None
    stack_audit = StackAudit(code_at, decoder, climb.pointer_proof if climb is not None else None)
    # R9: each pruned stack proof is scoped to the original flags still live at
    # this call. Separate instances prevent one subset's memoized result from
    # proving another subset or an unpruned frame obligation.
    pruned_audits = {frozenset(_ARITHMETIC_FLAGS): stack_audit}
    pending = deque([(start, live, (), first_frame, 0)])
    visited: set[tuple[int, frozenset[str], tuple[int, ...], int | None]] = set()
    unresolved: str | None = None
    killed = False
    proof: list[str] = []
    while pending:
        address, remaining, stack, frame, climbed = pending.popleft()
        key = (address, remaining, stack, frame)
        if key in visited:
            continue
        if len(visited) >= max_insns:
            unresolved = unresolved or f"more than {max_insns} flag states without a verdict"
            break
        visited.add(key)
        insn = _decode_at(decoder, code_at, address)
        if insn is None:
            unresolved = unresolved or f"no readable instruction at {_hex(address)}"
            continue
        proof.append(
            f"state {_hex(address)} live {','.join(sorted(remaining))} "
            f"depth {len(stack)} climbed {climbed}; "
            f"instruction SHA256 {hashlib.sha256(bytes(insn.bytes)).hexdigest()}"
        )
        reads, writes = _flag_effect(insn)
        if reads & remaining:
            return _Walk("unsafe", f"arithmetic flags read by {_hex(address)} first")
        remaining -= writes
        if not remaining:
            killed = True
            continue
        groups = _group_names(insn)
        following = address + insn.size
        direct = bool(insn.operands) and insn.operands[0].type == capstone.CS_OP_IMM
        if "ret" in groups:
            if stack:
                top = stack[-1]
                if isinstance(top, frozenset):
                    # One or more frames of a recursive cycle: the innermost returns to
                    # any member site, with more cycle frames left or none.
                    for site in sorted(top):
                        pending.append((site, remaining, stack, frame, climbed))
                        pending.append((site, remaining, stack[:-1], frame, climbed))
                else:
                    pending.append((top, remaining, stack[:-1], frame, climbed))
            elif climb is None or frame is None:
                unresolved = unresolved or f"returns with arithmetic flags live at {_hex(address)}"
            elif climbed >= MAX_FLAG_CLIMB_DEPTH:
                unresolved = (
                    unresolved or f"bounded caller climb with flags live at {_hex(address)}"
                )
            elif not (body := stack_audit.clean(frame, False)).clean:
                unresolved = unresolved or (
                    f"returns with flags live at {_hex(address)}, frame {_hex(frame)} "
                    f"may not return through its own slot: {body.reason}"
                )
            else:
                callers, why = climb.callers(frame)
                if callers is None:
                    unresolved = unresolved or (
                        f"returns with flags live at {_hex(address)}, caller set unproven: {why}"
                    )
                proof.append(
                    f"caller-climb frame {_hex(frame)} to "
                    f"{','.join(_hex(site) for site in sorted(callers or ()))}; "
                    f"complete caller set; return-slot {body.reason}"
                )
                for site in callers or ():
                    pending.append((site, remaining, (), climb.owner(site), climbed + 1))
        elif "call" in groups:
            if not direct:
                unresolved = unresolved or f"indirect call with flags live at {_hex(address)}"
                continue
            target = insn.operands[0].imm & 0xFFFFFFFF
            pushed = _push_return(stack, following)
            if len(pushed) > MAX_FLAG_CALL_DEPTH:
                unresolved = unresolved or f"bounded call chain with flags live at {_hex(address)}"
                continue
            if summaries is not None and summaries.authenticates(code_at):
                summary = summaries.summarize_flags(target, remaining)
                if summary is not None:
                    masks, evidence = summary
                    proof.append(evidence)
                    if not masks:
                        killed = True
                    for mask in masks:
                        pending.append((following, mask, stack, frame, climbed))
                    continue
            if remaining not in pruned_audits:
                pruned_audits[remaining] = StackAudit(
                    code_at,
                    decoder,
                    climb.pointer_proof if climb is not None else None,
                    prune_flags=remaining,
                )
            observation = ObservationExitProof(code_at).prove(
                target, remaining, MAX_FLAG_CALL_DEPTH - len(pushed)
            )
            if observation.kind == "read" and observation.read is not None:
                return _Walk(
                    "unsafe",
                    f"arithmetic flags read by {_hex(observation.read.address)} first",
                )
            if observation.source_sha256:
                proof.append(
                    f"observation-{observation.kind} entry {_hex(target)} "
                    f"live {','.join(sorted(remaining))}; "
                    f"source SHA256 {observation.source_sha256}"
                )
            if observation.kind == "all-dead":
                killed = True
                continue
            body = pruned_audits[remaining].clean(target, True)
            if not body.clean:
                unresolved = unresolved or (
                    f"callee {_hex(target)} called at {_hex(address)} may not return through "
                    f"its own slot: {body.reason}"
                )
                continue
            pending.append((target, remaining, pushed, frame, climbed))
        elif insn.mnemonic in _TRAPS or "int" in groups:
            unresolved = unresolved or f"trap at {_hex(address)}"
        elif "jump" in groups or "branch_relative" in groups:
            if not direct:
                unresolved = unresolved or f"indirect jump at {_hex(address)}"
                continue
            pending.append((insn.operands[0].imm & 0xFFFFFFFF, remaining, stack, frame, climbed))
            if insn.mnemonic != "jmp":
                pending.append((following, remaining, stack, frame, climbed))
        else:
            pending.append((following, remaining, stack, frame, climbed))
    if unresolved:
        return _Walk("unresolved", unresolved)
    if not killed:
        return _Walk("unresolved", "no path killed the arithmetic flags (cycle)")
    return _Walk(
        "safe", "all original arithmetic flags killed before any read", proof_path=tuple(proof)
    )


def audit_site(
    code_at: CodeAt,
    return_va: int,
    register: str,
    *,
    max_insns: int = MAX_INSNS,
    decoder: Any | None = None,
    memo: Memo | None = None,
    climb: CallerClimb | None = None,
    summaries: RegisterSummarySource | None = None,
) -> SiteVerdict:
    """Walk the original code from `return_va` and decide what happens to `register`.

    `climb` (EFLAGS only) lets the walk continue into the callers of the function that holds
    `return_va` when their set is proven complete.

    A DIRECT call reached with ecx or edx still live is followed into the callee, up to
    MAX_CALL_DEPTH calls deep. `memo` caches callee results across sites and may be shared
    by every site audited against the same image and `max_insns`.
    """
    if register == "eflags":
        result = _walk_flags(
            code_at,
            decoder if decoder is not None else _make_decoder(),
            return_va,
            max_insns,
            climb=climb,
        )
        # Preserve every decided legacy verdict. Only an UNRESOLVED flag walk
        # may consume independently authenticated summaries on a fresh pass.
        if (
            result.kind == "unresolved"
            and summaries is not None
            and summaries.authenticates(code_at)
        ):
            result = _walk_flags(
                code_at,
                decoder if decoder is not None else _make_decoder(),
                return_va,
                max_insns,
                climb=climb,
                summaries=summaries,
            )
        return SiteVerdict(
            return_va,
            register,
            result.kind,
            result.reason,
            unresolved_reason_code(result.reason) if result.kind == "unresolved" else "",
            result.proof_path,
        )
    if register not in _FAMILY:
        raise ValueError(f"cannot audit register {register!r}, expected one of {AUDITED_REGISTERS}")
    result = _walk(
        code_at,
        decoder if decoder is not None else _make_decoder(),
        return_va,
        register,
        max_insns,
        0,
        memo if memo is not None else {},
        frozenset(),
        summaries,
    )
    return SiteVerdict(
        return_va,
        register,
        result.kind,
        result.reason,
        unresolved_reason_code(result.reason) if result.kind == "unresolved" else "",
    )


# ---------------------------------------------------------------------------------------
# A function, a manifest
# ---------------------------------------------------------------------------------------


def audit_function(
    entry: ManifestEntry,
    gen_dir: Path,
    image: GuestImage,
    *,
    sites: dict[int, tuple[set[int], int]] | None = None,
    code_ranges: Iterable[tuple[int, int]] = (),
    max_insns: int = MAX_INSNS,
    memo: Memo | None = None,
    climb: CallerClimb | None = None,
    bridge: BridgeExemption | None = None,
    summaries: RegisterSummarySource | None = None,
) -> FunctionAudit:
    """Audit every scratch register of `entry` at every direct call site of it.

    `sites` is a precomputed `find_call_sites` result, so an audit of many functions reads
    the lifted tree once.
    """
    for register in entry.scratch:
        if register not in _FAMILY:
            raise ValueError(f"{entry.name}: scratch register {register!r} cannot be audited")
    found = sites if sites is not None else find_call_sites(gen_dir, {entry.va})
    return_vas, tail_jumps = found[entry.va]
    decoder = _make_decoder()
    shared = memo if memo is not None else {}
    verdicts = tuple(
        audit_site(
            image.code_at,
            return_va,
            register,
            max_insns=max_insns,
            decoder=decoder,
            memo=shared,
            summaries=summaries,
        )
        for return_va in sorted(return_vas)
        for register in AUDITED_REGISTERS
        if register in entry.scratch
    )
    return FunctionAudit(
        va=entry.va,
        direct_sites=len(return_vas),
        tail_jumps=tail_jumps,
        data_references=count_data_references(image, entry.va, code_ranges=code_ranges),
        verdicts=verdicts,
        flags=tuple(
            _bridged(
                audit_site(
                    image.code_at,
                    va,
                    "eflags",
                    max_insns=max(max_insns, MAX_FLAG_STATES),
                    decoder=decoder,
                    climb=climb,
                    summaries=summaries,
                ),
                entry,
                bridge,
                gen_dir,
            )
            for va in sorted(return_vas)
        ),
    )


def _bridged(
    verdict: SiteVerdict, entry: ManifestEntry, bridge: BridgeExemption | None, gen_dir: Path
) -> SiteVerdict:
    """Answer an unsafe or unresolved flag verdict from the flag bridge when it provably covers."""
    if bridge is None or verdict.verdict == "safe":
        return verdict
    proof = bridge.covers(entry.va, entry.source, verdict.return_va, gen_dir)
    if not proof:
        return verdict
    return SiteVerdict(verdict.return_va, verdict.register, "safe", proof)


def read_function_starts(functions: Path | None) -> list[int]:
    """Entry VAs of `generated/retail/functions.csv` (first column), empty when not given."""
    if functions is None or not functions.is_file():
        return []
    starts = []
    for line in functions.read_text(encoding="utf-8").splitlines()[1:]:
        field = line.split(",", 1)[0]
        if field.startswith("0x"):
            starts.append(int(field, 16))
    return starts


def audit_all(
    entries: Iterable[ManifestEntry],
    gen_dir: Path,
    image: GuestImage,
    *,
    code_ranges: Iterable[tuple[int, int]] = (),
    data_ranges: Iterable[tuple[int, int]] = (),
    max_insns: int = MAX_INSNS,
    bridge: BridgeExemption | None = None,
    functions: Path | None = None,
) -> list[FunctionAudit]:
    """Audit every entry, reading the lifted tree once. Sorted by address."""
    summaries = RegisterSummarySource.open(image, functions)
    wanted = list(entries)
    index = build_caller_index(gen_dir)
    sites = {
        entry.va: (index.sites.get(entry.va, set()), index.tails.get(entry.va, 0))
        for entry in wanted
    }
    ranges = tuple(code_ranges)
    climb = CallerClimb(
        index,
        image,
        code_ranges=ranges,
        data_ranges=data_ranges,
        function_starts=read_function_starts(functions),
    )
    memo: Memo = {}
    return [
        audit_function(
            entry,
            gen_dir,
            image,
            sites=sites,
            code_ranges=ranges,
            max_insns=max_insns,
            memo=memo,
            climb=climb,
            bridge=bridge,
            summaries=summaries,
        )
        for entry in sorted(wanted, key=lambda entry: entry.va)
    ]


def unresolved_reason_code(reason: str) -> str:
    """Stable first-refusal classes; the detailed reason retains original addresses."""
    text = reason.lower()
    for needles, code in (
        (("indirect",), "indirect-transfer"),
        (("readable", "immutable bytes", "incomplete instruction"), "unreadable-bytes"),
        (("call chain", "call depth"), "callee-depth"),
        (("flag states",), "flag-walk-budget"),
        (("budget", "bound", "too many"), "closure-budget"),
        (("caller set unproven",), "caller-set-unproven"),
        (("own slot",), "return-slot-unproven"),
        (("returns with",), "live-return"),
        (("cycle", "recursion"), "live-cycle"),
        (("trap",), "trap"),
    ):
        if any(needle in text for needle in needles):
            return code
    return "closure-unproven"


def _site_record(verdict: SiteVerdict) -> dict[str, Any]:
    record: dict[str, Any] = {
        "return_va": f"0x{verdict.return_va:08x}",
        "register": verdict.register,
        "reason": verdict.reason,
    }
    if verdict.reason_code:
        record["reason_code"] = verdict.reason_code
    if verdict.proof_path:
        record["proof_path"] = list(verdict.proof_path)
    return record


def write_audit(path: Path, audits: Iterable[FunctionAudit], *, manifest_sha: str) -> None:
    """Write the audit as JSON (schema 2). Counts and addresses only, no code bytes.

    The file describes the user's binary, so it belongs under `generated/replace/`
    (gitignored) and never in the repository.
    """
    functions = []
    for audit in sorted(audits, key=lambda item: item.va):
        functions.append(
            {
                "va": f"0x{audit.va:08x}",
                "direct_sites": audit.direct_sites,
                "tail_jumps": audit.tail_jumps,
                "data_references": audit.data_references,
                "safe": audit.safe,
                "unsafe": audit.unsafe,
                "unresolved": audit.unresolved,
                "eligible": audit.eligible,
                "safe_flag_sites": [
                    _site_record(item) for item in audit.flags if item.verdict == "safe"
                ],
                "unsafe_sites": [
                    _site_record(item)
                    for item in (*audit.verdicts, *audit.flags)
                    if item.verdict == "unsafe"
                ],
                "unresolved_sites": [
                    _site_record(item)
                    for item in (*audit.verdicts, *audit.flags)
                    if item.verdict == "unresolved"
                ],
            }
        )
    document = {"schema": SCHEMA, "manifest_sha": manifest_sha, "functions": functions}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
