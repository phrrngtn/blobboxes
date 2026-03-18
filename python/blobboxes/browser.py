"""
Playwright controller for blobboxes browser bundle.

Thin shim that:
  1. Loads the blobboxes JS bundle from disk
  2. Launches Chromium with --proxy-server (all traffic through personal proxy)
  3. Registers the bundle via CDP addScriptToEvaluateOnNewDocument into a
     named isolated world ("blobboxes") — the browser auto-injects it before
     any page script runs on each navigation
  4. Calls blobboxes.init() + blobboxes.snapshot() in the isolated world
  5. Returns structured bbox data

All outbound HTTP traffic from Chromium flows through the configured proxy.
The proxy handles rate-limiting, auth injection, TLS inspection, and
optionally chains to a corporate upstream proxy. Playwright never connects
directly to the public internet.

The JS bundle lives in blobboxes/browser/dist/ and is built by
`node build.js` in that directory.
"""

import asyncio
import json
import logging
import time
from dataclasses import dataclass, fields
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


async def _setup_cdp_init_script(cdp, bundle_js):
    """Register the bundle as an init script in a named isolated world via CDP.

    Uses Page.addScriptToEvaluateOnNewDocument with worldName so the browser
    auto-injects the bundle before page scripts on every navigation. Returns
    the CDP script identifier for later removal if needed.

    Also enables Runtime domain and sets up an event listener to capture the
    execution context ID for the "blobboxes" isolated world. Returns an
    async callable that waits until the context ID is available (set by the
    Runtime.executionContextCreated event after navigation).
    """
    await cdp.send("Page.enable")
    await cdp.send("Runtime.enable")

    resp = await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
        "source": bundle_js,
        "worldName": "blobboxes",
    })
    script_id = resp.get("identifier")
    log.debug("Registered init script in 'blobboxes' world, identifier=%s", script_id)

    # Capture the isolated world's execution context ID via CDP events.
    # The event fires after each navigation when the world is created.
    ctx_event = asyncio.Event()
    ctx_holder = {}

    def _on_context_created(params):
        context = params.get("context", {})
        if context.get("name") == "blobboxes":
            ctx_holder["id"] = context["id"]
            ctx_event.set()

    cdp.on("Runtime.executionContextCreated", _on_context_created)

    async def wait_for_context(timeout=30):
        """Wait until the blobboxes world context ID is available."""
        try:
            await asyncio.wait_for(ctx_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise TimeoutError(
                "Timed out waiting for 'blobboxes' isolated world context"
            )
        return ctx_holder["id"]

    return script_id, wait_for_context


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


class SemaphoreTimeout(Exception):
    """Raised when the concurrency semaphore cannot be acquired in time."""


def to_columnar(url, bboxes, extraction_ms=None):
    """Convert bbox list to columnar JSON dict.

    Each TextBBox field becomes a parallel array of length n.
    """
    n = len(bboxes)
    result = {"url": url, "n": n}
    if extraction_ms is not None:
        result["extraction_ms"] = round(extraction_ms, 1)
    for f in fields(TextBBox):
        result[f.name] = [getattr(b, f.name) for b in bboxes]
    return result


class BrowserPool:
    """
    Persistent Playwright browser instance with proxy configuration.

    Keeps Chromium running between requests so we pay the launch cost once.
    All traffic is routed through the configured proxy.

    Uses a single long-lived browser context with the bundle init script
    registered once via CDP addScriptToEvaluateOnNewDocument. The browser
    auto-injects the bundle into a named isolated world ("blobboxes") on
    every navigation — like a browser extension's content script.

    Cookies and storage are cleared between requests so each extraction
    starts clean. The init script registration survives the clear — it's
    bound to the context, not to any particular page or origin.

    All public methods are async. The pool is designed to run inside an
    asyncio event loop (mitmproxy addon, or a dedicated loop for the
    standalone HTTP controller).
    """

    def __init__(self, proxy=None, max_pages=4, extra_http_headers=None):
        """
        Args:
            proxy: Proxy URL for all Chromium traffic, e.g. "http://localhost:8080".
                   If None, Chromium connects directly (development/testing only).
            max_pages: Maximum concurrent extractions (semaphore size).
            extra_http_headers: Dict of headers added to every Chromium request.
                Defaults to {"X-BLOBBOXES-INTERNAL": "1"} so the proxy addon
                can detect and skip re-entrant requests from Chromium.
        """
        self._proxy = proxy
        self._pw = None
        self._browser = None
        self._context = None
        self._bundle_registered = False
        self._semaphore = asyncio.Semaphore(max_pages)
        self._extra_http_headers = extra_http_headers if extra_http_headers is not None else {
            "X-BLOBBOXES-INTERNAL": "1",
        }

    async def _ensure_browser(self):
        if self._browser is not None:
            return

        from playwright.async_api import async_playwright
        self._pw = await async_playwright().start()

        launch_args = {}
        if self._proxy:
            launch_args["proxy"] = {"server": self._proxy}
            log.info("Launching Chromium with proxy: %s", self._proxy)
        else:
            log.warning("Launching Chromium without proxy (direct outbound)")

        self._browser = await self._pw.chromium.launch(headless=True, **launch_args)

    async def _ensure_context(self, bundle_js, viewport_width, viewport_height):
        """Ensure the long-lived context exists with the init script registered.

        Creates the context on first call. The init script is registered once
        and persists for the lifetime of the context — the browser re-injects
        it into the "blobboxes" isolated world on every navigation, like a
        browser extension content script.
        """
        if self._context is not None:
            return

        ctx_kwargs = {
            "viewport": {"width": viewport_width, "height": viewport_height},
        }
        if self._extra_http_headers:
            ctx_kwargs["extra_http_headers"] = self._extra_http_headers
        self._context = await self._browser.new_context(**ctx_kwargs)
        self._bundle_registered = False

    async def _register_bundle(self, cdp, bundle_js):
        """Register the bundle init script on a page's CDP session.

        Must be called once per page (CDP sessions are per-page). The
        registration tells the browser to inject the bundle into the
        "blobboxes" isolated world on every navigation of that page.
        """
        _script_id, wait_for_context = await _setup_cdp_init_script(cdp, bundle_js)
        return wait_for_context

    async def extract(
        self,
        url,
        click_selector=None,
        click_wait_ms=3000,
        viewport_width=1440,
        viewport_height=900,
        bundle_js=None,
        cookies=None,
        timeout=30,
    ):
        """
        Extract bboxes from a single page.

        Each call creates a new page within the long-lived context, registers
        the init script on its CDP session, navigates, extracts, and tears
        down the page. Cookies/storage are cleared between requests.

        Args:
            cookies: Optional list of cookie dicts to inject before navigation.
                Each dict should have at minimum 'name', 'value', 'url' or 'domain'.
            timeout: Semaphore acquisition timeout in seconds. If the pool is
                at capacity, waits up to this long before raising SemaphoreTimeout.

        Returns:
            (bboxes, extraction_ms) tuple. extraction_ms covers navigation +
            wait + snapshot (not browser launch).
        """
        try:
            await asyncio.wait_for(self._semaphore.acquire(), timeout=timeout)
        except asyncio.TimeoutError:
            raise SemaphoreTimeout(
                f"Could not acquire extraction slot within {timeout}s"
            )

        t0 = time.perf_counter()
        try:
            await self._ensure_browser()

            if bundle_js is None:
                bundle_js = _load_bundle(full=False)

            await self._ensure_context(bundle_js, viewport_width, viewport_height)

            if cookies:
                await self._context.add_cookies(cookies)

            page = await self._context.new_page()
            try:
                cdp = await self._context.new_cdp_session(page)

                # Register bundle on this page's CDP session. The init script
                # auto-injects into the "blobboxes" world on navigation.
                wait_for_context = await self._register_bundle(cdp, bundle_js)

                await page.goto(url, wait_until="domcontentloaded", timeout=30000)
                await page.wait_for_timeout(5000)

                # The world context ID is available after navigation
                ctx_id = await wait_for_context(timeout=30)

                if click_selector:
                    tab = await page.query_selector(click_selector)
                    if tab:
                        await tab.click()
                        await page.wait_for_timeout(click_wait_ms)

                await cdp.send("Runtime.evaluate", {
                    "expression": "blobboxes.init()",
                    "contextId": ctx_id,
                    "returnByValue": True,
                })

                result = await cdp.send("Runtime.evaluate", {
                    "expression": "JSON.stringify(blobboxes.snapshot())",
                    "contextId": ctx_id,
                    "returnByValue": True,
                })

                raw = json.loads(result["result"]["value"])
            finally:
                await page.close()
                # Clear cookies/storage AFTER extraction. During page load,
                # Chromium's sub-resource requests may depend on cookies set
                # by earlier responses (CSRF tokens, session cookies from JS).
                # Clearing after ensures the page works normally during its
                # load, but the next request starts clean.
                await self._context.clear_cookies()

            bboxes = [TextBBox(**b) for b in raw]
            bboxes.sort(key=lambda b: (b.y, b.x))
            extraction_ms = (time.perf_counter() - t0) * 1000

            log.info(
                "Extracted %d text bboxes from %s in %.0f ms",
                len(bboxes), url, extraction_ms,
            )
            return bboxes, extraction_ms
        finally:
            self._semaphore.release()

    async def close(self):
        if self._context:
            await self._context.close()
            self._context = None
        if self._browser:
            await self._browser.close()
            self._browser = None
        if self._pw:
            await self._pw.stop()
            self._pw = None

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        await self.close()


# Module-level default pool (lazy, no proxy)
_default_pool = None


async def extract_bboxes(
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

    Returns:
        (bboxes, extraction_ms) tuple.
    """
    global _default_pool
    if _default_pool is None:
        _default_pool = BrowserPool(proxy=proxy)

    return await _default_pool.extract(
        url=url,
        click_selector=click_selector,
        click_wait_ms=click_wait_ms,
        viewport_width=viewport_width,
        viewport_height=viewport_height,
    )
