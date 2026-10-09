# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal i386 COFF `!<arch>` archive and object reader.

Produces, for every function-bearing code section, its bytes, a mask with every
relocated field wildcarded, and the relocations (offset, type, target symbol).
"""

from __future__ import annotations

import struct
from collections.abc import Iterator
from dataclasses import dataclass, field

ARCHIVE_MAGIC = b"!<arch>\n"
MACHINE_I386 = 0x14C

SCN_CNT_CODE = 0x20
SCN_LNK_COMDAT = 0x1000
SCN_LNK_NRELOC_OVFL = 0x01000000

REL_DIR32 = 0x06
REL_DIR32NB = 0x07
REL_SECTION = 0x0A
REL_SECREL = 0x0B
REL_REL32 = 0x14

# Bytes wildcarded per relocation type. SECTION fills a 16-bit field, the rest 32.
RELOC_WIDTH = {REL_DIR32: 4, REL_DIR32NB: 4, REL_SECREL: 4, REL_REL32: 4, REL_SECTION: 2}

SYM_CLASS_EXTERNAL = 2
SYM_CLASS_STATIC = 3
DTYPE_FUNCTION = 2
PADDING_BYTES = frozenset((0x00, 0x90, 0xCC))


class CoffError(ValueError):
    pass


@dataclass(frozen=True)
class Reloc:
    offset: int
    kind: int
    target: str
    addend: int = 0  # DIR32 only: the 4-byte in-place value, i.e. the offset added to the symbol


@dataclass
class FunctionBody:
    names: list[str]  # all symbols at the entry (aliases)
    data: bytes
    mask: bytes  # 0xFF where the byte is fixed, 0x00 where relocated
    relocs: list[Reloc] = field(default_factory=list)
    member: str = ""
    lib: str = ""
    section: str = ""  # linker section the body lives in, `$` suffix dropped

    @property
    def fixed_bytes(self) -> int:
        return sum(1 for value in self.mask if value)


def iter_members(data: bytes) -> Iterator[tuple[str, bytes]]:
    """Yield (name, bytes) of each object member, skipping the linker members."""
    if not data.startswith(ARCHIVE_MAGIC):
        raise CoffError("not an !<arch> archive")
    pos = len(ARCHIVE_MAGIC)
    longnames = b""
    index = 0
    while pos + 60 <= len(data):
        header = data[pos : pos + 60]
        if header[58:60] != b"`\n":
            raise CoffError(f"bad member header at {pos}")
        raw_name = header[:16].decode("latin-1")
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError as exc:
            raise CoffError(f"bad member size at {pos}") from exc
        body = data[pos + 60 : pos + 60 + size]
        pos += 60 + size + (size & 1)
        name = raw_name.strip()
        if name == "//":
            longnames = body
            index += 1
            continue
        if name == "/" and index < 2:  # first and second linker members
            index += 1
            continue
        index += 1
        if name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            end = longnames.find(b"\0", start)
            end = len(longnames) if end < 0 else end
            name = longnames[start:end].decode("latin-1").rstrip("/")
            name = name.rstrip("\n")
        else:
            name = name.rstrip("/")
        yield name, body


def _symbol_name(raw: bytes, strtab: bytes) -> str:
    if raw[:4] == b"\0\0\0\0":
        offset = struct.unpack_from("<I", raw, 4)[0]
        end = strtab.find(b"\0", offset)
        return strtab[offset : end if end >= 0 else None].decode("latin-1")
    return raw.rstrip(b"\0").decode("latin-1")


def functions_in_object(obj: bytes, member: str = "", lib: str = "") -> list[FunctionBody]:
    """Extract masked function bodies from one i386 COFF object.

    Returns [] for anything that is not an i386 object with code (import objects,
    LTCG intermediate-language objects, resource objects).
    """
    if len(obj) < 20:
        return []
    machine, nsect, _, symptr, nsyms, optsz, _ = struct.unpack_from("<HHIIIHH", obj)
    if machine != MACHINE_I386 or nsect == 0xFFFF:
        return []
    strtab_at = symptr + nsyms * 18
    strtab = (
        obj[strtab_at + 4 : strtab_at + struct.unpack_from("<I", obj, strtab_at)[0]]
        if (symptr and strtab_at + 4 <= len(obj))
        else b""
    )
    strtab = b"\0\0\0\0" + strtab  # offsets in names count the 4-byte size field

    symbols: list[
        tuple[str, int, int, int, int, int]
    ] = []  # name, value, section, type, class, naux
    slot_names: dict[int, str] = {}
    i = 0
    while i < nsyms:
        raw = obj[symptr + i * 18 : symptr + i * 18 + 18]
        if len(raw) < 18:
            raise CoffError("truncated symbol table")
        value, section, sym_type, klass, naux = struct.unpack_from("<IhHBB", raw, 8)
        name = _symbol_name(raw[:8], strtab)
        slot_names[i] = name
        symbols.append((name, value, section, sym_type, klass, naux))
        i += 1 + naux

    sections = []
    shdr = 20 + optsz
    for index in range(nsect):
        raw = obj[shdr + index * 40 : shdr + index * 40 + 40]
        if len(raw) < 40:
            raise CoffError("truncated section table")
        sections.append(struct.unpack_from("<8sIIIIIIHHI", raw))

    result: list[FunctionBody] = []
    for sec_no, sec in enumerate(sections, start=1):
        sec_name = _section_name(sec[0], strtab)
        _, _, _, rawsize, rawptr, relptr, _, nreloc, _, flags = sec
        if not flags & SCN_CNT_CODE or rawsize == 0 or rawptr == 0:
            continue
        data = obj[rawptr : rawptr + rawsize]
        if len(data) != rawsize:
            raise CoffError("section data out of range")
        relocs = [
            _with_addend(rel, data) for rel in _read_relocs(obj, relptr, nreloc, flags, slot_names)
        ]
        mask = bytearray(b"\xff" * rawsize)
        for rel in relocs:
            for k in range(RELOC_WIDTH.get(rel.kind, 4)):
                if rel.offset + k < rawsize:
                    mask[rel.offset + k] = 0
        entries: dict[int, list[str]] = {}
        for name, value, section, sym_type, klass, _ in symbols:
            if section != sec_no or klass not in (SYM_CLASS_EXTERNAL, SYM_CLASS_STATIC):
                continue
            # A static symbol only starts a function if typed as one; otherwise it is a
            # local label (assembly `loop_entry:`) and would split a body in two.
            if klass == SYM_CLASS_STATIC and sym_type >> 4 != DTYPE_FUNCTION:
                continue
            if not name or name[0] in ".$" or name == sec_name or value >= rawsize:
                continue
            if flags & SCN_LNK_COMDAT and value != 0:
                continue
            entries.setdefault(value, []).append(name)
        starts = sorted(entries)
        for pos, start in enumerate(starts):
            end = starts[pos + 1] if pos + 1 < len(starts) else rawsize
            if all(byte in PADDING_BYTES for byte in data[start:end]):
                continue
            result.append(
                FunctionBody(
                    names=sorted(set(entries[start])),
                    data=bytes(data[start:end]),
                    mask=bytes(mask[start:end]),
                    relocs=[
                        Reloc(r.offset - start, r.kind, r.target, r.addend)
                        for r in relocs
                        if start <= r.offset < end
                    ],
                    member=member,
                    lib=lib,
                    section=sec_name.split("$", 1)[0],
                )
            )
    return result


def _with_addend(rel: Reloc, data: bytes) -> Reloc:
    """Attach the in-place addend of a DIR32 reloc (the mask discards it, the matcher reuses it)."""
    if rel.kind != REL_DIR32 or rel.offset + 4 > len(data):
        return rel
    return Reloc(rel.offset, rel.kind, rel.target, struct.unpack_from("<I", data, rel.offset)[0])


def _read_relocs(
    obj: bytes, relptr: int, nreloc: int, flags: int, slot_names: dict[int, str]
) -> list[Reloc]:
    if not relptr:
        return []
    skip = 0
    if flags & SCN_LNK_NRELOC_OVFL and nreloc == 0xFFFF:
        nreloc = struct.unpack_from("<I", obj, relptr)[0]
        skip = 1
    out = []
    for k in range(skip, nreloc):
        at = relptr + k * 10
        if at + 10 > len(obj):
            raise CoffError("relocation table out of range")
        offset, symidx, kind = struct.unpack_from("<IIH", obj, at)
        out.append(Reloc(offset, kind, slot_names.get(symidx, f"<sym{symidx}>")))
    return out


def functions_in_archive(data: bytes, lib: str = "") -> tuple[list[FunctionBody], int]:
    """All function bodies of an archive, plus the count of unparseable members."""
    bodies: list[FunctionBody] = []
    skipped = 0
    for member, blob in iter_members(data):
        try:
            bodies.extend(functions_in_object(blob, member, lib))
        except (CoffError, struct.error):
            skipped += 1
    return bodies, skipped


def _section_name(raw: bytes, strtab: bytes) -> str:
    text = raw.rstrip(b"\0")
    if text[:1] == b"/" and text[1:].isdigit():
        offset = int(text[1:])
        end = strtab.find(b"\0", offset)
        return strtab[offset : end if end >= 0 else None].decode("latin-1")
    return text.decode("latin-1")
