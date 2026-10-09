#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Optional backend only: installs no second LLM and changes no project dependencies.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECKOUT="$REPO_ROOT/tmp/oghidra"
REVISION=41829c6b2b25a8a652147f50288f0d9b3721562f

if [ ! -d "$CHECKOUT/.git" ]; then
    git clone --no-checkout https://github.com/LLNL/OGhidra.git "$CHECKOUT"
    git -C "$CHECKOUT" checkout --detach "$REVISION"
fi
if [ "$(git -C "$CHECKOUT" rev-parse HEAD)" != "$REVISION" ] ||
   [ -n "$(git -C "$CHECKOUT" status --porcelain --untracked-files=no)" ]; then
    echo "Expected a clean OGhidra checkout at $REVISION: $CHECKOUT" >&2
    exit 1
fi

uv sync --project "$CHECKOUT" --python 3.12
# Pin the Java bridge exercised by the pilot; Capstone verifies instruction boundaries.
uv pip install --python "$CHECKOUT/.venv/bin/python" \
    pyghidra==3.1.0 JPype1==1.5.2 capstone==5.0.9
echo "OGhidra backend ready at $CHECKOUT; see docs/ghidra-analysis-pilot.md for queries."
