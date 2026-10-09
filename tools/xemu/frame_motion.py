#!/usr/bin/env python3
# ruff: noqa: E501
"""T1087 frame motion metrics between two screenshots: RMSE, mean shift (dx, dy) and divergence.

Block phase correlation on a grid over the grayscale frame.  Sign convention: dx > 0 means the scene content moved
right, dy > 0 down (so a camera pan right gives dx < 0, a pitch up gives dy > 0).  Divergence > 0 means the content
expands away from the centre (moving forward), < 0 contracts (moving back).  Not a pose log: a coarse, sign-level
indicator, used with a no-input control pair.

    python -m tools.xemu.frame_motion before.png after.png [--crop X Y W H]
"""

from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image

BLOCK = 64


def load(path: str, crop: tuple[int, int, int, int] | None) -> np.ndarray:
    image = Image.open(path).convert("L")
    if crop:
        x, y, w, h = crop
        image = image.crop((x, y, x + w, y + h))
    return np.asarray(image, dtype=np.float64)


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a - b) ** 2)) / 255.0)


def block_shift(a: np.ndarray, b: np.ndarray) -> tuple[float, float, float]:
    """Shift of content from a to b by phase correlation, and the peak sharpness (0..1)."""
    window = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa = np.fft.fft2((a - a.mean()) * window)
    fb = np.fft.fft2((b - b.mean()) * window)
    cross = fb * np.conj(fa)
    cross /= np.abs(cross) + 1e-9
    corr = np.fft.ifft2(cross).real
    peak = np.unravel_index(int(np.argmax(corr)), corr.shape)
    dy, dx = (p if p <= s // 2 else p - s for p, s in zip(peak, corr.shape, strict=True))
    return float(dx), float(dy), float(corr[peak])


def motion(a: np.ndarray, b: np.ndarray) -> dict[str, float]:
    height, width = a.shape
    centre_x, centre_y = width / 2, height / 2
    samples = []
    for top in range(0, height - BLOCK + 1, BLOCK):
        for left in range(0, width - BLOCK + 1, BLOCK):
            if a[top : top + BLOCK, left : left + BLOCK].std() < 4:
                continue
            dx, dy, sharp = block_shift(
                a[top : top + BLOCK, left : left + BLOCK], b[top : top + BLOCK, left : left + BLOCK]
            )
            if sharp > 0.08:
                samples.append((left + BLOCK / 2 - centre_x, top + BLOCK / 2 - centre_y, dx, dy))
    if not samples:
        return {"rmse": rmse(a, b), "dx": 0.0, "dy": 0.0, "div": 0.0, "blocks": 0}
    data = np.array(samples)
    radius = np.maximum(np.hypot(data[:, 0], data[:, 1]), 1.0)
    divergence = float(np.mean((data[:, 2] * data[:, 0] + data[:, 3] * data[:, 1]) / radius))
    return {
        "rmse": rmse(a, b),
        "dx": float(np.median(data[:, 2])),
        "dy": float(np.median(data[:, 3])),
        "div": divergence,
        "blocks": len(samples),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "W", "H"), default=None)
    args = parser.parse_args()
    crop = tuple(args.crop) if args.crop else None
    result = motion(load(args.before, crop), load(args.after, crop))
    print(
        " ".join(
            f"{key}={value:.3f}" if isinstance(value, float) else f"{key}={value}"
            for key, value in result.items()
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
