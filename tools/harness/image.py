# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the flat guest image both sides load, and read code bytes out of it.

One image, produced once, handed to the oracle as bytes and to the subject as a file.
Both sides must start from BYTE-IDENTICAL memory or the comparison means nothing, so
there is deliberately only one producer of it.

The file format is a sequence of `va:u32le, length:u32le, bytes` records, which is what
`tools/harness/driver.c` reads. It is written under `generated/` or `tmp/`, never into
the repository: it is a verbatim copy of sections of the user's own binary.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from tools.xbe import parse_xbe

from .seeding import (
    GUEST_HI,
    GUEST_LO,
    GUEST_SPAN,
    SCRATCH_BASE,
    SCRATCH_WORDS,
    SEEDED_THUNK_SLOTS,
    THUNK_TARGET_BASE,
    SeedPolicy,
    scratch_arena_bytes,
    seeded_guest_dwords,
)


@dataclass(frozen=True, slots=True)
class SourceSection:
    """Original section bounds/permissions; does not change the flat runtime mapping."""

    address: int
    size: int
    executable: bool
    writable: bool

    def __post_init__(self) -> None:
        if (
            type(self.address) is not int
            or type(self.size) is not int
            or not 0 <= self.address < 1 << 32
            or not 0 < self.size <= (1 << 32) - self.address
            or type(self.executable) is not bool
            or type(self.writable) is not bool
        ):
            raise ValueError("invalid source section bounds/permissions")


@dataclass(frozen=True)
class GuestImage:
    """The guest window as a flat byte string, plus where each section landed."""

    data: bytes
    base: int = GUEST_LO
    source_sections: tuple[SourceSection, ...] | None = None

    def code_at(self, va: int, size: int) -> bytes:
        """`size` bytes at guest address `va`, or `b""` if that falls outside the window."""
        if va < self.base or va + size > self.base + len(self.data):
            return b""
        offset = va - self.base
        return self.data[offset : offset + size]


def build_guest_image(xbe_path: Path, *, policy: SeedPolicy | None = None) -> GuestImage:
    """Flatten every XBE section that fits in the guest window into one image.

    Sections outside `[GUEST_LO, GUEST_HI)` are dropped rather than clamped: a section
    half in the window would give the two sides different memory at the boundary, and a
    silent clamp is exactly the kind of thing that turns into an unexplainable
    disagreement weeks later.
    """
    raw = xbe_path.read_bytes()
    xbe = parse_xbe(raw)
    flat = bytearray(GUEST_SPAN)

    # The XBE headers live at the image base and real code reads them.
    header_len = min(0x1000, len(raw))
    flat[xbe.base_address - GUEST_LO : xbe.base_address - GUEST_LO + header_len] = raw[:header_len]

    for section in xbe.sections:
        if section.virtual_addr < GUEST_LO:
            continue
        if section.virtual_addr + section.virtual_size > GUEST_HI:
            continue
        length = min(section.raw_size, section.virtual_size)
        offset = section.virtual_addr - GUEST_LO
        body = raw[section.raw_addr : section.raw_addr + length]
        flat[offset : offset + len(body)] = body
        if section.virtual_addr < SCRATCH_BASE + SCRATCH_WORDS * 4 and (
            section.virtual_addr + section.virtual_size > SCRATCH_BASE
        ):
            # The scratch arena is pre-filled below, which would silently corrupt a
            # section that reached into it. In this title the highest section ends far
            # below the arena, but a different image must fail loudly rather than have
            # its data quietly overwritten.
            raise ValueError(
                f"section at {section.virtual_addr:#010x} overlaps the scratch arena at "
                f"{SCRATCH_BASE:#010x}; move SCRATCH_BASE"
            )

    # Pre-fill the scratch arena that registers and stack arguments are aimed at. This is
    # part of the INITIAL guest memory both sides load, so the two sides see identical
    # bytes by construction. Without it the arena reads as zero beyond the small
    # per-case patch, so a pointer chase loads 0 and the next dereference faults near
    # address 0 -- measured as the single largest named cause of ORACLE-FAULTED.
    arena = scratch_arena_bytes(policy)
    arena_offset = SCRATCH_BASE - GUEST_LO
    flat[arena_offset : arena_offset + len(arena)] = arena

    # Resolve the chosen kernel import thunk slots (see `SEEDED_THUNK_SLOTS`). Only a slot
    # still holding a raw ordinal (high bit set) is patched, so a different image fails loudly.
    for index, (slot, value) in enumerate(sorted(SEEDED_THUNK_SLOTS.items())):
        target = THUNK_TARGET_BASE + index * 4
        slot_offset = slot - GUEST_LO
        if flat[slot_offset + 3] & 0x80 == 0:
            raise ValueError(f"thunk slot {slot:#010x} does not hold a raw kernel ordinal")
        flat[slot_offset : slot_offset + 4] = target.to_bytes(4, "little")
        flat[target - GUEST_LO : target - GUEST_LO + 4] = value.to_bytes(4, "little")
    # The fs segment and TLS slots (see `KPCR_BASE`), baked in like the thunk targets.
    for address, value in seeded_guest_dwords().items():
        flat[address - GUEST_LO : address - GUEST_LO + 4] = value.to_bytes(4, "little")
    return GuestImage(
        data=bytes(flat),
        source_sections=(SourceSection(xbe.base_address, header_len, False, False),)
        + tuple(
            SourceSection(
                section.virtual_addr, section.virtual_size, section.executable, section.writable
            )
            for section in xbe.sections
            if section.virtual_size > 0
        ),
    )


def write_image_file(image: GuestImage, path: Path, *, chunk: int = 1 << 20) -> int:
    """Write `image` in the driver's record format. Returns the byte count written.

    Emitted as a handful of large records rather than one 16 MB record so the driver's
    read loop is exercised, and so a truncated file is detectable.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with path.open("wb") as stream:
        for offset in range(0, len(image.data), chunk):
            blob = image.data[offset : offset + chunk]
            stream.write((image.base + offset).to_bytes(4, "little"))
            stream.write(len(blob).to_bytes(4, "little"))
            stream.write(blob)
            written += len(blob)
    return written
