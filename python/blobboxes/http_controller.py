"""
HTTP controller for blobboxes browser extraction.

Jina-like HTTP interface over a persistent headless Chromium instance.
All Chromium traffic is routed through a configured proxy (personal proxy),
which may itself chain to a corporate upstream proxy.

Architecture (option A):
    Caller → blobboxes HTTP (:8484) → Playwright → personal proxy (:8080)
                                                        → [corporate proxy] → internet

Endpoints:
    GET  /read?url=...&click=...     → columnar bbox JSON
    GET  /health                     → {"status": "ok"}

Usage:
    uv run python -m blobboxes.http_controller \\
        --port 8484 \\
        --proxy http://localhost:8080

    curl 'http://localhost:8484/read?url=https://mistral.ai/pricing&click=button:has-text("API pricing")'

The service itself should sit behind the same reverse proxy that fronts
Bifrost and OpenBao. Auth and rate-limiting for callers is handled there,
not here. This service trusts its callers.
"""

import asyncio
import json
import logging
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

from blobboxes.browser import BrowserPool, to_columnar

log = logging.getLogger(__name__)

# Module-level persistent browser pool and event loop.
# The pool's async methods run on _loop (a dedicated asyncio event loop
# in a background daemon thread). The sync HTTP handler dispatches to it
# via asyncio.run_coroutine_threadsafe.
_pool = None
_loop = None


def _run_async(coro):
    """Run an async coroutine on the background event loop, blocking until done."""
    return asyncio.run_coroutine_threadsafe(coro, _loop).result(timeout=120)


def _handle_read(params):
    """Handle a /read request, returning columnar bbox JSON."""
    url = params.get("url", [None])[0]
    if not url:
        return 400, {"error": "url parameter required"}

    click = params.get("click", [None])[0]
    wait = int(params.get("wait", ["3000"])[0])
    width = int(params.get("width", ["1440"])[0])
    height = int(params.get("height", ["900"])[0])

    bboxes, extraction_ms = _run_async(_pool.extract(
        url=url,
        click_selector=click,
        click_wait_ms=wait,
        viewport_width=width,
        viewport_height=height,
    ))

    return 200, to_columnar(url, bboxes, extraction_ms)


class BBoxHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/read":
            params = parse_qs(parsed.query)
            try:
                status, body = _handle_read(params)
            except Exception as e:
                log.exception("Error handling /read")
                status, body = 500, {"error": str(e)}
        elif parsed.path == "/health":
            status, body = 200, {"status": "ok"}
        else:
            status, body = 404, {"error": "not found"}

        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def log_message(self, fmt, *args):
        log.info(fmt, *args)


def serve(host="127.0.0.1", port=8484, proxy=None):
    """
    Run the HTTP server with a persistent browser pool.

    Creates a background asyncio event loop for the async BrowserPool,
    then runs the sync HTTP server in the main thread.

    Args:
        host: Bind address. Default 127.0.0.1 (localhost only — callers
              should come through the reverse proxy, not directly).
        port: Listen port.
        proxy: Proxy URL for all Chromium outbound traffic,
               e.g. "http://localhost:8080". Required in production.
    """
    global _pool, _loop

    if proxy is None:
        log.warning(
            "No --proxy specified. Chromium will connect directly to the "
            "internet. This is suitable for development only."
        )

    # Start a background event loop for async Playwright operations.
    _loop = asyncio.new_event_loop()
    threading.Thread(target=_loop.run_forever, daemon=True).start()

    _pool = BrowserPool(proxy=proxy)

    server = HTTPServer((host, port), BBoxHandler)
    log.info("blobboxes HTTP server on %s:%d, proxy=%s", host, port, proxy or "(direct)")
    print(f"blobboxes HTTP server on {host}:{port}, proxy={proxy or '(direct)'}")

    try:
        server.serve_forever()
    finally:
        _run_async(_pool.close())
        _loop.call_soon_threadsafe(_loop.stop)


if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.INFO)

    port = 8484
    proxy = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--port" and i + 1 < len(args):
            port = int(args[i + 1])
            i += 2
        elif args[i] == "--proxy" and i + 1 < len(args):
            proxy = args[i + 1]
            i += 2
        else:
            i += 1

    serve(port=port, proxy=proxy)
