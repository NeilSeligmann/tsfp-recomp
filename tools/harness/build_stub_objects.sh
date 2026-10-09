#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Recompile the generated chunks with the callee-stub interception in place.
#
# The subject can only intercept a call where the generated code routes it
# through the RECOMP_ABI_CALL macro, and a macro is a compile-time thing, so
# bringing `call`-bearing functions into scope needs its own object set. This
# script produces that object set WITHOUT touching the generated tree: it
# symlinks the generated sources and the real headers into a scratch build
# directory and drops tools/harness/call_stub_shim.h in beside them under the
# name `recomp_funcs.h`, which is the header the chunks actually include.
#
# See call_stub_shim.h for why shadowing that particular header is the only
# interception point that works.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build_stub_objects.sh [GEN_DIR] [BUILD_DIR] [JOBS]

Recompiles every *.c in GEN_DIR into BUILD_DIR/obj with the callee-stub shim
shadowing recomp_funcs.h, so that every lifted call site calls
harness_stub_call() instead of its callee.

Positional arguments (all optional, in order):
  GEN_DIR     Directory holding the generated recomp_*.c and the real headers.
              Default: <repo>/generated/lifted/gen
  BUILD_DIR   Scratch directory for the shadow headers and objects.
              Default: $REPO_ROOT/tmp/harness-stub/build
  JOBS        Parallel compiler jobs. Default: nproc

Environment overrides:
  CC          Compiler to invoke. Default: cc
  WEAK_DIR    Directory of weak_<chunk>.h headers written by `tools/replace wire`. A chunk
              with a header there is compiled with it force-included, which makes the
              lifted definitions named in it weak so a hand-written strong definition
              replaces them at link time. Unset: every chunk compiles exactly as before.
  REUSE_OBJ_DIR  An object directory from an earlier run of this script on the SAME GEN_DIR
              (its sibling provenance.txt must carry the same tree_sha_full and shim_sha). Chunks
              WITHOUT a weak header are taken from it instead of being recompiled, so
              adding a replacement recompiles only the chunk it lives in.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=tools/harness/tree_digest.sh
source "$SCRIPT_DIR/tree_digest.sh"

# Defaults point at the tree the repo SHIPS, not at a scratch directory. The old
# default was $REPO_ROOT/tmp/lifter-eval/gen, which was stale in lockstep with
# build_subject.sh's equally stale defaults, so an all-defaults build of three
# mutually consistent stale inputs succeeded and measured nothing anyone ships.
GEN_DIR="$(realpath -m "${1:-$REPO_ROOT/generated/lifted/gen}")"
BUILD_DIR="$(realpath -m "${2:-$REPO_ROOT/tmp/harness-stub/build}")"
JOBS="${3:-$(nproc)}"
CC="${CC:-cc}"

SHIM="$SCRIPT_DIR/call_stub_shim.h"
WEAK_DIR="${WEAK_DIR:-}"
REUSE_OBJ_DIR="${REUSE_OBJ_DIR:-}"
if [[ -n "$WEAK_DIR" ]]; then
    WEAK_DIR="$(realpath -m "$WEAK_DIR")"
fi
if [[ -n "$REUSE_OBJ_DIR" ]]; then
    REUSE_OBJ_DIR="$(realpath -m "$REUSE_OBJ_DIR")"
    REUSE_PROVENANCE="$(dirname "$REUSE_OBJ_DIR")/provenance.txt"
    if [[ ! -f "$REUSE_PROVENANCE" ]]; then
        echo "error: REUSE_OBJ_DIR has no sibling provenance.txt: $REUSE_PROVENANCE" >&2
        exit 1
    fi
fi

for required in "$GEN_DIR/recomp_funcs.h" "$GEN_DIR/recomp_types.h" "$SHIM"; do
    if [[ ! -f "$required" ]]; then
        echo "error: required file missing: $required" >&2
        exit 1
    fi
done

SRC_DIR="$BUILD_DIR/src"
OBJ_DIR="$BUILD_DIR/obj"
LOG_DIR="$BUILD_DIR/log"
# A failed rebuild can leave old objects beside freshly written metadata. Never
# let a later link stamp that partial object set as a successful current build.
trap 'build_status=$?; if [[ "$build_status" -ne 0 ]]; then rm -f "$BUILD_DIR/provenance.txt"; fi' EXIT
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR" "$OBJ_DIR" "$LOG_DIR"

# The chunks are symlinked, not copied: a quoted #include resolves against the
# directory of the file as the compiler opened it, which for a symlink is this
# build directory -- so the shadow header below wins without duplicating a
# multi-megabyte generated tree that another process may be regenerating.
for src in "$GEN_DIR"/*.c; do
    ln -sf "$src" "$SRC_DIR/$(basename "$src")"
done
ln -sf "$GEN_DIR/recomp_funcs.h" "$SRC_DIR/recomp_funcs_real.h"
ln -sf "$GEN_DIR/recomp_types.h" "$SRC_DIR/recomp_types.h"
cp "$SHIM" "$SRC_DIR/recomp_funcs.h"

# Record what this object set was built from. A stale object set silently
# measured against a regenerated lifted tree is the failure this guards against.
{
    echo "gen_dir=$GEN_DIR"
    echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "compile_policy=O0-no-strict-aliasing-v1"
    echo "shim_sha=$(sha256sum "$SHIM" | cut -d' ' -f1)"
    echo "gen_headers_sha=$(cat "$GEN_DIR/recomp_funcs.h" "$GEN_DIR/recomp_types.h" | sha256sum | cut -d' ' -f1)"
    # Whole-tree digest, not just the two headers: a chunk body can change
    # without either header moving, and that is exactly what a re-lift does.
    # build_subject.sh stamps the first 16 digits of this into the binary.
    echo "tree_sha_full=$(harness_tree_digest "$GEN_DIR")"
    echo "weak_dir=${WEAK_DIR:-NONE}"
    if [[ -n "$WEAK_DIR" ]]; then
        # Which chunks were compiled with which weak header, by content.
        for weak in "$WEAK_DIR"/weak_recomp_*.h; do
            [[ -f "$weak" ]] && echo "weak_header=$(basename "$weak") $(sha256sum "$weak" | cut -d' ' -f1)"
        done
    fi
} > "$BUILD_DIR/provenance.txt"

if [[ -n "$REUSE_OBJ_DIR" ]]; then
    reuse_sha="$(sed -n 's/^tree_sha_full=//p' "$REUSE_PROVENANCE" | head -1)"
    now_sha="$(sed -n 's/^tree_sha_full=//p' "$BUILD_DIR/provenance.txt" | head -1)"
    if [[ -z "$reuse_sha" || "$reuse_sha" != "$now_sha" ]]; then
        echo "error: REUSE_OBJ_DIR was built from a different lifted tree" >&2
        echo "         reused: $reuse_sha" >&2
        echo "         now:    $now_sha" >&2
        exit 1
    fi
    reuse_shim_sha="$(sed -n 's/^shim_sha=//p' "$REUSE_PROVENANCE" | head -1)"
    now_shim_sha="$(sed -n 's/^shim_sha=//p' "$BUILD_DIR/provenance.txt" | head -1)"
    if [[ -z "$reuse_shim_sha" || "$reuse_shim_sha" != "$now_shim_sha" ]]; then
        echo "error: REUSE_OBJ_DIR was built with a different or unrecorded callee-stub shim" >&2
        exit 1
    fi
    reuse_policy="$(sed -n 's/^compile_policy=//p' "$REUSE_PROVENANCE" | head -1)"
    if [[ "$reuse_policy" != "O0-no-strict-aliasing-v1" ]]; then
        echo "error: REUSE_OBJ_DIR has no matching compiler policy" >&2
        exit 1
    fi
    if grep -q '^weak_dir=' "$REUSE_PROVENANCE" && ! grep -q '^weak_dir=NONE$' "$REUSE_PROVENANCE"; then
        echo "error: REUSE_OBJ_DIR was itself built with weak headers; reuse a BASELINE set" >&2
        exit 1
    fi
fi

compile_one() {
    local src="$1" obj_dir="$2" log_dir="$3" gen_dir="$4" cc="$5" src_dir="$6"
    local weak_dir="$7" reuse_dir="$8"
    local base weak_flags=()
    base="$(basename "$src")"
    if [[ -n "$weak_dir" && -f "$weak_dir/weak_${base%.c}.h" ]]; then
        weak_flags=(-include "$weak_dir/weak_${base%.c}.h")
    elif [[ -n "$reuse_dir" && -f "$reuse_dir/$base.o" ]]; then
        cp -p "$reuse_dir/$base.o" "$obj_dir/$base.o"
        return 0
    fi
    # -O0 and -fno-strict-aliasing match tools/harness/build_subject.sh: the
    # generated code type-puns heavily and is tested as written, not as the
    # optimizer reshapes it.
    if ! "$cc" -O0 -std=gnu11 -fno-strict-aliasing -w \
        -I"$src_dir" -I"$gen_dir" "${weak_flags[@]}" \
        -c "$src" -o "$obj_dir/$base.o" > "$log_dir/$base.log" 2>&1; then
        echo "FAILED $base" >&2
        return 1
    fi
}
export -f compile_one

echo "compiling $(ls "$SRC_DIR"/*.c | wc -l) chunk(s) with $JOBS job(s)" >&2
# A compile failure must fail the whole build rather than leaving a partial
# object set that links into a subject missing arbitrary functions.
printf '%s\n' "$SRC_DIR"/*.c \
    | xargs -P "$JOBS" -I{} bash -c \
        'compile_one "$@"' _ {} "$OBJ_DIR" "$LOG_DIR" "$GEN_DIR" "$CC" "$SRC_DIR" "$WEAK_DIR" "$REUSE_OBJ_DIR"

built=$(ls "$OBJ_DIR"/*.o 2>/dev/null | wc -l)
expected=$(ls "$SRC_DIR"/*.c | wc -l)
if [[ "$built" -ne "$expected" ]]; then
    echo "error: built $built object(s), expected $expected" >&2
    exit 1
fi

# Prove the interception actually took effect. Without this the build can
# succeed while silently producing an object set in which every call still goes
# straight to its callee -- which would make every stubbed verdict a lie.
#
# Counted across every object rather than probed on one: `grep -q` exits on the
# first match, which makes nm take SIGPIPE, which under `pipefail` turns a
# SUCCESSFUL match into a failed pipeline. That inverted check reported "the shim
# did not take effect" on a build where it demonstrably had.
#
# CHECKED PER TRANSLATION UNIT, NOT AS A COUNT. The bound used to be
# `intercepting -lt (expected - 2)`, hardcoding "exactly two translation units
# legitimately have no call sites". That is true of a default lift, where only the
# dispatch table and the unresolved-stub unit are call-free, and false of a
# --manual-functions lift, where recomp_xdk_manual.c is a third: it reported
# "only 64/67 objects reference harness_stub_call" and exited 1 after correctly
# producing all 67 objects.
#
# The fix is to stop guessing how many units should be call-free and ask each
# source whether it HAS a call to intercept. Both RECOMP_ABI_CALL (direct) and the
# RECOMP_ICALL family (indirect, which expands to RECOMP_ABI_CALL once the lookup
# resolves) route through the shim, so a source mentioning either must produce an
# object that references harness_stub_call. This is strictly STRONGER than the old
# count, not a widened bound: a count of 65 cannot tell which 65, so one genuinely
# un-intercepted unit hid behind the two it was allowed to lose.
missing=()
intercepting=0
with_calls=0
for src in "$SRC_DIR"/*.c; do
    base="$(basename "$src")"
    obj="$OBJ_DIR/$base.o"
    if [[ "$(nm -u "$obj" | grep -c harness_stub_call || true)" -gt 0 ]]; then
        intercepting=$((intercepting + 1))
        obj_intercepts=1
    else
        obj_intercepts=0
    fi
    # Matched as an INVOCATION, macro name immediately followed by `(`, not as a
    # bare mention: recomp_dispatch.c has a prose comment naming RECOMP_ICALL, and
    # a bare-name pattern failed the whole build on it. Deliberately excludes
    # RECOMP_ICALL_FAIL and RECOMP_ICALL_IS_CODE, which are the failure and
    # predicate arms of an indirect call rather than a call site.
    #
    # `grep` on a named path follows the symlink, so this reads the generated
    # source itself rather than the link.
    if grep -qE '(RECOMP_ABI_CALL|RECOMP_ICALL(_SAFE(_AT)?)?)[[:space:]]*\(' "$src"; then
        with_calls=$((with_calls + 1))
        if [[ "$obj_intercepts" -eq 0 ]]; then
            missing+=("$base")
        fi
    fi
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: ${#missing[@]} translation unit(s) contain call sites but their objects" >&2
    echo "       do not reference harness_stub_call, so the shim did not take effect" >&2
    echo "       and no stubbed verdict from them would be valid:" >&2
    for base in "${missing[@]}"; do
        echo "         $base" >&2
    done
    exit 1
fi
if [[ "$with_calls" -eq 0 ]]; then
    echo "error: no translation unit in $GEN_DIR contains a call site at all;" >&2
    echo "       either this is not a lifted tree or the grep pattern is wrong" >&2
    exit 1
fi
echo "interception verified: all $with_calls call-bearing unit(s) of $expected route" >&2
echo "       calls to the stub ($intercepting object(s) reference it in total)" >&2

echo "built $built stubbed object(s) in $OBJ_DIR" >&2
