"""Jev (TypeSafe SystemOne) client: many typed questions per request over one state.

Limits (docs + owner brief): 64k tokens per request, 32k for state plus the longest
question, $0.042 per million input tokens, output free. 401/422 are fatal, 429/529 retried.
Choice answers lean to the first listed option, so options are shuffled with a
deterministic seed and an optional reversed-order second pass checks consistency.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools.llm.common import (
    DEFAULT_CACHE_DIR,
    DEFAULT_LEDGER,
    DEFAULT_MAX_USD,
    Cache,
    Guard,
    Ledger,
    LlmError,
    Transport,
    estimate_cost,
    estimate_tokens,
    load_prices,
    post_with_retry,
    read_key,
    redact,
    register_secret,
    request_hash,
    urllib_transport,
)

URL = "https://api.typesafe.ai/v1/systemone"
MODEL = "jev-latest"
DEFAULT_KEY_PATH = Path(".secrets/typesafe-ai.txt")
MAX_REQUEST_TOKENS = 60_000  # margin under the 64k limit
MAX_STATE_QUESTION_TOKENS = 30_000  # margin under 32k


@dataclass
class Result:
    answers: dict = field(default_factory=dict)
    input_tokens: int = 0
    output_tokens: int = 0
    usd: float = 0.0
    cached: bool = False
    dry_run: bool = False
    latency: float = 0.0


def noul(instructions: str, true: str = "yes", false: str = "no") -> dict:
    return {
        "type": "noul",
        "instructions": instructions,
        "criteria": {"true": true, "false": false},
    }


def choice(instructions: str, options: dict[str, str]) -> dict:
    return {"type": "choice", "instructions": instructions, "criteria": dict(options)}


def score(instructions: str, levels: list[str]) -> dict:
    return {"type": "score", "instructions": instructions, "criteria": list(levels)}


def shuffled_options(options: dict, seed: int, question_id: str, reverse: bool = False) -> dict:
    """Deterministic option order for a (seed, question id) pair."""
    digest = hashlib.sha256(f"{seed}:{question_id}".encode()).digest()
    names = list(options)
    random.Random(int.from_bytes(digest[:8], "big")).shuffle(names)
    if reverse:
        names.reverse()
    return {name: options[name] for name in names}


def apply_order(questions: dict, seed: int, reverse: bool = False) -> dict:
    out = {}
    for qid, question in questions.items():
        if question["type"] == "choice":
            question = {
                **question,
                "criteria": shuffled_options(question["criteria"], seed, qid, reverse),
            }
        out[qid] = question
    return out


@dataclass
class JevClient:
    key: str
    timeout: float = 120.0
    retries: int = 5
    concurrency: int = 4
    max_usd: float = DEFAULT_MAX_USD
    dry_run: bool = False
    transport: Transport = urllib_transport
    cache: Cache = field(default_factory=lambda: Cache(DEFAULT_CACHE_DIR))
    ledger: Ledger = field(default_factory=lambda: Ledger(DEFAULT_LEDGER))
    sleep: object = time.sleep
    base_delay: float = 1.0

    def __post_init__(self) -> Any:
        register_secret(self.key)
        self.guard = Guard(self.max_usd)
        self.semaphore = threading.Semaphore(self.concurrency)
        self.prices = load_prices()

    def _check_size(self, state: object, questions: dict) -> int:
        total = estimate_tokens({"state": state, "questions": questions})
        longest = max((estimate_tokens(q) for q in questions.values()), default=0)
        if total > MAX_REQUEST_TOKENS:
            raise LlmError(
                f"request ~{total} tokens exceeds the {MAX_REQUEST_TOKENS} budget, "
                "split the questions"
            )
        if estimate_tokens(state) + longest > MAX_STATE_QUESTION_TOKENS:
            raise LlmError("state plus longest question exceeds the ~32k limit, shorten the state")
        return total

    def _call(self, state: object, questions: dict) -> Result:
        body = {"state": state, "model": MODEL, "questions": questions}
        in_est = self._check_size(state, questions)
        worst = estimate_cost(self.prices, "jev", MODEL, in_est, 0)
        if self.dry_run:
            return Result(input_tokens=in_est, usd=worst, dry_run=True)
        key = request_hash("jev", URL, body)
        hit = self.cache.get(key)
        if hit is not None:
            self.ledger.record("jev", MODEL, 0, 0, 0.0, cached=True)
            return Result(
                answers=hit["answers"],
                input_tokens=hit["usage"].get("input_tokens", 0),
                cached=True,
            )
        reserved = self.guard.reserve(self.ledger, worst)
        try:
            with self.semaphore:
                started = time.time()
                headers = {
                    "Authorization": f"Bearer {self.key}",
                    "Content-Type": "application/json",
                }
                data = post_with_retry(
                    self.transport,
                    URL,
                    headers,
                    json.dumps(body).encode(),
                    self.timeout,
                    self.retries,
                    (429, 529, 500, 502, 503, 504),
                    self.sleep,
                    self.base_delay,
                )
                latency = time.time() - started
        finally:
            self.guard.release(reserved)
        response = json.loads(data)
        usage = response.get("usage") or {}
        usd = estimate_cost(
            self.prices, "jev", MODEL, usage.get("input_tokens", 0), usage.get("output_tokens", 0)
        )
        self.ledger.record(
            "jev", MODEL, usage.get("input_tokens", 0), usage.get("output_tokens", 0), usd
        )
        answers = response.get("answers") or {}
        missing = set(questions) - set(answers)
        if missing:
            raise LlmError(f"response lacks answers for {sorted(missing)}")
        self.cache.put(key, {"answers": answers, "usage": usage})
        return Result(
            answers,
            usage.get("input_tokens", 0),
            usage.get("output_tokens", 0),
            usd,
            False,
            False,
            latency,
        )

    def ask(
        self, state: object, questions: dict, seed: int = 0, order_check: bool = False
    ) -> Result:
        """Ask all questions in one request. With order_check, choice questions are asked
        again in reversed option order and each choice answer gets `order_consistent`."""
        first = self._call(state, apply_order(questions, seed))
        choices = {qid: q for qid, q in questions.items() if q["type"] == "choice"}
        if not order_check or not choices or first.dry_run:
            return first
        second = self._call(state, apply_order(choices, seed, reverse=True))
        for qid in choices:
            a, b = first.answers[qid], second.answers[qid]
            a["order_consistent"] = a.get("choice") == b.get("choice")
            probs_a, probs_b = a.get("probabilities", {}), b.get("probabilities", {})
            a["probabilities_swapped"] = probs_b
            a["probabilities_mean"] = {
                k: (probs_a.get(k, 0.0) + probs_b.get(k, 0.0)) / 2 for k in choices[qid]["criteria"]
            }
        first.input_tokens += second.input_tokens
        first.output_tokens += second.output_tokens
        first.usd += second.usd
        return first


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="One-shot Jev call: a state and one noul question."
    )
    parser.add_argument("--key-file", default=str(DEFAULT_KEY_PATH))
    parser.add_argument("--state", required=True)
    parser.add_argument("--question", required=True, help="yes/no question text")
    parser.add_argument("--max-usd", type=float, default=DEFAULT_MAX_USD)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--cache-dir", default=str(DEFAULT_CACHE_DIR))
    parser.add_argument("--ledger", default=str(DEFAULT_LEDGER))
    args = parser.parse_args(argv)
    try:
        key = (
            read_key(args.key_file)
            if Path(args.key_file).is_file() or not args.dry_run
            else "dry-run-key"
        )
        client = JevClient(
            key=key,
            max_usd=args.max_usd,
            dry_run=args.dry_run,
            cache=Cache(Path(args.cache_dir), not args.no_cache),
            ledger=Ledger(Path(args.ledger)),
        )
        result = client.ask(args.state, {"q": noul(args.question)})
    except LlmError as exc:
        print(f"error: {redact(exc)}", file=sys.stderr)
        return 2
    if result.dry_run:
        print(f"dry-run: ~{result.input_tokens} input tokens, worst case ${result.usd:.6f}")
    else:
        print(json.dumps(result.answers))
        print(
            f"tokens in={result.input_tokens} out={result.output_tokens} usd={result.usd:.6f}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
