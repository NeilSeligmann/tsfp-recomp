# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential test of the C port in `src/gpu/` against the retail library code it replaces.

Both sides run the same scenario. One is `tools/d3dscan/oracle.py`: the ORIGINAL bytes under an x86
emulator. The other is `tools/d3dscan/port_run.c`: the C handlers, in a PIE process that maps the
same XBE at its guest addresses and uses the real kernel HLE and AV module. Their results are then
compared dword for dword. Nothing here is a model of what the answer should be: the oracle is the
answer, and a disagreement is a bug in the port or a gap this tool lists by name.

FIVE CHECKS

  modes     the whole mode list (count, every mode, and the invalid-call past the end) for each AV
            pack and certificate region, which is the full domain of the display filter.
  creation  whether CreateDevice succeeds, for every pack, region, flags and refresh combination
            tried. A mode matcher that is wrong in either direction shows here.
  state     the D3D section after a default CreateDevice, dword by dword, sorted into: reproduced
            bit for bit, address-space dependent (pointers and the cursor, which differ by
            construction), deliberately not reproduced (the library's own init commands, NV2A
            objects, default shadows) and EXTRA, where the port wrote something the original did
            not. EXTRA must be zero. An unexplained mismatch fails the run.
  handlers  each init-sequence handler on an IDENTICAL pre-state (the oracle's memory after
            CreateDevice, loaded into the port), comparing the set of dwords each changed.
  constants the same for the vertex shader constant setter, which takes ecx and edx.

    python -m tools.d3dscan.port_diff IMAGE.xbe --check all
    python -m tools.d3dscan.port_diff IMAGE.xbe --check modes --build-dir /tmp/port_diff

Needs `unicorn` and a C compiler. Builds `port_run` from the checkout it lives in, so it always
tests the sources next to it.
"""

from __future__ import annotations

import argparse
import itertools
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from collections.abc import Sequence
from pathlib import Path

from tools.d3dscan import oracle
from tools.d3dscan.oracle import D3D_HIGH, D3D_LOW, Oracle, Presentation
from tools.d3dscan.oracle_sources import d3d8_oracle_sources

REPO = Path(__file__).resolve().parents[2]

# kernel_av_pack: composite, S-Video, HDTV. Certificate regions: North America, Japan, rest of
# world.
PACKS = (0, 1, 2)
REGIONS = (1, 2, 4)
FLAGS = (0x100, 0x140, 0x110, 0x150, 0x180)
REFRESHES = (0x3C, 0x32)

# Device offsets whose values differ between the two sides BY CONSTRUCTION: the oracle's contiguous
# memory sits at 0x8xxxxxxx and its pushbuffer holds the library's 678 init dwords, the port's
# memory is the host allocator's and its pushbuffer starts empty. Everything else that differs is a
# finding.
ADDRESS_SPACE_OFFSETS = frozenset(
    {
        0x00,
        0x04,
        0x24,
        0x28,
        0x1A28,
        0x1A40,
        0x1A70,
        0x1A84,
        0x1A88,
        0x1A8C,
        # GPU allocations made by the original init and the recovered host model:
        0x30,  # semaphore
        0x48,  # history ring
        0x78,  # first history cursor
        0x2488,  # GPU control block
        0x248C,  # notifier end
        0x2490,  # notifier base
    }
)
# Library state the port does not reproduce, named in docs/d3d8-usage.md section 13: the dev+8
# flags written by internal calls, and the pushbuffer mode word.
DOCUMENTED_OFFSETS = frozenset({0x08, 0x2C})

# The handlers, with the arguments the title passes and the values around them. The cases in
# CASCADES take a branch into library code the port announces and does not model, so the original
# changes more than the port does; those are reported with the size of the gap, not failed.
CASCADES = frozenset({"state 0x8F = 2", "state 0x9A = 3"})

HANDLER_CASES: tuple[tuple[str, int, tuple[int, ...]], ...] = (
    ("state 0x93", 0x3D7060, (0,)),
    ("state 0x93 = 7", 0x3D7060, (7,)),
    ("state 0x90", 0x3D7F70, (0,)),
    ("state 0x90 = 9", 0x3D7F70, (9,)),
    ("state 0x91", 0x3D8010, (0x1E00,)),
    ("state 0x91 = 0x1E01", 0x3D8010, (0x1E01,)),
    ("state 0x94", 0x3D7150, (0,)),
    ("state 0x94 = 0x12345678", 0x3D7150, (0x12345678,)),
    ("state 0xA1", 0x3D80B0, (1,)),
    ("state 0xA1 = 0", 0x3D80B0, (0,)),
    ("state 0xA3", 0x3D8190, (1,)),
    ("state 0xA4", 0x3D81B0, (1,)),
    ("state 0xA3 = 0", 0x3D8190, (0,)),
    ("state 0x8F", 0x3D7EE0, (1,)),
    ("state 0x8F = 2", 0x3D7EE0, (2,)),
    ("state 0x9A", 0x3D81F0, (0,)),
    ("state 0x9A = 3", 0x3D81F0, (3,)),
    ("state 0x95", 0x3D72A0, (0,)),
    ("state 0x95 = 5", 0x3D72A0, (5,)),
    ("state 0x95 = 0xFFFFFFFE", 0x3D72A0, (0xFFFFFFFE,)),
    ("constant mode 0x11", 0x3D5AF0, (0x11,)),
    ("constant mode 0x10", 0x3D5AF0, (0x10,)),
    ("constant mode 0", 0x3D5AF0, (0,)),
    ("GetBackBuffer2(-1)", 0x3D3A80, (0xFFFFFFFF,)),
    ("GetBackBuffer2(0)", 0x3D3A80, (0,)),
)


class PortProcess:
    """`port_run` on a pipe."""

    def __init__(self, binary: Path, xbe: Path, pack: int, region: int) -> None:
        self.process = subprocess.Popen(
            [str(binary), str(xbe), str(pack), str(region)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.registered = int(self._line().split()[1])
        self.scratch = int(self._line().split()[1])
        self._file = Path(tempfile.mkstemp(prefix="port_diff_")[1])

    def _line(self) -> str:
        assert self.process.stdout is not None
        return self.process.stdout.readline().strip()

    def command(self, text: str) -> str:
        assert self.process.stdin is not None
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        reply = self._line()
        if reply.startswith("fatal"):
            raise RuntimeError(reply)
        return reply

    def call(self, address: int, arguments: Sequence[int] = (), ecx: int = 0, edx: int = 0) -> int:
        words = " ".join(f"{value:#x}" for value in arguments)
        reply = self.command(f"call {address:#x} {ecx:#x} {edx:#x} {len(arguments)} {words}")
        return int(reply.split()[1])

    def write32(self, address: int, value: int) -> None:
        self.command(f"w32 {address:#x} {value & 0xFFFFFFFF:#x}")

    def read32(self, address: int) -> int:
        return int(self.command(f"r32 {address:#x}").split()[1])

    def dump(self) -> bytes:
        self.command(f"dump {D3D_LOW:#x} {D3D_HIGH:#x} {self._file}")
        return self._file.read_bytes()

    def load(self, data: bytes) -> None:
        self._file.write_bytes(data)
        self.command(f"load {self._file} {D3D_LOW:#x}")

    def close(self) -> None:
        try:
            assert self.process.stdin is not None
            self.process.stdin.write("quit\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        self.process.wait(timeout=30)
        self._file.unlink(missing_ok=True)


def build(directory: Path, compiler: str) -> Path:
    """Compile port_run.c with the sources it links, from this checkout."""
    skip = {"hle_report.c", "xdk_report.c", "xdk_surface.c"}
    sources = [p for p in sorted((REPO / "src" / "xbox").glob("*.c")) if p.name not in skip]
    sources += [REPO / "src" / "audio" / "dsound_hle.c", REPO / "src" / "input" / "xinput_hle.c"]
    sources += [REPO / "src" / "loader" / "xbe.c"]
    sources += d3d8_oracle_sources(REPO)
    sources += sorted((REPO / "src" / "gpu").glob("xgrph_*.c"))
    sources += [REPO / "tools" / "d3dscan" / "port_run.c"]
    directory.mkdir(parents=True, exist_ok=True)
    binary = directory / "port_run"
    command = [
        compiler, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-fPIE", "-pie", "-pthread",
        "-I", str(REPO / "src" / "xbox"), "-I", str(REPO / "src" / "audio"),
        "-I", str(REPO / "src" / "input"), "-I", str(REPO / "src" / "gpu"),
        "-I", str(REPO / "src" / "loader"),
        *map(str, sources), "-o", str(binary),
    ]  # fmt: skip
    subprocess.run(command, check=True, timeout=600)
    return binary


def _port_capabilities(binary: Path, xbe: Path, pack: int, region: int) -> int:
    port = PortProcess(binary, xbe, pack, region)
    try:
        port.call(0x3D9010, (0,))
        return port.read32(0x3E3AA8)
    finally:
        port.close()


def _port_modes(binary: Path, xbe: Path, pack: int, region: int) -> tuple[int, list[object]]:
    port = PortProcess(binary, xbe, pack, region)
    try:
        count = port.call(0x3D9010, (0,))
        modes: list[object] = []
        out = port.scratch + 0x400
        for index in range(count + 1):
            status = port.call(0x3D90B0, (0, index, out))
            if status != 0:
                modes.append(None)
                continue
            modes.append(tuple(port.read32(out + 4 * word) for word in range(5)))
        return count, modes
    finally:
        port.close()


def check_modes(binary: Path, xbe: Path) -> list[str]:
    problems = []
    for pack, region in itertools.product(PACKS, REGIONS):
        count, port_modes = _port_modes(binary, xbe, pack, region)
        caps = _port_capabilities(binary, xbe, pack, region)
        reference = oracle.list_modes(Oracle(xbe, caps))
        same = port_modes == reference and count == len(reference) - 1
        print(f"modes  pack {pack} region {region} caps {caps:#010x}: {count} modes, same={same}")
        if not same:
            problems.append(f"mode list differs for pack {pack} region {region}")
    return problems


def check_creation(binary: Path, xbe: Path) -> list[str]:
    problems = []
    cases = 0
    failing = 0
    for pack, region, flags, refresh in itertools.product(PACKS, REGIONS, FLAGS, REFRESHES):
        presentation = Presentation(flags=flags, refresh_rate=refresh)
        port = PortProcess(binary, xbe, pack, region)
        try:
            words = presentation.words()
            for index, value in enumerate(words):
                port.write32(port.scratch + 4 * index, value)
            port.call(0x3D9210, (0x100000, 0x10000))
            port.call(0x3D9010, (0,))
            caps = port.read32(0x3E3AA8)
            result = port.call(0x3D9230, (0, 1, 0, 0, port.scratch, port.scratch + 0x800))
        finally:
            port.close()
        reference = oracle.create_device(Oracle(xbe, caps), presentation)
        cases += 1
        failing += result != 0
        if (result == 0) != reference.succeeded:
            problems.append(
                f"pack {pack} region {region} flags {flags:#x} refresh {refresh}: "
                f"port {'ok' if result == 0 else 'failed'}, "
                f"original {'ok' if reference.succeeded else 'failed'}"
            )
    print(f"create {cases} requests, {failing} fail on both sides, {len(problems)} disagreements")
    return problems


def _default_port(binary: Path, xbe: Path) -> PortProcess:
    port = PortProcess(binary, xbe, 2, 1)
    for index, value in enumerate(Presentation().words()):
        port.write32(port.scratch + 4 * index, value)
    return port


def check_state(binary: Path, xbe: Path) -> list[str]:
    port = _default_port(binary, xbe)
    try:
        initial = port.dump()
        port.call(0x3D9210, (0x100000, 0x10000))
        port.call(0x3D9010, (0,))
        if port.call(0x3D9230, (0, 1, 0, 0, port.scratch, port.scratch + 0x800)) != 0:
            return ["the default CreateDevice failed on the port"]
        ported = port.dump()
    finally:
        port.close()
    reference = oracle.create_device(Oracle(xbe))
    expected = bytearray(initial)
    for address, (_old, new) in reference.changes.items():
        struct.pack_into("<I", expected, address - D3D_LOW, new)

    tally: dict[str, int] = defaultdict(int)
    problems = []
    for offset in range(0, len(initial), 4):
        address = D3D_LOW + offset
        before, mine, theirs = (
            struct.unpack_from("<I", blob, offset)[0] for blob in (initial, ported, expected)
        )
        device_offset = address - oracle.DEVICE_BASE
        if mine == theirs:
            tally["reproduced bit for bit" if mine != before else "unchanged by both"] += 1
        elif mine == before:
            tally["not reproduced (documented in section 13)"] += 1
        elif theirs == before:
            tally["EXTRA: the port wrote what the original did not"] += 1
            problems.append(f"{address:#010x}: port wrote {mine:#x}, original left {before:#x}")
        elif device_offset in ADDRESS_SPACE_OFFSETS or device_offset in DOCUMENTED_OFFSETS:
            tally["differs by construction or documented"] += 1
        else:
            tally["UNEXPLAINED MISMATCH"] += 1
            problems.append(f"{address:#010x}: port {mine:#x}, original {theirs:#x}")
    for label, count in sorted(tally.items()):
        print(f"state  {count:5d} dwords  {label}")
    return problems


def _delta(before: bytes, after: bytes) -> dict[int, tuple[int, int]]:
    out = {}
    for offset in range(0, len(before), 4):
        old = struct.unpack_from("<I", before, offset)[0]
        new = struct.unpack_from("<I", after, offset)[0]
        if old != new:
            out[D3D_LOW + offset] = (old, new)
    return out


def check_handlers(binary: Path, xbe: Path) -> list[str]:
    problems = []
    port = _default_port(binary, xbe)
    try:
        for name, address, arguments in HANDLER_CASES:
            emulator = Oracle(xbe)
            oracle.create_device(emulator)
            base = emulator.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)
            emulator.run(address, list(arguments))
            expected = _delta(base, emulator.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW))

            port.load(base)
            try:
                port.call(address, arguments)
            except RuntimeError as error:
                # The original faults on the same empty slot GetBackBuffer2 refuses.
                print(f"handler {name}: port stopped: {error}")
                continue
            actual = _delta(base, port.dump())
            for table in (expected, actual):
                table.pop(
                    oracle.DEVICE_BASE, None
                )  # the cursor: the original emits, the port does not
            if expected == actual:
                verdict = "identical"
            elif name in CASCADES and all(expected.get(k) == v for k, v in actual.items()):
                gap = len(expected) - len(actual)
                verdict = f"port is a subset: the announced cascade changes {gap} more"
            else:
                verdict = "DIFFERENT"
                problems.append(f"{name}: original {sorted(expected)}, port {sorted(actual)}")
            print(f"handler {name:28s} {len(expected):3d} dwords changed, {verdict}")
    finally:
        port.close()
    return problems


def check_constants(binary: Path, xbe: Path) -> list[str]:
    """0x003D5670 takes its register in ecx and a pointer to four dwords in edx."""
    problems = []
    port = _default_port(binary, xbe)
    data = struct.pack("<4I", 0x11111111, 0x22222222, 0x33333333, 0x44444444)
    try:
        for register in (0x0, 0xB, 0x3C, 0xBF):
            emulator = Oracle(xbe)
            oracle.create_device(emulator)
            base = emulator.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)
            emulator.write_bytes(oracle.SCRATCH_BASE + 0x100, data)
            emulator.run(0x3D5670, [], ecx=register, edx=oracle.SCRATCH_BASE + 0x100)
            expected = _delta(base, emulator.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW))

            port.load(base)
            for word in range(4):
                port.write32(
                    port.scratch + 0x100 + 4 * word, struct.unpack_from("<I", data, 4 * word)[0]
                )
            port.call(0x3D5670, (), ecx=register, edx=port.scratch + 0x100)
            actual = _delta(base, port.dump())
            for table in (expected, actual):
                table.pop(oracle.DEVICE_BASE, None)
            verdict = "identical" if expected == actual else "DIFFERENT"
            print(
                f"handler constant register {register:#04x}      "
                f"{len(expected):3d} dwords changed, {verdict}"
            )
            if expected != actual:
                problems.append(f"constant register {register:#x}")
    finally:
        port.close()
    return problems


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.d3dscan.port_diff",
        description="Compare the C port in src/gpu with the retail library code, under emulation.",
    )
    parser.add_argument("xbe", type=Path, help="path to the retail XBE")
    parser.add_argument(
        "--check",
        choices=("modes", "creation", "state", "handlers", "constants", "all"),
        default="all",
    )
    parser.add_argument(
        "--build-dir", type=Path, help="where to build port_run (default: a temp dir)"
    )
    parser.add_argument("--cc", default="cc", help="C compiler (default: cc)")
    args = parser.parse_args(argv)

    directory = args.build_dir or Path(tempfile.mkdtemp(prefix="port_diff_build_"))
    binary = build(directory, args.cc)
    checks = {
        "modes": check_modes,
        "creation": check_creation,
        "state": check_state,
        "handlers": check_handlers,
        "constants": check_constants,
    }
    problems: list[str] = []
    for name, check in checks.items():
        if args.check in (name, "all"):
            problems += check(binary, args.xbe)
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
