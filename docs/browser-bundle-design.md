# blobboxes Browser Bundle: Architecture & Proxy Integration

> The design unit is the **JS + WASM bundle** itself, not the controllers.
> Controllers (Playwright, PySide6, Chrome extension) are thin integration
> shims that handle injection and data return.

## Why a bundle?

CDP `Runtime.evaluate` and Qt `runJavaScript` both accept a single string
of JavaScript. No module loader, no `<script>` tags, no filesystem access.
The injection target is an **isolated world** — a separate JS execution
context that shares the DOM with the page but has its own global scope.
The page's own scripts cannot detect, interfere with, or read the
injected code.

This constraint dictates the packaging: all source modules must be
concatenated into a single self-contained IIFE (Immediately Invoked
Function Expression) that exposes a `blobboxes` global. esbuild handles
the module resolution and concatenation. The output is a single `.js`
file that Python reads from disk and passes as a string to the browser.

## Bundle structure

```
blobboxes/browser/
├── src/
│   ├── index.js          # Entry point: init(), snapshot(), classify(), onMutation()
│   ├── bbox.js           # TreeWalker + Range.getClientRects extraction
│   ├── observer.js       # MutationObserver with batched flush
│   ├── classify.js       # roaring-wasm probing + FNV-1a hashing
│   └── highlight.js      # CSS overlay for interactive mode
├── build.js              # esbuild script
├── package.json          # depends on roaring-wasm, esbuild
└── dist/
    ├── blobboxes.lite.js     # snapshot + observer only, no WASM (~12 KB)
    └── blobboxes.bundle.js   # full bundle with roaring-wasm inlined (Phase 3)
```

Two build variants:

- **Lite** (`--no-roaring`): snapshot and observer only. No WASM dependency.
  Suitable for Phase 1-2 and headless extraction where classification
  happens server-side. ~12 KB.

- **Full**: includes roaring-wasm with the WASM binary inlined as base64.
  Required for browser-side domain classification (Phase 3) and the
  "browsing while hunting" interactive use case. The base64 inlining is
  necessary because isolated worlds cannot `fetch()` external resources.

## API surface

```javascript
// Initialize — call once after injection
blobboxes.init({
    onMutation: callback,     // optional: batched bbox mutations
    flushInterval: 200,       // ms between flushes
    flushSize: 50,            // max batch size
    maxTextLength: 200,       // skip text nodes longer than this
});

// One-shot snapshot of all visible text bboxes
const bboxes = blobboxes.snapshot();

// Load domain filters (portable roaring bitmaps from database)
await blobboxes.loadFilters([
    { domain: "price_per_mtok", bytes: Uint8Array },
    { domain: "model_identifier", bytes: Uint8Array },
]);

// Classify bboxes against loaded filters
const matches = blobboxes.classify(bboxes);
// → [{ bbox, matches: [{domain, score}] }]

// Visual overlay (interactive mode only)
blobboxes.highlight(matches, { color: "rgba(255,200,0,0.3)" });

// Cleanup
blobboxes.dispose();
```

## Bbox data structure

Every text node yields:

```javascript
{
    text,           // string, trimmed, ≤200 chars
    x, y, w, h,    // CSS px, 0.1 precision (getClientRects)
    font_family,    // first family from computedStyle
    font_size,      // px (float)
    font_weight,    // "400", "700", etc.
    color,          // "rgb(r,g,b)"
    tag,            // parent element tagName
    cls,            // parent className (truncated to 80 chars)
    t_ms,           // performance.now() at extraction
    mutation_type,  // "snapshot" | "added" | "changed"
}
```

## Controllers (thin shims)

Each controller does three things:
1. Inject the bundle into an isolated world
2. Call bundle functions (init, snapshot, classify)
3. Receive data back (callbacks or return values)

### Playwright (headless)

```python
pool = BrowserPool(proxy="http://localhost:8080")
bboxes = pool.extract(url="https://mistral.ai/pricing",
                      click_selector="button:has-text('API pricing')")
```

Internally: CDP `Page.createIsolatedWorld` → `Runtime.evaluate(bundle_js)`
→ `blobboxes.init()` → `blobboxes.snapshot()`. Each call creates a new
browser context (isolated cookies/storage) and tears it down after.

### PySide6 (interactive)

```python
script = QWebEngineScript()
script.setSourceCode(bundle_js)
script.setWorldId(QWebEngineScript.ScriptWorldId.ApplicationWorld)
script.setInjectionPoint(QWebEngineScript.InjectionPoint.DocumentReady)
page.scripts().insert(script)
```

The bundle persists across navigations. `QWebChannel` bridges JS→Python
for live MutationObserver callbacks. `DocumentCreation` injection point
survives framework hydration (React/Next.js replacing the main world).

### Chrome extension (future)

```javascript
// content_script.js — already runs in an isolated world
import blobboxes from './blobboxes.bundle.js';
blobboxes.init({ onMutation: (batch) => {
    chrome.runtime.sendMessage({ type: 'bboxes', data: batch });
}});
```

---

## Proxy-Routed Architecture

### Why not direct outbound?

Playwright's internal Chromium connects to target URLs on its own by
default. In an enterprise context this is unacceptable:

- No egress control or allowlisting
- No audit trail of outbound connections
- No auth injection for intranet pages (SPNEGO, mTLS)
- No rate-limiting against target domains
- SSRF vector if the service accepts arbitrary URLs

All Chromium traffic must flow through a proxy.

### The personal proxy model

```
Client (DuckDB/blobhttp, notebook, curl)
    │
    │  HTTP request with X-BLOBTASTIC-EXTRA-INFO header
    │
    ▼
Personal proxy (mitmproxy, :8080)
    │
    ├── Normal URL (no special header)?
    │     → Forward as usual: rate-limit, auth injection, logging
    │
    └── Has X-BLOBTASTIC-EXTRA-INFO header?
          → Hand to blobboxes addon:
              1. Strip the header, extract config (click selectors, cookies, filter GUIDs)
              2. Launch Playwright page through THIS SAME PROXY
                 (Chromium's sub-resource requests have no special header
                  → they flow through the proxy's normal pipeline)
              3. Inject blobboxes bundle into CDP isolated world
              4. Extract bboxes, optionally classify against filters
              5. Return bbox JSON as the HTTP response
    │
    ▼  (optionally chains to corporate proxy)
Corporate proxy
    │  ── allowlist, DLP, TLS inspection, centralized logging
    ▼
Public internet (or intranet)
```

The personal proxy may itself chain to a centrally-maintained corporate
proxy via mitmproxy's `--mode upstream:http://corporate-proxy:3128`. The
personal proxy is the user's agent — it holds their secrets, their rate
budgets, their domain filters. The corporate proxy enforces organizational
policy. These are separate security boundaries with separate
responsibilities.

### Dispatch via header, not URL

The blobboxes addon dispatches on the presence of the
`X-BLOBTASTIC-EXTRA-INFO` header, not on a sentinel domain or URL path.
This is critical:

- **The URL is the real URL.** `GET https://intranet.corp/data`, not
  `GET http://bboxes.internal/read?url=https://intranet.corp/data`.
  Proxy allowlists, domain-level rate-limiting, and access logs all work
  without special cases. The proxy sees a request to `intranet.corp` and
  applies `intranet.corp` policy.

- **No loop avoidance needed.** Chromium's sub-resource requests (CSS, JS,
  fonts, API calls made by the page) don't have the special header, so they
  flow through the proxy normally. Only the initial client request triggers
  the blobboxes addon.

- **The client owns the complexity.** blobhttp does pre-flight auth
  (SPNEGO negotiation, OAuth token exchange, OpenBao secret lookup),
  constructs the request with proper `Authorization` headers, and stashes
  any extra config (click selectors, filter references) into the
  `X-BLOBTASTIC-EXTRA-INFO` header. The proxy addon just unpacks it.

### The header payload

```json
{
    "click": "button:has-text('API pricing')",
    "wait_ms": 3000,
    "viewport": "1440x900",
    "cookies": [
        {"name": "session", "value": "abc123", "domain": ".corp.internal"}
    ],
    "domains": ["price_per_mtok", "model_identifier"],
    "filters": "a3f7b2c1-9e4d-4f8a-b5c6-1234567890ab"
}
```

All fields are optional. For a simple public page extraction, the header
can be just `{}`. For an authenticated intranet page with custom filters,
it carries cookies from blobhttp's pre-flight and a GUID referencing
stashed filter bitmaps.

### How blobhttp config applies to Chromium's requests

A key concern: blobhttp has per-domain config (auth headers, rate budgets,
proxy settings) stored in DuckDB. How does that config apply when Chromium
is the HTTP client, not blobhttp?

**It applies automatically.** When Chromium requests `https://intranet.corp/data`
through the proxy, the proxy sees a normal request to `intranet.corp` and
applies its normal pipeline — the same rate-limiting, auth injection, and
logging it would apply to any client. The blobboxes addon doesn't need to
know about per-domain config because it's not the one making the outbound
requests. Chromium is, and Chromium's traffic flows through the proxy
where that config lives.

For SPNEGO specifically: blobhttp does the Kerberos negotiation (pre-flight),
obtains a session cookie or Negotiate token, and the client passes it to
the blobboxes addon via the header payload's `cookies` array. Chromium
sends the cookie on its requests. The proxy never generates SPNEGO hashes
— blobhttp already did that work.

## Domain Filter Loading

### By name

The `domains` array in the header payload lists well-known domain filter
names (e.g., `price_per_mtok`, `model_identifier`). The blobboxes addon
resolves these from a local blobfilters database — a DuckDB or SQLite
file that is a deployment artifact of the service, not something the
client ships per-request.

```json
{"domains": ["price_per_mtok", "model_identifier", "currency_code"]}
```

### By GUID (arbitrary filters with TTL)

For ad-hoc filters that don't exist in the well-known catalog, the client:

1. Builds the roaring bitmap (via blobfilters in DuckDB)
2. Serializes it (`bf_roaring_serialize(bitmap, 'portable')`)
3. Stashes it in OpenBao under a GUID with a TTL

```sql
SELECT blobhttp_vault_put(
    'secret/data/filters/' || gen_random_uuid(),
    bf_roaring_serialize(bitmap, 'portable'),
    ttl := '300s'
);
```

4. Passes the GUID in the header:

```json
{"filters": "a3f7b2c1-9e4d-4f8a-b5c6-1234567890ab"}
```

The blobboxes addon retrieves the bitmap bytes from OpenBao by GUID,
deserializes them, and injects them into the browser's isolated world.

**The TTL is key.** The stashed filter self-destructs after 5 minutes (or
whatever TTL the client sets). No cleanup, no garbage collection, no
leaked state. OpenBao logs every read and write, so the audit trail is
automatic. The client can retry the request (the filter persists until
TTL expiry) but doesn't accumulate orphaned state.

This makes the entire flow **stateless from the caller's perspective**:
build filter → stash → call → done. The ephemeral state self-destructs.

### As a reified function

The complete pipeline — build filter, stash with TTL, construct request
with header, send through proxy, get bboxes back, reshape response — can
be expressed as a **reified function**: a row in the `llm_adapter` table
(or a sibling table) where the request template, response JMESPath, and
domain config are all data, not code.

```sql
-- The entire extraction is a single SQL call
SELECT * FROM blobboxes_extract(
    url := 'https://mistral.ai/pricing',
    click := 'button:has-text(''API pricing'')',
    domains := ['price_per_mtok', 'model_identifier']
);
```

Under the hood, the reified function:
1. Builds roaring bitmaps from the named domains (blobfilters, SQL)
2. Serializes and stashes in OpenBao with TTL (blobhttp `vault_put`)
3. Constructs the HTTP request with `X-BLOBTASTIC-EXTRA-INFO` header
   (inja template from adapter config)
4. Sends through the proxy → blobboxes addon → Playwright renders →
   bboxes extracted → response returned
5. Reshapes the response via JMESPath (from adapter config)

The inja template builds the header JSON. JMESPath reshapes the bbox
array. No Python glue code — the pipeline is a SQL call that triggers
a browser render on the way through. The TTL ensures the reified function
is stateless and safe to retry, schedule, or call from any context.

## Security Model

### Trust boundaries

```
┌─────────────────────────────────────────────────────┐
│ Client (DuckDB session)                             │
│   - Owns the query intent                           │
│   - Does pre-flight auth (SPNEGO, OAuth, etc.)      │
│   - Decides which domains/filters to apply          │
│   - Sets TTL on ephemeral state                     │
├─────────────────────────────────────────────────────┤
│ Personal proxy (mitmproxy + blobboxes addon)        │
│   - Routes requests (normal vs blobboxes)           │
│   - Rate-limits outbound per target domain          │
│   - Logs all traffic for audit                      │
│   - Retrieves filter bitmaps from OpenBao by GUID   │
│   - Manages persistent Chromium browser pool        │
│   - Never generates auth credentials                │
├─────────────────────────────────────────────────────┤
│ Corporate proxy (optional upstream)                 │
│   - Enforces domain allowlists                      │
│   - DLP scanning                                    │
│   - TLS inspection (if policy requires)             │
│   - Centralized logging                             │
├─────────────────────────────────────────────────────┤
│ CDP isolated world (in Chromium)                    │
│   - Shares DOM with page, separate JS global scope  │
│   - Page scripts cannot detect or interfere         │
│   - No outbound network access from isolated world  │
│   - blobboxes JS reads the DOM, nothing else        │
└─────────────────────────────────────────────────────┘
```

### What would make an infosec team nervous (and mitigations)

1. **Server-side browser.** The service runs headless Chromium — same
   trust model as Jina Reader, Browserless, or any headless-Chrome-as-a-
   service. Mitigation: all traffic proxied, only authenticated callers
   can trigger navigation, URLs subject to proxy allowlists.

2. **SSRF potential.** A caller could request `http://169.254.169.254/`
   (cloud metadata endpoint) or internal services. Mitigation: proxy
   allowlist enforcement. The blobboxes addon doesn't need its own
   allowlist — the proxy's existing domain policy applies because the
   URL is the real URL.

3. **Credential leakage.** Pre-flighted cookies/tokens transit via header.
   Mitigation: the header is stripped before forwarding. Chromium
   receives cookies via browser context injection, not via HTTP headers
   on the wire. TLS between client and proxy protects the header in
   transit.

4. **Ephemeral state in OpenBao.** Stashed filter bitmaps are sensitive
   only in that they reveal what the client is looking for. Mitigation:
   TTL ensures auto-deletion. OpenBao ACLs restrict who can read
   `secret/data/filters/*`. Audit log records every access.

## Deployment

### Option A: separate process (current)

```
blobboxes HTTP (:8484)  ←  caller
        │
        ▼
Playwright (--proxy-server=:8080)
        │
        ▼
Personal proxy (:8080)  →  [corporate proxy]  →  internet
```

The blobboxes HTTP service runs as a standalone process. Callers connect
directly (or through the reverse proxy that also fronts Bifrost/OpenBao).
Default bind is `127.0.0.1` — callers should come through the reverse
proxy, not directly.

### Option B: mitmproxy addon (target)

```
Personal proxy (:8080)  ←  caller
    │
    ├── X-BLOBTASTIC-EXTRA-INFO header?  →  blobboxes addon  →  Playwright
    │
    └── normal request  →  forward (with rate-limiting, auth, logging)
```

The blobboxes logic runs as a mitmproxy addon inside the personal proxy.
One process, one port. The addon dispatches on the header presence.
Migration from A to B is straightforward — the core logic (`BrowserPool`,
bundle injection, bbox extraction) is identical. Only the HTTP dispatch
layer changes.

## Hash function compatibility

The FNV-1a hash used for token hashing in the browser must be identical
to the one in blobfilters (C). `Math.imul` is critical — JavaScript
numbers are IEEE 754 doubles, so bare multiplication would lose precision
for 32-bit integer arithmetic. The `>>> 0` ensures the result is
unsigned.

```javascript
function fnv1a(str) {
    let hash = 0x811c9dc5;  // FNV offset basis
    for (let i = 0; i < str.length; i++) {
        hash ^= str.charCodeAt(i);
        hash = Math.imul(hash, 0x01000193);  // FNV prime
    }
    return hash >>> 0;  // unsigned 32-bit
}
```

## Phasing

### Phase 1: Shared snapshot extraction (done)
- Extraction JS extracted into `browser/src/bbox.js`
- Built as self-contained IIFE via esbuild (`dist/blobboxes.lite.js`)
- `BrowserPool` with proxy support in `python/blobboxes/browser.py`
- Playwright and PySide6 controllers both load and inject shared bundle
- blobapi's `bbox_extract.py` re-exports from blobboxes for compatibility

### Phase 2: MutationObserver for interactive mode
- `browser/src/observer.js` with batched flush (implemented in bundle)
- PySide6 shim wires `QWebChannel` for push delivery
- Playwright shim uses `expose_function` for async delivery

### Phase 3: roaring-wasm domain classification
- Add roaring-wasm dependency, build full bundle with inlined WASM
- `browser/src/classify.js` with FNV-1a + bitmap probing (implemented)
- Filter loading by name (from local blobfilters DB) and by GUID
  (from OpenBao, with TTL)

### Phase 4: Proxy integration
- mitmproxy addon dispatching on `X-BLOBTASTIC-EXTRA-INFO` header
- `BrowserPool` inside the addon with `--proxy-server` pointing to self
  (sub-resource requests flow through normal proxy pipeline)
- Reified function wrapping the full pipeline as a SQL call

## Cross-references

- **blobboxes/docs/browser-table-extraction.md** — HTTP service design,
  API shape, Docker deployment
- **blobfilters/docs/browser-domain-matching.md** — roaring-wasm
  integration, portable serialization, FNV-1a compatibility
- **blobapi/docs/schema-driven-table-extraction.md** — variable-resolution
  schema matching, the broader table recovery vision
- **blobapi/bbox_extract.py** — thin re-export from blobboxes.browser
- **blobapi/pyside6_bbox_demo.py** — PySide6 controller using shared bundle
