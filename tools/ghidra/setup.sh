#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install the exact Ghidra + XBE loader combination this project needs.
#
# Two things here are load-bearing and were both discovered the hard way:
#
#   1. Ghidra must be 12.0.3, not the latest. Ghidra extensions are strictly
#      version-pinned, and the XBE loader's newest prebuilt release targets
#      12.0.3. On a newer Ghidra the import fails with the unhelpful message
#      "No load spec found for import file", as though the file were corrupt.
#
#   2. The extension must go in the USER extension directory
#      (~/.config/ghidra/<version>/Extensions/). Extracting it into
#      <install>/Extensions/Ghidra/ looks right, has the correct
#      Module.manifest layout, and is silently ignored by headless.
#
# Nothing downloaded here is committed. xtlid.xml and XbSymbolDatabase are
# fetched at pinned versions; see docs/provenance.md for why they are acceptable
# symbol sources and which ones are not.
set -euo pipefail

GHIDRA_VERSION="${GHIDRA_VERSION:-12.0.3}"
GHIDRA_DATE="${GHIDRA_DATE:-20260210}"
XBE_EXT_BUILD="${XBE_EXT_BUILD:-build-202602250354}"
XBE_EXT_DATE="${XBE_EXT_DATE:-20260225}"
XTLID_VERSION="${XTLID_VERSION:-v0.1.2}"
XBSYMDB_VERSION="${XBSYMDB_VERSION:-v3.1.160}"

PREFIX="${PREFIX:-$HOME/.local/share/tsfp-decomp}"
DOWNLOADS="$PREFIX/downloads"
INSTALL="$PREFIX/ghidra_${GHIDRA_VERSION}_PUBLIC"
USER_EXT="$HOME/.config/ghidra/ghidra_${GHIDRA_VERSION}_PUBLIC/Extensions"

GHIDRA_ZIP="ghidra_${GHIDRA_VERSION}_PUBLIC_${GHIDRA_DATE}.zip"
GHIDRA_URL="https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_${GHIDRA_VERSION}_build/${GHIDRA_ZIP}"
XBE_EXT_ZIP="ghidra_${GHIDRA_VERSION}_PUBLIC_${XBE_EXT_DATE}_ghidra-xbe.zip"
XBE_EXT_URL="https://github.com/XboxDev/ghidra-xbe/releases/download/${XBE_EXT_BUILD}/${XBE_EXT_ZIP}"
XTLID_URL="https://github.com/XboxDev/xtlid/releases/download/${XTLID_VERSION}/xtlid.xml"
XBSYMDB_URL="https://github.com/Cxbx-Reloaded/XbSymbolDatabase/releases/download/${XBSYMDB_VERSION}/XbSymbolDatabase.zip"

mkdir -p "$DOWNLOADS" "$USER_EXT"

fetch() {
    local url="$1" dest="$2"
    if [ -s "$dest" ]; then
        echo "have $(basename "$dest")"
        return
    fi
    echo "fetching $(basename "$dest")"
    curl -fsSL -o "$dest" "$url"
    # A truncated download is worse than a failed one: it produces a confusing
    # extraction error much later. Fail loudly instead.
    if [ ! -s "$dest" ]; then
        echo "ERROR: $(basename "$dest") is empty" >&2
        exit 1
    fi
}

fetch "$GHIDRA_URL"  "$DOWNLOADS/$GHIDRA_ZIP"
fetch "$XBE_EXT_URL" "$DOWNLOADS/$XBE_EXT_ZIP"
fetch "$XTLID_URL"   "$DOWNLOADS/xtlid.xml"
fetch "$XBSYMDB_URL" "$DOWNLOADS/XbSymbolDatabase.zip"

# --- OPT-IN, AND DELIBERATELY NOT PART OF A NORMAL SETUP ---------------------
#
# IDA FLIRT pattern files for the Xbox XDK, consumed by tools/flirt/. These were
# produced by running IDA's library parser over LEAKED Microsoft XDK .lib
# archives. docs/provenance.md permits them as a *local, uncommitted analysis
# aid* and forbids them, or anything derived from them, entering this repository.
#
# So this step is off unless you ask for it, and nothing else in the toolchain
# depends on it. That is the point: a provenance-dubious source must not become
# load-bearing, and it must not be downloaded onto a contributor's machine as a
# side effect of setting up a disassembler. Everything above this line is clean
# and sufficient on its own.
#
# The honest reason to run it at all: `.XTLID` names only 294 of our 12,343
# recovered functions, and this set names several hundred more. The names it
# produces are PROPOSALS for review, measured against `.XTLID` on every run; see
# tools/flirt/cli.py. Enable with:
#
#     TSFP_FETCH_XDK_PATTERNS=1 tools/ghidra/setup.sh
#
if [ "${TSFP_FETCH_XDK_PATTERNS:-0}" = "1" ]; then
    PATTERNS_URL="${PATTERNS_URL:-https://raw.githubusercontent.com/PatrickvL/Dxbx/master/Resources/Patterns/Patterns.zip}"
    PATTERNS_DIR="$DOWNLOADS/xdk-patterns"
    echo
    echo "fetching XDK FLIRT patterns (see docs/provenance.md -- NEVER commit these)"
    fetch "$PATTERNS_URL" "$DOWNLOADS/Patterns.zip"
    if [ ! -d "$PATTERNS_DIR" ]; then
        mkdir -p "$PATTERNS_DIR"
        unzip -q -o "$DOWNLOADS/Patterns.zip" -d "$PATTERNS_DIR"
    fi
    # The archive covers ten XDK builds. Ours is 5849; see the .XTLID section's
    # own xdk_build field, which tools/xbe/parser.py reads, rather than assuming.
    echo "XDK pattern files are at $PATTERNS_DIR (build 5849 is the one that matches)"
    echo "  uv run python -m tools.flirt.cli <xbe> --patterns $PATTERNS_DIR/5849*.pat \\"
    echo "      --xtlid-db $DOWNLOADS/xtlid.xml --out generated/retail/flirt_names.csv"
fi

if [ ! -d "$INSTALL" ]; then
    echo "extracting Ghidra $GHIDRA_VERSION"
    mkdir -p "$PREFIX"
    ( cd "$PREFIX" && unzip -q "$DOWNLOADS/$GHIDRA_ZIP" )
fi

if [ ! -d "$USER_EXT/ghidra-xbe" ]; then
    echo "installing the XBE loader into the user extension directory"
    ( cd "$USER_EXT" && unzip -q "$DOWNLOADS/$XBE_EXT_ZIP" )
fi

chmod +x "$INSTALL/support/analyzeHeadless" || true
find "$USER_EXT/ghidra-xbe/os" -type f -name 'XbSymbolDatabaseCLI*' \
    ! -name '*.LICENSE' -exec chmod +x {} + 2>/dev/null || true

cat <<EOF

Ghidra $GHIDRA_VERSION ready.

  export GHIDRA_INSTALL_DIR="$INSTALL"
  uv run python -m tools.ghidra.run <xbe> --out generated

The xtlid database is at:
  $DOWNLOADS/xtlid.xml
  uv run python -m tools.xtlid_cli <xbe> --db $DOWNLOADS/xtlid.xml --out names.txt
EOF
