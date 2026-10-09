"""Shared pieces: redaction, transport, cache, ledger, cost cap, retry."""

from __future__ import annotations

import hashlib
import json
import os
import random
import re
import subprocess
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

PRICES_PATH = Path(__file__).with_name("prices.json")
DEFAULT_CACHE_DIR = Path("tmp/llm-cache")
DEFAULT_LEDGER = Path("tmp/llm-usage.jsonl")
DEFAULT_MAX_USD = 5.00
DEFAULT_GLOBAL_CEILING_USD = 10.00
GLOBAL_DIR_ENV = "TSFP_LLM_GLOBAL_DIR"

_SECRETS: set[str] = set()
_SECRET_LOCK = threading.Lock()
_PATTERNS = [
    re.compile(r"(?i)bearer\s+[A-Za-z0-9._~+/=-]+"),
    re.compile(r"sk-[A-Za-z0-9_-]{6,}"),
]


class LlmError(RuntimeError):
    """Base error. Messages are always redacted."""


class CostCapError(LlmError):
    """Raised before a call that would push spend past --max-usd."""


class HttpError(LlmError):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status


def register_secret(value: str) -> None:
    value = value.strip()
    if len(value) >= 4:
        with _SECRET_LOCK:
            _SECRETS.add(value)


def redact(text: object) -> str:
    """Remove registered keys, Bearer tokens and sk- strings from text."""
    out = str(text)
    with _SECRET_LOCK:
        secrets = sorted(_SECRETS, key=len, reverse=True)
    for secret in secrets:
        out = out.replace(secret, "[REDACTED]")
    for pattern in _PATTERNS:
        out = pattern.sub("[REDACTED]", out)
    return out


def read_key(path: Path | str) -> str:
    """Read a key file (path argument, relative by default). Never logged."""
    key = Path(path).read_text(encoding="utf-8").strip()
    if not key:
        raise LlmError(f"key file {path} is empty")
    register_secret(key)
    return key


def canonical_json(obj: object) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def request_hash(provider: str, url: str, body: object) -> str:
    """Key by the body exactly as sent. Insertion order is kept (no sort_keys) because
    option order is semantically meaningful for Jev choice questions."""
    text = json.dumps(
        {"p": provider, "u": url, "b": body}, separators=(",", ":"), ensure_ascii=False
    )
    return hashlib.sha256(text.encode()).hexdigest()


def estimate_tokens(obj: object) -> int:
    """Rough token count: chars / 3.5, rounded up (conservative for code)."""
    text = obj if isinstance(obj, str) else canonical_json(obj)
    return int(len(text) / 3.5) + 1


def load_prices(path: Path = PRICES_PATH) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def estimate_cost(
    prices: dict, provider: str, model: str, input_tokens: int, output_tokens: int
) -> float:
    table = prices.get(provider, {})
    entry = table.get(model) or table.get("default") or {"input": 100.0, "output": 300.0}
    return (input_tokens * entry["input"] + output_tokens * entry["output"]) / 1_000_000


Transport = Callable[[str, dict, bytes, float], "tuple[int, bytes]"]


def urllib_transport(url: str, headers: dict, body: bytes, timeout: float) -> tuple[int, bytes]:
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def global_dir() -> Path:
    """Directory shared by every worktree of this repo (the main checkout's tmp/llm-global).

    Per-process caps cannot bound several agents at once, so every paid call is also recorded here
    and checked against one ceiling. Override with TSFP_LLM_GLOBAL_DIR (tests do).
    """
    override = os.environ.get(GLOBAL_DIR_ENV)
    if override:
        return Path(override)
    try:
        common = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        ).stdout.strip()
        return Path(common).parent / "tmp" / "llm-global"
    except (OSError, subprocess.SubprocessError):
        return Path("tmp/llm-global")


def global_spent_usd(directory: Path | None = None) -> float:
    path = (directory or global_dir()) / "usage.jsonl"
    total = 0.0
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                total += float(json.loads(line).get("usd", 0.0))
            except (ValueError, TypeError):
                continue
    return total


def global_ceiling_usd(directory: Path | None = None) -> float:
    path = (directory or global_dir()) / "ceiling-usd.txt"
    if path.is_file():
        try:
            return float(path.read_text(encoding="utf-8").strip())
        except ValueError:
            pass
    return DEFAULT_GLOBAL_CEILING_USD


class Ledger:
    """Append-only JSONL usage ledger plus in-process spend total."""

    def __init__(self, path: Path = DEFAULT_LEDGER) -> None:
        self.path = Path(path)
        self._lock = threading.Lock()
        self.process_usd = 0.0

    def record(
        self,
        provider: str,
        model: str,
        input_tokens: int,
        output_tokens: int,
        usd: float,
        cached: bool = False,
    ) -> None:
        row = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "provider": provider,
            "model": model,
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
            "usd": round(usd, 6),
            "cached": cached,
        }
        with self._lock:
            self.process_usd += usd
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(row) + "\n")
            if usd > 0:
                shared = global_dir()
                shared.mkdir(parents=True, exist_ok=True)
                with (shared / "usage.jsonl").open("a", encoding="utf-8") as handle:
                    handle.write(json.dumps(row) + "\n")


class Cache:
    """On-disk response cache keyed by sha256 of the canonical request. Stores bodies only."""

    def __init__(self, directory: Path = DEFAULT_CACHE_DIR, enabled: bool = True) -> None:
        self.dir = Path(directory)
        self.enabled = enabled

    def _path(self, key: str) -> Path:
        return self.dir / key[:2] / f"{key}.json"

    def get(self, key: str) -> Any:
        if not self.enabled:
            return None
        path = self._path(key)
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        return None

    def put(self, key: str, value: dict) -> None:
        if not self.enabled:
            return
        path = self._path(key)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(redact(json.dumps(value)), encoding="utf-8")


class Guard:
    """Hard cost cap checked before each call using a worst-case estimate."""

    def __init__(self, max_usd: float = DEFAULT_MAX_USD) -> None:
        self.max_usd = max_usd
        self.reserved = 0.0
        self._lock = threading.Lock()

    def reserve(self, ledger: Ledger, worst_case_usd: float) -> float:
        with self._lock:
            shared_spent = global_spent_usd()
            shared_ceiling = global_ceiling_usd()
            if shared_spent + self.reserved + worst_case_usd > shared_ceiling:
                raise CostCapError(
                    f"GLOBAL cost ceiling {shared_ceiling:.2f} USD would be exceeded "
                    f"(all agents spent {shared_spent:.4f}, this process in flight "
                    f"{self.reserved:.4f}, "
                    f"this call worst case {worst_case_usd:.4f}); raise "
                    f"{global_dir() / 'ceiling-usd.txt'} only with the owner's approval"
                )
            if ledger.process_usd + self.reserved + worst_case_usd > self.max_usd:
                raise CostCapError(
                    f"cost cap {self.max_usd:.2f} USD would be exceeded "
                    f"(spent {ledger.process_usd:.4f}, in flight {self.reserved:.4f}, "
                    f"this call worst case {worst_case_usd:.4f})"
                )
            self.reserved += worst_case_usd
            return worst_case_usd

    def release(self, amount: float) -> None:
        with self._lock:
            self.reserved -= amount


def post_with_retry(
    transport: Transport,
    url: str,
    headers: dict,
    body: bytes,
    timeout: float,
    retries: int,
    retry_statuses: tuple[int, ...],
    sleep: Callable[[float], None] = time.sleep,
    base_delay: float = 1.0,
) -> bytes:
    """POST with exponential backoff and jitter. Errors are redacted before raising."""
    last = "no attempt"
    for attempt in range(retries + 1):
        try:
            status, data = transport(url, headers, body, timeout)
        except Exception as exc:  # network or timeout: retryable
            last = f"transport error: {type(exc).__name__}: {redact(exc)}"
        else:
            if status == 200:
                return data
            last = f"HTTP {status}: {redact(data.decode('utf-8', 'replace')[:500])}"
            if status not in retry_statuses:
                raise HttpError(status, last)
        if attempt < retries:
            sleep(base_delay * (2**attempt) * (0.5 + random.random() / 2))
    raise HttpError(-1, f"gave up after {retries + 1} attempts: {last}")
