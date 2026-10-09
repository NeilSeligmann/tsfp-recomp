# SPDX-License-Identifier: GPL-3.0-or-later
"""Combine one boot's captured output with the static census into a verdict."""

from __future__ import annotations

from dataclasses import dataclass

from tools.mmio_reach.census import Finding
from tools.mmio_reach.instrument import parse_hits
from tools.mmio_reach.trace import ThreadStop, XdkCall, parse_stops, parse_xdk_calls


class EmptyCapture(ValueError):
    """The capture holds no evidence, so any verdict from it would be vacuous."""


@dataclass(frozen=True)
class Assessment:
    stops: tuple[ThreadStop, ...]
    xdk_calls: tuple[XdkCall, ...]
    #: None when the host was not built from an instrumented tree, so nothing was measured.
    hits: tuple[int, ...] | None
    #: NV2A faults reported by a guest thread.
    nv2a_faults: tuple[ThreadStop, ...]
    #: Functions with a direct NV2A access that the boot entered, in entry order.
    executed_accessors: tuple[int, ...]
    #: Functions with a direct NV2A access that the boot did not enter.
    unexecuted_accessors: tuple[int, ...]

    @property
    def touched(self) -> bool:
        """True when the boot demonstrably reached NV2A register code."""
        return bool(self.nv2a_faults) or bool(self.executed_accessors)

    @property
    def execution_measured(self) -> bool:
        return self.hits is not None


def assess(text: str, accessors: list[Finding], instrumented: bool) -> Assessment:
    """`accessors` are the census findings with a direct access, whatever their reach."""
    stops = tuple(parse_stops(text))
    if not stops:
        raise EmptyCapture("no `guest thread ... stopped:` block: the guest never ran")
    hits = tuple(parse_hits(text)) if instrumented else None
    if instrumented and not hits:
        raise EmptyCapture("no T59-HIT lines: the host was not built from `instrument` output")
    entered = set(hits or ())
    direct = [f.entry for f in accessors if f.direct]
    executed = tuple(entry for entry in (hits or ()) if entry in set(direct))
    return Assessment(
        stops=stops,
        xdk_calls=tuple(parse_xdk_calls(text)),
        hits=hits,
        nv2a_faults=tuple(stop for stop in stops if stop.faulted_in_nv2a),
        executed_accessors=executed,
        unexecuted_accessors=tuple(entry for entry in direct if entry not in entered)
        if instrumented
        else (),
    )
