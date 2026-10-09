"""Standalone default-off O0/O3 build; no normal CMake/host linkage."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from .generate import IDENTITY, ROOT, canonical, describe, digest, generate


def build(profile_path: Path, xbe: Path, output: Path, opt: int, compiler: str = "cc") -> Path:
    if opt not in (0, 3):
        raise ValueError("pilot builds only O0 or O3")
    profile = json.loads(profile_path.read_text())
    if profile != describe(xbe):
        raise ValueError("stale or changed pilot profile/source/layout/image")
    cc = shutil.which(compiler)
    if cc is None:
        raise ValueError("pilot C compiler unavailable")
    version = subprocess.check_output([cc, "--version"], text=True).splitlines()[0]
    flags = ["-std=c11", f"-O{opt}", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror"]
    source_sha = hashlib.sha256(canonical(profile["sources"])).hexdigest()
    receipt = {
        "profile": profile,
        "compiler": cc,
        "compiler_version": version,
        "compiler_sha256": digest(Path(cc)),
        "flags": flags,
    }
    build_sha = hashlib.sha256(canonical(receipt)).hexdigest()
    output.mkdir(parents=True, exist_ok=True)
    header = output / "ret_pilot_profile_generated.h"
    header.write_text(
        "/* Generated private pilot pins; never production dispatch. */\n"
        "static const ret_pilot_profile generated_profile = {\n"
        "RET_PILOT_ABI, RET_PILOT_LAYOUT, sizeof(ret_pilot_machine),\n"
        "sizeof(ret_pilot_frame), sizeof(ret_pilot_context),\n"
        "RET_PILOT_VIEW_ABI, sizeof(ret_pilot_page), sizeof(ret_pilot_view),\n"
        f'"{IDENTITY}", "{profile["image_sha256"]}", "{source_sha}", "{build_sha}"\n'
        "};\n"
    )
    with header.open("a") as stream:
        stream.write(
            "static const struct { uint32_t va; unsigned size; unsigned char bytes[15]; } "
            "approved_code[] = {\n"
        )
        for address, instruction in profile["instructions"].items():
            body = bytes.fromhex(instruction["bytes"])
            values = ",".join(str(byte) for byte in body)
            stream.write(f"{{{address}u, {len(body)}u, {{{values}}}}},\n")
        stream.write("};\n")
    binary = output / "libret_pilot.so"
    subprocess.run(
        [
            cc,
            *flags,
            "-I",
            str(output),
            "-I",
            str(ROOT / "src/host"),
            str(ROOT / "src/host/recomp_ret_pilot.c"),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
    )
    receipt.update(
        {
            "build_sha256": build_sha,
            "binary_sha256": digest(binary),
            "generated_header_sha256": digest(header),
        }
    )
    (output / "build.json").write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    return binary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=ROOT / "build/default.xbe")
    parser.add_argument("--out", type=Path, default=ROOT / "generated/ret-pilot/v2")
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--opt", type=int, choices=[0, 3], default=0)
    args = parser.parse_args()
    profile = args.profile or generate(args.xbe, args.out)
    print(build(profile, args.xbe, args.out / f"o{args.opt}", args.opt))


if __name__ == "__main__":
    main()
