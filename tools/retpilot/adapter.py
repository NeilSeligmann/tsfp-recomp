"""Isolated integer snapshot/import adapter v1; never production dispatch.

The trusted mapping owner supplies an exclusive live lease and accurate host
backing. Offline original execution has no shared effects until checked import.
A stale/refused/partial import is an explicit pending quarantine, never rollback
or silent reexecution. Foreign pointers/races and process-crash durability are
outside this correctly typed live-storage API.
"""

from __future__ import annotations

import ctypes as C
import hashlib
from collections.abc import Callable
from contextlib import AbstractContextManager
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from capstone import CS_AC_READ, CS_ARCH_X86, CS_MODE_32, Cs, CsInsn
from capstone.x86 import X86_OP_MEM
from unicorn import UC_ERR_FETCH_PROT, UC_ERR_FETCH_UNMAPPED

from tools.name_candidates import Image

from .generate import IMAGE_SHA, ROOT, canonical, describe, digest
from .runner import (
    INTEGER_EFLAGS,
    Context,
    Frame,
    Machine,
    OriginalResult,
    Page,
    Profile,
    View,
    original_handoff,
)

ADAPTER_IDENTITY = "guest-ret-integer-adapter-v1"
Pages = dict[int, bytes]


@dataclass(frozen=True)
class MappingLease:
    """Owner-held exclusive mapping/backing lease, including snapshot/import I/O.

    snapshot reads all and only view pages, irrespective of guest permissions.
    apply_pages returns True iff all supplied pages committed, False iff none.
    Both callbacks run within the owner's lease; backing/context/frame storage
    stays valid. A detected partial callback result is quarantined without undo.
    The owner may advance metadata only after releasing this immutable-view
    lease; the next execution slice explicitly rebinds the fresh view.
    """

    view: View
    generation: int
    memory_offset: int
    snapshot: Callable[[], Pages]
    apply_pages: Callable[[Pages], bool]


class MappingOwner(Protocol):
    def acquire(self) -> AbstractContextManager[MappingLease]: ...


@dataclass(frozen=True)
class Journal:
    source: str
    before_eip: int
    after_eip: int
    opcode: str
    instruction: bytes
    retired: bool
    kind: str
    error: int | None = None


@dataclass(frozen=True)
class Snapshot:
    context: bytes
    frames: bytes
    pages: tuple[tuple[int, bytes], ...]
    view_key: tuple
    remaining: int


@dataclass(frozen=True)
class Pending:
    snapshot: Snapshot
    machine: bytes
    pages: tuple[tuple[int, bytes], ...]
    journal: Journal
    remaining: int
    frame_bytes: bytes
    depth: int
    fast_returns: int
    redirected_returns: int
    completed: int


@dataclass(frozen=True)
class AdapterResult:
    kind: str
    remaining: int
    journal: tuple[Journal, ...]
    reason: str = ""
    error: int | None = None
    pending: Pending | None = None


class LeaseRejected(ValueError):
    """Unqualified owner description; no guest exception is inferred."""


def _view_key(lease: MappingLease) -> tuple:
    v = lease.view
    if not isinstance(v, View) or not v.pages or not 0 < v.page_count <= 1048576:
        raise LeaseRejected("missing live view storage")
    if not lease.generation or lease.generation != v.generation:
        raise LeaseRejected("lease/view generation disagreement")
    return (
        lease.memory_offset,
        lease.generation,
        v.abi,
        v.size,
        v.required_state,
        v.reserved,
        v.image_sha256,
        tuple(
            (p.address, p.permissions, p.generation, p.code_generation)
            for p in (v.pages[i] for i in range(v.page_count))
        ),
    )


def _pages(lease: MappingLease) -> Pages:
    pages = lease.snapshot()
    expected = {lease.view.pages[i].address for i in range(lease.view.page_count)}
    if set(pages) != expected or any(a & 4095 or len(b) != 4096 for a, b in pages.items()):
        raise LeaseRejected("incomplete or misaligned owner snapshot")
    return {a: bytes(b) for a, b in pages.items()}


def _private_view(view: View) -> View:
    output = View.from_buffer_copy(bytes(view))
    storage = (Page * view.page_count)(
        *(Page.from_buffer_copy(bytes(view.pages[i])) for i in range(view.page_count))
    )
    output.pages = storage
    output._storage = storage
    return output


def _tuple_pages(pages: Pages) -> tuple[tuple[int, bytes], ...]:
    return tuple(sorted(pages.items()))


def _uncertain_read(view: View, machine: Machine, insn: CsInsn) -> bool:
    """Refuse the known oracle domain; never manufacture a permission fault.

    Genuine fetched flat integer bytes only. The guard covers decoded explicit
    reads and implicit POP/RET stack reads; LEA does not consume memory. It is a
    conservative no-verdict domain, not a complete guest MMU implementation.
    """
    pages = {view.pages[i].address: view.pages[i] for i in range(view.page_count)}
    for i in range(insn.size):
        p = pages.get((insn.address + i) & ~4095)
        if p is None or not p.permissions & 4 or p.code_generation != p.generation:
            return False  # actual fetch/stale qualification takes precedence
    registers = dict(
        zip(("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"), machine.r, strict=True)
    )
    registers.update({name[1:]: value & 0xFFFF for name, value in tuple(registers.items())})
    reads = []
    if insn.mnemonic in ("ret", "pop"):
        reads.append((machine.r[4], 2 if 0x66 in insn.prefix else 4))
    if insn.mnemonic != "lea":
        for operand in insn.operands:
            if operand.type != X86_OP_MEM or not operand.access & CS_AC_READ:
                continue
            mem = operand.mem
            if mem.segment and insn.reg_name(mem.segment) not in ("ds", "ss", "es", "cs"):
                continue  # state-consuming segment instructions stay unexecuted
            names = [insn.reg_name(reg) if reg else "" for reg in (mem.base, mem.index)]
            if any(name and name not in registers for name in names):
                continue
            address = registers.get(names[0], 0) + registers.get(names[1], 0) * mem.scale + mem.disp
            address &= 0xFFFF if 0x67 in insn.prefix else 0xFFFFFFFF
            reads.append((address, operand.size))
    for address, size in reads:
        end = address + size - 1
        if size <= 1 or end > 0xFFFFFFFF or address // 4096 == end // 4096:
            continue
        first, second = pages.get(address & ~4095), pages.get(end & ~4095)
        if (
            first is not None
            and first.permissions & 1
            and second is not None
            and not second.permissions & 1
        ):
            return True
    return False


class IntegerAdapter:
    """Single-owner ABI2 driver with explicit pending import and retirement log.

    Caller retains correctly typed live context/profile/frame storage and owner.
    Constructor does not open/close contexts or infer a view. An unfinished
    Pending prevents new execution; resume_import retries only that exact result.
    """

    def __init__(
        self,
        library: C.CDLL,
        context: Context,
        profile: C.POINTER(Profile),
        owner: MappingOwner,
        xbe: Path = ROOT / "build/default.xbe",
    ) -> None:
        self.library, self.context, self.profile, self.owner, self.xbe = (
            library,
            context,
            profile,
            owner,
            xbe,
        )
        if digest(xbe) != IMAGE_SHA:
            raise ValueError("adapter original image identity mismatch")
        current_profile = describe(xbe)
        source_sha = hashlib.sha256(canonical(current_profile["sources"])).hexdigest().encode()
        self._instructions = current_profile["instructions"]
        if not profile or profile.contents.source_sha256 != source_sha:
            raise ValueError("adapter/profile source pins are stale")
        self.pending: Pending | None = None
        self._applied = False
        self.journal: list[Journal] = []
        self._view: View | None = None  # retain bound Python view/page storage between slices
        self._decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        self._decoder.detail = True

    def _valid(self) -> bool:
        return self.library.ret_pilot_run(C.byref(self.context), self.profile, 0) in (1, 3)

    def _bind(self, lease: MappingLease) -> tuple:
        key = _view_key(lease)
        if lease.memory_offset != self.context.memory_offset:
            raise LeaseRejected("wrong context backing")
        if self.library.ret_pilot_bind_view(C.byref(self.context), C.byref(lease.view)) != 0:
            raise LeaseRejected("unqualified view/context")
        self._view = lease.view
        return key

    def _snapshot(self, lease: MappingLease, key: tuple, remaining: int) -> Snapshot:
        ctx = self.context
        return Snapshot(
            bytes(ctx),
            C.string_at(ctx.frames, ctx.capacity * C.sizeof(Frame)),
            _tuple_pages(_pages(lease)),
            key,
            remaining,
        )

    def _instruction(self, snapshot: Snapshot) -> CsInsn | None:
        """Decode captured bytes only, with no inferred default mapping."""
        ctx, pages = Context.from_buffer_copy(snapshot.context), dict(snapshot.pages)
        body = bytearray()
        for i in range(15):
            address = ctx.machine.eip + i
            page = pages.get(address & ~4095)
            if address > 0xFFFFFFFF or page is None:
                break
            body.append(page[address & 4095])
        return next(self._decoder.disasm(bytes(body), ctx.machine.eip), None)

    def _journal_original(
        self, snapshot: Snapshot, result: OriginalResult, insn: CsInsn | None
    ) -> Journal:
        before = Context.from_buffer_copy(snapshot.context).machine
        opcode = insn.mnemonic if insn is not None else ""
        retired = result.kind in ("STOP", "BUDGET") and result.trace == [before.eip]
        if result.kind == "ORIGINAL-FAULT" and result.trace == [before.eip]:
            # CALL/RET publish ESP only after their actual stack access completes.
            # A later target fetch may fault even under count1; it is not a failed
            # stack operation. No pre-execution trace is counted as retirement.
            if opcode == "call":
                retired = result.machine.r[4] == (before.r[4] - 4) & 0xFFFFFFFF
            elif opcode == "ret":
                immediate = int(insn.op_str, 0) if insn.op_str else 0
                retired = result.machine.r[4] == (before.r[4] + 4 + immediate) & 0xFFFFFFFF
            else:
                retired = result.error in (UC_ERR_FETCH_PROT, UC_ERR_FETCH_UNMAPPED)
        return Journal(
            "original",
            before.eip,
            result.machine.eip,
            opcode,
            bytes(insn.bytes) if insn is not None else b"",
            retired,
            result.kind,
            result.error,
        )

    def _pending_result(
        self, snapshot: Snapshot, result: OriginalResult, event: Journal, insn: CsInsn | None
    ) -> Pending:
        old = Context.from_buffer_copy(snapshot.context)
        frames = (Frame * old.capacity).from_buffer_copy(snapshot.frames)
        depth, fast, redirected = old.depth, old.fast_returns, old.redirected_returns
        if event.retired and event.opcode == "call":
            if depth == old.capacity:
                raise LeaseRejected("CALL tracking capacity exhausted before import")
            assert insn is not None
            f = frames[depth]
            f.owner, f.generation = old.owner, old.generation
            f.return_slot, f.expected_resume, f.active = (
                (old.machine.r[4] - 4) & 0xFFFFFFFF,
                (old.machine.eip + insn.size) & 0xFFFFFFFF,
                1,
            )
            depth += 1
        elif event.retired and event.opcode == "ret":
            slot, target = old.machine.r[4], result.machine.eip
            if depth and frames[depth - 1].return_slot == slot:
                depth -= 1
                if frames[depth].expected_resume == target:
                    fast = (fast + 1) & 0xFFFFFFFF
                else:
                    redirected = (redirected + 1) & 0xFFFFFFFF
                frames[depth].active = 0
            else:
                while depth:
                    depth -= 1
                    frames[depth].active = 0
                redirected = (redirected + 1) & 0xFFFFFFFF
        return Pending(
            snapshot,
            bytes(result.machine),
            _tuple_pages(result.pages),
            event,
            snapshot.remaining - int(event.retired),
            bytes(frames),
            depth,
            fast,
            redirected,
            (old.completed + int(event.retired)) & 0xFFFFFFFFFFFFFFFF,
        )

    def _import(self) -> str | None:
        pending = self.pending
        assert pending is not None
        snapshot, ctx = pending.snapshot, self.context
        with self.owner.acquire() as lease:
            try:
                if not self._valid():
                    return "CONTEXT-LIFETIME"
                if _view_key(lease) != snapshot.view_key:
                    return "STALE-VIEW"
                if (
                    bytes(ctx) != snapshot.context
                    or C.string_at(ctx.frames, ctx.capacity * C.sizeof(Frame)) != snapshot.frames
                ):
                    return "CHANGED-CONTEXT"
                current = _pages(lease)
                if not self._applied and _tuple_pages(current) != snapshot.pages:
                    return "CHANGED-RAM"
            except LeaseRejected as error:
                return str(error)
            after = dict(pending.pages)
            if self._applied:
                if current != after:
                    return "PARTIAL-IMPORT"
            else:
                changes = {a: body for a, body in after.items() if body != current[a]}
                committed = lease.apply_pages(changes) if changes else True
                self._applied = bool(committed)
                try:
                    actual = _pages(lease)
                except LeaseRejected:
                    return "INCOMPLETE-IMPORT-SNAPSHOT"
                if actual != after:
                    # Preserve actual owner writes; never undo or reexecute to
                    # conceal a partial commit. An applied phase retries import
                    # publication only, and never writes the RAM a second time.
                    self._applied = self._applied or actual != current
                    return "PARTIAL-IMPORT" if actual != current else "IMPORT-REFUSED"
                if not committed:
                    self._applied = True
                    return "COMMIT-STATUS-DISAGREEMENT"
            C.memmove(C.addressof(ctx.machine), pending.machine, C.sizeof(Machine))
            C.memmove(ctx.frames, pending.frame_bytes, len(pending.frame_bytes))
            ctx.depth, ctx.fast_returns, ctx.redirected_returns = (
                pending.depth,
                pending.fast_returns,
                pending.redirected_returns,
            )
            ctx.completed = pending.completed
            ctx.handoff_reason = 0
            self.journal.append(pending.journal)
            self.pending = None
            self._applied = False
        return None

    def resume_import(self) -> AdapterResult:
        """Import a queued exact result without reexecuting any guest instruction."""
        if self.pending is None:
            return AdapterResult("NO-PENDING", 0, tuple(self.journal))
        pending = self.pending
        try:
            reason = self._import()
        except LeaseRejected as error:
            reason = str(error)
        if reason:
            return AdapterResult(
                "PENDING",
                pending.remaining,
                tuple(self.journal),
                reason,
                pending.journal.error,
                pending,
            )
        return AdapterResult(
            pending.journal.kind,
            pending.remaining,
            tuple(self.journal),
            error=pending.journal.error,
        )

    def run(self, budget: int) -> AdapterResult:
        if not isinstance(budget, int) or not 0 <= budget <= 0xFFFFFFFF:
            raise ValueError("adapter budget must be uint32")
        if self.pending is not None:
            return AdapterResult(
                "PENDING",
                self.pending.remaining,
                tuple(self.journal),
                "IMPORT-REQUIRED",
                self.pending.journal.error,
                self.pending,
            )
        if not self._valid():
            return AdapterResult("REJECT", budget, tuple(self.journal), "CONTEXT-PROFILE-LIFETIME")
        ctx, remaining = self.context, budget
        while remaining and ctx.machine.eip != ctx.stop_va:
            try:
                with self.owner.acquire() as lease:
                    key = self._bind(lease)
                    before_eip, completed = ctx.machine.eip, ctx.completed
                    status = self.library.ret_pilot_run(C.byref(ctx), self.profile, 1)
                    if ctx.completed != completed:
                        remaining -= 1
                        instruction = self._instructions[str(before_eip)]
                        self.journal.append(
                            Journal(
                                "compiled",
                                before_eip,
                                ctx.machine.eip,
                                instruction["text"].split()[0],
                                bytes.fromhex(instruction["bytes"]),
                                True,
                                "STOP" if status == 1 else "BUDGET",
                            )
                        )
                    if status in (1, 3):
                        continue
                    if status != 2:
                        return AdapterResult(
                            "FRAME-LIMIT" if status == 5 else "REJECT",
                            remaining,
                            tuple(self.journal),
                        )
                    snapshot = self._snapshot(lease, key, remaining)
                    view = _private_view(lease.view)
            except LeaseRejected as error:
                return AdapterResult("UNEXECUTED-LEASE", remaining, tuple(self.journal), str(error))
            before = Context.from_buffer_copy(snapshot.context)
            # Adapter import is integer-only, including debug events. No TF
            # exception is normalized or guessed as an instruction retirement.
            if (
                view.required_state != 1
                or not before.machine.eflags & 2
                or before.machine.eflags & ~INTEGER_EFLAGS
            ):
                return AdapterResult("UNEXECUTED-STATE", remaining, tuple(self.journal))
            insn = self._instruction(snapshot)
            authentic = insn is not None and Image(self.xbe).read(
                before.machine.eip, insn.size
            ) == bytes(insn.bytes)
            if authentic and (
                insn.mnemonic == "int3"
                or (insn.mnemonic in ("call", "ret") and 0x66 in insn.prefix)
            ):
                return AdapterResult("UNEXECUTED-OPCODE", remaining, tuple(self.journal))
            if authentic and _uncertain_read(view, before.machine, insn):
                return AdapterResult("UNEXECUTED-ORACLE-LIMIT", remaining, tuple(self.journal))
            # Resource refusal is not a guest fault. Only genuinely original
            # CALL bytes can invoke the finite diagnostic frame-capacity limit.
            if authentic and insn.mnemonic == "call" and before.depth == before.capacity:
                return AdapterResult("FRAME-LIMIT", remaining, tuple(self.journal))
            result = original_handoff(
                before.machine,
                dict(snapshot.pages),
                before.stop_va,
                1,
                xbe=self.xbe,
                view=view,
                view_generation=before.view_generation,
            )
            event = self._journal_original(snapshot, result, insn)
            if result.kind.startswith("UNEXECUTED"):
                self.journal.append(event)
                return AdapterResult(result.kind, remaining, tuple(self.journal))
            self.pending = self._pending_result(snapshot, result, event, insn)
            imported = self.resume_import()
            if imported.kind == "PENDING":
                return imported
            remaining = imported.remaining
            if result.kind == "ORIGINAL-FAULT":
                return imported
        return AdapterResult(
            "STOP" if ctx.machine.eip == ctx.stop_va else "BUDGET", remaining, tuple(self.journal)
        )
