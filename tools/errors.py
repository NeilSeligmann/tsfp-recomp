# SPDX-License-Identifier: GPL-3.0-or-later
"""Error types shared by every binary-format parser.

Parsers never trust their input. Every offset and length is checked against the
buffer before use, so malformed data raises a typed error rather than producing
a silently wrong answer.
"""


class ParseError(Exception):
    """Base class for all binary-format parse failures."""


class TruncatedError(ParseError):
    """A structure referenced bytes beyond the end of the buffer."""


class BadMagicError(ParseError):
    """A structure did not begin with its expected magic bytes."""


def check_range(buf: bytes, offset: int, length: int, what: str) -> None:
    """Raise TruncatedError unless buf[offset:offset + length] is fully in bounds."""
    if offset < 0 or length < 0 or offset + length > len(buf):
        raise TruncatedError(
            f"{what}: wanted {length} bytes at offset {offset}, buffer is {len(buf)} bytes"
        )
