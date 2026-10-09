# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Parse the three record kinds an instrumented boot prints.

  probe: n=6 t3 addr=0x003EE2B3 caller=0x00020C1C args=0x0,0x5608D0,... words=0x4B85E4:0x3D hash=HEX/LEN
  t51: enter fn=0x00018EA0 ret=0x0001F2A9 a=0x2,0x1,0x16,0x0,0x0
  t51: store fn=0x00018EA0 at=0x00018F62 val=0x16

`probe:` lines come from the host's TSFP_XDK_PROBE (src/host/xdk_thunk.c), the `t51:` lines
from the instrumented lift (`instrument.py`). Both go to stderr, which is unbuffered and
written by one call per line, so a merged capture keeps their true order. Anything else in
the capture is ignored, so the kernel and HLE chatter does not matter. The parser never
guesses: a line that starts a record kind and does not parse raises, because a record
dropped silently would make an absence of keys read as measured.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass, field

_NUMBER = r"0x[0-9A-Fa-f]+"
_PROBE = re.compile(
    rf"^probe: n=(?P<n>\d+) t(?P<thread>\d+) addr=(?P<addr>{_NUMBER}) caller=(?P<caller>{_NUMBER})"
    rf" args=(?P<args>{_NUMBER}(?:,{_NUMBER})*) words=(?P<words>[^ ]*)"
    rf"(?: hash=(?P<hash>(?:unreadable:)?[0-9a-f]{{16}})/(?P<length>\d+))?\s*$"
)
_ENTER = re.compile(
    rf"^t51: enter fn=(?P<fn>{_NUMBER}) ret=(?P<ret>{_NUMBER}) a=(?P<args>{_NUMBER}(?:,{_NUMBER})*)\s*$"
)
_STOPPED = re.compile(r"^(?P<who>(?:main|guest) thread .*?) stopped: (?P<reason>.+?)\s*$")
_STOP_ADDRESS = re.compile(rf"^\s+guest address\s+(?P<address>{_NUMBER})\s*$")
_STOP_DETAIL = re.compile(r"^\s+detail\s+(?P<detail>.*?)\s*$")
_STORE = re.compile(
    rf"^t51: store fn=(?P<fn>{_NUMBER}) at=(?P<at>{_NUMBER}) val=(?P<val>{_NUMBER})\s*$"
)


@dataclass(frozen=True)
class Probe:
    """One logged dispatch at an XDK address."""

    number: int
    thread: int
    address: int
    caller: int
    args: tuple[int, ...]
    #: sampled guest dwords, address -> value
    words: dict[int, int] = field(compare=False)
    #: FNV-1a 64 of the buffer the spec named, None when the spec named none
    digest: int | None = None
    length: int | None = None
    #: True when part of the named buffer could not be read
    unreadable: bool = False
    #: position of the record in the capture
    order: int = 0


@dataclass(frozen=True)
class Enter:
    function: int
    caller: int
    args: tuple[int, ...]
    order: int = 0


@dataclass(frozen=True)
class Store:
    function: int
    label: int
    value: int
    order: int = 0


@dataclass(frozen=True)
class Stop:
    """Where one host thread stopped, from the host's own end-of-run report."""

    thread: str
    reason: str
    address: int | None = None
    detail: str = ""


@dataclass
class Capture:
    probes: list[Probe] = field(default_factory=list)
    enters: list[Enter] = field(default_factory=list)
    stores: list[Store] = field(default_factory=list)
    stops: list[Stop] = field(default_factory=list)


def _numbers(text: str) -> tuple[int, ...]:
    return tuple(int(part, 16) for part in text.split(","))


def _words(text: str) -> dict[int, int]:
    result: dict[int, int] = {}
    for item in text.split(","):
        if not item:
            continue
        address, _, value = item.partition(":")
        if value.startswith("unreadable="):
            raise ValueError(f"a sampled word was unreadable: {item}")
        result[int(address, 16)] = int(value, 16)
    return result


def parse(lines: Iterable[str]) -> Capture:
    """Parse a capture. Raises ValueError on a malformed record of a known kind."""
    capture = Capture()
    for order, raw in enumerate(lines):
        line = raw.rstrip("\n")
        stopped = _STOPPED.match(line)
        if stopped:
            capture.stops.append(Stop(stopped.group("who"), stopped.group("reason")))
            continue
        if capture.stops and capture.stops[-1].detail == "" and capture.stops[-1].address is None:
            address = _STOP_ADDRESS.match(line)
            if address:
                last = capture.stops[-1]
                capture.stops[-1] = Stop(
                    last.thread, last.reason, int(address.group("address"), 16)
                )
                continue
        if capture.stops and capture.stops[-1].detail == "":
            detail = _STOP_DETAIL.match(line)
            if detail:
                last = capture.stops[-1]
                capture.stops[-1] = Stop(
                    last.thread, last.reason, last.address, detail.group("detail")
                )
                continue
        if line.startswith("probe: "):
            match = _PROBE.match(line)
            if not match:
                raise ValueError(f"malformed probe record: {line!r}")
            digest = match.group("hash")
            unreadable = bool(digest and digest.startswith("unreadable:"))
            capture.probes.append(
                Probe(
                    number=int(match.group("n")),
                    thread=int(match.group("thread")),
                    address=int(match.group("addr"), 16),
                    caller=int(match.group("caller"), 16),
                    args=_numbers(match.group("args")),
                    words=_words(match.group("words")),
                    digest=int(digest.removeprefix("unreadable:"), 16) if digest else None,
                    length=int(match.group("length")) if digest else None,
                    unreadable=unreadable,
                    order=order,
                )
            )
        elif line.startswith("t51: enter "):
            match = _ENTER.match(line)
            if not match:
                raise ValueError(f"malformed enter record: {line!r}")
            capture.enters.append(
                Enter(
                    function=int(match.group("fn"), 16),
                    caller=int(match.group("ret"), 16),
                    args=_numbers(match.group("args")),
                    order=order,
                )
            )
        elif line.startswith("t51: store "):
            match = _STORE.match(line)
            if not match:
                raise ValueError(f"malformed store record: {line!r}")
            capture.stores.append(
                Store(
                    function=int(match.group("fn"), 16),
                    label=int(match.group("at"), 16),
                    value=int(match.group("val"), 16),
                    order=order,
                )
            )
    return capture


def fnv1a64(data: bytes) -> int:
    """The hash `xdk_thunk.c` computes over a probed buffer."""
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value
