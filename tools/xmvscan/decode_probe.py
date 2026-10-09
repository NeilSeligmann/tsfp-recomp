# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional installed-FFmpeg decode evidence; no guest adapter or playback claim."""

import argparse
import ctypes.util
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from tools.errors import ParseError
from tools.xmvscan import parse


class ProbeError(Exception):
    """Unavailable dependencies or failed decode proof, never synthetic success."""


def probe(asset: Path, headers: Path, *, compiler: str = "clang") -> dict[str, Any]:
    data = asset.read_bytes()
    container = parse(data)  # malformed input cannot reach external tooling
    for name in ("libavformat/avformat.h", "libavcodec/avcodec.h", "libavutil/avconfig.h"):
        if not (headers / name).is_file():
            raise ProbeError(f"prepared FFmpeg header unavailable: {headers / name}")
    cc = shutil.which(compiler)
    if cc is None:
        raise ProbeError(f"compiler unavailable: {compiler}")
    libraries = [ctypes.util.find_library(name) for name in ("avformat", "avcodec", "avutil")]
    if any(name is None for name in libraries):
        raise ProbeError("installed FFmpeg shared libraries unavailable")
    source = Path(__file__).with_suffix(".c")
    with tempfile.TemporaryDirectory(prefix="tsfp-xmv-decode-") as temporary:
        executable = Path(temporary) / "probe"
        snapshot = Path(temporary) / "validated-input.xmv"
        snapshot.write_bytes(data)  # decode the same bytes whose chain was validated
        command = [
            cc,
            "-std=c11",
            "-O2",
            "-I",
            str(headers.resolve()),
            str(source),
            *[f"-l:{name}" for name in libraries],
            "-o",
            str(executable),
        ]
        try:
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)  # noqa: S603
            decoded = subprocess.run(
                [str(executable), str(snapshot)],
                check=True,  # noqa: S603
                capture_output=True,
                text=True,
                timeout=120,
            )
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            raise ProbeError(f"compile/decode failed: {error}; stderr={error.stderr}") from error
    try:
        result = json.loads(decoded.stdout)
        videos = [stream for stream in result["streams"] if stream["media_type"] == 0]
        complete_frames = sum(stream["frames"] for stream in videos) == container.raw_frame_count
        result["container_raw_frame_count"] = container.raw_frame_count
        result["decoded_video_count_matches_container"] = complete_frames
        result["terminal_eio_candidate"] = (
            result["demux_end_code"] == -5
            and result["demux_offset"] == len(data)
            and result["file_size"] == len(data)
            and container.packets[-1].next_size == 0
            and complete_frames
            and result["send_errors"] == 0
            and all(stream["errors"] == 0 for stream in result["streams"])
        )
        if result["demux_end_code"] != -541478725 and not result["terminal_eio_candidate"]:
            raise ProbeError(f"unproven demux termination: {result['demux_end_code']}")
        if (
            not complete_frames
            or result["send_errors"]
            or any(stream["errors"] for stream in result["streams"])
        ):
            raise ProbeError("missing frames or codec errors")
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise ProbeError(f"invalid decoder report: {error}") from error
    result["decoder_stderr"] = decoded.stderr
    result["limitations"] = (
        "Real host decode evidence only. EIO is preserved, not converted to EOF; terminal "
        "candidate requires original comparison. Raw duration/PTS remain unreconciled. "
        "No guest object, timing, audio output, playback or completion equivalence. "
        "FNV64 fingerprints decoded bytes, not a cryptographic proof."
    )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--ffmpeg-headers", type=Path, required=True)
    parser.add_argument("--compiler", default="clang")
    args = parser.parse_args(argv)
    try:
        result = probe(args.input, args.ffmpeg_headers, compiler=args.compiler)
    except (OSError, ParseError, ProbeError) as error:
        parser.exit(2, f"xmv decode probe: {error}\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
