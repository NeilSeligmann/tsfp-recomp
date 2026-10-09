# SPDX-License-Identifier: GPL-3.0-or-later
"""Parser for IDA FLIRT `.pat` pattern files.

A `.pat` file is the plain-text intermediate FLAIR's library parsers emit before
`sigmake` compresses it into a binary `.sig`. The format is public, line oriented
and trivially greppable, which is why this package reads `.pat` rather than `.sig`:
there is no undocumented container to reverse, only a grammar to get right.

THE FILE ITSELF IS NEVER COMMITTED. Pattern sets covering the Xbox XDK were
produced by running IDA's library parser over leaked Microsoft `.lib` archives.
`docs/provenance.md` permits them as a *local, uncommitted analysis aid* and
forbids them entering this repository in any form, including as a test fixture.
Every fixture under `tests/test_flirt.py` is therefore hand-invented. If you are
adding one, invent the bytes and invent the names.

GRAMMAR, one module per line::

    <leading> <crc_len> <crc16> <total_len> <names...> [<tail>]

  * `leading` -- the first 32 bytes of the function as 64 hex characters, with a
    `..` pair standing for a byte the linker may rewrite (a relocation, mostly).
    Shorter when the function itself is shorter than 32 bytes.
  * `crc_len` -- hex, how many bytes immediately *after* those 32 the CRC covers.
    `00` means the module carries no CRC, because byte 33 was variable.
  * `crc16` -- hex, the CRC over that region. See `crc16` for the algorithm, which
    is not the standard CCITT one.
  * `total_len` -- hex, the function's full length in bytes. Emitted as four hex
    digits by some producers and eight by others; any width parses.
  * `names` -- see below. At least one is required.
  * `tail` -- the function's remaining bytes from `32 + crc_len` onward, same hex
    and `..` encoding as `leading`. Absent when nothing is left over. FLAIR writes
    it as one token, but it is accepted split across whitespace.

A line consisting of `---` terminates the file. Blank lines and `#`/`;` comments
are skipped.

TWO NAME DIALECTS EXIST and both are accepted, because the pattern set that
actually matters here uses the second one. Measured over the 200,072 pattern lines
of the Dxbx-era XDK set: **not one `:offset` token appears anywhere.**

  1. *Canonical `sigmake`*: every name is an explicit `<marker><offset> <name>`
     group. `:` defines a name at that offset, `^` merely references one there.
     Offsets are hex and may be negative (`:-0004`) for a label before the start.
  2. *The dialect in the wild*: the module's own public name follows `total_len`
     directly, as a bare token with no marker and no offset, implicitly at offset
     0. Reference groups then follow in the form `^<4 hex><flag> <name>`, where the
     flag is a single letter recording how the reference was encoded -- measured
     `R` 383,948 times, `D` 203,170 and `S` 144, which reads as relative, direct
     and short. The flag is preserved on `PatternName.flag` and is not interpreted.

Dialect 1 is what the public documentation describes; dialect 2 is what the files
contain. Guessing between them is not necessary: a marker character is present or
it is not, and the first token after `total_len` is a name either way.

DELIBERATELY NOT SUPPORTED, and raising rather than being quietly dropped:

  * **Binary `.sig` files.** Only the text `.pat` side is read. A `.sig` v8+ header
    can declare a pattern length other than 32, which would move the CRC region;
    nothing here handles that, and nothing here needs to.
  * **`leading` longer than 32 bytes.** FLIRT's pattern length is fixed at 32; a
    longer field means the file is not a `.pat`, and guessing would silently
    mis-align every following field.
  * **Name flag characters.** FLAIR has historically used leading `?`/`@` sigils on
    a name to mark it local or collision-resolved. A `?` is *also* the first
    character of every MSVC-mangled C++ symbol -- 513,167 names in the measured set
    begin with one, 472,149 of which contain `@@` and so are certainly mangled --
    making a strip unsafe to decide from the text alone. Names are therefore kept
    verbatim and `NameKind` is derived only from the unambiguous marker.
  * **The unspaced name form, `:0000SomeName`.** A name always comes from its own
    whitespace-delimited token. The unspaced form cannot be parsed correctly at all:
    the offset has no terminator, and the first characters of a name are frequently
    hex digits, so `:0000Face` is equally readable as offset 0x0000FACE with an
    empty name. Rather than pick, this raises on the non-hex offset. The real files
    use the spaced form throughout.
  * **A flagged reference at an offset wider than four hex digits.** See
    `_parse_names`: `D` is a hex digit, so the flag is only separable where the
    offset width is known. Every reference group in the measured set is four digits.
  * **Alternative/continuation syntax beyond repeated whole lines.** Two modules
    sharing a leading pattern appear as two independent lines, and that is how
    ambiguity is detected. No within-line alternation is parsed, and measured, not
    one line in the real set begins with whitespace or carries a `++` or `(` token.

Everything here is pure except `parse_pattern_file`, which reads a file.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

#: Bytes of leading pattern a `.pat` line can carry. Fixed by FLIRT, not a tunable:
#: the CRC region is defined as starting at this offset into the function, so
#: changing it would silently relocate every CRC check.
PATTERN_BYTES = 32

#: Largest CRC region a `.pat` line can describe. The field is two hex digits.
MAX_CRC_LENGTH = 0xFF

#: Reflected form of the CRC-16/CCITT polynomial 0x1021. FLIRT shifts right, so the
#: polynomial is bit-reversed; see `crc16` for why this is not plain CCITT anyway.
CRC16_POLYNOMIAL = 0x8408

#: The line FLAIR writes to mark end-of-file.
TERMINATOR = "---"

#: Characters that introduce a comment line. `;` is what the real files use.
COMMENT_CHARS = "#;"

#: Reference-encoding flags seen in a `^<offset><flag>` group. A single character
#: from this set immediately after the offset is a flag, not a one-letter name.
REFERENCE_FLAGS = frozenset("DRS")

#: Byte value standing for "wildcard" inside a parsed pattern. `None` rather than a
#: sentinel integer so that a wildcard can never compare equal to real data.
WILDCARD = None

_WILDCARD_PAIR = ".."


class NameKind(StrEnum):
    """Which marker introduced a name on a `.pat` line."""

    PUBLIC = "public"
    """`:offset name` -- a symbol *defined* at that offset in this module."""

    REFERENCE = "reference"
    """`^offset name` -- a symbol *referenced* from that offset.

    A reference names something outside the module, typically an import thunk or a
    symbol in another object file. It is recorded because it is corroborating
    evidence about what the function does, but it must not be applied as the name
    of an address inside the matched function: the address holds a *use* of that
    symbol, not its definition.
    """


@dataclass(frozen=True)
class PatternName:
    """One `<marker><offset> <name>` group from a `.pat` line."""

    offset: int
    """Byte offset from the module's start. May be negative."""

    name: str
    """The symbol text exactly as written, mangling and sigils intact."""

    kind: NameKind

    flag: str = ""
    """The reference-encoding letter from a `^<offset><flag>` group, or ''.

    Recorded, not interpreted. The measured values are `R`, `D` and `S`, which read
    as relative, direct and short, but nothing here depends on that reading being
    right and no behaviour branches on it.
    """


@dataclass(frozen=True)
class Pattern:
    """One parsed `.pat` line: a byte signature plus the names it carries."""

    leading: tuple[int | None, ...]
    """Up to `PATTERN_BYTES` entries; `WILDCARD` where the byte is variable."""

    crc_length: int
    """Bytes after `PATTERN_BYTES` the CRC covers. 0 means there is no CRC."""

    crc16: int
    """Expected CRC over that region. Meaningless when `crc_length` is 0."""

    total_length: int
    """The function's full byte length, as the library parser measured it."""

    names: tuple[PatternName, ...]
    tail: tuple[int | None, ...]
    """Bytes from `PATTERN_BYTES + crc_length` onward, same encoding as `leading`."""

    library: str
    """Where this line came from, normally the `.pat` file's stem. Provenance for a
    proposal, and the only thing distinguishing two otherwise identical lines."""

    source_line: int
    """1-based line number within `library`, so a disagreement can be looked up."""

    @property
    def entry_name(self) -> str | None:
        """The public name defined at offset 0, if the line declares one.

        This is the only name safely applicable to the *matched address*. Names at
        other offsets describe other addresses and are exposed through `names`.
        """
        for name in self.names:
            if name.kind is NameKind.PUBLIC and name.offset == 0:
                return name.name
        return None

    @property
    def concrete_prefix(self) -> bytes:
        """The longest run of non-wildcard bytes at the start of `leading`.

        Empty when the very first byte is a wildcard, which makes the line
        unindexable by prefix and so a candidate at every address.
        """
        prefix = bytearray()
        for byte in self.leading:
            if byte is WILDCARD:
                break
            prefix.append(byte)
        return bytes(prefix)

    @property
    def significant_bytes(self) -> int:
        """Non-wildcard bytes across `leading` and `tail`, plus the CRC region.

        A crude strength measure: a line that constrains three bytes is far weaker
        evidence than one constraining forty, regardless of either matching.
        """
        concrete = sum(1 for byte in self.leading if byte is not WILDCARD)
        concrete += sum(1 for byte in self.tail if byte is not WILDCARD)
        return concrete + self.crc_length


def crc16(data: bytes) -> int:
    """The CRC-16 variant FLIRT stores in a `.pat` line's CRC field.

    ESTABLISHED, not guessed. It is **byte-swapped CRC-16/X-25**:

      ===============  ====================================================
      polynomial       0x8408, the reflected form of 0x1021
      bit order        reflected, LSB first, right-shifting register
      initial value    0xFFFF
      final xor        0xFFFF, i.e. a one's complement
      post-processing  **the two result bytes are swapped**
      empty input      returns 0x0000, not the initial value
      length folding   none; the length is not mixed in anywhere
      ===============  ====================================================

    NOT standard CRC-16/CCITT. On the catalogue check string `b"123456789"`,
    CCITT-FALSE gives 0x29B1 and KERMIT 0x2189, while this gives 0x6E90. That
    0x6E90 is the independent anchor: the CRC RevEng catalogue's published check
    value for CRC-16/X-25 is 0x906E, and byte-swapping it is exactly what the final
    step does. `tests/test_flirt.py` pins it, so a future "simplification" to a
    stock CRC library fails loudly instead of silently halving the match rate.

    The byte swap and the final complement are the two steps easy to drop, and
    dropping either leaves a function that still looks plausible -- it produces
    16 bits that vary with the input -- while agreeing with FLIRT on nothing.

    THE REGION IT IS APPLIED TO IS THE OTHER TRAP, and it is not this function's
    business but belongs next to it: the CRC covers `crc_length` bytes starting at
    `PATTERN_BYTES` (32) from the function's start, **not** immediately after the
    line's leading field. Those differ whenever the leading field is shorter than
    32 bytes, which happens; such a field is logically wildcard-padded out to 32
    and the CRC still begins at 32. `tools/flirt/match.py` uses `PATTERN_BYTES`
    for exactly this reason.
    """
    if not data:
        return 0
    crc = 0xFFFF
    for byte in data:
        value = byte
        for _ in range(8):
            if (crc ^ value) & 1:
                crc = (crc >> 1) ^ CRC16_POLYNOMIAL
            else:
                crc >>= 1
            value >>= 1
    crc = ~crc & 0xFFFF
    return ((crc << 8) | (crc >> 8)) & 0xFFFF


def parse_hex_bytes(token: str, where: str) -> tuple[int | None, ...]:
    """Decode a `.pat` byte field: hex pairs, with `..` for a wildcard byte.

    Raises `ValueError` naming `where` for an odd number of characters or any pair
    that is neither `..` nor two hex digits. A half-wildcard such as `.A` is
    rejected: FLIRT wildcards whole bytes, and accepting it would mean inventing a
    nibble-level semantics the format does not have.
    """
    if len(token) % 2 != 0:
        raise ValueError(f"{where}: byte field has an odd character count: {token!r}")
    decoded: list[int | None] = []
    for index in range(0, len(token), 2):
        pair = token[index : index + 2]
        if pair == _WILDCARD_PAIR:
            decoded.append(WILDCARD)
            continue
        try:
            decoded.append(int(pair, 16))
        except ValueError:
            raise ValueError(
                f"{where}: byte field has a bad hex pair {pair!r} in {token!r}"
            ) from None
    return tuple(decoded)


def parse_pattern_line(line: str, *, library: str, source_line: int) -> Pattern | None:
    """Parse one `.pat` line.

    Returns `None` for a blank line, a `#` comment, or the `---` terminator, and a
    `Pattern` otherwise. Anything else that does not parse raises `ValueError`
    naming the library, the line number and the specific field at fault.

    IT RAISES, IT DOES NOT SKIP. A matcher that silently drops lines it cannot read
    reports a smaller unused-pattern count and a smaller match count, and both
    errors point the same way -- towards looking better than it is. The caller
    decides what to do with a bad line; `tools/flirt/cli.py` counts and reports
    them, which is a different thing from ignoring them.
    """
    where = f"{library}:{source_line}"
    stripped = line.strip()
    if not stripped or stripped[0] in COMMENT_CHARS or stripped == TERMINATOR:
        return None

    fields = stripped.split()
    if len(fields) < 4:
        raise ValueError(
            f"{where}: expected at least 4 fields "
            f"(pattern, crc_len, crc16, total_len), got {len(fields)}: {stripped!r}"
        )

    leading = parse_hex_bytes(fields[0], f"{where}: leading pattern")
    if len(leading) > PATTERN_BYTES:
        raise ValueError(
            f"{where}: leading pattern is {len(leading)} bytes, "
            f"FLIRT patterns are at most {PATTERN_BYTES}"
        )

    crc_length = _parse_hex_field(fields[1], "crc_len", where)
    if crc_length > MAX_CRC_LENGTH:
        raise ValueError(f"{where}: crc_len {crc_length} exceeds the maximum {MAX_CRC_LENGTH}")
    stored_crc = _parse_hex_field(fields[2], "crc16", where)
    if stored_crc > 0xFFFF:
        raise ValueError(f"{where}: crc16 {stored_crc:#x} does not fit in 16 bits")
    total_length = _parse_hex_field(fields[3], "total_len", where)

    names, rest = _parse_names(fields[4:], where)
    if not names:
        raise ValueError(f"{where}: no name field after total_len: {stripped!r}")

    tail = parse_hex_bytes("".join(rest), f"{where}: tail bytes") if rest else ()

    return Pattern(
        leading=leading,
        crc_length=crc_length,
        crc16=stored_crc,
        total_length=total_length,
        names=names,
        tail=tail,
        library=library,
        source_line=source_line,
    )


def parse_pattern_text(text: str, *, library: str) -> list[Pattern]:
    """Parse a whole `.pat` document. Line order is preserved."""
    patterns: list[Pattern] = []
    for number, line in enumerate(text.splitlines(), start=1):
        pattern = parse_pattern_line(line, library=library, source_line=number)
        if pattern is not None:
            patterns.append(pattern)
    return patterns


def parse_pattern_file(path: Path, *, library: str | None = None) -> list[Pattern]:
    """Parse a `.pat` file. `library` defaults to the file's stem.

    Decoded as latin-1 rather than UTF-8 on purpose: the names are mangled C and
    C++ identifiers that should be ASCII, but a stray high byte in a third-party
    file must not turn into a decode error halfway through an otherwise fine
    pattern set.
    """
    label = path.stem if library is None else library
    return parse_pattern_text(path.read_text(encoding="latin-1"), library=label)


def _parse_hex_field(token: str, column: str, where: str) -> int:
    """A `.pat` numeric field, which is always unsigned hex without a prefix."""
    try:
        return int(token, 16)
    except ValueError:
        raise ValueError(f"{where}: {column} is not hex: {token!r}") from None


def _parse_names(fields: list[str], where: str) -> tuple[tuple[PatternName, ...], list[str]]:
    """Split the trailing fields into name groups and whatever tail bytes follow.

    Handles both dialects described in the module docstring, and WHICH ONE A LINE IS
    IN IS DECIDED BY ITS FIRST FIELD, not guessed per group. An unmarked first token
    is the module's own public name at offset 0 and puts the line in the real-world
    dialect; a `:`-marked first token puts it in the canonical one.

    That decision has to be made per line, because a flag letter is not separable
    from an offset in isolation: `D` IS A HEX DIGIT, so `^0018D` reads equally well
    as "offset 0x18, flag D" and as "offset 0x18D". The dialect settles it -- flags
    occur only in the real-world dialect, where the offset is always four digits
    wide. A real-world reference at an offset needing five or more digits, i.e. in a
    function over 64 KiB, would be misread; the measured set has every one of its
    587,262 reference groups exactly six characters long, so none exists.

    Returns (names, leftover fields). The leftover is the tail-byte field, if any.
    """
    markers = {":": NameKind.PUBLIC, "^": NameKind.REFERENCE}
    names: list[PatternName] = []
    index = 0
    flagged_dialect = bool(fields) and fields[0][:1] not in markers

    if flagged_dialect:
        names.append(PatternName(offset=0, name=fields[0], kind=NameKind.PUBLIC))
        index = 1

    while index < len(fields):
        field = fields[index]
        kind = markers.get(field[:1])
        if kind is None:
            break
        index += 1
        offset_text, flag = _split_flag(field[1:], flagged_dialect)
        if index >= len(fields):
            raise ValueError(f"{where}: name group {field!r} has no name after it")
        names.append(
            PatternName(
                offset=_parse_signed_hex(offset_text, where, field),
                name=fields[index],
                kind=kind,
                flag=flag,
            )
        )
        index += 1
    return tuple(names), fields[index:]


#: Offset field width in the flagged dialect, where a flag letter may follow it.
FLAGGED_OFFSET_DIGITS = 4


def _split_flag(body: str, flagged_dialect: bool) -> tuple[str, str]:
    """Split a group body into (offset text, flag). The flag is '' unless present."""
    if (
        flagged_dialect
        and len(body) == FLAGGED_OFFSET_DIGITS + 1
        and body[FLAGGED_OFFSET_DIGITS] in REFERENCE_FLAGS
        and _is_hex(body[:FLAGGED_OFFSET_DIGITS])
    ):
        return body[:FLAGGED_OFFSET_DIGITS], body[FLAGGED_OFFSET_DIGITS]
    return body, ""


def _is_hex(text: str) -> bool:
    return bool(text) and all(character in "0123456789abcdefABCDEF" for character in text)


def _parse_signed_hex(token: str, where: str, field: str) -> int:
    """A name group's offset. Hex, optionally negative for a label before the start."""
    negative = token.startswith("-")
    digits = token[1:] if negative else token
    if not digits:
        raise ValueError(f"{where}: name group {field!r} has no offset")
    try:
        value = int(digits, 16)
    except ValueError:
        raise ValueError(f"{where}: name group {field!r} has a non-hex offset") from None
    return -value if negative else value
