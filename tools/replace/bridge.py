# SPDX-License-Identifier: GPL-3.0-or-later
"""Flag bridge exemption for the EFLAGS caller audit (T1252).

A replacement cannot leave the original EFLAGS in hardware, so the audit rejects a direct call
site whose return address is followed by a flag read. The flag bridge (T554,
`tools/lift/flag_bridge.py`) is the one sanctioned exception: the replacement PUBLISHES the
flag word and the lifted consumer READS it instead of the hardware flags. The exemption is
granted for one site only when ALL hold, each proven here and each pinned by a mutation test:

1. the replaced function is a bridge provider of the config, passes `provider_problems`, and
   its replacement source assigns both `g_flag_bridge_eflags` and `g_flag_bridge_mask`;
2. the instruction at the return address is a direct `call` of a bridge consumer C;
3. `prove_consumer` proves C on the image: every entry path reads flags only through
   jcc/setcc/cmovcc before a flag write, never leaves with them live, every reference to C is a
   direct call, and every call site of C is directly preceded by a provider call;
4. the return address is one of C's proven call sites;
5. the lifted tree really applies the bridge (`applied_problems` is empty).

Under those facts every path from the site reaches only bridged reads or a flag write, so the
walk's UNSAFE read is answered from the published word, not from the missing hardware flags.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.lift import flag_bridge as bridge_module

DEFAULT_CONFIG = Path("tools/config/flag_bridge.json")


@dataclass
class BridgeExemption:
    """Decides, per replaced function and return address, whether the bridge covers a flag read."""

    bridge: bridge_module.Bridge
    image: bridge_module.Image
    game_dir: Path
    sources: dict[int, str] = field(default_factory=dict)  # provider VA to replacement source name
    _cache: dict[tuple[int, int], str] = field(default_factory=dict)
    _global: list[str] | None = None

    @classmethod
    def open(
        cls, xbe: Path, gen_dir: Path, game_dir: Path, config: Path = DEFAULT_CONFIG
    ) -> BridgeExemption | None:
        """None when no config or no lifted tree is present (the exemption then never applies)."""
        from tools.lift import flag_bridge

        lifted = gen_dir.parent
        if not config.exists() or not (lifted / "disasm" / "functions.json").exists():
            return None
        bridge = flag_bridge.load_bridge(config)
        return cls(bridge, flag_bridge.Image.open(xbe, lifted), game_dir)

    def _global_problems(self, gen_dir: Path) -> list[str]:
        from tools.lift import flag_bridge

        if self._global is None:
            self._global = flag_bridge.applied_problems(gen_dir, self.bridge)
        return self._global

    def covers(self, va: int, source: str, return_va: int, gen_dir: Path) -> str:
        """The proof text when the bridge covers the flag read after `return_va`, else ''."""
        key = (va, return_va)
        if key not in self._cache:
            self._cache[key] = self._covers(va, source, return_va, gen_dir)
        return self._cache[key]

    def _covers(self, va: int, source: str, return_va: int, gen_dir: Path) -> str:
        from tools.lift import flag_bridge

        if va not in self.bridge.providers:
            return ""
        if flag_bridge.provider_problems(self.image, va):
            return ""
        text = (self.game_dir / source).read_text(encoding="utf-8", errors="replace")
        if "g_flag_bridge_eflags =" not in text or "g_flag_bridge_mask =" not in text:
            return ""
        following = self.image.decode(return_va)
        if not following or following[0].mnemonic != "call":
            return ""
        consumer = flag_bridge._target(following[0])
        if consumer not in self.bridge.consumers:
            return ""
        report = flag_bridge.prove_consumer(self.image, self.bridge, consumer)
        if not report.proven or return_va not in report.call_sites:
            return ""
        if self._global_problems(gen_dir):
            return ""
        return (
            f"flags bridged to consumer {consumer:#010x} (reads {', '.join(report.reads)}), "
            "provider publishes the flag word"
        )
