# SPDX-License-Identifier: GPL-3.0-or-later
"""The donor side of the alignment: ordered subsystem names, and nothing more.

WHAT THE DONOR ACTUALLY CONTAINS, because assuming otherwise wastes a lot of work.
`tools/gen_donor_symbols.py` parses `.mdebug` stabs via `tools/stabs.py`. Per function
that yields a name, an address, a size, a signature, a source file and whether the
parameters were named. Stabs describe *declarations*, not code: there is **no call
graph, no basic blocks, no instruction stream, no caller/callee relation**. So the
donor cannot supply a call sequence, and the symmetric phrase "align two call
sequences" is wrong. The donor supplies:

  * `lifecycle_link_order[suffix]` -- the subsystem prefixes carrying that suffix,
    in donor address order, i.e. link order
  * `lifecycle[prefix][suffix]` -- the donor address of each lifecycle function
  * `functions[*].size` -- a size per donor function, in MIPS bytes

That is the whole donor contribution. The measured invariant that makes it usable is
that the per-suffix orders are mutually order-concordant (100%, all 15 pairs), so each
suffix group is an independent *view* of one underlying subsystem order.

SIZES ARE MIPS BYTES AND THE TARGET IS X86. A MIPS function is typically larger in
bytes than the same C compiled for x86-32 -- fixed 4-byte instructions, no complex
addressing modes, explicit load/store for everything. So an absolute size comparison
is meaningless. `ScoringModel` therefore uses the *ratio* of sizes against a
per-alignment median ratio, which cancels the platform constant and leaves only
relative deviation. That is a weak signal and is weighted as one.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from tools.gen_donor_symbols import LIFECYCLE_SUFFIXES, order_concordance

if TYPE_CHECKING:
    from collections.abc import Sequence

#: Suffix groups with fewer members than this cannot support an alignment: with one or
#: two subsystems every ordering is trivially concordant, so the self-check degenerates
#: to "a comparison of nothing", the exact failure `order_concordance`'s docstring
#: warns about.
MIN_GROUP_SIZE = 4


@dataclass(frozen=True)
class DonorOrder:
    """Per-suffix ordered subsystem lists, plus the per-function sizes behind them."""

    donor: str
    """The donor build description recorded by `gen_donor_symbols.py`."""

    link_order: dict[str, tuple[str, ...]]
    """`suffix -> subsystem prefixes in donor link order`."""

    sizes: dict[tuple[str, str], int]
    """`(prefix, suffix) -> donor function size in bytes`."""

    def groups(self, *, min_size: int = MIN_GROUP_SIZE) -> list[str]:
        """Suffixes big enough to align and cross-check, largest group first.

        Ties break on the suffix name so the order is deterministic, which matters
        because the group order decides which group anchors a cross-check report.
        """
        usable = [suffix for suffix, order in self.link_order.items() if len(order) >= min_size]
        return sorted(usable, key=lambda suffix: (-len(self.link_order[suffix]), suffix))

    def names(self, suffix: str) -> tuple[str, ...]:
        """The full function names for a suffix group, in link order.

        `<prefix><suffix>` is how the donor spells them and how a TSFP call graph would
        show them, per `lifecycle_entries`' own note that the prefix is not always the
        source directory.
        """
        return tuple(prefix + suffix for prefix in self.link_order[suffix])

    def size_of(self, prefix: str, suffix: str) -> int | None:
        return self.sizes.get((prefix, suffix))

    def concordance(self, left: str, right: str) -> tuple[int, int]:
        """`(agreeing, total)` ordered pairs shared by two suffix groups.

        Reuses `gen_donor_symbols.order_concordance` rather than reimplementing it, so
        the number this package quotes is the same number that established the
        invariant in the first place.
        """
        return order_concordance(self.link_order[left], self.link_order[right])


def load_donor_order(path: Path) -> DonorOrder:
    """Read `tools/gen_donor_symbols.py --out` JSON into a `DonorOrder`.

    Raises `ValueError` naming the missing key rather than returning a half-populated
    object, because a donor payload without `lifecycle_link_order` predates the
    invariant and every statistic downstream would be computed over nothing.
    """
    payload = json.loads(path.read_text(encoding="utf-8"))
    return donor_order_from_payload(payload, source=str(path))


def donor_order_from_payload(payload: dict[str, object], *, source: str) -> DonorOrder:
    """`DonorOrder` from an already-parsed `gen_donor_symbols.py` payload."""
    raw_order = payload.get("lifecycle_link_order")
    if not isinstance(raw_order, dict):
        raise ValueError(f"{source}: missing 'lifecycle_link_order'; regenerate the donor payload")
    link_order: dict[str, tuple[str, ...]] = {}
    for suffix, prefixes in raw_order.items():
        if suffix not in LIFECYCLE_SUFFIXES:
            raise ValueError(f"{source}: unknown lifecycle suffix {suffix!r}")
        if not isinstance(prefixes, list):
            raise ValueError(f"{source}: lifecycle_link_order[{suffix!r}] is not a list")
        link_order[suffix] = tuple(str(prefix) for prefix in prefixes)

    sizes: dict[tuple[str, str], int] = {}
    raw_functions = payload.get("functions")
    if isinstance(raw_functions, list):
        by_name = {
            str(record.get("name")): int(record.get("size") or 0)
            for record in raw_functions
            if isinstance(record, dict)
        }
        for suffix, prefixes in link_order.items():
            for prefix in prefixes:
                size = by_name.get(prefix + suffix)
                if size:
                    sizes[(prefix, suffix)] = size

    donor = payload.get("donor")
    return DonorOrder(
        donor=str(donor) if donor is not None else "unknown donor",
        link_order=link_order,
        sizes=sizes,
    )


def shared_subsystems(order: DonorOrder, groups: Sequence[str]) -> list[str]:
    """Subsystems present in every one of `groups`, in the first group's link order.

    These are the only subsystems a cross-check between those groups can say anything
    about, and reporting their count alongside any agreement rate is what stops the
    rate being quoted over a handful of items.
    """
    if not groups:
        return []
    first = order.link_order[groups[0]]
    rest = [set(order.link_order[group]) for group in groups[1:]]
    return [prefix for prefix in first if all(prefix in other for other in rest)]
