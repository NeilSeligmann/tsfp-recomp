#!/usr/bin/env python3
"""T183 static probes: safepoint reachability of the title loop and vblank helper callers.

  loop     does the lifted 153970 loop pass through RECOMP_ABI_CALL (safepoint seam)?
  readers  T371: static call-graph closure of every guest thread start routine in the image, which
           can reach the counter readers (0x1538C0, 0x156CB0), spawn threads (0x37FEB5), or has an
           indirect call it cannot resolve.
  callers  caller graph of vblank helper 3DC2E0 plus IRQL-related kernel calls seen.
  schedule run tsfp_host with --trace-vblank-schedule several times and require the
           delivery schedule (guest progress and virtual clock only) to be identical.
           --readers adds the T371 getter and thread census lines to the compared record.
Reads generated/ (never edited). Exit 1 if an expectation is not met.
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCHEDULE_PREFIX = "vblank-schedule: "
READERS_PREFIX = "vblank-readers: "

IRQL_NAMES = (
    "KfRaiseIrql",
    "KfLowerIrql",
    "KeRaiseIrqlToDpcLevel",
    "KeGetCurrentIrql",
    "KeInitializeDpc",
    "KeInsertQueueDpc",
    "KeInitializeInterrupt",
    "KeConnectInterrupt",
)


def probe_loop(generated: str) -> bool:
    gen = Path(generated) / "lifted" / "gen"
    text = next(
        p for p in sorted(gen.glob("recomp_00*.c")) if "void sub_001538C0(void)" in p.read_text()
    ).read_text()
    start = text.index("loc_00153970: ;")
    window = text[start : text.index("goto loc_00153970", start)]
    call = re.search(r"RECOMP_ABI_CALL\(0x00022030u, sub_00022030\)", window)
    types = (gen / "recomp_types.h").read_text()
    seam = (
        "#define RECOMP_ABI_CALL(va, fn) do { RECOMP_CALL_SAFEPOINT(va); (fn)(); } while(0)"
        in types
    )
    print(f"loop body has RECOMP_ABI_CALL(0x22030): {bool(call)}")
    print(f"macro invokes RECOMP_CALL_SAFEPOINT(va) before callee when cooperative enabled: {seam}")
    hooks = re.findall(r"(recomp_\w+|harness_\w+|host_\w+)\(", window)
    print(f"other hooks in loop body: {hooks}")
    return bool(call) and seam


def probe_callers(generated: str) -> bool:
    src = Path(generated) / "retail" / "src"
    files = {p.stem[4:]: p.read_text() for p in src.glob("sub_*.c")}

    def callers(addr: str) -> list[str]:
        return sorted(a for a, t in files.items() if a != addr and re.search(rf"FUN_{addr}\(", t))

    seen, queue = {}, ["003dc2e0"]
    while queue:
        cur = queue.pop(0)
        for caller in callers(cur):
            if caller not in seen:
                seen[caller] = cur
                queue.append(caller)
    for caller, callee in sorted(seen.items()):
        irql = sorted({n for n in IRQL_NAMES if n in files[caller]})
        print(f"{caller} -> {callee}  irql/dpc calls: {irql}")
    dpc_or_isr = (
        r"KeInitializeDpc\([^)]*FUN_003dca90"
        r"|KeInitializeInterrupt\([^)]*FUN_003dc0e0"
    )
    registrations = [a for a, t in files.items() if re.search(dpc_or_isr, t)]
    print(f"DPC/ISR registration sites: {registrations}")
    return "003dca90" in seen


FUNCTION_START = re.compile(r"^void (sub_([0-9A-F]{8}))\(void\)\n\{", re.MULTILINE)
CALLEE = re.compile(r"\bsub_([0-9A-F]{8})\b")
ICALL_TARGET = re.compile(r"_icall_target = ([^;]+);")
IMPORT_SLOT = re.compile(r"MEM32\((0x[0-9A-Fa-f]+)\)")
READER_FUNCTIONS = (0x1538C0, 0x156CB0)
THREAD_CREATE = 0x37FEB5
THREAD_CREATE_CALL = re.compile(r"RECOMP_ABI_CALL\(0x0037FEB5u, sub_0037FEB5\)")


def parse_call_graph(
    sources: dict[str, str], import_low: int, import_high: int
) -> tuple[dict[int, set[int]], dict[int, int], dict[int, int]]:
    """Per function: direct callee set and the count of indirect calls through an import slot
    versus any other target. Returns (edges, kernel_calls, unresolved_calls)."""
    edges: dict[int, set[int]] = {}
    kernel: dict[int, int] = {}
    unresolved: dict[int, int] = {}
    for text in sources.values():
        starts = list(FUNCTION_START.finditer(text))
        for index, match in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            # A function ends at the first closing brace in column 0. The slice up to the next
            # `void` line also holds the NEXT function's doc comment, which names it, and that
            # once gave every function a call edge to its lexical successor (T424).
            close = text.find("\n}\n", match.end(), end)
            body = text[match.end() : close if close != -1 else end]
            address = int(match.group(2), 16)
            edges[address] = {int(c, 16) for c in CALLEE.findall(body)} - {address}
            slots = [
                ICALL_TARGET.search(line) for line in body.splitlines() if "_icall_target" in line
            ]
            kernel[address] = unresolved[address] = 0
            for found in slots:
                slot = IMPORT_SLOT.fullmatch(found.group(1).strip()) if found else None
                if slot and import_low <= int(slot.group(1), 16) < import_high:
                    kernel[address] += 1
                else:
                    unresolved[address] += 1
    return edges, kernel, unresolved


def closure(edges: dict[int, set[int]], root: int) -> set[int]:
    seen, queue = {root}, [root]
    while queue:
        for callee in edges.get(queue.pop(), ()):
            if callee not in seen:
                seen.add(callee)
                queue.append(callee)
    return seen


def classify_start(
    edges: dict[int, set[int]], unresolved: dict[int, int], root: int
) -> dict[str, object]:
    """What a thread started at `root` can do, from direct calls only. `blind` lists reachable
    functions with a non-import indirect call: the closure there is NOT proven closed. A root with
    no lifted code is never closed_non_reader: the host refuses such a thread (has_code), which is
    a different reason it cannot run and not evidence about what it would do."""
    reach = closure(edges, root)
    blind = sorted(a for a in reach if unresolved.get(a, 0))
    return {
        "has_code": root in edges,
        "functions": len(reach),
        "reaches_reader": bool(reach & set(READER_FUNCTIONS)),
        "spawns_threads": THREAD_CREATE in reach,
        "blind": blind,
        "closed_non_reader": root in edges
        and not (reach & set(READER_FUNCTIONS))
        and THREAD_CREATE not in reach
        and not blind,
    }


def thread_start_routines(sources: dict[str, str]) -> dict[int, list[int]]:
    """Start routine -> call sites of the CreateThread wrapper 0x37FEB5, read from the pushes of
    each call (the 3rd push counted back from the call is argument 2, the start address)."""
    found: dict[int, list[int]] = {}
    push = re.compile(r"PUSH32\(esp, (0x[0-9A-Fa-f]+|\w+)\);")
    for text in sources.values():
        lines = text.splitlines()
        for number, line in enumerate(lines):
            call = THREAD_CREATE_CALL.search(line)
            if not call:
                continue
            site = re.match(r"\s*PUSH32\(esp, (0x[0-9A-F]+)u\)", line)
            pushes = []
            for back in range(number - 1, max(number - 40, 0), -1):
                match = push.search(lines[back])
                if match:
                    pushes.append(match.group(1))
                if len(pushes) == 3:
                    break
            if len(pushes) == 3 and pushes[2].startswith("0x"):
                found.setdefault(int(pushes[2], 16), []).append(
                    int(site.group(1), 16) if site else 0
                )
    return found


def probe_readers(args: argparse.Namespace) -> bool:
    gen = Path(args.generated) / "lifted" / "gen"
    sources = {p.name: p.read_text() for p in sorted(gen.glob("recomp_00*.c"))}
    edges, _kernel, unresolved = parse_call_graph(sources, args.import_low, args.import_high)
    starts = thread_start_routines(sources)
    sites_total = sum(map(len, starts.values()))
    print(f"functions parsed: {len(edges)}, CreateThread wrapper call sites: {sites_total}")
    ok = bool(starts)
    for start, sites in sorted(starts.items()):
        info = classify_start(edges, unresolved, start)
        print(
            f"start 0x{start:06X} (created at {', '.join(f'0x{s:06X}' for s in sites)}): "
            f"lifted code {info['has_code']}, {info['functions']} functions, "
            f"reaches reader {info['reaches_reader']}, "
            f"spawns threads {info['spawns_threads']}, blind functions {len(info['blind'])}, "
            f"closed non-reader {info['closed_non_reader']}"
        )
    return ok


def schedule_lines(output: str, readers: bool = False) -> list[str]:
    """The schedule record of one run: only lines the host tagged. The schedule prefix is
    removed. With `readers` (T371) the reader and census lines are kept too, whole."""
    lines = []
    for line in output.splitlines():
        if line.startswith(SCHEDULE_PREFIX):
            lines.append(line[len(SCHEDULE_PREFIX) :])
        elif readers and line.startswith(READERS_PREFIX):
            lines.append(line)
    return lines


THREAD_FIELD = re.compile(r"\bthread=(0x[0-9A-Fa-f]+)")


def per_thread(record: list[str]) -> dict[str, list[str]]:
    """Group a record by guest thread. Order within a thread is guest progress, while
    the interleaving of two host threads' lines is host scheduling and is not compared."""
    grouped: dict[str, list[str]] = {}
    for line in record:
        match = THREAD_FIELD.search(line)
        grouped.setdefault(match.group(1) if match else "none", []).append(line)
    return grouped


def compare_schedules(runs: list[list[str]]) -> list[str]:
    """Problems found across runs. Empty records are a problem, never equal."""
    if not runs:
        return ["no runs"]
    problems = [
        f"run {index} produced an empty schedule" for index, record in enumerate(runs) if not record
    ]
    if problems:
        return problems
    reference = per_thread(runs[0])
    for index, record in enumerate(runs[1:], start=1):
        grouped = per_thread(record)
        if grouped.keys() != reference.keys():
            problems.append(
                f"run {index} threads {sorted(grouped)} differ from run 0 {sorted(reference)}"
            )
            continue
        for thread, lines in sorted(grouped.items()):
            if lines != reference[thread]:
                problems.append(f"run {index} thread {thread} differs from run 0")
    return problems


def run_host(args: argparse.Namespace) -> list[str]:
    """One host boot in its own process group, killed at the timeout, in a fresh hdd dir."""
    hdd = tempfile.mkdtemp(prefix="vblank-schedule-hdd-")
    trace = ["--trace-vblank-readers"] if args.readers else []
    command = [
        args.host,
        args.xbe,
        "--hdd",
        hdd,
        "--trace-vblank-schedule",
        *trace,
        *args.host_flags,
    ]
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True
    )
    try:
        output, _ = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise SystemExit(f"host exceeded {args.timeout}s and was killed") from None
    finally:
        shutil.rmtree(hdd, ignore_errors=True)
    return schedule_lines(output, args.readers)


def probe_schedule(args: argparse.Namespace) -> bool:
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        runs = list(pool.map(lambda _index: run_host(args), range(args.runs)))
    problems = compare_schedules(runs)
    for line in runs[0]:
        print(line)
    print(f"runs: {len(runs)}, lines per run: {len(runs[0])}")
    for problem in problems:
        print(f"PROBLEM: {problem}")
    return not problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("mode", choices=("loop", "callers", "schedule", "readers"))
    parser.add_argument("--generated", default="generated", help="generated tree root (relative)")
    parser.add_argument("--host", default="build/tsfp_host", help="schedule: host binary")
    parser.add_argument("--xbe", default="build/default.xbe", help="schedule: guest executable")
    parser.add_argument("--runs", type=int, default=2, help="schedule: number of boots")
    parser.add_argument("--jobs", type=int, default=1, help="schedule: concurrent boots")
    parser.add_argument("--timeout", type=int, default=120, help="schedule: seconds per boot")
    parser.add_argument(
        "--readers",
        action="store_true",
        help="schedule: also record --trace-vblank-readers (getter polls, thread census, T371)",
    )
    parser.add_argument(
        "--host-flags",
        nargs=argparse.REMAINDER,
        default=[],
        help="schedule: remaining arguments go to the host (a vblank policy is required)",
    )
    parser.add_argument(
        "--import-low",
        type=lambda v: int(v, 0),
        default=0x475780,
        help="readers: first address of the kernel import thunk table",
    )
    parser.add_argument(
        "--import-high",
        type=lambda v: int(v, 0),
        default=0x475900,
        help="readers: end of the kernel import thunk table",
    )
    args = parser.parse_args()
    if args.mode == "readers":
        return 0 if probe_readers(args) else 1
    if args.mode == "schedule":
        return 0 if probe_schedule(args) else 1
    ok = probe_loop(args.generated) if args.mode == "loop" else probe_callers(args.generated)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
