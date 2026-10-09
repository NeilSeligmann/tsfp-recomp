# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-build code comparison.

Two builds of TimeSplitters: Future Perfect exist -- retail `default.xbe` and the
OXM 46 demo `tsdemo_cd.xbe` -- compiled from substantially the same engine eight
weeks apart, and linked at different addresses. That makes them independent
*external* evidence about where functions begin and end, which is the open problem
in our decompiled output: 12,343 functions recovered against ~13,461 predicted.

The obstacle is that raw bytes barely match: measured 15.1% of 32-byte windows,
because `call rel32` displacements and absolute address immediates differ purely
from layout. Blanking those by a crude byte scan raised it to 39.0%. This package
does it properly, decoding instruction lengths so operands are masked at their real
positions rather than wherever a byte happened to look like an address.
"""

from tools.codediff.boundaries import (
    Candidate,
    Correspondence,
    Function,
    correspond,
    count_call_sites,
    filter_plausible,
    load_functions,
    preceded_by_terminator,
)
from tools.codediff.match import MatchedRun, find_runs
from tools.codediff.normalise import Insn, NormalisedText, normalise_text

__all__ = [
    "Candidate",
    "Correspondence",
    "Function",
    "Insn",
    "MatchedRun",
    "NormalisedText",
    "correspond",
    "count_call_sites",
    "filter_plausible",
    "find_runs",
    "load_functions",
    "normalise_text",
    "preceded_by_terminator",
]
