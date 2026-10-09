"""Mutations for `src/host/host_options.c`, the command-line defaults.

WHY THIS SET EXISTS AT ALL. These lines were in `src/host/main.c` until today, and
`tsfp_host` is not a ctest binary, so NOTHING here could be mutated. The task that built
`--hdd` wanted exactly the first mutation below, could not reach it, and settled for
mutating a kernel-side guard instead -- which is detection by NULL dereference rather
than by assertion. Every entry here was unreachable yesterday.

Each default is a safety argument rather than a convenience, so each mutation is a
plausible-looking change that would fail no build and no other test.
"""

MUTATIONS: list[dict] = [
    {
        "id": "hostopt-hdd-defaults-to-writable",
        "file": "src/host/host_options.c",
        "old": "    out->hdd_path = NULL;",
        "new": '    out->hdd_path = "/tmp";',
        "targets": ["test_host_options"],
        "why": "THE ONE A PREVIOUS TASK COULD NOT REACH. A host that is writable without "
        "being asked gives the title storage the operator never granted, and the run "
        "looks healthier than it is.",
    },
    {
        "id": "hostopt-disc-device-is-a-drive-letter",
        "file": "src/host/host_options.c",
        "old": "    out->disc_device = DEFAULT_DISC_DEVICE;",
        "new": '    out->disc_device = "D:";',
        "targets": ["test_host_options"],
        "why": "the title creates the D: alias ITSELF at guest 0x00381301 via "
        "IoCreateSymbolicLink. Mounting on D: competes with the title's own link "
        "instead of letting it resolve, and leaves two sources of truth for what D: "
        "means.",
    },
    {
        "id": "hostopt-hdd-device-gains-a-trailing-separator",
        "file": "src/host/host_options.h",
        "old": '#define DEFAULT_HDD_DEVICE "\\\\Device\\\\Harddisk0\\\\partition1"',
        "new": '#define DEFAULT_HDD_DEVICE "\\\\Device\\\\Harddisk0\\\\partition1\\\\"',
        "targets": ["test_host_options"],
        "why": "the mount table requires the character after a matched prefix to be a "
        "separator or end-of-string, so a prefix already ending in one matches the "
        "bare device and NOTHING UNDER IT. The mount reports success and every read "
        "beneath it fails, which reads as missing content rather than a bad default.",
    },
    {
        "id": "hostopt-open-missing-defaults-to-empty",
        "file": "src/host/host_options.c",
        "old": "    out->open_missing_as_empty = false;",
        "new": "    out->open_missing_as_empty = true;",
        "targets": ["test_host_options"],
        "why": "fabricating an empty file for any name is a DIAGNOSTIC, not a "
        "behaviour. On by default, the boot reaches 30 calls on five fabricated opens "
        "and looks identical to the honest --hdd run that creates real directories.",
    },
    {
        "id": "hostopt-continue-on-missing-defaults-on",
        "file": "src/host/host_options.c",
        "old": "    out->continue_on_missing = false;",
        "new": "    out->continue_on_missing = true;",
        "targets": ["test_host_options"],
        "why": "every observation after the first unimplemented ordinal would be made "
        "on a guest that received a fabricated answer, and nothing in the trace would "
        "say so.",
    },
    {
        "id": "hostopt-flag-consumes-the-wrong-argv-slot",
        "file": "src/host/host_options.c",
        "old": """        } else if (strcmp(arg, "--hdd") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->hdd_path = argv[i];""",
        "new": """        } else if (strcmp(arg, "--hdd") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            out->hdd_path = argv[i + 1];""",
        "targets": ["test_host_options"],
        "why": "reading the next slot WITHOUT advancing `i` means the path is also "
        "parsed as the next argument, so it becomes the positional XBE path and the "
        "real one is rejected as a second positional.",
    },
    {
        "id": "hostopt-missing-argument-falls-back-instead-of-refusing",
        "file": "src/host/host_options.c",
        "old": """        } else if (strcmp(arg, "--disc") == 0) {
            if (++i >= argc) {
                return false;
            }""",
        "new": """        } else if (strcmp(arg, "--disc") == 0) {
            if (++i >= argc) {
                continue;
            }""",
        "targets": ["test_host_options"],
        "why": "a flag given no argument would be silently ignored, so the operator "
        "asked for a disc, got none, and the run proceeds looking fine. Refusing is "
        "the only outcome that reaches the person who can fix it.",
    },
    {
        "id": "hostopt-negative-timeout-accepted",
        "file": "src/host/host_options.c",
        "old": """            long value = strtol(argv[i], NULL, 10);
            if (value < 0) {
                return false;
            }
            out->thread_timeout_ms = (unsigned)value;""",
        "new": """            long value = strtol(argv[i], NULL, 10);
            out->thread_timeout_ms = (unsigned)value;""",
        "targets": ["test_host_options"],
        "why": "a negative timeout wraps to an enormous unsigned, turning the watchdog "
        "off. A hang would then be waited on forever instead of reported, and a hang "
        "that is waited on reports nothing at all.",
    },
    {
        "id": "hostopt-stub-status-set-flag-not-raised",
        "file": "src/host/host_options.c",
        "old": "            out->stub_status_set = true;",
        "new": "            out->stub_status_set = (out->stub_status != 0u);",
        "targets": ["test_host_options"],
        "why": "0 is a legitimate status, so `--stub-status 0` must stay "
        "distinguishable from the flag not being given. Keying the flag off the value "
        "collapses exactly that distinction.",
    },
    {
        "id": "hostopt-mount-overflow-truncates-instead-of-refusing",
        "file": "src/host/host_options.c",
        "old": "            if (++i >= argc || out->mount_count >= OPTION_MOUNT_MAX) {\n"
        "                return false;\n"
        "            }\n"
        "            out->mounts[out->mount_count++] = argv[i];",
        "new": "            if (++i >= argc) {\n"
        "                return false;\n"
        "            }\n"
        "            if (out->mount_count < OPTION_MOUNT_MAX) {\n"
        "                out->mounts[out->mount_count++] = argv[i];\n"
        "            }",
        "targets": ["test_host_options"],
        "why": "a dropped mount presents as a missing file, which reads as a content "
        "problem rather than an argument one. The bound must be reported, not absorbed.",
    },
    {
        "id": "hostopt-second-positional-overwrites-the-first",
        "file": "src/host/host_options.c",
        "old": """        } else if (!out->xbe_path) {
            out->xbe_path = arg;
        } else {
            return false;
        }""",
        "new": """        } else {
            out->xbe_path = arg;
        }""",
        "targets": ["test_host_options"],
        "why": "two positional arguments mean the operator meant something we cannot "
        "guess. Silently taking the last one runs a different executable than the one "
        "they probably intended, with nothing said.",
    },
    {
        "id": "hostopt-cache-partitions-defaults-to-zero",
        "file": "src/host/host_options.c",
        "old": "    out->cache_partitions = DEFAULT_CACHE_PARTITIONS;",
        "new": "    out->cache_partitions = 0u;",
        "targets": ["test_host_options"],
        "why": "0 reads as a plausible empty answer, but the title then calls memmove with "
        "length 0xFFFFFFF4 and runs off the guest stack.",
    },
    {
        "id": "hostopt-cache-partitions-bound-off-by-one",
        "file": "src/host/host_options.c",
        "old": "            if (value < 0 || value > (long)MAX_CACHE_PARTITIONS) {",
        "new": "            if (value < 0 || value > (long)MAX_CACHE_PARTITIONS + 1) {",
        "targets": ["test_host_options"],
        "why": "a bound that admits one extra value is a bound nobody can state.",
    },
    {
        "id": "hostopt-cache-partitions-negative-accepted",
        "file": "src/host/host_options.c",
        "old": "            if (value < 0 || value > (long)MAX_CACHE_PARTITIONS) {",
        "new": "            if (value > (long)MAX_CACHE_PARTITIONS) {",
        "targets": ["test_host_options"],
        "why": "a negative wraps to a huge unsigned count that looks like a real geometry.",
    },
    {
        "id": "hostopt-t871-dowork-default-off",
        "file": "src/host/host_options.c",
        "old": "    } else if (out->passive_audio_completion && "
        "!out->passive_audio_completion_at_dowork) {",
        "new": "    } else if (false && out->passive_audio_completion && "
        "!out->passive_audio_completion_at_dowork) {",
        "targets": ["test_host_options"],
        "why": "T871: the DoWork delivery is the default of the completion model "
        "(xemu-level), losing it silently returns every boot to the T681 delivery.",
    },
    {
        "id": "hostopt-t871-dowork-optout-ignored",
        "file": "src/host/host_options.c",
        "old": "    if (out->passive_audio_completion_at_dowork_off) {\n"
        "        out->passive_audio_completion_at_dowork = false;",
        "new": "    if (false) {\n        out->passive_audio_completion_at_dowork = false;",
        "targets": ["test_host_options"],
        "why": "T871: --no-passive-audio-completion-at-dowork must reproduce the T681 delivery.",
    },
    {
        "id": "hostopt-t871-dowork-default-without-model",
        "file": "src/host/host_options.c",
        "old": "    } else if (out->passive_audio_completion && "
        "!out->passive_audio_completion_at_dowork) {",
        "new": "    } else if (!out->passive_audio_completion_at_dowork) {",
        "targets": ["test_host_options"],
        "why": "T871: the default is a mode of the completion model, never on without it.",
    },
    {
        "id": "hostopt-xonline-offline-flag-ignored",
        "file": "src/host/host_options.c",
        "old": '        } else if (strcmp(arg, "--xonline-offline") == 0) {\n'
        "            out->xonline_offline = true;",
        "new": '        } else if (strcmp(arg, "--xonline-offline") == 0) {\n'
        "            out->xonline_offline = false;",
        "targets": ["test_host_options"],
        "why": "the explicitly requested offline XONLINE policy must actually be enabled; "
        "silently ignoring it leaves the Challenge query at the default stop.",
    },
]
