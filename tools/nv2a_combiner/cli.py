# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for the combiner translator. Counts and causes only, never shader text.

    python -m tools.nv2a_combiner.cli probe XBE
    python -m tools.nv2a_combiner.cli corpus XBE --validate 500
    python -m tools.nv2a_combiner.cli random --configs 300 --fragments 200
    python -m tools.nv2a_combiner.cli translate BLOCK_FILE

The addresses default to the retail build measured in `docs/shader-inputs.md` section 10.
They belong to that build and are arguments for any other.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from collections import Counter
from collections.abc import Sequence
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import corpus, glsl, precision, probe, randomcfg, validate

DEFAULT_OUT = Path("generated/shaders/combiner")


def _hex(text: str) -> int:
    return int(text, 0)


def default_addresses() -> corpus.Addresses:
    """The retail build of `docs/shader-inputs.md` section 10. Arguments override each."""
    return corpus.Addresses(
        assembler=0x3EE2B3,
        heap_alloc=0x383678,
        heap_free=0x383DF3,
        setter=0x22720,
        builder=0x20CC0,
        builder_register="edx",
        builder_mask=0xC83FE804,
        key_target=0x18EA0,
        key_argument=3,
        key_global=0x4B85E4,
        wrapper_literals=((0x1CD90, "esi", 0x1CFE2), (0x1D0B0, "esi", 0x1D2D0)),
    )


def _add_addresses(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("xbe", type=Path)
    parser.add_argument("--assembler", type=_hex, default=default_addresses().assembler)
    parser.add_argument("--heap-alloc", type=_hex, default=default_addresses().heap_alloc)
    parser.add_argument("--heap-free", type=_hex, default=default_addresses().heap_free)


def cmd_probe(args: argparse.Namespace) -> int:
    oracle = probe.Oracle.load(args.xbe, args.assembler, args.heap_alloc, args.heap_free)
    failures = 0
    for fact in probe.FACTS:
        agrees, seen, expected = probe.check_fact(oracle, fact)
        if not agrees:
            failures += 1
            print(
                f"FACT   {fact.block}[{fact.index}] {fact.field_name}={fact.value_name}: "
                f"assembler bits read {seen}, documents expect {expected}"
            )
    print(f"facts: {len(probe.FACTS) - failures} of {len(probe.FACTS)} agree with the documents")
    constructs = probe.CONSTRUCTS + probe.FINAL_CONSTRUCTS
    bad = 0
    for construct in constructs:
        assembled, error = probe.check_construct(oracle, construct)
        if not assembled or error > 1e-9:
            bad += 1
            print(f"SEMANTICS {construct.name}: assembled={assembled} worst error {error:.3g}")
    print(f"semantics: {len(constructs) - bad} of {len(constructs)} constructs match Direct3D")
    return 1 if failures or bad else 0


def cmd_corpus(args: argparse.Namespace) -> int:
    addresses = corpus.Addresses(
        assembler=args.assembler,
        heap_alloc=args.heap_alloc,
        heap_free=args.heap_free,
        setter=args.setter,
        builder=args.builder,
        builder_register=args.builder_register,
        builder_mask=args.builder_mask,
        key_target=args.key_target,
        key_argument=args.key_argument,
        key_global=args.key_global,
        wrapper_literals=default_addresses().wrapper_literals,
    )
    from tools.shaderscan.image import Image

    image = Image.load(args.xbe)
    static = corpus.static_definitions(image, addresses.setter)
    generated = corpus.generated_definitions(args.xbe, image, addresses)
    blocks = {f"s_{address:x}": ("static", block) for address, block in static.items()}
    blocks |= {
        f"g_{digest[:12]}": ("generated", block) for digest, block in generated.blocks.items()
    }
    report: dict[str, object] = {}

    static_distinct = len(set(static.values()))
    generated_distinct = len(set(generated.blocks.values()))
    union = {block for _, block in blocks.values()}
    print(f"static: {len(static)} addresses, {static_distinct} distinct by content")
    print(
        f"generated: {len(generated.blocks)} sources assembled, {generated.rejected} rejected, "
        f"{generated.unfinished} unfinished, "
        f"{generated_distinct} distinct blocks, {len(generated.reachable)} in the reachable "
        f"estimate"
    )
    reachable_blocks = {generated.blocks[d] for d in generated.reachable}
    shared = set(static.values()) & set(generated.blocks.values())
    print(
        f"distinct by content: bound {static_distinct} + {generated_distinct} - {len(shared)} "
        f"shared = {len(union)}, estimate {static_distinct} + {len(reachable_blocks)} = "
        f"{len(set(static.values()) | reachable_blocks)}"
    )

    results = corpus.translate_blocks(blocks, args.out / "frag")
    outcomes = corpus.summarise(results)
    print(f"translation: {dict(outcomes)} of {len(results)} blocks")
    by_origin = Counter((entry.origin, entry.cause or "translated") for entry in results)
    for key, count in sorted(by_origin.items()):
        print(f"  {key[0]:9s} {key[1]:14s} {count}")
    configs = {entry.name: entry.config for entry in results if entry.config is not None}
    structures = {
        corpus.structure_key(block) for name, (_, block) in blocks.items() if name in configs
    }
    texts = {corpus.glsl_text_key(config) for config in configs.values()}
    print(
        f"translated blocks: {len(configs)}; distinct by structure (constants masked) "
        f"{len(structures)}; distinct translated GLSL {len(texts)}"
    )
    census = corpus.feature_census(list(configs.values()))
    print(f"features, configurations using each (of {len(configs)} translated):")
    for feature, count in sorted(census.items()):
        print(f"  {feature}: {count}")
    sensitivity = corpus.initial_state_sensitivity(list(configs.values()))
    print(
        f"reads of undefined initial state over {sensitivity.checked} configurations: {sensitivity}"
    )
    gaps = [
        precision.measure(config, fragments=args.precision_fragments) for config in configs.values()
    ]
    bounded = [g for g in gaps if g.bound is not None]
    print(
        f"float vs 9-bit model: {len(bounded)} of {len(gaps)} configurations have a bound; "
        f"largest bound {max(g.bound for g in bounded):.4f} (a bound, not a measurement), "
        f"largest measured gap {max(g.measured for g in gaps):.4f}, "
        f"configurations with any 8-bit output step differing "
        f"{sum(1 for g in gaps if g.output_steps_differ)}"
    )
    ordered = sorted(g.measured for g in gaps)
    steps = sum(g.output_steps_differ for g in gaps)
    total = sum(g.fragments for g in gaps)
    print(
        f"  measured gap per configuration: median {ordered[len(ordered) // 2]:.4f}, "
        f"largest {ordered[-1]:.4f}; fragments whose 8-bit output differs between the models "
        f"{steps} of {total} ({100.0 * steps / total:.1f}%, a rate on synthetic inputs with no "
        f"null model, not a prediction of image error)"
    )
    over = [g for g in bounded if g.bound is not None and g.measured > g.bound + 1e-12]
    print(f"measured above its bound (must be 0): {len(over)}")
    report.update(
        {
            "static_addresses": len(static),
            "static_distinct": static_distinct,
            "generated_distinct": generated_distinct,
            "reachable_generated": len(reachable_blocks),
            "union_distinct": len(union),
            "outcomes": dict(outcomes),
            "structures": len(structures),
            "glsl_distinct": len(texts),
        }
    )
    if args.validate:
        compare = validate.validate_configs(
            configs, args.out / "run", count=args.validate, seed=args.seed
        )
        failing = {
            n: c
            for n, c in compare.items()
            if c.over_tolerance or c.nan_outputs or c.killed_mismatch
        }
        print(
            f"validation on lavapipe: {len(compare)} configurations x {args.validate} fragments, "
            f"{sum(c.compared for c in compare.values())} compared, "
            f"{sum(c.skipped_boundary for c in compare.values())} skipped at a mux boundary, "
            f"max error {max(c.max_error for c in compare.values()):.3g}, "
            f"{len(failing)} configurations outside tolerance {validate.TOLERANCE}"
        )
        report["validation_failures"] = len(failing)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


def cmd_random(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    configs = {f"r{index:04d}": randomcfg.random_config(rng) for index in range(args.configs)}
    compare = validate.validate_configs(
        configs, args.out / "random", count=args.fragments, seed=args.seed + 1
    )
    failing = {
        n: c for n, c in compare.items() if c.over_tolerance or c.nan_outputs or c.killed_mismatch
    }
    print(
        f"{len(compare)} random configurations x {args.fragments} fragments: "
        f"{sum(c.compared for c in compare.values())} compared, "
        f"{sum(c.skipped_boundary for c in compare.values())} skipped at a mux boundary, "
        f"max error {max(c.max_error for c in compare.values()):.3g}, "
        f"{len(failing)} outside tolerance {validate.TOLERANCE}"
    )
    return 1 if failing else 0


def cmd_check(args: argparse.Namespace) -> int:
    """Validate saved blocks and random configurations. One JSON line, exit 1 on any miss.

    The mutation runner calls this in a fresh process per mutation, so a changed source
    file is always the one that runs.
    """
    populations: dict[str, dict[str, cfg.Config]] = {}
    if args.blocks:
        raw = args.blocks.read_bytes()
        count = len(raw) // cfg.BLOCK_BYTES
        populations["corpus"] = {
            f"c{index:03d}": cfg.decode(
                raw[index * cfg.BLOCK_BYTES : (index + 1) * cfg.BLOCK_BYTES]
            )
            for index in range(count)
        }
    rng = random.Random(args.seed)
    populations["random"] = {
        f"r{index:04d}": randomcfg.random_config(rng) for index in range(args.random_configs)
    }
    summary = {}
    failed = False
    for label, configs in populations.items():
        compare = validate.validate_configs(
            configs, args.out / f"check_{label}", count=args.fragments, seed=args.seed + 1
        )
        failing = [
            n for n, c in compare.items() if c.over_tolerance or c.nan_outputs or c.killed_mismatch
        ]
        summary[label] = {
            "configs": len(compare),
            "failing": len(failing),
            "max_error": max(c.max_error for c in compare.values()),
        }
        failed |= bool(failing)
    print(json.dumps(summary, sort_keys=True))
    return 1 if failed else 0


def cmd_translate(args: argparse.Namespace) -> int:
    block = args.block.read_bytes()
    try:
        config = cfg.decode(block)
    except cfg.UnsupportedConfig as error:
        print(f"cannot translate: {error}", file=sys.stderr)
        return 1
    sys.stdout.write(
        glsl.compute_shader(config) if args.stage == "compute" else glsl.fragment_shader(config)
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a_combiner.cli",
        description="Translate NV2A register-combiner configurations to GLSL and validate them.",
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    probe_parser = sub.add_parser(
        "probe", help="route A: check field meanings against the assembler"
    )
    _add_addresses(probe_parser)
    probe_parser.set_defaults(func=cmd_probe)

    corpus_parser = sub.add_parser("corpus", help="translate every combiner the title can set")
    _add_addresses(corpus_parser)
    corpus_parser.add_argument("--setter", type=_hex, default=0x22720)
    corpus_parser.add_argument("--builder", type=_hex, default=0x20CC0)
    corpus_parser.add_argument("--builder-register", default="edx")
    corpus_parser.add_argument("--builder-mask", type=_hex, default=0xC83FE804)
    corpus_parser.add_argument("--key-target", type=_hex, default=0x18EA0)
    corpus_parser.add_argument("--key-argument", type=int, default=3)
    corpus_parser.add_argument("--key-global", type=_hex, default=0x4B85E4)
    corpus_parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    corpus_parser.add_argument("--validate", type=int, default=0, help="fragments per config")
    corpus_parser.add_argument("--precision-fragments", type=int, default=100)
    corpus_parser.add_argument("--seed", type=int, default=1)
    corpus_parser.add_argument("--json", type=Path)
    corpus_parser.set_defaults(func=cmd_corpus)

    random_parser = sub.add_parser("random", help="validate on random valid configurations")
    random_parser.add_argument("--configs", type=int, default=300)
    random_parser.add_argument("--fragments", type=int, default=200)
    random_parser.add_argument("--seed", type=int, default=7)
    random_parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    random_parser.set_defaults(func=cmd_random)

    check_parser = sub.add_parser("check", help="validate saved blocks and random configurations")
    check_parser.add_argument("--blocks", type=Path, help="concatenated 240-byte blocks")
    check_parser.add_argument("--random-configs", type=int, default=100)
    check_parser.add_argument("--fragments", type=int, default=100)
    check_parser.add_argument("--seed", type=int, default=7)
    check_parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    check_parser.set_defaults(func=cmd_check)

    translate_parser = sub.add_parser("translate", help="print the GLSL for one 240-byte block")
    translate_parser.add_argument("block", type=Path)
    translate_parser.add_argument("--stage", choices=("fragment", "compute"), default="fragment")
    translate_parser.set_defaults(func=cmd_translate)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
