# SPDX-License-Identifier: GPL-3.0-or-later
"""The flag bridge (T554): flags that cross a call, proven on the image, and the census of the rest.

The lifter models EFLAGS per function, so a function that starts with a jcc on the flags
its caller's callee left has no flag owner and lifts to `RECOMP_FLAGS_UNRESOLVED`, a trap.
`--flag-bridge` (lifter patch 21, `docs/lifter-patches/21-flag-bridge.md`) lets a PROVIDER
publish its modelled CF PF ZF SF OF word and the mask of bits it could answer at every ret,
and a CONSUMER read both once at entry. That is only sound when the image really has the
shape, so this module PROVES the shape per site from the original bytes before a lift is
trusted, and reports the unresolved sites it does NOT cover by class.

``prove`` checks, for every consumer C and provider set P of ``tools/config/flag_bridge.json``:

* C's entry paths read flags only with jcc, setcc or cmovcc before any instruction writes
  them (the three forms the bridge answers), never leave through a call, a ret, an indirect
  jump or a jump out of the function with the flags still live, and never branch back to C.
* Every reference to C is a direct `call` found by the disassembler (xrefs), and no 4 byte
  little-endian word in the image holds C's address, so no indirect path can enter C with
  other flags.
* Every call site of C is directly preceded by a direct call to a provider, with no jump
  into the gap, so a provider's ret is the last thing before C starts.
* Every provider leaves only through `ret` (no tail jump, no indirect jump).

``census`` classifies every `RECOMP_FLAGS_UNRESOLVED` site of a lifted tree: where the flags
should come from (function entry, a call, a join, an instruction no rule models) and whether
anything can reach the function at all.

``replaced`` lists, for every hand-replaced function, which direct call sites read the flags the
original left (a replacement reproduces registers, memory and x87 state but not EFLAGS).

Run: ``python -m tools.lift.flag_bridge prove|census|applied|replaced``
with ``--xbe build/default.xbe --lifted generated/lifted``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

from tools.xbe.parser import parse_xbe

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = Path("tools/config/flag_bridge.json")

_READ = 0
_WRITE = 0
for _name in dir(cs_x86):
    # DF is not an arithmetic flag the bridge carries, and capstone gives the SSE movss and movsd
    # a DF read because they share a name with the string moves.
    if _name.endswith("_DF"):
        continue
    if _name.startswith("X86_EFLAGS_TEST_"):
        _READ |= getattr(cs_x86, _name)
    elif _name.startswith(
        ("X86_EFLAGS_MODIFY_", "X86_EFLAGS_RESET_", "X86_EFLAGS_SET_", "X86_EFLAGS_UNDEFINED_")
    ):
        # PRIOR_ is deliberately not a write: capstone uses it for instructions that keep the
        # old value, which is exactly what a flag bridge relies on.
        _WRITE |= getattr(cs_x86, _name)

#: x87 compares that write EFLAGS (ZF PF CF). Every other x87 instruction leaves them alone.
_FCOMI = frozenset({"fcomi", "fcomip", "fucomi", "fucomip"})


def _effect(insn: capstone.CsInsn) -> tuple[bool, bool]:
    """(reads EFLAGS, writes EFLAGS) of one instruction.

    capstone stores an x87 instruction's FPU status flags in the same field as EFLAGS (a union),
    so an `fld` or `fnstcw` would read as a flag writer. x87 and `wait` are decided by mnemonic.
    """
    if insn.mnemonic in _FCOMI:
        return False, True
    if insn.mnemonic.startswith("f") or insn.mnemonic in ("wait", "emms"):
        return False, False
    return bool(insn.eflags & _READ), bool(insn.eflags & _WRITE)


#: Mnemonic prefixes the bridge can answer (the lifter's `_bridge_condition` table).
_ANSWERED = ("j", "set", "cmov")
_FLAG_NEUTRAL_JUMPS = {"jmp", "ljmp"}
_UNRESOLVED = re.compile(r'RECOMP_FLAGS_UNRESOLVED\(_flags, "(\w+)", 0x([0-9A-Fa-f]+)u\)')
_FUNCTION = re.compile(r"^void (sub_[0-9A-Fa-f]{8})\(void\)")
MAX_WALK = 256


@dataclass(frozen=True)
class Bridge:
    providers: tuple[int, ...]
    consumers: tuple[int, ...]


def load_bridge(path: Path) -> Bridge:
    raw = json.loads(path.read_text(encoding="utf-8"))
    sides = []
    for key in ("providers", "consumers"):
        values = raw.get(key) if isinstance(raw, dict) else None
        if not values:
            raise ValueError(f"{path}: no {key}")
        sides.append(tuple(sorted({int(v, 16) if isinstance(v, str) else int(v) for v in values})))
    return Bridge(providers=sides[0], consumers=sides[1])


@dataclass
class Image:
    """The retail image, the function table and the xref table of one lifted tree."""

    data: bytes
    xbe: object
    functions: list[dict]
    xrefs_to: dict[int, list[dict]]
    _starts: list[int] = field(default_factory=list)
    _cs: capstone.Cs | None = None

    @staticmethod
    def open(xbe_path: Path, lifted: Path) -> Image:
        data = xbe_path.read_bytes()
        functions = json.loads((lifted / "disasm" / "functions.json").read_text(encoding="utf-8"))
        xrefs_to: dict[int, list[dict]] = {}
        for row in json.loads((lifted / "disasm" / "xrefs.json").read_text(encoding="utf-8")):
            xrefs_to.setdefault(int(row["to"], 16), []).append(row)
        image = Image(data=data, xbe=parse_xbe(data), functions=functions, xrefs_to=xrefs_to)
        image._starts = sorted(int(f["start"], 16) for f in functions)
        return image

    def decoder(self) -> capstone.Cs:
        if self._cs is None:
            self._cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            self._cs.detail = True
        return self._cs

    def function(self, start: int) -> dict | None:
        return next((f for f in self.functions if int(f["start"], 16) == start), None)

    def containing(self, address: int) -> dict | None:
        candidates = [
            f for f in self.functions if int(f["start"], 16) <= address < int(f["end"], 16)
        ]
        return candidates[-1] if candidates else None

    def decode(self, address: int, count: int = 1) -> list:
        offset = self.xbe.va_to_offset(address)
        if offset is None:
            return []
        return list(self.decoder().disasm(self.data[offset : offset + 16 * count], address, count))

    def function_instructions(self, function: dict) -> list:
        start, end = int(function["start"], 16), int(function["end"], 16)
        offset = self.xbe.va_to_offset(start)
        if offset is None:
            return []
        out, position = [], 0
        body = self.data[offset : offset + (end - start) + 1]
        while position < len(body):
            decoded = list(self.decoder().disasm(body[position:], start + position, 1))
            if not decoded:
                break
            out.append(decoded[0])
            position += decoded[0].size
        return out


def _target(insn: capstone.CsInsn) -> int | None:
    ops = insn.operands
    if len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
        return int(ops[0].imm)
    return None


def _is_cond_jump(insn: capstone.CsInsn) -> bool:
    return capstone.CS_GRP_JUMP in insn.groups and insn.mnemonic not in _FLAG_NEUTRAL_JUMPS


@dataclass
class SiteReport:
    consumer: int
    reads: list[str] = field(default_factory=list)
    call_sites: list[int] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    @property
    def proven(self) -> bool:
        return not self.problems


def entry_walk(image: Image, function: dict) -> tuple[list[str], list[str]]:
    """(flag reads, problems) of every path from the entry until a flag write."""
    start, end = int(function["start"], 16), int(function["end"], 16)
    instructions = {i.address: i for i in image.function_instructions(function)}
    reads: list[str] = []
    problems: list[str] = []
    seen: set[int] = set()
    pending = [start]
    while pending:
        address = pending.pop()
        while True:
            if address in seen:
                break
            if len(seen) > MAX_WALK:
                problems.append("walk budget exhausted with the flags still live")
                return reads, problems
            seen.add(address)
            insn = instructions.get(address)
            if insn is None:
                problems.append(f"0x{address:08X} is not an instruction of the function")
                break
            reading, writing = _effect(insn)
            mnemonic = insn.mnemonic
            if reading:
                if not mnemonic.startswith(_ANSWERED) or mnemonic in ("jmp", "jecxz", "jcxz"):
                    problems.append(
                        f"0x{address:08X} {mnemonic} reads flags, "
                        "the bridge answers only jcc setcc cmovcc"
                    )
                    break
                reads.append(f"0x{address:08X} {mnemonic}")
            if writing:
                break
            if mnemonic in ("ret", "retn", "retf", "iret", "iretd"):
                problems.append(f"0x{address:08X} {mnemonic} returns with the flags still live")
                break
            if mnemonic == "call":
                problems.append(f"0x{address:08X} call with the flags still live")
                break
            if mnemonic == "jmp" or _is_cond_jump(insn):
                target = _target(insn)
                if target is None:
                    problems.append(
                        f"0x{address:08X} indirect {mnemonic} with the flags still live"
                    )
                    break
                if not start <= target < end:
                    problems.append(
                        f"0x{address:08X} {mnemonic} leaves the function with the flags live"
                    )
                elif target == start:
                    problems.append(f"0x{address:08X} {mnemonic} branches back to the entry")
                else:
                    pending.append(target)
                if mnemonic == "jmp":
                    break
            address += insn.size
    return reads, problems


def _data_references(image: Image, address: int) -> list[int]:
    needle = address.to_bytes(4, "little")
    hits: list[int] = []
    position = image.data.find(needle)
    while position != -1:
        hits.append(position)
        position = image.data.find(needle, position + 1)
    return hits


def provider_problems(image: Image, provider: int) -> list[str]:
    function = image.function(provider)
    if function is None:
        return [f"provider 0x{provider:08X} is not a function of the lifted tree"]
    start, end = int(function["start"], 16), int(function["end"], 16)
    problems = []
    rets = 0
    for insn in image.function_instructions(function):
        if insn.mnemonic in ("ret", "retn"):
            rets += 1
        elif insn.mnemonic == "jmp" or _is_cond_jump(insn):
            target = _target(insn)
            if target is None:
                problems.append(
                    f"0x{insn.address:08X} indirect {insn.mnemonic} leaves the provider"
                )
            elif not start <= target < end:
                problems.append(
                    f"0x{insn.address:08X} {insn.mnemonic} 0x{target:08X} is a tail exit, "
                    "it would skip the publish"
                )
    if not rets:
        problems.append(f"provider 0x{provider:08X} has no ret")
    return problems


def prove_consumer(image: Image, bridge: Bridge, consumer: int) -> SiteReport:
    report = SiteReport(consumer=consumer)
    function = image.function(consumer)
    if function is None:
        report.problems.append("not a function of the lifted tree")
        return report
    reads, problems = entry_walk(image, function)
    report.reads = reads
    report.problems.extend(problems)
    if not reads:
        report.problems.append("no flag read before a flag write: nothing to bridge")

    references = image.xrefs_to.get(consumer, [])
    foreign = [r for r in references if r["type"] != "call"]
    if foreign:
        report.problems.append(
            "referenced other than by a direct call: "
            + ", ".join(f"{r['type']} from {r['from']}" for r in foreign[:4])
        )
    sites = sorted(int(r["from"], 16) for r in references if r["type"] == "call")
    report.call_sites = sites
    if not sites:
        report.problems.append("no direct call site: nothing provides its flags")
    words = _data_references(image, consumer)
    if words:
        report.problems.append(
            f"{len(words)} image word(s) hold its address "
            f"(file offsets {[hex(w) for w in words[:4]]})"
        )

    for site in sites:
        call = image.decode(site)
        if not call or call[0].mnemonic != "call" or _target(call[0]) != consumer:
            report.problems.append(f"0x{site:08X} is not a direct call of the consumer")
            continue
        caller = image.containing(site)
        if caller is None:
            report.problems.append(f"0x{site:08X} is in no function")
            continue
        before = None
        for insn in image.function_instructions(caller):
            if insn.address + insn.size == site:
                before = insn
                break
        if before is None or before.mnemonic != "call" or _target(before) not in bridge.providers:
            shown = "nothing" if before is None else f"{before.mnemonic} {before.op_str}"
            report.problems.append(f"0x{site:08X} is preceded by {shown}, not a provider call")
            continue
        entering = [r for r in image.xrefs_to.get(site, []) if r["type"] in ("jump", "cond_jump")]
        if entering:
            report.problems.append(
                f"a branch enters at 0x{site:08X} between the provider call and the consumer"
            )
    return report


def prove(image: Image, bridge: Bridge) -> list[str]:
    """Every problem of the whole bridge, empty when each consumer and provider is proven."""
    problems: list[str] = []
    for provider in bridge.providers:
        problems.extend(provider_problems(image, provider))
    for consumer in bridge.consumers:
        report = prove_consumer(image, bridge, consumer)
        problems.extend(f"consumer 0x{consumer:08X}: {p}" for p in report.problems)
    return problems


def applied_problems(gen_dir: Path, bridge: Bridge) -> list[str]:
    """After a lift: every provider publishes and every consumer reads the bridge, nobody traps."""
    wanted = {f"sub_{a:08X}": ("provider", a) for a in bridge.providers}
    wanted.update({f"sub_{a:08X}": ("consumer", a) for a in bridge.consumers})
    bodies: dict[str, list[str]] = {}
    for chunk in sorted(gen_dir.glob("recomp_[0-9]*.c")):
        current = None
        for line in chunk.read_text(encoding="utf-8", errors="replace").splitlines():
            match = _FUNCTION.match(line)
            if match:
                current = f"sub_{match.group(1)[4:].upper()}"
            if current in wanted:
                bodies.setdefault(current, []).append(line)
    problems = []
    for name, (role, address) in sorted(wanted.items()):
        body = "\n".join(bodies.get(name, []))
        if not body:
            problems.append(f"{role} 0x{address:08X} has no generated body")
        elif role == "provider" and "g_flag_bridge_eflags =" not in body:
            problems.append(f"provider 0x{address:08X} publishes nothing")
        elif role == "consumer" and not (
            "_bridge = g_flag_bridge_eflags" in body and "g_flag_bridge_mask = 0u;" in body
        ):
            problems.append(f"consumer 0x{address:08X} reads no bridge")
        elif role == "consumer" and "RECOMP_BRIDGE_FLAGS(" not in body:
            problems.append(f"consumer 0x{address:08X} reads the bridge but never consults it")
        elif role == "consumer" and "RECOMP_FLAGS_UNRESOLVED" in body:
            problems.append(f"consumer 0x{address:08X} still has an unresolved condition")
    return problems


# --------------------------------------------------------------------------------- census


@dataclass
class Site:
    function: str
    address: int
    condition: str
    flags_from: str
    reach: str
    callers: int


def classify_site(image: Image, function: dict, address: int) -> tuple[str, str, int]:
    """(flags come from, reachability, direct callers) of one unresolved condition."""
    start = int(function["start"], 16)
    instructions = image.function_instructions(function)
    index = next((i for i, x in enumerate(instructions) if x.address == address), None)
    callers = len([r for r in image.xrefs_to.get(start, []) if r["type"] == "call"])
    refs = image.xrefs_to.get(start, [])
    if callers:
        reach = "called"
    elif refs:
        reach = "address-taken or jumped to only"
    else:
        reach = "unreferenced"
    if index is None:
        return "not on an instruction boundary", reach, callers
    targets = set()
    for insn in instructions:
        if insn.mnemonic != "call":
            target = _target(insn)
            if target is not None:
                targets.add(target)
    flags_from = "function entry"
    for back in range(index - 1, -1, -1):
        insn = instructions[back]
        following = instructions[back + 1]
        if following.address in targets:
            flags_from = f"join (a branch lands at 0x{following.address:08X})"
            break
        if insn.mnemonic in ("jmp", "ret", "retn", "ljmp"):
            flags_from = f"block start after {insn.mnemonic}"
            break
        if insn.mnemonic == "call":
            flags_from = "a call"
            break
        if _effect(insn)[1]:
            flags_from = f"{insn.mnemonic} (a setter no rule answers)"
            break
    return flags_from, reach, callers


def census(image: Image, gen_dir: Path) -> list[Site]:
    by_name = {f["name"].lower(): f for f in image.functions}
    sites: list[Site] = []
    for chunk in sorted(gen_dir.glob("recomp_[0-9]*.c")):
        current = None
        for line in chunk.read_text(encoding="utf-8", errors="replace").splitlines():
            match = _FUNCTION.match(line)
            if match:
                current = match.group(1).lower()
            found = _UNRESOLVED.search(line)
            if found and current:
                function = by_name.get(current)
                if function is None:
                    continue
                address = int(found.group(2), 16)
                flags_from, reach, callers = classify_site(image, function, address)
                sites.append(Site(current, address, found.group(1), flags_from, reach, callers))
    return sites


def summarise(sites: list[Site]) -> list[str]:
    def family(text: str) -> str:
        return re.sub(r"\(.*\)|0x[0-9A-Fa-f]+", "", text).strip() or text

    table = Counter((family(s.flags_from), s.reach) for s in sites)
    lines = [f"{len(sites)} unresolved flag sites in {len({s.function for s in sites})} functions"]
    for (origin, reach), count in sorted(table.items(), key=lambda item: -item[1]):
        lines.append(f"  {count:4d}  flags from {origin:<28} {reach}")
    return lines


# ------------------------------------------------------------ flags after a replaced call

_CALL_SITE = re.compile(r"PUSH32\(esp, 0x([0-9A-Fa-f]{8})u\); RECOMP_ABI_CALL\(0x([0-9A-Fa-f]{8})u")


def replaced_vas(game_dir: Path) -> list[int]:
    """Guest addresses with a hand replacement, from the GAME_REPLACE registrations."""
    from tools.replace.scan import scan_directory

    return sorted({registration.va for registration in scan_directory(game_dir)})


def flag_reader_after(image: Image, return_address: int) -> list[str]:
    """Flag reads reachable from a return address before any flag write (both arms followed).

    A hand replacement reproduces registers, memory and the x87 state but not EFLAGS, so a
    caller that reads the flags the original left is wrong. `call`, `ret` and an indirect jump
    with the flags still live are reported as UNRESOLVED rather than guessed.
    """
    out: list[str] = []
    seen: set[int] = set()
    pending = [return_address]
    while pending:
        address = pending.pop()
        while address not in seen and len(seen) < MAX_WALK:
            seen.add(address)
            decoded = image.decode(address)
            if not decoded:
                out.append(f"UNRESOLVED 0x{address:08X} unreadable code")
                break
            insn = decoded[0]
            reading, writing = _effect(insn)
            if reading:
                out.append(f"READS 0x{address:08X} {insn.mnemonic} {insn.op_str}".rstrip())
                break
            if writing:
                break
            if insn.mnemonic == "call" and _target(insn) is not None:
                # A callee entered with the flags live reads them (a consumer) or overwrites them.
                callee = image.function(_target(insn))
                if callee is None:
                    out.append(
                        f"UNRESOLVED 0x{address:08X} call of no known function with the flags live"
                    )
                    break
                reads, problems = entry_walk(image, callee)
                if reads:
                    out.append(
                        f"READS 0x{address:08X} call 0x{_target(insn):08X} "
                        f"whose entry reads {reads[0]}"
                    )
                elif problems:
                    out.append(
                        f"UNRESOLVED 0x{address:08X} call 0x{_target(insn):08X}: {problems[0]}"
                    )
                break
            if insn.mnemonic in ("ret", "retn", "call") or (
                insn.mnemonic == "jmp" and _target(insn) is None
            ):
                out.append(f"UNRESOLVED 0x{address:08X} {insn.mnemonic} with the flags live")
                break
            if insn.mnemonic == "jmp" or _is_cond_jump(insn):
                target = _target(insn)
                if target is None:
                    out.append(f"UNRESOLVED 0x{address:08X} indirect {insn.mnemonic}")
                    break
                pending.append(target)
                if insn.mnemonic == "jmp":
                    break
            address += insn.size
    return out


def replaced_flag_report(image: Image, gen_dir: Path, game_dir: Path) -> list[str]:
    """One line per replaced function: its direct call sites and which of them read its flags."""
    vas = replaced_vas(game_dir)
    sites: dict[int, set[int]] = {va: set() for va in vas}
    for chunk in sorted(gen_dir.glob("recomp_[0-9]*.c")):
        for match in _CALL_SITE.finditer(chunk.read_text(encoding="utf-8", errors="replace")):
            target = int(match.group(2), 16)
            if target in sites:
                sites[target].add(int(match.group(1), 16))
    lines = []
    for va in vas:
        reads, unresolved = [], []
        for site in sorted(sites[va]):
            for finding in flag_reader_after(image, site):
                (reads if finding.startswith("READS") else unresolved).append(
                    f"after 0x{site:08X}: {finding}"
                )
        verdict = "FLAGS READ" if reads else ("flags unresolved" if unresolved else "flags dead")
        lines.append(f"0x{va:08X} {len(sites[va])} direct site(s) {verdict}")
        lines += [f"    {r}" for r in reads + unresolved]
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.lift.flag_bridge", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument("command", choices=("prove", "census", "applied", "replaced"))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--lifted", type=Path, default=Path("generated/lifted"))
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument(
        "--gen", type=Path, default=None, help="generated chunks (default: LIFTED/gen)"
    )
    parser.add_argument("--list", action="store_true", help="census: print every site")
    args = parser.parse_args(argv)
    gen = args.gen or args.lifted / "gen"
    bridge = load_bridge(args.config)
    if args.command == "applied":
        problems = applied_problems(gen, bridge)
        print(
            "\n".join(problems)
            if problems
            else "applied: every provider publishes, every consumer reads the bridge"
        )
        return 1 if problems else 0
    image = Image.open(args.xbe, args.lifted)
    if args.command == "prove":
        for consumer in bridge.consumers:
            report = prove_consumer(image, bridge, consumer)
            status = "PROVEN" if report.proven else "REFUSED"
            print(
                f"consumer 0x{consumer:08X} {status}, {len(report.call_sites)} call site(s), "
                f"reads {report.reads}"
            )
            for problem in report.problems:
                print(f"  problem: {problem}")
        for provider in bridge.providers:
            found = provider_problems(image, provider)
            print(f"provider 0x{provider:08X} {'PROVEN' if not found else 'REFUSED'}")
            for problem in found:
                print(f"  problem: {problem}")
        return 1 if prove(image, bridge) else 0
    if args.command == "replaced":
        report = replaced_flag_report(image, gen, REPO_ROOT / "src" / "game")
        print("\n".join(report))
        return 1 if any("FLAGS READ" in line for line in report) else 0
    sites = census(image, gen)
    print("\n".join(summarise(sites)))
    if args.list:
        for s in sorted(sites, key=lambda x: x.address):
            print(
                f"0x{s.address:08X} {s.condition:<4} {s.function} callers={s.callers} "
                f"{s.reach}: {s.flags_from}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
