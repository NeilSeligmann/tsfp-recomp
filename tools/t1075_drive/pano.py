"""T1240: contact sheet of named PNGs (look at the pictures with the Read tool)."""

import argparse

from PIL import Image


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--cols", type=int, default=2)
    a = ap.parse_args()
    width, height = 480, 360
    rows = (len(a.files) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (width * a.cols, height * rows))
    for i, name in enumerate(a.files):
        sheet.paste(
            Image.open(name).resize((width, height)), ((i % a.cols) * width, (i // a.cols) * height)
        )
    sheet.save(a.out)


if __name__ == "__main__":
    main()
