#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Exercise the pinned hand drafts, including roots rejected by the proof gates.
set -euo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d /tmp/t1788-native.XXXXXX)"
trap 'rm -rf "$work"' EXIT
gcc --version | head -n 1
clang --version | head -n 1
for opt in 0 3; do
    gcc -std=c11 -O"$opt" -malign-data=abi -Wall -Wextra -Werror -Isrc/game \
        tests/native/t1788_gameplay.c -x c docs/data/t1788-gameplay/frozen-drafts.c.txt docs/data/t1788-gameplay/supplemental-draft.c.txt \
        -o "$work/gcc-$opt"
    "$work/gcc-$opt"
    clang -std=c11 -O"$opt" -Wall -Wextra -Werror -Isrc/game \
        tests/native/t1788_gameplay.c -x c docs/data/t1788-gameplay/frozen-drafts.c.txt docs/data/t1788-gameplay/supplemental-draft.c.txt \
        -o "$work/clang-$opt"
    "$work/clang-$opt"
done
echo 'T1788 native PASS: GCC ABI alignment and Clang O0/O3, thirteen drafts and saved-slot alias controls'
