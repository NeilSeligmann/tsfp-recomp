"""
Disassembly engine using Capstone.

Provides linear sweep and recursive descent disassembly of x86-32 code,
with instruction classification and operand analysis.
"""

import bisect
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CsInsn
from capstone import CS_OP_IMM, CS_OP_MEM, CS_OP_REG

from . import config
from .loader import BinaryImage, SectionInfo


@dataclass
class Instruction:
    """A decoded instruction with metadata."""
    address: int
    size: int
    mnemonic: str
    op_str: str
    bytes_hex: str

    # Classification
    is_call: bool = False
    is_ret: bool = False
    is_jump: bool = False
    is_cond_jump: bool = False
    is_nop: bool = False

    # Resolved targets
    call_target: Optional[int] = None     # For direct calls
    jump_target: Optional[int] = None     # For direct jumps
    memory_ref: Optional[int] = None      # For [addr] references
    jump_table: Optional[int] = None      # For `jmp [reg*4 + table]`
    imm_ref: Optional[int] = None         # For `push offset x` / `mov reg, offset x`

    @property
    def is_branch(self) -> bool:
        return self.is_jump or self.is_cond_jump

    @property
    def is_terminator(self) -> bool:
        return self.is_ret or self.is_jump  # Unconditional terminator

    @property
    def end_address(self) -> int:
        return self.address + self.size

    def to_dict(self) -> dict:
        d = {
            "address": f"0x{self.address:08X}",
            "size": self.size,
            "mnemonic": self.mnemonic,
            "op_str": self.op_str,
            "bytes": self.bytes_hex,
        }
        if self.call_target is not None:
            d["call_target"] = f"0x{self.call_target:08X}"
        if self.jump_target is not None:
            d["jump_target"] = f"0x{self.jump_target:08X}"
        if self.memory_ref is not None:
            d["memory_ref"] = f"0x{self.memory_ref:08X}"
        if self.imm_ref is not None:
            d["imm_ref"] = f"0x{self.imm_ref:08X}"
        return d


class DisasmEngine:
    """
    x86-32 disassembly engine backed by Capstone.

    Supports both linear sweep (complete coverage) and recursive descent
    (reachability validation) modes.
    """

    def __init__(self, image: BinaryImage):
        self.image = image
        self._cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self._cs.detail = True

        # Instruction cache: address -> Instruction
        self.instructions: Dict[int, Instruction] = {}

        # Sorted address list (built lazily for range queries)
        self._sorted_addrs: Optional[List[int]] = None

        # Embedded jump tables: table VA -> end VA (exclusive).
        # Populated by resync_jump_tables() from the indexed indirect jumps
        # recorded during the sweep.
        self.jump_tables: Dict[int, int] = {}
        self._jt_candidates: Set[int] = set()
        # TSFP-LOCAL (patch 08): table base -> [(dispatch VA, index register)].
        self._jt_sites: Dict[int, List[Tuple[int, str]]] = {}
        # table base -> (greedy entries, kept entries, which terminator fired)
        self.jump_table_trims: Dict[int, Tuple[int, int, str]] = {}
        # TSFP-LOCAL (patch 14): dispatch displacement -> table START, recorded
        # only where the two differ (see _offset_table). jump_tables is keyed by
        # where the table really starts, because that is where the function
        # walk meets it, so a dispatch needs this to find its arms.
        self._jt_alias: Dict[int, int] = {}

    def _classify_instruction(self, cs_insn: CsInsn) -> Instruction:
        """Convert a Capstone instruction to our Instruction type."""
        mnemonic = cs_insn.mnemonic
        insn = Instruction(
            address=cs_insn.address,
            size=cs_insn.size,
            mnemonic=mnemonic,
            op_str=cs_insn.op_str,
            bytes_hex=cs_insn.bytes.hex(),
        )

        # Classify by mnemonic
        insn.is_call = mnemonic in config.CALL_MNEMONICS
        insn.is_ret = mnemonic in config.RET_MNEMONICS
        insn.is_jump = mnemonic in config.JMP_MNEMONICS
        insn.is_cond_jump = mnemonic in config.COND_JMP_MNEMONICS
        insn.is_nop = mnemonic in config.NOP_MNEMONICS

        # Resolve operand targets using Capstone detail
        try:
            operands = cs_insn.operands
        except Exception:
            operands = []

        if operands:
            op = operands[0]

            if insn.is_call:
                if op.type == CS_OP_IMM:
                    insn.call_target = op.imm & 0xFFFFFFFF
                elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    insn.memory_ref = op.mem.disp & 0xFFFFFFFF

            elif insn.is_jump or insn.is_cond_jump:
                if op.type == CS_OP_IMM:
                    insn.jump_target = op.imm & 0xFFFFFFFF
                elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    insn.memory_ref = op.mem.disp & 0xFFFFFFFF
                elif op.type == CS_OP_MEM and op.mem.index != 0:
                    # `jmp dword ptr [reg*4 + disp]` -- a switch dispatch. disp
                    # is the address of a table of code pointers, and MSVC
                    # parks that table inside the function body, right after
                    # this instruction. See resync_jump_tables().
                    disp = op.mem.disp & 0xFFFFFFFF
                    if self.image.base_address <= disp < (
                            self.image.base_address + self.image.image_size):
                        insn.jump_table = disp
                        self._jt_candidates.add(disp)
                        # TSFP-LOCAL (patch 08): remember WHICH register
                        # indexes this table and where the dispatch is, so
                        # resync_jump_tables can find the guest's own range
                        # check on that register. Without it the table has no
                        # terminator but the section end -- see that method.
                        try:
                            reg = self._cs.reg_name(op.mem.index) or ""
                        except Exception:
                            reg = ""
                        self._jt_sites.setdefault(disp, []).append(
                            (insn.address, reg))

            # Check for memory references in non-branch instructions
            if not (insn.is_call or insn.is_branch) and insn.memory_ref is None:
                for operand in operands:
                    if (operand.type == CS_OP_MEM and
                            operand.mem.base == 0 and operand.mem.index == 0):
                        addr = operand.mem.disp & 0xFFFFFFFF
                        if self.image.base_address <= addr < (
                                self.image.base_address + self.image.image_size):
                            insn.memory_ref = addr
                            break

            # Immediate operands that are addresses, e.g. `push offset str` or
            # `mov reg, offset table`. These are how string literals and static
            # data are almost always referenced, and they are NOT memory
            # operands, so the memory_ref scan above never sees them. On calls
            # and branches the immediate is the target, handled above.
            if not (insn.is_call or insn.is_branch):
                for operand in operands:
                    if operand.type != CS_OP_IMM:
                        continue
                    addr = operand.imm & 0xFFFFFFFF
                    if self.image.base_address <= addr < (
                            self.image.base_address + self.image.image_size):
                        insn.imm_ref = addr
                        break

        return insn

    def linear_sweep(self, section: SectionInfo,
                     progress_callback=None) -> int:
        """
        Linear sweep disassembly of a section.

        Decodes all bytes sequentially. On invalid byte sequences,
        skips forward and resumes decoding.

        Returns number of instructions decoded.
        """
        data = self.image.get_section_data(section)
        if not data:
            return 0

        va_start = section.virtual_addr
        total = len(data)
        count = 0
        offset = 0

        while offset < total:
            # Decode from current offset to end of section
            chunk = data[offset:]
            chunk_va = va_start + offset
            last_end_offset = offset

            for cs_insn in self._cs.disasm(chunk, chunk_va):
                insn = self._classify_instruction(cs_insn)
                self.instructions[insn.address] = insn
                count += 1
                last_end_offset = (cs_insn.address + cs_insn.size) - va_start

            # Progress reporting
            if progress_callback and last_end_offset > offset:
                progress_callback(min(last_end_offset, total), total)

            if last_end_offset > offset:
                offset = last_end_offset
            else:
                # Capstone couldn't decode anything - skip one byte
                offset += 1

            # If we've decoded to near the end, we're done
            if offset >= total:
                break

        # Invalidate sorted address cache
        self._sorted_addrs = None

        if progress_callback:
            progress_callback(total, total)

        return count

    def resync_jump_tables(self, min_entries: int = 3,
                           max_entries: int = 512) -> int:
        """
        Treat MSVC's embedded switch tables as data and realign after them.

        `jmp dword ptr [reg*4 + disp]` dispatches through a table of code
        pointers, and MSVC emits that table inline -- inside the function, on
        the fall-through path, immediately after the jump. linear_sweep has no
        way to know, so it decodes the pointers as instructions and comes out
        the far side out of phase. Every byte after it belongs to an
        instruction that does not exist, up to wherever the stream happens to
        resync.

        That is not a cosmetic loss. It routinely eats the function's own
        epilogue, so the `pop esi / pop edi` balancing the prologue is never
        lifted and **every caller silently loses those registers**. Half-Life 2
        hits it in the CRT: memcpy and memmove both carry a tail-copy table, so
        the C++ static-initialiser loop -- which walks its constructor array
        with esi as the cursor and edi as the limit -- had both clobbered by
        its own callees and stopped after 8% of the list, leaving Source with
        no registered interfaces to hand CreateInterface.

        Fix it where it starts: measure each table, drop the instructions the
        sweep hallucinated over it, and decode_at() past the end to pick the
        real code back up. A table entry must point into an executable section
        for the table to keep growing, which is what bounds it.

        Returns the number of tables resynced.

        TSFP-LOCAL (patch 08): the scan below has a TERMINATOR now.

        "A table entry must point into an executable section for the table to
        keep growing" is not a terminator. MSVC parks sibling switch tables
        back to back, so every dword of the NEXT table is also a valid address
        in the same section and the scan walks straight through it. Measured on
        our binary, that concatenation produced **470 spurious arms at 65 of
        707 dispatch sites -- 8.7% of all emitted arms**. At 0x0001CE11 the
        compiler's own `cmp eax, 6` establishes 7 arms; the greedy scan read
        35, merging four adjacent tables.

        The cost is not a wrong count in a report. Each spurious arm is emitted
        as a reachable `if (_jt == 0x...) goto ...` in the generated C -- a
        value the compiler's own range check explicitly excludes. At runtime
        that is a silent mis-dispatch to the wrong block: no fault, no log,
        wrong behaviour arbitrarily far from the cause.

        Two terminators, in order of authority:

        1. **The guest's own range check.** A switch dispatch is preceded by
           `cmp <index>, N` / `ja <default>`; N + 1 is the arm count, stated by
           the compiler. All 65 of the over-read tables have one.
        2. **The next known table base.** Where no bound is readable, a table
           cannot extend into another dispatch's base.

        Both only ever SHORTEN the greedy result, so neither can manufacture a
        table, and a table with neither signal behaves exactly as before.
        """
        resynced = 0
        bases = sorted(self._jt_candidates)
        for tbl in sorted(self._jt_candidates):
            # XBEs mark .rdata and .data executable, so "points at an
            # executable section" alone would let an array of data pointers
            # pass as a jump table -- and resyncing over real instructions is
            # far worse than missing a table. A switch never jumps out of its
            # own section, so require that.
            home = self.image.get_section_at_va(tbl)
            if home is None:
                continue
            lo = home.virtual_addr
            hi = lo + home.virtual_size
            entries = 0
            while entries < max_entries:
                target = self.image.read_u32_at_va(tbl + entries * 4)
                if target is None or not (lo <= target < hi):
                    break
                entries += 1
            # TSFP-LOCAL (patch 08): apply the two terminators. Both only
            # shorten, and min_entries is still enforced afterwards, so a
            # terminator can never turn a real table into a false one.
            greedy = entries
            bound = self._jt_bound(tbl)
            if bound is not None and bound < entries:
                entries = bound
                self.jump_table_trims[tbl] = (greedy, entries, "bound")

            # TSFP-LOCAL (T89, follows patch 15): a two-level dispatch keeps
            # its byte map DIRECTLY after the dword table, so the map load's
            # displacement is also where the table ends. When the map's first
            # bytes happen to read as an in-section dword the greedy run
            # swallows them as one extra arm (six tables on the Xbox build,
            # each exactly one dword long). The cap only shortens; a capped
            # count under the minimum must re-earn recognition through the
            # byte-map proof below, so no recognised table can be deleted by
            # a wrong cap.
            cap = self._map_cap(tbl, entries)
            if cap is not None and cap < entries and (
                    cap >= min_entries or self._byte_map_table(tbl, cap)):
                entries = cap
                self.jump_table_trims[tbl] = (greedy, entries, "bytemap")

            # TSFP-LOCAL (patch 14): the displacement is not always where the
            # table starts. Try the two offset shapes before giving up.
            start = tbl
            if entries < min_entries:
                offset = self._offset_table(tbl, min_entries, max_entries)
                if offset is not None:
                    start, entries = offset
                    self._jt_alias[tbl] = start

            # TSFP-LOCAL (patch 15): two entries is under the minimum, but a
            # two-level dispatch can prove them. See _byte_map_table.
            proven = (entries < min_entries
                      and self._byte_map_table(tbl, entries))
            if entries < min_entries and not proven:
                # Too short to distinguish from code that merely looks like
                # pointers. Leaving it alone costs nothing; a wrong skip here
                # would delete real instructions.
                continue

            end = start + entries * 4
            if start != tbl and start in self.jump_tables:
                # Another dispatch already measured this very table.
                continue
            for insn in self.get_instructions_in_range(
                    start - 16, end):
                if insn.end_address > start and insn.address < end:
                    del self.instructions[insn.address]
            self._sorted_addrs = None

            self.jump_tables[start] = end
            self.decode_at(end)
            resynced += 1

        return resynced

    # TSFP-LOCAL (patch 08) ------------------------------------------------
    # MSVC's switch prologue, and the one thing in it that states the arm
    # count. The compiler has already done the analysis; we only have to read
    # it back.
    #
    #     mov   eax, [esp+8]
    #     cmp   eax, 6             <- the bound: indices 0..6
    #     ja    default            <- everything above it leaves
    #     jmp   [eax*4 + table]    <- 7 arms, not however many dwords follow
    #
    # Deliberately conservative. The bound is used ONLY when the compared
    # register is the same one that indexes the table AND nothing between the
    # compare and the dispatch writes it. The redefinition test is what keeps
    # the two-level form honest:
    #
    #     cmp   eax, 0x1C
    #     ja    default
    #     movzx eax, byte ptr [eax + bytetable]   <- eax REDEFINED here
    #     jmp   [eax*4 + table]
    #
    # There the 0x1C bounds the OUTER index; the dword table holds one entry
    # per distinct byte value, which is fewer. Refusing the bound in that case
    # leaves the next-base terminator to do the work, which is correct rather
    # than merely safe.
    _CMP_IMM = re.compile(r"^(e?[a-d]x|e?[sd]i|e?bp|e?sp|[a-d][lh])\s*,\s*"
                          r"(0x[0-9a-fA-F]+|\d+)$")
    # Mnemonics whose first operand is a destination. Anything not listed is
    # treated as "might write", so an unknown instruction also refuses.
    _WRITES_FIRST = (
        "mov", "movzx", "movsx", "lea", "add", "sub", "adc", "sbb", "and",
        "or", "xor", "neg", "not", "shl", "sal", "shr", "sar", "rol", "ror",
        "inc", "dec", "pop", "imul", "mul", "xchg", "cdq", "cwde", "movd",
    )
    _NEVER_WRITES = ("cmp", "test", "push", "nop", "jmp", "ja", "jae", "jb",
                     "jbe", "je", "jne", "jg", "jge", "jl", "jle", "jnbe",
                     "jnae", "int3", "call")

    def _jt_bound(self, tbl: int) -> Optional[int]:
        """Arm count from the guest's own range check, or None."""
        best = None
        for jump_va, index_reg in self._jt_sites.get(tbl, ()):
            found = self._range_check_before(jump_va, index_reg)
            if found is not None and (best is None or found > best):
                best = found
        return best

    def _range_check_before(self, jump_va: int,
                            index_reg: str) -> Optional[int]:
        """`cmp <index_reg>, N` guarding the instruction at `jump_va`: N + 1."""
        if not index_reg:
            return None
        window = [i for i in self.get_instructions_in_range(
            jump_va - 64, jump_va) if i.address < jump_va]
        # Walk backwards. Stop at the first thing that writes the index
        # register, because past that point a `cmp` on it is about a
        # different value.
        for insn in reversed(window):
            m = insn.mnemonic
            if m == "cmp":
                match = self._CMP_IMM.match(insn.op_str.strip())
                if match and match.group(1) == index_reg:
                    limit = int(match.group(2), 0)
                    # The compare must actually guard the dispatch: an
                    # unsigned above-branch is what sends out-of-range
                    # indices to the default case.
                    if self._guarded_by_above(insn, jump_va):
                        return limit + 1
                return None
            if m in self._NEVER_WRITES:
                continue
            # Anything not in _NEVER_WRITES is treated as writing its
            # first operand, so an unfamiliar mnemonic refuses rather
            # than silently trusting a stale compare.
            first = insn.op_str.split(",", 1)[0].strip()
            if first == index_reg:
                return None          # redefined: refuse the bound
        return None

    def _guarded_by_above(self, cmp_insn, jump_va: int) -> bool:
        """Is there a ja/jae/jb/jbe between this cmp and the dispatch?"""
        for insn in self.get_instructions_in_range(
                cmp_insn.end_address, jump_va):
            if insn.mnemonic in ("ja", "jnbe", "jae", "jnb",
                                 "jb", "jnae", "jbe", "jna",
                                 "jg", "jnle", "jge", "jnl"):
                return True
            if insn.mnemonic in ("cmp", "test"):
                return False     # a different comparison took over
        return False

    # TSFP-LOCAL (patch 14) ------------------------------------------------
    # MSVC's CRT memcpy/memmove (the XDK build carries the same one) index
    # their tail and lead vectors in ways the displacement does not describe:
    #
    #     neg  ecx                  ; ecx = 0, -1 .. -7
    #     jmp  [ecx*4 + 0x3C9A98]   ; the displacement is the LAST slot, so the
    #                               ; table lies at and BELOW it
    #
    #     and  eax, 3               ; 1..3, never 0 here
    #     jmp  [eax*4 + 0x3C9860]   ; slot 0 is the jmp's own bytes, so the
    #                               ; table starts at disp + 4
    #
    # The forward greedy scan sees one valid dword (or none) and refuses, the
    # bytes are decoded as instructions, and the function walk runs through
    # them until the first byte capstone cannot decode. That cut memmove at
    # 0x3C9AA2, 160 bytes short of the 0x3C9B40 the next function starts at,
    # and left the whole backward-copy tail unlifted: the lead arms, the
    # second table and the epilogue.
    #
    # Only tried where the forward scan found FEWER than min_entries, so a
    # table the old rule accepted is never touched, and each shape needs its
    # own evidence rather than just a run of pointers:
    #   - neg: the index register is negated by the instruction right before
    #     the dispatch, so a table indexed by -n cannot lie above its base.
    #   - disp + 4: three or more pointers start at disp + 4 while the forward
    #     scan from disp found under three. A run that long from disp + 4 would
    #     have been read through disp had that dword been a pointer, so disp is
    #     not slot 0.
    # Both stay inside the dispatch's own section like the forward rule.
    def _offset_table(self, tbl: int, min_entries: int,
                      max_entries: int) -> Optional[Tuple[int, int]]:
        """(start, entry count) of a table that does not begin at `tbl`."""
        home = self.image.get_section_at_va(tbl)
        if home is None:
            return None
        lo = home.virtual_addr
        hi = lo + home.virtual_size

        def run(first_va: int, step: int) -> int:
            count = 0
            while count < max_entries:
                target = self.image.read_u32_at_va(first_va + step * count * 4)
                if target is None or not (lo <= target < hi):
                    break
                count += 1
            return count

        for jump_va, index_reg in self._jt_sites.get(tbl, ()):
            neg = self._negates_index(jump_va, index_reg)
            if neg is None:
                continue
            count = run(tbl, -1)
            guard = self._neg_guard_bound(neg, index_reg)
            if guard is not None and guard < count:
                count = guard
            if count >= min_entries:
                return tbl - (count - 1) * 4, count
        count = run(tbl + 4, 1)
        # TSFP-LOCAL (patch 15): the run has no terminator but the first dword
        # that is not a pointer, so it needs the compiler's own bound. `and
        # eax, 3` leaves 1..3 and so states the count. All three tables the
        # rule finds in the Xbox build carry one. A pointer run at a
        # dispatch with none is what the neighbourhood null produces (0.2 to
        # 0.5 percent of displacements near a real table), so it is refused.
        masks = [mask for mask in (
            self._mask_bound_before(jump_va, index_reg)
            for jump_va, index_reg in self._jt_sites.get(tbl, ()))
            if mask is not None]
        if not masks:
            return None
        count = min(count, max(masks))
        if count >= min_entries:
            return tbl + 4, count
        return None

    def _mask_bound_before(self, jump_va: int,
                           index_reg: str) -> Optional[int]:
        """MASK of the `and <index_reg>, MASK` that last wrote the index.

        Only a low-bit mask (`2**k - 1`) bounds anything: the index is then
        0..MASK. Anything else that writes the register first refuses.
        """
        if not index_reg:
            return None
        window = [i for i in self.get_instructions_in_range(
            jump_va - 64, jump_va) if i.address < jump_va]
        for insn in reversed(window):
            if insn.mnemonic == "call":
                return None          # the callee may rewrite the register
            if insn.mnemonic in self._NEVER_WRITES:
                continue
            first, _, rest = insn.op_str.partition(",")
            if first.strip() != index_reg:
                continue
            if insn.mnemonic != "and":
                return None
            match = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)", rest.strip())
            if not match:
                return None
            mask = int(match.group(1), 0)
            return mask if mask & (mask + 1) == 0 else None
        return None

    # TSFP-LOCAL (patch 15) ------------------------------------------------
    # MSVC's two-level switch keeps the arm addresses in a dword table and
    # the case-to-arm mapping in a byte table that FOLLOWS it directly:
    #
    #     cmp   eax, 0x32
    #     ja    default
    #     movzx eax, byte ptr [eax + MAP]   ; MAP == table + 4 * arms
    #     jmp   [eax*4 + table]
    #
    # With two arms that is below the minimum of three, so the table stayed
    # decoded as instructions. One of the 32 such tables in the Xbox build has
    # a consequence: sub_001CE650 ended one byte before its own `ret`, the
    # second arm. Two pointers prove nothing alone. Three facts about the
    # dispatch do: the byte table starts exactly where the pointer run ends,
    # the compiler's own range check says how long the byte table is, and the
    # largest value in it is the last arm's index, so no third arm exists.
    _MAP_LOAD = re.compile(
        r"^(\w+)\s*,\s*byte ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+)\]$")

    def _byte_map_table(self, tbl: int, count: int) -> bool:
        """Is the `count`-entry run at `tbl` the table of a two-level switch?"""
        if count < 2:
            return False
        for jump_va, index_reg in self._jt_sites.get(tbl, ()):
            # Only a movzx or movsx prints `reg, byte ptr [reg + disp]`.
            load = next((i for i in self.get_instructions_in_range(
                jump_va - 8, jump_va) if i.end_address == jump_va), None)
            if load is None:
                continue
            match = self._MAP_LOAD.match(load.op_str.strip())
            if not match or match.group(1) != index_reg:
                continue
            if int(match.group(3), 16) != tbl + 4 * count:
                continue
            length = self._range_check_before(load.address, match.group(2))
            if length is None:
                continue
            body = self.image.read_bytes_at_va(tbl + 4 * count, length)
            if body is not None and len(body) == length and (
                    max(body) == count - 1):
                return True
        return False

    # TSFP-LOCAL (T89) ------------------------------------------------------
    def _map_cap(self, tbl: int, entries: int) -> Optional[int]:
        """Dword count up to a two-level dispatch's byte map, or None.

        The byte map follows the dword table directly, so a map load whose
        displacement falls INSIDE the recognised run proves the run read the
        map's first bytes as a table entry. The displacement must sit on a
        dword boundary of the run, and when the compiler's range check makes
        the map readable, a map value indexing at or past the capped count
        refuses the cap (the map itself would contradict it).
        """
        best = None
        for jump_va, index_reg in self._jt_sites.get(tbl, ()):
            load = next((i for i in self.get_instructions_in_range(
                jump_va - 8, jump_va) if i.end_address == jump_va), None)
            if load is None:
                continue
            match = self._MAP_LOAD.match(load.op_str.strip())
            if not match or match.group(1) != index_reg:
                continue
            map_base = int(match.group(3), 16)
            if not (tbl < map_base < tbl + 4 * entries):
                continue
            if (map_base - tbl) % 4:
                continue
            capped = (map_base - tbl) // 4
            length = self._range_check_before(load.address, match.group(2))
            if length is not None:
                body = self.image.read_bytes_at_va(map_base, length)
                if (body is not None and len(body) == length
                        and max(body) >= capped):
                    continue
            if best is None or capped < best:
                best = capped
        return best

    def _negates_index(self, jump_va: int, index_reg: str):
        """The `neg <index_reg>` ending exactly at the dispatch, or None."""
        if not index_reg:
            return None
        for insn in self.get_instructions_in_range(jump_va - 8, jump_va):
            if (insn.end_address == jump_va and insn.mnemonic == "neg"
                    and insn.op_str.strip() == index_reg):
                return insn
        return None

    def _neg_guard_bound(self, neg_insn, index_reg: str) -> Optional[int]:
        """Entry count the compiler's own range check puts on a negated index.

        `cmp ecx, 8 / jb <neg>`: the index was below 8 before it was negated,
        so 0..-7 are the only values and the table has 8 slots. Read it where
        it is written rather than trusting the next dword to stop the scan,
        because MSVC parks sibling tables back to back (patch 08).
        """
        best = None
        window = self.get_instructions_in_range(
            neg_insn.address - 1024, neg_insn.address + 1024)
        by_end = {insn.end_address: insn for insn in window}
        for insn in window:
            if not insn.is_cond_jump or insn.jump_target != neg_insn.address:
                continue
            before = by_end.get(insn.address)
            if before is None or before.mnemonic != "cmp":
                continue
            match = self._CMP_IMM.match(before.op_str.strip())
            if not match or match.group(1) != index_reg:
                continue
            limit = int(match.group(2), 0)
            if insn.mnemonic in ("jb", "jnae", "jc"):
                count = limit
            elif insn.mnemonic in ("jbe", "jna"):
                count = limit + 1
            else:
                continue
            if best is None or count > best:
                best = count
        return best

    def jump_table_entries(self, tbl: int) -> List[int]:
        """Code pointers held by a resynced jump table, or [] if unknown."""
        tbl = self._jt_alias.get(tbl, tbl)
        end = self.jump_tables.get(tbl)
        if end is None:
            return []
        return [self.image.read_u32_at_va(a) or 0
                for a in range(tbl, end, 4)]

    def decode_at(self, addr: int, max_insns: int = 4096) -> int:
        """
        Decode a stream starting exactly at `addr`, realigning the sweep.

        linear_sweep decodes each section as one stream and resyncs only by
        skipping a byte when capstone fails. Where it enters a run of data --
        a jump table, alignment padding, an embedded constant -- it comes out
        the far side misaligned and stays that way until it happens to fall
        back into step. Everything downstream keys off self.instructions, so an
        address the sweep stepped over simply does not exist: recursive_descent
        stops dead there (it only reads this dict, it never decodes), and
        _pass_call_targets drops the candidate.

        That is how Halo 2276 ended up with 46 direct call targets that never
        became functions. 0x00017AB0 is named in a caller's own call list, is
        16-aligned, and sits in a 5.7 KB hole between two detected functions --
        the strongest evidence a function start can have, discarded because a
        byte-skip upstream put the stream out of phase.

        Decoding stops as soon as it lands on an address already decoded: at
        that point the streams have converged and the rest is already correct.
        So this rewrites only the out-of-phase run, not the section.

        Overlapping decodes are left in place rather than evicted. On x86 two
        valid instruction streams really can share bytes, and the callers that
        matter walk forward by end_address from a known start, so each follows
        its own chain. Evicting the old one would corrupt whichever function
        was already using it.

        Returns the number of instructions newly decoded.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return 0
        data = self.image.get_section_data(section)
        if not data:
            return 0

        offset = addr - section.virtual_addr
        if offset < 0 or offset >= len(data):
            return 0

        added = 0
        for cs_insn in self._cs.disasm(data[offset:], addr):
            if cs_insn.address != addr and cs_insn.address in self.instructions:
                break  # resynced with the existing stream
            if cs_insn.address not in self.instructions:
                self.instructions[cs_insn.address] = \
                    self._classify_instruction(cs_insn)
                added += 1
            insn = self.instructions[cs_insn.address]
            if insn.is_terminator:
                break
            if added >= max_insns:
                break

        if added:
            self._sorted_addrs = None
        return added

    def block_tail_jump(self, addr: int, max_insns: int = 256):
        """
        Where the straight-line block at `addr` jumps, if it ends in an
        unconditional jmp rather than a ret.

        probes_as_returning_body deliberately stops at such a jump, because for
        a weak candidate -- an address that merely appeared as an immediate --
        a tail jump is not evidence of a function. But for a branch target that
        is already known to be reached from inside a function, the jump is the
        block's terminator and its destination is the rest of the same
        function. Half-Life 2's _lock helper reaches its loop tail this way:
        "cmp [ebp-0x1c],-1 / jne / inc edi / jmp back into the body".

        Returns the jump target, or None if the block rets, runs long, or ends
        in something that is not a direct jmp.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return None
        data = self.image.read_bytes_at_va(addr, max_insns * 8)
        if not data:
            return None

        count = 0
        for decoded in self._cs.disasm(data, addr):
            count += 1
            if count > max_insns:
                return None
            mnemonic = decoded.mnemonic.lower()
            if mnemonic in config.RET_MNEMONICS:
                return None                 # a ret block: not this shape
            if mnemonic in config.JMP_MNEMONICS:
                try:
                    ops = decoded.operands
                except Exception:
                    return None
                if not ops or ops[0].type != CS_OP_IMM:
                    return None             # indirect: nothing to name
                return ops[0].imm & 0xFFFFFFFF
        return None

    def probes_as_returning_body(self, addr: int,
                                 max_insns: int = 64) -> bool:
        """
        Stricter sibling of probes_as_function_body: does the stream at `addr`
        reach a `ret` without first hitting an unconditional `jmp`?

        Used where the evidence that something is a function is weak -- an
        address that merely appears as an immediate -- so the bar has to be
        higher than "decodes to some terminator". Requiring a ret rejects data
        that happens to disassemble, and stopping at an unconditional jmp keeps
        this out of tail-call territory, which _pass_tail_jump_targets already
        covers with better evidence. Conditional branches are fine: a real
        function has them. So is a switch dispatch through a table that
        resync_jump_tables has measured: the probe continues in its arms.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return False
        count = 0
        resume = addr
        # One pass per straight-line segment. A segment ends at a ret (yes),
        # at anything tail-call shaped (no), or at a switch dispatch through
        # a table this engine has already measured -- which is neither: the
        # function continues in its arms, so the probe resumes at the first
        # arm past the jump and keeps its instruction budget. A function
        # reached only as an immediate, with no prologue, that opens with
        # exactly that dispatch was otherwise refused, and the switch tables
        # it owns were translated nowhere.
        while resume is not None and count < max_insns:
            data = self.image.read_bytes_at_va(resume, (max_insns - count) * 8)
            if not data:
                return False
            limit = resume + len(data)
            start = resume
            resume = None
            for decoded in self._cs.disasm(data, start):
                count += 1
                mnemonic = decoded.mnemonic.lower()
                if mnemonic in config.RET_MNEMONICS:
                    return True
                if mnemonic in config.JMP_MNEMONICS:
                    # An unconditional jump forward, still inside the window
                    # being probed, is ordinary control flow -- MSVC emits it
                    # constantly to skip an else-branch. Only a jump that
                    # leaves the window, or goes backwards, is tail-call
                    # shaped and ends the probe.
                    #
                    # Rejecting every jmp cost Half-Life 2 its
                    # CreateInterface list walk (0x00427F80): a clean 90-byte
                    # function that happens to contain one `jmp` over four
                    # instructions.
                    try:
                        ops = decoded.operands
                    except Exception:
                        return False
                    if not ops:
                        return False
                    if ops[0].type == CS_OP_MEM and ops[0].mem.index != 0:
                        arm = self._first_arm_after(
                            ops[0].mem.disp & 0xFFFFFFFF, decoded.address)
                        if arm is None:
                            return False
                        resume = arm
                        break
                    if ops[0].type != CS_OP_IMM:
                        return False
                    target = ops[0].imm & 0xFFFFFFFF
                    if not (decoded.address < target < limit):
                        return False
                if count >= max_insns:
                    return False
        return False

    def _first_arm_after(self, table: int, site: int) -> Optional[int]:
        """The lowest entry of a measured jump table that lies past `site`,
        or None when `table` is not one this engine has measured."""
        entries = self.jump_table_entries(table)
        later = [a for a in entries if a > site]
        return min(later) if later else None

    def probes_as_vcall_thunk(self, addr: int) -> bool:
        """Is this MSVC's virtual-call thunk?

            mov eax, [ecx]              ; load the vtable from `this`
            jmp dword ptr [eax + N]     ; dispatch to slot N

        A real function, and one that only ever exists as a value in a table --
        the compiler emits them for pointers-to-virtual-member-functions and
        for interface forwarding. They end in an indirect tail jump and never
        reach a `ret`, so probes_as_returning_body rejects them, and they are
        packed back to back with no int3 between them, so the padding boundary
        pass does not see them either.

        Half-Life 2 has 568 of these and only 41 were being found. Each missed
        one is an indirect call the runtime cannot resolve, so the call is
        skipped rather than made: two of them, at 0x00583BE2 and 0x00583BFA,
        were being reached 22 million times in a boot that then sat spinning.

        Matched shape-exactly rather than by relaxing the general prober,
        because "ends in an indirect jump" on its own is weak evidence that
        data happens to disassemble into.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return False
        data = self.image.read_bytes_at_va(addr, 16)
        if not data:
            return False

        insns = list(self._cs.disasm(data, addr, count=2))
        if len(insns) != 2:
            return False

        load, dispatch = insns
        if load.mnemonic.lower() != "mov":
            return False
        try:
            load_ops = load.operands
            jmp_ops = dispatch.operands
        except Exception:
            return False
        # mov <reg>, [<reg>]  -- the vtable load, no index, no displacement
        if len(load_ops) != 2:
            return False
        dst, src = load_ops
        if dst.type != CS_OP_REG or src.type != CS_OP_MEM:
            return False
        if src.mem.base == 0 or src.mem.index != 0 or src.mem.disp != 0:
            return False

        if dispatch.mnemonic.lower() not in config.JMP_MNEMONICS:
            return False
        if len(jmp_ops) != 1 or jmp_ops[0].type != CS_OP_MEM:
            return False
        # ...through the register the load just filled.
        return jmp_ops[0].mem.base == dst.reg and jmp_ops[0].mem.index == 0

    def probes_as_prologue(self, addr: int) -> bool:
        """
        Read-only: do the bytes at `addr` start with a recognisable MSVC
        function prologue?

        Weaker evidence than probes_as_function_body, and deliberately so:
        this is asked about an address the linear sweep has already claimed as
        the middle of an instruction, where a full-body probe would have to
        decide which of two overlapping decodings is real. A prologue is the
        one shape that does not occur by accident in the middle of another
        instruction, so it is enough to say the sweep drifted rather than that
        the address is wrong.

        Recognises what MSVC actually emits at -O2 for the Xbox XDK: the
        frame-pointer form, the register saves that start a frameless
        function, the stack adjustment, and the two-byte hot-patch nop.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return False
        data = self.image.get_section_data(section)
        if not data:
            return False
        offset = addr - section.virtual_addr
        if offset < 0 or offset >= len(data):
            return False

        insns = list(self._cs.disasm(data[offset:offset + 16], addr, count=2))
        if not insns:
            return False

        first = insns[0]
        m, ops = first.mnemonic, first.op_str

        if m == "push" and ops == "ebp":
            # push ebp; mov ebp, esp  -- or lea ebp, [esp-N] for a frame the
            # callee shifts, which is what default.xbe's XPP code uses.
            if len(insns) > 1:
                nxt = insns[1]
                if nxt.mnemonic == "mov" and nxt.op_str.replace(" ", "") == "ebp,esp":
                    return True
                if nxt.mnemonic == "lea" and nxt.op_str.startswith("ebp,"):
                    return True
            return False

        if m == "push" and ops in ("esi", "edi", "ebx"):
            return True
        if m == "sub" and ops.startswith("esp,"):
            return True
        if m == "mov" and ops.replace(" ", "") == "edi,edi":
            return True   # hot-patch pad

        # A function whose frame __SEH_prolog builds:
        #
        #     push <frame size>        immediate
        #     push <scope table>       immediate
        #     call __SEH_prolog
        #
        # There is no "push ebp; mov ebp, esp" to find, because the helper does
        # that on the caller's behalf, so none of the shapes above match and
        # such a function is invisible to every pass that asks this question.
        # Half-Life 2 has one at 0x001F572B, reached only through a vtable: the
        # call could not be resolved, so it was skipped rather than made, and
        # the arguments already pushed for it stayed on the stack. That shifted
        # the caller's frame, and its "pop ebx" then restored the wrong slot.
        #
        # Two immediate pushes followed by a call is specific enough not to
        # occur by accident -- and this is only ever asked about an address
        # that already looks like a boundary.
        if m == "push" and ops.startswith("0x") and len(insns) > 1:
            nxt = insns[1]
            if nxt.mnemonic == "push" and nxt.op_str.startswith("0x"):
                third = list(self._cs.disasm(
                    data[offset:offset + 24], addr, count=3))
                if len(third) > 2 and third[2].mnemonic == "call":
                    return True

        return False

    def probes_as_constant_stub(self, addr: int) -> bool:
        """Is this the whole of a constant-returning accessor?

            mov <reg>, <imm32>
            ret [imm16]

        MSVC emits runs of these for members that return a fixed address, packs
        them back to back with no padding, and reaches them only through
        vtables -- so no pass that looks for a prologue, padding or a call site
        finds them. Half-Life 2 has 3,147.

        Matched exactly, and not by asking a general "does a ret come soon"
        probe. That was tried: allowing any short run ending in ret added 825
        function starts instead of the 19 this shape accounts for, because data
        and mid-function fragments satisfy it too, and the title stopped
        loading. The narrow rule is the honest one -- it is what was measured.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return False
        data = self.image.get_section_data(section)
        if not data:
            return False
        offset = addr - section.virtual_addr
        if offset < 0 or offset >= len(data):
            return False

        insns = list(self._cs.disasm(data[offset:offset + 12], addr, count=2))
        if len(insns) != 2:
            return False
        first, second = insns
        if first.mnemonic != "mov":
            return False
        try:
            ops = first.operands
        except Exception:
            return False
        if len(ops) != 2 or ops[0].type != CS_OP_REG or ops[1].type != CS_OP_IMM:
            return False
        return second.mnemonic in config.RET_MNEMONICS

    def probes_as_function_body(self, addr: int,
                                max_insns: int = 8192) -> bool:
        """
        Read-only: does the instruction stream starting at `addr` look like a
        function body -- i.e. does it reach a ret or a tail jump without
        running into bytes that will not decode?

        Corroboration for a direct call target the linear sweep stepped over.
        Alignment used to serve that role, but a real MSVC function start is
        only aligned when the linker had a reason to pad it; the CRT's
        _mtinitlocks sits at Wreckless 0x000F211A, immediately after a jump
        table, at no alignment at all. Decoding is the stronger evidence and
        the one that actually distinguishes code from a plausible-looking
        address read out of data.

        Follows instructions the sweep already decoded where they exist, and
        decodes the rest here without recording them, so calling this never
        changes what the sweep produced.

        max_insns only bounds the walk's cost -- leaving the section already
        terminates it. Keep it well clear of the largest real function: Blood
        Wake 0x0005E670 runs 537 instructions before its first ret, and a cap
        of 512 rejected it.
        """
        section = self.image.get_section_at_va(addr)
        if section is None or not section.executable:
            return False
        data = self.image.get_section_data(section)
        if not data:
            return False

        for _ in range(max_insns):
            insn = self.instructions.get(addr)
            if insn is not None:
                if insn.is_ret or insn.is_jump:
                    return True
                addr = insn.end_address
            else:
                offset = addr - section.virtual_addr
                if offset < 0 or offset >= len(data):
                    return False
                decoded = next(self._cs.disasm(data[offset:], addr, count=1),
                               None)
                if decoded is None:
                    return False  # undecodable: not code
                mnemonic = decoded.mnemonic.lower()
                if (mnemonic in config.RET_MNEMONICS
                        or mnemonic in config.JMP_MNEMONICS):
                    return True
                addr += decoded.size
            if not (section.virtual_addr <= addr
                    < section.virtual_addr + section.virtual_size):
                return False  # ran off the section without terminating
        return False

    def recursive_descent(self, start_addresses: List[int],
                          section_bounds: List[Tuple[int, int]]) -> Set[int]:
        """
        Recursive descent from given start addresses.

        Follows control flow to determine reachable instructions.

        Returns set of reachable instruction addresses.
        """
        reachable: Set[int] = set()
        worklist = list(start_addresses)
        visited_starts: Set[int] = set()

        def in_bounds(addr: int) -> bool:
            return any(s <= addr < e for s, e in section_bounds)

        while worklist:
            addr = worklist.pop()
            if addr in visited_starts:
                continue
            visited_starts.add(addr)

            # Walk the instruction stream linearly from this start point
            while True:
                if addr in reachable:
                    break  # Already explored from here
                if not in_bounds(addr):
                    break

                insn = self.instructions.get(addr)
                if insn is None:
                    # Tried calling decode_at here to realign, on the theory
                    # that descent was being truncated by the same out-of-phase
                    # runs decode_at fixes for call targets. Measured on Halo
                    # 2276: +138 reachable instructions out of 640k (0.02%),
                    # zero new functions, and one tail_jump_target lost. Not
                    # worth the risk of following a bad decode deeper.
                    #
                    # It is small because descent starts from function heads the
                    # sweep already decoded and stops at ret/jmp, so it seldom
                    # walks into a hole in the first place -- the call-target
                    # path in _pass_call_targets already catches the cases that
                    # matter. See docs/technical/ms-fusion-adoption-plan.md.
                    break

                reachable.add(addr)

                # Follow call targets (but continue past the call)
                if insn.is_call and insn.call_target is not None:
                    if in_bounds(insn.call_target) and insn.call_target not in visited_starts:
                        worklist.append(insn.call_target)

                # Follow conditional jump targets (and continue fall-through)
                if insn.is_cond_jump and insn.jump_target is not None:
                    if in_bounds(insn.jump_target) and insn.jump_target not in visited_starts:
                        worklist.append(insn.jump_target)

                # Unconditional jump: follow target, stop linear walk
                if insn.is_jump:
                    if insn.jump_target is not None and in_bounds(insn.jump_target):
                        if insn.jump_target not in visited_starts:
                            worklist.append(insn.jump_target)
                    break

                if insn.is_ret:
                    break

                addr = insn.end_address

        return reachable

    def get_instruction(self, address: int) -> Optional[Instruction]:
        """Get a decoded instruction by address."""
        return self.instructions.get(address)

    def _ensure_sorted_addrs(self):
        """Build sorted address list if not cached."""
        if self._sorted_addrs is None:
            self._sorted_addrs = sorted(self.instructions.keys())

    def instruction_covering(self, addr: int) -> Optional[Instruction]:
        """The decoded instruction whose bytes contain `addr` but do not start
        at it, or None.

        A hit means the sweep already has a coherent decode across this
        address, so anything claiming a function starts here is claiming an
        instruction boundary in the middle of an instruction. That is worth
        distrusting: realigning there splits whatever the address sits in.
        """
        self._ensure_sorted_addrs()
        # Scan back over the longest an x86 instruction can be, rather than
        # looking only at the nearest preceding start. Overlapping decodes are
        # allowed here -- decode_at leaves the stream it realigned over in
        # place -- so `addr` can be a recorded boundary *and* sit inside an
        # instruction the sweep decoded. Checking one neighbour misses exactly
        # that case, which is the one worth catching.
        i = bisect.bisect_left(self._sorted_addrs, addr)
        j = bisect.bisect_left(self._sorted_addrs, addr - 15)
        for k in range(i - 1, j - 1, -1):
            if k < 0:
                break
            insn = self.instructions[self._sorted_addrs[k]]
            if insn.address < addr < insn.address + insn.size:
                return insn
        return None

    def get_instructions_in_range(self, start: int, end: int) -> List[Instruction]:
        """Get all instructions in address range [start, end), sorted."""
        self._ensure_sorted_addrs()
        import bisect
        lo = bisect.bisect_left(self._sorted_addrs, start)
        hi = bisect.bisect_left(self._sorted_addrs, end)
        return [self.instructions[self._sorted_addrs[i]]
                for i in range(lo, hi)]

    def instruction_count(self) -> int:
        return len(self.instructions)

    def get_call_targets(self) -> Set[int]:
        """Return all direct call target addresses."""
        targets = set()
        for insn in self.instructions.values():
            if insn.call_target is not None:
                targets.add(insn.call_target)
        return targets

    def get_indirect_call_refs(self) -> Dict[int, List[int]]:
        """
        Return memory addresses referenced by indirect calls.
        Maps: memory_address -> [calling_instruction_addresses]
        """
        refs: Dict[int, List[int]] = {}
        for insn in self.instructions.values():
            if insn.is_call and insn.memory_ref is not None:
                refs.setdefault(insn.memory_ref, []).append(insn.address)
        return refs
