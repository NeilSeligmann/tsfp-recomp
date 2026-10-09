# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only live web view of the agent status: `python3 -m tools.agents.webstatus`.

Serves the page in `webstatus_static/` for `/` (redirected to `/dashboard`) and every viewer
route, the JSON snapshot of `webstatus_data.build_snapshot` and the task, milestone and
session details. GET and HEAD only, no authentication: it exposes status
data already visible in the repository on the forwarded container port. One snapshot is
built at most every `--cache` seconds and shared by every client.
"""

import argparse
import json
import os
import re
import sys
import threading
import time
from collections.abc import Callable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from tools.agents import status, webstatus_data

STATIC_DIR = Path(__file__).parent / "webstatus_static"
STATIC = {
    "app.css": "text/css; charset=utf-8",
    "app.js": "text/javascript; charset=utf-8",
    "table-core.js": "text/javascript; charset=utf-8",
}
TASK_ID = re.compile(r"T\d+")
MILESTONE_ID = re.compile(r"M\d+")
OWNER = r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}"
SESSION_OWNER = re.compile(OWNER)
PAGE_ROUTE = re.compile(
    r"/(dashboard|milestones|tasks|sessions|workers|messages|progress|tasks/T\d+|milestones/M\d+"
    rf"|sessions/{OWNER})"
)
DETAIL_LABELS = {
    "task": ("T123", "the ledger"),
    "milestone": ("M12", "the roadmap"),
    "session": ("claude-main", "the coordination state"),
}
DETAIL_LOOKUPS = {
    "task": webstatus_data.task_detail,
    "milestone": webstatus_data.milestone_detail,
    "session": webstatus_data.session_detail,
}
SECURITY = {
    "X-Content-Type-Options": "nosniff",
    "Content-Security-Policy": "default-src 'self'",
}
TEXT = "text/plain; charset=utf-8"


class SnapshotCache:
    """One snapshot per `ttl` seconds, built under a lock so concurrent readers share it."""

    def __init__(self, build: Callable[[], dict], ttl: float) -> None:
        self._build = build
        self._ttl = ttl
        self._lock = threading.Lock()
        self._value: dict | None = None
        self._built_at = 0.0

    def get(self) -> dict:
        with self._lock:
            now = time.monotonic()
            if self._value is None or now - self._built_at >= self._ttl:
                self._value = self._build()
                self._built_at = time.monotonic()
            return self._value


def matches_etag(header: str, etag: str) -> bool:
    """True when an `If-None-Match` value (weak validators and lists allowed) names `etag`."""
    offered = [part.strip().removeprefix("W/") for part in header.split(",")]
    return etag in offered or "*" in offered


def read_static(name: str) -> bytes | None:
    try:
        return (STATIC_DIR / name).read_bytes()
    except OSError:
        return None


def make_handler(cache: SnapshotCache, args: argparse.Namespace) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "webstatus"

        def log_message(self, format: str, *values: object) -> None:  # noqa: A002
            sys.stderr.write(f"webstatus {self.address_string()} {format % values}\n")

        def send(
            self, code: int, body: bytes, kind: str, extra: dict[str, str] | None = None
        ) -> None:
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            for name, value in {**SECURITY, **(extra or {})}.items():
                self.send_header(name, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def send_json(self, code: int, value: object, extra: dict[str, str] | None = None) -> None:
            headers = {"Cache-Control": "no-store", **(extra or {})}
            self.send(code, json.dumps(value).encode(), "application/json", headers)

        def send_static(self, name: str, kind: str) -> None:
            body = read_static(name)
            if body is None:
                self.send_json(404, {"error": f"{name} is missing"})
            else:
                self.send(200, body, kind)

        def send_versioned(self, payload: dict, query: str) -> None:
            """200 with an ETag, or 304 when `If-None-Match` or `?since=` names the version."""
            version = payload["version"]
            etag = f'"{version}"'
            since = parse_qs(query).get("since", [""])[0]
            if matches_etag(self.headers.get("If-None-Match", ""), etag) or since == version:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.send_header("Cache-Control", "no-store")
                for name, value in SECURITY.items():
                    self.send_header(name, value)
                self.end_headers()
            else:
                self.send_json(200, payload, {"ETag": etag})

        def send_state(self, query: str) -> None:
            self.send_versioned(cache.get(), query)

        def send_detail(
            self, kind: str, entry_id: str, pattern: re.Pattern[str], query: str
        ) -> None:
            if not pattern.fullmatch(entry_id):
                label = DETAIL_LABELS[kind][0]
                self.send_json(400, {"error": f"{kind} id must look like {label}"})
                return
            detail = DETAIL_LOOKUPS[kind](args, entry_id)
            if detail is None:
                self.send_json(404, {"error": f"{entry_id} is not in {DETAIL_LABELS[kind][1]}"})
            else:
                self.send_versioned(detail, query)

        def send_page(self) -> None:
            page = read_static("index.html")
            if page is None:
                self.send_json(404, {"error": "index.html is missing"})
            else:
                self.send(200, page, "text/html; charset=utf-8")

        def route(self) -> None:
            url = urlparse(self.path)
            path = url.path
            name = path.removeprefix("/static/")
            if path == "/":
                self.send(302, b"", TEXT, {"Location": "/dashboard"})
            elif PAGE_ROUTE.fullmatch(path):
                self.send_page()
            elif path.startswith("/static/") and name in STATIC:
                self.send_static(name, STATIC[name])
            elif path == "/healthz":
                self.send(200, b"ok", TEXT, {"Cache-Control": "no-store"})
            elif path == "/api/state":
                self.send_state(url.query)
            elif path.startswith("/api/task/"):
                self.send_detail("task", path.removeprefix("/api/task/"), TASK_ID, url.query)
            elif path.startswith("/api/milestone/"):
                self.send_detail(
                    "milestone", path.removeprefix("/api/milestone/"), MILESTONE_ID, url.query
                )
            elif path.startswith("/api/session/"):
                self.send_detail(
                    "session", path.removeprefix("/api/session/"), SESSION_OWNER, url.query
                )
            else:
                self.send_json(404, {"error": "not found"})

        def serve(self) -> None:
            try:
                self.route()
            except Exception as error:  # one bad request must not drop the connection silently
                sys.stderr.write(
                    f"webstatus error on {self.path}: {type(error).__name__}: {error}\n"
                )
                self.send_json(500, {"error": f"{type(error).__name__}: {error}"})

        def do_GET(self) -> None:
            self.serve()

        def do_HEAD(self) -> None:
            self.serve()

        def refuse(self) -> None:
            self.send(405, b"read-only", TEXT, {"Allow": "GET, HEAD"})

        def __getattr__(self, name: str) -> Callable[[], None]:
            """Every other HTTP method, known or not, is a 405 instead of the default 501."""
            if name.startswith("do_"):
                return self.refuse
            raise AttributeError(name)

    return Handler


def make_server(
    host: str,
    port: int,
    args: argparse.Namespace,
    cache_seconds: float,
    interval: int,
    build: Callable[[], dict] | None = None,
) -> ThreadingHTTPServer:
    args.interval = interval
    builder = build or (lambda: webstatus_data.build_snapshot(args))
    cache = SnapshotCache(builder, cache_seconds)
    server = ThreadingHTTPServer((host, port), make_handler(cache, args))
    server.daemon_threads = True
    return server


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Serve a read-only live web view of the status.")
    parser.add_argument("--host", default="0.0.0.0", help="address to bind")
    parser.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("WEB_PORT", "7860")),
        help="port to bind (default $WEB_PORT or 7860)",
    )
    parser.add_argument("--interval", type=int, default=5, help="seconds between client polls")
    parser.add_argument("--cache", type=float, default=2, help="seconds a snapshot is shared")
    parser.add_argument("--tasks", type=Path, default=status.DEFAULT_TASKS, help="task ledger")
    parser.add_argument("--roadmap", type=Path, default=status.DEFAULT_ROADMAP, help="roadmap")
    parser.add_argument("--metrics", type=Path, default=status.DEFAULT_METRICS, help="metrics.json")
    parser.add_argument(
        "--stale", type=int, default=1200, help="seconds of silence after which a claim is dead"
    )
    parser.add_argument(
        "--all-sessions",
        action="store_true",
        help="also list ended sessions on the Sessions page (hidden by default)",
    )
    parser.add_argument(
        "--session-stale",
        type=int,
        default=status.SESSION_STALE_SECONDS,
        help="seconds without any coord command after which a session without a claim is ended",
    )
    parser.set_defaults(no_roadmap=False, no_progress=False)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    server = make_server(args.host, args.port, args, args.cache, args.interval)
    host, port = server.server_address[:2]
    print(f"serving on http://{host}:{port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
