# SPDX-License-Identifier: GPL-3.0-or-later
"""Layout-invariant normalisation of x86-32 code.

WHY. The retail `default.xbe` and the OXM 46 demo `tsdemo_cd.xbe` were built from
substantially the same engine but linked at different addresses (`.text` at VA
0x12000 and 0x11000 respectively). Comparing their bytes directly is almost
useless -- only 15.1% of 32-byte windows match -- not because the code differs but
because every `call rel32` displacement and every absolute pointer immediate is a
function of where the linker happened to put things. Blank those out and the same
function compiled into both builds becomes the same bytes, which is what makes the
demo usable as external evidence about function boundaries.

WHY DECODE RATHER THAN SCAN. The original feasibility probe found address-shaped
4-byte values by scanning the raw bytes and reached 39.0%. That both over-blanks
(an in-range value is blanked wherever it appears, including inside a ModRM byte
plus the start of the next instruction) and under-blanks (a `rel32` displacement
is not an image address, so a value-based scan misses it unless it is specifically
looked for). Decoding gives the true byte position and width of each operand, so
exactly the layout-dependent bytes are blanked and nothing else.

WHAT IS BLANKED, and nothing else:

1. The `rel8`/`rel16`/`rel32` immediate of any instruction whose target is
   computed from the instruction pointer -- `call`, `jmp`, `jcc`, `loop*`,
   `jecxz`. Capstone marks these with `CS_GRP_BRANCH_RELATIVE`. These differ
   between builds whenever *either* endpoint moved.
2. Any immediate or memory displacement whose encoded value `v` satisfies
   `image_lo <= v < image_hi`, i.e. a pointer to code or data that the linker
   placed differently.

Small immediates outside the image range -- loop counts, structure sizes, flag
masks, stack adjustments -- are deliberately left alone. They are the signal that
distinguishes one function from another; blanking them would make unrelated
functions look alike and destroy the whole point of the comparison.

OPERAND POSITIONS come from capstone's `insn.encoding`
(`imm_offset`/`imm_size`, `disp_offset`/`disp_size`), verified present on the
installed capstone. Note `capstone.__version__` reports 5.0.7 while the installed
*package* is 5.0.9: that attribute is the bundled core library version
(`CS_VERSION_*`), not the distribution version, which `importlib.metadata.version`
confirms as 5.0.9. The pin is satisfied. The value is read back out of the bytes at
that position rather than searched for, because the same four bytes can occur
elsewhere in an encoding.

KNOWN LIMIT. Capstone reports one immediate position per instruction, so for the
rare two-immediate forms (`enter imm16, imm8`) only the last is considered. Those
immediates are frame sizes, never addresses, so nothing layout-dependent escapes.

DECODING IS LINEAR AND RESYNCHRONISING. `.text` contains jump tables, alignment
padding and read-only data interleaved with code, so a linear sweep will hit bytes
that are not instructions. Those offsets are recorded and the sweep advances one
byte and tries again, rather than aborting; bytes no instruction covers keep their
original value in `masked`.

MEASURED, retail `default.xbe` against the OXM 46 demo `tsdemo_cd.xbe`:

    retail  .text 3,937,148 bytes  1,154,998 instructions  132 undecodable (0.003%)
    demo    .text 3,530,908 bytes  1,031,400 instructions  211 undecodable (0.006%)

    demo 32-byte windows, 16-byte stride, also present in retail
      raw bytes                                  15.14%   (baseline said 15.1%)
      this module                                36.93%
      the original crude byte scan, reproduced   38.14%   (baseline said 39.0%)

So this does NOT beat the crude scan on that headline number, and the gap is real,
not a measurement artifact -- the raw figure reproduces the recorded baseline
exactly, so the comparison is sound. What the headline number does not say is that
it rewards blanking *volume*, because every blanked byte makes two windows likelier
to be equal whether or not the byte meant anything:

  * Span-accurate, the crude scan blanks 29.3% of the demo's `.text`; this module
    blanks 20.8%.
  * Of 214,093 in-image dwords at *any* byte position in the demo, 103,398 are real
    decoded operands. The other 85,723 are coincidences -- address-shaped byte
    sequences straddling the opcode and operand of ordinary `mov`/`push`/`movss`
    instructions, only 25,262 of them even 4-byte aligned. The crude scan blanks all
    of them; this module blanks none.
  * Control: blanking a same-sized dword range sitting *above* the image, which
    therefore contains no addresses at all, still blanks 9.95% of bytes by
    coincidence and still lifts the match from 15.14% to 20.05%. Roughly half a
    point of "match" per point of meaningless blanking.
  * Per point of blanking, searching retail at every offset: this module converts
    1.36 points of match, the crude scan 1.04, the control 0.49.
  * On windows matching retail at exactly ONE position -- the ones actually usable as
    boundary evidence, since an ambiguous match says nothing -- the two are level:
    38.99% here against 39.49% crude, from 29% less blanking.

Separately, 16-byte stride on the haystack costs about nine points on its own: the
same masked buffers score 46.13% when retail is searched at every offset instead of
only 16-aligned ones, against 17.87% raw. Alignment, not normalisation, is the next
thing worth attacking.

The honest summary: precision is better and bytes-blanked is lower for the same
yield of unambiguous matches, but the stated metric is not improved. Pushing it past
the crude scan means masking *more*, and the only principled way to do that is to
stop decoding data as if it were code -- segment `.text` into code and data first,
then value-scan the data regions. Blanking a run of three or more consecutive
4-byte-aligned in-image dwords (a jump table or vtable) was tried and is worth only
+0.43 points, so that segmentation has to be better than a run-length heuristic.
"""

from __future__ import annotations

from dataclasses import dataclass

import capstone

#: No x86 instruction is longer than this, including all prefixes.
MAX_INSN_BYTES = 15

#: Decoded a window at a time so a resynchronisation after an undecodable byte
#: re-copies a bounded slice rather than the whole section. Must comfortably
#: exceed MAX_INSN_BYTES, so that a chunk always yields at least one accepted
#: instruction when the byte at its start is decodable.
CHUNK_BYTES = 4096

DEFAULT_SECTION_NAME = ".text"


@dataclass(frozen=True)
class Insn:
    """One decoded instruction, with its layout-dependent operands blanked."""

    offset: int
    """Byte offset within the section buffer."""

    length: int
    """Instruction length in bytes."""

    va: int
    """Virtual address of the instruction."""

    masked: bytes
    """Exactly `length` bytes, layout-dependent operands zeroed."""

    mnemonic: str
    """Capstone mnemonic, kept for reporting and debugging."""


@dataclass
class NormalisedText:
    """A whole code section, decoded and normalised."""

    section_name: str
    base_va: int
    data: bytes
    """The original section bytes, unmodified."""

    masked: bytes
    """Same length as `data`, with operands blanked in place."""

    insns: list[Insn]
    """Decoded instructions, ascending by offset."""

    undecodable: list[int]
    """Offsets where decoding failed, ascending."""


def _decoder() -> capstone.Cs:
    """A detail-enabled 32-bit x86 decoder. Built per call: no shared state."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def _read_le(raw: bytes, offset: int, size: int) -> int | None:
    """Unsigned little-endian field, or None when it is not fully inside `raw`.

    Read unsigned on purpose: the image range sits far below 2**31 in every XBE we
    have, so a negative displacement such as `[ebp-8]` can never be mistaken for an
    address, and an unsigned read needs no width-dependent sign handling.
    """
    if offset < 0 or size <= 0 or offset + size > len(raw):
        return None
    return int.from_bytes(raw[offset : offset + size], "little")


def _blank_spans(insn: capstone.CsInsn, image_lo: int, image_hi: int) -> list[tuple[int, int]]:
    """(offset, size) spans inside this instruction that depend on link layout."""
    encoding = insn.encoding
    raw = bytes(insn.bytes)
    spans: list[tuple[int, int]] = []

    if encoding.imm_size:
        value = _read_le(raw, encoding.imm_offset, encoding.imm_size)
        if value is not None:
            relative = capstone.CS_GRP_BRANCH_RELATIVE in insn.groups
            if relative or image_lo <= value < image_hi:
                spans.append((encoding.imm_offset, encoding.imm_size))

    if encoding.disp_size:
        value = _read_le(raw, encoding.disp_offset, encoding.disp_size)
        if value is not None and image_lo <= value < image_hi:
            spans.append((encoding.disp_offset, encoding.disp_size))

    return spans


def _mask_insn(insn: capstone.CsInsn, image_lo: int, image_hi: int) -> bytes:
    """This instruction's bytes with its layout-dependent spans zeroed."""
    raw = bytes(insn.bytes)
    spans = _blank_spans(insn, image_lo, image_hi)
    if not spans:
        return raw
    out = bytearray(raw)
    for offset, size in spans:
        out[offset : offset + size] = bytes(size)
    return bytes(out)


def normalise_text(
    data: bytes,
    base_va: int,
    image_lo: int,
    image_hi: int,
    *,
    section_name: str = DEFAULT_SECTION_NAME,
) -> NormalisedText:
    """Decode `data` as x86-32 code and blank every link-layout-dependent operand.

    `base_va` is the virtual address of `data[0]`. `image_lo`/`image_hi` bound the
    loaded image, half-open; an immediate or displacement landing inside them is
    treated as an absolute pointer and blanked.

    Pure and deterministic: the same arguments always give the same result.
    """
    if image_hi < image_lo:
        raise ValueError(f"image range is inverted: lo={image_lo:#x} hi={image_hi:#x}")

    md = _decoder()
    masked = bytearray(data)
    insns: list[Insn] = []
    undecodable: list[int] = []

    total = len(data)
    offset = 0
    while offset < total:
        end = min(offset + CHUNK_BYTES, total)
        # An instruction ending in the last MAX_INSN_BYTES of a non-final chunk may
        # have been cut short by the chunk boundary rather than by the data, so leave
        # it for the next chunk instead of trusting a possibly truncated decode.
        limit = end if end == total else end - MAX_INSN_BYTES

        progress = offset
        for insn in md.disasm(data[offset:end], base_va + offset):
            insn_offset = insn.address - base_va
            if insn_offset + insn.size > limit:
                break
            blanked = _mask_insn(insn, image_lo, image_hi)
            masked[insn_offset : insn_offset + insn.size] = blanked
            insns.append(
                Insn(
                    offset=insn_offset,
                    length=insn.size,
                    va=insn.address,
                    masked=blanked,
                    mnemonic=insn.mnemonic,
                )
            )
            progress = insn_offset + insn.size

        if progress == offset:
            undecodable.append(offset)
            offset += 1
        else:
            offset = progress

    return NormalisedText(
        section_name=section_name,
        base_va=base_va,
        data=data,
        masked=bytes(masked),
        insns=insns,
        undecodable=undecodable,
    )
