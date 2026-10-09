# SPDX-License-Identifier: GPL-3.0-or-later
"""Free Radical PAK container reading."""

from tools.pak.model import PakArchive, PakEntry
from tools.pak.reader import (
    MAGIC_HASHED,
    MAGIC_NAMETABLE,
    MAGIC_PLAINTEXT,
    STRIDE_INLINE_NAME,
    STRIDE_SHORT,
    parse_pak,
)

__all__ = [
    "MAGIC_HASHED",
    "MAGIC_NAMETABLE",
    "MAGIC_PLAINTEXT",
    "STRIDE_INLINE_NAME",
    "STRIDE_SHORT",
    "PakArchive",
    "PakEntry",
    "parse_pak",
]
