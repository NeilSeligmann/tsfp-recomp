# SPDX-License-Identifier: GPL-3.0-or-later
"""Distinct optional custom fixture provenance; legacy absent fields stay absent."""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

from . import scoped_fixture_authority as authority
from .effective_index import pin_valid
from .scoped_fixture_code import CUSTOM_GUARD
from .scoped_fixture_code import validate_document as validate_certificate
from .seeding import SeedPolicy

ROW_FIELD = "scoped_fixture_contract"
RUNTIME_FIELD = "scoped_fixture_runtime"
ENTRY_FIELD = "scoped_fixture_entry"
CONTEXT_FIELD = "scoped_fixture_contracts"


def validate_contract(value: object) -> dict[str, object]:
    if type(value) is not dict or set(value) != {"contract", "sha256"}:
        raise ValueError("malformed custom fixture contract")
    body = value["contract"]
    keys = {
        "version",
        "protocol",
        "entry",
        "size",
        "label",
        "namespace",
        "source_kind",
        "source_sha256",
        "physical_sections",
        "regions",
        "seed_policy",
        "factories",
        "dependencies",
        "dependency_directories",
        "streams",
        "certificate",
    }
    prefix = type(body) is dict and body.get("protocol") == "scoped-fixture-prefix-v2"
    if prefix:
        keys = keys | {"known_code"}
    if type(body) is dict and "argument_overlays" in body:
        keys = keys | {"argument_overlays"}
    if (
        type(body) is not dict
        or set(body) != keys
        or value["sha256"] != authority.digest(authority.canonical(body))
    ):
        raise ValueError("changed or incomplete custom fixture contract")
    if (
        type(body["version"]) is not int
        or body["version"] != (2 if prefix else 1)
        or body["protocol"] != ("scoped-fixture-prefix-v2" if prefix else "scoped-fixture-v1")
    ):
        raise ValueError("unknown custom fixture protocol")
    if (
        body["source_kind"] not in ("synthetic", "original-xbe")
        or (body["source_kind"] == "original-xbe" and not pin_valid(body["source_sha256"]))
        or (body["source_kind"] == "synthetic" and body["source_sha256"] is not None)
    ):
        raise ValueError("invalid custom physical source kind/pin")
    validate_certificate(body["certificate"])
    certificate = body["certificate"]["contract"]
    entry = body["entry"]
    if (
        type(entry) is not dict
        or not isinstance(entry.get("va"), str)
        or entry["va"] != f"0x{certificate['root']:08x}"
        or type(body["size"]) is not int
        or not any(
            node["address"] == certificate["root"] and node["size"] == body["size"]
            for node in certificate["nodes"]
        )
        or type(body["namespace"]) is not int
        or not 3 <= body["namespace"] < 1 << 16
        or body["namespace"] == 4
        or type(body["label"]) is not str
        or not body["label"]
        or body["seed_policy"] != asdict(SeedPolicy())
    ):
        raise ValueError("custom root/body/namespace/policy mismatch")
    sections = body["physical_sections"]
    if type(sections) is not list or not sections:
        raise ValueError("missing custom physical sections")
    geometry = []
    for section in sections:
        if (
            type(section) is not dict
            or set(section) != {"address", "size", "executable", "writable"}
            or type(section["address"]) is not int
            or type(section["size"]) is not int
            or not 0 <= section["address"] < section["address"] + section["size"] <= 1 << 32
            or type(section["executable"]) is not bool
            or type(section["writable"]) is not bool
        ):
            raise ValueError("invalid custom physical section")
        if any(
            authority.overlap(
                (section["address"], section["address"] + section["size"]),
                (old[0], old[0] + old[1]),
            )
            for old in geometry
        ):
            raise ValueError("overlapping custom physical sections")
        geometry.append([section[key] for key in ("address", "size", "executable", "writable")])
    if (
        authority.digest(json.dumps(geometry, separators=(",", ":")).encode())
        != certificate["sections_sha256"]
    ):
        raise ValueError("custom physical sections differ from certificate")
    if type(body["regions"]) is not list or not body["regions"]:
        raise ValueError("missing custom physical roles")
    occupied = []
    policy = SeedPolicy()
    protected = [
        (policy.sentinel, policy.sentinel + 4),
        (policy.stack_base, policy.stack_base + 0x7FF0 + 4 + 4 * policy.frame_args),
        (policy.scratch_base, policy.scratch_base + policy.scratch_fill_bytes),
    ]
    protected.extend(
        (node["address"], node["address"] + node["size"]) for node in certificate["nodes"]
    )
    if prefix:
        from .scoped_fixture_prefix import validate_document as validate_prefix

        protected.append(
            validate_prefix(body["known_code"], certificate, sections, body["source_kind"])
        )
    overlays = body.get("argument_overlays", [])
    if type(overlays) is not list or ("argument_overlays" in body and not overlays):
        raise ValueError("invalid custom argument overlay list")
    seen_roles, seen_slots = set(), set()
    for overlay in overlays:
        if type(overlay) is not dict or set(overlay) != {"role", "slot", "abi"}:
            raise ValueError("invalid custom argument overlay shape")
        parsed = authority.ArgumentOverlay(**overlay)
        parsed.validate(policy.frame_args)
        if parsed.role in seen_roles or parsed.slot in seen_slots:
            raise ValueError("duplicate custom argument overlay")
        seen_roles.add(parsed.role)
        seen_slots.add(parsed.slot)
    if any(type(r) is dict and r.get("role") in seen_roles for r in body["regions"]):
        raise ValueError("argument overlay role collides with physical role")
    for region in body["regions"]:
        if type(region) is not dict or set(region) != {"role", "kind", "start", "end", "width"}:
            raise ValueError("invalid custom role shape")
        role = authority.Region(**region)
        role.validate()
        span = role.start, role.end
        if any(authority.overlap(span, old) for old in (*occupied, *protected)):
            raise ValueError("custom role overlaps protected allocation/code")
        occupied.append(span)
        containing = [
            section
            for section in sections
            if section["address"] <= role.start < role.end <= section["address"] + section["size"]
        ]
        if role.kind == "source" and (len(containing) != 1 or not containing[0]["writable"]):
            raise ValueError("custom source role lacks writable physical authority")
        if role.kind == "guest" and any(
            authority.overlap(span, (section["address"], section["address"] + section["size"]))
            for section in sections
        ):
            raise ValueError("custom guest role overlaps physical source")
    # Manifest parsing supplies the same strict legacy/exact ABI metadata checks.
    from tools.replace.manifest import entry_from_json

    if authority.canonical(entry_from_json(entry).as_json()) != authority.canonical(entry):
        raise ValueError("noncanonical custom ABI entry")
    dependencies = body["dependencies"]
    directories = body["dependency_directories"]
    if (
        type(dependencies) is not dict
        or not dependencies
        or any(type(path) is not str or not pin_valid(pin) for path, pin in dependencies.items())
        or type(directories) is not list
        or not directories
        or "tools" not in directories
        or any(type(path) is not str for path in directories)
        or len(set(directories)) != len(directories)
    ):
        raise ValueError("missing custom dependency membership")
    if any(Path(path).is_absolute() or ".." in Path(path).parts for path in directories):
        raise ValueError("custom dependency directory escapes source")
    for path in dependencies:
        if (
            Path(path).is_absolute()
            or ".." in Path(path).parts
            or not path.endswith(".py")
            or not any(Path(path).is_relative_to(Path(directory)) for directory in directories)
        ):
            raise ValueError("custom dependency member escapes selection")
    factories = body["factories"]
    if type(factories) is not list or len(factories) < 2:
        raise ValueError("missing custom count/dispatcher identities")
    identities = set()
    for n, factory in enumerate(factories):
        if (
            type(factory) is not dict
            or set(factory) != {"role", "module", "name", "source", "source_sha256", "code_sha256"}
            or factory["role"] != ("count" if n == 0 else "dispatcher" if n == 1 else "delegate")
            or any(
                type(factory[key]) is not str or not factory[key]
                for key in ("module", "name", "source")
            )
            or dependencies.get(factory["source"]) != factory["source_sha256"]
            or not pin_valid(factory["source_sha256"])
            or not pin_valid(factory["code_sha256"])
        ):
            raise ValueError("invalid custom loaded factory identity")
        identity = factory["module"], factory["name"]
        if identity in identities:
            raise ValueError("duplicate custom factory identity")
        identities.add(identity)
    streams = body["streams"]
    if type(streams) is not list or not streams:
        raise ValueError("missing complete custom streams")
    seeds, counts = set(), set()
    for stream in streams:
        if (
            type(stream) is not dict
            or set(stream) != {"seed", "root", "namespace", "count", "digest"}
            or type(stream["seed"]) is not int
            or stream["seed"] in seeds
            or stream["root"] != certificate["root"]
            or stream["namespace"] != body["namespace"]
            or type(stream["count"]) is not int
            or not 0 < stream["count"] < 1 << 40
            or not pin_valid(stream["digest"])
        ):
            raise ValueError("invalid complete custom stream")
        seeds.add(stream["seed"])
        counts.add(stream["count"])
    if len(counts) != 1:
        raise ValueError("seed-dependent custom count unsupported")
    return body


def attach_context(document: dict[str, object]) -> None:
    contracts = {row["va"]: row[ROW_FIELD] for row in document["functions"] if ROW_FIELD in row}
    if contracts:
        document[CONTEXT_FIELD] = contracts


def validate_document(document: dict[str, object]) -> None:
    context = document.get("harness", document)
    rows = document.get("functions", [])
    actual = {}
    for row in rows:
        has_contract, has_runtime = ROW_FIELD in row, RUNTIME_FIELD in row
        if has_contract != has_runtime or (ENTRY_FIELD in row) != has_contract:
            raise ValueError("orphaned custom fixture contract/runtime")
        if not has_contract:
            continue
        if "named_global_object_contract" in row or "named_global_code_safety" in row:
            raise ValueError("mixed named/custom fixture authorities")
        if row["va"] in actual:
            raise ValueError("duplicate custom fixture root")
        body = validate_contract(row[ROW_FIELD])
        if row["va"] != body["entry"]["va"]:
            raise ValueError("custom fixture provenance belongs to another root")
        from tools.replace.input_abi import input_contract
        from tools.replace.manifest import entry_from_json

        entry = entry_from_json(body["entry"])
        expected_input = input_contract(entry.register_inputs, entry.convention, entry.stack_args)
        try:
            serialized_entry = row[ENTRY_FIELD]
            if type(serialized_entry) is not dict:
                raise ValueError("actual custom entry must be a JSON object")
            parsed_entry = entry_from_json(serialized_entry).as_json()
            canonical_entry = authority.canonical(serialized_entry)
            if authority.canonical(
                parsed_entry
            ) != canonical_entry or canonical_entry != authority.canonical(body["entry"]):
                raise ValueError("noncanonical or changed custom entry")
        except (ValueError, TypeError, KeyError) as error:
            raise ValueError("custom fixture actual replacement ABI/symbol differs") from error
        if (
            row.get("name") != entry.name
            or row.get("regs_ignored") != sorted(entry.scratch)
            or row.get("input_contract") != expected_input
        ):
            raise ValueError("custom fixture actual replacement ABI/symbol differs")
        actual[row["va"]] = row[ROW_FIELD]
    if not actual and CONTEXT_FIELD not in context:
        return
    if context.get(CONTEXT_FIELD) != actual or not actual:
        raise ValueError("missing/mismatched/orphan custom fixture root map")
    # No fabricated true marker or old named capability ratifies this protocol.
    if not authority.CUSTOM_RUNTIME_VALIDATED:
        raise ValueError("unvalidated local custom fixture runtime capability")
    for row in rows:
        if ROW_FIELD not in row:
            continue
        runtime = row[RUNTIME_FIELD]
        body = row[ROW_FIELD]["contract"]
        keys = {
            "protocol",
            "contract_sha256",
            "certificate_sha256",
            "seed",
            "checked_cases",
            "by_kind",
            "returned",
            "faulted",
            "case_observations_sha256",
            "validated",
        }
        seed = context.get("seed")
        if row.get("source_run") is not None:
            runs = [
                run
                for run in context.get("merged_runs", [])
                if run.get("label") == row["source_run"]
            ]
            if len(runs) != 1:
                raise ValueError("missing/duplicate custom source run")
            seed = runs[0].get("seed")
        if (
            type(runtime) is not dict
            or set(runtime) != keys
            or runtime["protocol"] != CUSTOM_GUARD
            or runtime["contract_sha256"] != row[ROW_FIELD]["sha256"]
            or runtime["certificate_sha256"] != body["certificate"]["sha256"]
            or type(seed) is not int
            or runtime["seed"] != seed
            or type(runtime["seed"]) is not int
            or runtime["validated"] is not True
            or any(
                type(runtime[key]) is not int or runtime[key] < 0
                for key in ("checked_cases", "returned", "faulted")
            )
            or runtime["checked_cases"] <= 0
            or runtime["checked_cases"] != row.get("cases")
            or runtime["returned"] + runtime["faulted"] != runtime["checked_cases"]
            or not pin_valid(runtime["case_observations_sha256"])
            or seed not in [stream["seed"] for stream in body["streams"]]
        ):
            raise ValueError("missing/mismatched/incomplete custom runtime provenance")

        by_kind = runtime["by_kind"]
        expected = next(stream for stream in body["streams"] if stream["seed"] == seed)
        if (
            type(by_kind) is not dict
            or set(by_kind) - {"random", "edge", "feedback", body["label"]}
            or any(type(count) is not int or count < 0 for count in by_kind.values())
            or sum(by_kind.values()) != runtime["checked_cases"]
            or by_kind.get(body["label"]) != expected["count"]
        ):
            raise ValueError("incomplete/unknown custom ordinary/provider stream counts")

        if any(
            type(row.get(field)) is not int or row[field] < 0 or by_kind.get(kind, 0) != row[field]
            for kind, field in (
                ("random", "random_cases"),
                ("edge", "edge_cases"),
                ("feedback", "feedback_cases"),
            )
        ):
            raise ValueError("custom runtime phase counts differ from actual row")
