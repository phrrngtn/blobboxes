"""
mitmproxy addon for blobboxes browser extraction.

Dispatches on the X-BLOBTASTIC-EXTRA-INFO header: if present, renders the
page in headless Chromium via BrowserPool and returns columnar bbox JSON.
If absent, the request passes through normally.

Chromium's own sub-resource requests carry X-BLOBBOXES-INTERNAL and are
always passed through without triggering extraction.

Usage:
    mitmdump -p 8080 -s python/blobboxes/proxy_addon.py \\
        --set blobboxes_max_pages=4

Composable with the rule4 traffic observer addon:
    mitmdump -p 8080 \\
        -s rule4/proxy_addon.py \\
        -s python/blobboxes/proxy_addon.py \\
        --set rate_limit=5 \\
        --set blobboxes_max_pages=4
"""

import json
import logging

from mitmproxy import ctx, http

from blobboxes.browser import BrowserPool, SemaphoreTimeout, to_columnar

log = logging.getLogger(__name__)

_EXTRA_INFO_HEADER = "X-BLOBTASTIC-EXTRA-INFO"
_INTERNAL_HEADER = "X-BLOBBOXES-INTERNAL"


class BlobboxesAddon:
    def __init__(self):
        self._pool = None

    def load(self, loader):
        loader.add_option(
            name="blobboxes_max_pages",
            typespec=int,
            default=4,
            help="Maximum concurrent Playwright pages for bbox extraction.",
        )
        loader.add_option(
            name="blobboxes_timeout",
            typespec=int,
            default=30,
            help="Seconds to wait for an extraction slot before returning 503.",
        )
        loader.add_option(
            name="blobboxes_bundle",
            typespec=str,
            default="full",
            help="Bundle type: 'full' (with roaring-wasm) or 'lite'.",
        )

    def configure(self, updated):
        if any(k in updated for k in (
            "blobboxes_max_pages", "blobboxes_timeout", "blobboxes_bundle",
        )):
            # Close the old pool (async close scheduled on the loop).
            if self._pool is not None:
                import asyncio
                asyncio.ensure_future(self._pool.close())

            # The proxy is "self" — Chromium routes through this same
            # mitmproxy instance. listen_host/listen_port give us the
            # address to point Chromium at.
            proxy_host = ctx.options.listen_host or "127.0.0.1"
            proxy_port = ctx.options.listen_port
            proxy_url = f"http://{proxy_host}:{proxy_port}"

            self._pool = BrowserPool(
                proxy=proxy_url,
                max_pages=ctx.options.blobboxes_max_pages,
            )
            log.info(
                "BlobboxesAddon: pool created, max_pages=%d, timeout=%ds, "
                "bundle=%s, proxy=%s",
                ctx.options.blobboxes_max_pages,
                ctx.options.blobboxes_timeout,
                ctx.options.blobboxes_bundle,
                proxy_url,
            )

    async def request(self, flow: http.HTTPFlow):
        # Chromium's own sub-resource requests — always passthrough.
        if flow.request.headers.get(_INTERNAL_HEADER):
            flow.request.headers.pop(_INTERNAL_HEADER, None)
            return

        # No extra-info header — normal proxy passthrough.
        extra_info_raw = flow.request.headers.get(_EXTRA_INFO_HEADER)
        if not extra_info_raw:
            return

        # This is a blobboxes extraction request. Remove the header so
        # it doesn't leak to the target server.
        flow.request.headers.pop(_EXTRA_INFO_HEADER, None)

        try:
            config = json.loads(extra_info_raw) if extra_info_raw.strip() else {}
        except json.JSONDecodeError as exc:
            flow.response = http.Response.make(
                400,
                json.dumps({"error": f"Invalid JSON in {_EXTRA_INFO_HEADER}: {exc}"}).encode(),
                {"Content-Type": "application/json"},
            )
            return

        url = flow.request.pretty_url

        click = config.get("click")
        wait_ms = config.get("wait_ms", 3000)
        viewport_str = config.get("viewport", "1440x900")
        cookies = config.get("cookies")

        try:
            vw, vh = (int(x) for x in viewport_str.split("x", 1))
        except (ValueError, AttributeError):
            vw, vh = 1440, 900

        timeout = ctx.options.blobboxes_timeout

        try:
            bboxes, extraction_ms = await self._pool.extract(
                url=url,
                click_selector=click,
                click_wait_ms=wait_ms,
                viewport_width=vw,
                viewport_height=vh,
                cookies=cookies,
                timeout=timeout,
            )
            result = to_columnar(url, bboxes, extraction_ms)
            body = json.dumps(result).encode()
            flow.response = http.Response.make(
                200, body, {"Content-Type": "application/json"},
            )
        except SemaphoreTimeout:
            flow.response = http.Response.make(
                503,
                json.dumps({"error": "All extraction slots busy", "url": url}).encode(),
                {"Content-Type": "application/json"},
            )
        except Exception as exc:
            log.exception("BlobboxesAddon: extraction failed for %s", url)
            flow.response = http.Response.make(
                502,
                json.dumps({"error": str(exc), "url": url}).encode(),
                {"Content-Type": "application/json"},
            )

    async def done(self):
        if self._pool is not None:
            await self._pool.close()
            self._pool = None


addons = [BlobboxesAddon()]
