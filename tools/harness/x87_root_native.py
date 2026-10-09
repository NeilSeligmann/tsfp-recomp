# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and drive the isolated native lifted-root runner (`x87_root_native.c`)."""

from __future__ import annotations

import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tools.harness.x87_state import X87State

HERE = Path(__file__).resolve().parent


@dataclass(frozen=True)
class NativeResult:
    regs: tuple[int, ...] | None
    x87: X87State | None
    writes: dict[int, int] | None
    fault: str | None = None


def encode_state(state: X87State) -> str:
    raw = b"".join(slot.to_bytes(10, "little") for slot in state.physical)
    return (raw + struct.pack("<HHH", state.tags, state.control, state.status)).hex()


def decode_state(text: str) -> X87State:
    data = bytes.fromhex(text)
    if len(data) != 86:
        raise ValueError("x87 record width")
    slots = tuple(int.from_bytes(data[i : i + 10], "little") for i in range(0, 80, 10))
    tags, control, status = struct.unpack("<HHH", data[80:])
    return X87State(slots, tags, control, status)


def build(output: Path, inc_dir: Path, optimization: int, compiler: str = "cc") -> Path:
    """Compile the runner; `inc_dir` must hold `roots_lifted.inc` (authenticated lowered roots)."""
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            compiler,
            "-std=gnu11",
            f"-O{optimization}",
            "-Wall",
            "-Wextra",
            "-Wno-unused-variable",
            "-Wno-unused-label",
            "-fPIE",
            "-pie",
            f"-I{HERE}",
            f"-I{inc_dir}",
            str(HERE / "x87_root_native.c"),
            str(HERE / "x87_runtime.c"),
            str(HERE / "x87_native.c"),
            "-o",
            str(output),
        ],
        check=True,
        timeout=180,
    )
    return output


class NativeRoots:
    """One runner process; a death is recorded per case and the process is restarted."""

    def __init__(self, binary: Path, image_path: Path) -> None:
        self._binary, self._image = binary, image_path
        self._process: subprocess.Popen[str] | None = None

    def _start(self) -> None:
        self._process = subprocess.Popen(
            [str(self._binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert self._process.stdin and self._process.stdout
        self._process.stdin.write(f"IMAGE {self._image}\n")
        self._process.stdin.flush()
        if self._process.stdout.readline().strip() != "READY":
            self.close()
            raise RuntimeError("native root runner did not become ready")

    def close(self) -> None:
        process, self._process = self._process, None
        if process is not None:
            if process.stdin:
                process.stdin.close()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if process.stdout:
                process.stdout.close()

    def run(
        self,
        root: int,
        regs: tuple[int, ...],
        state: X87State,
        patches: tuple[tuple[int, bytes], ...],
    ) -> NativeResult:
        if self._process is None or self._process.poll() is not None:
            self._start()
        process = self._process
        assert process and process.stdin and process.stdout
        lines = ["REGS " + " ".join(f"{value:08x}" for value in regs), "X87 " + encode_state(state)]
        lines.extend(f"PATCH {address:x} {blob.hex()}" for address, blob in patches)
        lines.append(f"RUN {root:x}")
        try:
            process.stdin.write("\n".join(lines) + "\n")
            process.stdin.flush()
        except BrokenPipeError:
            self.close()
            return NativeResult(None, None, None, "runner-died-on-input")
        out_regs: tuple[int, ...] | None = None
        out_state: X87State | None = None
        writes: dict[int, int] = {}
        fault: str | None = None
        while True:
            line = process.stdout.readline()
            if not line:
                self.close()
                return NativeResult(None, None, None, fault or "runner-exit-without-record")
            fields = line.split()
            if not fields:
                continue
            if fields[0] == "END":
                break
            if fields[0] == "REGS":
                out_regs = tuple(int(item, 16) for item in fields[1:])
            elif fields[0] == "X87OUT":
                out_state = decode_state(fields[1])
            elif fields[0] == "W":
                writes[int(fields[1], 16)] = int(fields[2], 16)
            else:
                fault = line.strip()
        if fault is not None:
            self.close()
            return NativeResult(None, None, None, fault)
        return NativeResult(out_regs, out_state, writes)
