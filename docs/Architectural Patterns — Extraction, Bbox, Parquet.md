# Architectural patterns — extraction / bbox / Parquet work

A synopsis of the patterns that recurred while designing the document-extraction →
interned-bbox → Parquet pipeline, written with a view to formalizing the reusable
ones for related and future work. Pattern-oriented (name, forces, where it showed up,
how it might generalize) rather than a narrative recap.

This revision adds the two large-scale foundations that emerged after the first
draft — **the Universal Address** (§1) and **Aboutness as a cheap surrogate** (§2) —
both of which turn out to be the same spine expressed at corpus scale.

---

## 0. The spine: decouple data from identity, location, and mechanism

The single principle under most of what follows: **never weld the data to (a) its
identity/name, (b) its location or source, or (c) the mechanism used to extract,
transport, or act on it.** Keep those as separable, late-bound concerns.

This is the Application-Level-Framing / SRM naming-independence lesson (Clark &
Tennenhouse; Floyd/Jacobson/McCanne et al., `wb`/`sdr` on the MBone): an Application
Data Unit is named persistently and independently of the sender and the transport, so
any holder can name, cache, request, and repair it by its own identity regardless of
who sent it or how. Transplanted here, the same idea governs how a parser takes input,
how values are keyed, how results are stored and reused — and, at the largest scale,
how every atom is *addressed* (§1) and how document *aboutness* is computed (§2). Most
patterns below are this principle applied at a different layer.

---

## 1. Foundation A — The Universal Address

**Everything reducible to: a value-or-formula, at a geometry, on a page, in a
document, in the soup.** Every atom of paginated material is addressed by

```
(doc_id, page_locus, region, payload)
```

a hierarchical locator bottoming out in content at a position. This is the corpus's
primary key and its universal join key. It is the spine's *name* facet made
hierarchical and spatial: the address identifies the atom independently of the
mechanism that extracted it.

**Structure universal, geometry variant.** The *address structure* is the same across
all formats; the *region geometry* is format-specific — an integer grid cell (xlsx), a
float rectangle (PDF/HTML), a character span (docx/text). This is why the INT-vs-DOUBLE
coordinate decision is *per-format*: it falls straight out of the model — universal
addressing, native geometry per source — rather than being an incidental optimization.
Fixed-layout formats carry true coordinates; reflowable formats (docx/html) carry
logical order with derived layout, so the region is *native* for some sources and
*synthetic* for others, but the address hierarchy is identical.

**Payload is a sum type: terminal or referential.** One arm is *terminal* — a literal
value or text run. The other is *referential* — a formula, a hyperlink, a
cross-reference. The referential arm matters because **every reference is an edge over
the address space**, so the corpus is not a bag of boxes but a **graph**: nodes are
addresses, edges are references. Excel formula dependencies are the cleanest instance;
the same covers hyperlinks, docx cross-references/TOC entries, footnote anchors, PDF
named destinations.

**Two structure signals over one address space.** Document structure is inferable from
(a) the *geometric/stylistic* signal — layout plus the formatting surrogates (bold,
borders, merges, named-style origin) — and (b) the *referential* signal — the parsed
payload graph. Structure inference is the fusion of both.

**Progressive typing ladder.** A payload is climbed only as far as you can or need:
opaque bytes → recognized typed value → parsed AST (e.g. a formula) → resolved
references-as-edges. "For some boxes we know how to parse them" = we climb higher for
those. This is lossy-by-contract and intern-then-interpret one level down.

---

## 2. Foundation B — Aboutness as a cheap surrogate

**What is this document / source-of-boxes / region-of-boxes *about*?** Two
complementary notions of aboutness ride over the address space:

- **Structural aboutness** — the reference graph from §1 (what depends on / points to
  what: inputs vs outputs, tables, totals, sections).
- **Semantic aboutness** — a *domain fingerprint*: a feature set, implemented as a
  **Roaring bitmap**, over an interned value/feature space.

### 2.1 The universal mechanism: type erasure to a shared surrogate space
Intern every value — int, string, date, decimal, anything — down to a **surrogate
integer over a shared dictionary**. Once done, *all* set membership and similarity is
uniform bitmap algebra (AND = intersection, OR = union, cardinality = overlap), and the
element's original type stops mattering. This is the styles' surrogate move — "compare
on the cheap integer, decode only to interpret" — promoted from *within one workbook*
to *across the whole corpus and the systems of record*. That promotion is what makes it
foundational: it is the spine at corpus scale.

### 2.2 Catalog-anchored distant supervision
Treat centralized relational databases as systems of record whose **catalogs are piles
of pre-labeled canonical sets**: primary-key domains, but also — often richer — lookup
/ dimension tables, enumerations, and foreign-key *targets*. Matching a document
region's value-set against these named domains labels the region with no human
annotation. Two precise handles for the formalization:
- The operation "is this region's value-set contained in domain D?" is **inclusion-
  dependency discovery** (data-profiling literature) — the thing that tells you a
  column of boxes *is* customer IDs because its values live inside a known key domain.
- The "unsupervised-supervised" texture is **distant / weak supervision**: the catalog
  supplies the labels; the matching against documents is unsupervised.

### 2.3 Scoring and compositionality
Scoring is all cheap set-ops: containment (⊆ a domain), overlap cardinality, Jaccard,
or *coverage* (fraction of the domain present in the region). "What is this region
about" = argmax over catalog domains of a containment/coverage score — ANDs and
popcounts. And because a Roaring bitmap **unions**, you fingerprint at *any* level of
the Universal Address — column, table, page, document, corpus slice — by OR-ing the
constituents, with identical algebra up and down the hierarchy. The aboutness
fingerprint nests exactly the way the address nests (§1).

### 2.4 Substrate fit
A Roaring bitmap serializes to a portable blob → it is a column. Domain masks stored as
blobs; set-ops run in a UDF or a bitmap-aware path. Kin to the Bloom filter proposed
for the cells table — a Bloom filter *is* a feature-presence bitset — so one primitive
serves two jobs (corpus domain-clustering and per-file membership pruning).

### 2.5 The crux: the shared canonical surrogate space is the hard part
The bitmap algebra is the easy part. **You can only AND two bitmaps that index the same
dictionary**, so the whole thing rests on a shared, canonical surrogate space — and
building it is the real work. Getting `"1,000"` and `1000`, an ISO date and an Excel
serial and a locale string, `"007"` and `7`, and at the limit `"Acme Inc."` and
`"ACME INCORPORATED"` to all intern to the *same* code is value canonicalization shading
into **entity resolution**. So type-agnostic comparison *downstream* is only possible
because of careful type-*aware* normalization *upstream* — canonicalize-at-the-boundary
(§4) in its hardest instance. The Roaring layer stays clean and type-blind no matter how
hard the normalization gets; the effort lives at the normalization boundary (handles:
semantic-type detection; blocking / MinHash-LSH for the fuzzy cases).

### 2.6 Honest limits
Bits capture *presence*, not frequency or weight; and hashing a large feature space into
fixed bits **aliases** (Bloom-style false positives). So the fingerprint is a **coarse
sieve, not a classifier** — blazing to shrink the soup to candidates, then refine
survivors with heavier methods (counts, TF-IDF, embeddings) only where the cheap layer
says two things are close. This is the intern-first / interpret-later discipline again.
Design question that carries the real weight: **what earns a bit.** For Excel there is
rich, already-parsed signal — the distinct number-format set, the function vocabulary,
header tokens, named ranges — each a natural bit source.

---

## 3. Patterns that are facets of the spine

**Path / Blob / Value polymorphism (input independence).**
An operation accepts *a path, a blob of bytes, or an already-parsed value* and behaves
identically; data is not tied to how it was located or delivered.
- *Where:* xlnt loading from path/istream/bytes; the "same bytes" hybrid (unzip once,
  streamer and metadata parser share one buffer); passing Arrow tables into DuckDB by
  value instead of by filename.
- *Toward formalization:* a `Source = Path | Bytes | ParsedHandle` at every stage
  boundary; a stage never re-fetches or re-parses what a caller can hand it.

**Name ≠ interpretation (surrogate/interning with deferred decode).**
Carry a small dense surrogate that preserves the *equivalence relation*; resolve to
*meaning* only when you must interpret. Same/different, cardinality, change-point run on
the surrogate.
- *Where:* `style_id` (and sheet/string ids) as the analysis primitive; decode only at
  interpret time. Generalized to full corpus set-membership in §2.
- *Toward formalization:* an `Interned<T>` pairing a surrogate space with a lazy decode
  table; the compare/explain boundary made explicit in the type.

**Content-addressed identity.**
Identity from the bytes (`SHA-256`), not from location or an assigned id — free dedup,
cache key, scope boundary.
- *Where:* SHA as `workbook_id`, dedup key, surrogate id-space boundary, and the handle
  for reusing a parsed workbook across a SQL batch.
- *Toward formalization:* content hash as the universal join key; path demoted to a mere
  *source*, never an identity.

**Values by construction, not codegen.**
Build typed values in memory, pass by value/reference; only the *locator* (filename) is
templated into a query — never the data.
- *Where:* Arrow builders / `duckdb::Value` / `from_pylist(rows, schema)` /
  `register(arrow_table)` instead of string-built `MAP {...}` SQL literals.
- *Toward formalization:* hard rule — data crosses the SQL boundary via binding / Arrow /
  registration; SQL text carries only mechanism. Injection-safety for free.

**Reuse via a safe handle, not re-derivation or a raw pointer.**
Reference an expensive intermediate by a stable *name* resolved at a controlled boundary
— not recomputed, not a bare address.
- *Where:* a table function keyed by SHA into a host-side parsed-workbook cache in
  preference to DuckDB's raw `POINTER`; temp tables / registered Arrow tables for
  row-shaped intermediates.
- *Toward formalization:* handles are content names into a registry with an explicit
  lifetime/eviction contract; raw pointers through a parallelizing engine are banned.

**Physical home by access pattern, not by ontology.**
Where data lives is decided by *how it is accessed*, not by what it "is."
- *Where:* bulk cells → compressed, sorted, prunable columns; small decode dicts →
  footer bag (opaque, self-contained, read-once) *or* nested typed columns (lazy,
  queryable); the "sparse/low-N ⇒ metadata, scales-with-N ⇒ column" rule; the
  single-schema-per-file constraint as the forcing function.
- *Toward formalization:* a placement decision function on (cardinality-vs-N,
  query-vs-carry, mutable-vs-fixed), independent of semantic category.

---

## 4. Orthogonal patterns (not facets of the spine, but load-bearing)

**Dumb hot path, smart cold path.**
Separate the O(N) bulk traversal (pure, single-pass, allocation-light byte walk) from
the low-N "gnarly" work (styles, merges, names, inheritance). Optimize the first for
throughput and safety, the second for correctness.
- *Where:* single-pass cell scan vs metadata parsers; only a second pass over `sheetData`
  is a regression (passes over small parts are free); the trap of dragging a full
  `load()` cell-walk in just to fetch metadata.
- *Toward formalization:* "count passes over the O(N) substrate; unbounded passes over
  O(1) data are not a smell." Bulk and metadata are different subsystems.

**Parallelism at the coarse grain; purity at the unit.**
Concurrency at the coarsest independent grain (per-file scan scheduling); the unit of
work pure (no shared mutable state), because purity is the *enabling condition* for safe
fan-out.
- *Where:* DuckDB fanning per-file scans across cores with zero parser threading;
  "concurrency-safe by construction"; the refusal to thread the parser; the skew caveat
  (tail bounded by the largest single unit).
- *Toward formalization:* composable parallelism = (pure unit + coarse independent grain),
  never threading the hot loop. Watch hidden process-global state (locale, lazy
  registries) that breaks unit purity.

**Discriminated union in columns.**
Encode a heterogeneous value as typed *sparse* columns plus a type tag — not stringly
typed, not opaque. Nulls near-free under RLE; predicates push down.
- *Where:* the cell value column (`vnum`/`vstr`/`vbool` + `cell_type`); per-format
  INT-vs-DOUBLE coordinate binding.
- *Toward formalization:* a standard columnar sum-type encoding; coordinate type as a
  per-variant bind, not a lowest-common-denominator cast.

**Lossy-by-contract licenses normalization.**
Pinning *what will never be needed* (here: we never reconstruct the source) unlocks
aggressive flattening, dropping, and canonicalization.
- *Where:* dropping XML scaffolding / calcChain / theme indirection; flattening styles
  while *keeping* the named-style link because it is signal; canonicalizing the surrogate
  space to restore same-id⇔same-thing.
- *Toward formalization:* state the "never needed" set at the top of a representation; it
  licenses every simplification below. Define the equivalence key deliberately — the key
  *is* the downstream meaning of "same."

**Canonicalize at the boundary.**
Establish the invariants downstream depends on *once*, at ingestion, so consumers inherit
the guarantee instead of re-deriving or mistrusting it.
- *Where:* write-time re-interning of redundant `xf` records so `different id ⇒ different
  style`; strings expanded then re-dictionaried by the Parquet encoder; and — hardest —
  the shared canonical value space of §2.5.
- *Toward formalization:* normalization is a trust-boundary operation; publish the
  invariant it guarantees alongside the artifact.

**Defang the third-party library behind a small, database-friendly C surface.**
Use a fat, good-at-its-job C/C++/Rust library for what it does well, but do not let its
large, idiosyncratic API leak into the bindings. Write a *small* set of C-callable
endpoints — the only surface the bindings ever see — that collapses the library's
exported complexity to a handful of stable functions. Structured results tunnel back as
**JSON through scalar functions**, because scalars compose (nest, filter, join) where
table functions do not, and JSON is the interchange every host already parses. The one
carve-out is the high-cardinality bulk (the cells), which goes through a flat, typed
*table* function for throughput — routing millions of rows through a JSON string is the
pathological case. So: **bulk → flat table-fn; everything low-N → JSON scalar**, and all
transformation lives in the C endpoint, never in a binding.
- *Where:* xlnt / PDFium / miniz / pugixml behind `bboxes_*_json` C endpoints; DuckDB,
  SQLite, and Python each merely *register* the same C functions (so cross-dialect
  consistency is free — change the C endpoint once, every binding inherits it); JSON
  tunnelling for doc/pages/fonts/styles/metadata/headers; the flat table fn only for cells.
- *Toward formalization:* a binding is a mechanism-adapter, never a logic site. A new
  host binding is a few dozen lines of registration. The endpoint set *is* the contract —
  keep it small, stable, and JSON-shaped; the third party's surface area never escapes it.

---

## 5. Methodological pattern

**Verify the substrate before you design on it.**
Every load-bearing assumption about the tools was checked against reality rather than
recalled: xlnt's cell/format internals from source, DuckDB's `KV_METADATA` behavior, that
the footer is *not* codec-compressed (16 KB blob stored verbatim), sort not being
automatic, cross-reader portability via pyarrow, the ~5× size result. Cheap checks
repeatedly overturned plausible-but-wrong priors before they reached the design.
- *Toward formalization:* a "substrate assumptions" checklist per external dependency,
  each entry cited to source/spec or backed by a throwaway probe.

---

## 6. Candidates for formalization / future work

Ranked by reuse leverage:

1. **The Universal Address** `(doc_id, page, region, payload)` as the corpus primary key,
   with *structure universal / geometry variant* and *payload = terminal | reference*.
   The reference arm defines the corpus-wide graph. This is the schema everything else
   hangs on.
2. **The shared canonical value space + normalization boundary** (§2.5) — the hard,
   high-value enabler for both surrogate equality and cross-source set membership; shades
   into entity resolution.
3. **Roaring domain fingerprints + inclusion-dependency matching** against catalog-
   anchored domains (distant supervision) — the type-blind, compositional aboutness
   mechanism; coarse sieve, then refine.
4. **The `Source = Path | Bytes | Handle` boundary + content-name handle registry** — the
   most literal, most library-shaped embodiment of the spine.
5. **`Interned<T>` (surrogate space + lazy decode) with an explicit compare/explain
   split**, plus canonicalize-at-boundary making surrogate equality sound and complete.
6. **The placement decision function** (access-pattern-driven physical home).
7. **The bulk/metadata subsystem split** and **pure-unit + coarse-grain** parallelism.
8. **The columnar sum-type encoding** with per-variant coordinate typing.

Open question worth naming: how much of the spine can be captured as *types* (so the
compiler enforces data ≠ identity ≠ mechanism, and address ≠ payload) versus how much
stays convention — and, relatedly, whether the Universal Address and the interned value
space are best expressed as a single library primitive.

---

*Provenance: synthesized across the blobboxes / xl_refract design sessions (multiple
Claude collaborators + Paul Harrington). Grounded against the implemented pipeline —
the Universal Address is the artifact row (`sha256` + `page_id` + `x/y/w/h` +
`text|formula`); the interned surrogate is `style_id` → `s_to_id` → `style_decode`;
the reference arm is `xlr_references` → the region graph.*
