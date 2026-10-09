"""OpenAI Responses API client with structured output, cache, retries, cap and dry-run.

Probe findings (2026-10-06, gpt-5.5): POST /v1/responses works with reasoning.effort
(none, low, medium, high, xhigh; not minimal), max_output_tokens and
text.format json_schema strict. temperature is rejected. Chat completions also works
but needs max_completion_tokens and reasoning_effort. See docs/llm-pipeline.md.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools.llm import schema as schema_mod
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
    request_hash,
    urllib_transport,
)

URL = "https://api.openai.com/v1/responses"
DEFAULT_MODEL = "gpt-5.5"
DEFAULT_KEY_PATH = Path(".secrets/openai-api-key.txt")


@dataclass
class Result:
    text: str = ""
    parsed: object = None
    input_tokens: int = 0
    output_tokens: int = 0
    usd: float = 0.0
    cached: bool = False
    dry_run: bool = False
    latency: float = 0.0
    raw_status: str = ""


@dataclass
class OpenAIClient:
    key: str
    model: str = DEFAULT_MODEL
    effort: str = "low"
    max_output_tokens: int = 4000
    timeout: float = 300.0
    retries: int = 4
    concurrency: int = 4
    max_usd: float = DEFAULT_MAX_USD
    dry_run: bool = False
    transport: Transport = urllib_transport
    cache: Cache = field(default_factory=lambda: Cache(DEFAULT_CACHE_DIR))
    ledger: Ledger = field(default_factory=lambda: Ledger(DEFAULT_LEDGER))
    sleep: object = time.sleep
    base_delay: float = 1.0

    def __post_init__(self) -> Any:
        from tools.llm.common import register_secret

        register_secret(self.key)
        self.guard = Guard(self.max_usd)
        self.semaphore = threading.Semaphore(self.concurrency)
        self.prices = load_prices()

    def build_body(
        self, prompt: str, system: str | None, schema: dict | None, schema_name: str
    ) -> dict:
        body: dict = {
            "model": self.model,
            "input": prompt,
            "max_output_tokens": self.max_output_tokens,
            "reasoning": {"effort": self.effort},
        }
        if system:
            body["instructions"] = system
        if schema is not None:
            body["text"] = {
                "format": {
                    "type": "json_schema",
                    "name": schema_name,
                    "strict": True,
                    "schema": schema,
                }
            }
        return body

    def complete(
        self,
        prompt: str,
        system: str | None = None,
        schema: dict | None = None,
        schema_name: str = "output",
    ) -> Result:
        body = self.build_body(prompt, system, schema, schema_name)
        key = request_hash("openai", URL, body)
        in_est = estimate_tokens(body)
        worst = estimate_cost(self.prices, "openai", self.model, in_est, self.max_output_tokens)
        if self.dry_run:
            return Result(input_tokens=in_est, output_tokens=0, usd=worst, dry_run=True)
        hit = self.cache.get(key)
        if hit is not None:
            self.ledger.record("openai", self.model, 0, 0, 0.0, cached=True)
            return self._parse(hit["response"], schema, 0.0, cached=True, saved=hit.get("usd", 0.0))
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
                    (429, 500, 502, 503, 504, 529),
                    self.sleep,
                    self.base_delay,
                )
                latency = time.time() - started
        finally:
            self.guard.release(reserved)
        response = json.loads(data)
        usage = response.get("usage") or {}
        usd = estimate_cost(
            self.prices,
            "openai",
            self.model,
            usage.get("input_tokens", 0),
            usage.get("output_tokens", 0),
        )
        self.ledger.record(
            "openai", self.model, usage.get("input_tokens", 0), usage.get("output_tokens", 0), usd
        )
        result = self._parse(response, schema, latency, cached=False)
        result.usd = usd
        self.cache.put(key, {"response": response, "usd": usd})
        return result

    def _parse(
        self, response: dict, schema: dict | None, latency: float, cached: bool, saved: float = 0.0
    ) -> Result:
        usage = response.get("usage") or {}
        status = response.get("status", "")
        if status != "completed":
            reason = (response.get("incomplete_details") or {}).get("reason", "")
            raise LlmError(f"response status {status} {reason} (raise --max-output-tokens?)")
        text = "".join(
            part.get("text", "")
            for item in response.get("output", [])
            if item.get("type") == "message"
            for part in item.get("content", [])
            if part.get("type") == "output_text"
        )
        parsed = None
        if schema is not None:
            try:
                parsed = json.loads(text)
                schema_mod.validate(parsed, schema)
            except (ValueError, schema_mod.SchemaError) as exc:
                raise LlmError(f"structured output failed validation: {redact(exc)}") from exc
        return Result(
            text=text,
            parsed=parsed,
            input_tokens=usage.get("input_tokens", 0),
            output_tokens=usage.get("output_tokens", 0),
            usd=0.0,
            cached=cached,
            latency=latency,
            raw_status=status,
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="One-shot OpenAI Responses call (prompt from --prompt or stdin)."
    )
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument(
        "--effort", default="low", choices=["none", "low", "medium", "high", "xhigh"]
    )
    parser.add_argument("--key-file", default=str(DEFAULT_KEY_PATH))
    parser.add_argument("--prompt")
    parser.add_argument("--system")
    parser.add_argument("--schema-file", help="JSON schema file for structured output")
    parser.add_argument("--max-output-tokens", type=int, default=4000)
    parser.add_argument("--max-usd", type=float, default=DEFAULT_MAX_USD)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--cache-dir", default=str(DEFAULT_CACHE_DIR))
    parser.add_argument("--ledger", default=str(DEFAULT_LEDGER))
    args = parser.parse_args(argv)
    try:
        key = (
            read_key(args.key_file)
            if not args.dry_run or Path(args.key_file).is_file()
            else "dry-run-key"
        )
        client = OpenAIClient(
            key=key,
            model=args.model,
            effort=args.effort,
            max_output_tokens=args.max_output_tokens,
            max_usd=args.max_usd,
            dry_run=args.dry_run,
            cache=Cache(Path(args.cache_dir), not args.no_cache),
            ledger=Ledger(Path(args.ledger)),
        )
        schema = json.loads(Path(args.schema_file).read_text()) if args.schema_file else None
        result = client.complete(args.prompt or sys.stdin.read(), args.system, schema)
    except LlmError as exc:
        print(f"error: {redact(exc)}", file=sys.stderr)
        return 2
    print(
        result.text
        if not result.dry_run
        else f"dry-run: ~{result.input_tokens} input tokens, worst case ${result.usd:.4f}"
    )
    print(
        f"tokens in={result.input_tokens} out={result.output_tokens} "
        f"usd={result.usd:.5f} cached={result.cached}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
