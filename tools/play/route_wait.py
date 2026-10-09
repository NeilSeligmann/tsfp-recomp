# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1633: the event waits of a route, parsed and validated BEFORE the game launches.

`src/input/xinput_route_spec.c` is the authority (`xinput_route_event_wait_parse`). This module is its Python mirror for
`python -m tools.play --check-route` and `tools.route_events`, and a differential test
(`tests/test_t1633_route_waits.py`) compiles the C parser and compares accept/reject and the parsed fields on a corpus.

A wait is `markK:field,field,...` (docs/input-replay.md "Event driven route"). The record itself may carry
`# wait: markK:fields` lines, the command line may add `--route-wait-event SPEC` (a command line wait for a mark replaces
the record's wait for it). Marks are the `# mark: at=N` lines of the record.
"""

from __future__ import annotations

import re
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

ROUTE_COND_MAX = 6
ROUTE_COND_TEXT = 48
MAX_MARKS = 16
MAX_WAITS = 8
MAX_POLLS_LIMIT = 10_000_000
TIMEOUT_MS_LIMIT = 86_400_000
DEFAULT_TIMEOUT_MS = 600_000
DEFAULT_MAX_POLLS = 36_000
GUEST_LIMIT = 0x04000000

_NUMBER = re.compile(r"0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*")
_WS = " \t\n\v\f\r"


class WaitError(ValueError):
    """The spec is refused (the C parser returns false)."""


def c_number(text: str, limit: int) -> int | None:
    """`number()` of xinput_route_spec.c: strtoull base 0, no sign, at most 31 characters, <= limit."""
    if not text or len(text) >= 32 or text[0] in "-+":
        return None
    stripped = text.lstrip(_WS)
    if not _NUMBER.fullmatch(stripped):
        return None
    if stripped[:2] in ("0x", "0X"):
        value = int(stripped, 16)
    elif stripped[0] == "0":
        value = int(stripped, 8)
    else:
        value = int(stripped, 10)
    return value if value <= limit else None


@dataclass
class Cond:
    kind: str  # mem file-open file-read file-idle call frame-change frame-stable
    indirect: bool = False
    address: int = 0
    offset: int = 0
    width: int = 4
    cmp: str = "=="
    value: int = 0
    mask: int = 0xFFFFFFFF
    text: str = ""
    va: int = 0
    amount: int = 0

    def format(self) -> str:
        """`xinput_route_cond_format`."""
        if self.kind == "mem":
            if self.indirect:
                return f"mem=*0x{self.address:X}+0x{self.offset:X}:{self.width}{self.cmp}0x{self.value:X}"
            return f"mem=0x{self.address:X}:{self.width}{self.cmp}0x{self.value:X}"
        if self.kind == "file-open":
            return f"file-open={self.text}"
        if self.kind == "file-read":
            return f"file-read={self.text}@{self.amount}"
        if self.kind == "file-idle":
            return (
                f"file-idle={self.amount}@{self.text}" if self.text else f"file-idle={self.amount}"
            )
        if self.kind == "call":
            return f"call=0x{self.va:X}@{self.amount}"
        if self.kind == "frame-change":
            return "frame-change"
        return f"frame-stable={self.amount}"


@dataclass
class Wait:
    mark: int = 0
    event: bool = True
    conds: list[Cond] = field(default_factory=list)
    any: bool = False
    min_polls: int = 0
    min_ms: int = 0
    max_polls: int = 0
    timeout_ms: int = 0
    # legacy (--route-wait) memory condition
    has_mem: bool = False
    address: int = 0
    width: int = 4
    value: int = 0
    mask: int = 0xFFFFFFFF


def _lower_text(text: str) -> str | None:
    if not text or len(text) >= ROUTE_COND_TEXT:
        return None
    out = []
    for char in text:
        code = ord(char)
        if code < 0x20 or code > 0x7E or char == ",":
            return None
        out.append(char.lower() if "A" <= char <= "Z" else char)
    return "".join(out)


_OPERATORS = ("==", "!=", "<=", ">=", "<", ">")


def _find_operator(text: str) -> tuple[int, str] | None:
    for index in range(len(text)):
        for (
            operator
        ) in _OPERATORS:  # the C scan tests the two character forms first at each position
            if text.startswith(operator, index):
                return index, operator
    return None


def _mem_event_condition(text: str) -> Cond | None:
    if not text or len(text) >= 96:
        return None
    indirect = text[0] == "*"
    start = 1 if indirect else 0
    found = _find_operator(text[start:])
    if found is None:
        return None
    op_index, operator = found
    op_index += start
    colon = text.find(":", start, op_index)
    address_end = colon if colon != -1 else op_index
    plus = text.find("+", start, address_end) if indirect else -1
    offset = 0
    if plus != -1:
        parsed = c_number(text[plus + 1 : address_end], 0xFFFFFFFF)
        if parsed is None:
            return None
        offset = parsed
        address_end = plus
    address = c_number(text[start:address_end], 0xFFFFFFFF)
    if address is None:
        return None
    width = 4
    if colon != -1:
        parsed = c_number(text[colon + 1 : op_index], 4)
        if parsed is None or parsed not in (1, 2, 4):
            return None
        width = parsed
    value_text = text[op_index + len(operator) :]
    amp = value_text.find("&")
    value_part = value_text[:amp] if amp != -1 else value_text
    value = c_number(value_part, 0xFFFFFFFF)
    if value is None:
        return None
    mask = 0xFFFFFFFF
    if amp != -1:
        parsed = c_number(value_text[amp + 1 :], 0xFFFFFFFF)
        if parsed is None:
            return None
        mask = parsed
    if width < 4:
        mask &= (1 << (8 * width)) - 1
    if indirect:
        if address % 4 != 0 or address + 4 > GUEST_LIMIT:
            return None
    elif address % width != 0 or address + width > GUEST_LIMIT:
        return None
    if operator == "==" and (value & ~mask & 0xFFFFFFFF) != 0:
        return None
    return Cond("mem", indirect, address, offset, width, operator, value, mask)


def _split_at(text: str, sign: str = "@", last: bool = False) -> tuple[str, str | None]:
    index = text.rfind(sign) if last else text.find(sign)
    return (text, None) if index == -1 else (text[:index], text[index + 1 :])


def _event_field(item: str, wait: Wait) -> bool:
    """One field of an event wait; True when it is good (mirrors `event_field`)."""

    def add(cond: Cond) -> bool:
        if len(wait.conds) >= ROUTE_COND_MAX:
            return False
        wait.conds.append(cond)
        return True

    if item.startswith("mem="):
        cond = _mem_event_condition(item[4:])
        return cond is not None and add(cond)
    if item.startswith("file-open="):
        text = _lower_text(item[10:])
        return text is not None and add(Cond("file-open", text=text))
    if item.startswith("file-read="):
        body = item[10:]
        # C: the first '@' splits (memchr); the text before it is the substring
        text_part, amount_part = _split_at(body, "@")
        amount = 1
        if amount_part is not None:
            parsed = c_number(amount_part, 0xFFFFFFFF)
            if parsed is None or parsed == 0:
                return False
            amount = parsed
        text = _lower_text(text_part)
        return text is not None and add(Cond("file-read", text=text, amount=amount))
    if item.startswith("file-idle="):
        ms_part, text_part = _split_at(item[10:], "@")
        amount = c_number(ms_part, TIMEOUT_MS_LIMIT)
        if amount is None or amount == 0:
            return False
        text = ""
        if text_part is not None:
            lowered = _lower_text(text_part)
            if lowered is None:
                return False
            text = lowered
        return add(Cond("file-idle", text=text, amount=amount))
    if item.startswith("call="):
        va_part, count_part = _split_at(item[5:], "@")
        va = c_number(va_part, 0x03FFFFFF)
        if va is None or va == 0:
            return False
        amount = 1
        if count_part is not None:
            parsed = c_number(count_part, 0xFFFFFF)
            if parsed is None or parsed == 0:
                return False
            amount = parsed
        return add(Cond("call", va=va, amount=amount))
    if item == "frame-change":
        return add(Cond("frame-change"))
    if item.startswith("frame-stable="):
        amount = c_number(item[13:], TIMEOUT_MS_LIMIT)
        return amount is not None and amount != 0 and add(Cond("frame-stable", amount=amount))
    if item.startswith("min="):
        amount = c_number(item[4:], MAX_POLLS_LIMIT)
        if wait.min_polls == 0 and amount is not None and amount != 0:
            wait.min_polls = amount
            return True
        return False
    if item.startswith("min-ms="):
        amount = c_number(item[7:], TIMEOUT_MS_LIMIT)
        if wait.min_ms == 0 and amount is not None and amount != 0:
            wait.min_ms = amount
            return True
        return False
    if item.startswith("max="):
        amount = c_number(item[4:], MAX_POLLS_LIMIT)
        if wait.max_polls == 0 and amount is not None and amount != 0:
            wait.max_polls = amount
            return True
        return False
    if item.startswith("timeout="):
        amount = c_number(item[8:], TIMEOUT_MS_LIMIT)
        if wait.timeout_ms == 0 and amount is not None and amount != 0:
            wait.timeout_ms = amount
            return True
        return False
    if item == "any":
        if wait.any:
            return False
        wait.any = True
        return True
    return False


def _legacy_mem(text: str, wait: Wait) -> bool:
    eq = text.find("==")
    if eq == -1:
        return False
    colon = text.find(":", 0, eq)
    address_end = colon if colon != -1 else eq
    address = c_number(text[:address_end], 0xFFFFFFFF)
    if address is None:
        return False
    width = 4
    if colon != -1:
        parsed = c_number(text[colon + 1 : eq], 4)
        if parsed is None or parsed not in (1, 2, 4):
            return False
        width = parsed
    value_text = text[eq + 2 :]
    amp = value_text.find("&")
    value = c_number(value_text[:amp] if amp != -1 else value_text, 0xFFFFFFFF)
    if value is None:
        return False
    mask = 0xFFFFFFFF
    if amp != -1:
        parsed = c_number(value_text[amp + 1 :], 0xFFFFFFFF)
        if parsed is None:
            return False
        mask = parsed
    if address % width != 0 or address + width > GUEST_LIMIT:
        return False
    if width < 4:
        mask &= (1 << (8 * width)) - 1
    if value & ~mask & 0xFFFFFFFF:
        return False
    wait.has_mem, wait.address, wait.width, wait.value, wait.mask = (
        True,
        address,
        width,
        value,
        mask,
    )
    return True


def parse_wait(spec: str, event: bool = True) -> Wait:
    """`xinput_route_event_wait_parse` (event=True) or `xinput_route_wait_parse` (event=False). Raises WaitError."""
    wait = Wait(event=event, max_polls=0 if event else DEFAULT_MAX_POLLS)
    if not spec.startswith("mark"):
        raise WaitError(f"route wait '{spec}': want markK:fields")
    colon = spec.find(":")
    mark = c_number(spec[4:colon], MAX_MARKS) if colon != -1 else None
    if mark is None or mark == 0:
        raise WaitError(f"route wait '{spec}': the mark must be mark1..mark{MAX_MARKS}")
    wait.mark = mark
    have_min = False
    body = spec[colon + 1 :]
    position = 0
    while position < len(body):
        end = body.find(",", position)
        if end == -1:
            end = len(body)
        item = body[position:end]
        if event:
            good = _event_field(item, wait)
        elif item.startswith("mem=") and not wait.has_mem:
            good = 0 < len(item) - 4 < 96 and _legacy_mem(item[4:], wait)
        elif item.startswith("min=") and not have_min:
            amount = c_number(item[4:], MAX_POLLS_LIMIT)
            good = amount is not None
            if good:
                wait.min_polls = amount or 0
                have_min = True
        elif item.startswith("max="):
            amount = c_number(item[4:], MAX_POLLS_LIMIT)
            good = amount is not None and amount > 0
            if good:
                wait.max_polls = amount or 0
        else:
            good = False
        if not good:
            raise WaitError(
                f"route wait '{spec}': bad, repeated or too many fields near '{body[position : position + 24]}'"
            )
        position = end + 1 if end < len(body) else end
    if event:
        if not wait.conds and wait.min_polls == 0 and wait.min_ms == 0:
            raise WaitError(
                f"route wait '{spec}': give at least one condition (mem=, file-open=, ...) or min=/min-ms=, else it never waits"
            )
        if wait.any and len(wait.conds) < 2:
            raise WaitError(f"route wait '{spec}': 'any' needs two or more conditions")
        if wait.timeout_ms == 0 and wait.max_polls == 0:
            wait.timeout_ms = DEFAULT_TIMEOUT_MS
        if wait.timeout_ms and wait.min_ms > wait.timeout_ms:
            raise WaitError(f"route wait '{spec}': min-ms is above timeout")
        if wait.max_polls and wait.min_polls > wait.max_polls:
            raise WaitError(f"route wait '{spec}': min is above max")
        return wait
    if not wait.has_mem and not have_min:
        raise WaitError(f"route wait '{spec}': give mem=ADDR==VALUE or min=N, else it never waits")
    if wait.min_polls > wait.max_polls:
        raise WaitError(f"route wait '{spec}': min is above max")
    return wait


# ---- the record ----


@dataclass
class RecordInfo:
    polls: int = 0
    marks: list[int] = field(default_factory=list)
    waits: list[tuple[int, str]] = field(
        default_factory=list
    )  # (line number, spec text after '# wait:')
    infos: dict[int, str] = field(default_factory=dict)  # mark number -> '# mark-info:' text
    total_polls: int | None = None


def read_record(path: Path) -> RecordInfo:
    """Marks, `# wait:` lines, `# mark-info:` lines and the poll total of a record file."""
    info = RecordInfo()
    total = 0
    with path.open(encoding="utf-8", errors="replace") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.rstrip("\r\n")
            if line.startswith("# mark:"):
                match = re.fullmatch(r"# mark:\s*at=(\d+)\s*", line)
                if match:
                    info.marks.append(int(match.group(1)))
            elif line.startswith("# wait:"):
                info.waits.append((number, line[7:].strip()))
            elif line.startswith("# mark-info:"):
                match = re.fullmatch(r"# mark-info:\s*mark(\d+)\s+(.*)", line)
                if match:
                    info.infos[int(match.group(1))] = match.group(2)
            elif line.startswith("# polls:"):
                match = re.fullmatch(r"# polls:\s*(\d+)\s*", line)
                if match:
                    info.total_polls = int(match.group(1))
            elif line and not line.startswith("#"):
                head = line.split(None, 1)[0]
                if head.isdigit():
                    total += int(head)
    info.polls = total
    return info


def validate(
    record: RecordInfo,
    command_waits: Sequence[tuple[str, str]],
    host_flags: Sequence[str] = (),
    pokes: Sequence[str] = (),
) -> list[str]:
    """Problems the host would hit, as one line each. `command_waits` are (flag, spec) of --route-wait / --route-wait-event,
    `pokes` the --poke-at-poll specs (a markK trigger needs the mark)."""
    problems: list[str] = []
    for poke in pokes:
        match = re.match(r"mark(\d+):", poke)
        if match and not 1 <= int(match.group(1)) <= len(record.marks):
            problems.append(f"--poke-at-poll {poke}: the record has {len(record.marks)} mark(s)")
    seen: dict[int, str] = {}
    merged: list[tuple[str, int, Wait]] = []  # (where, mark, wait)
    for flag, spec in command_waits:
        try:
            wait = parse_wait(spec, event=flag == "--route-wait-event")
        except WaitError as error:
            problems.append(f"{flag} {spec}: {error}")
            continue
        if wait.mark in seen:
            problems.append(
                f"two waits for mark{wait.mark} on the command line ({seen[wait.mark]} and {flag})"
            )
        seen[wait.mark] = flag
        merged.append((f"{flag} {spec}", wait.mark, wait))
    command_marks = set(seen)
    record_seen: set[int] = set()
    for line_number, spec in record.waits:
        try:
            wait = parse_wait(spec, event=True)
        except WaitError as error:
            problems.append(f"record '# wait:' line {line_number}: {error}")
            continue
        if wait.mark in record_seen:
            problems.append(f"the record has two '# wait:' lines for mark{wait.mark}")
        record_seen.add(wait.mark)
        if wait.mark not in command_marks:
            merged.append((f"record line {line_number}", wait.mark, wait))
    if len(merged) > MAX_WAITS:
        problems.append(f"{len(merged)} waits, the route supports {MAX_WAITS}")
    for where, mark, wait in merged:
        if mark > len(record.marks):
            problems.append(
                f"{where}: names mark{mark} but the record has {len(record.marks)} mark(s)"
            )
        if (
            any(cond.kind == "call" for cond in wait.conds)
            and "--headless-second-vblank" not in host_flags
            and "--headless-first-vblank" not in host_flags
        ):
            problems.append(
                f"{where}: call= needs the cooperative safepoint (--headless-second-vblank)"
            )
    return problems
