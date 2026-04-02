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
