# SPDX-License-Identifier: GPL-3.0-or-later
"""Verified private installed-baseline plus genuine T803 menu entry (T941).

Never installs a fresh lift wholesale and never writes the canonical baseline.
Receipts and generated code remain private, derived from the user's retail XBE.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.graft_lifted_function import CHUNK_HEADER, function_body, graft

ADDRESS = 0x00191B10
END = 0x00191B97
NAME = "sub_00191B10"
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
SPAN_SHA = "34937a6df92c51d93a7b719a939fdac73f680054555a1092bed67f802b58167e"
VERSION = 1


class Seed:
    """One genuine observed lift entry grafted into a private profile (never canonical)."""

    def __init__(self, address: int, end: int, span_sha: str, cache: str, label: str) -> None:
        self.address, self.end, self.span_sha = address, end, span_sha
        self.cache, self.label = cache, label
        self.name = f"sub_{address:08X}"


MENU = Seed(ADDRESS, END, SPAN_SHA, "seed-profiles", "menu")
# T1128 callback registered only by `push 0x5FCF0` at 0xD35F7: the Story level start
# stops on it unless the private profile carries the genuine lifted body (T1075).
STORY = Seed(
    0x0005FCF0,
    0x0005FDB4,
    "0ed79202b0808b3ef6ba169e4838dbbad710254367f354222097f77545d5842d",
    "story-seed-profiles",
    "story callback",
)

# T1180: MapMaker screen-record entry observed in run20261005-222346.
MAPMAKER = Seed(
    0x00313010,
    0x003132CB,
    "6cb83a1ec37884defd49b51878d3602ff315f4ad962dc137c51a5423350d11e7",
    "mapmaker-seed-profiles",
    "MapMaker callback",
)

# T1200: Map Maker text-field record function (dword 0x4D8D78) observed in the user's
# save attempt run 20261005-231320 (stop: indirect call/safe 0x7E800).
TEXTFIELD = Seed(
    0x0007E800,
    0x0007EA2F,
    "8320606e9383526a533493db7b851337b0224e2ed3a62dca27e61d050ef1d279",
    "textfield-seed-profiles",
    "Map Maker text-field callback",
)


# T1496: the Audio/Video widget registers this draw callback as an immediate.
# It follows another RET without INT3 padding and discovery omitted its entry.
AUDIO_VIDEO = Seed(
    0x002C3500,
    0x002C360E,
    "b6445bb45503804596f6164bdeb00883e216c5094700c50b53ee9cd8f1f982a0",
    "audio-video-seed-profiles",
    "Audio/Video draw callback",
)


# T1514: record-table callback reached when Story level 2 starts.
LEVEL2 = Seed(
    0x001E9CC0,
    0x001EA635,
    "a77309796f01c1495c35bbaecf9217c93685d5a5f54f4affc398501d3551fdcf",
    "level2-seed-profiles",
    "Level 2 record callback",
)


# T1518: callback registered by `push 0x1E8BD0` inside the LEVEL2 body, reached next.
LEVEL2_B = Seed(
    0x001E8BD0,
    0x001E8C76,
    "b4e362bff817e9c885cfc9e4b83077d3344c2af6cf59ff567aea47b8f08062d5",
    "level2b-seed-profiles",
    "Level 2 registered callback",
)


# T1530: Map Maker Flare Gun record method (dword at 0x4DF4DC), no direct caller.
FLARE = Seed(
    0x000F1A90,
    0x000F2907,
    "d674ef38ff75c2813eee5b67592595ae0bb9b09a82c78b5170a1a9a0fa7a95a3",
    "flare-seed-profiles",
    "Flare Gun record method",
)


# T1533: data-dword record methods the disassembler never started (audit of every code
# address in .rdata/.data/.data1, tools/data_dword_function_audit.py). Same class as FLARE.
# All share one cache and one fresh lift: the first seed lifts every body in the tuple.
DATA_DWORD_SEED_SPANS = (
    (0x00162450, 0x0016251B, "bbc324564d32c7798f2cedf3c2a421c3bec63669f8aa08aa21cae4c4ce7e7d1e"),
    (0x00163220, 0x001632FA, "16bd533e5b28d8d0cdeb9a09ede80627059da0b22fd2bf8ea813471e371ad3b3"),
    (0x001F5A10, 0x001F5FBA, "b0cf60cf0d261d983055db4eda6f36844a61f97a2fe9807d73c99c048ea26eaf"),
    (0x00213F50, 0x0021421C, "c9a2e26651f3880511d7558f5a76a6d29384dd4c0e86e317634b4e4d1e9809e7"),
    (0x00220600, 0x00220756, "32c1cc53826643631af199bfb9f7330509d3daf6915f32cbc3ac61a8f1523c27"),
    (0x00251450, 0x002517F3, "0bc9be9d62f6cd2a0a7e63aadaadd57213ea29dd4682fa29291ed9402c3d7722"),
    (0x00315B30, 0x00315D07, "ddedbb19362c620a6cbc8a0a7abe534c44d2dc658cb171a8704643daf292ea5c"),
    (0x00337A50, 0x00337B43, "c0d7057869abef859d19ca60461c0bc7114f59a9bc6841bd077f9cd5ebf2ef9b"),
    (0x00371410, 0x003716F2, "84908f0b65d26ca2fdcb33f76402096e954dc0825c6169c1300cb65dbb29ad5a"),
)
DATA_DWORD_SEEDS = tuple(
    Seed(
        address,
        end,
        span_sha,
        "data-dword-seed-profiles",
        f"data dword record method {address:#010x}",
    )
    for address, end, span_sha in DATA_DWORD_SEED_SPANS
)


class ProfileError(RuntimeError):
    pass


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def manifest(directory: Path) -> dict[str, dict[str, object]]:
    """Hash every file and its mode; reject symlinks/special objects rather than dereference."""
    result = {}
    if directory.is_symlink() or not directory.is_dir():
        raise ProfileError(f"baseline/profile is not a plain directory: {directory}")
    for path in sorted(directory.rglob("*")):
        info = path.lstat()
        key = path.relative_to(directory).as_posix()
        mode = stat.S_IMODE(info.st_mode)
        if stat.S_ISDIR(info.st_mode):
            result[key] = {"mode": mode, "directory": True}
        elif stat.S_ISREG(info.st_mode):
            result[key] = {"mode": mode, "sha256": sha(path)}
        else:
            raise ProfileError(f"unsupported linked/special profile input: {path}")
    return result


def identity(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def _test_input(relative: str) -> bool:
    path = Path(relative)
    return "tests" in path.parts or path.name.startswith("test_")


def provenance(root: Path) -> dict[str, object]:
    paths = [root / "tools/lift", root / "tools/config", root / "third_party/xboxrecomp"]
    result = {}
    for base in paths:
        if not base.is_dir():
            raise ProfileError(f"missing lifter input: {base}")
        for path in sorted(base.rglob("*")):
            # Python caches/build output are not lifter inputs. Source/config/schema/patches are.
            if path.is_file() and path.suffix in {".py", ".json", ".patch", ".h", ".c"}:
                if "__pycache__" not in path.parts and "output" not in path.parts:
                    relative = path.relative_to(root).as_posix()
                    if _test_input(relative):
                        continue
                    if path.name == "VENDOR.json":
                        vendor = json.loads(path.read_text())
                        for bucket in ("files", "local"):
                            vendor[bucket] = {
                                k: v
                                for k, v in vendor.get(bucket, {}).items()
                                if not _test_input(k)
                            }
                        result[relative] = identity(vendor)
                    else:
                        result[relative] = sha(path)
    for relative in ("tools/graft_lifted_function.py", "tools/play/seed_profile.py"):
        result[relative] = sha(root / relative)
    return result


def verify_xbe(xbe: Path, seed: Seed = MENU) -> None:
    from tools.dsoundscan.image import Image

    if sha(xbe) != XBE_SHA:
        raise ProfileError("menu seed profile requires the hash-bound retail XBE")
    size = seed.end - seed.address
    span = Image(xbe).read(seed.address, size)
    if len(span) != size or hashlib.sha256(span).hexdigest() != seed.span_sha:
        raise ProfileError(
            f"retail {seed.label} seed span does not match the reviewed {size:#x}-byte body"
        )


def verify_graft(baseline: Path, fresh: Path, output: Path, chunk: str, seed: Seed = MENU) -> None:
    """Verify exact two metadata insertions and one body; preserve all other bytes/modes."""
    before, after = manifest(baseline), manifest(output)
    added = f"recomp_{chunk}.c"
    if added in before or set(after) != set(before) | {added}:
        raise ProfileError("private graft added/removed unexpected files or collided with a chunk")
    for key, value in before.items():
        if key not in {"recomp_dispatch.c", "recomp_funcs.h"}:
            if after[key] != value:
                raise ProfileError(f"private graft changed baseline identity: {key}")
        elif after[key]["mode"] != value["mode"]:
            raise ProfileError(f"private graft changed baseline mode: {key}")
    header = (baseline / "recomp_funcs.h").read_text()
    at = header.rfind("#endif")
    expected = header[:at] + f"void {seed.name}(void);\n\n" + header[at:]
    if at < 0 or (output / "recomp_funcs.h").read_text() != expected:
        raise ProfileError("private graft prototype change is not the exact insertion")
    dispatch = (baseline / "recomp_dispatch.c").read_text()
    row = f"    {{ 0x{seed.address:08X}u, (recomp_func_t){seed.name} }},\n"
    rows = list(
        re.finditer(r"^    \{ 0x([0-9A-F]{8})u, \(recomp_func_t\)\w+ \},\n", dispatch, re.M)
    )
    size = re.search(r"static const size_t g_recomp_table_size = (\d+);", dispatch)
    if not rows or size is None or any(int(r[1], 16) == seed.address for r in rows):
        raise ProfileError("baseline dispatch is missing, malformed, or already contains the seed")
    if [int(r[1], 16) for r in rows] != sorted(int(r[1], 16) for r in rows):
        raise ProfileError("baseline dispatcher is not sorted")
    if int(size[1]) != len(rows):
        raise ProfileError("baseline dispatcher count does not match its rows")
    following = [r for r in rows if int(r[1], 16) > seed.address]
    at = following[0].start() if following else rows[-1].end()
    expected = dispatch[:at] + row + dispatch[at:]
    expected = expected.replace(
        size[0], f"static const size_t g_recomp_table_size = {len(rows) + 1};"
    )
    if (output / "recomp_dispatch.c").read_text() != expected:
        raise ProfileError("private graft dispatcher change is not the exact sorted insertion")
    if (output / added).read_text() != CHUNK_HEADER.format(name=seed.name) + function_body(
        fresh, seed.name
    ):
        raise ProfileError("private graft body differs from the frozen fresh lift")


def run_lift(command: list[str], root: Path, timeout: float = 3600) -> None:
    """Bound the whole private pipeline, including subprocess stages, and reap it."""
    process = subprocess.Popen(command, cwd=root, start_new_session=True)  # noqa: S603
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise ProfileError("private menu lift exceeded its process-group deadline") from error
    if status:
        raise subprocess.CalledProcessError(status, command)


def reusable_seed_lift(baseline: Path, inputs: dict[str, object], seed: Seed) -> Path | None:
    """A previous seed used the same lift inputs; authenticate its complete output."""
    try:
        receipt = json.loads((baseline.parent / "receipt.json").read_text())
        prior = receipt["inputs"]
        for key in ("lifter", "xbe_sha256", "manual_sha256", "bridge_sha256"):
            if prior[key] != inputs[key]:
                return None
        fresh = baseline.parent / "fresh/gen"
        if receipt["output"] != manifest(baseline) or receipt["fresh_output"] != manifest(fresh):
            return None
        function_body(fresh, seed.name)
        return fresh
    except (OSError, KeyError, ValueError, RuntimeError, ProfileError):
        return None


def _prepare_seed(root: Path, xbe: Path, baseline: Path | None = None, seed: Seed = MENU) -> Path:
    root, xbe = root.resolve(), xbe.resolve()
    baseline = (baseline or root / "generated/lifted/gen").resolve()
    dispatch = baseline / "recomp_dispatch.c"
    if not dispatch.is_file():
        raise ProfileError(f"installed lifted baseline is unavailable: {baseline}")
    if f"(recomp_func_t){seed.name}" in dispatch.read_text():
        print(f"play: installed baseline already contains genuine {seed.name}: {baseline}")
        return baseline
    verify_xbe(xbe, seed)
    before = manifest(baseline)
    inputs = {
        "version": VERSION,
        "baseline": before,
        "baseline_path": str(baseline),
        "xbe_sha256": XBE_SHA,
        "span_sha256": seed.span_sha,
        "span": [seed.address, seed.end],
        "lifter": provenance(root),
        "manual_sha256": sha(root / "generated/lifted/manual.json"),
        "bridge_sha256": sha(root / "tools/config/flag_bridge.json"),
    }
    key = identity(inputs)
    cache = root / "tmp/play" / seed.cache
    cache.mkdir(parents=True, exist_ok=True)
    final = cache / key
    receipt_path = final / "receipt.json"
    if final.exists():
        try:
            receipt = json.loads(receipt_path.read_text())
            if receipt["inputs"] != inputs or manifest(final / "gen") != receipt["output"]:
                raise ProfileError(
                    "private seed cache identity mismatch; refusing stale/modified code"
                )
        except (OSError, KeyError, ValueError) as error:
            raise ProfileError(f"invalid private seed cache receipt: {error}") from error
        print(f"play: verified private {seed.label} seed profile {key}")
        return final / "gen"
    staging = Path(tempfile.mkdtemp(prefix=".prepare-", dir=cache))
    try:
        fresh = staging / "fresh"
        command = [
            sys.executable,
            "-m",
            "tools.lift",
            "--vendor-root",
            str(root / "third_party/xboxrecomp"),
            "run",
            str(xbe),
            "--out-dir",
            str(fresh),
            # Only these bodies are consumed by private graft/refresh stages.
            # Discovery and ABI retain the complete function database.
            *[
                value
                for address in (
                    seed.address,
                    *[extra.address for extra in DATA_DWORD_SEEDS],
                    MENU.address,
                    STORY.address,
                    MAPMAKER.address,
                    TEXTFIELD.address,
                    0x002561D0,
                    0x002FB4C0,
                    0x0007A7B0,
                )
                for value in ("--only-function", hex(address))
            ],
            "--manual-functions",
            str(root / "generated/lifted/manual.json"),
            "--flag-bridge",
            str(root / "tools/config/flag_bridge.json"),
        ]
        print(
            f"play: preparing genuine hash-bound {seed.label} seed "
            "(private lift; canonical untouched)"
        )
        shared = reusable_seed_lift(baseline, inputs, seed)
        if shared is None:
            run_lift(command, root)
        else:
            print(f"play: reusing verified fresh lift for {seed.label} seed")
            shutil.copytree(shared, fresh / "gen")
        gen = fresh / "gen"
        body = function_body(gen, seed.name)
        prototypes = (baseline / "recomp_funcs.h").read_text()
        dependencies = set(re.findall(r"\bsub_[0-9A-F]{8}\b", body)) - {seed.name}
        missing = [name for name in sorted(dependencies) if f"void {name}(void);" not in prototypes]
        if missing:
            raise ProfileError(f"private seed has unavailable baseline dependencies: {missing}")
        chunk = next(
            (f"{n:04d}" for n in range(9000, 10000) if f"recomp_{n:04d}.c" not in before), None
        )
        if chunk is None:
            raise ProfileError("no unused private graft chunk")
        graft(baseline, gen, staging / "gen", seed.address, chunk)
        verify_graft(baseline, gen, staging / "gen", chunk, seed)
        if manifest(baseline) != before or provenance(root) != inputs["lifter"]:
            raise ProfileError(
                "baseline/lifter changed during private preparation; refusing publication"
            )
        receipt = {
            "inputs": inputs,
            "key": key,
            "body_sha256": hashlib.sha256(body.encode()).hexdigest(),
            "fresh_output": manifest(gen),
            "chunk": chunk,
            "output": manifest(staging / "gen"),
            "lift_command": command,
        }
        (staging / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        # Preserve exact fresh output/private generation provenance for later inspection.
        try:
            os.rename(staging, final)
        except OSError as error:
            if final.exists():
                raise ProfileError(
                    "concurrent private profile publication; retry after receipt validation"
                ) from error
            raise
        print(
            f"play: verified private {seed.label} seed profile {key}; body {receipt['body_sha256']}"
        )
        return final / "gen"
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def _prepare_menu(root: Path, xbe: Path, baseline: Path | None = None) -> Path:
    return _prepare_seed(root, xbe, baseline, MENU)


def prepare(root: Path, xbe: Path, baseline: Path | None = None) -> Path:
    from tools.play.float_profile import WIDGET_REFRESH
    from tools.play.float_profile import prepare as prepare_float
    from tools.play.zf_profile import prepare as prepare_zf

    menu = _prepare_menu(root, xbe, baseline)
    story = _prepare_seed(root, xbe, menu, STORY)
    mapmaker = _prepare_seed(root, xbe, story, MAPMAKER)
    textfield = _prepare_seed(root, xbe, mapmaker, TEXTFIELD)
    audio_video = _prepare_seed(root, xbe, textfield, AUDIO_VIDEO)
    level2 = _prepare_seed(root, xbe, audio_video, LEVEL2)
    level2_b = _prepare_seed(root, xbe, level2, LEVEL2_B)
    flare = _prepare_seed(root, xbe, level2_b, FLARE)
    for audited in DATA_DWORD_SEEDS:
        flare = _prepare_seed(root, xbe, flare, audited)
    scalar = prepare_float(root, xbe, prepare_zf(root, xbe, flare))
    return prepare_float(root, xbe, scalar, WIDGET_REFRESH)
