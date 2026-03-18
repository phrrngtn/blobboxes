"""
Playwright controller for blobboxes browser bundle.

Thin shim that:
  1. Loads the blobboxes JS bundle from disk
  2. Launches Chromium with --proxy-server (all traffic through personal proxy)
  3. Injects the bundle into a CDP isolated world
  4. Calls blobboxes.init() + blobboxes.snapshot()
  5. Returns structured bbox data

All outbound HTTP traffic from Chromium flows through the configured proxy.
The proxy handles rate-limiting, auth injection, TLS inspection, and
optionally chains to a corporate upstream proxy. Playwright never connects
directly to the public internet.

The JS bundle lives in blobboxes/browser/dist/ and is built by
`node build.js` in that directory.
"""

import json
import logging
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)

# Locate the bundle relative to this file
_BROWSER_DIR = Path(__file__).resolve().parent.parent.parent / "browser" / "dist"
_LITE_BUNDLE = _BROWSER_DIR / "blobboxes.lite.js"
_FULL_BUNDLE = _BROWSER_DIR / "blobboxes.bundle.js"

_bundle_cache = {}


def _load_bundle(full=False):
    """Load the JS bundle, caching the string."""
    path = _FULL_BUNDLE if full else _LITE_BUNDLE
    key = str(path)
    if key not in _bundle_cache:
        if not path.exists():
            raise FileNotFoundError(
                f"Bundle not found: {path}\n"
                f"Run: cd {_BROWSER_DIR.parent} && node build.js"
            )
        _bundle_cache[key] = path.read_text()
        log.info("Loaded bundle: %s (%d bytes)", path.name, len(_bundle_cache[key]))
    return _bundle_cache[key]


@dataclass
class TextBBox:
    """A text node's bounding box with font and DOM context."""
    text: str
    x: float
    y: float
    w: float
    h: float
    font_family: str
    font_size: float
    font_weight: str
    color: str
    tag: str
    cls: str
    t_ms: float
    mutation_type: str


class BrowserPool:
    """
    Persistent Playwright browser instance with proxy configuration.

    Keeps Chromium running between requests so we pay the launch cost once.
    All traffic is routed through the configured proxy.
    """

    def __init__(self, proxy=None):
        """
        Args:
            proxy: Proxy URL for all Chromium traffic, e.g. "http://localhost:8080".
                   If None, Chromium connects directly (development/testing only).
        """
        self._proxy = proxy
        self._pw = None
        self._browser = None

    def _ensure_browser(self):
        if self._browser is not None:
            return

        from playwright.sync_api import sync_playwright
        self._pw = sync_playwright().start()

        launch_args = {}
        if self._proxy:
            launch_args["proxy"] = {"server": self._proxy}
            log.info("Launching Chromium with proxy: %s", self._proxy)
        else:
            log.warning("Launching Chromium without proxy (direct outbound)")

        self._browser = self._pw.chromium.launch(headless=True, **launch_args)

    def extract(
        self,
        url,
        click_selector=None,
        click_wait_ms=3000,
        viewport_width=1440,
        viewport_height=900,
        bundle_js=None,
    ):
        """
        Extract bboxes from a single page using the persistent browser.

        Each call creates a new browser context (isolated cookies/storage)
        that is torn down after extraction.
        """
        self._ensure_browser()

        if bundle_js is None:
            bundle_js = _load_bundle(full=False)

        context = self._browser.new_context(
            viewport={"width": viewport_width, "height": viewport_height}
        )
        try:
            page = context.new_page()

            page.goto(url, wait_until="domcontentloaded", timeout=30000)
            page.wait_for_timeout(5000)

            if click_selector:
                tab = page.query_selector(click_selector)
                if tab:
                    tab.click()
                    page.wait_for_timeout(click_wait_ms)

            # Create isolated world and inject bundle
            cdp = context.new_cdp_session(page)
            tree = cdp.send("Page.getFrameTree")
            frame_id = tree["frameTree"]["frame"]["id"]
            world = cdp.send("Page.createIsolatedWorld", {
                "frameId": frame_id,
                "worldName": "blobboxes",
            })
            ctx_id = world["executionContextId"]

            cdp.send("Runtime.evaluate", {
                "expression": bundle_js,
                "contextId": ctx_id,
                "returnByValue": True,
            })

            cdp.send("Runtime.evaluate", {
                "expression": "blobboxes.init()",
                "contextId": ctx_id,
                "returnByValue": True,
            })

            result = cdp.send("Runtime.evaluate", {
                "expression": "JSON.stringify(blobboxes.snapshot())",
                "contextId": ctx_id,
                "returnByValue": True,
            })

            raw = json.loads(result["result"]["value"])
        finally:
            context.close()

        bboxes = [TextBBox(**b) for b in raw]
        bboxes.sort(key=lambda b: (b.y, b.x))

        log.info("Extracted %d text bboxes from %s", len(bboxes), url)
        return bboxes

    def close(self):
        if self._browser:
            self._browser.close()
            self._browser = None
        if self._pw:
            self._pw.stop()
            self._pw = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# Module-level default pool (lazy, no proxy)
_default_pool = None


def extract_bboxes(
    url,
    click_selector=None,
    click_wait_ms=3000,
    viewport_width=1440,
    viewport_height=900,
    proxy=None,
):
    """
    Extract all visible text bounding boxes from a rendered page.

    Convenience function using a module-level BrowserPool. For long-running
    services, create a BrowserPool directly with proxy configuration.

    Args:
        proxy: Proxy URL, e.g. "http://localhost:8080". If None, connects
               directly (suitable for development/testing only).
    """
    global _default_pool
    if _default_pool is None:
        _default_pool = BrowserPool(proxy=proxy)

    return _default_pool.extract(
        url=url,
        click_selector=click_selector,
        click_wait_ms=click_wait_ms,
        viewport_width=viewport_width,
        viewport_height=viewport_height,
    )
