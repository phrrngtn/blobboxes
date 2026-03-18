# Browser Table Extraction (blobboxes/browser)

> Data starts in a table, gets rendered through frameworks and CSS into
> a visual layout. blobboxes reverses the pipeline — recovering tabular
> structure from rendered documents so the data can participate in JOINs.
>
> The browser is just another document source alongside PDF. The core
> abstraction is the same: documents have regions, regions have bounding
> boxes, bounding boxes have text and spatial coordinates.

## What it is

A proxy-integrated browser extraction service that dispatches on a
single HTTP header — **not** a Jina-style URL-rewriting gateway.

The client sends a normal HTTP request to the real URL through the
personal proxy. The presence of the `X-BLOBTASTIC-EXTRA-INFO` header
tells the proxy's blobboxes addon to render the page in headless
Chromium and return extracted data instead of raw HTML. No URL
embedding, no sentinel domain, no `/read?url=...` indirection.

```
GET https://mistral.ai/pricing HTTP/1.1
Host: mistral.ai
X-BLOBTASTIC-EXTRA-INFO: {"click":"button:has-text('API pricing')","domains":["price_per_mtok","model_identifier"]}

→ {
    "url": "https://mistral.ai/pricing",
    "bboxes": 463,
    "tables": [
      {
        "confidence": 0.92,
        "columns": ["model_identifier", "price_per_mtok", "price_per_mtok"],
        "headers": ["Model", "Input (/M tokens)", "Output (/M tokens)"],
        "rows": [
          {"model_identifier": "Mistral Large 3",
           "Input (/M tokens)": 0.5,
           "Output (/M tokens)": 1.5},
          ...
        ],
        "region": {"x": 153, "y": 458, "w": 1200, "h": 400}
      }
    ]
  }
```

**The URL is the real URL.** The proxy sees a request to `mistral.ai`
and applies `mistral.ai` policy — rate-limiting, allowlisting, auth
injection, logging — all without special cases. The header carries the
extraction config (click selectors, domains, cookies); the URL carries
the intent. This is why we don't need Jina-style `r.jina.ai/https://...`
URL embedding.

## Why not just use Jina Reader?

| | Jina Reader | blobboxes/browser |
|---|---|---|
| Dispatch | URL rewriting (`r.jina.ai/URL`) | Header on real URL (`X-BLOBTASTIC-EXTRA-INFO`) |
| Hosting | External SaaS | On-prem (proxy addon) |
| Proxy integration | None — separate hop | Native — addon inside the proxy |
| Rate limits | 20 RPM free | Your hardware, per-domain budgets via proxy |
| Auth/proxy | None | blobhttp (Vault, mTLS, SPNEGO) |
| Output | Markdown text | Structured table candidates with bboxes |
| Table detection | `X-Target-Selector: table` (CSS only) | Domain-aware schema matching (roaring bitmaps) |
| JS interaction | None (renders once) | Click tabs, scroll, wait for content |
| Domain filtering | None | Roaring bitmap probing against known schemas |
| PDF/Excel | No | Future: blobboxes for PDF, openpyxl for Excel |

Jina is great for the simple case (static HTML tables → markdown).
blobboxes/browser handles the hard cases: JS-rendered pages, tabbed content,
domain-aware extraction, authenticated sources. The key architectural
difference is dispatch by header, not URL — the proxy's existing
per-domain policy applies automatically because the URL is unmolested.

## Architecture

```
Client (DuckDB / Python / curl)
  │
  │  GET https://target-url/ with X-BLOBTASTIC-EXTRA-INFO header
  │
  ▼
Personal proxy (mitmproxy, :8080)
  │
  ├── Has X-BLOBTASTIC-EXTRA-INFO header?
  │     → blobboxes addon:
  │         ├── Playwright (render)
  │         │     Headless Chromium (--proxy-server=self)
  │         │     Tab clicks, wait conditions
  │         │
  │         ├── CDP isolated world (extract)
  │         │     addScriptToEvaluateOnNewDocument → auto-inject bundle
  │         │     TreeWalker + getClientRects → bboxes
  │         │     MutationObserver for dynamic content
  │         │
  │         ├── roaring-wasm (classify)
  │         │     Domain filters from local DB or OpenBao by GUID
  │         │     Token hashing + bitmap probing per bbox
  │         │
  │         └── Table recovery (assemble)
  │               Spatial clustering → grid detection → JSON tables
  │
  └── Normal request (no header)?
        → Forward as usual: rate-limit, auth injection, logging
```

Chromium's sub-resource requests (CSS, JS, fonts, API calls) flow back
through the same proxy without the special header — they get normal
rate-limiting and auth injection. No loop avoidance needed.

## API

### Header-based dispatch (primary — via proxy)

The client sends a normal request to the target URL through the proxy.
The `X-BLOBTASTIC-EXTRA-INFO` header carries extraction config as JSON:

```json
{
    "click": "button:has-text('API pricing')",
    "wait_ms": 3000,
    "viewport": "1440x900",
    "domains": ["price_per_mtok", "model_identifier"],
    "cookies": [{"name": "session", "value": "abc123", "domain": ".corp.internal"}],
    "filters": "a3f7b2c1-9e4d-4f8a-b5c6-1234567890ab",
    "format": "tables"
}
```

All fields are optional. For a simple public page extraction, `{}` suffices.

| Field | Type | Description |
|---|---|---|
| `click` | string | CSS selector to click before extraction |
| `wait_ms` | int | Extra wait time in ms (default: 3000) |
| `viewport` | string | `WxH` viewport size (default: `1440x900`) |
| `domains` | string[] | Domain filter names for classification |
| `cookies` | object[] | Cookies to inject (from blobhttp pre-flight auth) |
| `filters` | string | OpenBao GUID for ad-hoc filter bitmaps (with TTL) |
| `format` | string | `bboxes` (raw), `tables` (structured), `markdown` |

### Standalone HTTP service (development / Option A fallback)

For development without the proxy, the standalone HTTP service still works:

### `GET /read`

| Param | Type | Description |
|---|---|---|
| `url` | string | URL to fetch and render |
| `click` | string | CSS selector to click before extraction (optional) |
| `wait` | string | CSS selector to wait for (optional) |
| `wait_ms` | int | Extra wait time in ms (default: 3000) |
| `domains` | string | Comma-separated domain names for filtering (optional) |
| `format` | string | `bboxes` (raw), `tables` (structured), `markdown` |
| `viewport` | string | `WxH` viewport size (default: `1440x900`) |

### `GET /domains`

List available domain filters (loaded from database or YAML).

### `POST /classify`

Classify pre-extracted bboxes against domain filters (for when the
client does its own rendering).

### `GET /health`

Health check.

## Response format: columnar JSON

The response uses a **columnar layout**: each bbox field is an array,
not repeated per-row as an object key. This eliminates the key
repetition that dominates row-oriented JSON — a 463-bbox response
drops from ~90 KB to ~35 KB.

```json
{
  "url": "https://mistral.ai/pricing",
  "n": 463,
  "extraction_ms": 5230,
  "text":        ["Mistral Large 3", "$2.00", "$15.00", "..."],
  "x":           [153.2, 580.0, 580.0, "..."],
  "y":           [458.0, 458.0, 476.0, "..."],
  "w":           [120.5, 40.0, 45.0, "..."],
  "h":           [18.0, 18.0, 18.0, "..."],
  "font_family": ["Inter", "Inter", "Inter", "..."],
  "font_size":   [14.0, 14.0, 14.0, "..."],
  "font_weight": ["400", "400", "400", "..."],
  "color":       ["rgb(0,0,0)", "rgb(0,0,0)", "rgb(0,0,0)", "..."],
  "tag":         ["span", "td", "td", "..."],
  "cls":         ["model-name", "price", "price", "..."],
  "t_ms":        [1234.56, 1235.01, 1235.12, "..."],
  "mutation_type": ["snapshot", "snapshot", "snapshot", "..."]
}
```

When `domains` are requested, the response includes a `matches` array
parallel to the bbox columns. Each element is the list of domain
matches for that bbox (empty array if no match):

```json
{
  "url": "https://mistral.ai/pricing",
  "n": 463,
  "extraction_ms": 5230,
  "text":    ["Mistral Large 3", "$2.00", "$15.00", "some noise text"],
  "x":       [153.2, 580.0, 580.0, 800.0],
  "y":       [458.0, 458.0, 476.0, 10.0],
  "w":       [120.5, 40.0, 45.0, 90.0],
  "h":       [18.0, 18.0, 18.0, 12.0],
  "matches": [
    [{"domain": "model_identifier", "score": 1.0}],
    [{"domain": "price_per_mtok", "score": 0.5}],
    [{"domain": "price_per_mtok", "score": 0.5}],
    []
  ]
}
```

All arrays have length `n`. Position `i` across all arrays describes
the same bbox. This is the natural shape for DuckDB consumption —
`json_extract` pulls a whole column as an array, `unnest` turns it
into rows.

**Why columnar, not row-oriented:**

- A bbox has 13 fields. Row-oriented JSON repeats all 13 key strings
  per bbox. On a 463-bbox page that's ~6,000 redundant key copies.
  Columnar: 13 keys total, regardless of bbox count.
- DuckDB's `unnest` + `json_extract` works directly on parallel
  arrays: `SELECT unnest(json_extract(resp, '$.text')) AS text,
  unnest(json_extract(resp, '$.x')) AS x, ...` — no per-row parsing.
- gzip compresses columnar better (adjacent values of the same type
  have higher entropy coherence than interleaved heterogeneous fields).

**TODO:** A `select` field in the header could let callers request only
a subset of columns (e.g., `"select": ["text", "x", "y", "matches"]`),
reducing response size further. Not yet implemented — all columns are
returned.

## How it connects to SQL

From DuckDB, blobboxes/browser is a normal `bh_http_get` call to the
real URL — the proxy header triggers extraction. blobhttp routes the
request through the personal proxy where the blobboxes addon intercepts
it, and the columnar JSON response is unnested into rows.

### DuckDB session setup

Three things need to be running before the DuckDB session:

1. **The blobboxes proxy** (terminal 1):
   ```bash
   cd ~/checkouts/blobboxes
   uv run python -m blobboxes.run_proxy -p 8080
   ```

2. **OpenBao** (terminal 2, optional — only needed if using vault-managed
   API keys for other blobhttp calls in the same session):
   ```bash
   bao server -dev -dev-root-token-id=dev-blobapi-token \
       -dev-listen-address=127.0.0.1:8200
   ```

3. **DuckDB** (terminal 3):
   ```bash
   duckdb -unsigned
   ```

Then in DuckDB, load blobhttp and configure it to route through the
proxy:

```sql
-- Load blobhttp extension (unsigned because it's a local build)
LOAD '~/checkouts/blobhttp/build/release/extension/bhttp/bhttp.duckdb_extension';

-- Route all blobhttp traffic through the blobboxes proxy.
-- verify_ssl=false because mitmproxy uses its own CA for HTTPS.
-- For production, use ca_bundle pointing to ~/.mitmproxy/mitmproxy-ca-cert.pem
SET VARIABLE bh_http_config = MAP {
    'default': '{"proxy": "http://localhost:8080", "verify_ssl": false}'
};
```

If OpenBao is running and you need vault-managed API keys for other
services in the same session:

```sql
SET VARIABLE bh_http_config = MAP {
    'default': '{"proxy": "http://localhost:8080",
                 "verify_ssl": false,
                 "vault_addr": "http://127.0.0.1:8200",
                 "vault_token": "dev-blobapi-token"}'
};
```

### Raw bh_http_get call

The extraction header is passed via blobhttp's `headers` parameter:

```sql
SELECT bh_http_get(
    'http://example.com',
    headers := '{"X-BLOBTASTIC-EXTRA-INFO": "{}"}'
).response_body::JSON AS resp;
```

The response is a single JSON value containing the columnar arrays
(`text[]`, `x[]`, `y[]`, etc.). To get rows, unnest the parallel
arrays:

```sql
WITH RAW AS (
    SELECT bh_http_get(
        'https://mistral.ai/pricing',
        headers := json_object(
            'X-BLOBTASTIC-EXTRA-INFO', json_object(
                'click', 'button:has-text(''API pricing'')'))
    ).response_body::JSON AS resp
)
SELECT unnest(from_json(resp->'$.text', '["varchar"]')) AS text,
       unnest(from_json(resp->'$.x', '["double"]')) AS x,
       unnest(from_json(resp->'$.y', '["double"]')) AS y,
       unnest(from_json(resp->'$.w', '["double"]')) AS w,
       unnest(from_json(resp->'$.h', '["double"]')) AS h,
       unnest(from_json(resp->'$.font_size', '["double"]')) AS font_size,
       unnest(from_json(resp->'$.tag', '["varchar"]')) AS tag
FROM RAW;
```

The URL is `https://mistral.ai/pricing`, not
`http://localhost:9090/read?url=https://mistral.ai/pricing`. The proxy
sees a request to `mistral.ai` and applies its normal per-domain
policy. The header is the only signal that extraction is requested.

## Setup

### Prerequisites

From the `blobboxes` checkout:

```bash
# 1. Install Python dependencies (mitmproxy, playwright)
uv pip install mitmproxy playwright

# 2. Install Chromium for Playwright (one-time)
uv run python -m playwright install chromium

# 3. Build the JS extraction bundle
cd browser && node build.js && cd ..
# → browser/dist/blobboxes.lite.js  (~10 KB, snapshot only)
# → browser/dist/blobboxes.bundle.js (~310 KB, with roaring-wasm)
```

Verify the install:

```bash
uv run python -c "
from blobboxes.browser import BrowserPool, to_columnar
print('imports OK')
"
```

### Checking for port conflicts

Before starting any service, check that the port is free:

```bash
lsof -i :8080 -P    # check port 8080
lsof -i :8484 -P    # check port 8484
```

Docker commonly binds to port 8080. If it's in use, pick another port
(e.g., 9080) and adjust the commands below.

## Deployment

### Option B: mitmproxy addon (primary)

The blobboxes addon runs inside the personal proxy. One process, one
port. No separate service to manage.

```
Personal proxy (:8080)  ←  caller
    │
    ├── X-BLOBTASTIC-EXTRA-INFO header?  →  blobboxes addon  →  Playwright
    │                                         (--proxy-server=self)
    │
    └── normal request  →  forward (rate-limiting, auth, logging)
```

#### Starting the proxy

The addon requires the venv's Python (for `blobboxes`, `playwright`,
and `mitmproxy` packages). A launcher module avoids the need to locate
the addon script manually:

```bash
# Default: port 8080, max 4 concurrent pages
uv run python -m blobboxes.run_proxy

# Custom port and concurrency
uv run python -m blobboxes.run_proxy -p 9080 --set blobboxes_max_pages=2

# Composed with the rule4 traffic observer
uv run python -m blobboxes.run_proxy -p 8080 \
    -s rule4/proxy_addon.py \
    --set rate_limit=5
```

The launcher automatically prepends `-s python/blobboxes/proxy_addon.py`
and defaults to port 8080 if `-p` is not given. All other arguments are
passed through to `mitmdump`.

On successful startup you'll see:

```
Loading script .../proxy_addon.py
BlobboxesAddon: pool created, max_pages=4, timeout=30s, ...
HTTP(S) proxy listening at *:8080.
```

#### Stopping the proxy

Ctrl+C in the terminal. The addon's `done()` hook closes the Playwright
browser and cleans up. Alternatively, `kill <pid>`.

#### mitmproxy options

| Option | Type | Default | Description |
|---|---|---|---|
| `blobboxes_max_pages` | int | 4 | Maximum concurrent Playwright pages |
| `blobboxes_timeout` | int | 30 | Seconds to wait for a slot before 503 |
| `blobboxes_bundle` | str | `full` | `full` (with roaring-wasm) or `lite` |

#### Concurrency model

An `asyncio.Semaphore` bounds the number of concurrent Playwright pages.
When all slots are busy, incoming extraction requests wait up to
`blobboxes_timeout` seconds; if no slot frees up, they get a 503
response. Normal passthrough requests are unaffected.

Chromium's own sub-resource requests (CSS, JS, fonts, images) carry the
`X-BLOBBOXES-INTERNAL` header and pass through the proxy without
consuming an extraction slot or triggering the addon.

#### Testing the addon

```bash
# Terminal 1 — start the proxy:
uv run python -m blobboxes.run_proxy -p 8080

# Terminal 2 — normal passthrough (no header → raw HTML):
curl -x http://localhost:8080 http://example.com
# → HTML of example.com

# Terminal 2 — extraction request (header → columnar JSON):
curl -x http://localhost:8080 \
  -H 'X-BLOBTASTIC-EXTRA-INFO: {}' \
  http://example.com
# → {"url": "http://example.com/", "n": 3, "extraction_ms": 5524.7,
#    "text": ["Example Domain", ...], "x": [288, ...], ...}

# Terminal 2 — extraction with click selector:
curl -x http://localhost:8080 \
  -H 'X-BLOBTASTIC-EXTRA-INFO: {"click":"button:has-text(\"API pricing\")"}' \
  https://mistral.ai/pricing

# Terminal 2 — invalid header JSON → 400:
curl -x http://localhost:8080 \
  -H 'X-BLOBTASTIC-EXTRA-INFO: not json' \
  http://example.com
# → {"error": "Invalid JSON in X-BLOBTASTIC-EXTRA-INFO: ..."}
```

**What to look for in the mitmdump log:**

```
# Client's extraction request:
[::1]:58850: GET http://example.com/
          << 200 OK 621b

# Chromium's sub-resource request (passthrough, no extraction):
127.0.0.1:58851: GET http://example.com/
              << 200 OK 371b
```

The `127.0.0.1` source is Chromium (routed through `--proxy-server=self`).
The `[::1]` source is the original curl client. Only the client request
triggers extraction; Chromium's requests pass through normally.

### Option A: standalone HTTP service (development fallback)

For development without the proxy. Does not require mitmproxy.

#### Starting

```bash
# Without proxy (direct outbound — development only):
uv run python -m blobboxes.http_controller --port 8484

# With proxy (Chromium traffic routed through personal proxy):
uv run python -m blobboxes.http_controller --port 8484 --proxy http://localhost:8080
```

On successful startup:

```
blobboxes HTTP server on 127.0.0.1:8484, proxy=(direct)
```

#### Stopping

Ctrl+C or `kill <pid>`. The `finally` block in `serve()` closes the
Playwright browser.

#### Testing

```bash
# Health check:
curl http://localhost:8484/health
# → {"status": "ok"}

# Extract bboxes:
curl 'http://localhost:8484/read?url=http://example.com'
# → columnar JSON: {"url": "...", "n": 3, "extraction_ms": ..., "text": [...], ...}

# With click selector:
curl 'http://localhost:8484/read?url=https://mistral.ai/pricing&click=button:has-text("API pricing")'

# Missing URL → 400:
curl http://localhost:8484/read
# → {"error": "url parameter required"}
```

#### HTTP endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/read?url=...` | GET | Extract bboxes. Returns columnar JSON. |
| `/health` | GET | Health check. Returns `{"status": "ok"}`. |

| `/read` param | Type | Default | Description |
|---|---|---|---|
| `url` | string | (required) | URL to fetch and render |
| `click` | string | none | CSS selector to click before extraction |
| `wait` | int | 3000 | Wait time in ms after page load / click |
| `width` | int | 1440 | Viewport width |
| `height` | int | 900 | Viewport height |

The standalone service is a development convenience. In production,
the proxy addon is the deployment target — it eliminates the extra
hop and gets header-based routing for free.

## SQL macro (reified function)

The `blobboxes_extract` table macro wraps the full pipeline as a single
SQL call. It constructs the `X-BLOBTASTIC-EXTRA-INFO` header, calls
`bh_http_get` through the proxy, and unnests the columnar response into
rows. Requires blobhttp loaded and the blobboxes proxy running.

```sql
CREATE OR REPLACE MACRO blobboxes_extract(
    url, click := NULL, wait_ms := 5000, viewport := '1440x900'
) AS TABLE (
    WITH RAW AS (
        SELECT bh_http_get(
            url,
            headers := json_object(
                'X-BLOBTASTIC-EXTRA-INFO',
                json_object('click', click, 'wait_ms', wait_ms,
                            'viewport', viewport))
        ).response_body::JSON AS resp
    )
    SELECT unnest(from_json(resp->'$.text', '["varchar"]')) AS text,
           unnest(from_json(resp->'$.x', '["double"]')) AS x,
           unnest(from_json(resp->'$.y', '["double"]')) AS y,
           unnest(from_json(resp->'$.w', '["double"]')) AS w,
           unnest(from_json(resp->'$.h', '["double"]')) AS h,
           unnest(from_json(resp->'$.font_family', '["varchar"]')) AS font_family,
           unnest(from_json(resp->'$.font_size', '["double"]')) AS font_size,
           unnest(from_json(resp->'$.font_weight', '["varchar"]')) AS font_weight,
           unnest(from_json(resp->'$.color', '["varchar"]')) AS color,
           unnest(from_json(resp->'$.tag', '["varchar"]')) AS tag,
           unnest(from_json(resp->'$.cls', '["varchar"]')) AS cls
    FROM RAW
);
```

Usage:

```sql
-- Simple extraction (no interaction):
SELECT * FROM blobboxes_extract('http://example.com');
-- ┌────────────────┬────────┬────────┬────────┬────────┬─────────────┬───────────┬─────────────┬──────────────────┬─────┬─────┐
-- │      text      │   x    │   y    │   w    │   h    │ font_family │ font_size │ font_weight │      color       │ tag │ cls │
-- ├────────────────┼────────┼────────┼────────┼────────┼─────────────┼───────────┼─────────────┼──────────────────┼─────┼─────┤
-- │ Example Domain │  288.0 │  135.0 │  186.4 │   28.0 │ system-ui   │      24.0 │ 700         │ rgb(0, 0, 0)     │ h1  │     │
-- │ This domain... │  288.0 │  179.1 │  746.7 │   18.0 │ system-ui   │      16.0 │ 400         │ rgb(0, 0, 0)     │ p   │     │
-- │ Learn more     │  288.0 │  213.1 │   82.0 │   18.0 │ system-ui   │      16.0 │ 400         │ rgb(51, 68, 136) │ a   │     │
-- └────────────────┴────────┴────────┴────────┴────────┴─────────────┴───────────┴─────────────┴──────────────────┴─────┴─────┘

-- With click selector (e.g. click a tab before extracting):
SELECT * FROM blobboxes_extract(
    'https://mistral.ai/pricing',
    click := 'button:has-text(''API pricing'')'
);

-- Filter to just headings:
SELECT text, font_size, font_weight
FROM blobboxes_extract('https://anthropic.com/pricing')
WHERE tag IN ('h1', 'h2', 'h3');
```

### Complete end-to-end session

```bash
# Terminal 1: start the blobboxes proxy
cd ~/checkouts/blobboxes
uv run python -m blobboxes.run_proxy -p 8080

# Terminal 2: DuckDB
duckdb -unsigned
```

```sql
-- Load blobhttp, configure proxy
LOAD '~/checkouts/blobhttp/build/release/extension/bhttp/bhttp.duckdb_extension';
SET VARIABLE bh_http_config = MAP {
    'default': '{"proxy": "http://localhost:8080", "verify_ssl": false}'
};

-- Define the macro
CREATE OR REPLACE MACRO blobboxes_extract(
    url, click := NULL, wait_ms := 5000, viewport := '1440x900'
) AS TABLE (
    WITH RAW AS (
        SELECT bh_http_get(
            url,
            headers := json_object(
                'X-BLOBTASTIC-EXTRA-INFO',
                json_object('click', click, 'wait_ms', wait_ms,
                            'viewport', viewport))
        ).response_body::JSON AS resp
    )
    SELECT unnest(from_json(resp->'$.text', '["varchar"]')) AS text,
           unnest(from_json(resp->'$.x', '["double"]')) AS x,
           unnest(from_json(resp->'$.y', '["double"]')) AS y,
           unnest(from_json(resp->'$.w', '["double"]')) AS w,
           unnest(from_json(resp->'$.h', '["double"]')) AS h,
           unnest(from_json(resp->'$.font_family', '["varchar"]')) AS font_family,
           unnest(from_json(resp->'$.font_size', '["double"]')) AS font_size,
           unnest(from_json(resp->'$.font_weight', '["varchar"]')) AS font_weight,
           unnest(from_json(resp->'$.color', '["varchar"]')) AS color,
           unnest(from_json(resp->'$.tag', '["varchar"]')) AS tag,
           unnest(from_json(resp->'$.cls', '["varchar"]')) AS cls
    FROM RAW
);

-- Extract
SELECT * FROM blobboxes_extract('http://example.com');
```

### Stopping everything

1. Exit DuckDB (`.exit` or Ctrl+D)
2. Stop the proxy (Ctrl+C in terminal 1)
3. Stop OpenBao if running (Ctrl+C or `brew services stop openbao`)

## What makes it a blob* extension

blobboxes/browser follows the blob family pattern:

- **Core logic in C/WASM**: roaring bitmap operations (via roaring-wasm)
  and spatial analysis (future: via blobboxes compiled to WASM)
- **Multiple wrappers**: HTTP API (primary), Python library, potentially
  DuckDB table function
- **Composable with siblings**: uses blobhttp for fetching, blobfilters
  for classification, blobboxes for spatial analysis, blobtemplates for
  JMESPath reshaping of results
- **Data-driven configuration**: domain filters, click selectors, and
  wait conditions come from the database (providers.yaml / llm_provider
  table), not hardcoded

## The journey of a price

To illustrate the full round trip:

1. **Origin**: Someone at Anthropic types `$5 / MTok` into a CMS
2. **Rendering**: Next.js RSC serializes it, React hydrates it,
   CSS positions it at (461, 644) in a card grid
3. **blobboxes/browser**: Chromium renders the page, TreeWalker finds the
   text node, getClientRects returns the bbox, FNV-1a hashes the
   tokens, roaring bitmap matches `price_per_mtok` domain
4. **Table recovery**: Spatial clustering groups it with "Mistral
   Large 3" (above, same x-column) and "Output (/M tokens) $1.5"
   (below, same card)
5. **SQL**: `SELECT * FROM blobboxes/browser_tables WHERE domain = 'price_per_mtok'`
6. **JOIN**: The price participates in a cost accounting query
   alongside `_meta.prompt_tokens` from an LLM call

The data is home.

## Cross-references

- **blobapi/bbox_extract.py**: working Playwright prototype of the
  TreeWalker + getClientRects extraction
- **blobapi/browser_extract.py**: headless browser + LLM extraction
  (current pricing scraper implementation)
- **blobapi/docs/schema-driven-table-extraction.md**: the broader
  vision for variable-resolution schema matching
- **blobfilters/docs/browser-domain-matching.md**: roaring-wasm
  domain classification design
- **blobhttp/docs/dev-setup.md**: proxy, auth, rate limiting setup
