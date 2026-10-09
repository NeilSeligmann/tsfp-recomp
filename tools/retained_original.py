# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared retained-body translation; wrappers own profile validation and publication."""

from __future__ import annotations

import hashlib
import re
import struct
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Profile:
    section: str
    symbol: str
    lookup: str
    identity: str
    identity_function: str
    version: int | None = None


def input_paths(
    *, entries: Path, manual: Path, surface: Path, lifted: Path, vendor: Path
) -> dict[str, Path]:
    """The files whose hashes the manifest records under ``inputs``."""
    return {
        "entries": entries,
        "manual": manual,
        "surface": surface,
        "functions": lifted / "disasm/functions.json",
        "labels": lifted / "disasm/labels.json",
        "identified": lifted / "func_id/identified_functions.json",
        "abi": lifted / "abi/abi_functions.json",
        "template": vendor / "templates/runtime/recomp_types.h",
        "compiled_manual": lifted / "gen/recomp_xdk_manual.c",
        "compiled_template": lifted / "gen/recomp_types.h",
        "vendor_manifest": vendor / "VENDOR.json",
    }


def vendor_source_hashes(
    vendor: Path, *, hash_source: Callable[[bytes], str] = digest
) -> dict[str, str]:
    """Hash of every vendored translator source, keyed by its vendor-relative path."""
    return {
        str(path.relative_to(vendor)): hash_source(path.read_bytes())
        for path in sorted((vendor / "tools").rglob("*.py"))
    }


def translate(
    *,
    xbe: Path,
    lifted: Path,
    manual: Path,
    entries: Path,
    surface: Path,
    vendor: Path,
    input_loader: Callable[..., Any],
    input_builder: Callable[..., dict[str, Path]],
    vendor_hasher: Callable[[Path], dict[str, str]],
    profile_for: Callable[[list[int]], Profile],
    image_loader: Callable[..., Any],
    vendor_loader: Callable[..., Any],
    module_importer: Callable[..., Any],
    alias_body: Callable[..., str],
    translation_aliases: dict[int, int] | None = None,
) -> tuple[str, dict[str, Any]]:
    inputs = input_builder(
        entries=entries, manual=manual, surface=surface, lifted=lifted, vendor=vendor
    )
    hashes = {key: digest(path.read_bytes()) for key, path in inputs.items()}
    vendor_files = sorted((vendor / "tools").rglob("*.py"))
    if not vendor_files:
        raise ValueError("vendor implementation sources missing")
    vendor_hashes = vendor_hasher(vendor)
    xbe_hash = digest(xbe.read_bytes())
    selected, mapping = input_loader(entries, manual, surface)
    profile = profile_for(selected)
    compiled_names = {
        int(va, 16): f"sub_{va}"
        for va in re.findall(
            r"\bvoid sub_([0-9A-F]{8})\(void\)\s*\{", inputs["compiled_manual"].read_text()
        )
    }
    if compiled_names != mapping:
        raise ValueError("manual map differs from compiled manual definitions")
    if inputs["template"].read_bytes() != inputs["compiled_template"].read_bytes():
        raise ValueError("compiled runtime template differs from translation template")
    image = image_loader(xbe)
    if digest(image.raw) != xbe_hash:
        raise ValueError("XBE changed while creating image snapshot")
    translation_aliases = {} if translation_aliases is None else dict(translation_aliases)
    if not set(translation_aliases).issubset(selected):
        raise ValueError("translation aliases must name selected original entries")
    alias_bytes: dict[int, bytes] = {}
    for alias, target in translation_aliases.items():
        displacement = target - (alias + 5)
        if not -(1 << 31) <= displacement < (1 << 31):
            raise ValueError(f"translation alias target out of rel32 range {alias:#x}")
        raw_alias = image.read(alias, 5)
        expected = b"\xe9" + struct.pack("<i", displacement)
        if raw_alias != expected:
            raise ValueError(f"translation alias bytes do not match {alias:#x}->{target:#x}")
        alias_bytes[alias] = raw_alias
    config, cls = vendor_loader(vendor)
    # configure_from_xbe's public implementation imports its parser by the
    # upstream absolute tools name; scope that alias to this call only.
    parser_name = "tools.xbe_parser.xbe_parser"
    parser_package = "tools.xbe_parser"
    old_modules = {key: sys.modules.get(key) for key in (parser_package, parser_name)}
    try:
        for key, suffix in (
            (parser_package, ".xbe_parser"),
            (parser_name, ".xbe_parser.xbe_parser"),
        ):
            sys.modules[key] = module_importer("_tsfp_original_vendor" + suffix)
        config.configure_from_xbe(str(xbe))
    finally:
        for key, old in old_modules.items():
            if old is None:
                sys.modules.pop(key, None)
            else:
                sys.modules[key] = old
    batch = cls(
        xbe_path=str(xbe),
        func_json_path=str(inputs["functions"]),
        labels_json_path=str(inputs["labels"]),
        identified_json_path=str(inputs["identified"]),
        abi_json_path=str(inputs["abi"]),
        protected_function_starts=set(mapping),
    )
    if digest(batch.xbe_data) != xbe_hash:
        raise ValueError("XBE changed while loading translator")
    for va, name in mapping.items():
        if va in batch.func_db:
            batch.func_db[va]["name"] = name
    bodies = []
    records = []
    for va in selected:
        source_va = translation_aliases.get(va, va)
        info = batch.func_db.get(source_va)
        if (
            not info
            or source_va in batch.translator.owned_function_starts
            or info.get("section") != profile.section
        ):
            raise ValueError(f"missing/owned/non-{profile.section} original function {va:#x}")
        end = info["end"] if isinstance(info["end"], int) else int(info["end"], 16)
        raw = image.read(source_va, end - source_va)
        if not raw or end <= source_va or end - source_va != info["size"]:
            raise ValueError("invalid original body span")
        batch.translator.lifter.manual_functions = set(mapping) - {va, source_va}
        before = {key: len(value) for key, value in batch.translator.lifter.unimplemented.items()}
        code = batch.translate_single(source_va)
        if not code or any(
            len(value) > before.get(key, 0)
            for key, value in batch.translator.lifter.unimplemented.items()
        ):
            raise ValueError(f"unimplemented original instructions {va:#x}")
        source_name = mapping.get(source_va, info.get("name", f"sub_{source_va:08X}"))
        body = alias_body(code, source_va, source_name)
        if source_va != va:
            source_symbol = f"recomp_original_{source_va:08X}"
            alias_symbol = f"recomp_original_{va:08X}"
            if body.count(source_symbol) != 1:
                raise ValueError(
                    f"translated alias body has ambiguous source symbol {source_va:#x}"
                )
            body = body.replace(source_symbol, alias_symbol, 1)
        bodies.append(body)
        record = {
            "address": f"0x{va:08X}",
            "symbol": profile.symbol.format(va),
            "size": len(raw),
            "original_sha256": digest(raw),
            "generated_body_sha256": digest(body.encode()),
        }
        if source_va != va:
            record["translation_source"] = f"0x{source_va:08X}"
            record["entry_sha256"] = digest(alias_bytes[va])
        records.append(record)
    text = '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n\n' + "\n\n".join(bodies)
    text += (
        f"\n\nrecomp_func_t {profile.lookup}(uint32_t address);\n"
        f"recomp_func_t {profile.lookup}(uint32_t address)\n{{\n"
        "    switch (address) {\n"
    )
    text += "".join(
        f"    case 0x{va:08X}u: return {profile.symbol.format(va)};\n" for va in selected
    )
    text += "    default: return NULL;\n    }\n}\n"
    if profile.version is not None:
        text += (
            "uint32_t recomp_original_profile_version(void);\n"
            f"uint32_t recomp_original_profile_version(void) {{ return {profile.version}u; }}\n"
        )
    text += (
        f"const char *{profile.identity_function}(void);\n"
        f'const char *{profile.identity_function}(void) {{ return "{profile.identity}"; }}\n'
    )
    manifest = {
        "schema": 1,
        "profile": profile.identity,
        "xbe_sha256": xbe_hash,
        "vendor_sources": vendor_hashes,
        "inputs": hashes,
        "generated_chunk_sha256": digest(text.encode()),
        "bodies": records,
    }
    if translation_aliases:
        manifest["translation_aliases"] = {
            f"0x{alias:08X}": f"0x{target:08X}"
            for alias, target in sorted(translation_aliases.items())
        }
    if (
        digest(xbe.read_bytes()) != xbe_hash
        or any(digest(path.read_bytes()) != hashes[key] for key, path in inputs.items())
        or sorted((vendor / "tools").rglob("*.py")) != vendor_files
        or any(
            digest(path.read_bytes()) != vendor_hashes[str(path.relative_to(vendor))]
            for path in vendor_files
        )
    ):
        raise ValueError("generation inputs changed during translation")
    return text, manifest
