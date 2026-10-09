# SPDX-License-Identifier: GPL-3.0-or-later
"""Read what the host printed: how each guest thread stopped, and the ordered XDK calls.

The host prints, per stopped guest thread:

    guest thread 0xe10000 (entered 0x0037FE1D) stopped: host fault
      signal         11
      fault address  0x00000000FD001804

and, in call order, one line per HLE boundary crossing:

    280  t2  xdk 0x003D9230  Direct3D_CreateDevice   implemented   eax=0x0  from 0x00023B41
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from tools.mmio_reach.census import NV2A_WINDOW

_STOP = re.compile(
    r"^guest thread (0x[0-9a-fA-F]+) \(entered 0x([0-9A-Fa-f]+)\) stopped: (.+)$", re.MULTILINE
)
_FIELD = re.compile(r"^\s+(fault address|guest address|detail)\s+(.*)$")
_XDK = re.compile(
    r"^\s*(\d+)\s+t(\d+)\s+xdk\s+0x([0-9A-Fa-f]+)\s+(\S+)\s+(\S+)"
    r"\s+eax=(.+?)\s+from 0x([0-9A-Fa-f]+)\s*$"
)


@dataclass(frozen=True)
class ThreadStop:
    thread: str
    entry: int
    reason: str
    fault_address: int | None = None
    guest_address: int | None = None
    detail: str = ""

    @property
    def faulted_in_nv2a(self) -> bool:
        return self.fault_address is not None and self.fault_address in NV2A_WINDOW


@dataclass(frozen=True)
class XdkCall:
    sequence: int
    thread: int
    address: int
    name: str
    status: str
    return_address: int


def parse_stops(text: str) -> list[ThreadStop]:
    """One entry per `guest thread ... stopped:` block, with its address fields."""
    lines = text.splitlines()
    stops: list[ThreadStop] = []
    for position, line in enumerate(lines):
        header = _STOP.match(line)
        if not header:
            continue
        fields: dict[str, str] = {}
        for follower in lines[position + 1 :]:
            field = _FIELD.match(follower)
            if field is None:
                if follower.startswith("    ") or follower.startswith("  "):
                    continue
                break
            fields[field.group(1)] = field.group(2).strip()
        fault = fields.get("fault address")
        guest = fields.get("guest address")
        stops.append(
            ThreadStop(
                thread=header.group(1),
                entry=int(header.group(2), 16),
                reason=header.group(3).strip(),
                fault_address=int(fault, 16) if fault else None,
                guest_address=int(guest, 16) if guest else None,
                detail=fields.get("detail", ""),
            )
        )
    return stops


def parse_xdk_calls(text: str) -> list[XdkCall]:
    calls = []
    for line in text.splitlines():
        found = _XDK.match(line)
        if found:
            calls.append(
                XdkCall(
                    sequence=int(found.group(1)),
                    thread=int(found.group(2)),
                    address=int(found.group(3), 16),
                    name=found.group(4),
                    status=found.group(5),
                    return_address=int(found.group(7), 16),
                )
            )
    return calls
