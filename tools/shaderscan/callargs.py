# SPDX-License-Identifier: GPL-3.0-or-later
"""Follow the argument pushes at a call site and say what each argument IS.

WHY. Whether the title's shader inputs form a closed set depends on the values handed
to `XGAssembleShader`: a pointer to a static string is enumerable offline, a pointer to
a buffer the game fills at run time is not, at least not without asking how it is filled.
A call count cannot say which, so this module reads the pushes.

HOW. For a call at instruction index `i` it walks BACKWARD through straight-line code
collecting `push` instructions, and classifies each pushed value:

    imm          a literal, `push 0x120` or `mov reg, 0x120` then `push reg`
    stack_addr   `lea reg, [esp+k]` / `[ebp-k]`: a stack buffer, i.e. a RUNTIME buffer
    global_load  `mov reg, [disp32]`: a value read from a fixed global at call time
    stack_slot   `[esp+k]` / `[ebp-k]`: a parameter or local, i.e. decided by the caller
    mem_load     any other memory read
    call_result  eax right after a `call`
    computed     the result of arithmetic, or an indexed `lea`
    live_in      a callee-saved register with no writer in this block, so decided earlier
    unresolved   anything else, named in `note`
    missing      fewer than `nargs` pushes were found before the block boundary

WHAT STOPS THE WALK, AND WHY IT IS CONSERVATIVE. A branch target (so the walk never
borrows pushes from a block that merely falls into this one), a call (its own pushes and
its own cleanup are in the way), a jump, a return, or any write to `esp` other than
`push`. An argument the walk cannot reach is reported `missing` rather than guessed. A
reader who sees `missing` knows the count is a lower bound on that class.

WHAT IT CANNOT SEE. A call through a register or a vtable has no direct target and is
not found. `count_indirect_calls` reports how many exist so the denominator is visible.
Arguments built with `sub esp, N` and `mov [esp+k], v` instead of pushes are `missing`.

DECODING IS THE REPO'S RESYNCHRONISING SWEEP (`tools.gen_d3d8_surface.decode_section`)
and not `capstone.disasm` on the whole section, which stops silently at the first
undecodable byte. `.text` has 132 such bytes, so the trap is real here.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass

import capstone
from capstone import x86 as cs_x86

from tools.shaderscan.image import Image

#: 32-bit register -> every name that aliases part of it.
_FAMILIES: dict[str, tuple[str, ...]] = {
    "eax": ("eax", "ax", "al", "ah"),
    "ebx": ("ebx", "bx", "bl", "bh"),
    "ecx": ("ecx", "cx", "cl", "ch"),
    "edx": ("edx", "dx", "dl", "dh"),
    "esi": ("esi", "si"),
    "edi": ("edi", "di"),
    "ebp": ("ebp", "bp"),
    "esp": ("esp", "sp"),
}
FAMILY_OF: dict[str, str] = {alias: full for full, names in _FAMILIES.items() for alias in names}

#: Registers a callee may destroy, so a value held in one does not survive a `call`.
VOLATILE = frozenset({"eax", "ecx", "edx"})

#: Backward-scan budget. Argument setup is local, and a long scan only finds an
#: unrelated writer of the same register.
SCAN_LIMIT = 64

#: Depth of `mov reg, reg` chains followed before giving up.
MOVE_CHAIN_LIMIT = 4

KIND_IMM = "imm"
KIND_STACK_ADDR = "stack_addr"
KIND_GLOBAL_LOAD = "global_load"
KIND_STACK_SLOT = "stack_slot"
KIND_MEM_LOAD = "mem_load"
KIND_CALL_RESULT = "call_result"
KIND_COMPUTED = "computed"
KIND_LIVE_IN = "live_in"
KIND_UNRESOLVED = "unresolved"
KIND_MISSING = "missing"

_CONTROL_MNEMONICS = frozenset({"ret", "retn", "retf", "jmp", "ljmp", "int3", "hlt", "ud2"})

_ARITHMETIC = frozenset(
    {"add", "sub", "and", "or", "xor", "shl", "shr", "sar", "inc", "dec", "imul", "neg", "not"}
)


@dataclass(frozen=True)
class ArgValue:
    """What one stack argument turned out to be."""

    kind: str
    #: The literal for `imm`, the absolute address for `global_load`, else None.
    value: int | None = None
    note: str = ""


@dataclass(frozen=True)
class Site:
    """One transfer to a target, with its classified arguments (argument 1 first)."""

    va: int
    target: int
    #: "call" for `call rel32`, "jmp" for a tail `jmp rel32`.
    kind: str
    args: tuple[ArgValue, ...]
    #: When the transfer is a branch target reached over several paths, the address of the
    #: instruction on THIS path whose pushes these arguments came from. None for an
    #: ordinary site. One call instruction then yields one `Site` per path.
    path: int | None = None


def _family(name: str) -> str:
    return FAMILY_OF.get(name, name)


def branch_leaders(insns: Sequence[capstone.CsInsn]) -> frozenset[int]:
    """Addresses some direct branch or call transfers to, i.e. basic-block starts."""
    leaders: set[int] = set()
    for insn in insns:
        if capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
            continue
        ops = insn.operands
        if len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
            leaders.add(ops[0].imm & 0xFFFFFFFF)
    return frozenset(leaders)


def _writes_family(insn: capstone.CsInsn, family: str) -> bool:
    _, written = insn.regs_access()
    return any(_family(insn.reg_name(reg)) == family for reg in written)


def _is_stack_relative(insn: capstone.CsInsn, operand: cs_x86.X86Op) -> bool:
    base = insn.reg_name(operand.mem.base) if operand.mem.base else ""
    return base in ("esp", "ebp")


def _classify_source(
    insns: Sequence[capstone.CsInsn],
    index: int,
    leaders: frozenset[int],
    depth: int,
) -> ArgValue:
    """Classify the value the instruction at `index` leaves in its destination."""
    insn = insns[index]
    mnemonic = insn.mnemonic
    ops = insn.operands

    if mnemonic == "mov" and len(ops) == 2 and ops[0].size == 4:
        source = ops[1]
        if source.type == cs_x86.X86_OP_IMM:
            return ArgValue(KIND_IMM, source.imm & 0xFFFFFFFF)
        if source.type == cs_x86.X86_OP_REG:
            if depth >= MOVE_CHAIN_LIMIT:
                return ArgValue(KIND_UNRESOLVED, note="mov chain too deep")
            family = _family(insn.reg_name(source.reg))
            return _resolve_register(insns, index, family, leaders, depth + 1)
        if source.type == cs_x86.X86_OP_MEM:
            return _classify_memory(insn, source)
    if (
        mnemonic in ("xor", "sub")
        and len(ops) == 2
        and ops[0].type == ops[1].type == cs_x86.X86_OP_REG
        and ops[0].reg == ops[1].reg
        and ops[0].size == 4
    ):
        return ArgValue(KIND_IMM, 0)
    if mnemonic == "lea" and len(ops) == 2 and ops[1].type == cs_x86.X86_OP_MEM:
        mem = ops[1].mem
        if mem.base == 0 and mem.index == 0:
            return ArgValue(KIND_IMM, mem.disp & 0xFFFFFFFF, note="lea absolute")
        if _is_stack_relative(insn, ops[1]) and mem.index == 0:
            return ArgValue(KIND_STACK_ADDR, note=insn.op_str)
        return ArgValue(KIND_COMPUTED, note="lea " + insn.op_str)
    if mnemonic in _ARITHMETIC or mnemonic in ("movzx", "movsx"):
        return ArgValue(KIND_COMPUTED, note=mnemonic)
    return ArgValue(KIND_UNRESOLVED, note=f"{mnemonic} {insn.op_str}")


def _classify_memory(insn: capstone.CsInsn, operand: cs_x86.X86Op) -> ArgValue:
    mem = operand.mem
    if mem.base == 0 and mem.index == 0:
        return ArgValue(KIND_GLOBAL_LOAD, mem.disp & 0xFFFFFFFF)
    if _is_stack_relative(insn, operand) and mem.index == 0:
        return ArgValue(KIND_STACK_SLOT, note=insn.op_str)
    return ArgValue(KIND_MEM_LOAD, note=insn.op_str)


def _resolve_register(
    insns: Sequence[capstone.CsInsn],
    index: int,
    family: str,
    leaders: frozenset[int],
    depth: int = 0,
) -> ArgValue:
    """What `family` holds just before `insns[index]` executes, within this block.

    Refuses to scan past a branch target: the writer on the other side of one may not
    be the one that reaches this instruction. That includes `insns[index]` ITSELF being
    one, which a scan that only inspects earlier instructions would never notice.
    """
    if insns[index].address in leaders:
        return ArgValue(KIND_LIVE_IN, note="use is a branch target")
    for step in range(1, SCAN_LIMIT + 1):
        position = index - step
        if position < 0:
            return ArgValue(KIND_LIVE_IN, note="start of decoded range")
        insn = insns[position]
        if insn.mnemonic == "call":
            if family == "eax":
                return ArgValue(KIND_CALL_RESULT)
            if family in VOLATILE:
                return ArgValue(KIND_UNRESOLVED, note="clobbered by call")
        elif _writes_family(insn, family):
            return _classify_source(insns, position, leaders, depth)
        elif insn.mnemonic in _CONTROL_MNEMONICS:
            return ArgValue(KIND_LIVE_IN, note="block boundary")
        if insn.address in leaders:
            return ArgValue(KIND_LIVE_IN, note="branch target reached")
    return ArgValue(KIND_LIVE_IN, note="no writer in scan window")


def _classify_push(
    insns: Sequence[capstone.CsInsn], index: int, leaders: frozenset[int]
) -> ArgValue:
    insn = insns[index]
    operand = insn.operands[0]
    if operand.type == cs_x86.X86_OP_IMM:
        return ArgValue(KIND_IMM, operand.imm & 0xFFFFFFFF)
    if operand.type == cs_x86.X86_OP_REG:
        return _resolve_register(insns, index, _family(insn.reg_name(operand.reg)), leaders)
    if operand.type == cs_x86.X86_OP_MEM:
        return _classify_memory(insn, operand)
    return ArgValue(KIND_UNRESOLVED, note="push " + insn.op_str)


def branch_sources(insns: Sequence[capstone.CsInsn]) -> dict[int, list[int]]:
    """target VA -> indices of the direct `jmp`/`jcc` instructions that transfer to it.

    Calls are left out: a call target is a function entry, not a path into the middle of
    one. These are the paths that merge at a shared `call` tail, which compilers emit for
    a `switch` whose arms all call the same function with a different literal.
    """
    sources: dict[int, list[int]] = {}
    for position, insn in enumerate(insns):
        if insn.mnemonic == "call" or capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
            continue
        ops = insn.operands
        if len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
            sources.setdefault(ops[0].imm & 0xFFFFFFFF, []).append(position)
    return sources


def trace_arguments(
    insns: Sequence[capstone.CsInsn],
    index: int,
    nargs: int,
    leaders: frozenset[int],
    *,
    entry_is_a_path: bool = False,
) -> tuple[ArgValue, ...]:
    """Classify the `nargs` stack arguments of the call at `insns[index]`, argument 1 first.

    The LAST push before the call is argument 1, so the walk collects pushes backward
    and the list order is already the argument order.
    """
    pushes: list[int] = []
    if insns[index].address in leaders and not entry_is_a_path:
        # Several paths may reach the call, each with its own pushes.
        return tuple(ArgValue(KIND_MISSING) for _ in range(nargs))
    for step in range(1, SCAN_LIMIT + 1):
        position = index - step
        if position < 0 or len(pushes) >= nargs:
            break
        insn = insns[position]
        if insn.mnemonic == "push":
            pushes.append(position)
            if insn.address in leaders:
                break
            continue
        if insn.mnemonic == "call" or insn.mnemonic in _CONTROL_MNEMONICS:
            break
        if insn.mnemonic.startswith("j") or _writes_family(insn, "esp"):
            break
        if insn.address in leaders:
            break
    args = [_classify_push(insns, position, leaders) for position in pushes]
    args.extend(ArgValue(KIND_MISSING) for _ in range(nargs - len(args)))
    return tuple(args)


def find_sites(
    insns: Sequence[capstone.CsInsn],
    targets: set[int],
    nargs: int,
    leaders: frozenset[int] | None = None,
    *,
    expand_paths: bool = True,
) -> list[Site]:
    """Every direct `call`/`jmp rel32` to one of `targets`, with its arguments traced.

    A transfer that is itself a branch target has several paths into it, each with its
    own pushes. With `expand_paths` (the default) it yields one `Site` per path: every
    direct `jmp`/`jcc` to it, and the fall-through predecessor, each traced from its own
    instruction. Without it such a site reports its arguments as `missing`. A path that
    is itself reached over a merge is not followed further, so it reports `missing`.
    """
    if leaders is None:
        leaders = branch_leaders(insns)
    sources = branch_sources(insns) if expand_paths else {}
    found: list[Site] = []
    for index, insn in enumerate(insns):
        if insn.mnemonic not in ("call", "jmp"):
            continue
        if capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
            continue
        ops = insn.operands
        if len(ops) != 1 or ops[0].type != cs_x86.X86_OP_IMM:
            continue
        target = ops[0].imm & 0xFFFFFFFF
        if target not in targets:
            continue
        if expand_paths and insn.address in leaders:
            found.extend(_path_sites(insns, index, target, nargs, leaders, sources))
            continue
        args = trace_arguments(insns, index, nargs, leaders)
        found.append(Site(va=insn.address, target=target, kind=insn.mnemonic, args=args))
    return found


def _path_sites(
    insns: Sequence[capstone.CsInsn],
    index: int,
    target: int,
    nargs: int,
    leaders: frozenset[int],
    sources: dict[int, list[int]],
) -> list[Site]:
    """One `Site` per path into the transfer at `insns[index]`."""
    insn = insns[index]
    sites = [
        Site(
            insn.address,
            target,
            insn.mnemonic,
            trace_arguments(insns, source, nargs, leaders),
            path=insns[source].address,
        )
        for source in sources.get(insn.address, [])
    ]
    previous = insns[index - 1] if index > 0 else None
    if previous is not None and previous.mnemonic not in ("jmp", "ret", "retn", "int3"):
        args = trace_arguments(insns, index, nargs, leaders, entry_is_a_path=True)
        sites.append(Site(insn.address, target, insn.mnemonic, args, path=previous.address))
    if not sites:
        missing = tuple(ArgValue(KIND_MISSING) for _ in range(nargs))
        sites.append(Site(insn.address, target, insn.mnemonic, missing))
    return sites


def count_indirect_calls(insns: Sequence[capstone.CsInsn]) -> int:
    """`call`/`jmp` through a register or memory, which no direct scan can attribute."""
    count = 0
    for insn in insns:
        if insn.mnemonic not in ("call", "jmp"):
            continue
        ops = insn.operands
        if len(ops) == 1 and ops[0].type in (cs_x86.X86_OP_REG, cs_x86.X86_OP_MEM):
            count += 1
    return count


def census(sites: Sequence[Site], argument: int) -> Counter[str]:
    """How many sites pass each kind of value as `argument` (1-based)."""
    counts: Counter[str] = Counter()
    for site in sites:
        counts[site.args[argument - 1].kind] += 1
    return counts


def pointer_class(image: Image, value: ArgValue) -> str:
    """Class an `imm` argument that is meant as a pointer, by what it points at.

    `static`   initialised bytes in a read-only section (cannot change at run time)
    `init_rw`  initialised bytes in a WRITABLE section: static at load, but code may
               overwrite it, so it is not proof of a closed set on its own
    `bss`      past the section's initialised bytes: a runtime buffer
    `not_pointer` / `unmapped` otherwise
    """
    if value.kind != KIND_IMM or value.value is None:
        return value.kind
    if not image.looks_like_pointer(value.value):
        return "not_pointer"
    location = image.locate(value.value)
    if location is None:
        return "unmapped"
    if not location.initialised:
        return "bss"
    return "init_rw" if location.writable else "static"
