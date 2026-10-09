#!/usr/bin/env python3
"""Mutation-test the C suites: apply a known defect, check that a suite notices.

WHY THIS EXISTS AS A COMMITTED TOOL RATHER THAN A SCRATCH SCRIPT. A passing test
proves the code runs, not that the test would object if the code were wrong. On this
project that gap has bitten repeatedly: three hand-written tests once passed with the
implementation they covered DELETED. So every change is supposed to be mutation
tested, and a one-off script that is thrown away means the next change starts from
nothing and the mutation set is never cumulative.

ITS FIRST RUN FOUND A REAL DEFECT, which is the argument for it. Swapping the two
volume-geometry writes in `kernel_io.c` leaves their product at 32 * 512 == 16384, so
the guest's own validation of that product could not catch it AND neither could
`test_kernel_io`, which deliberately asserted only the product. The suite now asserts
each factor against its hardware literal and the mutant dies.

THREE GUARDS AGAINST A FALSE RESULT, each for a failure this project has actually hit:

  - THE BINARY IS FINGERPRINTED before and after the rebuild. A stale binary gives a
    false PASS, and when two agents share one build directory they overwrite each
    other's output; two agents once got a false PASS against mutated source this way.
    An unchanged hash is reported as INVALID, never as a survivor. An unchanged hash is
    first CONFIRMED against a clean rebuild (`confirm_stale`, T501): after a killed
    mutant the binary on disk is that mutant's, so a mutant whose object code equals the
    previous one's (the compiler folds both to the same function) is a fresh build, and
    only a result equal to the clean build is a build that never happened.

  - A MUTATION THAT DOES NOT COMPILE IS NOT A SURVIVOR, it is NOT-A-MUTANT. A
    `_Static_assert` that rejects a bad constant is the defence working, and counting
    it as a kill would inflate the score while counting it as a survivor would demand
    a test for something unreachable.

  - OUTPUT IS DECODED WITH errors="replace". `test_disc_seam` prints raw bytes read off
    the disc, which are not UTF-8; a strict decode crashed an earlier version of this
    harness mid-run and silently skipped the mutations after it. A harness that stops
    early reports FEWER survivors than exist, which is the one failure mode it must
    not have.

THE SOURCE IS ALWAYS RESTORED, in a `finally`, and the tree is verified clean at the
end. A killed process leaves a mutation applied on disk, and the next agent to build
inherits it as a phantom failure in code they did not touch.

RUN IT IN A SCRATCH WORKTREE pinned to the commit under test, never in a checkout
another agent is building in -- the rebuilds here would race theirs.

    git worktree add /workspace/tmp/wt/mut -b mut/<slug> <sha>
    ln -s /workspace/discs /workspace/tmp/wt/mut/discs
    cmake -S /workspace/tmp/wt/mut -B /workspace/tmp/wt/mut/build && make -C .../build
    ./.venv/bin/python tools/mutate/c_suites.py --root /workspace/tmp/wt/mut
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

# Each entry names ONE load-bearing line and the defect to inject. `why` is the
# argument for why a survivor would matter, written before the run so a survivor
# cannot be rationalised after the fact.
MUTATIONS = [
    {
        "id": "disc-mount-trailing-separator-unguarded",
        "file": "src/xbox/kernel_file.c",
        "old": """    if (!prefix_has_no_trailing_separator(prefix, length)) {
        return false;
    }

    /* Open and VALIDATE here, not at the title's first read.""",
        "new": """
    /* Open and VALIDATE here, not at the title's first read.""",
        "targets": ["test_hdd_backing"],
        "why": "this is the state the disc mount was ACTUALLY IN before today: the rule "
        "existed for the writable mount and the disc one never called it, so a "
        "--disc-device ending in a separator mounted the bare device and resolved "
        "nothing beneath it while reporting success.",
    },
    {
        "id": "hdd-mount-trailing-separator-unguarded",
        "file": "src/xbox/kernel_file.c",
        "old": """    if (!prefix_has_no_trailing_separator(prefix, length)) {
        return false;
    }

    /*
     * OPENED AND VALIDATED HERE, not at the title's first create.""",
        "new": """
    /*
     * OPENED AND VALIDATED HERE, not at the title's first create.""",
        "targets": ["test_hdd_backing"],
        "why": "the other half of the shared rule. Both callers are mutated separately "
        "because one shared helper with one tested caller drifts just as surely as "
        "two copies.",
    },
    {
        "id": "io-swap-geometry-fields",
        "file": "src/xbox/kernel_io.c",
        "old": """        !kernel_guest_write_u32(fs_information + FS_SIZE_SECTORS_PER_UNIT_OFFSET,
                                sectors_per_unit) ||
        !kernel_guest_write_u32(fs_information + FS_SIZE_BYTES_PER_SECTOR_OFFSET,
                                bytes_per_sector)) {""",
        "new": """        !kernel_guest_write_u32(fs_information + FS_SIZE_SECTORS_PER_UNIT_OFFSET,
                                bytes_per_sector) ||
        !kernel_guest_write_u32(fs_information + FS_SIZE_BYTES_PER_SECTOR_OFFSET,
                                sectors_per_unit)) {""",
        "targets": ["test_kernel_io"],
        "why": "the guest validates the PRODUCT, so swapping the fields leaves "
        "32*512 intact and the guest cannot catch it. Only a per-field "
        "assertion can. THIS ONE SURVIVED on first run.",
    },
    {
        "id": "hal-49-registered-under-the-wrong-ordinal",
        "file": "src/xbox/kernel_hal.c",
        "old": "#define ORD_HalReturnToFirmware 49u",
        "new": "#define ORD_HalReturnToFirmware 48u",
        "targets": ["test_kernel_hal"],
        "why": "the handler can be perfectly correct and bound to the wrong number, "
        "which leaves 49 a stub and the boot terminus unreached. Deleting the "
        "binding row instead does not compile (-Werror on the now-unused "
        "function), so it is a NOT-A-MUTANT rather than a test of this.",
    },
    {
        "id": "hal-49-ignores-its-argument",
        "file": "src/xbox/kernel_hal.c",
        "old": "    last_firmware_routine = routine;",
        "new": "    last_firmware_routine = 2u;",
        "targets": ["test_kernel_hal"],
        "why": "three of the four measured sites push 2, so hardcoding 2 satisfies every "
        "common-case assertion and silently mislabels the one site pushing 4.",
    },
    {
        "id": "hal-49-sink-return-not-reported",
        "file": "src/xbox/kernel_hal.c",
        "old": '        kernel_hle_log()("kernel: the firmware sink RETURNED from "',
        "new": '        if (0) kernel_hle_log()("kernel: the firmware sink RETURNED from "',
        "targets": ["test_kernel_hal"],
        "why": "a sink that comes back means the run continued past a reboot. Silence "
        "there is this project's worst failure mode: a plausible, wrong trace.",
    },
    {
        "id": "hal-49-reset-detaches-the-sink",
        "file": "src/xbox/kernel_hal.c",
        "old": "    firmware_return_count = 0u;\n    last_firmware_routine = 0u;",
        "new": "    firmware_return_count = 0u;\n    last_firmware_routine = 0u;\n"
        "    firmware_sink = NULL;",
        "targets": ["test_kernel_hal"],
        "why": "host wiring must survive a reset. Clearing it leaves the next reboot "
        "request unable to stop the run, with nothing saying the sink had gone.",
    },
    {
        "id": "hal-49-pending-count-hardcoded-zero",
        "file": "src/xbox/kernel_hal.c",
        "old": "    const unsigned pending = kernel_hal_shutdown_count();",
        "new": "    const unsigned pending = 0u;",
        "targets": ["test_kernel_hal"],
        "why": "the pending count is the only true part of the claim that the real "
        "kernel walks the shutdown set. Zero is also the common value, so a case "
        "with no live registrations could not tell the difference.",
    },
    {
        "id": "hdd-escape-check-removed",
        "file": "src/xbox/kernel_file.c",
        "old": """    if (length == 1u && component[0] == '.') {
        return true;
    }
    return length == 2u && component[0] == '.' && component[1] == '.';""",
        "new": """    (void)component;
    (void)length;
    return false;""",
        "targets": ["test_hdd_backing"],
        "why": "the guest is untrusted input and `..` is the whole escape. Without this "
        "check a guest path walks straight out of the backing directory, and the "
        "only thing stopping it writing anywhere the host user can is the absence "
        "of a write path.",
    },
    {
        "id": "hdd-symlink-traversal-followed",
        "file": "src/xbox/kernel_file.c",
        "old": "            if (S_ISLNK(info.st_mode)) {",
        "new": "            if (false && S_ISLNK(info.st_mode)) {",
        "targets": ["test_hdd_backing"],
        "why": "refusing `..` alone is not enough: a host symbolic link planted inside "
        "the backing directory is an ordinary-looking component that resolves "
        "anywhere. This is the half a reviewer forgets, and the O_NOFOLLOW left "
        "behind reports the WRONG status for it, not the right one.",
    },
    {
        "id": "hdd-symlink-leaf-not-refused",
        "file": "src/xbox/kernel_file.c",
        # Two lines, because `str.count` matches substrings: the one-line version of this
        # anchor also occurs inside the more deeply indented check in the walk, and the
        # harness correctly reported ANCHOR-DRIFT rather than mutating the wrong site.
        "old": "    if (S_ISLNK(info.st_mode)) {\n"
        "        /* Visible here BECAUSE of AT_SYMLINK_NOFOLLOW",
        "new": "    if (false && S_ISLNK(info.st_mode)) {\n"
        "        /* Visible here BECAUSE of AT_SYMLINK_NOFOLLOW",
        "targets": ["test_hdd_backing"],
        "why": "a link as the FINAL component is refused in a different place from one "
        "traversed on the way through. Dropping this still refuses the open, via "
        "the not-a-regular-file arm, so only the escape COUNTER can tell -- and a "
        "refusal nobody counts is a refusal nobody notices stopping.",
    },
    {
        "id": "hdd-dup-shares-the-directory-offset",
        "file": "src/xbox/kernel_file.c",
        "old": '    const int scan_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);',
        "new": "    const int scan_fd = fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);",
        "targets": ["test_hdd_backing"],
        "why": "THIS DEFECT ACTUALLY HAPPENED during development. `dup` shares the file "
        "offset, so the first directory scan leaves it at EOF and every later scan "
        "reads nothing and reports the name absent. It presented as a created "
        "`TData` sitting beside an existing `TDATA`.",
    },
    {
        "id": "hdd-case-scan-is-case-sensitive",
        "file": "src/xbox/kernel_file.c",
        "old": "        if (!paths_equal(entry->d_name, wanted)) {",
        "new": "        if (strcmp(entry->d_name, wanted) != 0) {",
        "targets": ["test_hdd_backing"],
        "why": "FATX is case-insensitive and a Linux host is not. This binary spells its "
        "own names two ways, so a directory created as TDATA has to be findable as "
        "tdata; otherwise the miss reads as 'the save data is gone'.",
    },
    {
        "id": "hdd-disc-guard-removed",
        "file": "src/xbox/kernel_file.c",
        "old": "            volume != NULL && volume->backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "new": "            volume != NULL) {",
        "targets": ["test_hdd_backing"],
        "why": "the user's disc image is their property and must never reach a creating "
        "path. This is the REACHABLE guard; the one inside hostdir_create_locked "
        "is defence in depth behind it.",
    },
    {
        "id": "hdd-writable-without-a-mount",
        "file": "src/xbox/kernel_file.c",
        "old": "            volume != NULL && volume->backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "new": "            volume->backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "targets": ["test_hdd_backing"],
        "why": "without --hdd there is no volume, and creation must not be attempted at "
        "all. Expected to be killed by a NULL dereference rather than an assertion, "
        "which is still detection -- and note that the TRUEST form of this default, "
        "`out->hdd_path = NULL` in src/host/main.c, is reachable by NO C suite, "
        "because this harness drives ctest binaries and tsfp_host is not one.",
    },
    {
        "id": "hdd-mount-accepts-a-regular-file",
        "file": "src/xbox/kernel_file.c",
        # Anchored on the comment that ends just above it: this open appears TWICE now (the
        # writable mount and the cache-partition mount), and an anchor matching twice is
        # reported as drift rather than silently mutating the wrong one.
        "old": "     * writable volume.\n     */\n"
        "    const int root_fd = open(host_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);",
        "new": "     * writable volume.\n     */\n"
        "    const int root_fd = open(host_dir, O_RDONLY | O_CLOEXEC);",
        "targets": ["test_hdd_backing"],
        "why": "O_DIRECTORY is the STRUCTURAL reason an operator cannot hand their ISO to "
        "--hdd. Without it the mount succeeds on a regular file and the disc image "
        "becomes a writable volume, which is the one outcome this whole design is "
        "built to make impossible rather than merely unlikely.",
    },
    {
        "id": "hdd-open-if-treated-as-file-create",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (resolved && disposition == FILE_CREATE) {",
        "new": "    if (resolved && (disposition == FILE_CREATE || disposition == FILE_OPEN_IF)) {",
        "targets": ["test_hdd_backing"],
        "why": "the measured boot creates TDATA and then opens it again on the next pass. "
        "If FILE_OPEN_IF failed on an existing name the title would take the same "
        "XLaunchNewImage reboot for a new reason, so the fix would look like no fix.",
    },
    {
        "id": "hdd-created-reported-as-opened",
        "file": "src/xbox/kernel_file.c",
        "old": "                if (resolved) {\n"
        "                    information = FILE_INFORMATION_CREATED;\n"
        "                }",
        "new": "                if (resolved) {\n"
        "                    information = FILE_INFORMATION_OPENED;\n"
        "                }",
        "targets": ["test_hdd_backing"],
        "why": "the guest READS IO_STATUS_BLOCK.information, measured at 0x0037D394. "
        "Saying 'opened' after a create tells the title its save data was already "
        "there, and STATUS_SUCCESS is identical either way so nothing else can tell.",
    },
    {
        "id": "hdd-missing-name-falls-through-to-the-policy",
        "file": "src/xbox/kernel_file.c",
        "old": "        return hostdir_resolve_locked(volume, path, rest, desired_access, out,\n"
        "                                      out_status);",
        "new": "        if (hostdir_resolve_locked(volume, path, rest, desired_access, out,\n"
        "                                   out_status)) {\n"
        "            return true;\n"
        "        }",
        "targets": ["test_hdd_backing"],
        "why": "`resolve_locked`'s `declared` line counts ANY mounted volume as declared, "
        "so a host-directory arm that forgets to return early fabricates an empty "
        "file on a volume whose real contents we can list -- and does not even "
        "count it as a fabrication.",
    },
    {
        "id": "hdd-directory-read-answered-empty",
        "file": "src/xbox/kernel_file.c",
        "old": "                             entry->state.path);\n            return false;",
        "new": "                             entry->state.path);\n            return true;",
        "targets": ["test_hdd_backing"],
        "why": "a read of a directory answered with zero bytes reads as END OF FILE, so a "
        "title told its save directory is an empty file concludes the data is gone. "
        "Listing one is ordinal 207 and is not implemented.",
    },
    {
        "id": "hdd-host-read-falls-into-the-empty-arm",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (entry->state.backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "new": "    if (false && entry->state.backing == KERNEL_FILE_BACKING_HOST_DIR) {",
        "targets": ["test_hdd_backing"],
        "why": "the fabricated-empty arm reports SUCCESS with 0 bytes transferred, so a "
        "writable volume would be write-only: the title could create its save file "
        "and never read it back, and the read would not even fail.",
    },
    {
        "id": "hdd-reset-leaves-the-volume-mounted",
        "file": "src/xbox/kernel_file.c",
        "old": "    kernel_file_unmount_all();\n    lock();\n"
        "    memset(openable, 0, sizeof(openable));",
        "new": "    lock();\n    memset(openable, 0, sizeof(openable));",
        "targets": ["test_hdd_backing", "test_kernel_file"],
        "why": "a reset that leaves a writable volume mounted makes one test's hard disk "
        "the next one's silent default -- the exact failure this feature is designed "
        "against, arriving through the test suite instead of the command line. It "
        "also leaks the disc image's descriptor on every reset.",
    },
    {
        "id": "xdvdfs-nil-max-not-recognised",
        "file": "src/xbox/xdvdfs.c",
        "old": "    if (link == DIRENT_LINK_NIL_ZERO || link == DIRENT_LINK_NIL_MAX) {",
        "new": "    if (link == DIRENT_LINK_NIL_ZERO) {",
        "targets": ["test_xdvdfs_c", "test_disc_seam"],
        "why": "0xFFFF is one of the two nil encodings and is exactly what 0xFF "
        "padding decodes to, so not recognising it walks into padding.",
    },
    {
        "id": "xdvdfs-nil-zero-not-recognised",
        "file": "src/xbox/xdvdfs.c",
        "old": "    if (link == DIRENT_LINK_NIL_ZERO || link == DIRENT_LINK_NIL_MAX) {",
        "new": "    if (link == DIRENT_LINK_NIL_MAX) {",
        "targets": ["test_xdvdfs_c", "test_disc_seam"],
        "why": "0 is BOTH a nil link and the offset of the root node, so failing to "
        "recognise it re-pushes the root. Expected to hang rather than fail, "
        "which is why the per-suite timeout is a kill and not an error.",
    },
    {
        "id": "xdvdfs-link-unit-2-not-4",
        "file": "src/xbox/xdvdfs.c",
        "old": "    walk->pending[walk->pending_count] = (uint32_t)link * 4u;",
        "new": "    walk->pending[walk->pending_count] = (uint32_t)link * 2u;",
        "targets": ["test_xdvdfs_c", "test_disc_seam"],
        "why": "directory links are in 4-byte units; a wrong multiplier lands "
        "mid-entry and decodes as garbage rather than failing cleanly.",
    },
    # ---------------------------------------------------------------------
    # The address-keyed XDK dispatch path (src/host/xdk_thunk.c).
    #
    # Everything here is about a failure that DOES NOT CRASH. A wrong row, a
    # wrong module, a guessed argument count or a relaxed stop policy all
    # produce a run that keeps going and lies, which is why the suite asserts
    # each of them separately rather than only checking that a call arrives.
    # ---------------------------------------------------------------------
    {
        "id": "xdk-lookup-returns-neighbour",
        "file": "src/host/xdk_thunk.c",
        "old": """        } else {
            hi = mid;
        }
    }
    return NULL;
}""",
        "new": """        } else {
            hi = mid;
        }
    }
    return lo > 0u ? &g_rows[lo - 1u] : NULL;
}""",
        "targets": ["test_xdk_dispatch"],
        "why": "a sorted table searched sloppily answers for an address it does "
        "not hold, and the answer is the PREVIOUS row -- a different XDK "
        "function, silently, from a different module. Only an exact-match "
        "assertion on a near miss catches it.",
    },
    {
        "id": "xdk-stop-on-missing-inverted",
        "file": "src/host/xdk_thunk.c",
        "old": """        if (g_stop_on_missing) {
            host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, label);
        }""",
        "new": """        if (!g_stop_on_missing) {
            host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, label);
        }""",
        "targets": ["test_xdk_dispatch"],
        "why": "inverted, an unimplemented address returns the module's stub "
        "default by DEFAULT. That is the plausible, wrong trace that "
        "cannot be falsified from the inside -- this project's worst "
        "failure mode -- and it looks like progress.",
    },
    {
        "id": "xdk-arity-refusal-removed",
        "file": "src/host/xdk_thunk.c",
        "old": "    if (!row->abi_known) {",
        "new": "    if (false) {",
        "targets": ["test_xdk_dispatch"],
        "why": "without the refusal an address with no established convention is "
        "called anyway and esp is adjusted by a zeroed row's arithmetic. "
        "__stdcall is callee-cleanup, so the modelled esp desyncs "
        "permanently and the damage surfaces arbitrarily far away.",
    },
    {
        "id": "xdk-routes-d3d-to-dsound",
        "file": "src/host/xdk_thunk.c",
        "old": """    case XDK_MODULE_D3D8:
        return d3d8_hle_call(row->address, frame);""",
        "new": """    case XDK_MODULE_D3D8:
        return dsound_hle_call(row->address, frame);""",
        "targets": ["test_xdk_dispatch"],
        "why": "the WRONG module answers. dsound_hle_call reports an address "
        "absent from its own table and returns 0, so the guest gets a "
        "fabricated value and a log line that blames the audio surface "
        "for a graphics call. A one-directional routing test misses this.",
    },
    {
        "id": "xdk-unknown-section-routed-to-d3d",
        "file": "src/host/xdk_thunk.c",
        "old": """    return XDK_MODULE_NONE;
}

const char *xdk_cc_name(xdk_cc cc)""",
        "new": """    return XDK_MODULE_D3D8;
}

const char *xdk_cc_name(xdk_cc cc)""",
        "targets": ["test_xdk_dispatch"],
        "why": "XGRPH, XNET, XONLINE and XMV are 102 of the 236 rows and have no "
        "HLE module. Routing them to the nearest one answers 102 "
        "addresses from a subsystem with no business seeing them.",
    },
    {
        "id": "xdk-measured-quorum-one-site",
        "file": "src/host/xdk_thunk.h",
        "old": "#define XDK_MEASURED_ARITY_MIN_SITES 3u",
        "new": "#define XDK_MEASURED_ARITY_MIN_SITES 1u",
        "targets": ["test_xdk_dispatch"],
        "why": "one voter always agrees with itself, so a one-site row is "
        "flagged unanimous and means nothing. On the kernel side that "
        "admitted ordinal 196 at twelve arguments from a single site, "
        "which is more than that export takes.",
    },
    {
        "id": "xdk-measured-unanimity-ignored",
        "file": "src/host/xdk_thunk.c",
        # Anchored on the comment close above it: the callee path added a second
        # `if (!unanimous)` later in the file, so the bare line matches twice.
        "old": "     */\n    if (!unanimous) {",
        # `if (false)` here is a NOT-A-MUTANT: it leaves `unanimous` unused and
        # -Wunused-parameter -Werror rejects it, which would be scored as a
        # compile-time guard rather than as the defect it is meant to inject. The
        # flag still has to be READ for the mutation to compile, and ignored for
        # the mutation to bite.
        "new": "     */\n    if (!unanimous && false) {",
        "targets": ["test_xdk_dispatch"],
        "why": "of 13 kernel ordinals whose arity was independently verified, the "
        "measured table was wrong for 6 and all six are flagged "
        "non-unanimous. Ignoring the flag accepts a minimum as a count.",
    },
    {
        "id": "xdk-cdecl-pops-arguments",
        "file": "src/host/xdk_thunk.c",
        "old": """    if (row->cc == XDK_CC_CDECL) {
        return 4u;
    }""",
        "new": """    if (row->cc == XDK_CC_CDECL) {
        return 4u + 4u * row->stack_args;
    }""",
        "targets": ["test_xdk_dispatch"],
        "why": "cdecl is CALLER cleanup. Popping the arguments as well eats the "
        "caller's own locals, and the lifter's per-function headers "
        "classify several D3D entries as cdecl, so this is live.",
    },
    {
        "id": "xdk-missing-call-not-traced",
        "file": "src/host/xdk_thunk.c",
        "old": """    const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address,
                                                 return_address, implemented);""",
        "new": """    const size_t slot = implemented
                            ? thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address,
                                                         return_address, implemented)
                            : THUNK_TRACE_NO_SLOT;""",
        "targets": ["test_xdk_dispatch"],
        "why": "the stop is a siglongjmp that never returns, so a call recorded "
        "after it is never recorded at all. The trace would then END "
        "WITHOUT the thing that stopped the run, which is the one entry a "
        "reader needs.",
    },
    {
        "id": "xdk-frame-off-by-one-slot",
        "file": "src/host/xdk_thunk.c",
        "old": "        .stack_ptr = (kernel_guest_ptr)g_esp,",
        "new": "        .stack_ptr = (kernel_guest_ptr)(g_esp + 4u),",
        "targets": ["test_xdk_dispatch"],
        "why": "g_esp points at the return address, so argument N is at +4+4N. "
        "Shifting the base makes every handler read argument N+1 and the "
        "last one read off the end -- a plausible wrong value, never a "
        "fault.",
    },
    # --- the Prcb debug-monitor notify (src/host/monitor_thunk.c) --------------
    #
    # EVERY ONE OF THESE TARGETS test_monitor_thunk, AND NONE OF THEM COULD IF THE
    # NOTIFY HAD BEEN BUILT WHERE IT WAS PROPOSED. The proposed home was
    # `recomp_lookup_kernel` in src/host/kernel_thunk.c, which is compiled into
    # tsfp_host alone -- and tsfp_host is not a ctest binary, so no mutation can
    # reach it. That is this harness's own first documented trap, and it is why the
    # notify is answered from `recomp_lookup_manual` in the tsfp_thunk library
    # instead. The one piece of this change that does live in kernel_thunk.c (the
    # pre-append that finally put ordinal 49 in the ordered trace) is for that reason
    # NOT mutation-testable, and is recorded here as a gap rather than left to look
    # like coverage.
    {
        "id": "monitor-va-unaligned-in-the-window",
        "file": "src/host/monitor_thunk.h",
        "old": "#define MONITOR_THUNK_NOTIFY_VA KERNEL_THUNK_VA(MONITOR_THUNK_NOTIFY_SLOT)",
        "new": "#define MONITOR_THUNK_NOTIFY_VA (KERNEL_THUNK_VA_BASE + MONITOR_THUNK_NOTIFY_SLOT)",
        "targets": ["test_monitor_thunk"],
        "why": "this is the version of 'the VA points outside the window' that "
        "actually COMPILES: base+379 is still inside the mapped page and still "
        "passes every _Static_assert, but it decodes to ordinal 94, so the "
        "window's own addressing disagrees with the slot the module documents "
        "and a stop address would send a reader to the wrong export. Moving "
        "the VA fully outside the window instead is a NOT-A-MUTANT, which is "
        "the compile-time guard working.",
    },
    {
        "id": "monitor-slot-inside-the-kernel-ordinals",
        "file": "src/host/monitor_thunk.h",
        "old": "#define MONITOR_THUNK_NOTIFY_SLOT (XBOX_KERNEL_ORDINAL_MAX + 1u)",
        "new": "#define MONITOR_THUNK_NOTIFY_SLOT 300u",
        "targets": ["test_monitor_thunk"],
        "why": "a slot below XBOX_KERNEL_ORDINAL_MAX shadows a real kernel export, "
        "so a call to ordinal 300 would become a monitor notification and pop "
        "2 arguments whatever that export takes. EXPECTED TO BE SCORED "
        "NOT-A-MUTANT: the _Static_assert in monitor_thunk.h rejects it, which "
        "is the defence working. Kept so that deleting the assertion is itself "
        "caught -- without the assertion this becomes a live mutant that the "
        "slot-adjacency check then kills.",
    },
    {
        "id": "monitor-pops-one-argument",
        "file": "src/host/monitor_thunk.c",
        "old": "    g_esp += 4u + 4u * MONITOR_THUNK_NOTIFY_STACK_ARGS;",
        "new": "    g_esp += 4u + 4u * 1u;",
        "targets": ["test_monitor_thunk"],
        "why": "__stdcall is callee cleanup, so the pop is ours. Popping 8 leaves "
        "4 bytes of the caller's arguments on its stack and esp never "
        "recovers. MEASURED arity is 2, unanimous across all 8 call sites and "
        "proven by the stack balance in sub_0037FDE1, which reads its own "
        "incoming argument at [esp+4] after the call returns.",
    },
    {
        "id": "monitor-pops-three-arguments",
        "file": "src/host/monitor_thunk.c",
        "old": "    g_esp += 4u + 4u * MONITOR_THUNK_NOTIFY_STACK_ARGS;",
        "new": "    g_esp += 4u + 4u * 3u;",
        "targets": ["test_monitor_thunk"],
        "why": "the other direction, and the worse one: over-popping eats the "
        "caller's own locals. Both directions are mutated because a test that "
        "only checked `esp moved` would pass for either.",
    },
    {
        "id": "monitor-argument-order-swapped",
        "file": "src/host/monitor_thunk.c",
        "old": """    const bool read_code = kernel_frame_arg(&frame, 0u, &code);
    const bool read_pointer = kernel_frame_arg(&frame, 1u, &pointer);""",
        "new": """    const bool read_code = kernel_frame_arg(&frame, 1u, &code);
    const bool read_pointer = kernel_frame_arg(&frame, 0u, &pointer);""",
        "targets": ["test_monitor_thunk"],
        "why": "the guest pushes ptr first and code second, so code is argument 0. "
        "Swapped, a pointer is reported as a notification code -- and because "
        "4 of the 8 sites push 0 for ptr, half the trace would read 'code 0' "
        "and look entirely plausible.",
    },
    {
        "id": "monitor-frame-off-by-one-slot",
        "file": "src/host/monitor_thunk.c",
        "old": "        .stack_ptr = (kernel_guest_ptr)g_esp,",
        "new": "        .stack_ptr = (kernel_guest_ptr)(g_esp + 4u),",
        "targets": ["test_monitor_thunk"],
        "why": "g_esp points at the return address, so argument N is at +4+4N. "
        "Shifting the base reports the pointer as the code and reads one slot "
        "past the arguments for the pointer -- a plausible wrong value, never "
        "a fault.",
    },
    {
        "id": "monitor-notification-not-traced",
        "file": "src/host/monitor_thunk.c",
        "old": """    (void)thunk_trace_append(THUNK_KIND_MONITOR, 0u, MONITOR_THUNK_NOTIFY_VA,
                             return_address, 0u, true);
""",
        "new": "",
        "targets": ["test_monitor_thunk"],
        "why": "a notification the guest believes succeeded is a deliberate "
        "divergence from a retail console, which has no monitor block and "
        "never makes this call at all. An invisible divergence is the one kind "
        "this project cannot afford, and the ordered trace is where it has to "
        "show up.",
    },
    {
        "id": "monitor-traced-as-a-kernel-ordinal",
        "file": "src/host/monitor_thunk.c",
        "old": "    (void)thunk_trace_append(THUNK_KIND_MONITOR, 0u, MONITOR_THUNK_NOTIFY_VA,",
        "new": "    (void)thunk_trace_append(THUNK_KIND_ORDINAL, 0u, MONITOR_THUNK_NOTIFY_VA,",
        "targets": ["test_monitor_thunk"],
        "why": "the kernel-ordinal and XDK-address totals are how a reader judges "
        "how much of the CONSOLE's own surface the guest reached. A synthetic "
        "VA this host invented is not part of it, so folding it in inflates "
        "the one number the run report exists to state honestly.",
    },
    {
        "id": "monitor-not-routed-by-the-production-lookup",
        "file": "src/host/xdk_thunk.c",
        "old": """    recomp_func_t notify = monitor_thunk_lookup(xbox_va);
    if (notify) {
        return notify;
    }""",
        # `if (false)` here would leave `notify` unused and -Werror would reject it,
        # scoring a NOT-A-MUTANT that reads like evidence. The value still has to be
        # READ for the mutation to compile and ignored for it to bite.
        "new": """    recomp_func_t notify = monitor_thunk_lookup(xbox_va);
    if (notify && false) {
        return notify;
    }""",
        "targets": ["test_monitor_thunk"],
        "why": "THE src/gpu FAILURE, exactly: a complete, correct boundary with no "
        "caller. recomp_lookup_manual is what the lifter's own dispatch macros "
        "consult first; without this the guest's call falls through to "
        "recomp_lookup and recomp_lookup_kernel, both of which refuse a slot "
        "above XBOX_KERNEL_ORDINAL_MAX, and the run stops at a NULL indirect "
        "call again with every unit test still green.",
    },
    {
        "id": "monitor-lookup-answers-a-range",
        "file": "src/host/monitor_thunk.c",
        "old": "    if (va != MONITOR_THUNK_NOTIFY_VA) {",
        "new": "    if (va < MONITOR_THUNK_NOTIFY_VA) {",
        "targets": ["test_monitor_thunk"],
        "why": "a range test swallows every slot above the ordinals, so the next "
        "synthetic callable added to the window silently becomes a monitor "
        "notification -- and pops 2 arguments whatever its own arity is.",
    },
    {
        "id": "monitor-callback-never-written",
        "file": "src/xbox/kernel_thread.c",
        "old": """    if (g_monitor_callback != 0u
        && !kernel_guest_write_u32(monitor_base + KERNEL_MONITOR_CALLBACK,
                                   g_monitor_callback)) {
        return false;
    }""",
        "new": """    if (g_monitor_callback != 0u && false) {
        return false;
    }""",
        "targets": ["test_monitor_thunk"],
        "why": "the dispatcher can be perfectly correct and never reached, because "
        "the guest finds its target by reading monitor+0x14. This is the "
        "injection half of the same reachability question the lookup mutation "
        "asks, and it has to be asked separately: one of the two working is "
        "not the chain working.",
    },
    {
        "id": "monitor-callback-at-the-wrong-offset",
        "file": "src/xbox/kernel_thread.c",
        "old": "        && !kernel_guest_write_u32(monitor_base + KERNEL_MONITOR_CALLBACK,",
        "new": "        && !kernel_guest_write_u32(monitor_base + KERNEL_MONITOR_BLOCK_A,",
        "targets": ["test_monitor_thunk"],
        "why": "+0x14 is the called function pointer and +0x20 is an OPTIONAL shared "
        "block the guest writes 0x24 bytes through when it is non-null. Writing "
        "the VA there leaves the notify null AND arms a handshake nothing has "
        "implemented, so it breaks the fix and invents a second divergence.",
    },
    {
        "id": "trace-total-ignores-the-third-kind",
        "file": "src/host/thunk_trace.c",
        "old": """    uint64_t snapshot = 0u;
    for (unsigned kind = 0; kind < (unsigned)THUNK_KIND_COUNT; kind++) {
        snapshot += g_total[kind];
    }""",
        "new": "    const uint64_t snapshot = g_total[0] + g_total[1];",
        "targets": ["test_monitor_thunk"],
        "why": "this is the state the function was in before a third kind existed. "
        "A grand total that silently omits a kind makes a complete trace look "
        "truncated, which is the one failure mode a total must not have.",
    },
    {
        "id": "trace-reset-leaves-the-third-kind",
        "file": "src/host/thunk_trace.c",
        "old": """    for (unsigned kind = 0; kind < (unsigned)THUNK_KIND_COUNT; kind++) {
        g_total[kind] = 0u;
    }""",
        "new": """    g_total[0] = 0u;
    g_total[1] = 0u;""",
        "targets": ["test_monitor_thunk"],
        "why": "a reset that clears two of three counters leaks one suite's calls "
        "into the next one's totals, and every per-kind assertion after the "
        "first becomes order-dependent.",
    },
    {
        "id": "trace-total-only-counts-what-fits",
        "file": "src/host/thunk_trace.c",
        "old": "    g_total[total_index(kind)]++;",
        "new": "    if (g_trace_count < THUNK_TRACE_MAX) {\n"
        "        g_total[total_index(kind)]++;\n    }",
        "targets": ["test_xdk_dispatch"],
        "why": "a run that makes more than 512 calls would report exactly 512 and "
        "a reader would conclude the guest stopped there. The total has to "
        "keep counting what the array dropped.",
    },
]


#: Extra mutation sets, one module per file, loaded from `tools/mutate/sets/`.
#:
#: WHY A DIRECTORY RATHER THAN ONE LIST. Five concurrent tasks each need to add their own
#: mutations, and five concurrent edits to one list clobber each other -- that has already
#: happened on this project, and a lost mutation looks exactly like a mutation that was
#: never written. One file per owner means no two tasks touch the same bytes.
#:
#: Each file defines a module-level `MUTATIONS` list in the same shape as the one above.
#: A file that fails to import is REPORTED AND FATAL, never skipped: a mutation set that
#: silently does not load reports fewer survivors than exist, which is the one failure
#: mode this harness must not have.
SETS_DIR = pathlib.Path(__file__).parent / "sets"


SKIP_RETURN_CODE = 77  # ctest SKIP_RETURN_CODE of the Vulkan suites (no device)
SKIPPED_NO_DEVICE = "SKIPPED-NO-DEVICE"

#: A target of the form "pytest:<arguments>" runs pytest in --root instead of a ctest binary,
#: for coverage that lives in a Python test which builds its own runner from the mutated C
#: source (T259: the C-versus-Python combiner plan parity). A mutation using one must ALSO
#: list at least one ctest binary: that binary's rebuild is the compile guard, because a
#: mutation that fails to compile would make the pytest runner build fail, and a failed build
#: reads exactly like a kill.
PYTEST_PREFIX = "pytest:"


def binary_targets(mutation: dict) -> list[str]:
    return [t for t in mutation["targets"] if not t.startswith(PYTEST_PREFIX)]


def run_pytest_target(target: str, root: pathlib.Path, timeout: int) -> str:
    """Run one pytest target and return "killed", "passed" or "skipped".

    Raises (so the harness records ERRORED, never a kill) on anything that is not a clean
    verdict: a collection or usage error, a fixture ERROR with no test FAILED, no tests.
    """
    arguments = shlex.split(target[len(PYTEST_PREFIX) :])
    done = run(
        [sys.executable, "-m", "pytest", "-q", "-x", "-p", "no:cacheprovider", *arguments],
        timeout,
        cwd=root,
    )
    summary = done.stdout.strip().splitlines()[-1] if done.stdout.strip() else ""
    if done.returncode == 1 and re.search(r"\d+ failed", summary):
        return "killed"
    if done.returncode == 0 and re.search(r"\d+ skipped", summary):
        return "skipped"
    if done.returncode == 0:
        return "passed"
    raise RuntimeError(
        f"{target}: pytest exit {done.returncode} without a failed test: {summary!r}"
    )


#: Verdict of a mutation whose only kills came from pytest suites that already fail on the
#: clean tree (T543). Neither killed nor survived: no verdict, and it fails the run.
BASELINE_FAILING = "BASELINE-FAILING"


def pytest_baseline(
    target: str,
    root: pathlib.Path,
    build: pathlib.Path,
    args: argparse.Namespace,
    baselines: dict[str, str],
) -> str:
    """Return "passed", "skipped" or "failing" for `target` run on the CLEAN tree (T543).

    A `pytest:` suite that already fails without any mutation "kills" every mutant that
    selects it, so a kill is only evidence when the same selection passes on the baseline.
    Cached per run in `baselines`, keyed by the normalised selection: one clean run per
    distinct selection however many mutants use it. The tree is clean and the same for every
    mutant of a run (each restores its source), so the selection alone identifies the
    baseline. The CALLER must have restored the source first. Before a cache miss runs, the
    whole build directory is rebuilt clean, because some suites run a built binary
    (`build/tsfp_shader_probe`) and the binary on disk is the last mutant's.

    A hang counts as failing (a clean tree must not hang). A collection or usage error
    raises, so the mutation is ERRORED, and the error is cached so it is not retried.
    """
    key = shlex.join(shlex.split(target[len(PYTEST_PREFIX) :]))
    if key not in baselines:
        rebuilt = run(["cmake", "--build", str(build)], args.build_timeout, cwd=build)
        if rebuilt.returncode != 0:
            raise RuntimeError(f"{target}: the clean rebuild before its baseline run failed")
        try:
            verdict = {"killed": "failing", "passed": "passed", "skipped": "skipped"}[
                run_pytest_target(target, root, args.pytest_timeout)
            ]
        except subprocess.TimeoutExpired:
            verdict = "failing"
        except RuntimeError as error:
            verdict = f"error: {error}"
        baselines[key] = verdict
        if verdict == "failing":
            print(f"BASELINE {target} FAILS on the clean tree, so it cannot kill any mutant")
    if baselines[key].startswith("error: "):
        raise RuntimeError(f"baseline of {baselines[key][len('error: ') :]}")
    return baselines[key]


def load_mutation_sets(sets_dir: pathlib.Path) -> tuple[list[dict], list[str]]:
    """Return (mutations, names) from every module in `sets_dir`, sorted by filename."""
    import importlib.util

    mutations: list[dict] = []
    names: list[str] = []
    if not sets_dir.is_dir():
        return mutations, names
    for path in sorted(sets_dir.glob("*.py")):
        if path.name.startswith("_"):
            continue
        spec = importlib.util.spec_from_file_location(f"_mutset_{path.stem}", path)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"{path}: could not be loaded as a module")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        found = getattr(module, "MUTATIONS", None)
        if not isinstance(found, list):
            raise RuntimeError(f"{path}: defines no module-level MUTATIONS list")
        mutations.extend(found)
        names.append(f"{path.name} ({len(found)})")
    return mutations, names


def fingerprint(path: pathlib.Path) -> str:
    """Hash a built binary, so a rebuild that did not happen cannot pass unnoticed."""
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()[:16]


def clear_stale_bytecode(source: pathlib.Path) -> None:
    """Remove `__pycache__` beside a mutated `.py` file. No-op for anything else.

    CPython validates cached bytecode on source mtime TO THE SECOND plus size, so two
    same-size mutations of one module written within the same second reuse the first
    one's bytecode, and a mutation that was never loaded looks exactly like a survivor.
    Called after EVERY write to a `.py` entry (mutate and restore), so neither the run
    under test nor the next build can import stale bytecode.
    """
    if source.suffix == ".py":
        shutil.rmtree(source.parent / "__pycache__", ignore_errors=True)


def run(
    command: list[str], timeout: int, cwd: pathlib.Path | None = None
) -> subprocess.CompletedProcess[str]:
    """Run a command as an argument list. No shell: nothing here needs one."""
    return subprocess.run(  # noqa: S603 -- argument list, never a shell string
        command,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        cwd=cwd,
        check=False,
    )


def confirm_stale(
    mutation: dict,
    stale: list[str],
    source: pathlib.Path,
    original: str,
    binaries: list[str],
    build: pathlib.Path,
    build_timeout: int,
) -> list[str]:
    """Return the targets that are STILL stale once measured against a clean rebuild.

    `before` is whatever binary is on disk, which after a killed mutant is THAT mutant's
    binary (the source is restored but nothing is rebuilt). A mutant whose object code
    equals the previous mutant's (a validation the compiler proves unsatisfiable folds to
    `return false`) therefore looks like a build that never happened (T501). So on a stale
    verdict: restore the source, rebuild for a clean fingerprint, re-apply, rebuild. A
    target is genuinely stale only if the re-applied build equals the clean one, because
    then the mutation never reached the binary. Equal to the leftover but not to clean is a
    real fresh build. The clean fingerprint is taken HERE, never once per run: a baseline
    from before the run would equal a real no-rebuild's leftover and hide it.

    Leaves the mutation applied and built, so the caller can run the suites on it.
    """
    source.write_text(original)
    clear_stale_bytecode(source)
    built = run(["cmake", "--build", str(build), "--target", *binaries], build_timeout, cwd=build)
    if built.returncode != 0:
        raise RuntimeError(f"{mutation['id']}: the clean rebuild failed while confirming STALE")
    clean = {t: fingerprint(build / t) for t in stale}
    source.write_text(original.replace(mutation["old"], mutation["new"]))
    clear_stale_bytecode(source)
    built = run(["cmake", "--build", str(build), "--target", *binaries], build_timeout, cwd=build)
    if built.returncode != 0:
        raise RuntimeError(f"{mutation['id']}: the re-applied rebuild failed after it compiled")
    return [t for t in stale if fingerprint(build / t) == clean[t]]


def run_one_mutation(
    mutation: dict,
    root: pathlib.Path,
    build: pathlib.Path,
    args: argparse.Namespace,
    survivors: list[tuple[str, str]],
    baselines: dict[str, str] | None = None,
) -> None:
    """Apply one mutation, build, run its suites, restore. Appends to `survivors`.

    `baselines` is the per-run cache of clean pytest runs (see `pytest_baseline`).
    """
    if baselines is None:
        baselines = {}
    source = root / mutation["file"]
    original = source.read_text()
    occurrences = original.count(mutation["old"])
    if occurrences != 1:
        # Not a pass and not a fail: the anchor has drifted, so this mutation is
        # no longer testing what it claims. Reported as unresolved, because a
        # silently skipped mutation looks exactly like a killed one in a summary.
        print(
            f"ANCHOR-DRIFT {mutation['id']}: matched {occurrences} times in "
            f"{mutation['file']}, expected exactly 1. The code moved; update "
            f"the mutation rather than deleting it."
        )
        survivors.append((mutation["id"], "ANCHOR-DRIFT"))
        return

    binaries = binary_targets(mutation)
    if not binaries:
        raise RuntimeError(f"{mutation['id']}: no ctest binary target to guard compilation")
    missing = [t for t in binaries if not (build / t).is_file()]
    if missing:
        # UNBUILT, not a crash and not a survivor. Reading a baseline fingerprint for
        # a binary that was never built used to raise FileNotFoundError out of main(),
        # which aborted the WHOLE run on the first such mutation and said nothing
        # about the ones after it. That is the same failure as the strict-decode crash
        # this harness already carries a note about: a run that stops early reports
        # FEWER survivors than exist, which is the one thing it must never do.
        #
        # Counted as unresolved rather than skipped silently, because "this mutation
        # did not run" and "this mutation was killed" must not look the same in a
        # summary. Build the targets, or pass --only.
        print(
            f"UNBUILT {mutation['id']}: {missing} not built in {build}. Build the "
            f"target(s) first; this mutation was NOT run."
        )
        survivors.append((mutation["id"], "UNBUILT"))
        return

    before = {t: fingerprint(build / t) for t in binaries}
    try:
        # The mutating write lives INSIDE the try so that anything that raises
        # between it and the verdict (including clear_stale_bytecode itself) still
        # reaches the restoring finally. A write outside the try is a window in
        # which a crash leaves the mutation applied on disk.
        source.write_text(original.replace(mutation["old"], mutation["new"]))
        clear_stale_bytecode(source)
        built = run(
            ["cmake", "--build", str(build), "--target", *binaries],
            args.build_timeout,
            cwd=build,
        )
        if built.returncode != 0:
            print(
                f"NOT-A-MUTANT {mutation['id']}: does not compile, so a "
                f"compile-time guard already rejects it"
            )
            return

        after = {t: fingerprint(build / t) for t in binaries}
        stale = [t for t in binaries if before[t] == after[t]]
        if stale:
            stale = confirm_stale(
                mutation, stale, source, original, binaries, build, args.build_timeout
            )
        if stale:
            print(
                f"INVALID {mutation['id']}: {stale} unchanged after the "
                f"rebuild, so any verdict would be a false PASS against a "
                f"stale binary"
            )
            survivors.append((mutation["id"], "STALE-BINARY"))
            return

        killed_by, hung, skipped, claimed, hung_claims = [], [], [], [], []
        for target in mutation["targets"]:
            try:
                if target.startswith(PYTEST_PREFIX):
                    verdict = run_pytest_target(target, root, args.pytest_timeout)
                    returncode = {"killed": 1, "skipped": SKIP_RETURN_CODE, "passed": 0}[verdict]
                else:
                    returncode = run([str(build / target)], args.test_timeout).returncode
            except subprocess.TimeoutExpired:
                (claimed if target.startswith(PYTEST_PREFIX) else hung).append(target)
                hung_claims.append(target)
                continue
            if returncode == SKIP_RETURN_CODE:
                # ctest's SKIP_RETURN_CODE for the Vulkan suites: the device-free checks
                # passed and the device ones did NOT run. That is no verdict, so it must
                # never be counted as a kill (nonzero) NOR as a survivor.
                skipped.append(target)
            elif returncode != 0:
                (claimed if target.startswith(PYTEST_PREFIX) else killed_by).append(target)

        # T543: a pytest suite counts as the killer only if it passes on the clean tree.
        failing_baseline = []
        if claimed:
            source.write_text(original)  # the baseline runs on the clean source
            clear_stale_bytecode(source)
            for target in claimed:
                if pytest_baseline(target, root, build, args, baselines) == "failing":
                    failing_baseline.append(target)
                else:
                    (hung if target in hung_claims else killed_by).append(target)

        if hung:
            print(
                f"killed(hang) {mutation['id']}: {hung} did not finish in "
                f"{args.test_timeout}s. Detection, but by the worse mechanism: "
                f"a hang reports nothing about what went wrong."
            )
        elif killed_by:
            print(f"killed   {mutation['id']} by {killed_by}")
            if failing_baseline:
                print(
                    f"NOTE     {mutation['id']}: {failing_baseline} fail on the clean tree and "
                    f"were NOT counted as killers"
                )
        elif failing_baseline:
            print(
                f"BASELINE-FAILING {mutation['id']}: {failing_baseline} already fail on the clean "
                f"tree, so they cannot have killed it. NOT counted as killed: repair the suite "
                f"and sweep this mutation again."
            )
            survivors.append((mutation["id"], BASELINE_FAILING))
        elif skipped:
            print(
                f"SKIPPED  {mutation['id']}: {skipped} exited {SKIP_RETURN_CODE} (no Vulkan "
                f"device), so only a device run can kill this. NOT counted as killed."
            )
            if "needs_device" in mutation and not mutation["needs_device"]:
                print(f"NOTE     {mutation['id']} is not recorded as needs_device but was skipped")
            survivors.append((mutation["id"], SKIPPED_NO_DEVICE))
        else:
            print(f"SURVIVED {mutation['id']}: {mutation['why']}")
            survivors.append((mutation["id"], "SURVIVED"))
    finally:
        # Unconditional, including on KeyboardInterrupt: a mutation left on disk
        # becomes a phantom failure for whoever builds next.
        source.write_text(original)
        clear_stale_bytecode(source)


def check_pristine_targets(selected: list[dict], root: pathlib.Path) -> str:
    """Return git's dirt report for the selected mutations' source files, or ``""``.

    A killed harness leaves its current mutation applied on disk (a recorded trap).
    Starting a run on such a file would fingerprint a mutated baseline and restore
    the leftover mutation as if it were the original, so a dirty target must refuse
    the run before anything is written. Non-empty output means REFUSE.
    """
    target_files = sorted({mutation["file"] for mutation in selected})
    dirty = run(["git", "status", "--porcelain", "--", *target_files], 120, cwd=root)
    if dirty.returncode != 0:
        # Not a git checkout (or git itself failed): the check cannot run. Say so
        # rather than silently skipping it, but do not block the run on it.
        print(
            f"WARNING: could not verify pristine targets in {root} "
            f"(git status failed), so a leftover mutation from a killed run "
            f"would go undetected"
        )
        return ""
    return dirty.stdout.strip()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Mutation-test the C suites in a scratch worktree."
    )
    parser.add_argument(
        "--root", default=".", help="worktree root (default: the current directory)"
    )
    parser.add_argument(
        "--build", default="build", help="build directory, relative to --root (default: build)"
    )
    parser.add_argument("--only", default=None, help="run a single mutation by its id")
    parser.add_argument(
        "--prefix", default=None, help="run only the mutations whose id starts with this"
    )
    parser.add_argument(
        "--list", action="store_true", help="list the mutation ids and exit without building"
    )
    parser.add_argument(
        "--build-timeout",
        type=int,
        default=900,
        help="seconds allowed for one rebuild (default: 900)",
    )
    parser.add_argument(
        "--test-timeout",
        type=int,
        default=120,
        help="seconds allowed for one suite; exceeding it counts as "
        "a kill, because a hang is detection (default: 120)",
    )
    parser.add_argument(
        "--pytest-timeout",
        type=int,
        default=600,
        help="seconds allowed for one pytest: target; exceeding it counts as a kill "
        "like a hung suite (default: 600)",
    )
    args = parser.parse_args(argv)

    extra, set_names = load_mutation_sets(SETS_DIR)
    all_mutations = [*MUTATIONS, *extra]

    seen: dict[str, int] = {}
    for mutation in all_mutations:
        seen[mutation["id"]] = seen.get(mutation["id"], 0) + 1
    duplicates = sorted(key for key, count in seen.items() if count > 1)
    if duplicates:
        # Fatal rather than deduplicated. Two mutations sharing an id means two owners
        # believe they have coverage and only one of them does, and --only would silently
        # run whichever came first.
        print(f"duplicate mutation id(s) across sets: {duplicates}")
        return 2

    if args.list:
        for mutation in all_mutations:
            print(f"{mutation['id']:42s} {mutation['file']}")
        if set_names:
            print(f"\n{len(MUTATIONS)} built-in + sets: {', '.join(set_names)}")
        return 0

    root = pathlib.Path(args.root).resolve()
    build = root / args.build
    if not build.is_dir():
        print(
            f"no build directory at {build}: configure and build it first, so the "
            f"baseline fingerprints are of a known-good binary"
        )
        return 2

    survivors = []
    selected = [
        m
        for m in all_mutations
        if (not args.only or m["id"] == args.only)
        and (not args.prefix or m["id"].startswith(args.prefix))
    ]
    if not selected:
        print(
            f"no mutation matching --only {args.only!r} --prefix {args.prefix!r}; "
            f"--list shows the ids"
        )
        return 2

    leftover = check_pristine_targets(selected, root)
    if leftover:
        print(
            "REFUSING to start: mutation target file(s) are already modified, most "
            "likely a mutation left applied by a killed earlier run:"
        )
        print(leftover)
        print(
            "Running now would fingerprint a mutated baseline and restore the "
            "leftover as if it were the original. Restore the file(s) first "
            "(e.g. git checkout -- <file>) and rerun."
        )
        return 2

    errored: list[str] = []
    baselines: dict[str, str] = {}
    for mutation in selected:
        try:
            run_one_mutation(mutation, root, build, args, survivors, baselines)
        except Exception as error:  # noqa: BLE001 -- see the comment below
            # ANY per-mutation failure is THAT MUTATION's failure, never the run's.
            # (KeyboardInterrupt and SystemExit derive from BaseException and still
            # abort, as they should.)
            #
            # This loop has aborted mid-run twice: once on a strict UTF-8 decode of a
            # suite that prints raw disc bytes, and once on a missing baseline binary.
            # Both specific causes are fixed, and both were fixed only AFTER a run had
            # silently reported nothing about every mutation after the first failure.
            # A harness that stops early reports FEWER SURVIVORS THAN EXIST, which is
            # the single thing it must never do, so the catch is deliberately broad.
            #
            # Recorded as its own ERROR outcome, never as killed and never folded
            # into the survivor count, because "did not run to a verdict", "was
            # killed" and "survived" must not look the same in a summary.
            print(f"ERRORED {mutation['id']}: {type(error).__name__}: {error}")
            errored.append(mutation["id"])

    rebuilt = run(["cmake", "--build", str(build)], args.build_timeout, cwd=build)
    if rebuilt.returncode != 0:
        print(
            "WARNING: the restoring rebuild failed, so the build directory holds "
            "mutated binaries. Rebuild before trusting any test result."
        )

    dirty = run(["git", "status", "--porcelain", "src", "tests"], 120, cwd=root)
    if dirty.stdout.strip():
        print("WARNING: sources are not clean after restore:")
        print(dirty.stdout.rstrip())
        return 1

    print()
    skipped = [entry for entry in survivors if entry[1] == SKIPPED_NO_DEVICE]
    broken = [entry for entry in survivors if entry[1] == BASELINE_FAILING]
    survivors = [
        entry for entry in survivors if entry[1] not in (SKIPPED_NO_DEVICE, BASELINE_FAILING)
    ]
    if skipped:
        # By name, and never folded into "killed": an unevaluated mutation must not read
        # as a passing one. It does not fail the run either (ctest treats 77 as a skip).
        print(
            f"{len(skipped)} mutation(s) SKIPPED, no Vulkan device (NOT killed, no verdict): "
            f"{[mutation_id for mutation_id, _ in skipped]}"
        )
    if broken:
        # By name, loudly, and fails the run: a suite that fails on the clean tree has no
        # say about a mutant, so the mutant is neither killed nor a survivor (T543).
        print(
            f"{len(broken)} mutation(s) BASELINE-FAILING (a pytest suite already fails on the "
            f"clean tree, NOT killed, no verdict): {[mutation_id for mutation_id, _ in broken]}"
        )
    if errored:
        # Always BY NAME, never only a count: a reader must be able to see exactly
        # which mutations have no verdict, or an erroring run reads as a clean sweep.
        print(f"{len(errored)} mutation(s) ERRORED (no verdict, NOT killed): {errored}")
    if survivors:
        print(f"{len(survivors)} mutation(s) not killed: {survivors}")
    if errored or survivors or broken:
        return 1
    if skipped:
        print(
            f"{len(selected) - len(skipped)} of {len(selected)} killed, "
            f"{len(skipped)} skipped, sources clean"
        )
        return 0
    print(f"all {len(selected)} mutation(s) killed, sources clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
