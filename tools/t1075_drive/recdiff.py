"""T1240: word diff of the player record (*0x7B0C48, 0x1584 bytes) across guest dump labels."""

import argparse
import re
import struct

SIZE = 0x1584


def load(path: str) -> bytes:
    base = None
    data = bytearray(SIZE)
    for line in open(path):
        if line.startswith("range "):
            base = int(line.split()[1], 16)
        m = re.match(r"([0-9A-F]{8}): ((?:[0-9a-f]{2} ?)+)", line)
        if m and base is not None:
            off = int(m[1], 16) - base
            raw = bytes.fromhex(m[2].replace(" ", ""))
            if 0 <= off < SIZE:
                data[off : off + len(raw)] = raw[: SIZE - off]
    return bytes(data)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("labels", nargs="+")
    ap.add_argument("--maxrows", type=int, default=400)
    a = ap.parse_args()
    d = [load(f"{a.dir}/guestdump.{n}") for n in a.labels]
    print("labels", a.labels)
    rows = 0
    for off in range(0, SIZE, 4):
        w = [struct.unpack_from("<I", x, off)[0] for x in d]
        if len(set(w)) > 1 and rows < a.maxrows:
            f = [struct.unpack_from("<f", x, off)[0] for x in d]
            print(hex(off), " ".join(f"{x:08x}" for x in w), " ".join(f"{x:.4g}" for x in f))
            rows += 1


if __name__ == "__main__":
    main()
