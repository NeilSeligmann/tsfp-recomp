"""Live smoke test: one cheap request per provider. Prints status, latency, tokens, cost only."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Any

from tools.llm import jev_client, openai_client
from tools.llm.common import Cache, Ledger, LlmError, read_key, redact


def smoke_openai(args: Any) -> bool:
    client = openai_client.OpenAIClient(
        key=read_key(args.openai_key_file),
        model=args.model,
        effort="low",
        max_output_tokens=300,
        max_usd=args.max_usd,
        cache=Cache(Path(args.cache_dir), False),
        ledger=Ledger(Path(args.ledger)),
    )
    started = time.time()
    result = client.complete("Reply with the single word: ok")
    print(
        f"openai model={args.model} status=OK latency={time.time() - started:.2f}s "
        f"in={result.input_tokens} out={result.output_tokens} usd={result.usd:.6f} "
        f"reply_nonempty={bool(result.text.strip())}"
    )
    return bool(result.text.strip())


def smoke_jev(args: Any) -> bool:
    client = jev_client.JevClient(
        key=read_key(args.jev_key_file),
        max_usd=args.max_usd,
        cache=Cache(Path(args.cache_dir), False),
        ledger=Ledger(Path(args.ledger)),
    )
    started = time.time()
    result = client.ask(
        "The function calls memcpy.",
        {"q": jev_client.noul("Does the text say a function calls memcpy?")},
    )
    probability = result.answers["q"]["noul"]
    print(
        f"jev model=jev-latest status=OK latency={time.time() - started:.2f}s "
        f"in={result.input_tokens} out={result.output_tokens} "
        f"usd={result.usd:.8f} noul={probability}"
    )
    return 0.0 <= probability <= 1.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", choices=["openai", "jev", "both"], default="both")
    parser.add_argument("--model", default=openai_client.DEFAULT_MODEL)
    parser.add_argument("--openai-key-file", default=str(openai_client.DEFAULT_KEY_PATH))
    parser.add_argument("--jev-key-file", default=str(jev_client.DEFAULT_KEY_PATH))
    parser.add_argument("--max-usd", type=float, default=0.25)
    parser.add_argument("--cache-dir", default="tmp/llm-cache")
    parser.add_argument("--ledger", default="tmp/llm-usage.jsonl")
    args = parser.parse_args(argv)
    ok = True
    for name, fn in (("openai", smoke_openai), ("jev", smoke_jev)):
        if args.provider in (name, "both"):
            try:
                ok = fn(args) and ok
            except LlmError as exc:
                print(f"{name} status=FAIL {redact(exc)}")
                ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
