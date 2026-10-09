# SPDX-License-Identifier: GPL-3.0-or-later
"""Private capture transcription; local by default, API requires --allow-upload."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
import urllib.error
import urllib.request
from datetime import UTC, datetime
from difflib import SequenceMatcher
from pathlib import Path


def convert(source: Path, target: Path) -> None:
    """Preserve capture clock while downmixing 48k stereo s16le to 16k mono."""
    if target.exists():
        raise FileExistsError("Conversion output already exists")
    if source.stat().st_size % 4:
        raise ValueError("PCM must contain complete stereo frames")
    subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-f",
            "s16le",
            "-ar",
            "48000",
            "-ac",
            "2",
            "-i",
            str(source),
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            "-n",
            str(target),
        ],
        check=True,
    )


def api_transcribe(wav: Path, key_file: Path) -> dict:
    """Key stays in memory; never in argv, environment or exception messages."""
    data = wav.read_bytes()
    if len(data) >= 25_000_000:
        raise ValueError("Upload must be under 25 MB")
    boundary = "t1792" + hashlib.sha256(data).hexdigest()
    fields = {
        "model": "whisper-1",
        "response_format": "verbose_json",
        "timestamp_granularities[]": "segment",
        "language": "en",
    }
    parts = []
    for name, value in fields.items():
        parts.append(
            (
                f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'
            ).encode()
        )
    parts.append(
        (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
            'filename="capture.wav"\r\nContent-Type: audio/wav\r\n\r\n'
        ).encode()
        + data
        + b"\r\n"
    )
    parts.append(f"--{boundary}--\r\n".encode())
    request = urllib.request.Request(
        "https://api.openai.com/v1/audio/transcriptions",
        data=b"".join(parts),
        headers={
            "Authorization": "Bearer " + key_file.read_text().strip(),
            "Content-Type": "multipart/form-data; boundary=" + boundary,
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"Transcription HTTP status {error.code}") from None
    except urllib.error.URLError:
        raise RuntimeError("Transcription network connection failed") from None


def normalize(text: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


def match(segments: list[dict], expected: list[dict], threshold: float = 0.65) -> dict:
    """One-to-one fuzzy matching, constrained by clock if requests expose it."""
    if not 0 <= threshold <= 1:
        raise ValueError("Match threshold must be between zero and one")
    used = set()
    matches = []
    unmatched = []
    for index, segment in enumerate(segments):
        candidates = []
        for j, line in enumerate(expected):
            if j in used:
                continue
            if "start" in line and abs(line["start"] - segment["start"]) > 5:
                continue
            score = SequenceMatcher(
                None, normalize(segment["text"]), normalize(line["text"])
            ).ratio()
            candidates.append((score, j))
        score, j = max(candidates, default=(0, -1))
        if score >= threshold:
            used.add(j)
            matches.append(
                {
                    "segment": index,
                    "expected": j,
                    "score": score,
                    "label_id": expected[j].get("label_id"),
                    "voice_id": expected[j].get("voice_id"),
                }
            )
        else:
            unmatched.append(index)
    return {
        "matched": matches,
        "unmatched_speech": unmatched,
        "expected_but_unplayed": [j for j in range(len(expected)) if j not in used],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--backend", choices=["local", "api"], default="local")
    parser.add_argument("--allow-upload", action="store_true")
    parser.add_argument("--key-file", type=Path, default=Path(".secrets/openai-api-key.txt"))
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument(
        "--iso", type=Path, help="Read-only owner disc for label-only expected rows"
    )
    parser.add_argument(
        "--expected",
        type=Path,
        help="Private JSON request rows with text, label_id, voice_id, start",
    )
    args = parser.parse_args()
    if args.backend == "api" and not args.allow_upload:
        parser.error("API requires explicit --allow-upload owner authorization")
    if args.backend == "local" and args.model_dir is None:
        parser.error("Local requires --model-dir (pinned faster-whisper model directory)")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    wav = args.out_dir / "capture-16k-mono.wav"
    if wav.exists():
        parser.error("Output wav exists: use a fresh out-dir to avoid stale conversion")
    convert(args.source, wav)
    manifest = {
        "backend": args.backend,
        "model": "whisper-1" if args.backend == "api" else str(args.model_dir),
        "date": datetime.now(UTC).isoformat(),
        "wav_bytes": wav.stat().st_size,
        "wav_sha256": hashlib.sha256(wav.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
        "status": "prepared; transcription not completed",
    }
    manifest_path = args.out_dir / "attempt.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    if args.backend == "api":
        try:
            result = api_transcribe(wav, args.key_file)
        except RuntimeError as error:
            manifest["status"] = str(error)
            manifest_path.write_text(json.dumps(manifest, indent=2))
            raise
        model = "whisper-1"
        pins = {}
    else:
        from faster_whisper import WhisperModel

        pins = {
            str(p.relative_to(args.model_dir)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(args.model_dir.rglob("*"))
            if p.is_file()
        }
        model = str(args.model_dir)
        engine = WhisperModel(model, device="cpu", compute_type="int8")
        segments, info = engine.transcribe(str(wav), language="en", beam_size=5)
        result = {
            "language": info.language,
            "segments": [
                {
                    "start": s.start,
                    "end": s.end,
                    "text": s.text,
                    "avg_logprob": s.avg_logprob,
                    "no_speech_prob": s.no_speech_prob,
                    "confidence": math.exp(min(0, s.avg_logprob)),
                }
                for s in segments
            ],
        }
    for segment in result["segments"]:
        if "avg_logprob" in segment:
            segment["confidence"] = math.exp(min(0, segment["avg_logprob"]))
    manifest["status"] = "completed"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    result["provenance"] = {
        "backend": args.backend,
        "model": model,
        "date": datetime.now(UTC).isoformat(),
        "wav_sha256": hashlib.sha256(wav.read_bytes()).hexdigest(),
        "upload_completed": args.backend == "api",
        "wav_bytes": wav.stat().st_size,
        "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
        "model_files_sha256": pins,
        "confidence_kind": "uncalibrated exp(min(0,avg_logprob)); absent if unavailable",
        "clock": "capture seconds; no prefix trimmed",
    }
    (args.out_dir / "transcript.json").write_text(json.dumps(result, indent=2))
    expected = json.loads(args.expected.read_text()) if args.expected else []
    if any("text" not in row for row in expected):
        if args.iso is None:
            parser.error("Label-only expected rows require --iso")
        from tools.frontend_labels import load

        labels = load(args.iso)
        expected = [
            dict(row, text=row.get("text") or labels.text(int(str(row["label_id"]), 0)))
            for row in expected
        ]
    report = (
        match(result["segments"], expected)
        if args.expected
        else {
            "matched": [],
            "unmatched_speech": None,
            "expected_but_unplayed": None,
            "reason": "No observed expected request list; absence is not zero requests",
        }
    )
    report["expected_available"] = args.expected is not None
    (args.out_dir / "matches.json").write_text(json.dumps(report, indent=2))
    print(
        json.dumps(
            {
                "segments": len(result["segments"]),
                "matched": len(report["matched"]),
                "expected_available": report["expected_available"],
                "wav_bytes": wav.stat().st_size,
            }
        )
    )


if __name__ == "__main__":
    main()
