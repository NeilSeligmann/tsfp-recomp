# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Resolve the front end's localized label ids to text from the user's own disc (T845), read only.

The front end resolves nonzero valid string ids through `0x3D7B0(id)`. For an id below `0xD87` the text is `table1[id]`, otherwise `table2[id - 0xD87]`
(the two loaders `0x3DB80` and `0x3DD20`: the file `data/str_ts_<p><x><language letter>.bs`, resp. `..._story.bs`, a table of
dword offsets from the file start followed by NUL terminated text). Both files are entries of the level pak `pak/arcade/l_103.pak`,
found by the pak's key `zlib.crc32(name)`. The retail disc image is the user's, nothing is written.

    python3 -m tools.frontend_labels --iso discs/tsfp-xbox.iso 0x366 0x404 0x40A
"""

import argparse
import struct
import zlib
from pathlib import Path

from tools.pak.reader import parse_pak
from tools.xdvdfs import XisoReader

PAK = "pak/arcade/l_103.pak"
FIRST_TABLE_ENTRIES = 0xD87  # ids below this index the first file
SECOND_TABLE_ENTRIES = 0x1035
ENGLISH = {"first": "data/str_ts_pxe.bs", "second": "data/str_ts_pxe_story.bs"}


class Labels:
    """The English label tables of the front end."""

    def __init__(self, first: bytes, second: bytes) -> None:
        self.first = first
        self.second = second

    @staticmethod
    def _text(data: bytes, entries: int, index: int) -> str:
        if not 0 <= index < entries:
            raise KeyError(index)
        (offset,) = struct.unpack_from("<I", data, 4 * index)
        return data[offset : data.index(b"\0", offset)].decode("latin-1")

    def text(self, label: int) -> str:
        """Decode a bank entry. Retail id 0 instead returns a special pointer (T1760)."""
        if label < FIRST_TABLE_ENTRIES:
            return self._text(self.first, FIRST_TABLE_ENTRIES, label)
        return self._text(self.second, SECOND_TABLE_ENTRIES, label - FIRST_TABLE_ENTRIES)


def load(iso: Path) -> Labels:
    """Read both English label files out of the disc image."""
    with iso.open("rb") as stream:
        data = XisoReader(stream).read_file(PAK)
    pak = parse_pak(data)
    by_key = {entry.key: entry for entry in pak.entries}
    files = {}
    for slot, name in ENGLISH.items():
        entry = by_key[zlib.crc32(name.encode())]
        files[slot] = pak.entry_data(data, entry)
    return Labels(files["first"], files["second"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--iso", type=Path, default=Path("discs/tsfp-xbox.iso"))
    parser.add_argument("ids", nargs="+", help="label ids, for example 0x366")
    args = parser.parse_args()
    labels = load(args.iso)
    for text in args.ids:
        label = int(text, 0)
        print(f"{label:#06x} {labels.text(label)}")


if __name__ == "__main__":
    main()
