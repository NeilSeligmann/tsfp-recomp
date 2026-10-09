# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional OGhidra imports and the measured JPype byte-buffer correction.

Imported only after query.py verifies the upstream checkout. Never runs a model
client or the upstream mutation tools.
"""

from __future__ import annotations

import base64

import jpype
from src.ghidra_client import PyGhidraClient


class ReadClient(PyGhidraClient):
    """Use Java byte[] explicitly: JPype doesn't copy changes back to bytearray."""

    def executable_sha256(self) -> str:
        return str(self._require_program().getExecutableSHA256()).lower()

    def instruction_addresses(self, address: str) -> list[int]:
        program = self._require_program()
        function = self._get_function_for_address(self._address_from_hex(address))
        if function is None:
            raise RuntimeError(f"no function at {address}")
        return [
            int(instruction.getAddress().getOffset())
            for instruction in program.getListing().getInstructions(function.getBody(), True)
        ]

    def read_bytes(self, address: str, length: int = 16, format: str = "raw") -> str:
        if not 1 <= length <= 4096:
            raise ValueError("read length must be 1-4096")
        if format != "raw":
            raise ValueError("pilot byte reads use base64 raw format only")
        buffer = jpype.JArray(jpype.JByte)(length)
        count = (
            self._require_program().getMemory().getBytes(self._address_from_hex(address), buffer)
        )
        if count != length:
            raise RuntimeError(f"short Ghidra read at {address}: {count}/{length}")
        raw = bytes(int(value) & 0xFF for value in buffer)
        return base64.b64encode(raw).decode("ascii")
