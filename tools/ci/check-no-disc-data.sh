#!/usr/bin/env bash
# Fail if anything that looks like disc-derived data is staged or tracked.
#
# This repository is CODE ONLY. Players supply their own TimeSplitters: Future
# Perfect disc; nothing derived from it may be committed or distributed.
#
# Usage:
#   tools/ci/check-no-disc-data.sh            # check the index (pre-commit)
#   tools/ci/check-no-disc-data.sh --tracked  # check everything tracked (CI)
set -euo pipefail

MODE="${1:---staged}"
MAX_BYTES=$((5 * 1024 * 1024))   # nothing legitimate in this repo is >5 MB
MAX_ADDRESS_LINES=200            # see the content check below for the measured basis

# Extensions that are always disc-derived. Keep in sync with .gitignore.
BANNED_EXT='\.(iso|rvz|wia|gcz|cso|ciso|chd|wbfs|nrg|mdf|mds|cue|img|gcm|xiso|xbe|dol|rel|elf|xex|irx|pak|p4ck|p5ck|war|c2n|tad|xbt|xbr|xbm|xbg|xbx|raw|vag|mib|pss|xmv|thp|bik|bnr|sfo|pdb|pat|sig)$'

# Filenames that are always disc-derived.
BANNED_NAME='^(SLUS_|SLES_|SLED_|SLPS_|SCUS_|SCES_|SCED_|ZDATA|default\.xbe$|main\.dol$|SYSTEM\.CNF$|opening\.bnr$)'

case "$MODE" in
  --staged)  files=$(git diff --cached --name-only --diff-filter=ACMR) ;;
  --tracked) files=$(git ls-files) ;;
  *) echo "usage: $0 [--staged|--tracked]" >&2; exit 2 ;;
esac

fail=0
while IFS= read -r f; do
  [ -z "$f" ] && continue
  base=$(basename "$f")

  if printf '%s' "$base" | grep -qiE "$BANNED_EXT"; then
    echo "REJECTED (disc-derived extension): $f" >&2
    fail=1
    continue
  fi

  if printf '%s' "$base" | grep -qE "$BANNED_NAME"; then
    echo "REJECTED (disc-derived filename): $f" >&2
    fail=1
    continue
  fi

  if [ -f "$f" ]; then
    size=$(stat -c%s "$f" 2>/dev/null || echo 0)
    if [ "$size" -gt "$MAX_BYTES" ]; then
      echo "REJECTED (larger than 5 MB, likely disc data): $f ($size bytes)" >&2
      fail=1
      continue
    fi

    # Bulk per-address analysis output. This is not disc data in the copyright
    # sense, but the repository is deliberately code-only and a dump of every
    # function address in the user's binary belongs in gitignored generated/.
    # Catching it by CONTENT rather than by extension or path is what makes this
    # survive `git add -f`, which .gitignore alone does not.
    #
    # Two conditions, because a count alone is not enough. An absolute count
    # catches size, and a DENSITY ratio distinguishes a data dump from source that
    # legitimately mentions many addresses. A first version used the count only and
    # rejected tests/test_jumptables.py, which has 552 hand-assembled fixture
    # addresses and is obviously not a dump.
    #
    # Both thresholds measured on real files:
    #   tests/test_jumptables.py   552 of 2,287 lines =  24%   legitimate
    #   context.md                  27 of 1,562      =   2%    legitimate
    #   flirt_names.csv            363 of   364      =  99.7%  a dump
    #   unnamed_functions.csv   11,685 of 11,686     =  99.99% a dump
    # Every dump is >=99% and every hand-written file <=24%, so 80% separates them
    # with very wide margin in both directions.
    # The T1260 name overlay is hand-curated (one evidence line per row), is the
    # tracked source of the naming badge and is address-keyed by design; it grows
    # past MAX_ADDRESS_LINES with T1261. Exempt that single path, not the check.
    # T1266: the verified function-table additions are the same kind of file: tracked source
    # of the function set, one evidence line per row, address-keyed by design.
    # T1776: authenticated size overrides are also the tracked function-set source,
    # with one evidence line per row; permit only this exact path as with additions.
    # T1641: the library overlay and the classification table are the same kind of file (hand-curated
    # decisions, one evidence line per row, address-keyed by design, the tracked source of the library
    # naming figure and of the game/library split). They carry no names from the XDK libs.
    if [ "$f" = "tools/data/function_names.csv" ] || [ "$f" = "docs/data/t1473-renames.csv" ] || [ "$f" = "tools/data/function_additions.csv" ] || [ "$f" = "tools/data/function_overrides.csv" ] || [ "$f" = "tools/data/library_names.csv" ] || [ "$f" = "tools/data/function_classification.csv" ]; then
      continue
    fi
    addresses=$(grep -cIE '\b0x[0-9a-fA-F]{5,8}\b' "$f" 2>/dev/null || true)
    addresses=${addresses:-0}
    if [ "$addresses" -gt "$MAX_ADDRESS_LINES" ]; then
      total=$(wc -l < "$f" 2>/dev/null || echo 1)
      [ "$total" -lt 1 ] && total=1
      # addresses/total > 80%  <=>  addresses*5 > total*4, in integer arithmetic.
      if [ $((addresses * 5)) -gt $((total * 4)) ]; then
        echo "REJECTED (bulk per-address output, $addresses of $total lines;" \
             "put it in generated/): $f" >&2
        fail=1
      fi
    fi
  fi
done <<< "$files"

if [ "$fail" -ne 0 ]; then
  cat >&2 <<'EOF'

This repository must contain no disc images, console executables, or game
assets. If a match above is a genuine false positive, override deliberately:

    git add -f <path>

and explain why in the commit message.
EOF
  exit 1
fi

echo "check-no-disc-data: OK ($MODE)"

# T1462: the same hook also refuses a staged known -> unknown badge downgrade, so an
# already-installed pre-commit hook (`exec ./tools/ci/check-no-disc-data.sh --staged`)
# needs no change. Only when docs/badges/metrics.json is part of the commit.
if [ "$MODE" = "--staged" ] && git diff --cached --name-only | grep -q '^docs/badges/'; then
  PY=python3
  [ -x .venv/bin/python ] && PY=.venv/bin/python
  "$PY" -m tools.ci.check_badges_not_downgraded --base HEAD --new index
fi
