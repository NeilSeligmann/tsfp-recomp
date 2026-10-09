#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lift the measured shader-assembler CPU profile into separate private aliases.

Normal manual bodies and routing maps stay unchanged. This is provenance, not
runtime cryptographic attestation; the host must explicitly enable the profile.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.util
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

from tools import retained_original
from tools.gen_xdk_manual_list import parse_surface, symbol_for
from tools.shaderscan.image import Image

REQUIRED = frozenset((0x003EE2B3, 0x003E6714, 0x003F1791, 0x003F42A0, 0x003EA301))
REQUIRED_V2 = REQUIRED | frozenset((0x003FC8A8, 0x00402BCF, 0x00402C5C))
REQUIRED_V3 = REQUIRED_V2 | frozenset((0x003EFEA7,))
REQUIRED_V4 = REQUIRED_V3 | frozenset((0x003F22B4,))
REQUIRED_V5 = REQUIRED_V4 | frozenset((0x003F36F1,))
REQUIRED_V6 = REQUIRED_V5 | frozenset((0x00400D98,))
REQUIRED_V7 = REQUIRED_V6 | frozenset((0x003F17AE,))
REQUIRED_V8 = REQUIRED_V7 | frozenset((0x003F1786, 0x003F17D4))
VARIANT_HELPERS = (0x003F1012, 0x003F00B9, 0x003F11AC, 0x003F010B)
RECURSIVE_HELPERS = (0x003F1102, 0x003F00E2)
ARRAY_TEARDOWN_TRANSLATION = {0x00400D98: 0x00400926}
ARRAY_TEARDOWN_HELPERS = (
    0x00400926,
    0x004006EF,
    0x004001CE,
    0x00380CF3,
    0x003820BA,
    0x00383DF3,
)
MEMBERSHIP_HELPERS = (
    0x003F3497,
    0x003F0F95,
    0x003F2D45,
    0x003F007C,
    0x003F2385,
    0x003F25D4,
    0x003F1922,
    0x003F12A3,
    0x003F12C4,
    0x003F1355,
    0x003F13A0,
)
PROFILES = {
    "shader-assembler": REQUIRED,
    "shader-assembler-v2": REQUIRED_V2,
    "shader-assembler-v3": REQUIRED_V3,
    "shader-assembler-v4": REQUIRED_V4,
    "shader-assembler-v5": REQUIRED_V5,
    "shader-assembler-v6": REQUIRED_V6,
    "shader-assembler-v7": REQUIRED_V7,
    "shader-assembler-v8": REQUIRED_V8,
}
ROOT = Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def address(value: object) -> int:
    if isinstance(value, bool):
        raise ValueError("boolean address")
    if isinstance(value, str) and re.fullmatch(r"0[xX][0-9a-fA-F]{1,8}", value):
        return address(int(value, 16))
    if isinstance(value, int) and 0 < value < 0xFE000000:
        return value
    raise ValueError("address must be a nonzero guest uint32 address")


def strict_json(path: Path) -> Any:
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result

    return json.loads(path.read_text(), object_pairs_hook=unique)


def load_inputs(entries: Path, manual: Path, surface: Path) -> tuple[list[int], dict[int, str]]:
    profile = strict_json(entries)
    if not isinstance(profile, dict) or set(profile) != {"schema", "profile", "addresses"}:
        raise ValueError("strict profile schema required")
    if (
        type(profile["schema"]) is not int
        or profile["schema"] != 1
        or not isinstance(profile["profile"], str)
        or profile["profile"] not in PROFILES
    ):
        raise ValueError("unsupported original profile")
    if not isinstance(profile["addresses"], list):
        raise ValueError("addresses must be an array")
    selected = [address(v) for v in profile["addresses"]]
    if len(selected) != len(set(selected)) or set(selected) != PROFILES[profile["profile"]]:
        raise ValueError("the complete selected shader profile is required")
    raw = strict_json(manual)
    if not isinstance(raw, dict) or not raw:
        raise ValueError("full pinned manual map required")
    mapping: dict[int, str] = {}
    for key, name in raw.items():
        va = address(key)
        if va in mapping or name != symbol_for(va):
            raise ValueError("manual map requires unique canonical pinned symbols")
        mapping[va] = name
    rows = parse_surface(surface)
    if len({row[0] for row in rows}) != len(rows):
        raise ValueError("duplicate surface address")
    sections = {row[0]: row[1] for row in rows}
    if any(sections.get(va) != "XGRPH" or va not in mapping for va in selected):
        raise ValueError("original entries must be normal XGRPH manual members")
    return sorted(selected), mapping


def vendor_api(vendor: Path) -> tuple[Any, Any]:
    """Import the vendored public API under its own package, without tools clashes."""
    package = "_tsfp_original_vendor"
    cached = sys.modules.get(package)
    vendor_tools = (vendor / "tools").resolve()
    if cached is not None:
        if [Path(path).resolve() for path in cached.__path__] != [vendor_tools]:
            raise ValueError("translation API cache belongs to a different vendor tree")
        # Reload same-tree implementation rather than retaining an earlier body.
        for name in list(sys.modules):
            if name == package or name.startswith(package + "."):
                del sys.modules[name]
    spec = importlib.util.spec_from_file_location(
        package, vendor / "tools/__init__.py", submodule_search_locations=[str(vendor / "tools")]
    )
    if spec is None or spec.loader is None:
        raise ValueError("vendored translation API unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[package] = module
    spec.loader.exec_module(module)
    config = importlib.import_module(package + ".recomp.config")
    translator = importlib.import_module(package + ".recomp.translator")
    return config, translator.BatchTranslator


def alias_body(code: str, va: int, name: str) -> str:
    definition = rf"\bvoid {re.escape(name)}\(void\)"
    if (
        len(re.findall(definition, code)) != 1
        or "UNIMPLEMENTED" in code
        or "/* unimplemented" in code.lower()
    ):
        raise ValueError(f"unsupported or ambiguous translated body {va:#x}")
    return re.sub(definition, f"void recomp_original_{va:08X}(void)", code, count=1)


def input_paths(
    *, entries: Path, manual: Path, surface: Path, lifted: Path, vendor: Path
) -> dict[str, Path]:
    """The files whose hashes the manifest records under ``inputs``."""
    paths = retained_original.input_paths(
        entries=entries, manual=manual, surface=surface, lifted=lifted, vendor=vendor
    )
    profile = strict_json(entries)
    if isinstance(profile, dict) and profile.get("profile") in {
        "shader-assembler-v4",
        "shader-assembler-v5",
        "shader-assembler-v6",
        "shader-assembler-v7",
        "shader-assembler-v8",
    }:
        # The original entry tail-calls these existing compiled bodies. Freeze the
        # coupling as well as its metadata/template; a later helper edit is stale.
        helpers = (0x003EFFB7, 0x003F0F80)
        if profile["profile"] in {
            "shader-assembler-v5",
            "shader-assembler-v6",
            "shader-assembler-v7",
            "shader-assembler-v8",
        }:
            from tools.prepare_xgrph_membership import RECEIPT, verify

            if profile["profile"] == "shader-assembler-v8":
                from tools.prepare_xgrph_recursive import RECEIPT as RECURSIVE_RECEIPT
                from tools.prepare_xgrph_recursive import verify as verify_recursive
                from tools.prepare_xgrph_variants import RECEIPT as VARIANT_RECEIPT
                from tools.prepare_xgrph_variants import verify as verify_variants

                variant_receipt = verify_variants(lifted)
                recursive_parent = Path(variant_receipt["parent"])
                recursive_receipt = verify_recursive(recursive_parent)
                paths["cleanup_variants_receipt"] = lifted / VARIANT_RECEIPT
                paths["recursive_seed_receipt"] = recursive_parent / RECURSIVE_RECEIPT
                paths["membership_seed_receipt"] = Path(recursive_receipt["parent"]) / RECEIPT
                helpers += RECURSIVE_HELPERS + VARIANT_HELPERS
            elif profile["profile"] == "shader-assembler-v7":
                from tools.prepare_xgrph_recursive import (
                    RECEIPT as CHILD_RECEIPT,
                )
                from tools.prepare_xgrph_recursive import (
                    verify as verify_child,
                )

                receipt = verify_child(lifted)
                paths["recursive_seed_receipt"] = lifted / CHILD_RECEIPT
                paths["membership_seed_receipt"] = Path(receipt["parent"]) / RECEIPT
                helpers += RECURSIVE_HELPERS
            else:
                verify(lifted)
                paths["membership_seed_receipt"] = lifted / RECEIPT
            helpers += MEMBERSHIP_HELPERS
        if profile["profile"] in {
            "shader-assembler-v6",
            "shader-assembler-v7",
            "shader-assembler-v8",
        }:
            helpers += ARRAY_TEARDOWN_HELPERS
        found: dict[int, list[Path]] = {helper: [] for helper in helpers}
        for chunk in sorted((lifted / "gen").glob("recomp_[0-9]*.c")):
            text = chunk.read_text()
            for helper in helpers:
                matches = re.findall(rf"^void sub_{helper:08X}\(void\)\s*\{{", text, re.M)
                found[helper].extend([chunk] * len(matches))
        for helper in helpers:
            if len(found[helper]) != 1:
                raise ValueError(f"profile requires one compiled original helper {helper:#x}")
            paths[f"closure_{helper:08X}"] = found[helper][0]
    return paths


def vendor_source_hashes(vendor: Path) -> dict[str, str]:
    """Hash of every vendored translator source, keyed by its vendor-relative path."""
    return retained_original.vendor_source_hashes(vendor, hash_source=digest)


def generate(
    *, xbe: Path, lifted: Path, manual: Path, entries: Path, surface: Path, vendor: Path, out: Path
) -> dict[str, Any]:
    profile_data = strict_json(entries)
    profile_name = profile_data.get("profile") if isinstance(profile_data, dict) else None

    def profile_for(selected: list[int]) -> retained_original.Profile:
        selected_set = set(selected)
        if selected_set == REQUIRED:
            version = 1
        elif selected_set == REQUIRED_V2:
            version = 2
        elif selected_set == REQUIRED_V3:
            version = 3
        elif selected_set == REQUIRED_V4:
            version = 4
        elif selected_set == REQUIRED_V5:
            version = 5
        elif selected_set == REQUIRED_V6:
            version = 6
        elif selected_set == REQUIRED_V7:
            version = 7
        elif selected_set == REQUIRED_V8:
            version = 8
        else:
            raise ValueError("unsupported complete original XDK profile")
        return retained_original.Profile(
            "XGRPH",
            "recomp_original_{:08X}",
            "recomp_lookup_original",
            f"shader-assembler-v{version}",
            "recomp_original_profile_identity",
            version,
        )

    text, manifest = retained_original.translate(
        xbe=xbe,
        lifted=lifted,
        manual=manual,
        entries=entries,
        surface=surface,
        vendor=vendor,
        input_loader=load_inputs,
        input_builder=input_paths,
        vendor_hasher=vendor_source_hashes,
        profile_for=profile_for,
        image_loader=Image.load,
        vendor_loader=vendor_api,
        module_importer=importlib.import_module,
        alias_body=alias_body,
        translation_aliases=(
            ARRAY_TEARDOWN_TRANSLATION
            if profile_name in {"shader-assembler-v6", "shader-assembler-v7", "shader-assembler-v8"}
            else None
        ),
    )
    if profile_name == "shader-assembler-v8":
        from tools.prepare_xgrph_variants import verify as verify_variants

        verify_variants(lifted)
    elif profile_name == "shader-assembler-v7":
        from tools.prepare_xgrph_recursive import verify as verify_child

        verify_child(lifted)
    elif profile_name in {"shader-assembler-v5", "shader-assembler-v6"}:
        from tools.prepare_xgrph_membership import verify

        verify(lifted)
    # Validation completes before either output is published. Each replace is
    # atomic separately; the two files are not a transaction across I/O failures.
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as temp:
        stage = Path(temp)
        (stage / "recomp_xdk_original.c").write_text(text)
        (stage / "original_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        for name in ("recomp_xdk_original.c", "original_manifest.json"):
            os.replace(stage / name, out / name)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--lifted-dir", type=Path, required=True)
    parser.add_argument("--manual-functions", type=Path, required=True)
    parser.add_argument("--original-entries", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--surface", type=Path, default=ROOT / "src/xbox/xdk_surface.c")
    parser.add_argument("--vendor-root", type=Path, default=ROOT / "third_party/xboxrecomp")
    args = parser.parse_args()
    try:
        generate(
            xbe=args.xbe,
            lifted=args.lifted_dir,
            manual=args.manual_functions,
            entries=args.original_entries,
            surface=args.surface,
            vendor=args.vendor_root,
            out=args.out_dir,
        )
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"original bodies: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
