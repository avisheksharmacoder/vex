#!/usr/bin/env python3
"""Zero-dependency HTTP server for the Material-styled Vex Browser Studio."""

from __future__ import annotations

import argparse
import json
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

import vex
from .studio_core import DEFAULT_SCRIPT, SNIPPETS, TempVexSession

STATIC_DIR = Path(__file__).resolve().parent / "static"

_MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
}


class StudioHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer holding a single thread-safe TempVexSession."""

    daemon_threads = True

    def __init__(self, server_address: tuple[str, int]) -> None:
        self.session = TempVexSession()
        self.lock = threading.Lock()
        super().__init__(server_address, StudioRequestHandler)

    def server_close(self) -> None:
        try:
            self.session.close()
        finally:
            super().server_close()


class StudioRequestHandler(BaseHTTPRequestHandler):
    """Serves static HTML/CSS/JS assets and JSON query/DB inspection endpoints."""

    server: StudioHTTPServer

    def log_message(self, format: str, *args: Any) -> None:
        # Suppress noisy per-request stderr logs when launched from tests or CLI.
        return

    def _send_bytes(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send_bytes(status, "application/json; charset=utf-8", raw)

    def _read_json_body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(min(length, 4 * 1024 * 1024))
        data = json.loads(raw.decode("utf-8"))
        return data if isinstance(data, dict) else {}

    def _serve_static(self, rel_name: str) -> None:
        target = (STATIC_DIR / rel_name).resolve()
        if target.parent != STATIC_DIR.resolve() or not target.is_file():
            self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "asset not found"})
            return
        mime = _MIME_TYPES.get(target.suffix, "application/octet-stream")
        self._send_bytes(HTTPStatus.OK, mime, target.read_bytes())

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/":
            self._serve_static("studio.html")
            return
        if path in ("/static/studio.css", "/static/studio.js"):
            self._serve_static(path.removeprefix("/static/"))
            return
        if path == "/api/state":
            qs = parse_qs(parsed.query)
            selected = qs.get("file", [None])[0]
            with self.server.lock:
                state = self.server.session.stats_payload(selected_file=selected)
            self._send_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "state": state,
                    "default_script": DEFAULT_SCRIPT,
                    "snippets": SNIPPETS,
                },
            )
            return
        if path == "/api/file":
            qs = parse_qs(parsed.query)
            selected = qs.get("name", [None])[0]
            with self.server.lock:
                state = self.server.session.stats_payload(selected_file=selected)
            self._send_json(HTTPStatus.OK, {"ok": True, "state": state})
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": f"unknown route: {path}"})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path

        try:
            body = self._read_json_body()
        except (ValueError, UnicodeDecodeError) as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": f"invalid JSON: {exc}"})
            return

        selected = body.get("selected_file")
        if path == "/api/run":
            script = str(body.get("script", ""))
            with self.server.lock:
                try:
                    snap = self.server.session.run_script(script)
                    state = self.server.session.stats_payload(selected_file=selected)
                    self._send_json(
                        HTTPStatus.OK,
                        {"ok": True, "result": snap.to_payload(), "state": state},
                    )
                except vex.VexError as exc:
                    state = self.server.session.stats_payload(selected_file=selected)
                    self._send_json(
                        HTTPStatus.OK,
                        {"ok": False, "error": str(exc), "state": state},
                    )
            return

        if path == "/api/checkpoint":
            with self.server.lock:
                try:
                    self.server.session.checkpoint()
                    state = self.server.session.stats_payload(selected_file=selected)
                    self._send_json(HTTPStatus.OK, {"ok": True, "state": state})
                except vex.VexError as exc:
                    state = self.server.session.stats_payload(selected_file=selected)
                    self._send_json(HTTPStatus.OK, {"ok": False, "error": str(exc), "state": state})
            return

        if path == "/api/reset":
            with self.server.lock:
                self.server.session.open_new()
                state = self.server.session.stats_payload()
            self._send_json(HTTPStatus.OK, {"ok": True, "state": state})
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": f"unknown route: {path}"})


def create_server(host: str = "127.0.0.1", port: int = 0) -> StudioHTTPServer:
    """Create a bound StudioHTTPServer instance ready for serve_forever()."""
    return StudioHTTPServer((host, port))


def main(argv: list[str] | None = None) -> int:
    """Start the Browser Studio server and optionally open the default browser."""
    parser = argparse.ArgumentParser(prog="vex-web-studio", description="Vex Browser Studio")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765, help="Port to listen on (0 for ephemeral)")
    parser.add_argument("--no-browser", action="store_true", help="Do not auto-open browser tab")
    args = parser.parse_args(argv)

    srv = create_server(args.host, args.port)
    actual_port = srv.server_address[1]
    url = f"http://{args.host}:{actual_port}/"
    print(f"Vex Browser Studio running at {url} (Ctrl+C to stop)", flush=True)

    if not args.no_browser:
        threading.Timer(0.25, lambda: webbrowser.open(url)).start()

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down Vex Browser Studio...", flush=True)
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

