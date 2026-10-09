# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate T1732 retail movie request witnesses without extracting movie data."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from tools.name_candidates import Image

REQUESTS = {
    0x70601: (0x70410, "language-selected EA logo", 0),
    0x7060F: (0x70410, "frd", 0),
    0x7062C: (0x70410, "eag_e", 0),
    0x70637: (0x70410, "frd", 0),
    0x1CDF52: (0x1CDE30, "attract1", 1),
    0x2D3CBC: (0x2D3BF0, "attract", 1),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    args = parser.parse_args()
    image = Image(args.xbe)
    digest = hashlib.sha256(image.data).hexdigest()
    assert digest == "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
    for site, (caller, movie, skip) in REQUESTS.items():
        expected = b"\xe8" + struct.pack("<i", 0x30280 - site - 5)
        assert image.read(site, 5) == expected, hex(site)
        print(f"{caller:#010x} call {site:#010x}: {movie}, skippable={skip}")
    for site in (0x2D3CAD, 0x2D3CC7):
        assert image.read(site, 5) == b"\xe8" + struct.pack("<i", 0x30210 - site - 5)
    for address, expected in {
        0x479628: b"frd\0",
        0x47CFA8: b"eag_e\0",
        0x47CFB0: b"eag_s\0",
        0x47CFB8: b"eag_i\0",
        0x47CFC0: b"eag_f\0",
        0x49DCF0: b"attract1\0",
        0x49F630: b"attract\0",
    }.items():
        assert image.read(address, len(expected)) == expected, hex(address)
    assert struct.unpack("<4I", image.read(0x70650, 16)) == (
        0x705E3,
        0x705FB,
        0x705EB,
        0x705F3,
    )
    assert struct.unpack("<7I", image.read(0x1CE158, 28))[4] == 0x1CDF3F
    assert image.read(0x28D40, 9) == bytes.fromhex("8b442404e967ffffff")
    print(f"PASS: six direct CALL witnesses, two setup CALLs, names, tables, tail; XBE {digest}")


if __name__ == "__main__":
    main()
