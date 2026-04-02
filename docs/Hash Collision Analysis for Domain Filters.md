# Hash Collision Analysis for Domain Filters

> How likely are false positives when probing tokens against roaring
> bitmap domain filters, and when should we consider 64-bit?

## Context

The [[blobfilters]] domain membership system hashes text tokens with
FNV-1a and probes the resulting 32-bit values against roaring bitmaps.
Each domain (e.g., "company_name", "country_code", "model_identifier")
is a roaring bitmap containing the hashed values of its known members.

The question: at what scale do hash collisions produce meaningful false
positives, and what would it cost to fix?

## False Positive Rate

For a domain with **k** members hashed into a **2^b** universe, the
false positive rate for a single random probe is:

```
FP_rate ≈ k / 2^b
```

This is the probability that a token NOT in the domain hashes to a value
that IS set in the bitmap. It's analogous to the fill ratio of a direct-
mapped Bloom filter with one hash function.

### 32-bit universe (current)

| Domain size (k) | FP rate / probe | Expected FPs / 10K probes |
|---|---|---|
| 1,000 | 0.000023% | 0.002 |
| 10,000 | 0.00023% | 0.023 |
| 50,000 | 0.0012% | 0.12 |
| 100,000 | 0.0023% | 0.23 |
| 1,000,000 | 0.023% | 2.3 |
| 10,000,000 | 0.23% | 23 |

### Cross-domain accumulation

When probing T tokens against D domains each with k members:

```
Expected total FPs = T × D × k / 2^32
```

Current scale (~86 domains, ~2,700 members avg, 1,000 token probes):

```
1,000 × 86 × 2,700 / 4,294,967,296 ≈ 0.054
```

Essentially zero. **32-bit is safe by a wide margin at current scale.**

### Planned common-vocabulary filter

A ~50K common English words filter adds one large domain:

```
10,000 probes × 1 domain × 50,000 / 2^32 ≈ 0.12 expected FPs
```

Still negligible. The common-vocab filter is well within 32-bit territory.

### Danger zone

False positives become noticeable with million-member domains:

- 1M-member domain, 100K probes: ~2,300 expected FPs
- 10M-member domain, 100K probes: ~23,000 expected FPs

This could matter for very large registries (e.g., all EDGAR company
names, all ISIN identifiers, all English words + proper nouns).

## 64-bit: overkill or insurance?

| Domain size (k) | 32-bit FP rate | 64-bit FP rate |
|---|---|---|
| 50,000 | 1.2 × 10⁻³ % | 2.7 × 10⁻¹² % |
| 1,000,000 | 2.3 × 10⁻² % | 5.4 × 10⁻¹¹ % |
| 10,000,000 | 2.3 × 10⁻¹ % | 5.4 × 10⁻¹⁰ % |

64-bit effectively eliminates false positives at any realistic domain
size. The costs:

| Factor | 32-bit | 64-bit | Impact |
|---|---|---|---|
| Memory per bitmap | ~40-80 KB @ 10K members | ~80-160 KB | Negligible |
| Probe speed | O(1), ~ns | O(1), ~ns (slightly slower two-level index) | Negligible |
| WASM bundle | roaring-wasm 32-bit (~300 KB) | 64-bit variant exists, larger | Moderate |
| Hash speed | FNV-1a-32: byte-at-a-time | FNV-1a-64: same speed, wider prime | Zero cost |

## Many Small Domains vs One Large Domain

The expected false positive count depends only on the **total set bits
probed against**, not how they're distributed across domains:

```
10 domains × 100K members × T probes / 2^32  =  T × 1,000,000 / 2^32
 1 domain  ×   1M members × T probes / 2^32  =  T × 1,000,000 / 2^32
```

The math is identical. But the **consequences** differ:

- **Many small domains**: false positives are independent. A token might
  false-match "us_state" but not "country_code". Downstream logic can
  filter by requiring agreement across multiple domains, checking that
  a match is semantically consistent with neighboring cells, or using
  containment score (what fraction of the token's n-grams match).
  Individual false matches are cheap to discard.

- **One mega-domain**: a false match is a single event against a broad
  category. There's no second opinion to cross-check against. The false
  positive carries more weight and is harder to filter.

This is an argument for **keeping domains granular** rather than merging
them into large catch-all filters. A "common_english_words" filter at
50K members is fine, but merging it with "company_names" (100K) and
"geographic_names" (200K) into a single 350K "known_text" filter would
lose the ability to distinguish *which* domain matched — and the false
positive rate would be 7× higher than any individual domain, with no
way to cross-check.

The design principle: **domains should be semantically cohesive**.
Membership in a domain should mean something specific enough that a
false positive is detectable by context.

## Set-Oriented Probing vs Single-Element Probing

The analysis above assumes single-element probes: hash one token, check
one bitmap. But the progressive masking pipeline and `bf_containment`
operate on **sets** — an entire column of values probed against a domain
bitmap at once. This changes the false positive story qualitatively, not
just quantitatively.

### How it works

Given a column of N values (e.g., all values in the "Region" column of
a table), hash all N into a **query bitmap** Q, then compute:

```
containment = |Q ∩ D| / |Q|
```

For a column that truly belongs to domain D: containment ≈ 1.0.
For a random column with no members in D: each hash in Q has probability
`p = k / 2^32` of colliding with a set bit in D.

### Why the improvement is exponential

The number of false matches in a random column follows
`Binomial(|Q|, p)`. The containment score concentrates tightly around
its expected value:

```
E[containment_false] = p = k / 2^32
Std[containment_false] = sqrt(p(1-p) / |Q|)
```

Any threshold significantly above p rejects false matches with
overwhelming probability. The false positive rate drops **exponentially**
with column size, not linearly:

| Domain size (k) | Single probe FP | 10-value set FP (threshold 0.5) | 20-value set FP |
|---|---|---|---|
| 10,000 | 2.3 × 10⁻⁴ % | ~10⁻⁵⁶ | ~10⁻¹¹² |
| 100,000 | 2.3 × 10⁻³ % | ~10⁻⁴⁶ | ~10⁻⁹² |
| 1,000,000 | 2.3 × 10⁻² % | ~10⁻³⁶ | ~10⁻⁷⁰ |

A single probe against a 1M-member domain gives a false positive once
every ~430K probes. A 20-element set probe at the same per-element rate
is essentially impossible to fool — the probability is ~10⁻⁷⁰.

### Implications

1. **32-bit is more than sufficient for set-oriented probes.** The bit
   width debate only matters for single-element probes. In the
   progressive masking pipeline, most classification happens at the
   column level via containment scores, so the 2^32 universe is
   effectively infinite.

2. **Small columns still benefit.** Even |Q| = 5 distinct values gives
   an exponential improvement over single probes. You don't need large
   columns to see the effect.

3. **Containment threshold is forgiving.** Because the false containment
   distribution is concentrated near zero, any threshold above ~0.05
   effectively eliminates false matches. The choice of 0.5 vs 0.8 vs
   0.95 is about how many *true* partial matches you want to accept, not
   about false positive control.

4. **This is the mathematical argument for the sieve architecture.**
   The progressive masking pipeline first detects table structure
   (grouping bboxes into columns), then probes columns as sets. The
   table detection step isn't just about structure recovery — it's
   about assembling the sets that make domain probing exponentially
   more reliable.

### Connection to `bf_containment`

The existing `bf_containment(text, filter_bitmap)` function in
[[blobfilters]] tokenizes the input text, hashes the tokens into a query
bitmap, and computes `|Q ∩ D| / |Q|`. This is exactly the set-oriented
probe described above. When used on a column of values (via
`bf_containment_json_normalized`), the same concentration-of-measure
effect applies — the containment score for non-matching columns clusters
near zero while matching columns score near 1.0.

## FNV-1a Quality

FNV-1a is adequate for this use case:

- **Distribution**: good on short strings (3-30 byte tokens). Avalanche
  is slightly incomplete on 1-2 byte inputs but this doesn't matter for
  natural-language tokens.
- **Speed**: byte-at-a-time processing. Slower than SIMD-optimized
  alternatives on long strings, but for typical tokens the difference
  is lost in memory access latency.
- **Simplicity**: trivial to implement in C, JavaScript, SQL. Important
  because the same hash must be computed in DuckDB extensions, SQLite
  extensions, Python, and browser WASM.

### Alternative hash functions

| Hash | Bits | Speed (short strings) | Quality | Notes |
|---|---|---|---|---|
| FNV-1a | 32 or 64 | ~1 ns/token | Good | Current. Portable, trivial to implement |
| xxHash (xxh32/xxh64) | 32 or 64 | ~0.5 ns/token | Excellent | Faster on longer strings. Widely available |
| wyhash | 64 native | ~0.3 ns/token | Excellent | Fastest non-crypto hash. 64-bit only |
| MurmurHash3 | 32 or 128 | ~0.7 ns/token | Excellent | Good 128→32 folding for dual-bit Bloom |

For the current use case, **the hash function matters much less than the
bit width**. All of the above have adequate distribution for domain
membership testing. The speed differences are sub-nanosecond and
irrelevant when the bottleneck is bitmap I/O.

## Recommendation

1. **Stay 32-bit for now.** Current domain sizes (max ~10K members) are
   well within safe territory. The planned 50K common-vocab filter is
   also fine.

2. **Monitor domain growth.** If any single domain exceeds ~500K members,
   reconsider. The signal: probe results that don't make sense (a token
   matching a domain it clearly doesn't belong to).

3. **If upgrading, go to 64-bit roaring first.** Switching hash functions
   (FNV-1a → xxHash) buys almost nothing in collision terms — it's the
   universe size that matters. FNV-1a-64 is a drop-in replacement with
   zero speed cost.

4. **Keep FNV-1a for portability.** The same hash must run in C (DuckDB/
   SQLite extensions), JavaScript (browser WASM), and Python. FNV-1a's
   simplicity (~10 lines in any language) is a real advantage over
   xxHash/wyhash which need platform-specific optimizations to be fast.

## References

- Birthday problem: `P(collision) ≈ 1 - e^(-n²/(2 × 2^b))`
- FNV-1a spec: http://www.isthe.com/chongo/tech/comp/fnv/
- Roaring bitmap format: https://roaringbitmap.org/
- 64-bit roaring: `Roaring64Map` in CRoaring, also available in
  roaring-wasm via `RoaringBitmap64`
