# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded OGhidra reads on a temporary project copy, verified against an XBE.

Run with tmp/oghidra/.venv/bin/python after tools/ghidra/oghidra_setup.sh.
Raw results must stay in generated/; see docs/ghidra-analysis-pilot.md.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.metadata
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from tools.ghidra.run import GHIDRA_VERSION, ghidra_dir
from tools.xbe import XbeSection, parse_xbe

UPSTREAM_REVISION = "41829c6b2b25a8a652147f50288f0d9b3721562f"
REPO_ROOT = Path(__file__).resolve().parents[2]
PAGE_SIZE = 500
MAX_LINES = 100_000
MAX_BODY_BYTES = 1_048_576


class QueryError(RuntimeError):
    """Evidence could not be obtained or verified."""


def checked_text(value: Any) -> str:
    text = value if isinstance(value, str) else "\n".join(value)
    if not text.strip() or any(
        line.startswith(("Error:", "Request failed:")) for line in text.splitlines()
    ):
        raise QueryError(f"backend returned unavailable evidence: {text[:300]}")
    return text


def read_pages(method: Any, address: str, *, text: bool = False) -> str | list[str]:
    """Require every advertised item; never treat a partial page as full evidence."""
    output: list[str] = []
    offset = 0
    total: int | None = None
    while total is None or offset < total:
        page = checked_text(method(address, offset=offset, limit=PAGE_SIZE)).splitlines()
        pattern = r"\[Total Lines: (\d+)\]" if text else r"\[Total: (\d+)\]"
        match = re.match(pattern, page[0])
        if match is None:
            raise QueryError(f"missing pagination metadata: {page[0]}")
        page_total = int(match[1])
        if page_total > MAX_LINES or (total is not None and page_total != total):
            raise QueryError("pagination total changed or exceeded query bound")
        total = page_total
        content = page[1:]
        if text and content and content[-1].startswith("... [Next:"):
            content.pop()
        expected = min(PAGE_SIZE, total - offset)
        if len(content) != expected:
            raise QueryError(f"incomplete page at {offset}: {len(content)}/{expected}")
        output.extend(content)
        offset += len(content)
        if total == 0:
            break
    if text and not output:
        raise QueryError("empty decompilation")
    return "\n".join(output) if text else output


def image_bytes(raw: bytes, sections: list[XbeSection], address: int, size: int) -> bytes:
    for section in sections:
        delta = address - section.virtual_addr
        if 0 <= delta and delta + size <= section.raw_size:
            start = section.raw_addr + delta
            result = raw[start : start + size]
            if len(result) == size:
                return result
    raise QueryError(f"range {address:#x}+{size:#x} isn't backed by original image bytes")


def canonical_opcode(opcode: str) -> str:
    opcode = opcode.lower()
    if " " in opcode:
        return " ".join(canonical_opcode(part) for part in opcode.split())
    if "." in opcode:
        operation, prefix = opcode.split(".", 1)
        if prefix in {"rep", "repe", "repz", "repne", "repnz"}:
            return f"{canonical_opcode(prefix)} {canonical_opcode(operation)}"
    conditions = {
        "z": "e",
        "nz": "ne",
        "c": "b",
        "nae": "b",
        "nc": "ae",
        "nb": "ae",
        "na": "be",
        "nbe": "a",
        "pe": "p",
        "po": "np",
        "nge": "l",
        "nl": "ge",
        "ng": "le",
        "nle": "g",
    }
    for prefix in ("cmov", "set", "j"):
        if opcode.startswith(prefix):
            return prefix + conditions.get(opcode[len(prefix) :], opcode[len(prefix) :])
    return {"sal": "shl", "repz": "repe", "repnz": "repne"}.get(opcode, opcode)


def inspect_function(
    client: Any, address: int, raw: bytes, sections: list[XbeSection], *, decompile: bool = True
) -> dict:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    identifier = f"{address:08x}"
    info = checked_text(client.get_function_by_address(identifier))
    entry_match = re.search(r"^Entry: ([0-9a-fA-F]+)$", info, re.MULTILINE)
    body_match = re.search(r"^Body: ([0-9a-fA-F]+) - ([0-9a-fA-F]+)$", info, re.MULTILINE)
    if entry_match is None or body_match is None:
        raise QueryError(f"unrecognized function metadata: {info}")
    entry = int(entry_match[1], 16)
    start, end = int(body_match[1], 16), int(body_match[2], 16)
    size = end - start + 1
    if not start <= entry <= end or not 1 <= size <= MAX_BODY_BYTES:
        raise QueryError("invalid or oversized function body")
    expected = image_bytes(raw, sections, start, size)
    actual = bytearray()
    for offset in range(0, size, 4096):
        length = min(4096, size - offset)
        encoded = checked_text(client.read_bytes(f"{start + offset:08x}", length, format="raw"))
        try:
            block = base64.b64decode(encoded, validate=True)
        except ValueError as exc:
            raise QueryError("invalid raw-byte response") from exc
        if len(block) != length:
            raise QueryError(f"short raw-byte response: {len(block)}/{length}")
        actual.extend(block)
    if actual != expected:
        raise QueryError(f"Ghidra bytes differ from the supplied XBE at {start:#010x}")

    disassembly = checked_text(client.disassemble_function(identifier)).splitlines()
    engine = Cs(CS_ARCH_X86, CS_MODE_32)
    instructions: list[dict] = []
    calls: list[dict] = []
    for line in disassembly:
        match = re.match(r"([0-9a-fA-F]+): (.+)", line)
        if match is None:
            raise QueryError(f"unrecognized disassembly: {line}")
        pc = int(match[1], 16)
        if not start <= pc <= end:
            raise QueryError("disassembly escapes reported body")
        decoded = list(engine.disasm(expected[pc - start : pc - start + 15], pc, count=1))
        if not decoded:
            raise QueryError(f"original bytes do not decode at {pc:#x}")
        instruction = decoded[0]
        # Different operand spellings are fine; a different opcode is not.
        ghidra_opcode = match[2].split()[0].lower()
        original_opcode = instruction.mnemonic
        if canonical_opcode(ghidra_opcode) != canonical_opcode(original_opcode):
            raise QueryError(f"opcode mismatch at {pc:#x}: {line} / {instruction.mnemonic}")
        row = {
            "address": f"0x{pc:08x}",
            "bytes": instruction.bytes.hex(),
            "original": f"{instruction.mnemonic} {instruction.op_str}".strip(),
            "ghidra": match[2],
        }
        instructions.append(row)
        if instruction.mnemonic == "call" and re.fullmatch(r"0x[0-9a-f]+", instruction.op_str):
            calls.append({"site": f"0x{pc:08x}", "target": instruction.op_str})
    addresses = [int(row["address"], 16) for row in instructions]
    if addresses != client.instruction_addresses(identifier):
        raise QueryError("upstream disassembly omitted, reordered or duplicated instructions")
    return {
        "requested_address": f"0x{address:08x}",
        "entry": f"0x{entry:08x}",
        "is_entry": entry == address,
        "function": info,
        "body_sha256": hashlib.sha256(expected).hexdigest(),
        "verified_bytes": size,
        "decompilation": (
            read_pages(client.decompile_function_by_address, identifier, text=True)
            if decompile
            else None
        ),
        "decompilation_status": "complete" if decompile else "not_requested",
        "instructions": instructions,
        "direct_calls": calls,
        "xrefs_to": read_pages(client.get_xrefs_to, identifier),
    }


def project_digest(project: Path) -> str:
    """Hash project contents, including relative names, before and after access."""
    digest = hashlib.sha256()
    paths = [project, *sorted(project.with_suffix(".rep").rglob("*"))]
    for path in paths:
        if path.is_file():
            digest.update(str(path.relative_to(project.parent)).encode())
            digest.update(b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def validate_upstream(checkout: Path) -> None:
    revision = subprocess.check_output(
        ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
    ).strip()
    dirty = subprocess.check_output(
        ["git", "-C", str(checkout), "status", "--porcelain", "--untracked-files=no"], text=True
    ).strip()
    if revision != UPSTREAM_REVISION or dirty:
        raise QueryError(f"requires clean OGhidra revision {UPSTREAM_REVISION}")


def validate_output(output: Path) -> Path:
    resolved = output.resolve()
    if not resolved.is_relative_to((REPO_ROOT / "generated").resolve()):
        raise QueryError("raw analysis output must be beneath this repository's generated/")
    ignored = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "check-ignore", "-q", str(resolved)], check=False
    )
    if ignored.returncode != 0:
        raise QueryError("raw analysis output must be gitignored")
    return resolved


def query(args: argparse.Namespace) -> dict:
    output = validate_output(args.out)
    validate_upstream(args.upstream)
    install = ghidra_dir(args.ghidra).resolve()
    properties = (install / "Ghidra/application.properties").read_text()
    version = re.search(r"^application.version=(.*)$", properties, re.MULTILINE)
    if version is None or version[1] != GHIDRA_VERSION:
        raise QueryError(f"requires Ghidra {GHIDRA_VERSION}")
    if importlib.metadata.version("pyghidra") != "3.1.0":
        raise QueryError("requires the pilot's tested pyghidra 3.1.0")
    project = args.project.resolve()
    if (
        project.suffix != ".gpr"
        or not project.is_file()
        or not project.with_suffix(".rep").is_dir()
    ):
        raise QueryError("--project must be an existing .gpr with its sibling .rep directory")
    for suffix in (".lock", ".lock~"):
        if project.with_suffix(suffix).exists():
            raise QueryError("source project is locked; close its Ghidra session before copying")
    raw = args.xbe.read_bytes()
    image = parse_xbe(raw)
    source_digest = project_digest(project)
    output.parent.mkdir(parents=True, exist_ok=True)
    os.environ["GHIDRA_INSTALL_DIR"] = str(install)
    sys.path.insert(0, str(args.upstream.resolve()))
    from src.config import GhidraMCPConfig

    from tools.ghidra.oghidra_backend import ReadClient

    begun = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="project-copy-", dir=output.parent) as temp:
        copy = Path(temp) / project.name
        shutil.copy2(project, copy)
        shutil.copytree(project.with_suffix(".rep"), copy.with_suffix(".rep"))
        if project_digest(copy) != source_digest or project_digest(project) != source_digest:
            raise QueryError("source project changed while copying")
        client = ReadClient(
            GhidraMCPConfig(
                backend="pyghidra", pyghidra_project_path=str(copy), pyghidra_program=args.program
            )
        )
        ready = time.monotonic()
        try:
            if not client.health_check():
                raise QueryError("OGhidra program health check failed")
            if client.executable_sha256() != hashlib.sha256(raw).hexdigest():
                raise QueryError("Ghidra executable identity differs from the supplied XBE")
            functions = [
                inspect_function(
                    client, a, raw, image.sections, decompile=not args.disassembly_only
                )
                for a in args.address
            ]
            references = {
                f"0x{a:08x}": read_pages(client.get_xrefs_to, f"{a:08x}") for a in args.xref
            }
            result = {
                "schema": 1,
                "upstream_revision": UPSTREAM_REVISION,
                "ghidra_version": GHIDRA_VERSION,
                "pyghidra_version": "3.1.0",
                "dependency_versions": {
                    name: importlib.metadata.version(name)
                    for name in ("JPype1", "capstone", "pydantic", "pydantic-settings")
                },
                "python_version": sys.version,
                "source_project": str(project),
                "source_project_sha256": source_digest,
                "xbe_sha256": hashlib.sha256(raw).hexdigest(),
                "program": client.get_current_program_info(),
                "functions": functions,
                "references": references,
                "startup_and_copy_seconds": ready - begun,
                "query_seconds": time.monotonic() - ready,
                "byte_adapter": "explicit Java byte[]; upstream bytearray copyback is defective",
            }
        finally:
            client.close()
    if project_digest(project) != source_digest:
        raise QueryError("source project changed during the query")
    result["source_project_unchanged"] = True
    # Publish only a complete, verified result; leave an earlier report intact on failure.
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False) as pending:
        json.dump(result, pending, indent=2)
        pending.write("\n")
    Path(pending.name).replace(output)
    return result


def address(value: str) -> int:
    try:
        result = int(value, 16)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use a hexadecimal guest address") from exc
    if not 0 <= result <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("guest address must fit 32 bits")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--program", default="/default.xbe")
    parser.add_argument("--xbe", required=True, type=Path)
    parser.add_argument("--ghidra", type=Path)
    parser.add_argument("--upstream", type=Path, default=REPO_ROOT / "tmp/oghidra")
    parser.add_argument("--address", action="append", type=address, default=[])
    parser.add_argument("--xref", action="append", type=address, default=[])
    parser.add_argument(
        "--disassembly-only",
        action="store_true",
        help="explicitly omit decompilation for difficult functions",
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    if not args.address and not args.xref:
        parser.error("supply at least one --address or --xref")
    try:
        result = query(args)
    except (QueryError, OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"query failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"Verified {len(result['functions'])} functions; source project unchanged; wrote {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
