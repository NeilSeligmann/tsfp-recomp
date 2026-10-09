# SPDX-License-Identifier: GPL-3.0-or-later
"""Name x86 functions by aligning ordered call sequences against the donor's link order.

THE IDEA. `tools/gen_donor_symbols.py` measured that the TS2 donor's lifecycle
suffix groups (`<sys>Make`, `<sys>End`, `<sys>Reset`, ...) are all the *same*
subsystem sequence restricted to their own members: order concordance is 100% across
all 15 suffix-group pairs, because the linker emits each translation unit together
and in one order. If a TSFP dispatcher calls its subsystems in that same relative
order, naming reduces to aligning two ordered lists, and because several suffix
groups give independent views of the same order the alignment is self-checking.

WHAT IS AND IS NOT AVAILABLE. The donor is MIPS (PS2) and the target x86 (Xbox), so
byte- or instruction-level matching is impossible and `tools/codediff` does not
apply. The donor side is `.mdebug` stabs: names, addresses, sizes, source files. It
contains **no call graph at all**, so "align two call sequences" is a misnomer. What
is actually aligned is:

  * donor side -- an ordered list of subsystem *names* (link order, per suffix group)
  * target side -- an ordered list of `call rel32` *targets* from one x86 function

so the only content-free structure shared by both sides is ordinal position, plus one
weak numeric signal: function size, which survives a change of ISA as a rank rather
than as a value.

THE PARTS.

  * `sequences` -- decode a target function's ordered `call rel32` targets (capstone)
  * `dispatcher` -- rank target functions by how much they look like a lifecycle
    dispatcher, since the technique needs one before it can start
  * `donor` -- read `gen_donor_symbols.py` output into per-suffix ordered name lists
  * `align` -- Needleman-Wunsch with affine gaps, over an explicit scoring model
  * `crosscheck` -- the self-check: order concordance *within* the target's own
    address space, and *between* independent suffix groups
  * `propose` -- confidence per proposed name, and the proposals CSV
  * `cli` -- the end-to-end run

HONEST STATUS. See `verdict.md`-style notes in `cli.py`'s epilog and the module
docstrings. The cross-checks here exist to be able to *fail*, and on the real retail
binary they do. Nothing in this package should be used to apply names without reading
what `crosscheck` reports first.
"""

from tools.seqalign.align import (
    AlignedPair,
    Alignment,
    ScoringModel,
    align_sequences,
)
from tools.seqalign.crosscheck import (
    Colocality,
    ConcordanceResult,
    CrossCheck,
    GroupAssignment,
    address_order_concordance,
    colocality,
    cross_group_concordance,
    parallel_dispatchers,
    run_cross_checks,
)
from tools.seqalign.dispatcher import (
    DispatcherCandidate,
    PremiseTest,
    measure_premise,
    rank_dispatchers,
    survey_premise,
)
from tools.seqalign.donor import DonorOrder, load_donor_order
from tools.seqalign.pipeline import GroupInput, align_group, build_group_input
from tools.seqalign.propose import (
    KnownNameCheck,
    Proposal,
    check_known_names,
    propose_names,
    write_proposals,
)
from tools.seqalign.sequences import (
    CallSequence,
    extract_call_sequence,
    extract_call_sequences,
    first_occurrences,
)

__all__ = [
    "AlignedPair",
    "Alignment",
    "CallSequence",
    "Colocality",
    "ConcordanceResult",
    "CrossCheck",
    "DispatcherCandidate",
    "DonorOrder",
    "GroupAssignment",
    "GroupInput",
    "KnownNameCheck",
    "PremiseTest",
    "Proposal",
    "ScoringModel",
    "address_order_concordance",
    "align_group",
    "align_sequences",
    "build_group_input",
    "check_known_names",
    "colocality",
    "cross_group_concordance",
    "extract_call_sequence",
    "extract_call_sequences",
    "first_occurrences",
    "load_donor_order",
    "measure_premise",
    "parallel_dispatchers",
    "propose_names",
    "rank_dispatchers",
    "run_cross_checks",
    "survey_premise",
    "write_proposals",
]
