import argparse
import glob
import re

from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--cols", type=int, default=5)
ap.add_argument("--first", type=int, default=0)
ap.add_argument("--n", type=int, default=20)
a = ap.parse_args()
fs = sorted(
    glob.glob(a.dir + "/shot-*.png"), key=lambda f: int(re.search(r"shot-(\d+)-", f).group(1))
)[a.first : a.first + a.n]
W, H = 256, 192
rows = (len(fs) + a.cols - 1) // a.cols
im = Image.new("RGB", (W * a.cols, H * rows))
for i, f in enumerate(fs):
    im.paste(
        Image.open(f).crop((320, 120, 960, 600)).resize((W, H)),
        ((i % a.cols) * W, (i // a.cols) * H),
    )
im.save(f"{a.dir}/sheet-{a.first}.png")
print([re.search(r"shot-(\d+)-", f).group(1) for f in fs])
