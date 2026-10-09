# SPDX-License-Identifier: GPL-3.0-or-later
"""An OHCI USB host controller and the USB devices on it, as a MODEL for running the title's
statically linked XPP stack under emulation.

WHAT IT IS FOR. The title's peripheral library (`XPP`) is a USB host stack. Its answers to
`XGetDevices`, `XInputOpen` and `XInputGetCapabilities` are produced by talking to an OHCI
controller, so running the real library bytes with no controller proves nothing about a connected
pad. This module is just enough of OHCI (open host controller interface, release 1.0a) and just
enough USB to let the REAL library enumerate a device: root hub ports with connect and reset
behaviour, endpoint descriptor lists, transfer descriptors, the done queue and the interrupt.

WHAT IT IS NOT. It is not a USB stack and claims nothing about timing, bandwidth, error recovery
or low level signalling. A transfer completes the next time the model is pumped. A device here is a
dictionary of what it answers, and every answer the model gives that the real hardware could not be
asked to confirm is listed in `docs/xpp-init.md` under "what the model fabricates".

THE DEVICES. `XidGamepad` is a gamepad that answers the XID class requests and reports a fixed
input. Its descriptors are defaults chosen so the library accepts the device, and they are an INPUT
of every comparison in `port_diff.py`: both the library and the port are given the same device, so
the comparison is about what each does with it, never about what the device says.
"""

from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass, field

# --- OHCI operational registers, byte offsets from the controller base -------------------------
HC_REVISION = 0x00
HC_CONTROL = 0x04
HC_COMMAND_STATUS = 0x08
HC_INTERRUPT_STATUS = 0x0C
HC_INTERRUPT_ENABLE = 0x10
HC_INTERRUPT_DISABLE = 0x14
HC_HCCA = 0x18
HC_PERIOD_CURRENT_ED = 0x1C
HC_CONTROL_HEAD_ED = 0x20
HC_CONTROL_CURRENT_ED = 0x24
HC_BULK_HEAD_ED = 0x28
HC_BULK_CURRENT_ED = 0x2C
HC_DONE_HEAD = 0x30
HC_FM_INTERVAL = 0x34
HC_FM_REMAINING = 0x38
HC_FM_NUMBER = 0x3C
HC_PERIODIC_START = 0x40
HC_LS_THRESHOLD = 0x44
HC_RH_DESCRIPTOR_A = 0x48
HC_RH_DESCRIPTOR_B = 0x4C
HC_RH_STATUS = 0x50
HC_RH_PORT_STATUS = 0x54

# HcControl
CTRL_PLE = 1 << 2
CTRL_CLE = 1 << 4
CTRL_BLE = 1 << 5
CTRL_HCFS_SHIFT = 6
HCFS_OPERATIONAL = 2

# HcInterruptStatus and Enable
INT_WDH = 1 << 1
INT_SF = 1 << 2
INT_FNO = 1 << 5
INT_RHSC = 1 << 6
INT_MIE = 1 << 31

# HcCommandStatus
CMD_HCR = 1 << 0

# HcRhPortStatus read bits
PORT_CCS = 1 << 0
PORT_PES = 1 << 1
PORT_PRS = 1 << 4
PORT_PPS = 1 << 8
PORT_LSDA = 1 << 9
PORT_CSC = 1 << 16
PORT_PESC = 1 << 17
PORT_PRSC = 1 << 20

ED_FORMAT_ISO = 1 << 15

# Condition codes
CC_NO_ERROR = 0x0
CC_STALL = 0x4
CC_DATA_UNDERRUN = 0x9
CC_NOT_ACCESSED = 0xE

# Where a physical address the controller is handed lives in the oracle's guest address space.
CONTIGUOUS_VIRTUAL = 0x80000000
CONTIGUOUS_PHYSICAL_LOW = 0x01000000
CONTIGUOUS_PHYSICAL_HIGH = 0x05000000


def physical_to_virtual(physical: int) -> int:
    """The oracle's inverse of its MmGetPhysicalAddress: contiguous memory sits at 0x80000000 | p,
    everything else is identity mapped."""
    if CONTIGUOUS_PHYSICAL_LOW <= physical < CONTIGUOUS_PHYSICAL_HIGH:
        return CONTIGUOUS_VIRTUAL | physical
    return physical


class Memory:
    """What the controller needs from the guest: dwords and bytes at VIRTUAL addresses."""

    def physical_to_virtual(self, physical: int) -> int:
        return physical_to_virtual(physical)

    def read32(self, address: int) -> int:
        raise NotImplementedError

    def write32(self, address: int, value: int) -> None:
        raise NotImplementedError

    def read_bytes(self, address: int, length: int) -> bytes:
        raise NotImplementedError

    def write_bytes(self, address: int, data: bytes) -> None:
        raise NotImplementedError


@dataclass
class Transfer:
    """One USB transaction as the controller sees it, for the log and for tests."""

    address: int
    endpoint: int
    kind: str
    detail: str


class UsbDevice:
    """A device on a root hub port. Subclasses answer class and vendor requests."""

    low_speed = False

    def __init__(self) -> None:
        self.address = 0
        self.configuration = 0
        self._pending_address: int | None = None

    # standard descriptors, overridden by subclasses
    def device_descriptor(self) -> bytes:
        raise NotImplementedError

    def configuration_descriptor(self) -> bytes:
        raise NotImplementedError

    def string_descriptor(self, index: int) -> bytes | None:
        return None

    def reset(self) -> None:
        self.address = 0
        self.configuration = 0
        self._pending_address = None

    def control(self, setup: bytes, data: bytes) -> bytes | None:
        """A control transfer. `data` is the OUT data stage. Returns the IN data stage (possibly
        empty) or None to STALL."""
        request_type, request, value, index, length = struct.unpack("<BBHHH", setup)
        kind = (request_type >> 5) & 3
        if kind == 0:
            return self._standard(request_type, request, value, index, length, data)
        return self.class_request(request_type, request, value, index, length, data)

    def finish_status_stage(self) -> None:
        """SET_ADDRESS takes effect after its status stage."""
        if self._pending_address is not None:
            self.address = self._pending_address
            self._pending_address = None

    def _standard(
        self, request_type: int, request: int, value: int, index: int, length: int, data: bytes
    ) -> bytes | None:
        del request_type, index, data
        if request == 6:  # GET_DESCRIPTOR
            kind = value >> 8
            if kind == 1:
                return self.device_descriptor()[:length]
            if kind == 2:
                return self.configuration_descriptor()[:length]
            if kind == 3:
                text = self.string_descriptor(value & 0xFF)
                return None if text is None else text[:length]
            return None
        if request == 5:  # SET_ADDRESS
            self._pending_address = value & 0x7F
            return b""
        if request == 9:  # SET_CONFIGURATION
            self.configuration = value & 0xFF
            return b""
        if request == 8:  # GET_CONFIGURATION
            return bytes([self.configuration])[:length]
        if request == 0:  # GET_STATUS
            return b"\x00\x00"[:length]
        if request in (1, 3, 7, 11):  # CLEAR_FEATURE, SET_FEATURE, SET_DESCRIPTOR, SET_INTERFACE
            return b""
        return None

    def class_request(
        self, request_type: int, request: int, value: int, index: int, length: int, data: bytes
    ) -> bytes | None:
        del request_type, request, value, index, length, data
        return None

    def interrupt_in(self, endpoint: int) -> bytes | None:
        """The next report on an interrupt IN endpoint, or None for NAK."""
        del endpoint
        return None

    def interrupt_out(self, endpoint: int, data: bytes) -> bool:
        del endpoint, data
        return False

    def bulk_in(self, endpoint: int, length: int) -> bytes | None:
        del endpoint, length
        return None

    def bulk_out(self, endpoint: int, data: bytes) -> bool:
        del endpoint, data
        return False

    def iso_in(self, endpoint: int, length: int) -> bytes | None:
        """The bytes an isochronous IN endpoint delivers in one frame (at most `length`), or None
        for a missed frame (the model reports DATA_UNDERRUN-free zero length)."""
        del endpoint, length
        return None

    def iso_out(self, endpoint: int, data: bytes) -> bool:
        """One frame of isochronous OUT data. True when the device accepted it."""
        del endpoint, data
        return False


class XidGamepad(UsbDevice):
    """An Xbox controller: interface class 0x58, subclass 0x42, one interrupt IN and one interrupt
    OUT endpoint, XID capability requests, a 20 byte input report and a 6 byte output report."""

    INPUT_REPORT_BYTES = 20
    OUTPUT_REPORT_BYTES = 6

    def __init__(self, sub_type: int = 1) -> None:
        super().__init__()
        self.sub_type = sub_type
        # report id, length, buttons, 8 analog buttons, 4 thumb axes
        self.input_report = bytearray(self.INPUT_REPORT_BYTES)
        self.input_report[1] = self.INPUT_REPORT_BYTES
        self.report_dirty = True
        self.output_reports: list[bytes] = []
        self.input_capabilities = bytes([0, 0x14]) + b"\xff" * 18
        self.output_capabilities = bytes([0, 0x06]) + b"\xff" * 4

    def set_input(self, buttons: int, analog: bytes, thumbs: tuple[int, int, int, int]) -> None:
        """Replace the report. `analog` is the eight pressure bytes, `thumbs` the signed axes."""
        if len(analog) != 8:
            raise ValueError("input requires exactly eight pressure bytes")
        report = bytearray(self.INPUT_REPORT_BYTES)
        report[1] = self.INPUT_REPORT_BYTES
        struct.pack_into("<H", report, 2, buttons & 0xFFFF)
        report[4:12] = analog
        struct.pack_into("<hhhh", report, 12, *thumbs)
        if report != self.input_report:
            self.input_report = report
            self.report_dirty = True

    def device_descriptor(self) -> bytes:
        return struct.pack(
            "<BBHBBBBHHHBBBB", 18, 1, 0x0110, 0, 0, 0, 8, 0x045E, 0x0202, 0x0100, 0, 0, 0, 1
        )

    def configuration_descriptor(self) -> bytes:
        config = struct.pack("<BBHBBBBB", 9, 2, 32, 1, 1, 0, 0x80, 50)
        interface = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 2, 0x58, 0x42, 0, 0)
        endpoint_in = struct.pack("<BBBBHB", 7, 5, 0x82, 3, 0x20, 4)
        endpoint_out = struct.pack("<BBBBHB", 7, 5, 0x02, 3, 0x20, 4)
        return config + interface + endpoint_in + endpoint_out

    def xid_descriptor(self) -> bytes:
        return struct.pack(
            "<BBHBBBB4H",
            16,
            0x42,
            0x0100,
            1,
            self.sub_type,
            self.INPUT_REPORT_BYTES,
            self.OUTPUT_REPORT_BYTES,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
        )

    def class_request(
        self, request_type: int, request: int, value: int, index: int, length: int, data: bytes
    ) -> bytes | None:
        del index
        if request_type == 0xC1 and request == 0x06 and value == 0x4200:
            return self.xid_descriptor()[:length]
        if request_type == 0xC1 and request == 0x01 and value == 0x0100:
            return self.input_capabilities[:length]
        if request_type == 0xC1 and request == 0x01 and value == 0x0200:
            return self.output_capabilities[:length]
        if request_type == 0xA1 and request == 0x01 and value == 0x0100:
            return bytes(self.input_report)[:length]
        if request_type == 0x21 and request == 0x09 and value == 0x0200:
            self.output_reports.append(bytes(data))
            return b""
        return None

    def interrupt_in(self, endpoint: int) -> bytes | None:
        if endpoint != 2 or not self.report_dirty:
            return None
        self.report_dirty = False
        return bytes(self.input_report)

    def interrupt_out(self, endpoint: int, data: bytes) -> bool:
        if endpoint != 2:
            return False
        self.output_reports.append(bytes(data))
        return True


@dataclass
class RootPort:
    device: UsbDevice | None = None
    connected: bool = False
    enabled: bool = False
    reset: bool = False
    connect_change: bool = False
    enable_change: bool = False
    reset_change: bool = False

    def status(self) -> int:
        value = PORT_PPS
        value |= PORT_CCS if self.connected else 0
        value |= PORT_PES if self.enabled else 0
        value |= PORT_PRS if self.reset else 0
        value |= PORT_CSC if self.connect_change else 0
        value |= PORT_PESC if self.enable_change else 0
        value |= PORT_PRSC if self.reset_change else 0
        return value


@dataclass
class Ohci:
    """The controller. Registers are plain, side effects are the OHCI ones the library relies on."""

    memory: Memory
    port_count: int = 4
    log: list[Transfer] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.revision = 0x110
        self.control = 0
        self.command_status = 0
        self.interrupt_status = 0
        self.interrupt_enable = 0
        self.hcca = 0
        self.control_head = 0
        self.bulk_head = 0
        self.done_head = 0
        self.fm_interval = 0x2EDF
        self.periodic_start = 0
        self.ls_threshold = 0
        self.rh_descriptor_a = self.port_count
        self.rh_descriptor_b = 0
        self.rh_status = 0
        self.ports = [RootPort() for _ in range(self.port_count)]
        self.devices_by_address: dict[int, UsbDevice] = {}
        self.register_trace: list[tuple[str, int, int]] = []
        self.frame_number = 0
        self._control_state: dict[int, tuple[bytes, bytes, bytes]] = {}

    # ------------------------------------------------------------------ registers

    def read(self, offset: int) -> int:
        value = self._read(offset)
        self.register_trace.append(("r", offset, value))
        return value

    def _read(self, offset: int) -> int:
        if offset == HC_REVISION:
            return self.revision
        if offset == HC_CONTROL:
            return self.control
        if offset == HC_COMMAND_STATUS:
            return self.command_status
        if offset == HC_INTERRUPT_STATUS:
            return self.interrupt_status
        if offset in (HC_INTERRUPT_ENABLE, HC_INTERRUPT_DISABLE):
            return self.interrupt_enable
        if offset == HC_HCCA:
            return self.hcca
        if offset == HC_CONTROL_HEAD_ED:
            return self.control_head
        if offset == HC_BULK_HEAD_ED:
            return self.bulk_head
        if offset == HC_DONE_HEAD:
            return self.done_head
        if offset == HC_FM_NUMBER:
            return self.frame_number
        if offset == HC_FM_INTERVAL:
            return self.fm_interval
        if offset == HC_PERIODIC_START:
            return self.periodic_start
        if offset == HC_RH_DESCRIPTOR_A:
            return self.rh_descriptor_a & ~0xFF | self.port_count
        if offset == HC_RH_DESCRIPTOR_B:
            return self.rh_descriptor_b
        if offset == HC_RH_STATUS:
            return self.rh_status
        if offset >= HC_RH_PORT_STATUS:
            index = (offset - HC_RH_PORT_STATUS) // 4
            if index < self.port_count:
                return self.ports[index].status()
        return 0

    def write(self, offset: int, value: int) -> None:
        self.register_trace.append(("w", offset, value))
        value &= 0xFFFFFFFF
        if offset == HC_CONTROL:
            self.control = value
        elif offset == HC_COMMAND_STATUS:
            if value & CMD_HCR:
                self._software_reset()
            self.command_status |= value & ~CMD_HCR
        elif offset == HC_INTERRUPT_STATUS:
            self.interrupt_status &= ~value
        elif offset == HC_INTERRUPT_ENABLE:
            self.interrupt_enable |= value
        elif offset == HC_INTERRUPT_DISABLE:
            self.interrupt_enable &= ~value
        elif offset == HC_HCCA:
            self.hcca = value & ~0xFF
        elif offset == HC_CONTROL_HEAD_ED:
            self.control_head = value & ~0xF
        elif offset == HC_BULK_HEAD_ED:
            self.bulk_head = value & ~0xF
        elif offset == HC_DONE_HEAD:
            self.done_head = value & ~0xF
        elif offset == HC_FM_INTERVAL:
            self.fm_interval = value
        elif offset == HC_PERIODIC_START:
            self.periodic_start = value
        elif offset == HC_LS_THRESHOLD:
            self.ls_threshold = value
        elif offset == HC_RH_DESCRIPTOR_A:
            self.rh_descriptor_a = value
        elif offset == HC_RH_DESCRIPTOR_B:
            self.rh_descriptor_b = value
        elif offset == HC_RH_STATUS:
            self.rh_status = value
        elif offset >= HC_RH_PORT_STATUS:
            index = (offset - HC_RH_PORT_STATUS) // 4
            if index < self.port_count:
                self._write_port(index, value)

    def _software_reset(self) -> None:
        self.control = 0
        self.interrupt_status = 0
        self.interrupt_enable = 0
        self.done_head = 0
        self._control_state.clear()

    def _write_port(self, index: int, value: int) -> None:
        port = self.ports[index]
        if value & (1 << 0):  # ClearPortEnable
            port.enabled = False
        if value & (1 << 1) and port.connected:  # SetPortEnable
            port.enabled = True
        if value & (1 << 4) and port.connected:  # SetPortReset
            port.reset = True
            # The reset completes at once: the device forgets its address and the port enables.
            port.reset = False
            port.enabled = True
            port.reset_change = True
            if port.device is not None:
                port.device.reset()
                self.devices_by_address.pop(port.device.address, None)
            self.interrupt_status |= INT_RHSC
        if value & PORT_CSC:
            port.connect_change = False
        if value & PORT_PESC:
            port.enable_change = False
        if value & PORT_PRSC:
            port.reset_change = False

    # ------------------------------------------------------------------ the root hub, driven by the
    # scenario

    def attach(self, index: int, device: UsbDevice) -> None:
        port = self.ports[index]
        port.device = device
        port.connected = True
        port.connect_change = True
        self.interrupt_status |= INT_RHSC

    def detach(self, index: int) -> None:
        port = self.ports[index]
        if port.device is not None:
            self.devices_by_address.pop(port.device.address, None)
        port.device = None
        port.connected = False
        port.enabled = False
        port.connect_change = True
        self.interrupt_status |= INT_RHSC

    def interrupt_pending(self) -> bool:
        return bool(
            self.interrupt_enable & INT_MIE
            and self.interrupt_status & self.interrupt_enable & 0x7FFFFFFF
        )

    # ------------------------------------------------------------------ physical memory

    def _r32(self, physical: int) -> int:
        return self.memory.read32(self.memory.physical_to_virtual(physical))

    def _w32(self, physical: int, value: int) -> None:
        self.memory.write32(self.memory.physical_to_virtual(physical), value)

    def _rb(self, physical: int, length: int) -> bytes:
        return self.memory.read_bytes(self.memory.physical_to_virtual(physical), length)

    def _wb(self, physical: int, data: bytes) -> None:
        self.memory.write_bytes(self.memory.physical_to_virtual(physical), data)

    # ------------------------------------------------------------------ list processing

    def _device_at(self, function_address: int) -> UsbDevice | None:
        for port in self.ports:
            device = port.device
            if device is not None and port.enabled and device.address == function_address:
                return device
        return None

    def set_frame(self, frame: int) -> None:
        """The frame counter the hardware advances once a millisecond, mirrored into the HCCA the
        way the controller does at each frame start."""
        frame &= 0xFFFF
        if frame != self.frame_number:
            self.interrupt_status |= INT_SF
            if (frame ^ self.frame_number) & 0x8000:
                self.interrupt_status |= INT_FNO
        self.frame_number = frame
        if self.hcca and (self.control >> CTRL_HCFS_SHIFT) & 3 == HCFS_OPERATIONAL:
            self._w32(self.hcca + 0x80, self.frame_number)

    def process(self) -> bool:
        """Run every enabled list once. True if any transfer descriptor retired."""
        if (self.control >> CTRL_HCFS_SHIFT) & 3 != HCFS_OPERATIONAL:
            return False
        retired = False
        if self.control & CTRL_CLE:
            retired |= self._walk(self.control_head, periodic=False)
        if self.control & CTRL_BLE:
            retired |= self._walk(self.bulk_head, periodic=False)
        if self.control & CTRL_PLE and self.hcca:
            seen: set[int] = set()
            for slot in range(32):
                head = self._r32(self.hcca + 4 * slot) & ~0xF
                if head in seen:
                    continue
                seen.add(head)
                retired |= self._walk(head, periodic=True)
        if retired:
            self._write_done_head()
        return retired

    def _walk(self, head: int, periodic: bool) -> bool:
        retired = False
        guard = 0
        ed = head
        while ed and guard < 64:
            guard += 1
            retired |= self._service_ed(ed, periodic)
            ed = self._r32(ed + 0x0C) & ~0xF  # NextED is the fourth dword, not adjacent data.
        return retired

    def _service_ed(self, ed: int, periodic: bool) -> bool:
        word = self._r32(ed)
        function_address = word & 0x7F
        endpoint = (word >> 7) & 0xF
        direction = (word >> 11) & 3
        skip = bool(word & (1 << 14))
        if word & ED_FORMAT_ISO:
            return self._service_iso_ed(ed, word)
        tail = self._r32(ed + 4) & ~0xF
        head_word = self._r32(ed + 8)
        head = head_word & ~0xF
        halted = bool(head_word & 1)
        carry = (head_word >> 1) & 1
        if skip or halted or head == tail:
            return False
        retired = False
        steps = 0
        while head != tail and not halted and steps < 16:
            steps += 1
            outcome = self._execute_td(head, function_address, endpoint, direction, carry, periodic)
            if outcome is None:
                break  # NAK: the TD stays at the head
            carry, condition = outcome
            next_td = self._r32(head + 8) & ~0xF
            self._retire(head, condition)
            retired = True
            head = next_td
            halted = condition != CC_NO_ERROR
            self._w32(ed + 8, head | (carry << 1) | int(halted))
        return retired

    def _service_iso_ed(self, ed: int, word: int) -> bool:
        """Isochronous TDs (OHCI 1.0a section 4.3.2): each of the FC+1 frames is serviced once the
        frame counter reaches StartingFrame+i, then the TD retires with NoError. MODEL: every
        frame succeeds when the device answers, the model has no bandwidth or timing faults."""
        if word & (1 << 14):
            return False
        function_address = word & 0x7F
        endpoint = (word >> 7) & 0xF
        tail = self._r32(ed + 4) & ~0xF
        head_word = self._r32(ed + 8)
        head = head_word & ~0xF
        if head == tail or head_word & 1:
            return False
        device = self._device_at(function_address)
        if device is None:
            return False
        retired = False
        steps = 0
        while head != tail and steps < 16:
            steps += 1
            if not self._execute_iso_td(head, device, endpoint, (word >> 11) & 3):
                break
            next_td = self._r32(head + 8) & ~0xF
            self._w32(head, (self._r32(head) & 0x0FFFFFFF) | (CC_NO_ERROR << 28))
            self._w32(head + 8, self.done_head)
            self.done_head = head
            retired = True
            head = next_td
            self._w32(ed + 8, head | (head_word & 2))
        return retired

    def _execute_iso_td(self, td: int, device: UsbDevice, endpoint: int, direction: int) -> bool:
        """Service every frame that is due. True once all FC+1 frames are done."""
        word = self._r32(td)
        start = word & 0xFFFF
        frames = ((word >> 24) & 7) + 1
        buffer_page = self._r32(td + 4) & ~0xFFF
        end_page = self._r32(td + 0xC) & ~0xFFF
        end_address = self._r32(td + 0xC)
        offsets = [self._iso_psw(td, index) for index in range(frames)]
        token = (word >> 19) & 3
        if direction in (1, 2):
            token = direction
        for index in range(frames):
            status = offsets[index]
            if status >> 12 != CC_NOT_ACCESSED:
                continue
            if (self.frame_number - (start + index)) & 0xFFFF >= 0x8000:
                return False  # this frame is still in the future
            begin = self._iso_address(offsets[index], buffer_page, end_page)
            if index + 1 < frames:
                finish = self._iso_address(offsets[index + 1], buffer_page, end_page)
            else:
                finish = end_address + 1
            length = max(0, finish - begin)
            if token == 2:
                data = device.iso_in(endpoint, length) or b""
                data = data[:length]
                if data:
                    self._wb(begin, data)
                self._set_iso_psw(td, index, (CC_NO_ERROR << 12) | len(data))
                self.log.append(Transfer(device.address, endpoint, "iso-in", str(len(data))))
            else:
                data = self._rb(begin, length) if length else b""
                accepted = device.iso_out(endpoint, data)
                self._set_iso_psw(td, index, (CC_NO_ERROR if accepted else CC_STALL) << 12)
                self.log.append(Transfer(device.address, endpoint, "iso-out", data.hex()))
        return True

    @staticmethod
    def _iso_address(psw_offset: int, buffer_page: int, end_page: int) -> int:
        page = end_page if psw_offset & 0x1000 else buffer_page
        return page + (psw_offset & 0xFFF)

    def _iso_psw(self, td: int, index: int) -> int:
        return (self._r32(td + 0x10 + 4 * (index // 2)) >> (16 * (index & 1))) & 0xFFFF

    def _set_iso_psw(self, td: int, index: int, value: int) -> None:
        address = td + 0x10 + 4 * (index // 2)
        shift = 16 * (index & 1)
        self._w32(address, (self._r32(address) & ~(0xFFFF << shift)) | (value << shift))

    def _retire(self, td: int, condition: int) -> None:
        word = self._r32(td)
        word = (word & 0x0FFFFFFF) | (condition << 28)
        word &= ~(3 << 26)  # error count
        self._w32(td, word)
        self._w32(td + 8, self.done_head)
        self.done_head = td

    def _write_done_head(self) -> None:
        if not self.hcca or self.interrupt_status & INT_WDH:
            return
        self._w32(self.hcca + 0x84, self.done_head)
        self.done_head = 0
        self.interrupt_status |= INT_WDH

    def _execute_td(
        self,
        td: int,
        function_address: int,
        endpoint: int,
        direction: int,
        carry: int,
        periodic: bool,
    ) -> tuple[int, int] | None:
        word = self._r32(td)
        buffer_pointer = self._r32(td + 4)
        buffer_end = self._r32(td + 0xC)
        rounding = bool(word & (1 << 18))
        token = (word >> 19) & 3
        if direction in (1, 2):
            token = {1: 1, 2: 2}[direction]
        toggle_bits = (word >> 24) & 3
        toggle = (toggle_bits & 1) if toggle_bits & 2 else carry
        length = self._buffer_length(buffer_pointer, buffer_end)
        device = self._device_at(function_address)
        if device is None:
            return None
        next_toggle = toggle ^ 1
        if endpoint == 0:
            return self._control_td(
                td, device, token, buffer_pointer, length, rounding, next_toggle
            )
        if token == 2:
            report = device.interrupt_in(endpoint) if periodic else device.bulk_in(endpoint, length)
            if report is None:
                return None
            return self._complete_in(td, buffer_pointer, length, report, rounding, next_toggle)
        data = self._rb(buffer_pointer, length) if length else b""
        accepted = (
            device.interrupt_out(endpoint, data) if periodic else device.bulk_out(endpoint, data)
        )
        self.log.append(Transfer(device.address, endpoint, "out", data.hex()))
        if not accepted:
            return next_toggle, CC_STALL
        self._w32(td + 4, 0)
        return next_toggle, CC_NO_ERROR

    @staticmethod
    def _buffer_length(buffer_pointer: int, buffer_end: int) -> int:
        if not buffer_pointer:
            return 0
        if buffer_pointer >> 12 == buffer_end >> 12:
            return buffer_end - buffer_pointer + 1
        return (0x1000 - (buffer_pointer & 0xFFF)) + (buffer_end & 0xFFF) + 1

    def _complete_in(
        self, td: int, buffer_pointer: int, length: int, report: bytes, rounding: bool, toggle: int
    ) -> tuple[int, int]:
        taken = report[:length]
        if taken:
            self._wb(buffer_pointer, taken)
        if len(taken) < length:
            if not rounding:
                self._w32(td + 4, buffer_pointer + len(taken))
                return toggle, CC_DATA_UNDERRUN
            self._w32(td + 4, buffer_pointer + len(taken))
        else:
            self._w32(td + 4, 0)
        return toggle, CC_NO_ERROR

    def _control_td(
        self,
        td: int,
        device: UsbDevice,
        token: int,
        buffer_pointer: int,
        length: int,
        rounding: bool,
        toggle: int,
    ) -> tuple[int, int]:
        key = id(device)
        state = self._control_state.get(key)
        if token == 0:  # SETUP
            setup = self._rb(buffer_pointer, 8)
            _, _, _, _, wlength = struct.unpack("<BBHHH", setup)
            direction_in = bool(setup[0] & 0x80)
            self.log.append(Transfer(device.address, 0, "setup", setup.hex()))
            if direction_in or wlength == 0:
                reply = device.control(setup, b"")
                if reply is None:
                    self._control_state.pop(key, None)
                    self._w32(td + 4, 0)
                    return toggle, CC_STALL
                self._control_state[key] = (setup, reply, b"")
            else:
                self._control_state[key] = (setup, b"", b"")
            self._w32(td + 4, 0)
            return toggle, CC_NO_ERROR
        if state is None:
            return toggle, CC_STALL
        setup, reply, collected = state
        wlength = struct.unpack("<BBHHH", setup)[4]
        if token == 2 and setup[0] & 0x80 and length and len(reply) is not None and wlength:
            # IN data stage: hand over what the device has, up to the buffer.
            outcome = self._complete_in(td, buffer_pointer, length, reply, rounding, toggle)
            self._control_state[key] = (setup, reply[length:], collected)
            return outcome
        if token == 1 and not setup[0] & 0x80 and length:
            data = self._rb(buffer_pointer, length)
            self._control_state[key] = (setup, reply, collected + data)
            self._w32(td + 4, 0)
            return toggle, CC_NO_ERROR
        # Status stage (zero length, opposite direction of the data stage).
        if not setup[0] & 0x80 and wlength:
            result = device.control(setup, collected)
            if result is None:
                self._control_state.pop(key, None)
                return toggle, CC_STALL
        device.finish_status_stage()
        for port in self.ports:
            if port.device is device:
                self.devices_by_address[device.address] = device
        self._control_state.pop(key, None)
        self._w32(td + 4, 0)
        return toggle, CC_NO_ERROR


InterruptHook = Callable[[], None]
