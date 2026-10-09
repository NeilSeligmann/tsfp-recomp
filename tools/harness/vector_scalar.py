# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620 fp-scalar-v1 runtime: SSE site table, NaN-pair tripwire, pointee overlay, class coverage.

One `ScalarFpTracker` is attached to the `UnicornOracle` (`oracle.scalar_fp`). The oracle's
code hook calls `before()` at every SSE site of the live closure. The tracker

* records the effective address of every float memory read (trace pass of the pointee overlay),
* classifies both operands of every admitted arithmetic site (per-site coverage gate),
* trips the NaN-pair wire: add/sub/mul/div/comiss/ucomiss with two NaN operands of different
  bit patterns stop the oracle with fault NAN-PAIR, a counted non-verdict (Unicorn and silicon
  disagree on which NaN is returned, MEASURED in tests/test_vector_scalar_native_matrix.py).

`prepare_case` runs the trace pass: it patches every dword an SSE site read with a seeded float
class value in the case itself, so the oracle and the subject start from identical memory.
Legacy vector mode never constructs a tracker.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, replace
from typing import Any

from .model import FP_SCALAR_MODE, FP_SCALAR_MXCSR, FP_SCALAR_STATUS_MASK, Case
from .vector import VECTOR_SCALAR_FP, LaneSampler, is_nan_bits

#: most pointee-overlay passes per case (each pass can expose reads behind newly seeded data)
MAX_OVERLAY_PASSES = 4
#: at most this fraction of a root's cases may be NaN-pair non-verdicts
EXCLUSION_CAP = 0.5
#: arithmetic sites where two different NaN operands make the case a non-verdict
TRIPWIRE = frozenset({"addss", "subss", "mulss", "divss", "comiss", "ucomiss"})
#: legacy float loads whose memory source is float-shaped (overlay only, no operand classes)
FLOAT_LOADS = frozenset({"movss", "movups", "movlps", "movhps"})
FLOAT_CLASSES = ("zero", "denormal", "inf", "negative", "tie")
#: single float operand (no tie partner): cvttss2si
CVTT_CLASSES = ("zero", "denormal", "inf", "negative", "nan")
INT_CLASSES = ("zero", "negative", "large")


@dataclass(frozen=True)
class Mem:
    base: str | None
    index: str | None
    scale: int
    disp: int


@dataclass(frozen=True)
class Site:
    va: int
    mnemonic: str
    #: "float2" two float operands, "cvtsi2ss" integer source, "load" float read only
    kind: str
    dest: int | None
    src_xmm: int | None
    src_gpr: str | None
    mem: Mem | None
    read_size: int


def classify_float(bits: int) -> set[str]:
    """Operand class labels of a float32 bit pattern."""
    exponent, fraction = (bits >> 23) & 0xFF, bits & 0x7FFFFF
    labels: set[str] = set()
    if exponent == 0xFF:
        labels.add("nan" if fraction else "inf")
    elif exponent == 0:
        labels.add("denormal" if fraction else "zero")
    else:
        labels.add("normal")
    if bits >> 31 and "nan" not in labels:
        labels.add("negative")
    return labels


def classify_int(value: int) -> set[str]:
    signed = value - (1 << 32) if value >> 31 else value
    labels = {"zero" if signed == 0 else "small"}
    if signed < 0:
        labels.add("negative")
    if abs(signed) >= 1 << 24:
        labels.add("large")
    return labels


def build_sites(instructions: list[Any]) -> dict[int, Site]:
    """Site table of the SSE instructions that read floats, from decoded capstone instructions."""
    from capstone.x86_const import X86_OP_MEM, X86_OP_REG

    sites: dict[int, Site] = {}
    for insn in instructions:
        mnemonic = insn.mnemonic.lower()
        if mnemonic == "movaps":
            continue  # register copy: no arithmetic, no memory read, no operand classes
        if mnemonic not in VECTOR_SCALAR_FP and mnemonic not in FLOAT_LOADS:
            continue
        if len(insn.operands) != 2:
            continue
        left, right = insn.operands
        mem = None
        read_size = 0
        if right.type == X86_OP_MEM:
            m = right.mem
            mem = Mem(
                insn.reg_name(m.base) if m.base else None,
                insn.reg_name(m.index) if m.index else None,
                m.scale,
                m.disp,
            )
            read_size = right.size
        dest = None
        if mnemonic == "cvttss2si":
            pass  # the destination is a GPR
        elif left.type == X86_OP_REG and insn.reg_name(left.reg).startswith("xmm"):
            dest = int(insn.reg_name(left.reg)[3:])
        elif mnemonic in FLOAT_LOADS:
            continue  # a store: nothing is read from memory
        src_xmm = src_gpr = None
        if right.type == X86_OP_REG:
            name = insn.reg_name(right.reg)
            if name.startswith("xmm"):
                src_xmm = int(name[3:])
            else:
                src_gpr = name
        if mnemonic in FLOAT_LOADS:
            if mem is None:
                continue
            kind = "load"
        elif mnemonic == "cvtsi2ss":
            kind = "cvtsi2ss"
        elif mnemonic == "cvttss2si":
            kind = "cvttss2si"
        else:
            kind = "float2"
        sites[insn.address] = Site(
            insn.address, mnemonic, kind, dest, src_xmm, src_gpr, mem, read_size
        )
    return sites


def harvest_constants(sites: dict[int, Site], read_dword: Any) -> tuple[int, ...]:
    """Float bit patterns of absolute-address (`.rdata` style) m32 operands of float sites."""
    seen: dict[int, None] = {}
    for site in sites.values():
        if site.kind == "cvtsi2ss" or site.mem is None or site.read_size != 4:
            continue
        if site.mem.base is None and site.mem.index is None:
            raw = read_dword(site.mem.disp & 0xFFFFFFFF)
            if raw is not None:
                seen.setdefault(raw, None)
    return tuple(sorted(seen))


class ScalarFpTracker:
    """Per-root oracle observer. See the module docstring."""

    def __init__(self, sites: dict[int, Site], constants: tuple[int, ...] = ()) -> None:
        self.sites = sites
        self.constants = constants
        # per-run state
        self.run_eas: list[tuple[int, int]] = []
        self.run_hits: list[tuple[int, int, int]] = []
        self.nan_pair = False
        self._x86: Any = None
        # aggregate state
        self.cases = 0
        self.excluded = 0
        self.overlay_cases = 0
        self.overlay_passes = 0
        self.overlay_dwords = 0
        self.overlay_unconverged = 0
        self.coverage: dict[int, dict[str, int]] = {}
        #: build-flag record from tools.replace.fp_build and the host MXCSR probe (cli sets both)
        self.build: dict[str, Any] | None = None
        self.host: Any = None
        self.host_info: dict[str, str] | None = None
        #: T1624: identity (result_file, sha256) of the tracked native-vs-Unicorn matrix result
        self.matrix: dict[str, str] | None = None
        self.hits: dict[int, int] = {}

    # ---- oracle callbacks -------------------------------------------------------------

    def begin_run(self) -> None:
        self.run_eas = []
        self.run_hits = []
        self.nan_pair = False

    def _reg(self, uc: Any, name: str) -> int:
        if self._x86 is None:
            from unicorn import x86_const

            self._x86 = x86_const
        return uc.reg_read(getattr(self._x86, f"UC_X86_REG_{name.upper()}"))

    def _xmm_lane0(self, uc: Any, index: int) -> int:
        return self._reg(uc, f"xmm{index}") & 0xFFFFFFFF

    def effective_address(self, uc: Any, mem: Mem) -> int:
        address = mem.disp
        if mem.base:
            address += self._reg(uc, mem.base)
        if mem.index:
            address += self._reg(uc, mem.index) * mem.scale
        return address & 0xFFFFFFFF

    def before(self, uc: Any, address: int) -> bool:
        """Called by the oracle code hook. True asks the oracle to stop (NaN-pair tripwire)."""
        site = self.sites.get(address)
        if site is None:
            return False
        ea = None
        if site.mem is not None:
            ea = self.effective_address(uc, site.mem)
            if site.kind != "cvtsi2ss":
                self.run_eas.append((ea, site.read_size))
        if site.kind == "load":
            return False
        try:
            if site.kind == "cvtsi2ss":
                if ea is not None:
                    source = int.from_bytes(uc.mem_read(ea, 4), "little")
                else:
                    source = self._reg(uc, site.src_gpr) & 0xFFFFFFFF
                self.run_hits.append((address, source, 0))
                return False
            if site.kind == "cvttss2si":
                # one float operand: no NaN pair is possible, the pair slot is a dummy zero
                if ea is not None:
                    source_bits = int.from_bytes(uc.mem_read(ea, 4), "little")
                else:
                    source_bits = self._xmm_lane0(uc, site.src_xmm)
                self.run_hits.append((address, source_bits, 0))
                return False
            dest_bits = self._xmm_lane0(uc, site.dest)
            if ea is not None:
                source_bits = int.from_bytes(uc.mem_read(ea, 4), "little")
            else:
                source_bits = self._xmm_lane0(uc, site.src_xmm)
        except Exception:  # an unmapped operand faults in the instruction itself
            return False
        self.run_hits.append((address, dest_bits, source_bits))
        if (
            site.mnemonic in TRIPWIRE
            and is_nan_bits(dest_bits)
            and is_nan_bits(source_bits)
            and dest_bits != source_bits
        ):
            self.nan_pair = True
            return True
        return False

    # ---- aggregation ------------------------------------------------------------------

    def commit_case(self, faulted: bool) -> None:
        """Fold the LAST run (the verdict run) into the root totals."""
        self.cases += 1
        if self.nan_pair:
            self.excluded += 1
        if faulted or self.nan_pair:
            return
        for va, first, second in self.run_hits:
            site = self.sites[va]
            counts = self.coverage.setdefault(va, {})
            self.hits[va] = self.hits.get(va, 0) + 1
            if site.kind == "cvtsi2ss":
                labels = classify_int(first)
            elif site.kind == "cvttss2si":
                labels = classify_float(first)
            else:
                labels = classify_float(first) | classify_float(second)
                if first == second or (not (first | second) & 0x7FFFFFFF):
                    labels.add("tie")
            for label in labels:
                counts[label] = counts.get(label, 0) + 1

    def document(self) -> dict[str, Any]:
        """Receipt block. `gate.passed` is the per-site operand-class and exclusion-cap gate."""
        failures: list[str] = []
        rows = []
        unreached = []
        for va in sorted(self.sites):
            site = self.sites[va]
            if site.kind == "load":
                continue
            if va not in self.hits:
                unreached.append(f"0x{va:08x}")
                continue
            counts = self.coverage[va]
            required = list(
                INT_CLASSES
                if site.kind == "cvtsi2ss"
                else CVTT_CLASSES
                if site.kind == "cvttss2si"
                else FLOAT_CLASSES
            )
            if site.mnemonic in ("comiss", "ucomiss"):
                required.append("nan")
            missing = [label for label in required if not counts.get(label)]
            rows.append(
                {
                    "va": f"0x{va:08x}",
                    "mnemonic": site.mnemonic,
                    "hits": self.hits[va],
                    "classes": dict(sorted(counts.items())),
                    "missing": missing,
                }
            )
            if missing:
                failures.append(f"site 0x{va:08x} {site.mnemonic} never saw: {','.join(missing)}")
        ratio = self.excluded / self.cases if self.cases else 0.0
        if ratio > EXCLUSION_CAP:
            failures.append(
                f"nan-pair exclusions {self.excluded}/{self.cases} exceed {EXCLUSION_CAP}"
            )
        if not rows:
            failures.append("no admitted arithmetic site was reached by a verdict case")
        return {
            "mode": FP_SCALAR_MODE,
            "mxcsr_control_word": f"0x{FP_SCALAR_MXCSR:04x}",
            "mxcsr_status_mask": f"0x{FP_SCALAR_STATUS_MASK:02x}",
            "sites_total": sum(1 for s in self.sites.values() if s.kind != "load"),
            "sites_unreached": unreached,
            "cases": self.cases,
            "nan_pair_excluded": self.excluded,
            "nan_pair_ratio": round(ratio, 6),
            "nan_pair_cap": EXCLUSION_CAP,
            "overlay": {
                "cases_with_overlay": self.overlay_cases,
                "passes": self.overlay_passes,
                "dwords_patched": self.overlay_dwords,
                "not_converged": self.overlay_unconverged,
                "max_passes": MAX_OVERLAY_PASSES,
            },
            "build": self.build,
            "host": self.host_info,
            "matrix": self.matrix,
            "constants": [f"0x{value:08x}" for value in self.constants],
            "site_coverage": rows,
            "gate": {"passed": not failures, "failures": failures},
        }


def prepare_case(
    oracle: Any,
    tracker: ScalarFpTracker,
    case: Case,
    plan: Any,
    *,
    window: tuple[int, int],
) -> Case:
    """Trace-driven pointee overlay: patch every dword an SSE site read with a float-class value.

    Runs the oracle (trace only, nothing is recorded) until the set of float reads stops growing
    or MAX_OVERLAY_PASSES. The returned case carries the extra patches for BOTH sides. Values
    come from `LaneSampler` keyed by the address, so they are deterministic.
    """
    sampler = LaneSampler(case, tracker.constants)
    for value in case.xmm or ():
        for lane in range(4):
            sampler.history.append((value >> (32 * lane)) & 0xFFFFFFFF)
    patched: dict[int, int] = {}
    current = case
    converged = False
    passes = 0
    for _ in range(MAX_OVERLAY_PASSES):
        passes += 1
        oracle.run(current, plan)
        fresh: list[int] = []
        for ea, size in tracker.run_eas:
            for offset in range(0, max(size, 4), 4):
                address = (ea + offset) & 0xFFFFFFFF
                if (
                    address not in patched
                    and address not in fresh
                    and window[0] <= address
                    and address + 4 <= window[1]
                ):
                    fresh.append(address)
        if not fresh:
            converged = True
            break
        extra = []
        for address in fresh:
            _, bits = sampler.sample(f"ptr:{address:08x}")
            patched[address] = bits
            extra.append((address, struct.pack("<I", bits)))
        current = replace(current, patches=current.patches + tuple(extra))
    tracker.overlay_passes += passes
    if patched:
        tracker.overlay_cases += 1
        tracker.overlay_dwords += len(patched)
    if not converged:
        tracker.overlay_unconverged += 1
    return current
