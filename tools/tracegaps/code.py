# SPDX-License-Identifier: GPL-3.0-or-later
"""The whole retail image decoded once, with the cross-reference queries the traces need.

WHY ALL EXECUTABLE SECTIONS. T142 swept `.text` and found one caller of the CreateEvent
wrapper. The title also carries code in `XONLINE`, `XNET`, `XMV`, `XPP`, `D3D`, `DSOUND`
and the rest, and a call from any of them reaches the same wrapper. A census that stops at
`.text` is a census of one section.

Decoding is the repo's resynchronising sweep (`tools.gen_d3d8_surface.decode_section`),
not `capstone.disasm` over a section, which stops silently at the first undecodable byte.
"""

from __future__ import annotations

import struct
import sys
from array import array
from bisect import bisect_right
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

from tools.gen_d3d8_surface import decode_section
from tools.shaderscan.image import Image
from tools.xbe import Xbe, XbeCertificate, XbeSection
from tools.xbe.model import SECTION_FLAG_EXECUTABLE

#: Decoded call/jump transfer to a fixed address.
TRANSFERS = ("call", "jmp")

#: Bytes of padding or alignment that precede a function entry.
_PADDING = frozenset({0xCC, 0x90, 0x00})

#: Instructions scanned back from a table `jmp` for its `and reg, mask`.
MASK_WINDOW = 24
#: Largest `and` mask taken as a table bound (slots - 1).
MAX_MASK = 16


@dataclass(frozen=True)
class Transfer:
    """One direct `call rel32` or `jmp rel32`."""

    address: int
    mnemonic: str
    target: int
    section: str

    @property
    def return_address(self) -> int:
        return self.address + 5


class Code:
    """An XBE with every executable section decoded and indexed by virtual address."""

    def __init__(self, image: Image, *, decode_table_arms: bool = True) -> None:
        self.image = image
        self.sections: list[XbeSection] = [s for s in image.xbe.sections if s.executable]
        self.insns: list[capstone.CsInsn] = []
        self.undecodable: dict[str, int] = {}
        for section in self.sections:
            body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            decoded, bad = decode_section(body, section.virtual_addr)
            self.insns.extend(decoded)
            self.undecodable[section.name] = len(bad)
        self.insns.sort(key=lambda insn: insn.address)
        self.index: dict[int, int] = {insn.address: n for n, insn in enumerate(self.insns)}
        self._starts = [insn.address for insn in self.insns]
        self._transfers: dict[int, list[Transfer]] | None = None
        self._call_targets: frozenset[int] | None = None
        self._branch_targets: frozenset[int] | None = None
        self._indirect_transfers: int | None = None
        self._sources: dict[int, list[capstone.CsInsn]] | None = None
        #: entry points added by `extend`, in order (T607)
        self.extra_entries: list[int] = []
        if decode_table_arms:
            self.extend(self.table_arm_entries())

    @classmethod
    def load(cls, path: Path) -> Code:
        return cls(Image.load(path))

    @classmethod
    def from_blob(
        cls,
        base: int,
        blob: bytes,
        *,
        name: str = ".text",
        data: tuple[int, bytes] | None = None,
        decode_table_arms: bool = True,
    ) -> Code:
        """A `Code` over hand-made bytes: one executable section, optional `(address, bytes)` data.

        For tests and experiments. The data section is not decoded, so a stored pointer in it
        reads as a pointer and not as part of an instruction.
        """
        sections = [
            XbeSection(
                name=name,
                flags=SECTION_FLAG_EXECUTABLE,
                virtual_addr=base,
                virtual_size=len(blob),
                raw_addr=0,
                raw_size=len(blob),
            )
        ]
        raw = bytes(blob)
        end = base + len(blob)
        if data is not None:
            data_base, data_bytes = data
            sections.append(
                XbeSection(".data", 0, data_base, len(data_bytes), len(raw), len(data_bytes))
            )
            raw += data_bytes
            end = max(end, data_base + len(data_bytes))
        certificate = XbeCertificate(0, "", 0, 0, 0, 0, 0, 0)
        xbe = Xbe(
            base_address=base,
            size_of_image=end - base,
            size_of_headers=0,
            sections=sections,
            certificate=certificate,
        )
        return cls(Image(raw, xbe), decode_table_arms=decode_table_arms)

    # ------------------------------------------------------------------ table arms (T607)

    def and_mask(self, jump: capstone.CsInsn) -> int | None:
        """N when `jump` is `jmp [reg*4 + base]` right after `and reg, N` (N = 2^k - 1), else None.

        Reads back through the straight line (no gap) over instructions that do not write `reg`;
        a conditional jump falls through with `reg` unchanged and is passed, any other transfer
        ends the search. This is the compiler's own mask of a CRT tail table (`memcpy`: `and edx, 3`
        then `jmp [edx*4 + table]`). It does not check branch targets: `jump_table` does, this only
        sizes the slots whose arms must exist as decoded code.
        """
        from tools.tracegaps.flow import FAMILY_OF

        if jump.mnemonic != "jmp" or len(jump.operands) != 1:
            return None
        operand = jump.operands[0]
        if operand.type != cs_x86.X86_OP_MEM or operand.mem.base != 0 or operand.mem.scale != 4:
            return None
        if operand.mem.index == 0:
            return None
        family = FAMILY_OF.get(jump.reg_name(operand.mem.index), "")
        if not family:
            return None
        cursor = jump
        for _ in range(MASK_WINDOW):
            previous = self.previous_insn(cursor)
            if previous is None or previous.address + previous.size != cursor.address:
                return None
            _, written = previous.regs_access()
            writes = {FAMILY_OF.get(previous.reg_name(register), "") for register in written}
            if family in writes:
                first = previous.operands[0] if previous.operands else None
                if (
                    previous.mnemonic == "and"
                    and len(previous.operands) == 2
                    and first is not None
                    and first.type == cs_x86.X86_OP_REG
                    and first.size == 4
                    and previous.operands[1].type == cs_x86.X86_OP_IMM
                    and FAMILY_OF.get(previous.reg_name(first.reg), "") == family
                ):
                    mask = previous.operands[1].imm & 0xFFFFFFFF
                    return mask if 0 < mask < MAX_MASK and mask & (mask + 1) == 0 else None
                return None
            if capstone.CS_GRP_JUMP in previous.groups:
                relative = previous.operands and previous.operands[0].type == cs_x86.X86_OP_IMM
                if previous.mnemonic == "jmp" or not relative:
                    return None
            elif (
                capstone.CS_GRP_CALL in previous.groups
                or capstone.CS_GRP_RET in previous.groups
                or previous.mnemonic in ("int3", "hlt", "ud2", "int")
            ):
                return None
            cursor = previous
        return None

    def borrow_range(self, jump: capstone.CsInsn) -> int | None:
        """N when `jmp [reg*4 + base]` is reached only by `sub reg, N ; jb jump`, else None.

        `sub reg, N` sets the carry exactly when `reg < N` unsigned, so on the taken edge the new
        `reg` is in `-N..-1` and the jump reads the slots `-N..-1` below `base`. Needs: the
        previous instruction does not fall into the jump, and EVERY direct transfer to the jump is
        such a `jb` right after such a `sub`. (CRT `memcpy`: `sub ecx, 4 ; jb tail`.)
        """
        from tools.tracegaps.flow import FAMILY_OF

        if jump.mnemonic != "jmp" or len(jump.operands) != 1:
            return None
        operand = jump.operands[0]
        if operand.type != cs_x86.X86_OP_MEM or operand.mem.base != 0 or operand.mem.scale != 4:
            return None
        if operand.mem.index == 0:
            return None
        family = FAMILY_OF.get(jump.reg_name(operand.mem.index), "")
        before = self.previous_insn(jump)
        if before is not None and before.address + before.size == jump.address:
            falls = not (
                before.mnemonic in ("jmp", "int3", "hlt", "ud2")
                or capstone.CS_GRP_RET in before.groups
            )
            if falls:
                return None
        sources = self.direct_sources().get(jump.address, [])
        if not sources:
            return None
        limit = 0
        for source in sources:
            if source.mnemonic not in ("jb", "jc"):
                return None
            sub = self.previous_insn(source)
            if sub is None or sub.address + sub.size != source.address or sub.mnemonic != "sub":
                return None
            first = sub.operands[0]
            if first.type != cs_x86.X86_OP_REG or first.size != 4:
                return None
            if FAMILY_OF.get(sub.reg_name(first.reg), "") != family:
                return None
            if sub.operands[1].type != cs_x86.X86_OP_IMM or not 0 < sub.operands[1].imm < MAX_MASK:
                return None
            limit = max(limit, sub.operands[1].imm)
        return limit or None

    def direct_sources(self) -> dict[int, list[capstone.CsInsn]]:
        """target -> every direct `call`, `jmp` and conditional jump to it."""
        if self._sources is None:
            found: dict[int, list[capstone.CsInsn]] = {}
            for insn in self.insns:
                if len(insn.operands) != 1:
                    continue
                if capstone.CS_GRP_JUMP not in insn.groups and insn.mnemonic != "call":
                    continue
                if insn.operands[0].type == cs_x86.X86_OP_IMM:
                    found.setdefault(insn.operands[0].imm & 0xFFFFFFFF, []).append(insn)
            self._sources = found
        return self._sources

    def masked_table(self, jump: capstone.CsInsn) -> tuple[list[int], list[int]] | None:
        """(slot values that are instruction starts, those that are not) of an `and`-masked table.

        Every slot `0..mask` must be an address of the jump's own section or a value outside every
        section (padding bytes read as a dword, such a jump runs no guest code). Any other slot
        (unreadable, another section) means the mask is not the table's bound: None. A mask wider
        than 16 slots is never taken (`MAX_MASK`), the retail `and reg, 0x7F` and `0xFF` operands
        are not table bounds and their junk slots would be read as code.
        """
        mask = self.and_mask(jump)
        if mask is None:
            return None
        section = self.image.section_at(jump.address)
        base = jump.operands[0].mem.disp & 0xFFFFFFFF
        starts: list[int] = []
        others: list[int] = []
        for slot in range(mask + 1):
            value = self.image.u32(base + 4 * slot)
            if value is None:
                return None
            where = self.image.section_at(value)
            if where is None:
                continue
            if where is not section:
                return None
            if value in self.index:
                if value not in starts:
                    starts.append(value)
            elif value not in others:
                others.append(value)
        return starts, others

    def table_arm_entries(self) -> list[int]:
        """Slot values of `and`-masked jump tables that are not decoded instruction starts.

        The sweep reads the table dwords themselves as instructions and can resynchronise past an
        arm (the CRT `memcpy` tail: slot 0 `0x3C995C` sits inside a swept `test byte ptr [...]`).
        """
        found: list[int] = []
        for insn in self.insns:
            if insn.mnemonic != "jmp" or len(insn.operands) != 1:
                continue
            if insn.operands[0].type != cs_x86.X86_OP_MEM:
                continue
            table = self.masked_table(insn)
            if table is None:
                continue
            found.extend(value for value in table[1] if value not in found)
        return found

    def extend(self, entries: Iterable[int]) -> list[int]:
        """Decode code from each of `entries` that is no instruction start, return the new starts.

        Linear from the entry to the first `ret`, `jmp`, `int3` or `hlt`, or until it reaches an
        address the sweep already has as a start (resynchronised). The swept instructions that
        overlap the new ones were decoded out of the middle of them (table bytes, a swallowed
        prefix) and are dropped. Call before the transfer indexes are first read.
        """
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        starts = set(self.index)
        added: list[capstone.CsInsn] = []
        for entry in entries:
            section = self.image.section_at(entry)
            if section is None or not section.executable or entry in starts:
                continue
            self.extra_entries.append(entry)
            address = entry
            while section.virtual_addr <= address < section.virtual_addr + section.raw_size:
                if address in starts and address != entry:
                    break
                window = self.image.read(address, 16)
                if window is None:
                    break
                insn = next(md.disasm(window, address), None)
                if insn is None:
                    break
                added.append(insn)
                address += insn.size
                if insn.mnemonic in ("ret", "retn", "jmp", "int3", "hlt"):
                    break
        if not added:
            return []
        new = {insn.address: insn for insn in added}
        dropped: set[int] = set()
        for low, high in sorted((insn.address, insn.address + insn.size) for insn in new.values()):
            position = max(0, bisect_right(self._starts, low) - 16)
            while position < len(self.insns) and self.insns[position].address < high:
                old = self.insns[position]
                if old.address not in new and old.address + old.size > low:
                    dropped.add(old.address)
                position += 1
        kept = [insn for insn in self.insns if insn.address not in dropped]
        merged = {insn.address: insn for insn in kept}
        merged.update(new)
        self.insns = sorted(merged.values(), key=lambda insn: insn.address)
        self.index = {insn.address: n for n, insn in enumerate(self.insns)}
        self._starts = [insn.address for insn in self.insns]
        self._transfers = None
        self._call_targets = None
        self._branch_targets = None
        self._indirect_transfers = None
        self._sources = None
        return sorted(new)

    def section_name(self, address: int) -> str:
        section = self.image.section_at(address)
        return section.name if section else "?"

    def insn_at(self, address: int) -> capstone.CsInsn | None:
        position = self.index.get(address)
        return None if position is None else self.insns[position]

    def next_insn(self, insn: capstone.CsInsn) -> capstone.CsInsn | None:
        position = self.index.get(insn.address)
        if position is None or position + 1 >= len(self.insns):
            return None
        return self.insns[position + 1]

    def previous_insn(self, insn: capstone.CsInsn) -> capstone.CsInsn | None:
        position = self.index.get(insn.address)
        if position is None or position == 0:
            return None
        return self.insns[position - 1]

    def window(self, address: int, before: int, after: int) -> list[capstone.CsInsn]:
        """`before` instructions up to `address`'s predecessor, then `after` from `address` on."""
        position = self.index[address]
        return self.insns[max(0, position - before) : position + after]

    # ------------------------------------------------------------------ transfers

    def transfers(self) -> dict[int, list[Transfer]]:
        """target -> every direct `call`/`jmp rel32` to it, in any executable section."""
        if self._transfers is None:
            table: dict[int, list[Transfer]] = {}
            for insn in self.insns:
                if insn.mnemonic not in TRANSFERS:
                    continue
                if capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
                    continue
                ops = insn.operands
                if len(ops) != 1 or ops[0].type != cs_x86.X86_OP_IMM:
                    continue
                target = ops[0].imm & 0xFFFFFFFF
                table.setdefault(target, []).append(
                    Transfer(insn.address, insn.mnemonic, target, self.section_name(insn.address))
                )
            self._transfers = table
        return self._transfers

    def callers(self, target: int) -> list[Transfer]:
        """Direct `call`s (not tail `jmp`s) to `target`."""
        return [t for t in self.transfers().get(target, []) if t.mnemonic == "call"]

    def tail_jumps(self, target: int) -> list[Transfer]:
        return [t for t in self.transfers().get(target, []) if t.mnemonic == "jmp"]

    def call_targets(self) -> frozenset[int]:
        if self._call_targets is None:
            self._call_targets = frozenset(
                target
                for target, found in self.transfers().items()
                if any(t.mnemonic == "call" for t in found)
            )
        return self._call_targets

    def branch_targets(self) -> frozenset[int]:
        """Every address a direct `call`, `jmp` or conditional jump names (T603).

        The places control can enter without falling through from the previous instruction, as
        far as direct transfers go. A jump table or a pointer is not in it.
        """
        if self._branch_targets is None:
            found: set[int] = set()
            for insn in self.insns:
                if not insn.operands or len(insn.operands) != 1:
                    continue
                if capstone.CS_GRP_JUMP not in insn.groups and insn.mnemonic != "call":
                    continue
                if insn.operands[0].type == cs_x86.X86_OP_IMM:
                    found.add(insn.operands[0].imm & 0xFFFFFFFF)
            self._branch_targets = frozenset(found)
        return self._branch_targets

    def indirect_transfer_count(self) -> int:
        """`call`/`jmp` through a register or memory: the callers no direct census can name."""
        if self._indirect_transfers is None:
            count = 0
            for insn in self.insns:
                if insn.mnemonic not in TRANSFERS or len(insn.operands) != 1:
                    continue
                if insn.operands[0].type in (cs_x86.X86_OP_REG, cs_x86.X86_OP_MEM):
                    count += 1
            self._indirect_transfers = count
        return self._indirect_transfers

    # ------------------------------------------------------------------ data references

    def absolute_operand_refs(self, address: int) -> list[capstone.CsInsn]:
        """Instructions with a `[address]` memory operand (no base, no index) or an `imm` of it.

        Found from the raw byte occurrences of the address, so only the few instructions that
        contain those four bytes have their operands decoded.
        """
        found: list[capstone.CsInsn] = []
        for _, hit, _ in self.dword_hits(address):
            insn = self._covering(hit)
            if insn is None or insn in found:
                continue
            for op in insn.operands:
                if op.type == cs_x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    if (op.mem.disp & 0xFFFFFFFF) == address:
                        found.append(insn)
                        break
                elif op.type == cs_x86.X86_OP_IMM and (op.imm & 0xFFFFFFFF) == address:
                    if capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
                        found.append(insn)
                        break
        found.sort(key=lambda item: item.address)
        return found

    def _covering(self, address: int) -> capstone.CsInsn | None:
        position = bisect_right(self._starts, address) - 1
        if position < 0:
            return None
        insn = self.insns[position]
        return insn if insn.address <= address < insn.address + insn.size else None

    def dword_hits(self, value: int) -> list[tuple[str, int, bool]]:
        """Every 4-byte little-endian occurrence of `value` in every section's initialised bytes.

        Returns `(section, address, in_decoded_code)`. A hit inside a decoded instruction is
        usually an immediate or displacement, not a stored pointer. The search is the upper
        bound that rules out indirect dispatch: no hit outside code means no pointer table.
        """
        needle = struct.pack("<I", value)
        hits: list[tuple[str, int, bool]] = []
        for section in self.image.xbe.sections:
            body = self.image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            position = body.find(needle)
            while position != -1:
                address = section.virtual_addr + position
                hits.append((section.name, address, self._inside_instruction(address)))
                position = body.find(needle, position + 1)
        return hits

    def dword_hits_many(self, values: Iterable[int]) -> dict[int, list[tuple[str, int, bool]]]:
        """`dword_hits` for many values in ONE pass over the image, the same hits in the same order.

        `dword_hits` rescans every section per value, which is quadratic for a barrier over
        hundreds of thousands of body addresses (T599). This reads each section once per byte
        offset 0..3 as aligned little-endian dwords, so every 4-byte window (overlapping ones
        included, exactly the windows `bytes.find(needle, position + 1)` visits) is tested for
        membership in `values` once. Values with no hit are absent from the result, and the
        hits of a value are ordered by section, then by address, as `dword_hits` returns them.
        """
        wanted = frozenset(values)
        found: dict[int, list[tuple[str, int, bool]]] = {}
        if not wanted:
            return found
        for section in self.image.xbe.sections:
            body = self.image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            positions: dict[int, list[int]] = {}
            for offset in range(4):
                count = (len(body) - offset) // 4
                if count <= 0:
                    continue
                words = array("I")
                if words.itemsize != 4:
                    raise RuntimeError("array('I') is not 32 bits on this platform")
                words.frombytes(body[offset : offset + 4 * count])
                if sys.byteorder == "big":
                    words.byteswap()
                for index, word in enumerate(words):
                    if word in wanted:
                        positions.setdefault(word, []).append(offset + 4 * index)
            for word, offsets in positions.items():
                for position in sorted(offsets):
                    address = section.virtual_addr + position
                    found.setdefault(word, []).append(
                        (section.name, address, self._inside_instruction(address))
                    )
        return found

    def _inside_instruction(self, address: int) -> bool:
        return self._covering(address) is not None

    # ------------------------------------------------------------------ functions

    def entry_candidates(self, address: int) -> Iterator[int]:
        """Plausible function entries at or below `address`, nearest first.

        A stand-in for a function table, which this image does not have. An address
        qualifies when it follows `int3` padding, or follows a return or an unconditional
        `jmp` and is a direct call target or opens with `push ebp` plus a frame setup or
        `sub esp`. Which candidate is the real entry is decided by the caller: the right
        one is the nearest whose forward flood reaches `address` (`flow.flow_for`).
        """
        targets = self.call_targets()
        position = self.index.get(address)
        if position is None:
            raise KeyError(f"{address:#x} is not an instruction boundary")
        while position >= 0:
            if position == 0 or self._qualifies_as_entry(position, targets):
                yield self.insns[position].address
            position -= 1

    def _qualifies_as_entry(self, position: int, targets: frozenset[int]) -> bool:
        insn = self.insns[position]
        previous = self.insns[position - 1]
        if previous.address + previous.size != insn.address:
            return True
        if previous.mnemonic == "int3":
            return True
        ends_path = capstone.CS_GRP_RET in previous.groups or (
            previous.mnemonic == "jmp" and capstone.CS_GRP_BRANCH_RELATIVE in previous.groups
        )
        if not ends_path:
            return False
        return insn.address in targets or self._looks_like_prologue(position)

    def _looks_like_prologue(self, position: int) -> bool:
        insn = self.insns[position]
        if insn.mnemonic == "sub" and insn.op_str.startswith("esp, "):
            return True
        if insn.mnemonic == "push" and insn.op_str == "ebp" and position + 1 < len(self.insns):
            following = self.insns[position + 1]
            return following.mnemonic in ("mov", "lea") and following.op_str.startswith("ebp, ")
        return False

    def iter_range(self, low: int, high: int) -> Iterator[capstone.CsInsn]:
        position = bisect_right(self._starts, low - 1)
        while position < len(self.insns) and self.insns[position].address < high:
            yield self.insns[position]
            position += 1
