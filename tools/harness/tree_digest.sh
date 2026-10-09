# SPDX-License-Identifier: GPL-3.0-or-later
#
# Content digest of a lifted tree, shared by build_stub_objects.sh and
# build_subject.sh so that both stamp the SAME value.
#
# WHY A CONTENT DIGEST AND NOT JUST THE PATH
# ------------------------------------------
# The failure this exists to prevent was a subject measured against a lift ten
# hours older than the fix it was supposed to be testing, reported as a lifter
# defect, and adopted into the project record twice before being retracted. A
# path alone cannot catch that: `generated/lifted/gen` is regenerated in place,
# so a subject built from yesterday's contents of that directory has a gen_dir
# that matches the shipped tree exactly while measuring something else. Only the
# bytes distinguish them.
#
# The definition is mirrored in tools/harness/provenance.py. The two
# implementations are pinned against each other by a test, because a Python
# reader that computed a different digest from the shell writer would make every
# provenance check pass vacuously.
#
# Definition: sha256 of the LC_ALL=C-sorted `sha256sum` listing of every *.c,
# *.h and *.inc file directly inside the directory. Filenames are included in
# the listing, so renaming a chunk changes the digest.

# Prints the 64-hex digest of the tree at $1, or exits non-zero if $1 is not a
# directory or holds no hashable file.
harness_tree_digest() {
    local dir="$1"
    if [[ ! -d "$dir" ]]; then
        echo "error: not a directory: $dir" >&2
        return 1
    fi
    local listing
    listing="$(
        cd "$dir" || exit 1
        LC_ALL=C find . -maxdepth 1 ! -type d \
            \( -name '*.c' -o -name '*.h' -o -name '*.inc' \) -printf '%f\n' \
            | LC_ALL=C sort \
            | tr '\n' '\0' \
            | xargs -0 -r sha256sum
    )"
    if [[ -z "$listing" ]]; then
        echo "error: no *.c/*.h/*.inc files in $dir" >&2
        return 1
    fi
    # The trailing newline matters: `sha256sum` terminates every line including
    # the last, and provenance.py builds its listing the same way.
    printf '%s\n' "$listing" | sha256sum | cut -d' ' -f1
}

# The first 16 hex digits, which is what goes on every results.csv row. Full
# width would add 64 bytes to ~80,000 rows for no extra discrimination at this
# scale; the full digest is kept in the provenance sidecar and the summary.
harness_tree_digest_short() {
    harness_tree_digest "$1" | cut -c1-16
}
