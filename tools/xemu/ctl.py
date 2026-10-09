"""Bounded xemu launch planning. Emulator evidence is not NV2A silicon evidence."""

import argparse
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

try:
    from . import lifecycle
except ImportError:
    import lifecycle

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "tmp" / "xemu"
APPIMAGE_URL = (
    "https://github.com/xemu-project/xemu/releases/download/v0.8.136/xemu-0.8.136-x86_64.AppImage"
)
DEFAULT_DISPLAY = ":99"
DISPLAY_RANGE = range(99, 89, -1)


def display_occupied(display: str) -> bool:
    number = display.lstrip(":")
    return Path(f"/tmp/.X{number}-lock").exists() or Path(f"/tmp/.X11-unix/X{number}").exists()


def choose_display(requested: str | None) -> str:
    """Explicit --display or $XEMU_DISPLAY (refused if busy), else the first free of :99..:90."""
    requested = requested or os.environ.get("XEMU_DISPLAY")
    if requested:
        if display_occupied(requested):
            sys.exit(f"display {requested} already occupied; no existing display is stopped")
        return requested
    for number in DISPLAY_RANGE:
        if not display_occupied(f":{number}"):
            return f":{number}"
    sys.exit("no free display in :99..:90; pass --display")


def shot_display(requested: str | None) -> str:
    """Display of the running launch: --display, $XEMU_DISPLAY, the recorded one, else :99."""
    recorded = WORK / "display"
    if requested or os.environ.get("XEMU_DISPLAY"):
        return requested or os.environ["XEMU_DISPLAY"]
    return recorded.read_text().strip() if recorded.is_file() else DEFAULT_DISPLAY


def rooted(value: str) -> Path:
    path = Path(value).expanduser()
    return (ROOT / path).resolve() if not path.is_absolute() else path.resolve()


def environment(display: str = DEFAULT_DISPLAY) -> dict[str, str]:
    env = dict(os.environ)
    env.update(
        DISPLAY=display,
        LIBGL_ALWAYS_SOFTWARE="1",
        SDL_AUDIODRIVER="dummy",
        XDG_RUNTIME_DIR=str(WORK / "rt"),
    )
    return env


def install(_args: argparse.Namespace) -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    image = WORK / "xemu.AppImage"
    if not image.exists():
        subprocess.run(["curl", "-sL", "-o", str(image), APPIMAGE_URL], check=True, timeout=600)
        image.chmod(0o755)
    if not (WORK / "squashfs-root").exists():
        subprocess.run(
            [str(image), "--appimage-extract"],
            cwd=WORK,
            check=True,
            stdout=subprocess.DEVNULL,
            timeout=120,
        )


def readiness(args: argparse.Namespace) -> tuple[dict[str, Path], list[str]]:
    files = rooted(args.files_dir)
    paths = {
        "MCPX": files / "mcpx" / "mcpx_1.0.bin",
        "BIOS": files / "bios" / "Complex_4627.bin",
        "HDD": files / "hdd" / "xbox_hdd.qcow2",
    }
    if args.dvd:
        paths["DVD"] = rooted(args.dvd)
    missing = [name for name, path in paths.items() if not path.is_file()]
    executable = WORK / "squashfs-root" / "AppRun"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        missing.append("xemu executable (run install explicitly)")
    missing.extend(command for command in ("Xvfb",) if shutil.which(command) is None)
    return paths, missing


def status(args: argparse.Namespace) -> None:
    _, missing = readiness(args)
    owner = None
    if (WORK / "owner.json").exists():
        try:
            owner = lifecycle.request(WORK, "status")
            owner.pop("nonce", None)
        except lifecycle.OwnershipError as error:
            owner = {"owned": False, "error": str(error)}
    print(
        json.dumps(
            {
                "ready": not missing
                and not (WORK / "pids").exists()
                and not (WORK / "owner.json").exists(),
                "missing": missing,
                "actual_version": None,
                "selected_release": "v0.8.136",
                "owner": owner,
                "unverified_pid_record": (WORK / "pids").exists(),
            }
        )
    )


# Explicit port1 keyboard map (SDL scancodes) equal to xemu's own defaults (config_spec.yml), so no
# two entries share a key (the T1094 map put black on e, also lstick_up). Arrows=dpad, Return=start,
# a/b/x/y=face, Backspace=back, 1/2=white/black, 3/4=stick clicks, e/s/f/d=left stick up/left/right/
# down, w=left trigger, i/j/l/k=right stick up/left/right/down, o=right trigger. Sticks and triggers
# are full deflection only (digital keys), see docs/t1087-story-input-xemu.md.
KEYBOARD_PAD = """[input.bindings]
port1_driver = 'usb-xbox-gamepad'
port1 = 'keyboard'
[input.keyboard_controller_scancode_map]
a = 4
b = 5
x = 27
y = 28
dpad_left = 80
dpad_up = 82
dpad_right = 79
dpad_down = 81
back = 42
start = 40
white = 30
black = 31
lstick_btn = 32
rstick_btn = 33
lstick_up = 8
lstick_left = 22
lstick_right = 9
lstick_down = 7
ltrigger = 26
rstick_up = 12
rstick_left = 13
rstick_right = 15
rstick_down = 14
rtrigger = 18
"""


def start(args: argparse.Namespace) -> None:
    if args.lifetime <= 0:
        sys.exit("lifetime must be positive")
    if (WORK / "pids").exists() or (WORK / "owner.json").exists():
        sys.exit("ownership cannot be established from the legacy PID record")
    paths, missing = readiness(args)
    try:
        lifecycle.supported()
    except lifecycle.OwnershipError as error:
        sys.exit(str(error))
    if missing:
        sys.exit("not ready: " + ", ".join(missing))
    hdd = WORK / "hdd.qcow2"
    entries = {"bootrom_path": paths["MCPX"], "flashrom_path": paths["BIOS"], "hdd_path": hdd}
    if args.dvd:
        entries["dvd_path"] = paths["DVD"]
    config = "[general]\nshow_welcome = false\n[sys]\nmem_limit = '64'\n[sys.files]\n"
    config += "".join(
        f"{key} = {json.dumps(str(path), ensure_ascii=False)}\n" for key, path in entries.items()
    )
    if args.keyboard_pad:
        config += KEYBOARD_PAD
    if getattr(args, "soft_fpu", False):
        config += "[perf]\nhard_fpu = false\n"
    config += "[display]\nrenderer = 'OPENGL'\n[audio]\nuse_dsp = false\n"
    display = choose_display(args.display)
    command = [str(WORK / "squashfs-root" / "AppRun"), "-config_path", str(WORK / "xemu.toml")]
    if args.dry_run:
        print(
            json.dumps(
                {
                    "config": config,
                    "xemu_argv": command,
                    "supervisor_lifetime": args.lifetime,
                    "display": display,
                }
            )
        )
        return
    if WORK.is_symlink() or (
        WORK.exists() and (WORK.stat().st_uid != os.getuid() or WORK.stat().st_mode & 0o022)
    ):
        sys.exit("unsafe ownership directory")
    WORK.mkdir(parents=True, exist_ok=True, mode=0o700)
    commands = [["Xvfb", display, "-screen", "0", "1024x768x24"], command]
    # Sole CLI reaper: retain any exited direct child's PID until our own wait.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    supervisor = subprocess.Popen(
        [
            sys.executable,
            str(Path(lifecycle.__file__).resolve()),
            "--work",
            str(WORK),
            "--lifetime",
            str(args.lifetime),
            "--commands",
            json.dumps(commands),
            "--config",
            config,
            "--hdd-source",
            str(paths["HDD"]),
        ],
        env=environment(display),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        start_new_session=True,
    )
    try:
        descriptor = os.pidfd_open(supervisor.pid)
    except OSError:
        try:
            # Allow its two-child TERM/KILL cleanup to finish before escalation.
            lifecycle._reap_unregistered_child(supervisor, grace=6)
        finally:
            supervisor.stdout.close()
        raise
    try:
        if not select.select([supervisor.stdout], [], [], 8)[0]:
            signal_error = "supervisor startup acknowledgement timeout"
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
            supervisor.wait(timeout=5)
            sys.exit(signal_error)
        acknowledgement = json.loads(supervisor.stdout.readline())
        if not acknowledgement.get("started"):
            supervisor.wait(timeout=5)
            sys.exit("ownership startup failed: " + acknowledgement.get("error", "unknown"))
        (WORK / "display").write_text(display + "\n")
        print(json.dumps({**acknowledgement, "display": display}))
    finally:
        supervisor.stdout.close()
        os.close(descriptor)


def shot(args: argparse.Namespace) -> None:
    env = environment(shot_display(args.display))
    ids = subprocess.run(
        ["xdotool", "search", "--name", r"^xemu \|"],
        env=env,
        capture_output=True,
        text=True,
        timeout=20,
    ).stdout.split()
    if not ids:
        sys.exit("no xemu window (is it started?)")
    subprocess.run(
        ["import", "-window", ids[0], str(rooted(args.out))], env=env, check=True, timeout=30
    )
    print(f"wrote {rooted(args.out)}")


def key(args: argparse.Namespace) -> None:
    """Tap keys into the xemu window (T1213)."""
    display = shot_display(args.display)
    env = environment(display)
    ids = subprocess.run(
        ["xdotool", "search", "--name", r"^xemu \|"],
        env=env,
        capture_output=True,
        text=True,
        timeout=20,
    ).stdout.split()
    if not ids:
        sys.exit("no xemu window (is it started?)")
    subprocess.run(["xdotool", "windowfocus", ids[0]], env=env, check=True, timeout=20)
    if args.chord:
        # All keys down together (stick plus trigger plus button), shot taken before release.
        for name in args.keys:
            subprocess.run(["xdotool", "keydown", name], env=env, check=True, timeout=20)
        time.sleep(args.hold)
        if args.shot:
            shot(argparse.Namespace(out=args.shot, display=display))
        for name in args.keys:
            subprocess.run(["xdotool", "keyup", name], env=env, check=True, timeout=20)
        time.sleep(args.gap)
        return
    for name in args.keys:
        subprocess.run(["xdotool", "keydown", name], env=env, check=True, timeout=20)
        time.sleep(args.hold)
        subprocess.run(["xdotool", "keyup", name], env=env, check=True, timeout=20)
        time.sleep(args.gap)


def stop(_args: argparse.Namespace) -> None:
    try:
        result = lifecycle.request(WORK, "stop")
    except lifecycle.OwnershipError as error:
        sys.exit("stop refused: " + str(error))
    if not result.get("stopped"):
        sys.exit("stop refused: supervisor did not acknowledge cleanup")
    (WORK / "display").unlink(missing_ok=True)
    print("stopped owned direct children")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    one = sub.add_parser("install", help="explicitly download/extract xemu into tmp/xemu")
    one.set_defaults(func=install)
    for name, function in (("start", start), ("status", status)):
        child = sub.add_parser(name)
        child.add_argument("--files-dir", default="tmp/Xbox-Emulator-Files")
        child.add_argument("--dvd", default=None)
        child.set_defaults(func=function)
        if name == "start":
            child.add_argument("--lifetime", type=int, default=600)
            child.add_argument("--dry-run", action="store_true")
            child.add_argument("--display", default=None, help="X display (default: first free)")
            child.add_argument(
                "--keyboard-pad",
                action="store_true",
                help="write explicit [input.bindings] and SDL scancode map (T1094)",
            )
            child.add_argument(
                "--soft-fpu",
                action="store_true",
                help="write perf.hard_fpu = false (QEMU softfloat x87, T1510)",
            )
    three = sub.add_parser("shot")
    three.add_argument("out")
    three.add_argument("--display", default=None, help="X display (default: recorded launch)")
    three.set_defaults(func=shot)
    five = sub.add_parser("key", help="tap keys into the xemu window (xdotool)")
    five.add_argument("keys", nargs="+")
    five.add_argument("--display", default=None)
    five.add_argument("--hold", type=float, default=0.6)
    five.add_argument("--gap", type=float, default=1.2)
    five.add_argument("--chord", action="store_true", help="hold all keys together (T1087)")
    five.add_argument("--shot", default=None, help="with --chord: screenshot before release")
    five.set_defaults(func=key)
    four = sub.add_parser("stop", help="refuse stopping without verified ownership")
    four.set_defaults(func=stop)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
