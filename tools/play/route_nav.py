# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1640: the nav steps of a route and the menu table, parsed and validated BEFORE the game launches.

`src/input/xinput_route_nav_spec.[ch]` is the authority (the host parses the same text). This module is its Python mirror for
`python -m tools.play --check-route`, `tools.route_nav` and `tools.route_trim`, the way `route_wait.py` mirrors
`xinput_route_event_wait_parse`; `tests/test_t1640_route_nav.py` compiles the C parser and compares accept/reject and every
parsed field on a corpus plus seeded mutations. Docs: docs/t1640-nav-tools.md.

Record line (a comment, an old host ignores it):

    # nav: at=P to=Q menu=ID select=SEL [activate] [expect=FIELDS] [timeout=MS] [retry=N]

* P, Q are record poll positions, 0 <= P <= Q <= the record's polls (C integers, base 0). The nav step plays INSTEAD of the
  recorded presses of the polls [P, Q): the host reads the menu state, presses the d-pad until the cursor is on the target,
  presses A (when `activate`), checks `expect`, then the replay continues at Q.
* At most 64 nav lines, ascending, ranges [P, Q) not overlapping (a range may start where the previous one ends), and no
  `# mark: at=M` with P < M < Q (a mark exactly at P or Q is fine: a mark is where the replay waits, never inside a nav range).
* SEL is `index:N` (0 based, 0..0xFFFFFF) or `name:TEXT`. TEXT is percent encoded, `%XX` for any byte (%20 space, %25 percent,
  %3D equals), the decoded name is 1..63 printable ASCII bytes (0x20..0x7E) and matches the item label case blind.
* `expect=FIELDS` is a T1633 wait field list WITHOUT spaces (`route_wait.parse_wait`, `mark1:` is prepended for the check) and
  needs `activate` (it is the post-condition of the activation).
* `timeout=MS` 1..86400000, default 120000. `retry=N` 0..100, default 5.

Menu table (tools/data/menu_nav.txt), one menu per line, everything after a `#` is a comment:

    menu ID active=<cond>[,<cond>] cursor=<mem> count=<mem> axis=v|h wrap=0|1 [ready=<cond>[,<cond>]]
         [names=<mem>/STRIDE/ascii|utf16|ptr-ascii|ptr-utf16/MAXLEN] [select=BUTTON] [conf=TEXT]

`<cond>` is the T1633 `mem=` field value `[*]ADDR[+OFF][:W]OP VALUE[&MASK]`, `<mem>` the same without the operator and value,
`[*]ADDR[+OFF][:W][&MASK]` (the host parses it as `==0`), BUTTON one of A B X Y BLACK WHITE START BACK.
"""

from __future__ import annotations

import re
import subprocess
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path

from tools.play import route_wait

NAV_PREFIX = "# nav:"
HOST_MARKER = "T1640"
MAX_NAV_LINES = 64
NAV_POLL_LIMIT = 0xFFFFFFFF * 16  # UINT32_MAX * 16ull
NAV_TEXT_LIMIT = 512  # ROUTE_NAV_LINE_MAX, the text after '# nav:'
NAME_MAX = 63  # decoded bytes (ROUTE_NAV_NAME_MAX 64 with the NUL)
SELECT_TEXT_LIMIT = 112
EXPECT_LIMIT = 224
INDEX_MAX = 0xFFFFFF
DEFAULT_TIMEOUT_MS = 120_000
DEFAULT_RETRY = 5
RETRY_MAX = 100
MAX_MENUS = 32
TIMING_LIMIT = 120
DEFAULT_LOST_EXTRA = 2
MODES = ("on", "off", "strict")
MENU_ID_RE = re.compile(r"[A-Za-z0-9_.-]{1,31}")
SELECT_BUTTONS = ("A", "B", "X", "Y", "BLACK", "WHITE", "START", "BACK")
NAME_KINDS = ("ascii", "utf16", "ptr-ascii", "ptr-utf16")
DEFAULT_MENU_FILE = "tools/data/menu_nav.txt"
NAV_KEYS = ("at", "to", "menu", "select", "activate", "expect", "timeout", "retry")
_SPACE = re.compile(r"[ \t\r\n]+")


class NavError(ValueError):
    """A nav line or menu table line the host would refuse."""


def as_bytes_text(text: str) -> str:
    """The text with one character per byte (UTF-8 bytes as latin-1), the way the C parser sees it."""
    return text.encode("utf-8", errors="surrogateescape").decode("latin-1")


def tokens_of(text: str) -> list[str]:
    return [token for token in _SPACE.split(text) if token]


def _number(text: str, limit: int, what: str, minimum: int = 0) -> int:
    value = route_wait.c_number(text, limit)
    if value is None or value < minimum:
        raise NavError(f"bad number in {what}={text} (want 0..{limit} in C notation)")
    return value


# ---- names (percent encoding) ----


def decode_name(text: str) -> bytes:
    """`name:` text to bytes (`route_nav_percent_decode`). Raises NavError."""
    out = bytearray()
    index = 0
    while index < len(text):
        char = text[index]
        if char == "%":
            digits = text[index + 1 : index + 3]
            if len(digits) != 2 or not all(c in "0123456789abcdefABCDEF" for c in digits):
                raise NavError(f"bad percent escape near {text[index : index + 8]!r}")
            code = int(digits, 16)
            index += 3
        else:
            code = ord(char)
            index += 1
        if not 0x20 <= code <= 0x7E:
            raise NavError(f"the name has a byte {code:#04x} outside printable ASCII")
        if len(out) + 1 >= NAME_MAX + 1:
            raise NavError(f"the name is longer than {NAME_MAX} bytes")
        out.append(code)
    if not out:
        raise NavError("the name is empty")
    return bytes(out)


def encode_name(raw: bytes | str) -> str:
    """Bytes (or text, as UTF-8) to the `name:` form: printable ASCII except space, `%` and `=` as is, the rest `%XX`."""
    data = raw.encode("utf-8") if isinstance(raw, str) else raw
    return "".join(
        chr(byte) if 0x21 <= byte <= 0x7E and byte not in (0x25, 0x3D) else f"%{byte:02X}"
        for byte in data
    )


def names_match(wanted: bytes, label: bytes) -> bool:
    """Case blind (ASCII) equality, the way the host compares an item label with the wanted name."""

    def fold(data: bytes) -> bytes:
        return bytes(byte + 32 if 0x41 <= byte <= 0x5A else byte for byte in data)

    return fold(wanted) == fold(label)


# ---- nav lines ----


@dataclass(frozen=True)
class NavStep:
    at: int
    to: int
    menu: str
    select_kind: str  # index id name
    select_index: int = 0
    select_name: bytes = b""
    activate: bool = False
    expect: str | None = None
    timeout_ms: int | None = None  # None = the default
    retry: int | None = None

    @property
    def select_text(self) -> str:
        if self.select_kind == "index":
            return f"index:{self.select_index}"
        if self.select_kind == "id":
            return (
                f"id:{self.select_index}"
                if self.select_index < 0x10000
                else f"id:0x{self.select_index:X}"
            )
        return "name:" + encode_name(self.select_name)

    @property
    def effective_timeout_ms(self) -> int:
        return DEFAULT_TIMEOUT_MS if self.timeout_ms is None else self.timeout_ms

    @property
    def effective_retry(self) -> int:
        return DEFAULT_RETRY if self.retry is None else self.retry

    def format(self) -> str:
        """The canonical line; `parse_nav_line(step.format()) == step`."""
        parts = [NAV_PREFIX, f"at={self.at}", f"to={self.to}", f"menu={self.menu}"]
        parts.append(f"select={self.select_text}")
        if self.activate:
            parts.append("activate")
        if self.expect is not None:
            parts.append(f"expect={self.expect}")
        if self.timeout_ms is not None:
            parts.append(f"timeout={self.timeout_ms}")
        if self.retry is not None:
            parts.append(f"retry={self.retry}")
        return " ".join(parts)


def parse_nav_text(text: str) -> NavStep:
    """The text AFTER `# nav:` (`route_nav_line_parse`). Raises NavError with the reason (the host refuses the same lines)."""
    text = as_bytes_text(text)
    if len(text) > NAV_TEXT_LIMIT:
        raise NavError(f"the nav line is longer than {NAV_TEXT_LIMIT} characters")
    seen: dict[str, str] = {}
    for token in tokens_of(text):
        key, separator, value = token.partition("=")
        if token == "activate":
            key, separator = "activate", "="
        elif key not in NAV_KEYS or key == "activate" or not separator:
            raise NavError(f"unknown field {token[:40]!r}")
        if key in seen:
            raise NavError(f"'{key}' given twice")
        seen[key] = value
    for key in ("at", "to", "menu", "select"):
        if key not in seen:
            raise NavError(f"missing {key}=")
    at = _number(seen["at"], NAV_POLL_LIMIT, "at")
    to = _number(seen["to"], NAV_POLL_LIMIT, "to")
    if not MENU_ID_RE.fullmatch(seen["menu"]):
        raise NavError(f"bad menu id {seen['menu']!r} (1..31 of A-Za-z0-9_.-)")
    select = seen["select"]
    if len(select) >= SELECT_TEXT_LIMIT:
        raise NavError(f"select= is longer than {SELECT_TEXT_LIMIT - 1} characters")
    kind, index, name = "index", 0, b""
    if len(select) > 6 and select.startswith("index:"):
        index = _number(select[6:], INDEX_MAX, "select index")
    elif len(select) > 3 and select.startswith("id:"):
        kind, index = "id", _number(select[3:], 0xFFFFFFFF, "select id")
    elif len(select) > 5 and select.startswith("name:"):
        kind, name = "name", decode_name(select[5:])
    else:
        raise NavError(f"select={select}: want index:N, id:N or name:TEXT")
    expect = seen.get("expect")
    if expect is not None:
        if not 0 < len(expect) < EXPECT_LIMIT:
            raise NavError(f"expect= must be 1..{EXPECT_LIMIT - 1} characters")
        try:
            route_wait.parse_wait("mark1:" + expect, event=True)
        except route_wait.WaitError as error:
            raise NavError(f"expect={expect}: {error}") from None
    timeout = (
        _number(seen["timeout"], route_wait.TIMEOUT_MS_LIMIT, "timeout", 1)
        if "timeout" in seen
        else None
    )
    retry = _number(seen["retry"], RETRY_MAX, "retry") if "retry" in seen else None
    if at > to:
        raise NavError(f"at={at} is above to={to}")
    activate = "activate" in seen
    if expect is not None and not activate:
        raise NavError("expect= is the post-condition of the activation, give `activate` too")
    return NavStep(
        at,
        to,
        seen["menu"],
        kind,
        index,
        name,
        activate=activate,
        expect=expect,
        timeout_ms=timeout,
        retry=retry,
    )


def parse_nav_line(line: str) -> NavStep:
    """One whole `# nav:` record line to a NavStep."""
    if not line.startswith(NAV_PREFIX):
        raise NavError(f"not a nav line (want '{NAV_PREFIX} ...')")
    return parse_nav_text(line[len(NAV_PREFIX) :])


# ---- memory references and conditions ----


@dataclass(frozen=True)
class MemRef:
    """`[*]ADDR[+OFF][:W][&MASK]`, or with `page` an offset `@+OFF[:W][&MASK]` into the page widget of the menu (address = OFF)."""

    indirect: bool
    address: int
    offset: int = 0
    width: int = 4
    mask: int = 0xFFFFFFFF
    page: bool = False

    @property
    def key(self) -> tuple[bool, int, int, int]:
        """What a `--route-log-mem` entry identifies (the mask is applied when the value is read)."""
        return (self.indirect, self.address, self.offset, self.width)

    @property
    def samplable(self) -> bool:
        """A plain memory reference can be sampled by `--route-log-mem`, a page offset cannot (the slot of the page moves)."""
        return not self.page

    def spec(self) -> str:
        """The `--route-log-mem` argument (or the table spelling for a page offset)."""
        if self.page:
            text = f"@+0x{self.address:X}"
        else:
            text = ("*" if self.indirect else "") + f"0x{self.address:X}"
            if self.indirect and self.offset:
                text += f"+0x{self.offset:X}"
        if self.width != 4:
            text += f":{self.width}"
        return text

    def read(self, raw: int) -> int:
        return raw & self.mask


@dataclass(frozen=True)
class Cond:
    """One condition: a reference, a comparison, a value and a mask (the mask is the reference's)."""

    ref: MemRef
    cmp: str
    value: int

    @property
    def page(self) -> bool:
        return self.ref.page

    @property
    def mask(self) -> int:
        return self.ref.mask

    def format(self) -> str:
        prefix = "page=" if self.ref.page else "mem="
        return f"{prefix}{self.ref.spec()}{self.cmp}0x{self.value:X}"


def _page_text(text: str) -> str | None:
    """`@+OFF...` to `OFF...` (the page offset is parsed like an address), None when it is not a page spelling."""
    if not text.startswith("@+"):
        return None
    if text[2:3] in ("*", ""):
        raise NavError(f"{text!r}: a page offset is a plain number after '@+'")
    return text[2:]


def parse_mem_ref(text: str) -> MemRef:
    """`[*]ADDR[+OFF][:W][&MASK]` or `@+OFF[:W][&MASK]` (`xinput_route_mem_ref_parse`, which parses it as `==0`). Raises NavError."""
    text = as_bytes_text(text)
    body = _page_text(text)
    page = body is not None
    inner = body if body is not None else text
    if any(char in inner for char in "=<>!"):
        raise NavError(f"memory reference {text!r} must not hold a comparison")
    amp = inner.find("&")
    head = amp if amp != -1 else len(inner)
    tail = inner[amp:] if amp != -1 else ""
    if head == 0 or head + 3 + len(tail) >= 96:
        raise NavError(f"memory reference {text!r} is empty or too long")
    cond = route_wait._mem_event_condition(inner[:head] + "==0" + tail)
    if cond is None:
        raise NavError(
            f"memory reference {text!r} is not [*]ADDR[+OFF][:W][&MASK] or @+OFF[:W][&MASK]"
        )
    return MemRef(cond.indirect, cond.address, cond.offset, cond.width, cond.mask, page)


def parse_cond(text: str) -> Cond:
    """`[*]ADDR[+OFF][:W]OP VALUE[&MASK]` or `@+OFF[:W]OP VALUE[&MASK]` (`xinput_route_mem_cond_parse`). Raises NavError."""
    text = as_bytes_text(text)
    body = _page_text(text)
    cond = route_wait._mem_event_condition(body if body is not None else text)
    if cond is None:
        raise NavError(
            f"bad condition {text!r} (want [*]ADDR[+OFF][:W]OP VALUE[&MASK] or @+OFF[:W]OP VALUE[&MASK])"
        )
    ref = MemRef(cond.indirect, cond.address, cond.offset, cond.width, cond.mask, body is not None)
    return Cond(ref, cond.cmp, cond.value)


def parse_cond_list(text: str, key: str = "cond") -> tuple[Cond, ...]:
    items = text.split(",")
    if len(items) > route_wait.ROUTE_COND_MAX:
        raise NavError(f"{key}=: more than {route_wait.ROUTE_COND_MAX} conditions")
    if any(not item or len(item) >= 96 for item in items):
        raise NavError(f"{key}=: empty or too long condition")
    return tuple(parse_cond(item) for item in items)


def cond_ref(cond: Cond) -> MemRef:
    return cond.ref


def eval_cond(cond: Cond, value: int) -> bool:
    """`(value & mask) OP cond.value`, unsigned (`xinput_route_compare`)."""
    masked = value & cond.mask
    return {
        "==": masked == cond.value,
        "!=": masked != cond.value,
        "<": masked < cond.value,
        "<=": masked <= cond.value,
        ">": masked > cond.value,
        ">=": masked >= cond.value,
    }[cond.cmp]


# ---- the menu table ----


@dataclass(frozen=True)
class NamesSpec:
    ref: MemRef
    stride: int
    kind: str
    max_length: int


@dataclass(frozen=True)
class Items:
    """`items=walk(HEAD;next=+N;type=+N:T1|T2;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK)`: the rows of a menu as a linked list."""

    head: MemRef
    next_off: int
    type_off: int
    types: tuple[int, ...]
    id_off: int | None = None
    text_off: int | None = None
    flags_off: int | None = None
    skip_mask: int = 0
    grey_mask: int = 0


@dataclass(frozen=True)
class Widgets:
    """`widgets table=ADDR stride=N count=N [closing=S,S..]`: the table of UI page widgets the `page=` selection scans."""

    table: int
    stride: int
    count: int
    closing: tuple[int, ...] = ()
    line: int = 0


@dataclass(frozen=True)
class Menu:
    id: str
    active: tuple[Cond, ...]
    cursor: MemRef | None = None
    count: MemRef | None = None
    axis: str | None = None  # v or h, set whenever cursor= is
    wrap: bool = False
    ready: tuple[Cond, ...] = ()
    names: NamesSpec | None = None
    select: str = "A"
    conf: str | None = None
    line: int = 0
    page_builder: int | None = None  # page=builder:VA (None: page=none or no page= key)
    itemid: MemRef | None = None
    items: Items | None = None
    back: str | None = None
    dpad: bool = True  # dpad=0: the menu is stick/pointer driven, a nav step never presses the d-pad (only waits and activates)
    selectby: str = "id"  # what the RECORDER writes as select=: id (default), name or index

    @property
    def has_cursor(self) -> bool:
        return self.cursor is not None

    def refs(self, everything: bool = False) -> list[MemRef]:
        """The addresses a conversion needs sampled: the active conditions and the cursor (`everything`: count, itemid, ready too)."""
        found = [cond_ref(cond) for cond in self.active]
        if self.cursor is not None:
            found.append(self.cursor)
        if everything:
            found += [ref for ref in (self.count, self.itemid) if ref is not None]
            found += [cond_ref(cond) for cond in self.ready]
        return found

    def uses_page(self) -> bool:
        return (
            any(ref.page for ref in self.refs(everything=True))
            or (self.items is not None and self.items.head.page)
            or (self.names is not None and self.names.ref.page)
        )


class MenuTable(dict):
    """The menus of a table in file order (`dict` id -> Menu) and its `widgets` line."""

    widgets: Widgets | None = None


MENU_KEYS = (
    "page",
    "active",
    "cursor",
    "count",
    "axis",
    "wrap",
    "ready",
    "itemid",
    "items",
    "names",
    "select",
    "back",
    "conf",
    "dpad",
    "selectby",
)
SELECTBY = ("id", "name", "index")
WIDGET_KEYS = ("table", "stride", "count", "closing")
MAX_CLOSING = 4
MAX_TYPES = 4
WIDGET_STRIDE_MIN = 0x1B4  # the page widget's state dword (+0x1B0) must fit in a slot
WIDGET_STRIDE_LIMIT = 0x10000
WIDGET_COUNT_LIMIT = 256
ITEM_OFFSET_LIMIT = 0xFFFF
BUILDER_LIMIT = 0xFFFFFFFF


def _parse_names(text: str) -> NamesSpec:
    parts = text.split("/")
    if not text or len(text) >= 160 or len(parts) != 4:
        raise NavError("names=: want MEM/STRIDE/ascii|utf16|ptr-ascii|ptr-utf16/MAXLEN")
    stride = route_wait.c_number(parts[1], 4096)
    max_length = route_wait.c_number(parts[3], 63)
    if not stride or not max_length:
        raise NavError("names=: STRIDE is 1..4096 and MAXLEN 1..63")
    if parts[2] not in NAME_KINDS:
        raise NavError(f"names=: unknown encoding {parts[2]!r}")
    return NamesSpec(parse_mem_ref(parts[0]), stride, parts[2], max_length)


def _offset(text: str, what: str) -> int:
    """`+N` (C number) of an items= field."""
    value = route_wait.c_number(text[1:], ITEM_OFFSET_LIMIT) if text.startswith("+") else None
    if value is None:
        raise NavError(f"items=walk: {what}= wants +OFFSET (0..{ITEM_OFFSET_LIMIT:#x})")
    return value


def _parse_items(text: str) -> Items:
    """`walk(HEAD;next=+N;type=+N:T1|T2;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK)` exactly as `parse_items` of the C parser:
    a repeated key overrides (a repeated type= appends more types, 4 in all)."""
    if len(text) < 7 or len(text) >= 300 or not text.startswith("walk(") or not text.endswith(")"):
        raise NavError(
            "items=: want walk(HEAD;next=+N;type=+N:T1|T2;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK)"
        )
    fields = text[5:-1].split(";")
    head = parse_mem_ref(fields[0])
    next_off = type_off = None
    types: list[int] = []
    offsets: dict[str, int | None] = {"id": None, "text": None, "flags": None}
    masks = {"skip": 0, "grey": 0}
    for item in fields[1:]:
        key, separator, value = item.partition("=")
        if not separator:
            raise NavError(f"items=walk: {item[:30]!r} is not key=value")
        if key == "next":
            next_off = _offset(value, "next")
        elif key == "type":
            offset_text, colon, type_list = value.partition(":")
            if not colon:
                raise NavError(f"items=walk: bad type {value!r} (want +OFF:T1|T2)")
            type_off = _offset(offset_text, "type")
            for part in type_list.split("|"):
                number = route_wait.c_number(part, 0xFFFFFFFF)
                if len(types) >= MAX_TYPES or number is None:
                    raise NavError(f"items=walk: bad type list {value!r} (1..{MAX_TYPES} numbers)")
                types.append(number)
        elif key in offsets:
            offsets[key] = _offset(value, key)
        elif key in masks:
            number = route_wait.c_number(value, 0xFFFFFFFF)
            if number is None:
                raise NavError(f"items=walk: bad {key} {value!r}")
            masks[key] = number
        else:
            raise NavError(f"items=walk: unknown key {key[:30]!r}")
    if next_off is None or type_off is None or not types:
        raise NavError("items=walk: needs next= and type=")
    if (masks["skip"] or masks["grey"]) and offsets["flags"] is None:
        raise NavError("items=walk: needs flags= when skip= or grey= is given")
    return Items(
        head,
        next_off,
        type_off,
        tuple(types),
        offsets["id"],
        offsets["text"],
        offsets["flags"],
        masks["skip"],
        masks["grey"],
    )


def _button(text: str, key: str) -> str:
    if text not in SELECT_BUTTONS:
        raise NavError(f"{key}= wants A B X Y BLACK WHITE START or BACK")
    return text


def parse_widgets_line(tokens: list[str], number: int = 0) -> Widgets:
    """`widgets table=ADDR stride=N count=N [closing=S,S..]` (`parse_widgets_line`)."""
    seen: dict[str, str] = {}
    for token in tokens:
        key, separator, value = token.partition("=")
        if not separator or key not in WIDGET_KEYS or key in seen:
            raise NavError(f"widgets: bad, repeated or unknown field {token[:40]!r}")
        seen[key] = value
    table = route_wait.c_number(seen["table"], 0xFFFFFFFF) if "table" in seen else None
    stride = route_wait.c_number(seen["stride"], WIDGET_STRIDE_LIMIT) if "stride" in seen else None
    count = route_wait.c_number(seen["count"], WIDGET_COUNT_LIMIT) if "count" in seen else None
    if "table" in seen and not table:
        raise NavError(f"widgets: bad table={seen['table']!r} (an address)")
    if "stride" in seen and (stride is None or stride < WIDGET_STRIDE_MIN):
        raise NavError(
            f"widgets: bad stride={seen['stride']!r} ({WIDGET_STRIDE_MIN:#x}..{WIDGET_STRIDE_LIMIT:#x})"
        )
    if "count" in seen and not count:
        raise NavError(f"widgets: bad count={seen['count']!r} (1..{WIDGET_COUNT_LIMIT})")
    if table is None or stride is None or count is None:
        raise NavError("widgets needs table=, stride= and count=")
    closing: tuple[int, ...] = ()
    if "closing" in seen:
        parts = [route_wait.c_number(part, 0xFFFFFFFF) for part in seen["closing"].split(",")]
        if len(parts) > MAX_CLOSING or any(part is None for part in parts):
            raise NavError(f"widgets: closing= wants 1..{MAX_CLOSING} state numbers")
        closing = tuple(part for part in parts if part is not None)
    return Widgets(table, stride, count, closing, number)


def parse_menu_line(line: str, number: int = 0) -> Menu | Widgets | None:
    """One table line (`#` comment cut, as the host does): a Menu, the Widgets line, None for a blank line. Raises NavError."""
    tokens = tokens_of(as_bytes_text(line.split("#", 1)[0]))
    if not tokens:
        return None
    if tokens[0] == "widgets":
        return parse_widgets_line(tokens[1:], number)
    if tokens[0] != "menu" or len(tokens) < 2:
        raise NavError("expected `menu ID key=value ...` or `widgets key=value ...`")
    if not MENU_ID_RE.fullmatch(tokens[1]):
        raise NavError("bad menu id (1..31 of [A-Za-z0-9_.-])")
    seen: dict[str, str] = {}
    for token in tokens[2:]:
        key, separator, value = token.partition("=")
        if not separator:
            raise NavError(f"{token[:40]!r} is not key=value")
        if key not in MENU_KEYS:
            raise NavError(f"unknown key {key[:40]!r}")
        if key in seen:
            raise NavError(f"{key!r} given twice")
        seen[key] = value
    if "active" not in seen:
        raise NavError(f"menu {tokens[1]} is missing active=")
    if ("cursor" in seen) != ("count" in seen):
        raise NavError(f"menu {tokens[1]} needs cursor= and count= together")
    if "cursor" in seen and ("axis" not in seen or "wrap" not in seen):
        raise NavError(f"menu {tokens[1]} is missing {'axis' if 'axis' not in seen else 'wrap'}=")
    if "cursor" not in seen and any(
        key in seen for key in ("itemid", "items", "names", "dpad", "selectby")
    ):
        raise NavError(
            f"menu {tokens[1]} has itemid=, items=, names=, dpad= or selectby= without a cursor"
        )
    if "dpad" in seen and seen["dpad"] not in ("0", "1"):
        raise NavError("dpad= wants 0 or 1")
    if "selectby" in seen and seen["selectby"] not in SELECTBY:
        raise NavError("selectby= wants id, name or index")
    if "axis" in seen and seen["axis"] not in ("v", "h"):
        raise NavError("axis= wants v or h")
    if "wrap" in seen and seen["wrap"] not in ("0", "1"):
        raise NavError("wrap= wants 0 or 1")
    page_builder = None
    page_text = seen.get("page")
    if page_text is not None and page_text != "none":
        va = (
            route_wait.c_number(page_text[8:], BUILDER_LIMIT)
            if page_text.startswith("builder:")
            else None
        )
        if not va:
            raise NavError("page= wants builder:VA (a code address) or none")
        page_builder = va
    conf = seen.get("conf")
    if conf is not None and not 0 < len(conf) < 24:
        raise NavError("conf= wants 1..23 characters")
    menu = Menu(
        id=tokens[1],
        active=parse_cond_list(seen["active"], "active"),
        cursor=parse_mem_ref(seen["cursor"]) if "cursor" in seen else None,
        count=parse_mem_ref(seen["count"]) if "count" in seen else None,
        axis=seen.get("axis"),
        wrap=seen.get("wrap") == "1",
        ready=parse_cond_list(seen["ready"], "ready") if "ready" in seen else (),
        names=_parse_names(seen["names"]) if "names" in seen else None,
        select=_button(seen["select"], "select") if "select" in seen else "A",
        conf=conf,
        line=number,
        page_builder=page_builder,
        itemid=parse_mem_ref(seen["itemid"]) if "itemid" in seen else None,
        items=_parse_items(seen["items"]) if "items" in seen else None,
        back=_button(seen["back"], "back") if "back" in seen else None,
        dpad=seen.get("dpad") != "0",
        selectby=seen.get("selectby", "id"),
    )
    if menu.page_builder is None and menu.uses_page():
        raise NavError(f"menu {menu.id} uses @+OFFSET page specs but has no page=builder:VA")
    return menu


def parse_menu_table(text: str) -> MenuTable:
    """The whole table in file order (`route_nav_menus_parse`). Raises NavError 'line N: reason' on the first bad line."""
    menus = MenuTable()
    for number, raw in enumerate(text.split("\n"), 1):
        if not raw.split("#", 1)[0].strip(" \t\r\n"):
            continue
        if len(menus) >= MAX_MENUS and not raw.split("#", 1)[0].lstrip(" \t\r\n").startswith(
            "widgets"
        ):
            raise NavError(f"line {number}: more than {MAX_MENUS} menus")
        try:
            entry = parse_menu_line(raw, number)
        except (NavError, route_wait.WaitError) as error:
            raise NavError(f"line {number}: {error}") from None
        if isinstance(entry, Widgets):
            if menus.widgets is not None:
                raise NavError(
                    f"line {number}: second widgets line (the first is line {menus.widgets.line})"
                )
            menus.widgets = entry
            continue
        assert entry is not None
        if entry.id in menus:
            raise NavError(
                f"line {number}: menu {entry.id} is already defined on line {menus[entry.id].line}"
            )
        menus[entry.id] = entry
    needs_widgets = [menu.id for menu in menus.values() if menu.page_builder is not None]
    if needs_widgets and menus.widgets is None:
        raise NavError(
            f"menu {needs_widgets[0]} has page=builder:VA but the table has no widgets line"
        )
    return menus


def load_menu_table(path: Path) -> MenuTable:
    return parse_menu_table(path.read_bytes().decode("utf-8", errors="surrogateescape"))


# ---- validating a record ----


@dataclass
class Problem:
    line: int | None
    message: str

    def __str__(self) -> str:
        return f"line {self.line}: {self.message}" if self.line else self.message


@dataclass
class RecordNav:
    steps: list[tuple[int, NavStep]] = field(default_factory=list)  # (line number, step)
    bad: list[tuple[int, str]] = field(default_factory=list)  # (line number, reason)
    marks: list[int] = field(default_factory=list)
    polls: int = 0


def scan_record(text: str) -> RecordNav:
    """The nav lines (parsed), marks and poll total of a record text."""
    found = RecordNav()
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        if line.startswith(NAV_PREFIX):
            try:
                found.steps.append((number, parse_nav_line(line)))
            except NavError as error:
                found.bad.append((number, str(error)))
        elif line.startswith("# mark:"):
            match = re.fullmatch(r"# mark:\s*at=(\d+)\s*", line)
            if match:
                found.marks.append(int(match.group(1)))
        elif line and not line.startswith("#"):
            head = line.split(None, 1)[0]
            if head.isdigit():
                found.polls += int(head)
    return found


def validate_navs(text: str, menus: Mapping[str, Menu] | None) -> list[Problem]:
    """Problems the host would hit with the nav lines of a record. `menus` None = no table known (menu checks skipped)."""
    found = scan_record(text)
    problems = [Problem(number, reason) for number, reason in found.bad]
    total = len(found.steps) + len(found.bad)
    if total > MAX_NAV_LINES:
        problems.append(Problem(None, f"{total} nav lines, a route holds at most {MAX_NAV_LINES}"))
    previous: tuple[int, NavStep] | None = None
    for number, step in found.steps:
        if step.to > found.polls:
            problems.append(
                Problem(number, f"to={step.to} is past the record ({found.polls} polls)")
            )
        inside = [mark for mark in found.marks if step.at < mark < step.to]
        if inside:
            problems.append(
                Problem(
                    number,
                    f"mark at={inside[0]} lies inside the nav range (at={step.at}, to={step.to}), a mark may sit at either end only",
                )
            )
        if previous is not None and step.at < previous[1].to:
            problems.append(
                Problem(
                    number,
                    f"at={step.at} overlaps or precedes the nav step of line {previous[0]} (to={previous[1].to}), nav steps must be ascending",
                )
            )
        previous = (number, step)
        if menus is None:
            continue
        menu = menus.get(step.menu)
        if menu is None:
            known = ", ".join(menus) or "(none)"
            problems.append(
                Problem(number, f"unknown menu {step.menu!r} (the menu table has: {known})")
            )
        elif step.select_kind in ("name", "id") and menu.items is None:
            problems.append(
                Problem(
                    number,
                    f"select={step.select_kind}: on menu {step.menu}, which has no items= in the menu table (use index:N)",
                )
            )
    return problems


def validate_route(record_text: str, menus: Mapping[str, Menu] | None) -> list[str]:
    """`validate_navs` as one line per problem."""
    return [str(problem) for problem in validate_navs(record_text, menus)]


def has_nav_lines(record_text: str) -> bool:
    return any(line.startswith(NAV_PREFIX) for line in record_text.split("\n"))


# ---- launch glue (tools.play) ----


def parse_timing(text: str) -> tuple[int, int, int]:
    """`HOLD:GAP[:EXTRA]` polls, HOLD and GAP 1..120, EXTRA 0..120, default 2 (`route_nav_timing_parse`). Raises NavError."""
    hold_text, separator, rest = text.partition(":")
    gap_text, second, extra_text = rest.partition(":")
    hold = route_wait.c_number(hold_text, TIMING_LIMIT) if separator else None
    gap = route_wait.c_number(gap_text, TIMING_LIMIT) if separator else None
    extra = route_wait.c_number(extra_text, TIMING_LIMIT) if second else DEFAULT_LOST_EXTRA
    if not hold or not gap or extra is None:
        raise NavError(
            f"route nav timing {text!r}: want HOLD:GAP[:EXTRA], polls 1..{TIMING_LIMIT} (EXTRA 0..{TIMING_LIMIT})"
        )
    return hold, gap, extra


def nav_host_flags(
    record_text: str,
    mode: str | None,
    menus_file: Path | None,
    timing: str | None,
    opt_out: bool,
) -> tuple[list[str], list[str]]:
    """(host flags, notes) for a replay of a record with these nav settings.

    * a record with no nav lines and no explicit mode gets nothing (an old host keeps working);
    * `opt_out` (--no-route-nav) is `--route-nav off`;
    * otherwise `--route-nav MODE` (default on) and `--route-nav-menus FILE` (only when the file exists, else a note says the
      nav lines play as recorded).
    """
    notes: list[str] = []
    if opt_out:
        return ["--route-nav", "off"], notes
    if mode is None and not has_nav_lines(record_text):
        return [], notes
    flags = ["--route-nav", mode or "on"]
    if mode == "off":
        return flags, notes
    if menus_file is not None and menus_file.is_file():
        flags += ["--route-nav-menus", str(menus_file)]
    else:
        notes.append(
            f"the route has nav steps but the menu table {menus_file} does not exist: --route-nav off, the recorded presses play open loop"
        )
        return ["--route-nav", "off"], notes
    if timing is not None:
        parse_timing(timing)
        flags += ["--route-nav-timing", timing]
    return flags, notes


NAV_HOST_FLAGS = {
    "--route-nav": 1,
    "--route-nav-menus": 1,
    "--route-nav-timing": 1,
    "--route-nav-record": 1,
}


def nav_record_flags(
    menus_file: Path | None, opt_out: bool, passthrough: list[str]
) -> tuple[list[str], list[str]]:
    """(host flags, notes) for a RECORDING (`--record-input`): `--route-nav-menus FILE --route-nav-record on` so the host writes
    `# nav:` lines. Nothing with `--no-route-nav` or a missing table. An explicit `--route-nav-record on|off` on the command line
    (it reaches the host as a passthrough flag) wins: `off` adds nothing, `on` only adds the table, the flag is never repeated."""
    if opt_out:
        return [], []
    explicit = None
    if "--route-nav-record" in passthrough:
        at = passthrough.index("--route-nav-record")
        explicit = passthrough[at + 1] if at + 1 < len(passthrough) else None
    if explicit == "off":
        return [], []
    if menus_file is None or not menus_file.is_file():
        return [], [f"the menu table {menus_file} does not exist: this recording gets no nav lines"]
    flags = ["--route-nav-menus", str(menus_file)]
    if "--route-nav-record" not in passthrough:
        flags += ["--route-nav-record", "on"]
    return flags, []


def has_nav_flags(command: list[str]) -> bool:
    return any(token in NAV_HOST_FLAGS for token in command)


def strip_nav_flags(command: list[str]) -> list[str]:
    """The command without the host nav flags and their values (for a host that predates T1640)."""
    out: list[str] = []
    skip = 0
    for token in command:
        if skip:
            skip -= 1
        elif token in NAV_HOST_FLAGS:
            skip = NAV_HOST_FLAGS[token]
        else:
            out.append(token)
    return out


def host_supports_nav(host: Path) -> bool:
    """Does `HOST --help` show the T1640 marker (the way the fish scripts check T1618 / T1633)?"""
    try:
        result = subprocess.run(
            [str(host), "--help"], capture_output=True, text=True, timeout=60, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return HOST_MARKER in (result.stdout + result.stderr)
