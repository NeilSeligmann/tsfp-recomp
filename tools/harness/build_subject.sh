#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Links driver.c (the mprotect-based differential-test subject) against
# the minimal runtime and every lifted recomp_*.o object file into a
# runnable subject binary.
#
# -pie is NOT optional here. driver.c identity-maps the guest window at
# [0x00010000, 0x01000000) with mmap(MAP_FIXED_NOREPLACE). A -no-pie link
# produces an ET_EXEC loaded at the fixed base address 0x400000, which
# falls *inside* that window -- the kernel's own placement of our code and
# data would collide with the guest image, and the identity mmap fails.
# -pie makes this an ET_DYN that the kernel places at a high, randomized
# address instead, leaving the low window free for driver.c to claim.
#
# WHY THE DEFAULTS LIVE IN THE REPO
# ---------------------------------
# They used to point at three directories under tmp/: a stale object set, a
# runtime two lifter generations old, and the generated headers that matched
# them. All three were stale in lockstep, so an all-defaults link of three
# mutually consistent stale inputs SUCCEEDED, while a link against the tree the
# repo actually ships failed on 6,575 relocations to four trap symbols that
# runtime never defined. The script could not build the shipped tree without
# arguments at all, which is why "the harness as its own documentation invokes it
# has not been run against the committed lift" was true.
#
# PROVENANCE IS BAKED INTO THE BINARY
# -----------------------------------
# The gen_dir and a content digest of the lifted tree are compiled into driver.c
# with -D and reported on its READY line, so the harness reads them from the
# subject itself. A subject built from a lift ten hours older than the fix it was
# meant to be testing was reported as a lifter defect and written into the project
# record twice, because results.csv recorded the seed and the case index but not
# which tree produced the numbers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=tools/harness/tree_digest.sh
source "$SCRIPT_DIR/tree_digest.sh"

usage() {
    cat <<EOF
Usage: build_subject.sh [OBJ_DIR] [RUNTIME_SRC] [OUT_PATH]

Compiles tools/harness/driver.c and links it against RUNTIME_SRC plus every
*.o file in OBJ_DIR into a runnable differential-test subject binary.

Positional arguments (all optional, in order):
  OBJ_DIR       Directory holding the lifted recomp_*.o object files, as
                produced by tools/harness/build_stub_objects.sh.
                Default: $REPO_ROOT/tmp/harness-stub/build/obj
  RUNTIME_SRC   Path to the minimal runtime C source.
                Default: $SCRIPT_DIR/runtime_min.c  (tracked, in-repo)
  OUT_PATH      Output path for the linked subject binary.
                Default: $REPO_ROOT/tmp/harness-build/subject

Environment overrides:
  EXTRA_OBJS      Space separated hand-written replacement objects to link in (built by
                  `tools/replace build-subject`, never by hand). Setting it makes the
                  subject a REPLACEMENT subject: driver.c is compiled with
                  HARNESS_REPLACEMENT=1 against REPL_INCLUDE_DIR and the READY line reports
                  how many replacements are linked and REPL_SHA.
  REPL_INCLUDE_DIR  Directory holding game_replace.h. Required with EXTRA_OBJS.
  REPL_SHA        Content digest of src/game at build time, baked in. Required with
                  EXTRA_OBJS.
  GEN_DIR   Directory with the generated headers (recomp_funcs.h,
            recomp_types.h), passed as -I and recorded as this subject's
            provenance. Default: $REPO_ROOT/generated/lifted/gen
  CC        Compiler to invoke. Default: cc
  HARNESS_ALLOW_GEN_MISMATCH
            Set to 1 to link even when OBJ_DIR's recorded gen_dir disagrees
            with GEN_DIR. Off by default: objects built from one tree and
            headers from another is a subject whose behaviour belongs to
            neither, with nothing in its output saying so.

Examples:
  build_subject.sh
  build_subject.sh $REPO_ROOT/tmp/harness-stub/build/obj
  GEN_DIR=$REPO_ROOT/generated/lifted-manual/gen build_subject.sh \\
      $REPO_ROOT/tmp/harness-stub-manual/build/obj
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

OBJ_DIR="${1:-$REPO_ROOT/tmp/harness-stub/build/obj}"
RUNTIME_SRC="${2:-$SCRIPT_DIR/runtime_min.c}"
OUT_PATH="${3:-$REPO_ROOT/tmp/harness-build/subject}"
GEN_DIR="${GEN_DIR:-$REPO_ROOT/generated/lifted/gen}"
CC="${CC:-cc}"

DRIVER_SRC="$SCRIPT_DIR/driver.c"

EXTRA_OBJS="${EXTRA_OBJS:-}"
REPL_INCLUDE_DIR="${REPL_INCLUDE_DIR:-}"
REPL_SHA="${REPL_SHA:-}"
REPL_FLAGS=()
if [[ -n "$EXTRA_OBJS" ]]; then
    if [[ -z "$REPL_INCLUDE_DIR" || -z "$REPL_SHA" ]]; then
        echo "error: EXTRA_OBJS needs REPL_INCLUDE_DIR and REPL_SHA, so the subject can say" >&2
        echo "       which hand-written source it was built from" >&2
        exit 1
    fi
    for obj in $EXTRA_OBJS; do
        if [[ ! -f "$obj" ]]; then
            echo "error: replacement object not found: $obj" >&2
            exit 1
        fi
    done
    REPL_FLAGS=(-DHARNESS_REPLACEMENT=1 -I"$REPL_INCLUDE_DIR" -DHARNESS_REPL_SHA="\"$REPL_SHA\"")
fi

if [[ ! -f "$DRIVER_SRC" ]]; then
    echo "error: driver.c not found next to this script ($DRIVER_SRC)" >&2
    exit 1
fi
if [[ ! -d "$OBJ_DIR" ]]; then
    echo "error: object directory not found: $OBJ_DIR" >&2
    echo "       build it with tools/harness/build_stub_objects.sh" >&2
    exit 1
fi
if [[ ! -f "$RUNTIME_SRC" ]]; then
    echo "error: runtime source not found: $RUNTIME_SRC" >&2
    exit 1
fi
if [[ ! -d "$GEN_DIR" ]]; then
    echo "error: generated headers directory not found: $GEN_DIR" >&2
    exit 1
fi

# The object set records which tree it was compiled from. Cross-checking it
# against GEN_DIR here is the one place that can catch "objects from tree A,
# headers from tree B" before the mismatch becomes a number in a CSV.
OBJ_PROVENANCE="$(dirname "$OBJ_DIR")/provenance.txt"
if [[ -f "$OBJ_PROVENANCE" ]]; then
    OBJ_GEN_DIR="$(sed -n 's/^gen_dir=//p' "$OBJ_PROVENANCE" | head -1)"
    if [[ -n "$OBJ_GEN_DIR" && "$(realpath -m "$OBJ_GEN_DIR")" != "$(realpath -m "$GEN_DIR")" ]]; then
        echo "error: the objects in $OBJ_DIR were built from" >&2
        echo "         $OBJ_GEN_DIR" >&2
        echo "       but GEN_DIR is" >&2
        echo "         $GEN_DIR" >&2
        echo "       Linking these together produces a subject whose behaviour belongs" >&2
        echo "       to neither tree. Rebuild the objects, or set GEN_DIR to match, or" >&2
        echo "       set HARNESS_ALLOW_GEN_MISMATCH=1 if you know why you want this." >&2
        if [[ "${HARNESS_ALLOW_GEN_MISMATCH:-0}" != "1" ]]; then
            exit 1
        fi
        echo "       HARNESS_ALLOW_GEN_MISMATCH=1: continuing anyway" >&2
    fi
fi

mkdir -p "$(dirname "$OUT_PATH")"

mapfile -t OBJS < <(find "$OBJ_DIR" -maxdepth 1 -name '*.o' | sort)
if [[ ${#OBJS[@]} -eq 0 ]]; then
    echo "error: no .o files found in $OBJ_DIR" >&2
    exit 1
fi
echo "linking ${#OBJS[@]} object file(s) from $OBJ_DIR" >&2

# Whether these objects route their calls through the callee stub is DETECTED,
# never asserted by hand: the flag that makes the subject advertise `stub=1` at
# READY is derived from the objects actually being linked. A hand-set flag is how
# a stubbed oracle ends up compared against a subject running the real callees,
# with the difference reported as a lifter defect.
STUB_OBJECTS=0
for obj in "${OBJS[@]}"; do
    # Not `grep -q`: it exits on the first match, nm then takes SIGPIPE, and under
    # `set -o pipefail` a SUCCESSFUL match becomes a failed pipeline.
    if [[ "$(nm -u "$obj" | grep -c harness_stub_call || true)" -gt 0 ]]; then
        STUB_OBJECTS=1
        break
    fi
done
if [[ "$STUB_OBJECTS" -eq 1 ]]; then
    echo "objects are stub-enabled; subject will advertise stub=1" >&2
else
    echo "objects are NOT stub-enabled; call-bearing functions must stay out of scope" >&2
fi

# Whether the lifted tree publishes EFLAGS (a --publish-eflags lift) is likewise
# DETECTED, from the generated header: only such a lift declares the publication
# globals there, and the tree_sha cross-check above already pins the objects to
# this same GEN_DIR, so the header speaks for the objects. Reported at READY as
# eflags=, which is what lets the harness refuse a --compare-eflags run against
# a subject that publishes nothing.
EFLAGS_OBJECTS=0
if [[ "$(grep -c 'g_harness_eflags' "$GEN_DIR/recomp_funcs.h" || true)" -gt 0 ]]; then
    EFLAGS_OBJECTS=1
    echo "tree publishes EFLAGS; subject will advertise eflags=1" >&2
fi

# The digest is of GEN_DIR's CONTENTS, not of its path. `generated/lifted/gen` is
# regenerated in place, so a path alone cannot distinguish a current subject from
# one built from yesterday's bytes of the same directory -- which is the precise
# shape of the failure this stamping exists to prevent.
TREE_SHA_FULL="$(harness_tree_digest "$GEN_DIR")"
if [[ "$STUB_OBJECTS" -eq 1 ]]; then
    if [[ ! -f "$OBJ_PROVENANCE" ]]; then
        echo "error: stub objects have no provenance.txt; rebuild with build_stub_objects.sh" >&2
        exit 1
    fi
    OBJ_TREE_SHA="$(sed -n 's/^tree_sha_full=//p' "$OBJ_PROVENANCE" | head -1)"
    OBJ_SHIM_SHA="$(sed -n 's/^shim_sha=//p' "$OBJ_PROVENANCE" | head -1)"
    CURRENT_SHIM_SHA="$(sha256sum "$SCRIPT_DIR/call_stub_shim.h" | cut -d' ' -f1)"
    if [[ -z "$OBJ_TREE_SHA" || "$OBJ_TREE_SHA" != "$TREE_SHA_FULL" ]]; then
        echo "error: stub object tree digest differs from GEN_DIR; rebuild the objects" >&2
        exit 1
    fi
    if [[ -z "$OBJ_SHIM_SHA" || "$OBJ_SHIM_SHA" != "$CURRENT_SHIM_SHA" ]]; then
        echo "error: stub objects use a different or unrecorded shim; rebuild the objects" >&2
        exit 1
    fi
fi
TREE_SHA="${TREE_SHA_FULL:0:16}"
GEN_DIR_ABS="$(realpath -m "$GEN_DIR")"

# A verified native body is allowed only with the compilation policy whose
# page-probe reads were measured. Optimization can remove a dead TEST load.
STACKPROBE_APPROVED=0
if [[ "$STUB_OBJECTS" -eq 1 && -z "$EXTRA_OBJS" ]]; then
    OBJ_COMPILE_POLICY="$(sed -n 's/^compile_policy=//p' "$OBJ_PROVENANCE" | head -1)"
    if [[ "$OBJ_COMPILE_POLICY" == "O0-no-strict-aliasing-v1" ]] &&
       grep -q '^weak_dir=NONE$' "$OBJ_PROVENANCE"; then
        STACKPROBE_APPROVED="$(python3 "$SCRIPT_DIR/stackprobe.py" "$GEN_DIR")"
    fi
fi
echo "verified stack-probe passthrough: $STACKPROBE_APPROVED" >&2

# T1510 opt-in raw x87 backend: only when X87RAW=1 (the model-arbitrated-x87-v2 proof). The
# default link is unchanged: no define, no extra sources.
X87_FLAGS=()
X87_SOURCES=()
if [[ "${X87RAW:-0}" == "1" ]]; then
    X87_FLAGS=(-DHARNESS_X87RAW=1)
    X87_SOURCES=("$SCRIPT_DIR/x87_runtime.c" "$SCRIPT_DIR/x87_native_ext.c")  # T1510: ext = x87_native.c + ops 4..6 (3C9DE8)
    echo "raw x87 backend enabled (X87RAW=1); subject will advertise x87raw=1" >&2
fi

# T1508 opt-in code-write stop channel: only when CODEWRITE=1 (the faithful-codewrite-arbiter-v2
# proof). The default link is unchanged: no define.
if [[ "${CODEWRITE:-0}" == "1" ]]; then
    X87_FLAGS+=(-DHARNESS_CODEWRITE=1)
    echo "code-write stop channel enabled (CODEWRITE=1); subject will advertise codewrite=1" >&2
fi

# T1576 opt-in arm-witness subject: only when WITNESS=1 (guarded-jump-schema3 proof). The driver
# is built with clang source-based coverage so it can dump one profile per case. Default link
# is unchanged: no define, no extra flags.
WITNESS_FLAGS=()
if [[ "${WITNESS:-0}" == "1" ]]; then
    WITNESS_FLAGS=(-DHARNESS_WITNESS=1 -fprofile-instr-generate)
    echo "arm witness enabled (WITNESS=1); profiles via HARNESS_WITNESS_FILE" >&2
fi

# -O0: test the generated code as written, not as the optimizer reshapes it.
# -fno-strict-aliasing: the generated code and runtime do extensive
#   type-punning through the MEM32/MEMF-style macros; strict aliasing rules
#   would let the compiler miscompile those accesses.
# -pie / -lm: see the file header comment above.
# shellcheck disable=SC2086  # EXTRA_OBJS is a space separated list by contract
"$CC" -O0 -std=gnu11 -fno-strict-aliasing -I"$GEN_DIR" -I"$SCRIPT_DIR" \
    -DHARNESS_STUB_OBJECTS="$STUB_OBJECTS" \
    -DHARNESS_EFLAGS_OBJECTS="$EFLAGS_OBJECTS" \
    -DHARNESS_STACKPROBE_APPROVED="$STACKPROBE_APPROVED" \
    -DHARNESS_GEN_DIR="\"$GEN_DIR_ABS\"" \
    -DHARNESS_TREE_SHA="\"$TREE_SHA\"" \
    "${REPL_FLAGS[@]}" "${X87_FLAGS[@]}" "${WITNESS_FLAGS[@]}" \
    "$DRIVER_SRC" "$SCRIPT_DIR/call_stub.c" "$RUNTIME_SRC" "${OBJS[@]}" $EXTRA_OBJS \
    "${X87_SOURCES[@]}" \
    -pie -fno-strict-aliasing -lm \
    -o "$OUT_PATH"

# A sidecar for humans. The authoritative copy is the one compiled INTO the
# binary, which is what the harness reads and what reaches every CSV row: a
# sidecar can be left behind by a previous build, a baked-in string cannot.
{
    echo "subject=$OUT_PATH"
    echo "gen_dir=$GEN_DIR_ABS"
    echo "tree_sha=$TREE_SHA"
    echo "stackprobe=$STACKPROBE_APPROVED"
    echo "tree_sha_full=$TREE_SHA_FULL"
    echo "obj_dir=$OBJ_DIR"
    echo "runtime_src=$RUNTIME_SRC"
    echo "stub_objects=$STUB_OBJECTS"
    echo "eflags_objects=$EFLAGS_OBJECTS"
    echo "extra_objs=$EXTRA_OBJS"
    echo "repl_sha=${REPL_SHA:-NONE}"
    echo "repl_cflags=${REPL_CFLAGS:-}"
    echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT_PATH.provenance.txt"

echo "built: $OUT_PATH (stub=$STUB_OBJECTS tree_sha=$TREE_SHA)" >&2
echo "       from $GEN_DIR_ABS" >&2
