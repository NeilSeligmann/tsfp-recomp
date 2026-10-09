"""Linux owned direct-child lifecycle. No record-based PID or group signalling."""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import errno
import fcntl
import json
import math
import os
import secrets
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import time
from pathlib import Path


class OwnershipError(RuntimeError):
    """Fail-closed ownership or platform refusal."""


def boot_identity() -> str:
    """Boot identity: boot_id, else /proc/stat btime (boot_id errors in some containers, T1213)."""
    try:
        return Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        pass
    for line in Path("/proc/stat").read_text().splitlines():
        if line.startswith("btime "):
            return "btime-" + line.split()[1]
    raise OwnershipError("no boot identity available")


def identity(pid: int) -> dict[str, object]:
    proc = Path("/proc") / str(pid)
    fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    executable = (proc / "exe").stat()
    return {
        "pid": pid,
        "start": fields[19],
        "uid": proc.stat().st_uid,
        "namespace": os.readlink(proc / "ns/pid"),
        "boot": boot_identity(),
        "exe_dev": executable.st_dev,
        "exe_inode": executable.st_ino,
    }


def supported() -> None:
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise OwnershipError("Linux pidfd support is required")
    try:
        fd = os.pidfd_open(os.getpid())
        os.close(fd)
    except OSError as error:
        raise OwnershipError("kernel pidfd unavailable") from error


def request(work: Path, command: str) -> dict[str, object]:
    """Authenticate a live supervisor without signalling it or trusting PID liveness alone."""
    supported()
    try:
        record_path = work / "owner.json"
        if work.is_symlink() or work.stat().st_uid != os.getuid() or work.stat().st_mode & 0o022:
            raise OwnershipError("unsafe ownership directory")
        descriptor = os.open(record_path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor) as source:
            metadata = os.fstat(source.fileno())
            raw_record = source.read(8193)
        if len(raw_record) > 8192:
            raise OwnershipError("oversized ownership record")
        if metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
            raise OwnershipError("ownership record permissions/UID mismatch")
        record = json.loads(raw_record)
        expected = record["identity"]
        if type(expected.get("pid")) is not int or expected["pid"] <= 0:
            raise OwnershipError("malformed PID")
        if expected["uid"] != os.getuid():
            raise OwnershipError("foreign UID")
        if expected["namespace"] != os.readlink("/proc/self/ns/pid"):
            raise OwnershipError("foreign PID namespace")
        if expected["boot"] != boot_identity():
            raise OwnershipError("foreign boot")
        nonce = record["nonce"]
        if (
            not isinstance(nonce, str)
            or len(nonce) != 64
            or any(char not in "0123456789abcdef" for char in nonce)
        ):
            raise OwnershipError("malformed nonce")
        fd = os.pidfd_open(expected["pid"])
        try:
            if identity(expected["pid"]) != expected or select.select([fd], [], [], 0)[0]:
                raise OwnershipError("stale/reused supervisor identity")
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(8)
                connection.connect("\0tsfp-xemu-" + nonce)
                peer_pid, peer_uid, _ = struct.unpack(
                    "3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                )
                if peer_pid != expected["pid"] or peer_uid != expected["uid"]:
                    raise OwnershipError("socket peer ownership mismatch")
                connection.sendall(json.dumps({"nonce": nonce, "command": command}).encode())
                result = json.loads(connection.recv(4096))
                if result.get("nonce") != nonce:
                    raise OwnershipError("supervisor authentication failed")
                return result
        finally:
            os.close(fd)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise OwnershipError(f"ownership unavailable: {error}") from error


def cleanup(children: list[tuple[subprocess.Popen, int]]) -> None:
    for child, fd in children:
        if child.poll() is None:
            try:
                signal.pidfd_send_signal(fd, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 2
    for child, fd in children:
        try:
            child.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait(timeout=2)
        os.close(fd)


def _reap_unregistered_child(child: subprocess.Popen, grace: float = 2) -> None:
    """Reap our direct child before any wait/poll can release its PID.

    supervise installs SIGCHLD=SIG_DFL and has no concurrent child reaper. A
    direct child which exits is therefore a zombie until this owned Popen wait;
    its PID cannot be reused during these signals. This fallback is never used
    for an ownership record or an already-polled/reaped child.
    """
    try:
        os.kill(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        try:
            os.kill(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=2)


def validate_executable(target: int) -> None:
    metadata = os.fstat(target)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode & 0o6000:
        raise OwnershipError("privileged/nonregular child executable refused")
    if os.pread(target, 4, 0) != b"\x7fELF":
        raise OwnershipError("script/non-ELF child executable refused")
    try:
        capabilities = os.getxattr(target, "security.capability")
    except OSError as error:
        if error.errno not in (errno.ENODATA, errno.ENOTSUP):
            raise
    else:
        if capabilities:
            raise OwnershipError("capability-bearing child executable refused")


def open_executable(command: list[str]) -> int:
    if not command or not all(isinstance(value, str) for value in command):
        raise OwnershipError("nonempty executable argument list required")
    if os.execve not in os.supports_fd:
        raise OwnershipError("descriptor executable support required")
    executable = shutil.which(command[0])
    if executable is None:
        raise OwnershipError("child executable unavailable")
    target = os.open(str(Path(executable).resolve()), os.O_RDONLY | os.O_NOFOLLOW)
    try:
        validate_executable(target)
    except BaseException:
        os.close(target)
        raise
    return target


def owned_exec(
    parent: int, ready_fd: int, command: list[str], target_fd: int | None = None
) -> None:
    """Arm direct trusted ELF child death containment before same-PID exec."""
    target = open_executable(command) if target_fd is None else target_fd
    try:
        validate_executable(target)
        libc = ctypes.CDLL(None, use_errno=True)
        # no_new_privs prevents a later exec from elevating this trusted child.
        if libc.prctl(38, 1, 0, 0, 0) != 0 or libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
            raise OwnershipError("Linux parent-death protection unavailable")
        death_signal = ctypes.c_int()
        if libc.prctl(2, ctypes.byref(death_signal), 0, 0, 0) != 0:
            raise OwnershipError("parent-death protection check failed")
        if death_signal.value != signal.SIGKILL or os.getppid() != parent:
            raise OwnershipError("own parent changed before executable admission")
        os.write(ready_fd, b"ARMED")
        os.close(ready_fd)
        os.set_inheritable(target, False)
        os.execve(target, command, os.environ.copy())
    finally:
        os.close(target)


def supervise(
    work: Path,
    lifetime: float,
    commands: list[list[str]],
    config: str | None = None,
    hdd_source: Path | None = None,
) -> None:
    supported()
    if work.is_symlink():
        raise OwnershipError("symlink ownership directory")
    work.mkdir(parents=True, exist_ok=True, mode=0o700)
    if work.stat().st_uid != os.getuid() or work.stat().st_mode & 0o022:
        raise OwnershipError("unsafe ownership directory")
    children: list[tuple[subprocess.Popen, int]] = []
    stopping = False

    def terminate(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    # No inherited automatic reaping: acquisition-failure cleanup retains the
    # direct child PID until its sole owned wait. This supervisor is single-threaded.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    lock_fd = os.open(work / "lifecycle.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, "a") as lock, contextlib.ExitStack() as binaries:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        record_path = work / "owner.json"
        if record_path.exists() or record_path.is_symlink() or (work / "pids").exists():
            raise OwnershipError("existing ownership record requires owner resolution")
        for name in ("xemu.log", "xemu.toml", "hdd.qcow2", "rt"):
            if (work / name).is_symlink():
                raise OwnershipError("symlink managed path")
        targets = []
        for command in commands:
            target = open_executable(command)
            binaries.callback(os.close, target)
            targets.append(target)
        if config is not None:
            if hdd_source is None:
                raise OwnershipError("missing HDD source")
            if not (work / "hdd.qcow2").exists():
                shutil.copyfile(hdd_source, work / "hdd.qcow2")
            (work / "xemu.toml").write_text(config)
            (work / "rt").mkdir(exist_ok=True, mode=0o700)
        nonce = secrets.token_hex(32)
        with socket.socket(socket.AF_UNIX) as server, (work / "xemu.log").open("a") as log:
            server.bind("\0tsfp-xemu-" + nonce)
            server.listen(1)
            server.settimeout(0.05)
            published = False
            try:
                for command, target in zip(commands, targets, strict=True):
                    ready_read, ready_write = os.pipe()
                    try:
                        child = subprocess.Popen(
                            [
                                sys.executable,
                                str(Path(__file__).resolve()),
                                "--owned-child",
                                str(os.getpid()),
                                str(ready_write),
                                str(target),
                                json.dumps(command),
                            ],
                            stdout=log,
                            stderr=subprocess.STDOUT,
                            env=os.environ.copy(),
                            pass_fds=(ready_write, target),
                        )
                    except BaseException:
                        os.close(ready_read)
                        raise
                    finally:
                        os.close(ready_write)
                    try:
                        try:
                            fd = os.pidfd_open(child.pid)
                        except OSError:
                            _reap_unregistered_child(child)
                            raise
                        children.append((child, fd))
                        if not select.select([ready_read], [], [], 3)[0]:
                            raise OwnershipError("child protection acknowledgement timeout")
                        if os.read(ready_read, 6) != b"ARMED":
                            raise OwnershipError("child parent-death protection refused")
                    finally:
                        os.close(ready_read)
                    time.sleep(0.05)
                    if child.poll() is not None:
                        raise OwnershipError("child exited during startup")
                # Startup acknowledgement is not VM readiness, only owned process creation.
                record = {
                    "identity": identity(os.getpid()),
                    "nonce": nonce,
                    "children": [identity(child.pid) for child, _ in children],
                }
                temporary = work / ("owner-" + nonce + ".tmp")
                descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(descriptor, "w") as output:
                    json.dump(record, output)
                temporary.replace(record_path)
                published = True
                print(json.dumps({"started": True, "pid": os.getpid()}), flush=True)
                deadline = time.monotonic() + lifetime
                while not stopping and time.monotonic() < deadline:
                    if any(child.poll() is not None for child, _ in children):
                        break
                    try:
                        connection, _ = server.accept()
                    except TimeoutError:
                        continue
                    with connection:
                        connection.settimeout(1)
                        try:
                            message = json.loads(connection.recv(4096))
                            if message.get("nonce") != nonce:
                                continue
                            command = message.get("command")
                            if command == "stop":
                                cleanup(children)
                                children.clear()
                                record_path.unlink()
                                published = False
                                stopping = True
                            connection.sendall(
                                json.dumps(
                                    {"nonce": nonce, "owned": True, "stopped": stopping}
                                ).encode()
                            )
                        except (OSError, ValueError):
                            continue
            finally:
                cleanup(children)
                if published:
                    record_path.unlink(missing_ok=True)


def main() -> None:
    if len(sys.argv) == 6 and sys.argv[1] == "--owned-child":
        try:
            owned_exec(
                int(sys.argv[2]), int(sys.argv[3]), json.loads(sys.argv[5]), int(sys.argv[4])
            )
        except (OwnershipError, OSError, ValueError, TypeError) as error:
            print(f"owned child refused: {error}", flush=True)
            sys.exit(1)
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--lifetime", type=float, required=True)
    parser.add_argument("--commands", required=True)
    parser.add_argument("--config")
    parser.add_argument("--hdd-source", type=Path)
    args = parser.parse_args()
    try:
        if not math.isfinite(args.lifetime) or args.lifetime <= 0:
            raise OwnershipError("positive lifetime required")
        supervise(args.work, args.lifetime, json.loads(args.commands), args.config, args.hdd_source)
    except (OwnershipError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"started": False, "error": str(error)}), flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
