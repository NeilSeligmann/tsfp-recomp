"""List the interior-copy dwords the caller audit still refuses for hypothetical roots (T1252 R17).

Audits arbitrary guest VAs as if they were registered (like `audit_measure`) and records, for every
body the climb asks about, the image dwords equal to an interior address of that body that no rule
(instruction map, aligned data pointer rule, mid instruction rule) explains away, with the XBE
section each one lives in. Only the audit runs, nothing is proven.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.harness.image import build_guest_image
from tools.xbe import parse_xbe

from . import audit as audit_module
from .audit_measure import DEFAULT_ROOTS
from .manifest import ManifestEntry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--gen-dir", type=Path, default=Path("generated/lifted/gen"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument("--roots", default=DEFAULT_ROOTS, help="hex VA:stack_args,...")
    parser.add_argument("--out", type=Path, help="write the report to this file")
    args = parser.parse_args()
    entries = [
        ManifestEntry(
            int(item.split(":")[0], 16),
            f"root_{item.split(':')[0]}",
            "cdecl",
            int(item.split(":")[1]),
            "eax",
            ("ecx", "edx"),
            "measure",
        )
        for item in args.roots.split(",")
    ]
    from .bridge import BridgeExemption
    from .straddle_corpus import data_ranges

    sections = [
        (s.name, s.virtual_addr, s.virtual_addr + s.virtual_size)
        for s in parse_xbe(args.xbe.read_bytes()).sections
    ]

    def section_of(address: int) -> str:
        return next((name for name, low, high in sections if low <= address < high), "?")

    image = build_guest_image(args.xbe)
    ranges = [(low, high) for _, low, high in data_ranges(args.xbe)]
    bridge = BridgeExemption.open(args.xbe, args.gen_dir, Path("src/game"))
    strays: dict[int, list[tuple[int, int, str]]] = {}
    original = audit_module.CallerClimb._interior_copy

    def recording(self: audit_module.CallerClimb, frame: int, end: int) -> str:
        top = min(end, self._image.base + len(self._image.data))
        found = []
        if frame + 1 < top:
            for at in audit_module._dwords_in_range(self._image.data, frame + 1, top - 1):
                address = self._image.base + at
                value = int.from_bytes(self._image.data[at : at + 4], "little")
                if (
                    not self._inside_reachable_code(address)
                    and not self._unaligned_data_straddle(address)
                    and not self._mid_instruction(frame, value)
                ):
                    found.append((address, value, section_of(address)))
        strays[frame] = found
        return original(self, frame, end)

    audit_module.CallerClimb._interior_copy = recording  # type: ignore[method-assign]
    audit_module.audit_all(
        entries,
        args.gen_dir,
        image,
        data_ranges=ranges,
        bridge=bridge,
        functions=args.functions,
    )
    lines = [
        f"0x{frame:x} " + str([(f"0x{a:x}", f"0x{v:x}", s) for a, v, s in found])
        for frame, found in sorted(strays.items())
        if found
    ]
    text = "\n".join(lines) + "\n"
    print(text, end="")
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
