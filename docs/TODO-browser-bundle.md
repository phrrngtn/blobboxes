# TODO: Browser Bundle — Persistent Injection, Proxy Integration, Interactive Decoration

Items arising from the persistent bundle injection work and subsequent
design discussions. Grouped by theme, roughly ordered by dependency.

## Build: fix lite bundle roaring-wasm stub

The lite bundle (`blobboxes.lite.js`) uses `external: ['roaring-wasm']`
in esbuild, which leaves a bare `require("roaring-wasm")` in the IIFE
output. This crashes at eval time in the browser:
`Error: Dynamic require of "roaring-wasm" is not supported`.

The lite bundle should work for snapshot-only extraction (no classify).
Fix: replace the `external` with an esbuild `alias` or `define` that
stubs out the roaring-wasm imports with no-ops/nulls, so the IIFE
evaluates cleanly and `loadFilters()`/`classify()` throw a clear
"roaring-wasm not available in lite bundle" error instead of crashing
the whole bundle.

**File:** `browser/build.js`

## Proxy addon: mitmproxy integration

Implement the mitmproxy addon that dispatches on the
`X-BLOBTASTIC-EXTRA-INFO` header. Core logic:

- [x] Addon skeleton: intercept request, check for header, strip
  header before forwarding
- [x] Parse header JSON (click, wait_ms, viewport, domains, cookies,
  filters, format)
- [x] Instantiate `BrowserPool` within the addon (long-lived, one per
  proxy process)
- [ ] Inject pre-flighted cookies into the browser context before
  navigation
- [ ] Retrieve domain filter bitmaps by name from local blobfilters DB
  (DuckDB or SQLite)
- [ ] Retrieve ad-hoc filter bitmaps by GUID from OpenBao (with TTL)
- [ ] Inject filter bitmaps into isolated world via CDP
  (`loadFilters()` with base64-encoded bytes, `awaitPromise: true`)
- [ ] Call `classify()` in the isolated world if domains were requested
- [x] Serialize response as columnar JSON
- [x] Return response as the HTTP response body
  (`Content-Type: application/json`)

**Files:** `python/blobboxes/proxy_addon.py`,
`python/blobboxes/browser.py`

## Columnar JSON response serialization

The `extract()` method currently returns a list of `TextBBox`
dataclasses. Add a serialization path that produces the columnar JSON
format documented in `browser-table-extraction.md`:

- [x] `to_columnar(bboxes, classified=None)` function: takes bbox list
  and optional classification results, returns dict with parallel
  arrays
- [x] Wire into `http_controller.py` response path
- [x] Wire into proxy addon response path

**Files:** `python/blobboxes/browser.py`, `python/blobboxes/http_controller.py`

## Filter injection via CDP

Python-side filter cache and CDP injection into the isolated world:

- [ ] Filter cache: `dict[str, bytes]` mapping filter name to
  serialized portable roaring bitmap bytes. Loaded from DuckDB on
  first access, LRU or TTL eviction.
- [ ] `inject_filters(cdp, ctx_id, filters)` helper: base64-encode
  bitmap bytes, construct JS expression calling
  `blobboxes.loadFilters(...)`, evaluate with `awaitPromise: true`
- [ ] Integrate into `BrowserPool.extract()` — accept optional
  `domains` parameter, resolve from cache, inject before `classify()`

**Files:** `python/blobboxes/browser.py`

## Response column selection (future/maybe)

A `select` field in the `X-BLOBTASTIC-EXTRA-INFO` header to request
only a subset of columns in the response, e.g.
`"select": ["text", "x", "y", "matches"]`. Reduces response size when
the caller doesn't need font metadata.

Not yet committed to — the full column set is small enough that this
may never be needed. Revisit if response size becomes a concern.

## Interactive decoration: `decorate()` replacing `highlight()`

Replace the overlay-div `highlight()` with span-wrapping + table halos
for the interactive/CTP path:

- [ ] `browser/src/decorate.js`: new module implementing:
  - `decorate(classified)` — span-wrap matched tokens, draw table
    halos around spatial clusters
  - `clearDecorations()` — remove all injected elements
  - `onHover(callback)` — register hover inspector for domain match
    details
- [ ] CSS injection: single `<style>` element with `.bb-token-match`
  and `.bb-table-halo` classes, CSS custom properties for per-domain
  coloring
- [ ] Span-wrapping: split text nodes at token boundaries, wrap matched
  tokens in `<span class="bb-token-match">` with
  `style.setProperty('--bb-domain-color', color)`. Process in reverse
  offset order so earlier positions remain valid after DOM mutation.
- [ ] Table halo: detect rectangular clusters of classified bboxes,
  draw `position: absolute` divs with dashed border around bounding
  rectangle. Reposition on scroll.
- [ ] Deprecate `highlight.js` (overlay-div approach drifts across
  browsers/fonts)
- [ ] Wire into bundle entry point: `blobboxes.decorate()`,
  `blobboxes.clearDecorations()`, `blobboxes.onHover()`

**Files:** new `browser/src/decorate.js`, update
`browser/src/index.js`, update `browser/build.js`

## Live reclassification via MutationObserver

For the interactive/CTP path, the observer detects new or changed text
nodes and incrementally updates decorations:

- [ ] On observer flush: run `classify()` on new/changed bboxes only
- [ ] Call `decorate()` incrementally (add new highlights without
  clearing existing ones)
- [ ] Debounce: don't reclassify while the user is actively scrolling
  or clicking (wait for idle)

**Files:** `browser/src/observer.js`, `browser/src/decorate.js`

## QWebEngineScript documentation for Excel CTP

The pattern for PySide6/QWebEngineView is documented in
`browser-bundle-design.md` (Controllers section). Remaining items for
the blobapi repo (not this repo):

- [ ] `DocumentCreation` injection point + `ApplicationWorld` isolation
- [ ] Profile-level script registration (persists across pages)
- [ ] `QWebChannel` bridge for Python→JS filter injection and
  JS→Python result delivery
- [ ] PyXLL integration: ribbon commands to trigger extraction,
  task pane to display results
