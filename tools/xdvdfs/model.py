# SPDX-License-Identifier: GPL-3.0-or-later
"""Data types for the Xbox DVD filesystem."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class XisoEntry:
    path: str
    start_sector: int
    size: int
    is_dir: bool
