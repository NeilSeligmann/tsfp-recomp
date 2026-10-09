# SPDX-License-Identifier: GPL-3.0-or-later
"""FLIRT pattern matching for XDK library functions in the retail XBE.

Only 5.3% of the 12,343 functions our analysis recovered carry any name at all.
The `.XTLID` section gives 294 of them exactly, by a clean and independent route
(`tools/xtlid.py`), and that is the whole of what we have. IDA FLIRT pattern sets
covering XDK 5849 -- our exact build -- describe tens of thousands of library
functions, which makes them by far the largest untapped naming source.

THEY ARE ALSO THE DIRTIEST ONE, and the licensing shapes this package's design.
Those pattern sets were produced by running IDA's library parser over leaked
Microsoft `.lib` archives. Per `docs/provenance.md` they may be used as a *local,
uncommitted analysis aid* and may never enter this repository: not as a file, not
as a test fixture, not inlined in a docstring. What is committed is the parser and
the matcher in this package, and tests built from patterns and names invented for
the purpose. `.gitignore` blocks `*.pat`, `*.sig`, `/flirt/` and `/generated/`.

WHAT MAKES THE OUTPUT TRUSTWORTHY IS `.XTLID`, not the matcher. The 294 addresses
`.XTLID` already names are a held-out ground truth the matcher never sees, so the
agreement rate on them is a direct measurement of the false-positive rate on the
addresses it does not. `tools/flirt/cli.py` recomputes that rate on every run and
prints it before anything else. A matcher that contradicts `.XTLID` on the cases
we can check cannot be believed on the cases we cannot, and no number of plausible
proposals offsets that.
"""

from tools.flirt.match import (
    MIN_SIGNIFICANT_BYTES,
    PREFIX_TIERS,
    EntryMatch,
    LengthCheck,
    MatchReport,
    PatternIndex,
    match_entries,
    matches_at,
)
from tools.flirt.pattern import (
    CRC16_POLYNOMIAL,
    MAX_CRC_LENGTH,
    PATTERN_BYTES,
    WILDCARD,
    NameKind,
    Pattern,
    PatternName,
    crc16,
    parse_hex_bytes,
    parse_pattern_file,
    parse_pattern_line,
    parse_pattern_text,
)

__all__ = [
    "CRC16_POLYNOMIAL",
    "MAX_CRC_LENGTH",
    "MIN_SIGNIFICANT_BYTES",
    "PATTERN_BYTES",
    "PREFIX_TIERS",
    "WILDCARD",
    "EntryMatch",
    "LengthCheck",
    "MatchReport",
    "NameKind",
    "Pattern",
    "PatternIndex",
    "PatternName",
    "crc16",
    "match_entries",
    "matches_at",
    "parse_hex_bytes",
    "parse_pattern_file",
    "parse_pattern_line",
    "parse_pattern_text",
]
