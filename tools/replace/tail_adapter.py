# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 typed tail adapter checks (opt-in guarded-jump contract only).

An original that leaves its body through `jmp <exact function entry>` is replaced by a draft that
calls `game_guest_tail(<that exact VA>, GAME_CC_x, N)` (tools/replace/game_guest_tail.h). The
adapter itself never invents a return VA and never saves or restores a register, so the target
runs on the live full register file. This module checks, against the ORIGINAL bytes only, that

* every original out-of-body direct JMP targets an exact known function entry (not the stack
  probe, not an interior pointer, not the adjacent function) whose pop is derivable,
* the enclosing registration is GAME_REPLACE_EXACT* (no register is excluded from the compare,
  so EBX/EBP/ESI/EDI and ESP are compared exactly like EAX/ECX/EDX),
* the draft calls the adapter with a literal target equal to each original target, with the
  convention/argument count whose pop equals the original target's pop, and
* no `game_guest_call` (a call, not a tail) reaches a tail target, and no `game_guest_tail`
  call has a computed target.

Runtime evidence (per-case tail taken on both sides) is produced by guarded_jump_session.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

from tools.harness.callstub import CalleeConventions
from tools.harness.jumps import direct_jump_target
from tools.harness.stackprobe import STACKPROBE_VA

TAIL_CALL = re.compile(r"\bgame_guest_tail\s*\(")
STRICT_TAIL_CALL = re.compile(
    r"\bgame_guest_tail\s*\(\s*(0[xX][0-9A-Fa-f]+)[uU]?\s*,\s*GAME_CC_(cdecl|stdcall|thiscall|fastcall)"
    r"\s*,\s*(\d+)[uU]?\s*\)"
)
GUEST_CALL = re.compile(r"\bgame_guest_call\s*\(\s*(0[xX][0-9A-Fa-f]+)")


class TailRefusal(ValueError):
    """The original or the draft is outside the typed tail contract."""


@dataclass(frozen=True)
class TailSite:
    site: int
    target: int
    cleanup_bytes: int

    def document(self) -> dict[str, object]:
        return {
            "site": f"0x{self.site:08x}",
            "target": f"0x{self.target:08x}",
            "cleanup_bytes": self.cleanup_bytes,
        }


def original_tails(
    va: int,
    size: int,
    instructions: Iterable[Any],
    sizes: dict[int, int],
    conventions: CalleeConventions,
) -> tuple[TailSite, ...]:
    """Every out-of-body direct JMP of the original, each proven to be an exact-entry tail."""
    found: list[TailSite] = []
    for insn in instructions:
        target = direct_jump_target(insn)
        if target is None or va <= target < va + size:
            continue
        if target in (va + size, STACKPROBE_VA) or sizes.get(target, 0) <= 0:
            raise TailRefusal(f"tail jmp at 0x{insn.address:08x} is not an exact known entry")
        pop = conventions.pop_bytes(target)
        if pop is None:
            raise TailRefusal(f"tail target 0x{target:08x} has no derivable pop")
        found.append(TailSite(insn.address, target, pop))
    return tuple(found)


def check_draft(entry_text: str, registration: Any, sites: tuple[TailSite, ...]) -> list[str]:
    """Problems that must refuse the tail adapter (empty list = typed contract satisfied)."""
    problems: list[str] = []
    if not sites:
        return ["no original tail site"]
    if not registration.exact_registers:
        problems.append(
            "a tail root must be GAME_REPLACE_EXACT* so the full register file is compared"
        )
    all_calls = TAIL_CALL.findall(entry_text)
    strict = STRICT_TAIL_CALL.findall(entry_text)
    if len(all_calls) != len(strict):
        problems.append("game_guest_tail target/convention/count must be literals")
    declared = {int(address, 16) for address, _cc, _count in strict}
    wanted = {site.target for site in sites}
    if declared != wanted:
        problems.append(
            "tail targets differ: draft "
            + ",".join(f"0x{v:08X}" for v in sorted(declared))
            + " original "
            + ",".join(f"0x{v:08X}" for v in sorted(wanted))
        )
    pops = {site.target: site.cleanup_bytes for site in sites}
    for address, convention, count in strict:
        target = int(address, 16)
        popped = 0 if convention == "cdecl" else 4 * int(count)
        if target in pops and popped != pops[target]:
            problems.append(f"tail 0x{target:08X} pops {pops[target]} but the draft says {popped}")
    registered_pop = {
        "cdecl": 0,
        "stdcall": 4 * registration.stack_args,
        "thiscall": 4 * registration.stack_args,
        "fastcall": 4 * registration.stack_args,
    }[registration.convention]
    for site in sites:
        if site.cleanup_bytes != registered_pop:
            problems.append(
                f"registration pops {registered_pop} but the tail target pops {site.cleanup_bytes}"
            )
    for address in GUEST_CALL.findall(entry_text):
        if int(address, 16) in wanted:
            problems.append(f"0x{int(address, 16):08X} is a tail target but is invoked as a call")
    return sorted(set(problems))
