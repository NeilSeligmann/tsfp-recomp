# SPDX-License-Identifier: GPL-3.0-or-later
"""Record the calls the title's own code makes into the DSOUND section while it runs under the
oracle.

A call is recorded when the guest reaches a surface address from OUTSIDE the DSOUND section, with
the
stack arguments the callee will pop (counted from its own `ret imm16`), `ecx`, the return address,
and, once control comes back to the caller, `eax`. Calls the library makes to its own surface
addresses are not recorded: the title cannot see them.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from tools.dsoundscan.image import Image
from tools.dsoundscan.oracle import Oracle
from tools.dsoundscan.surface import read_surface, stack_arguments


@dataclass
class Call:
    address: int
    name: str | None
    arguments: tuple[int, ...]
    ecx: int
    caller: int
    result: int | None = None


class CallRecorder:
    """Hooks every surface address of `oracle` and appends a `Call` per game-side entry."""

    def __init__(self, oracle: Oracle, image: Image | None = None) -> None:
        self.oracle = oracle
        self.calls: list[Call] = []
        self._pending: dict[int, list[tuple[int, Call]]] = {}
        image = image or oracle.image
        low, high = image.dsound_range
        self._low, self._high = low, high
        names = {row.address: row.name for row in read_surface()}
        for address, name in names.items():
            count = stack_arguments(image, address) or 0
            oracle.hook_pc(address, self._entry(address, name, count))

    def _entry(self, address: int, name: str | None, count: int) -> Callable[[Oracle], None]:
        def fire(oracle: Oracle) -> None:
            esp = oracle.register("esp")
            caller = oracle.read32(esp)
            if self._low <= caller < self._high:
                return
            call = Call(
                address,
                name,
                tuple(oracle.read32(esp + 4 + 4 * index) for index in range(count)),
                oracle.register("ecx"),
                caller,
            )
            self.calls.append(call)
            if caller not in self._pending:
                self._pending[caller] = []
                oracle.hook_pc(caller, self._return_hook(caller))
            self._pending[caller].append((esp + 4 + 4 * count, call))

        return fire

    def _return_hook(self, caller: int) -> Callable[[Oracle], None]:
        def fire(oracle: Oracle) -> None:
            esp = oracle.register("esp")
            stack = self._pending[caller]
            for index in range(len(stack) - 1, -1, -1):
                if stack[index][0] == esp:
                    stack[index][1].result = oracle.register("eax")
                    del stack[index]
                    return

        return fire
