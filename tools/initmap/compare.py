# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the static walk against a trace the host recorded while running the title.

The host prints "HLE calls reached, in call order" with, for each call, the thread, the
kernel ordinal or XDK address, and the guest return address ("from 0x..."). A static hit
carries the same return address, so the two are keyed identically. That makes the walk
falsifiable: how many of the sites the title really reached did the walk find, and in the
sites it did find, is the walk's order the title's order.

NULL MODEL FOR THE ORDER. Under a random permutation each pair of sites is in the right
order half the time, so the pairwise concordance of an order-free walk is 0.5. The figure
reported is only meaningful as the distance above that.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from tools.initmap.walk import CERTAIN, HIT_KERNEL, HIT_XDK, Walk

_ORDINAL_LINE = re.compile(
    r"^\s*(\d+)\s+t(\d+)\s+ordinal\s+(\d+)\s+\S+\s+\S+\s+eax=0x[0-9a-fA-F]+\s+from 0x([0-9A-Fa-f]+)"
)
_XDK_LINE = re.compile(r"^\s*(\d+)\s+t(\d+)\s+xdk\s+0x([0-9A-Fa-f]+)\s+.*from 0x([0-9A-Fa-f]+)")


@dataclass(frozen=True)
class RuntimeCall:
    sequence: int
    thread: int
    kind: str
    value: int
    return_va: int


@dataclass(frozen=True)
class Comparison:
    runtime_calls: int
    runtime_sites: int
    found_sites: int
    #: Sites the title reached that the walk did not, with their runtime sequence number.
    missed: tuple[tuple[int, str, int, int], ...]
    pairs: int
    concordant_pairs: int
    #: The same agreement restricted to sites the walk labelled `always`.
    certain_pairs: int = 0
    certain_concordant_pairs: int = 0
    #: Runtime sites matched only through a tail-jump wrapper, which has no return address.
    matched_through_wrapper: int = 0
    #: Pairs in order when the sites are sorted by guest address instead. A baseline that
    #: needs no graph at all, so the walk's figure is only worth what it adds to this one.
    address_order_concordant_pairs: int = 0

    @property
    def coverage(self) -> float:
        return self.found_sites / self.runtime_sites if self.runtime_sites else 0.0

    @property
    def concordance(self) -> float:
        return self.concordant_pairs / self.pairs if self.pairs else 0.0

    @property
    def address_order_concordance(self) -> float:
        return self.address_order_concordant_pairs / self.pairs if self.pairs else 0.0

    @property
    def certain_concordance(self) -> float:
        return self.certain_concordant_pairs / self.certain_pairs if self.certain_pairs else 0.0


def parse_runtime_trace(text: str) -> list[RuntimeCall]:
    """Read the call-order table out of the host's report. Other lines are ignored."""
    calls: list[RuntimeCall] = []
    for line in text.splitlines():
        ordinal = _ORDINAL_LINE.match(line)
        if ordinal:
            calls.append(
                RuntimeCall(
                    int(ordinal.group(1)),
                    int(ordinal.group(2)),
                    HIT_KERNEL,
                    int(ordinal.group(3)),
                    int(ordinal.group(4), 16),
                )
            )
            continue
        xdk = _XDK_LINE.match(line)
        if xdk:
            calls.append(
                RuntimeCall(
                    int(xdk.group(1)),
                    int(xdk.group(2)),
                    HIT_XDK,
                    int(xdk.group(3), 16),
                    int(xdk.group(4), 16),
                )
            )
    return calls


def compare(runtime: list[RuntimeCall], walk: Walk, thread: int | None = None) -> Comparison:
    """Coverage and pairwise order agreement, optionally for one runtime thread.

    A kernel wrapper that tail-jumps into the import has no return address of its own, so
    a runtime call reaching the import through one is matched on (kind, value) alone, and
    counted in `matched_through_wrapper` so the looser match is visible.
    """
    selected = [call for call in runtime if thread is None or call.thread == thread]
    always = walk.always_functions()
    exact: dict[tuple[str, int, int], tuple[int, bool, int]] = {}
    wrapper: dict[tuple[str, int], tuple[int, bool, int]] = {}
    for hit in walk.hits:
        if hit.kind not in (HIT_KERNEL, HIT_XDK) or hit.value is None:
            continue
        certain = walk.certainty(hit, always) == CERTAIN
        if hit.return_va:
            exact.setdefault(
                (hit.kind, hit.value, hit.return_va), (hit.tick, certain, hit.return_va)
            )
        else:
            wrapper.setdefault((hit.kind, hit.value), (hit.tick, certain, hit.function))

    first_seen: dict[tuple[str, int, int], RuntimeCall] = {}
    for call in selected:
        first_seen.setdefault((call.kind, call.value, call.return_va), call)
    ordered = sorted(first_seen.items(), key=lambda item: item[1].sequence)

    matched: list[tuple[int, bool, int]] = []
    missed: list[tuple[int, str, int, int]] = []
    through_wrapper = 0
    for key, call in ordered:
        if key in exact:
            matched.append(exact[key])
        elif (key[0], key[1]) in wrapper:
            matched.append(wrapper[(key[0], key[1])])
            through_wrapper += 1
        else:
            missed.append((call.sequence, call.kind, call.value, call.return_va))

    pairs = concordant = certain_pairs = certain_concordant = by_address = 0
    for first in range(len(matched)):
        for second in range(first + 1, len(matched)):
            agrees = matched[first][0] < matched[second][0]
            pairs += 1
            concordant += agrees
            by_address += matched[first][2] < matched[second][2]
            if matched[first][1] and matched[second][1]:
                certain_pairs += 1
                certain_concordant += agrees
    return Comparison(
        runtime_calls=len(selected),
        runtime_sites=len(ordered),
        found_sites=len(matched),
        missed=tuple(missed),
        pairs=pairs,
        concordant_pairs=concordant,
        certain_pairs=certain_pairs,
        certain_concordant_pairs=certain_concordant,
        matched_through_wrapper=through_wrapper,
        address_order_concordant_pairs=by_address,
    )
