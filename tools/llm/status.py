"""Print cumulative LLM spend per provider and model from the usage ledger. No key material."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

from tools.llm.common import DEFAULT_LEDGER, global_ceiling_usd, global_dir, global_spent_usd


def summarize(path: Path) -> dict:
    totals: dict = defaultdict(
        lambda: {"calls": 0, "cached": 0, "input_tokens": 0, "output_tokens": 0, "usd": 0.0}
    )
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            row = json.loads(line)
            entry = totals[(row["provider"], row["model"])]
            entry["calls"] += 1
            entry["cached"] += 1 if row.get("cached") else 0
            entry["input_tokens"] += row["input_tokens"]
            entry["output_tokens"] += row["output_tokens"]
            entry["usd"] += row["usd"]
    return totals


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", default=str(DEFAULT_LEDGER))
    args = parser.parse_args(argv)
    totals = summarize(Path(args.ledger))
    per_provider: dict = defaultdict(float)
    for (provider, model), entry in sorted(totals.items()):
        per_provider[provider] += entry["usd"]
        print(
            f"{provider:7} {model:16} calls={entry['calls']} cache_hits={entry['cached']} "
            f"in={entry['input_tokens']} out={entry['output_tokens']} usd={entry['usd']:.4f}"
        )
    for provider, usd in sorted(per_provider.items()):
        print(f"TOTAL {provider}: ${usd:.4f}")
    if not totals:
        print("no usage recorded in this ledger")
    print(
        f"GLOBAL (all agents, {global_dir()}): spent ${global_spent_usd():.4f} "
        f"of ceiling ${global_ceiling_usd():.2f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
