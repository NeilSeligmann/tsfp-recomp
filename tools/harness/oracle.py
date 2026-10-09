# SPDX-License-Identifier: GPL-3.0-or-later
"""The oracle: Unicorn executing the ORIGINAL x86-32 bytes from the retail XBE.

This is the reference the whole project is measured against, so it is kept as close to
"just run the real instructions" as possible. No instrumentation of the guest, no
rewriting, no assumptions about what the function does.

Two implementation notes that matter for correctness and for scale:

**The write-set is computed against a reconstructed baseline, not against a snapshot
taken inside the write hook.** Whether Unicorn invokes `UC_HOOK_MEM_WRITE` before or
after committing the store is an implementation detail; depending on it would make the
first byte of every page silently wrong if it ever changed. Instead the hook is used
only to learn WHICH pages were touched, and the original contents come from the
pristine image plus this case's patches, which is exact by construction.

**One `Uc` is built for the whole run and only dirty pages are restored between cases.**
The throwaway version rebuilt the emulator and rewrote all 16 MB per case, so cost grew
with the size of the guest window instead of with the function under test. That is why
it never finished a scaled run: two attempts died on wall-clock, one at an 1,800 s
timeout and one stopped at ~85 minutes. Restoring only touched pages is what makes
thousands of cases affordable. Translated blocks are explicitly flushed after
restoration and ordered input patches, so code from a previous case cannot survive
the memory reset (T1508).
"""

from __future__ import annotations

import hashlib
from typing import Any

from .callstub import STUB_EAX, STUB_ECX, STUB_EDX, CallSite, StubPlan
from .code_safety import Certificate, ExecutionGuard
from .fault_diagnostics import FaultObservation
from .guarded_tables import GuardedJumpTable, table_regions
from .image import GuestImage
from .model import NAN_PAIR_FAULT, Case, ExecResult, Reachability
from .scoped_fixture_binding import Binding as ScopedFixtureBinding
from .scoped_fixture_code import CustomExecutionGuard
from .seeding import GUEST_LO, GUEST_SPAN, KPCR_BASE, SENTINEL
from .stackprobe import (
    STACKPROBE_SIZE,
    original_stackprobe_caller_proven,
    original_stackprobe_proven,
)
from .static_tables import StaticJumpTable
from .x87 import DEFAULT_CONTROL, double_to_extended, extended_to_double_bits
from .x87_state import X87State

#: The model-specific register that holds the fs segment base in Unicorn.
IA32_FS_BASE_MSR = 0xC0000100

PAGE = 0x1000
PAGE_MASK = ~(PAGE - 1)

#: Enough for any tier-1 function, few enough that a runaway loop is cut short rather
#: than hanging the run. Exhausting it is reported as a fault, never as a pass.
DEFAULT_MAX_INSNS = 200_000

#: IF set, DF clear. The guest is entered with a sane, fixed flags word so that the
#: initial state is fully determined by the seed.
BASE_EFLAGS = 0x202
DF_BIT = 0x400


class UnicornOracle:
    """Executes original guest bytes and reports final registers plus the write-set."""

    CASE_ISOLATION_CONTRACT = "pristine-image-ordered-patches-tbflush-v1"

    def __init__(
        self,
        image: bytes,
        *,
        base: int = GUEST_LO,
        span: int = GUEST_SPAN,
        sentinel: int = SENTINEL,
        max_insns: int = DEFAULT_MAX_INSNS,
        record_loads: bool = False,
    ) -> None:
        if len(image) != span:
            raise ValueError(f"image is {len(image)} bytes, expected {span}")
        # Imported here, not at module scope, so that `tools.harness.model`,
        # `.compare` and `.seeding` stay importable -- and unit-testable -- on a machine
        # with no Unicorn installed.
        import unicorn
        from unicorn import x86_const

        self._uc_mod = unicorn
        self._x86 = x86_const
        self._image = image
        self._base = base
        self._span = span
        self._sentinel = sentinel
        self._max_insns = max_insns

        self._regs = [
            x86_const.UC_X86_REG_EAX,
            x86_const.UC_X86_REG_ECX,
            x86_const.UC_X86_REG_EDX,
            x86_const.UC_X86_REG_EBX,
            x86_const.UC_X86_REG_ESP,
            x86_const.UC_X86_REG_EBP,
            x86_const.UC_X86_REG_ESI,
            x86_const.UC_X86_REG_EDI,
        ]

        self._uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
        self._uc.mem_map(base, span)
        self._uc.mem_write(base, image)
        # The fs segment base the subject driver mirrors in `g_fs_base` (T469).
        self._uc.reg_write(x86_const.UC_X86_REG_MSR, (IA32_FS_BASE_MSR, KPCR_BASE))
        self._touched: set[int] = set()
        self._uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._on_write)
        # Loads are recorded only on request. A Python callback per guest memory read is
        # not free, and only replacement mode uses what it learns (see feedback.py).
        self._loads: list[tuple[int, int, int]] = []
        if record_loads:
            self._uc.hook_add(unicorn.UC_HOOK_MEM_READ, self._on_read)
        # One code hook serves both jobs: counting what ran (reachability) and noticing
        # when execution arrives at a call site that must be stubbed. Per-instruction
        # Python callbacks are not free, but a separate hook per call site would mean
        # adding and removing hooks for every function, and the counting has to happen
        # on every instruction anyway.
        self._uc.hook_add(unicorn.UC_HOOK_CODE, self._on_code)
        # T1576: remember the address of an invalid access. Returning False leaves Unicorn's
        # behaviour unchanged (the same UcError), so no verdict or receipt moves.
        self._last_invalid_addr: int | None = None
        self._uc.hook_add(unicorn.UC_HOOK_MEM_INVALID, self._on_invalid)
        #: T1620: fp-scalar-v1 observer (ScalarFpTracker) or None. Legacy runs never set it.
        self.scalar_fp: Any = None
        self._insns = 0
        self._last_invalid_addr = None
        self._covered: set[int] = set()
        self._body_lo = 0
        self._body_hi = 0
        self._stub_sites: dict[int, CallSite] = {}
        self._stub_applied = 0
        self._passthrough_applied = 0
        self._contract_fault: str | None = None
        self._pending_site: CallSite | None = None
        self._static_tables: tuple[StaticJumpTable, ...] = ()
        #: T1576 guarded-jump evidence of the LAST run: ("slot"|"default", table site, slot or -1,
        #: target) per executed dispatch/taken default, ("tail", site, target) per executed tail.
        self.arm_events: list[tuple] = []
        self.tail_sites: dict[int, int] = {}
        #: T1576: closure-node entries whose execution is recorded as an ("enter", ...) event.
        self.watch_entries: frozenset[int] = frozenset()
        self._ja_pending: GuardedJumpTable | None = None
        self._guard_active = False
        self._tail_pending: tuple[int, int] | None = None
        #: Pages modified since the last restore: written by the guest, or patched.
        self._dirty: set[int] = set()
        self._errnames = {
            value: name
            for name, value in vars(unicorn.unicorn_const).items()
            if name.startswith("UC_ERR_") and isinstance(value, int)
        }

    def _on_invalid(self, _uc: Any, _access: int, address: int, *_rest: Any) -> bool:
        self._last_invalid_addr = address & 0xFFFFFFFF
        return False

    def _on_write(
        self, _uc: Any, _access: int, address: int, size: int, _value: int, _data: Any
    ) -> None:
        for table in self._static_tables:
            if (
                address < table.body_va + table.body_size and table.body_va < address + size
            ) or any(
                address < base + len(data) and base < address + size
                for base, data in table_regions(table)
            ):
                self._contract_fault = "STATIC-JUMP-TABLE-WRITE"
                self._uc.emu_stop()
        # The hook fires for out-of-window stores too -- it runs before the mapping is
        # validated, so an unmapped write is seen here and only then raises. Recording
        # such a page would make the next restore write to unmapped memory and abort the
        # whole run, so the window check is mandatory, not defensive.
        for addr in range(address & PAGE_MASK, address + size, PAGE):
            page = addr & PAGE_MASK
            if self._base <= page < self._base + self._span:
                self._touched.add(page)

    def _on_read(
        self, uc: Any, _access: int, address: int, size: int, _value: int, _data: Any
    ) -> None:
        # The value is read here, before the guest consumes it. An address outside the
        # window is skipped: the load will fault and there is nothing there to learn.
        if self._base <= address and address + size <= self._base + self._span and size <= 8:
            raw = bytes(uc.mem_read(address, size))
            self._loads.append((address, size, int.from_bytes(raw, "little")))

    def _on_code(self, uc: Any, address: int, _size: int, _data: Any) -> None:
        guard = getattr(self, "_code_guard", None)
        if guard is not None:
            guard.before(
                address,
                _size,
                uc.reg_read(self._x86.UC_X86_REG_ESP),
                lambda at, count: bytes(uc.mem_read(at, count)),
            )
            if guard.violation:
                uc.emu_stop()
                return
        tracker = self.scalar_fp
        if tracker is not None and tracker.before(uc, address):
            self._contract_fault = NAN_PAIR_FAULT
            uc.emu_stop()
            return
        self._insns += 1
        if self._body_lo <= address < self._body_hi:
            self._covered.add(address)
        if self._static_tables and (
            address == self._body_lo or any(address == table.site for table in self._static_tables)
        ):
            for table in self._static_tables:
                body = bytes(uc.mem_read(table.body_va, table.body_size))
                if hashlib.sha256(body).hexdigest() != table.body_sha256 or any(
                    bytes(uc.mem_read(base, len(data))) != data
                    for base, data in table_regions(table)
                ):
                    self._contract_fault = "STATIC-JUMP-TABLE-IDENTITY"
                    uc.emu_stop()
                    return
                if address == table.site:
                    register = getattr(self._x86, f"UC_X86_REG_{table.register.upper()}")
                    slot = uc.reg_read(register)
                    if slot >= len(table.targets):
                        self._contract_fault = "STATIC-JUMP-TABLE-INDEX"
                        uc.emu_stop()
                        return
                    if isinstance(table, GuardedJumpTable):
                        self.arm_events.append(("slot", table.site, slot, table.targets[slot]))
        if self._guard_active:
            self._record_guard_events(address)
        site = self._stub_sites.get(address)
        if site is not None and site.passthrough:
            code = bytes(uc.mem_read(site.target_va, STACKPROBE_SIZE))
            caller = bytes(uc.mem_read(self._body_lo, self._body_hi - self._body_lo))
            if not (
                original_stackprobe_proven(site.target_va, code)
                and original_stackprobe_caller_proven(self._body_lo, caller)
            ):
                self._contract_fault = "PASSTHROUGH-PROOF"
                uc.emu_stop()
            else:
                self._passthrough_applied += 1
            return
        if site is not None:
            # Deliberately changes NO state here. Whether Unicorn lets the current
            # instruction complete after `emu_stop` from a code hook is an
            # implementation detail, and a stub that half-applied around it would
            # desync esp by exactly four bytes -- reported as a caller defect. The
            # effects are applied in `_apply_stub` once emu_start has returned and the
            # actual stopping point can be observed.
            self._pending_site = site
            uc.emu_stop()

    def _record_guard_events(self, address: int) -> None:
        """Default arm (JA taken) and tail (out-of-body JMP executed) events, from the trace."""
        pending = self._ja_pending
        self._ja_pending = None
        if pending is not None and address == pending.default_target:
            self.arm_events.append(("default", pending.site, -1, address))
        tail = self._tail_pending
        self._tail_pending = None
        if tail is not None and address == tail[1]:
            self.arm_events.append(("tail", tail[0], -1, address))
        for table in self._static_tables:
            if isinstance(table, GuardedJumpTable) and address == table.guard_site:
                self._ja_pending = table
        if address in self.tail_sites:
            self._tail_pending = (address, self.tail_sites[address])
        if address in self.watch_entries:
            self.arm_events.append(("enter", address, -1, address))

    def _mark_dirty(self, address: int, length: int) -> None:
        """Record pages the harness itself wrote through the API.

        `mem_write` from the Python side does NOT fire `UC_HOOK_MEM_WRITE`, so a page the
        stub's return-address push touched would otherwise never be restored and would
        leak into the next case's baseline -- the same class of bug as the subject's
        PATCH-versus-dirty-page split.
        """
        for offset in range(address & PAGE_MASK, address + length, PAGE):
            page = offset & PAGE_MASK
            if self._base <= page < self._base + self._span:
                self._touched.add(page)

    def _apply_stub(self, site: CallSite) -> str | None:
        """Make the callee's synthetic effects happen. Returns a fault name on confusion.

        Both of Unicorn's plausible stopping points are handled explicitly rather than
        assumed, and anything else becomes a named fault instead of a silent guess.
        """
        eip = self._uc.reg_read(self._x86.UC_X86_REG_EIP)
        esp = self._uc.reg_read(self._x86.UC_X86_REG_ESP)

        if eip == site.site_va:
            # Stopped before the call executed: the push has not happened yet, so the
            # stub performs it exactly as the hardware would.
            pushed = (esp - 4) & 0xFFFFFFFF
            self._uc.mem_write(pushed, site.return_va.to_bytes(4, "little"))
            self._mark_dirty(pushed, 4)
        elif eip == site.target_va:
            # The call completed: the hardware already pushed the return address, and
            # the write hook already recorded that page.
            pushed = esp
        else:
            return "STUB-DESYNC"

        # The callee's `ret`: pop the return address, then its stdcall arguments. The
        # 4-byte store itself stays in guest memory, because the hardware leaves it
        # there too and it must appear in both write-sets.
        self._uc.reg_write(self._x86.UC_X86_REG_ESP, (pushed + 4 + site.pop_bytes) & 0xFFFFFFFF)
        self._uc.reg_write(self._x86.UC_X86_REG_EAX, STUB_EAX)
        self._uc.reg_write(self._x86.UC_X86_REG_ECX, STUB_ECX)
        self._uc.reg_write(self._x86.UC_X86_REG_EDX, STUB_EDX)
        self._stub_applied += 1
        return None

    def _restore(self) -> None:
        """Put every dirty page back to the pristine image. O(pages), not O(window)."""
        for page in self._dirty:
            offset = page - self._base
            self._uc.mem_write(page, self._image[offset : offset + PAGE])
        self._dirty.clear()

    def run(
        self,
        case: Case,
        plan: StubPlan | None = None,
        *,
        static_tables: tuple[StaticJumpTable, ...] = (),
        code_safety: Certificate | None = None,
        code_safety_source: tuple[GuestImage, bytes] | None = None,
        scoped_fixture_binding: ScopedFixtureBinding | None = None,
        scoped_fixture_kind: str | None = None,
    ) -> ExecResult:
        """Execute `case` and return its final state, or the fault it took.

        `plan` stubs the named callees instead of executing them, so that a verdict on a
        call-bearing function is about the caller. The same table is sent to the subject,
        so neither side decides for itself what a callee does.
        """
        self._code_guard = None
        self.code_safety_observation = None
        guard_type = ExecutionGuard
        if scoped_fixture_binding is not None:
            if type(scoped_fixture_binding) is not ScopedFixtureBinding:
                raise ValueError("exact custom fixture Binding required")
            if code_safety is not None or code_safety_source is not None:
                raise ValueError("mixed named/custom code safety authority")
            if plan is not None or static_tables:
                raise ValueError("custom code safety refuses stubs/static-table exceptions")
            scoped_fixture_binding.prepare(case, scoped_fixture_kind)
            code_safety = scoped_fixture_binding.certificate
            code_safety_source = scoped_fixture_binding.source()
            guard_type = CustomExecutionGuard
        elif scoped_fixture_kind is not None:
            raise ValueError("orphaned custom fixture phase")
        if code_safety is None and code_safety_source is not None:
            raise ValueError("orphaned scoped code-safety source")
        if code_safety is not None:
            if scoped_fixture_binding is None and type(code_safety) is not Certificate:
                raise ValueError("exact immutable named-global Certificate required")
            if plan is not None or static_tables:
                raise ValueError("scoped code safety refuses stubs and static-table exceptions")
            if code_safety_source is None:
                raise ValueError("scoped code safety requires actual image/index source")
            source_image, source_index = code_safety_source
            if (
                type(source_image) is not GuestImage
                or type(source_index) is not bytes
                or source_image.data != self._image
                or source_image.base != self._base
            ):
                raise ValueError("scoped code safety original image/index mismatch")
            authenticated = getattr(self, "_code_safety_authenticated", None)
            if authenticated is None or not (
                authenticated[0] is code_safety
                and authenticated[1] is source_image
                and authenticated[2] is source_index
            ):
                # These three objects are independently authenticated immutable
                # inputs. A new object/index binds afresh; every case still checks
                # the live file in SourceBinding and all post-patch node bytes.
                code_safety.authenticate(source_image, source_index)
                self._code_safety_authenticated = (code_safety, source_image, source_index)
            self._code_guard = guard_type(code_safety, case, self._sentinel)
        self._restore()
        self._touched.clear()
        if self.scalar_fp is not None:
            self.scalar_fp.begin_run()
        self._insns = 0
        self._last_invalid_addr = None
        self._covered = set()
        self._loads = []
        self._stub_applied = 0
        self._passthrough_applied = 0
        self._contract_fault: str | None = None
        self._pending_site = None
        self._body_lo = case.va
        self._body_hi = case.va + case.size
        self._stub_sites = plan.sites_by_va if plan is not None else {}
        self._static_tables = static_tables
        self.arm_events = []
        self._ja_pending = None
        self._tail_pending = None
        self._guard_active = (
            bool(self.tail_sites)
            or bool(self.watch_entries)
            or any(isinstance(table, GuardedJumpTable) for table in static_tables)
        )

        # Patches are INITIAL state, so they are applied before execution and folded
        # into the baseline below rather than being counted as writes.
        patched: dict[int, int] = {}
        for addr, blob in case.patches:
            self._uc.mem_write(addr, blob)
            for i, byte in enumerate(blob):
                patched[addr + i] = byte
            for offset in range(addr & PAGE_MASK, addr + len(blob), PAGE):
                page = offset & PAGE_MASK
                if self._base <= page < self._base + self._span:
                    self._dirty.add(page)

        self._fault_patched = patched

        # API mem_write restores/patches memory but Unicorn may retain translated
        # instructions from an earlier case (T1508). Flush AFTER ordered patches,
        # not merely after restoration: a patch can change the next instruction too.
        # Guest stores during this case retain ordinary Unicorn self-modification
        # behavior; no write, fault, or case is excluded by this isolation reset.
        self._uc.ctl_flush_tb()
        if self._code_guard is not None:
            self._code_guard.preflight(lambda at, count: bytes(self._uc.mem_read(at, count)))

        for reg, value in zip(self._regs, case.regs, strict=True):
            self._uc.reg_write(reg, value)
        eflags = BASE_EFLAGS | (DF_BIT if case.df else 0)
        self._uc.reg_write(self._x86.UC_X86_REG_EFLAGS, eflags)
        self._raw_x87_requested = case.x87_state is not None
        self._load_fp(case)
        # Always reset vector state, even when this case does not observe it.
        for index in range(8):
            self._uc.reg_write(
                getattr(self._x86, f"UC_X86_REG_XMM{index}"),
                case.xmm[index] if case.xmm is not None else 0,
            )
        self._uc.reg_write(self._x86.UC_X86_REG_MXCSR, case.mxcsr)

        if any(
            (
                (table.body_va != case.va or table.body_size != case.size)
                and not isinstance(table, GuardedJumpTable)
            )
            or not 1 <= len(table.targets) <= 256
            or table.register not in {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"}
            or not self._base
            <= table.body_va
            < table.body_va + table.body_size
            <= self._base + self._span
            or any(
                not self._base <= base < base + len(data) <= self._base + self._span
                for base, data in table_regions(table)
            )
            for table in static_tables
        ):
            return self._faulted("STATIC-JUMP-TABLE-CONTEXT")

        # A stub stops emulation, so execution proceeds in segments: one emu_start per
        # stubbed call plus one for the remainder. The budget is tracked across all of
        # them by instruction count, which also makes COUNT-LIMIT exact rather than
        # inferred from where emu_start happened to stop.
        eip = case.va
        while True:
            self._pending_site = None
            remaining = self._max_insns - self._insns
            if remaining <= 0:
                self._dirty |= self._touched
                return self._faulted("COUNT-LIMIT")
            try:
                self._uc.emu_start(eip, self._sentinel, count=remaining)
            except self._uc_mod.UcError as error:
                self._dirty |= self._touched
                if self._code_guard is not None:
                    self._code_guard.check(error)
                    self.code_safety_observation = {
                        "certificate_sha256": code_safety.document()["sha256"],
                        "case_index": case.index,
                        "outcome": "faulted",
                    }
                return self._faulted(self._fault_name(error))

            if self._code_guard is not None:
                self._dirty |= self._touched
                self._code_guard.check()
            if self._contract_fault:
                self._dirty |= self._touched
                return self._faulted(self._contract_fault)
            site = self._pending_site
            if site is None:
                break
            desync = self._apply_stub(site)
            if desync is not None:
                self._dirty |= self._touched
                return self._faulted(desync)
            eip = site.return_va

        self._dirty |= self._touched
        if self._uc.reg_read(self._x86.UC_X86_REG_EIP) != self._sentinel:
            # Either the instruction budget ran out or the function left through an
            # exit this harness does not model. Not a verdict.
            return self._faulted("COUNT-LIMIT")

        if self._code_guard is not None:
            self._code_guard.finish(
                self._uc.reg_read(self._x86.UC_X86_REG_EIP),
                self._uc.reg_read(self._x86.UC_X86_REG_ESP),
            )
            self.code_safety_observation = {
                "certificate_sha256": code_safety.document()["sha256"],
                "case_index": case.index,
                "outcome": "returned",
            }
        final = tuple(self._uc.reg_read(reg) for reg in self._regs)
        flags = self._uc.reg_read(self._x86.UC_X86_REG_EFLAGS)
        return ExecResult(
            regs=final,
            x87_state=self._read_raw_x87() if self._raw_x87_requested else None,
            fp=self._read_fp() if not self._raw_x87_requested else None,
            fp_control=self._uc.reg_read(self._x86.UC_X86_REG_FPCW)
            if not self._raw_x87_requested
            else None,
            fp_status=self._uc.reg_read(self._x86.UC_X86_REG_FPSW)
            if not self._raw_x87_requested
            else None,
            fp_status_mask=0xFFFF if not self._raw_x87_requested else None,
            fp_ext=self._read_fp_ext() if not self._raw_x87_requested else None,
            xmm=tuple(self._uc.reg_read(getattr(self._x86, f"UC_X86_REG_XMM{i}")) for i in range(8))
            if case.xmm is not None
            else None,
            mxcsr=self._uc.reg_read(self._x86.UC_X86_REG_MXCSR) if case.xmm is not None else None,
            writes=self._collect_writes(patched),
            flags=flags,
            reach=self._reach(),
        )

    def _load_fp(self, case: Case) -> None:
        """Reset the x87 unit and push the case's entry stack (top first).

        Done for every case, not only x87 ones, so a previous case's stack or control word
        can never leak into this one.
        """
        x86 = self._x86
        if case.x87_state is not None:
            state = case.x87_state
            for index, raw in enumerate(state.physical):
                self._uc.reg_write(x86.UC_X86_REG_FP0 + index, (raw & ((1 << 64) - 1), raw >> 64))
            self._uc.reg_write(x86.UC_X86_REG_FPCW, state.control)
            self._uc.reg_write(x86.UC_X86_REG_FPSW, state.status)
            self._uc.reg_write(x86.UC_X86_REG_FPTAG, state.tags)
            if self._read_raw_x87() != state:
                raise ValueError("Unicorn cannot exactly restore required raw x87 state")
            return
        depth = len(case.fp_stack)
        self._uc.reg_write(x86.UC_X86_REG_FPCW, case.fp_control or DEFAULT_CONTROL)
        self._uc.reg_write(x86.UC_X86_REG_FPSW, ((8 - depth) & 7) << 11)
        # Full tag word: 00 valid for each occupied physical register, 11 empty otherwise.
        tags = 0xFFFF
        for position in range(depth):
            physical = (8 - depth + position) & 7
            tags &= ~(3 << (2 * physical))
            mantissa, exponent = double_to_extended(case.fp_stack[position])
            self._uc.reg_write(x86.UC_X86_REG_FP0 + physical, (mantissa, exponent))
        self._uc.reg_write(x86.UC_X86_REG_FPTAG, tags)

    def _read_raw_x87(self) -> X87State:
        x86 = self._x86
        slots = tuple(self._uc.reg_read(x86.UC_X86_REG_FP0 + index) for index in range(8))
        return X87State(
            tuple(low | (high << 64) for low, high in slots),
            self._uc.reg_read(x86.UC_X86_REG_FPTAG),
            self._uc.reg_read(x86.UC_X86_REG_FPCW),
            self._uc.reg_read(x86.UC_X86_REG_FPSW),
        )

    def _read_fp_ext(self) -> tuple[tuple[int, int], ...]:
        """The exit x87 stack, top first, as exact `(mantissa, sign_exponent)` registers."""
        x86 = self._x86
        top = (self._uc.reg_read(x86.UC_X86_REG_FPSW) >> 11) & 7
        depth = (8 - top) & 7
        return tuple(
            tuple(self._uc.reg_read(x86.UC_X86_REG_FP0 + ((top + position) & 7)))  # type: ignore[misc]
            for position in range(depth)
        )

    def _read_fp(self) -> tuple[int, ...]:
        """The exit x87 stack, top first, as double bit patterns."""
        x86 = self._x86
        top = (self._uc.reg_read(x86.UC_X86_REG_FPSW) >> 11) & 7
        depth = (8 - top) & 7
        out: list[int] = []
        for position in range(depth):
            mantissa, exponent = self._uc.reg_read(x86.UC_X86_REG_FP0 + ((top + position) & 7))
            out.append(extended_to_double_bits(mantissa, exponent))
        return tuple(out)

    def _reach(self) -> Reachability:
        return Reachability(
            insns=self._insns,
            covered_vas=frozenset(self._covered),
            stub_applied=self._stub_applied,
            passthrough_applied=self._passthrough_applied,
            loads=tuple(self._loads),
        )

    def _faulted(self, name: str) -> ExecResult:
        """A fault still reports how far it got, so a fault-heavy function is visible.

        A function whose cases all fault after two instructions has not been tested
        either, and that is worth knowing even though no verdict is possible.
        """
        errors = []
        observed = {}
        fault_addr = (
            getattr(self, "_last_invalid_addr", None)
            if name.startswith("UC_ERR_") and ("UNMAPPED" in name or "PROT" in name)
            else None
        )
        self._last_invalid_addr = None
        readers = {
            "regs": lambda: tuple(self._uc.reg_read(reg) for reg in self._regs),
            "flags": lambda: self._uc.reg_read(self._x86.UC_X86_REG_EFLAGS),
            "backend_ip": lambda: self._uc.reg_read(self._x86.UC_X86_REG_EIP),
            "changed_bytes": lambda: tuple(
                sorted(self._collect_writes(self._fault_patched).items())
            ),
        }
        for field, reader in readers.items():
            try:
                value = reader()
                # Validate widths/shape before publishing any optional snapshot field.
                FaultObservation("oracle-stopped-backend", name, **{field: value})
                observed[field] = value
            except Exception as error:
                errors.append(f"{field}: {type(error).__name__}: {error}")
        observation = FaultObservation(
            "oracle-stopped-backend", name, **observed, capture_errors=tuple(errors)
        )
        return ExecResult(
            fault=name,
            fault_addr=fault_addr,
            fault_observation=observation,
            reach=self._reach(),
            x87_state=self._read_raw_x87() if getattr(self, "_raw_x87_requested", False) else None,
        )

    def _collect_writes(self, patched: dict[int, int]) -> dict[int, int]:
        """Bytes whose value actually changed, across the pages the guest wrote to."""
        writes: dict[int, int] = {}
        for page in sorted(self._touched):
            after = bytes(self._uc.mem_read(page, PAGE))
            offset = page - self._base
            before = self._image[offset : offset + PAGE]
            for i in range(PAGE):
                addr = page + i
                original = patched.get(addr, before[i])
                if after[i] != original:
                    writes[addr] = after[i]
        return writes

    def _fault_name(self, error: Exception) -> str:
        errno = getattr(error, "errno", None)
        if errno is None:
            return f"UC:{error}"
        return self._errnames.get(errno, f"UC_ERR_{errno}")
