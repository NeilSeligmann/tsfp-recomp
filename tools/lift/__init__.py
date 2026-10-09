# SPDX-License-Identifier: GPL-3.0-or-later
"""Static-recompilation driver: XBE -> lifted C.

Phase 1.4 of ``docs/superpowers/plans/2026-10-01-runnable-game.md``. This package
runs `sp00nznet/xboxrecomp`'s four-stage analysis pipeline over a user-supplied
XBE and writes the lifted C into a gitignored output directory, so the lifted
code can be compiled and linked against our own HLE.

INTEGRATION MODEL: SELECTIVE VENDORED SUBTREE under ``third_party/xboxrecomp/``.

Three options were on the table and the constraints decide between them:

  * **git submodule** -- rejected. `docs/lifter-evaluation.md` returns a verdict
    of ADAPT, not ADOPT: ten enumerated changes to the lifter are required before
    we would trust it, starting with making an unresolved indirect dispatch trap
    instead of silently returning zero. A submodule makes every one of those
    changes live in a separate repository with a separate history, which is
    exactly the dependency the ADAPT verdict says not to take. A submodule also
    pulls the whole upstream tree, including the nine LGPL files in ``src/``.
  * **fetched at build time**, as ``tools/ghidra/setup.sh`` does -- rejected.
    That pattern is right for Ghidra because we run Ghidra unmodified and it is
    a 400 MB binary distribution. Neither applies here: the lifter is 1.5 MB of
    pure Python that we intend to modify. Fetching also makes the build depend on
    upstream still existing at a pinned commit, and it turns our local changes
    into an out-of-tree patch series rather than ordinary reviewable commits.
  * **selective vendored subtree** -- chosen. 116 files, ~1.5 MB, all Python and
    one C header. Our changes become normal commits with normal diffs; the build
    has no network dependency; and because the import is *selective* we take only
    the analysis half and none of the runtime.

WHAT IS VENDORED, AND WHY THAT SET.  ``VENDORED_PATHS`` below is the whole of it:
the four pipeline stages (``tools/xbe_parser``, ``tools/disasm``,
``tools/func_id``, ``tools/abi_analysis``), the lifter itself (``tools/recomp``),
and ``templates/runtime/recomp_types.h``, which the generated C includes. The
import survey backing that list found no imports outside this set.

WHAT IS DELIBERATELY NOT VENDORED.  Upstream's entire ``src/`` tree. That is
where all nine LGPL-2.1-or-later files extracted from xemu live (7 APU files,
``nv2a_regs.h``, and ``apu_shim.h``; enumerated in upstream's own ``NOTICE``).
**Not one of them is in the lifter**, so excluding ``src/`` costs us nothing and
keeps this repository free of LGPL code -- see ``docs/provenance.md``. Excluding
``src/`` also drops the Windows-only runtime, the D3D11 host and the
``WinMain``-based game template, none of which we can use: our host is
``src/host/`` and our kernel HLE is ``src/xbox/``.

LICENCE OBLIGATIONS.  The vendored subset is MIT (Copyright (c) 2026 sp00nz).
MIT requires the copyright notice and the permission notice to travel with the
code, so ``vendor.py`` copies upstream's ``LICENSE`` verbatim and refuses to
write a tree without it. Upstream's ``NOTICE`` is copied as well even though
every file it covers is excluded: it is the document that *proves* the exclusion
is deliberate and complete, which is worth more to us than it costs. Our own
GPL-3.0-or-later and MIT are compatible in this direction (MIT code may be
incorporated into a GPL work); the MIT notice stays attached to the MIT files and
no MIT file acquires a GPL header.

The sibling ``tools`` name collision is handled by invoking each stage as a
subprocess with ``cwd`` set to the vendored root, so upstream's ``from tools.X
import ...`` resolves to its own package and never to ours.
"""

from __future__ import annotations

from tools.lift.pipeline import LiftResult, StageResult, run_pipeline
from tools.lift.vendor import VENDORED_PATHS, VendorStatus, vendor_status

__all__ = [
    "VENDORED_PATHS",
    "LiftResult",
    "StageResult",
    "VendorStatus",
    "run_pipeline",
    "vendor_status",
]
