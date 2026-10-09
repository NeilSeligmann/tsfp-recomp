# SPDX-License-Identifier: GPL-3.0-or-later
"""Adapter for x87_native_ext.c (operations 0..6), leaving x87_native.py pinned."""

from __future__ import annotations

import ctypes
import struct

from tools.harness.x87_native import NativeX87
from tools.harness.x87_state import X87State


class NativeX87Ext(NativeX87):
    def execute(
        self,
        state: X87State,
        operation: int,
        operand: int = 0,
        *,
        host_canary: X87State | None = None,
    ) -> tuple[X87State, int]:
        if type(operation) is not int or operation not in range(7):
            raise ValueError("unsupported x87 operation")
        if host_canary is not None:
            raise ValueError("canary runs use the base adapter")
        raw = b"".join(slot.to_bytes(10, "little") for slot in state.physical)
        raw += struct.pack("<HHH", state.tags, state.control, state.status)
        before = ctypes.create_string_buffer(raw, 86)
        after = ctypes.create_string_buffer(86)
        stored = ctypes.c_uint32()
        result = self.operation(before, after, operation, operand, ctypes.byref(stored))
        if result:
            raise RuntimeError(f"native x87 refused or failed: {result}")
        data = after.raw
        slots = tuple(
            int.from_bytes(data[index : index + 10], "little") for index in range(0, 80, 10)
        )
        tags, control, status = struct.unpack("<HHH", data[80:])
        return X87State(slots, tags, control, status), stored.value
