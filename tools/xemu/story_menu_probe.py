"""T1094: wait for the Story player-count menu in xemu, then run a key sequence with a
no-key control and save a screenshot after each step. Needs ctl start --keyboard-pad."""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


def shot(path: Path) -> None:
    subprocess.run(
        [sys.executable, "-m", "tools.xemu.ctl", "shot", str(path)],
        check=True,
        capture_output=True,
        timeout=60,
    )


def is_menu(path: Path) -> bool:
    """True when the frame matches the stored menu reference (normalised RMSE < 0.12)."""
    ref = Path("tmp/xemu/story_menu_ref.png")  # private disc frame, gitignored
    run = subprocess.run(
        ["compare", "-metric", "RMSE", str(path), str(ref), "null:"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    text = run.stderr.split("(")[-1].split(")")[0]
    try:
        return float(text) < 0.12
    except ValueError:
        return False


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="tmp/xemu/menu")
    parser.add_argument(
        "--keys",
        default="none,Down,Down,Up,Return",
        help="comma list, 'none' is the no-key control",
    )
    parser.add_argument("--wait", type=int, default=300)
    parser.add_argument("--hold", type=float, default=0.3)
    parser.add_argument("--display", default=":99")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DISPLAY=args.display)
    deadline = time.time() + args.wait
    probe = out / "probe.png"
    while time.time() < deadline:
        shot(probe)
        if is_menu(probe):
            break
        time.sleep(2)
    else:
        sys.exit("menu not reached")
    for index, key in enumerate(args.keys.split(",")):
        if key != "none":
            subprocess.run(["xdotool", "keydown", key], env=env, check=True, timeout=10)
            time.sleep(args.hold)
            subprocess.run(["xdotool", "keyup", key], env=env, check=True, timeout=10)
        time.sleep(1.5)
        shot(out / f"{index}_{key}.png")
        print(index, key, "menu" if is_menu(out / f"{index}_{key}.png") else "other")


if __name__ == "__main__":
    main()
