# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolated native x87 operation probe, not a live guest execution producer."""

from __future__ import annotations

import ctypes
import platform
import struct
import subprocess
from pathlib import Path

from tools.harness.x87_state import X87State


def build_probe(output: Path, *, compiler: str = "cc", source: Path | None = None) -> Path:
    """Build explicitly in caller-owned scratch space; caller owns scheduling."""
    if platform.machine() != "x86_64":
        raise RuntimeError("native x87 probe requires x86_64")
    output.parent.mkdir(parents=True, exist_ok=True)
    source = source or Path(__file__).with_suffix(".c")
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fPIC",
            "-shared",
            str(source),
            "-o",
            str(output),
        ],
        check=True,
    )
    return output


class NativeX87:
    """Strict raw-state adapter for the standalone native instruction probe."""

    def __init__(self, library: Path) -> None:
        self.library = ctypes.CDLL(str(library.resolve()))
        self.operation = self.library.x87_operation
        self.operation.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.operation.restype = ctypes.c_int

    def execute(
        self,
        state: X87State,
        operation: int,
        operand: int = 0,
        *,
        host_canary: X87State | None = None,
    ) -> tuple[X87State, int]:
        if type(operation) is not int or operation not in range(4):
            raise ValueError("unsupported x87 operation")
        if type(operand) is not int or not 0 <= operand <= 0xFFFFFFFF:
            raise ValueError("operand must be raw binary32")
        raw = b"".join(slot.to_bytes(10, "little") for slot in state.physical)
        raw += struct.pack("<HHH", state.tags, state.control, state.status)
        before = ctypes.create_string_buffer(raw, 86)
        after = ctypes.create_string_buffer(86)
        stored = ctypes.c_uint32()
        if host_canary is None:
            result = self.operation(before, after, operation, operand, ctypes.byref(stored))
        else:
            canary = b"".join(slot.to_bytes(10, "little") for slot in host_canary.physical)
            canary += struct.pack("<HHH", host_canary.tags, host_canary.control, host_canary.status)
            host = ctypes.create_string_buffer(canary, 86)
            call = self.library.x87_operation_canary
            call.argtypes = [ctypes.c_void_p, *self.operation.argtypes]
            call.restype = ctypes.c_int
            result = call(host, before, after, operation, operand, ctypes.byref(stored))
        if result:
            raise RuntimeError(f"native x87 refused or failed: {result}")
        data = after.raw
        slots = tuple(
            int.from_bytes(data[index : index + 10], "little") for index in range(0, 80, 10)
        )
        tags, control, status = struct.unpack("<HHH", data[80:])
        return X87State(slots, tags, control, status), stored.value
