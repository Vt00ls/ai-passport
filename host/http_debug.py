#!/usr/bin/env python3
"""Expose the dashboard status string over HTTP for ``curl`` debugging.

Python 3 standard library only. Run on dev-forge:

    python3 host/http_debug.py --host 0.0.0.0 --port 8000

Then from the Mac (or anywhere on the LAN):

    curl http://<dev-forge>:8000/status       # raw status string
    curl http://<dev-forge>:8000/status.json  # payload + decoded fields

This is a transient debug helper with no authentication. Bind it to 127.0.0.1
unless you specifically need LAN access, and stop it when done.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

import collector  # noqa: E402
import protocol  # noqa: E402

# Shared, configurable collector inputs; http_debug's main() may override paths.
SOURCES = collector.Sources()


def build_payload_json() -> dict:
    payload = collector.collect(SOURCES)
    fields = protocol.decode(payload)
    return {
        "payload": payload,
        "bytes": len(payload.encode("utf-8")),
        "fields": dict(fields),
        "generated_at": time.time(),
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "ai-passport-dashboard-debug/1.0"

    def _send(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path in ("/", "/status", "/health"):
            try:
                payload = collector.collect(SOURCES)
                self._send(200, "text/plain; charset=utf-8", (payload + "\n").encode("utf-8"))
            except Exception as exc:  # pragma: no cover - defensive
                self._send(500, "text/plain; charset=utf-8", f"collect failed: {exc}\n".encode("utf-8"))
        elif path in ("/status.json", "/health.json", "/json"):
            try:
                body = json.dumps(build_payload_json(), indent=2).encode("utf-8") + b"\n"
                self._send(200, "application/json; charset=utf-8", body)
            except Exception as exc:  # pragma: no cover - defensive
                self._send(500, "text/plain; charset=utf-8", f"collect failed: {exc}\n".encode("utf-8"))
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found\n")

    def log_message(self, fmt: str, *args) -> None:  # noqa: N802
        sys.stderr.write("[http_debug] %s\n" % (fmt % args))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8000, help="bind port (default: 8000)")
    parser.add_argument("--sessions-dir", default=None, help="agent session jsonl directory (default: ~/.pi/agent/sessions)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.sessions_dir:
        SOURCES.sessions_dir = args.sessions_dir

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"http_debug listening on http://{args.host}:{args.port}", file=sys.stderr)
    print("endpoints: /status (raw string), /status.json (parsed fields)", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
