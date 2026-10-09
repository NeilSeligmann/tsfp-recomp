"""Explicit ABI bindings and bounded original-byte integer handoff runner.

Opaque modeled FP/MMX/XMM state survives integer continuations. Instructions
that could access it, segment state or other unmodeled state stop UNEXECUTED;
this is not a general production CPU emulator or a fabricated guest fault.
"""

from __future__ import annotations

import ctypes as C
import re
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X

from tools.name_candidates import Image

from .generate import IMAGE_SHA, ROOT, digest


class Machine(C.Structure):
    _fields_ = [
        ("r", C.c_uint32 * 8),
        ("eip", C.c_uint32),
        ("eflags", C.c_uint32),
        ("fs_base", C.c_uint32),
        ("seh_ebp", C.c_uint32),
        ("flag_bridge", C.c_uint32),
        ("flag_bridge_mask", C.c_uint32),
        ("mmx", C.c_uint64 * 8),
        ("xmm", (C.c_uint32 * 4) * 8),
        ("x87", C.c_double * 8),
        ("x87_top", C.c_uint32),
        ("x87_control", C.c_uint32),
        ("x87_compare", C.c_uint32),
        ("x87_condition", C.c_uint32),
    ]


class Frame(C.Structure):
    _fields_ = [
        ("owner", C.c_uint64),
        ("generation", C.c_uint64),
        ("return_slot", C.c_uint32),
        ("expected_resume", C.c_uint32),
        ("active", C.c_uint32),
    ]


class Profile(C.Structure):
    _fields_ = [
        ("abi", C.c_uint32),
        ("layout", C.c_uint32),
        ("machine_size", C.c_uint32),
        ("frame_size", C.c_uint32),
        ("context_size", C.c_uint32),
        ("view_abi", C.c_uint32),
        ("page_size", C.c_uint32),
        ("view_size", C.c_uint32),
        ("identity", C.c_char * 32),
        ("image_sha256", C.c_char * 65),
        ("source_sha256", C.c_char * 65),
        ("build_sha256", C.c_char * 65),
    ]


VIEW_ABI = 2
STATE_INTEGER = 1
STATE_X87 = 2
STATE_MMX = 4
STATE_SIMD = 8
STATE_SEGMENT = 16
STATE_DEBUG = 32
INTEGER_EFLAGS = 0xED7


class Page(C.Structure):
    _fields_ = [
        ("address", C.c_uint32),
        ("permissions", C.c_uint32),
        ("generation", C.c_uint64),
        ("code_generation", C.c_uint64),
    ]


class View(C.Structure):
    _fields_ = [
        ("abi", C.c_uint32),
        ("size", C.c_uint32),
        ("required_state", C.c_uint32),
        ("reserved", C.c_uint32),
        ("generation", C.c_uint64),
        ("pages", C.POINTER(Page)),
        ("page_count", C.c_size_t),
        ("image_sha256", C.c_char * 65),
    ]


def mapping_view(pages: dict[int, bytes], protections: dict[int, int] | None = None) -> View:
    """Explicit fixture view; retain its owned page storage for the live view.

    Generation1 asserts caller-qualified original identity; native dispatch and
    original fallback still compare actual instruction bytes before execution.
    This helper does not inspect or confer host mapping lifetime/permissions.
    """
    storage = (Page * len(pages))(
        *(Page(address, (protections or {}).get(address, 7), 1, 1) for address in sorted(pages))
    )
    view = View(
        VIEW_ABI, C.sizeof(View), STATE_INTEGER, 0, 1, storage, len(storage), IMAGE_SHA.encode()
    )
    view._storage = storage
    return view


class Context(C.Structure):
    _fields_ = [
        ("machine", Machine),
        ("frames", C.POINTER(Frame)),
        ("capacity", C.c_size_t),
        ("depth", C.c_size_t),
        ("memory_offset", C.c_size_t),
        ("owner", C.c_uint64),
        ("generation", C.c_uint64),
        ("completed", C.c_uint64),
        ("live", C.c_uint32),
        ("stop_va", C.c_uint32),
        ("fast_returns", C.c_uint32),
        ("redirected_returns", C.c_uint32),
        ("view", C.POINTER(View)),
        ("view_generation", C.c_uint64),
        ("handoff_reason", C.c_uint32),
    ]


REGISTERS = (
    X.UC_X86_REG_EAX,
    X.UC_X86_REG_ECX,
    X.UC_X86_REG_EDX,
    X.UC_X86_REG_EBX,
    X.UC_X86_REG_ESP,
    X.UC_X86_REG_EBP,
    X.UC_X86_REG_ESI,
    X.UC_X86_REG_EDI,
)
# Conservatively bounded to integer continuations with modeled operands.
INTEGER = {
    "mov",
    "lea",
    "push",
    "pop",
    "ret",
    "call",
    "jmp",
    "nop",
    "int3",
    "add",
    "sub",
    "and",
    "or",
    "xor",
    "cmp",
    "test",
    "inc",
    "dec",
    "jz",
    "jnz",
    "je",
    "jne",
    "ja",
    "jae",
    "jb",
    "jbe",
    "jg",
    "jge",
    "jl",
    "jle",
}


@dataclass
class OriginalResult:
    kind: str
    machine: Machine
    pages: dict[int, bytes]
    trace: list[int]
    writes: list[tuple[int, int, int]]
    error: int | None = None


def original_handoff(
    machine: Machine,
    pages: dict[int, bytes],
    stop: int,
    budget: int = 100,
    protections: dict[int, int] | None = None,
    xbe: Path = ROOT / "build/default.xbe",
    *,
    view: View | None = None,
    view_generation: int | None = None,
) -> OriginalResult:
    """Consume an unexecuted state; execute genuine supplied original bytes.

    RAM pages are fixture snapshots. An explicit version2 view and bound
    generation qualify the same guest mappings used by native dispatch; missing
    qualification yields no execution/verdict. NX/unmapped faults come from real
    Unicorn fetches, never metadata-generated exceptions. Each executed
    instruction must match the pinned original image. Nonoriginal/unsupported instruction stops
    before execution; actual read/fetch/trap faults are Unicorn outcomes.
    """
    if digest(xbe) != IMAGE_SHA:
        raise ValueError("handoff original image identity mismatch")
    if not isinstance(budget, int) or not 0 <= budget <= 0xFFFFFFFF:
        raise ValueError("handoff budget must be a uint32 instruction count")
    output = Machine.from_buffer_copy(bytes(machine))
    for address, data in pages.items():
        if address & 4095 or len(data) != 4096:
            raise ValueError("handoff requires complete aligned pages")

    def unexecuted(kind: str) -> OriginalResult:
        return OriginalResult(kind, output, dict(pages), [], [])

    if budget == 0 or machine.eip == stop:
        return unexecuted("STOP" if machine.eip == stop else "BUDGET")
    if (
        view is None
        or view.abi != VIEW_ABI
        or view.size != C.sizeof(View)
        or not view.generation
        or view.generation != view_generation
        or view.reserved
        or view.image_sha256 != IMAGE_SHA.encode()
        or not view.pages
        or not 0 < view.page_count <= 1048576
    ):
        return unexecuted("UNEXECUTED-UNQUALIFIED-VIEW")
    mapped = {}
    previous = -1
    for index in range(view.page_count):
        page = view.pages[index]
        address = int(page.address)
        if (
            address & 4095
            or address <= previous
            or page.permissions & ~7
            or not page.generation
            or address not in pages
        ):
            return unexecuted("UNEXECUTED-UNQUALIFIED-VIEW")
        previous = address
        mapped[address] = page
    if protections is not None and any(
        address not in mapped or mapped[address].permissions != permission
        for address, permission in protections.items()
    ):
        raise ValueError("handoff protections disagree with the explicit view")
    # TF is imported for a genuine Unicorn single-step exception. Other debug,
    # privileged/unnormalized flags and unimportable required state are no-verdict.
    if (
        view.required_state != STATE_INTEGER
        or not machine.eflags & 2
        or machine.eflags & ~(INTEGER_EFLAGS | 0x100)
    ):
        return unexecuted("UNEXECUTED-UNSUPPORTED-STATE")
    image = Image(xbe)
    uc, decoder = Uc(UC_ARCH_X86, UC_MODE_32), Cs(CS_ARCH_X86, CS_MODE_32)
    for address, page in mapped.items():
        uc.mem_map(address, 4096)
        uc.mem_write(address, pages[address])
        uc.mem_protect(address, 4096, page.permissions)
    for register, value in zip(REGISTERS, machine.r, strict=True):
        uc.reg_write(register, int(value))
    uc.reg_write(X.UC_X86_REG_EFLAGS, machine.eflags)
    trace, writes = [], []
    unsupported = False
    nonoriginal = False
    stale = False

    def inspect(u: Uc, address: int, size: int, data: object) -> None:
        nonlocal unsupported, nonoriginal, stale
        for offset in range(size):
            page = mapped.get((address + offset) & ~4095)
            if page is None or page.code_generation != page.generation:
                stale = True
                u.emu_stop()
                return
        body = bytes(u.mem_read(address, size))
        if image.read(address, size) != body:
            nonoriginal = True
            u.emu_stop()
            return
        insn = next(decoder.disasm(body, address), None)
        if (
            insn is None
            or insn.mnemonic not in INTEGER
            or any(word in insn.op_str for word in (":", "xmm", "mm", "st("))
            or re.search(r"\b(?:[cdefgs]s|cr\d+|dr\d+)\b", insn.op_str)
        ):
            unsupported = True
            u.emu_stop()
            return
        trace.append(address)

    from unicorn import UC_HOOK_MEM_WRITE

    uc.hook_add(UC_HOOK_CODE, inspect)
    uc.hook_add(
        UC_HOOK_MEM_WRITE,
        lambda u, access, address, size, value, data: writes.append((address, size, value)),
    )
    kind, error = "STOP", None
    try:
        uc.emu_start(machine.eip, stop, count=budget)
        if stale:
            kind = "UNEXECUTED-STALE-CODE"
        elif nonoriginal:
            kind = "UNEXECUTED-NONORIGINAL-CODE"
        elif unsupported:
            kind = "UNEXECUTED-UNSUPPORTED"
        elif uc.reg_read(X.UC_X86_REG_EIP) != stop:
            kind = "BUDGET"
    except UcError as fault:
        kind, error = "ORIGINAL-FAULT", fault.errno
    for index, register in enumerate(REGISTERS):
        output.r[index] = uc.reg_read(register)
    output.eip, output.eflags = uc.reg_read(X.UC_X86_REG_EIP), uc.reg_read(X.UC_X86_REG_EFLAGS)
    # FP/MMX/XMM/segment/bridge shadow fields are unchanged because unsupported
    # operations never execute. This explicitly bounded domain avoids claiming
    # that independent runtime MMX and double x87 model the real shared80-bit file.
    after = {
        address: bytes(uc.mem_read(address, 4096)) if address in mapped else body
        for address, body in pages.items()
    }
    return OriginalResult(kind, output, after, trace, writes, error)
