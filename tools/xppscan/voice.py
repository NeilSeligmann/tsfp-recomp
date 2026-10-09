# SPDX-License-Identifier: GPL-3.0-or-later
"""A MODELLED Xbox voice (headset) USB device for the XPP oracle (T1086).

WHAT IS GROUNDED. The retail XPP code in `tmp/oxm-extract/retail/default.xbe` registers two
interface-class records (class 0x78 and 0x79, record at 0x0046C8C4/0x0046C8DC) whose insertion
callback is 0x00474862. That callback requires an interface descriptor and endpoint descriptors:
interface subclass 0 or 1 (0x80000400 otherwise), then the first isochronous IN endpoint
(`0x00470065(dev, 1, 1, 0)`) makes a microphone slot and, failing that, the first isochronous OUT
endpoint a headphone slot. The slot stores the endpoint address (+0x0A) and max packet size (+0x08).
Subclass 1 sets slot flag 0x80 (the high-fidelity microphone selector of 0x004752C6).

WHAT IS FABRICATED. The vendor/product ids, the string-free descriptors, the 0x41/3 request
answers and the PCM the microphone returns are model choices, never measured from a headset.
The Xbox Communicator may present both directions on one device. The original code only
shows two separate singles (IN-only mic, OUT-only headphone), which is what this model offers.
"""

from __future__ import annotations

import struct

from tools.xppscan.ohci import UsbDevice

VOICE_INTERFACE_CLASS = 0x78


class XboxVoiceDevice(UsbDevice):
    """A voice endpoint. `direction` is "in" (microphone) or "out" (headphone)."""

    def __init__(
        self,
        direction: str = "in",
        sub_class: int = 0,
        interface_class: int = VOICE_INTERFACE_CLASS,
        max_packet: int = 0x40,
        stall_requests: set[int] | None = None,
    ) -> None:
        super().__init__()
        if direction not in ("in", "out"):
            raise ValueError("direction must be 'in' or 'out'")
        self.direction = direction
        self.sub_class = sub_class
        self.interface_class = interface_class
        self.max_packet = max_packet
        self.requests: list[tuple[int, int, int, int, bytes]] = []
        self.captured: bytearray = bytearray()
        self.counter = 0
        # Ordinals (0 based, among vendor requests) the device STALLs. FABRICATED fault injection.
        self.stall_requests: set[int] = set(stall_requests or ())
        self._request_count = 0

    def device_descriptor(self) -> bytes:
        return struct.pack(
            "<BBHBBBBHHHBBBB", 18, 1, 0x0110, 0, 0, 0, 8, 0x045E, 0x0283, 0x0100, 0, 0, 0, 1
        )

    @property
    def endpoint_address(self) -> int:
        return 0x81 if self.direction == "in" else 0x01

    def configuration_descriptor(self) -> bytes:
        endpoint = struct.pack("<BBBBHB", 7, 5, self.endpoint_address, 1, self.max_packet, 1)
        interface = struct.pack(
            "<BBBBBBBBB", 9, 4, 0, 0, 1, self.interface_class, self.sub_class, 0, 0
        )
        total = 9 + len(interface) + len(endpoint)
        return struct.pack("<BBHBBBBB", 9, 2, total, 1, 1, 0, 0x80, 50) + interface + endpoint

    def class_request(
        self, request_type: int, request: int, value: int, index: int, length: int, data: bytes
    ) -> bytes | None:
        del length
        ordinal = self._request_count
        self._request_count += 1
        if ordinal in self.stall_requests:
            return None
        self.requests.append((request_type, request, value, index, bytes(data)))
        return b""  # FABRICATED: accept every vendor request the original issues

    def iso_in(self, endpoint: int, length: int) -> bytes | None:
        if self.direction != "in" or endpoint != 1:
            return None
        # FABRICATED PCM: a deterministic little-endian counter so tests can follow the bytes.
        data = bytes((self.counter + i) & 0xFF for i in range(length))
        self.counter = (self.counter + length) & 0xFF
        return data

    def iso_out(self, endpoint: int, data: bytes) -> bool:
        if self.direction != "out" or endpoint != 1:
            return False
        self.captured.extend(data)
        return True
