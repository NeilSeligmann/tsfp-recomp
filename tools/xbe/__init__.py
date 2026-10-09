# SPDX-License-Identifier: GPL-3.0-or-later
"""Xbox executable (XBE) parsing."""

from tools.xbe.model import Xbe, XbeCertificate, XbeSection, XtlidEntry
from tools.xbe.parser import (
    ENTRY_XOR_DEBUG,
    ENTRY_XOR_RETAIL,
    THUNK_XOR_DEBUG,
    THUNK_XOR_RETAIL,
    parse_xbe,
    parse_xtlid,
)

__all__ = [
    "ENTRY_XOR_DEBUG",
    "ENTRY_XOR_RETAIL",
    "THUNK_XOR_DEBUG",
    "THUNK_XOR_RETAIL",
    "Xbe",
    "XbeCertificate",
    "XbeSection",
    "XtlidEntry",
    "parse_xbe",
    "parse_xtlid",
]
