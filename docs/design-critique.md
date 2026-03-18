# blobboxes Browser Bundle: Design Critique & Rebuttals

An honest assessment of what might go wrong, what critics would attack,
and where the design is genuinely strong.

---

## Good Faith Objections

These are real concerns that we should address or acknowledge as
limitations.

### 1. The whitespace tokenizer is too naive

**The problem:** Splitting on `\s+` means "sweet potato" is two tokens
that independently match. "New York" becomes "new" (stop word, filtered)
and "york" (no match). Hyphenated terms like "stir-fry" work only if
the exact hyphenated form is in the vocabulary. Multi-word domain values
from database tables ("Mistral Large 3") become three independent tokens
with low individual scores.

**Why it matters:** The domain vocabularies come from database tables
where values are often multi-word. A column containing country names
("United States", "South Korea", "United Kingdom") will score poorly
because each word is probed independently and "united" alone isn't
distinctive.

**Mitigation:** Build the domain bitmaps with both the full value hash
AND individual token hashes. When classifying, also hash n-grams
(bigrams, trigrams) from the text. `"united states"` gets hashed as a
bigram alongside `"united"` and `"states"`. The bigram matches the
full-value hash in the domain bitmap. Cost: more hashes per bbox
(~3x for bigrams), still microseconds with roaring bitmaps.

**Status:** Not yet implemented. The current tokenizer works for
single-word domain values (currency codes, vegetable names). Multi-word
matching needs the n-gram extension.

### 2. FNV-1a hash collisions

**The problem:** FNV-1a is a 32-bit hash. With ~1000 tokens across all
domain bitmaps, birthday paradox gives ~0.01% collision probability per
pair. With millions of tokens on a large page, false positives from hash
collisions become non-negligible.

**Why it matters:** A collision means a random word on the page matches
a domain filter. The "IO" / Io (Jupiter's moon) collision in the
Wikipedia demo was actually a vocabulary overlap, not a hash collision —
but real hash collisions would be indistinguishable from true matches.

**Mitigation:** The score denominator helps — a single false-positive
token in a 20-token text node produces a score of 0.05, easily
thresholded. For high-precision use cases, a second-pass verification
step (exact string comparison of matched tokens against the domain
vocabulary) eliminates all hash collisions. This can be done SQL-side
after the initial bitmap probe.

**Status:** Not a problem in practice at current scale. Monitor if
domain vocabularies grow past ~10,000 tokens.

### 3. The 297 KB bundle is large for injection

**The problem:** The full bundle with roaring-wasm is 297 KB of
JavaScript. Injecting this via `Runtime.evaluate` on every page load
adds latency. For the PySide6 controller with `QWebEngineScript`
persistence, it's a one-time cost. For headless Playwright doing many
pages, it's per-context overhead.

**Why it matters:** If the service processes 100 pages/second, the
bundle injection becomes a bottleneck. The WASM initialization (base64
decode + `WebAssembly.instantiate`) is the expensive part.

**Mitigation:**
- The lite bundle (10 KB) is sufficient when classification happens
  server-side (Python/DuckDB). Only use the full bundle when
  browser-side classification is needed.
- Playwright's `browser.new_context()` can share browser instances.
  The bundle could be injected once per browser and re-used across
  contexts via `Page.addScriptToEvaluateOnNewDocument`.
- The WASM module could be pre-compiled and cached via
  `WebAssembly.compile()` if the execution context supports it.

**Status:** Not a bottleneck at current usage patterns (seconds per
page, not milliseconds). Optimize if throughput requirements increase.

### 4. Proxy loop risk in option B (mitmproxy addon)

**The problem:** The blobboxes addon lives inside mitmproxy. Playwright
is configured to use the same mitmproxy as its proxy. If something goes
wrong with header stripping or dispatch logic, Chromium's requests could
re-trigger the addon, creating an infinite loop.

**Why it matters:** A proxy loop would hang the process, exhaust memory,
and potentially crash the proxy — affecting all traffic, not just
blobboxes requests.

**Mitigation:** Defense in depth:
- The `X-BLOBTASTIC-EXTRA-INFO` header is stripped before Playwright
  navigates. Chromium never sends it on sub-resource requests.
- The addon should check a request-scoped flag (e.g., a second header
  `X-BLOBBOXES-INTERNAL: 1` added to Playwright's requests) to detect
  and drop re-entrant requests.
- A timeout on the addon's request handler prevents infinite hangs.
- Rate-limiting on the addon (max concurrent Playwright sessions)
  bounds resource usage.

**Status:** Option A (separate process) avoids this entirely. When
migrating to option B, implement the re-entrancy guard before anything
else.

### 5. DOM mutation observer can miss changes

**The problem:** `MutationObserver` fires on DOM mutations but not on:
- CSS changes that make elements visible/invisible without DOM changes
- `<canvas>` or WebGL rendering (no DOM text nodes)
- Shadow DOM changes in closed shadow roots
- `<iframe>` content (separate document)
- CSS `content` property changes (pseudo-elements)

**Why it matters:** Some pricing pages use canvas rendering, shadow DOM
components, or iframes for content. The TreeWalker won't find text in
these contexts.

**Mitigation:**
- For iframes: walk `document.querySelectorAll('iframe')` and extract
  from each iframe's `contentDocument` (same-origin only; cross-origin
  iframes are inaccessible by design).
- For shadow DOM: `element.shadowRoot` (open shadow roots only; closed
  roots are inaccessible by design — this is intentional encapsulation).
- For canvas: no DOM-based solution. Would need OCR or the canvas
  drawing commands. Out of scope — these pages need a different
  extraction strategy.
- For CSS visibility changes: periodic re-snapshot (poll every N ms)
  rather than relying solely on MutationObserver.

**Status:** The current implementation handles the common cases (React,
Next.js, Vue — all use real DOM nodes). Canvas-heavy pages are rare for
tabular data.

### 6. getClientRects() coordinates are viewport-relative

**The problem:** `Range.getClientRects()` returns coordinates relative
to the viewport, not the document. If the page has been scrolled, the
coordinates of off-screen elements may be incorrect or clipped. For long
pages (the Wikipedia demo was 27,545px tall), elements below the fold
have large positive y values that represent their document position only
when the page is at scroll position 0.

**Why it matters:** If extraction happens after scrolling (e.g., after
clicking a tab that scrolls the view), the coordinates of previously-
visible elements shift. Two snapshots at different scroll positions
produce inconsistent y values for the same element.

**Mitigation:**
- Always extract at scroll position 0, or normalize coordinates by
  adding `window.scrollX/Y` to produce document-relative positions.
- The current code doesn't add scroll offsets in the snapshot — this
  should be fixed. The highlight code already accounts for scroll
  (adding `scrollX/Y` to overlay positions).
- For very long pages, take multiple snapshots at different scroll
  positions and merge, deduplicating by text content + approximate
  position.

**Status:** Works correctly for pages that fit in the viewport or are
extracted without scrolling. Needs a fix for scroll-normalized
coordinates.

### 7. SPNEGO pre-flight credential lifetime

**The problem:** blobhttp does SPNEGO negotiation and obtains a session
cookie or Negotiate token. This credential is passed via the header
payload to Playwright. But Kerberos tickets and session cookies have
limited lifetimes (often 10-15 minutes for Negotiate tokens). If the
page render takes longer than the credential lifetime, sub-resource
requests will fail with 401.

**Why it matters:** Complex intranet pages with many API calls could
take 10+ seconds to fully render. If the SPNEGO token expires during
rendering, some resources fail silently, producing an incomplete page.

**Mitigation:**
- blobhttp should obtain a session cookie (persistent) rather than
  relying on per-request Negotiate tokens. Most SPNEGO-protected
  apps issue a session cookie after the initial authentication.
- The pre-flight should request a cookie, not just a token.
- Fallback: if sub-resources return 401, the addon could re-do
  the pre-flight and retry. But this adds complexity.

**Status:** Not yet tested with real SPNEGO-protected pages. The
architecture supports it; the credential lifecycle needs validation.

### 8. pyroaring vs CRoaring portable format compatibility

**The problem:** The demo uses `pyroaring.BitMap.serialize()` to produce
portable-format bitmaps that are deserialized by roaring-wasm in the
browser. Both claim to use the Roaring portable serialization format,
but they're different codebases (pyroaring wraps CRoaring via Cython;
roaring-wasm is an Emscripten compile of CRoaring). If there are
version skew issues or subtle format differences, deserialization could
fail silently or produce corrupt bitmaps.

**Why it matters:** A corrupt bitmap would produce wrong classification
results with no error — silent data corruption.

**Mitigation:**
- Both pyroaring and roaring-wasm wrap CRoaring, which has a
  well-specified portable format. Cross-implementation compatibility
  is a core design goal of the format.
- The end-to-end demo verified this: bitmaps built in pyroaring,
  deserialized in roaring-wasm, correct classification results.
- Add a checksum or round-trip test: serialize in Python, deserialize
  in JS, re-serialize in JS, compare bytes.

**Status:** Verified working in the demo. Should add a CI test that
round-trips bitmaps between pyroaring and roaring-wasm.

---

## Bad Faith Criticisms (and Rebuttals)

These are arguments someone might make to dismiss the approach, often
from a position of competing interests or superficial understanding.

### "You're just reinventing Jina Reader / Browserless / Puppeteer"

**The criticism:** There are existing headless browser services. Why
build another one? This is NIH syndrome.

**Rebuttal:** Jina Reader returns Markdown text. We return spatially-
located, domain-classified bounding boxes. That's a fundamentally
different output — it's the difference between OCR (text) and document
understanding (structure). Browserless gives you a browser; we give you
table candidates. The headless browser is an implementation detail, not
the product.

Also: Jina is an external SaaS. Try running your SPNEGO-protected
intranet page through it. The on-prem requirement isn't optional — it's
the entire reason this exists.

### "Just use an LLM to extract the tables"

**The criticism:** GPT-4V / Claude can look at a screenshot and extract
tables. Why bother with bounding boxes and roaring bitmaps?

**Rebuttal:** Cost, latency, and determinism. An LLM call to extract
one page costs ~$0.05 and takes ~5 seconds. Our pipeline costs ~$0.00
and takes ~10ms. For a monitoring job that checks 50 pricing pages every
hour, that's $60/day vs $0/day. The LLM also produces non-deterministic
output — same page, different JSON structure each time. Bitmap probing
is deterministic.

The LLM is the right tool for disambiguation *after* structural matching
has narrowed the candidate region. It's the last resort, not the first
step. Our pipeline finds the table; the LLM (if needed) interprets
ambiguous cells. The LLM sees 5 cells, not 200KB of HTML.

### "Roaring bitmaps are overkill for this"

**The criticism:** You have ~1000 tokens per domain. A simple Set or
hash table lookup would be faster to implement and fast enough.

**Rebuttal:** For individual token lookup, yes — `Set.has()` is fine.
But the power of roaring bitmaps is in set operations:
`andCardinality`, `orCardinality`, `jaccard`. The column-level reverse
probe — "build a bitmap from this column's tokens and intersect with
each domain" — is a single `andCardinality` call. With a Set, you'd
loop over every token and check membership. Same asymptotic complexity,
but the roaring operations are SIMD-optimized in WASM and operate on
compressed representations.

More importantly: the bitmaps are the same ones that blobfilters produces
on the database side. Same format, same hashes, same serialization. The
browser is just another consumer of the same data structure. Building a
parallel Set-based system would mean maintaining two implementations of
the domain filter — one for SQL, one for JS. The roaring bitmap IS the
interop format.

### "The proxy architecture is too complex"

**The criticism:** Personal proxy chaining to corporate proxy, header-
based dispatch, pre-flight auth stashed in OpenBao with TTL — this is
over-engineered. Just make direct HTTP requests.

**Rebuttal:** The complexity is not in our design — it's in the
enterprise environment. SPNEGO, mTLS, egress firewalls, DLP scanning,
audit logging — these exist whether or not we use them. Our design
routes through the existing proxy infrastructure instead of bypassing
it. The alternative — direct outbound from a headless browser running
on a server — is the thing that would get flagged by infosec.

The header-based dispatch is actually the *simplest* integration point.
No special URLs, no sentinel domains, no separate port. The proxy sees
a normal request with an extra header. Remove the header and it's a
normal request. The complexity is in the enterprise environment; our
design is a thin adapter to it.

### "Nobody will maintain the JS/WASM bundle"

**The criticism:** You've introduced npm, esbuild, and a WASM dependency
into a project that was pure C + Python + SQL. Who maintains this?

**Rebuttal:** The JS is 5 source files totaling ~250 lines. The build
script is 80 lines. The npm dependency is exactly one package
(roaring-wasm). This is not a React app with 1,500 transitive
dependencies.

The JS is also *stable* — the DOM APIs it uses (TreeWalker,
Range.getClientRects, MutationObserver, getComputedStyle) have been
unchanged since 2015. There is no framework churn here. The roaring-wasm
package wraps CRoaring, which has been stable for a decade.

The build step (`node build.js`) produces two static `.js` files that
are checked into the repo. If npm disappears tomorrow, the last-built
bundles still work. The build is a convenience, not a runtime
dependency.

### "The token classification scores are meaningless"

**The criticism:** A score of 0.25 means one token out of four matched.
That's not a confidence score — it's just a ratio. You can't threshold
on it reliably because it depends on text length.

**Rebuttal:** Correct — it's not a confidence score, it's a density
signal. And that's exactly what you want for table detection. A table
cell containing just "Broccoli" scores 1.0. A paragraph mentioning
broccoli once scores 0.05. The score directly measures "what fraction
of this text is about vegetables?" — which is the right question for
distinguishing table cells from prose.

The score IS length-dependent, and that's a feature. Short text with
high score = table cell. Long text with low score = prose. The
length-dependence encodes structural information. If you want a
length-independent score, use the column-level reverse probe, which
aggregates across all cells in a spatial column.

### "You're storing secrets (filter bitmaps) in a key-value store with TTL — that's Redis with extra steps"

**The criticism:** OpenBao for ephemeral bitmap storage is using a
secrets manager as a cache. Just use Redis.

**Rebuttal:** OpenBao gives us three things Redis doesn't: ACLs per
path, audit logging of every read/write, and auto-expiry that's
guaranteed (not best-effort like Redis `EXPIRE`). The filter bitmaps
aren't secrets, but they transit through the same infrastructure that
handles secrets (API keys, session tokens). One system, one audit trail,
one set of ACLs. Adding Redis would mean a second system to secure,
monitor, and operate — for what? Ephemeral storage of small byte arrays
with TTL. OpenBao already does this.

Also: the blobhttp extension already has OpenBao integration. Redis
would be a new dependency. The best infrastructure is the infrastructure
you already have.

---

## Genuine Weaknesses (no good rebuttal)

Honesty section — things that are genuinely hard and don't have clean
answers yet.

### Table detection across non-contiguous DOM regions

CSS grid and flexbox can place visually-adjacent cells in completely
unrelated DOM subtrees. Two `<div>` elements that appear side-by-side
in a pricing card might be in different branches of the DOM tree with
no common ancestor below `<body>`. The TreeWalker visits them in DOM
order, which might not match visual order. Spatial clustering by
coordinates is the only reliable signal, and that requires solving the
column-detection problem robustly.

### Responsive layouts

The same page at 1440px wide might have a 4-column pricing table. At
768px (tablet) it collapses to 2 columns. At 375px (mobile) it's a
vertical stack of cards. The bboxes are completely different at each
breakpoint. The domain labels are the same, but the spatial structure
isn't. A system that learns "the pricing table is at these coordinates"
is useless across breakpoints. Only domain-level classification
(which is coordinate-independent) generalizes.

### Dynamic pricing with client-side computation

Some pricing pages compute prices in JavaScript based on user
selections (usage tier, billing period, region). The displayed price
is never in the HTML source — it's calculated and rendered on the fly.
The MutationObserver catches the rendered result, but the extracted
price has no provenance — we can't tell if it's a base price, a
discounted price, or a price for a specific configuration without
understanding the page's UI state.

### Cross-origin iframes

Pricing widgets embedded in cross-origin iframes (e.g., Stripe
Checkout, embedded calculators) are completely inaccessible to our
extraction JS. The browser's same-origin policy prevents reading
their DOM. There is no workaround short of running a separate
extraction on the iframe's URL — which may not even make sense
outside the parent page's context.
