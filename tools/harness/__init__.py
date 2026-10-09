# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential harness: lifted C versus the original guest code under Unicorn.

The correctness oracle for the recompilation. A hand-written or lifted function is
trusted only when it cannot be distinguished from the original machine code running
over byte-identical initial state. See `tools/harness/cli.py` for the entry point and
`docs/lifter-evaluation.md` §7 for the throwaway version this productionises.
"""

from __future__ import annotations
