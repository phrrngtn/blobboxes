# Application Architecture — reduce every document to a long, narrow table of boxes

**Domain-specific.** This is the product bet unique to `blobboxes`: a spreadsheet, a PDF, a
Word file, a text file all collapse into **one universal, narrow row shape — a box — and
structure, meaning, and lineage are recovered by *query* over that table rather than baked
into per-format schemas.** The *how-it-is-built* substrate lives in the companion
[System Architecture](System%20Architecture%20—%20The%20blob%20Chassis.md); this document is
the *what-it-models*.

---

## 1. The Universal Address

Every atom of paginated material is addressed by

```
(doc_id, page_locus, region, payload)
```

a hierarchical locator bottoming out in content at a position — the corpus's primary key
and universal join key.

- **Structure universal, geometry variant.** The address hierarchy is identical across
  formats; only the *region geometry* is native to each — an integer grid cell (xlsx), a
  float rectangle (PDF/HTML), a character span (docx/text). This is *why* the INT-vs-DOUBLE
  coordinate decision is per-format: it falls out of the model, not from an optimization.
  Fixed-layout formats carry true coordinates; reflowable ones carry logical order with
  derived layout (a docx paragraph is a text span, `w = len(text)`, exactly like the text
  reader) — region *synthetic* for some sources, *native* for others, address identical.
- **Payload is a sum type: terminal or referential.** A literal value/text run, *or* a
  formula / hyperlink / cross-reference. The referential arm makes the corpus a **graph**
  (§4).
- **Progressive typing ladder.** A payload is climbed only as far as needed: opaque bytes →
  recognized typed value (`cell_type` + `vnum`/`vstr`/`vbool`) → parsed AST (a formula) →
  resolved references-as-edges.

## 2. The long, narrow table of boxes (the universal-relation bet)

The bet: **one narrow schema for everything**, and *not* modelling each format's structure.
Flatten to `(address, value)` rows and **recover structure by query** — `make_consistent`
and `region_graph` are SQL over the box table, not bespoke parsers. Structure is a
*projection*, late-bound, the same late-binding discipline as the spine (and, further out,
the projectional-editor idea): the schema does not encode the structure; queries do.

The three properties each buy something concrete:
- **Long** (billions of rows) → the columnar / min-max-pruning / content-addressed-artifact
  machinery earns its keep.
- **Narrow** (a handful of typed columns) → the discriminated-union value encoding (§6),
  not a wide per-format schema zoo.
- **Uniform** (all documents → the same row) → the whole corpus is *one queryable relation*.

## 3. Name ≠ interpretation — the domain interning

Carry a small dense surrogate that preserves the *equivalence relation* of a thing; resolve
to its *meaning* only when you must interpret. `style_id → s_to_id → style_decode`: "are
these two cells the same style?" is an integer compare; "is it bold / currency / a named
style?" is a decode paid only at interpret time. Canonicalize the surrogate space at write
time so `different id ⇔ different thing` holds corpus-wide.

## 4. The reference graph

Every reference in a payload is an **edge over the address space**, so the corpus is not a
bag of boxes but a graph: nodes are addresses, edges are references. Excel formula
dependencies are the cleanest instance (built: `xlr_references → region_graph`); hyperlinks,
docx cross-references/TOC, footnote anchors, and PDF named destinations are the *same arm*,
awaiting extraction — cheap, because the address space to point into already exists.

## 5. Aboutness

Two complementary notions of "what is this region / document / corpus-slice about?" ride
over the address space:

- **Structural aboutness** — the reference graph (§4): inputs vs outputs, tables, totals,
  sections.
- **Semantic aboutness** — a *domain fingerprint*: intern every value to a surrogate integer
  over a shared dictionary, and set-membership/similarity becomes uniform **Roaring-bitmap**
  algebra (AND/OR/popcount), type-blind. Match a region's value-set against **catalog-anchored
  domains** (a relational catalog's primary-key domains, dimension/lookup tables, FK targets
  — pre-labeled canonical sets) — "is this set contained in domain D?" is *inclusion-dependency
  discovery*; the labels come from the catalog, the matching is unsupervised (*distant
  supervision*). The fingerprint nests exactly the way the address nests: OR the constituents
  to fingerprint a column, table, page, document, or corpus slice with identical algebra.

**The crux is the shared canonical value space, not the algebra.** You can only AND two
bitmaps indexing the same dictionary, so getting `"1,000"` = `1000`, ISO-date = Excel-serial,
`"Acme Inc."` = `"ACME INCORPORATED"` to intern to the *same* code is value canonicalization
shading into **entity resolution** — canonicalize-at-the-boundary in its hardest instance. The
Roaring layer stays clean; the effort lives at the normalization boundary. And a fingerprint
is a **coarse sieve, not a classifier** (bits are presence, hashing aliases): blaze to shrink
the soup to candidates, then refine survivors with heavier methods only where the cheap layer
says two things are close.

This closes a loop back to the repository's founding task — telling a spreadsheet's
*dimensions* from its *measures* from catalog metadata and FK membership is the small, local
case of asking what a corpus is about. (Clean-IP: catalog anchoring must use *public* authority
files / reference domains, never a private system-of-record.)

## 6. Discriminated union in columns (the box value model)

A cell's heterogeneous value is encoded as a **type tag + sparse typed columns**
(`cell_type` + `vnum` / `vstr` / `vbool`), not a stringly-typed blob. Nulls are near-free
under RLE and predicates push down (`vnum > 1000`). The per-format INT-vs-DOUBLE coordinate
binding is the same move for geometry — a per-variant bind, not a lowest-common-denominator
cast.

## 7. Lossy-by-contract + canonicalize-at-the-boundary

Pinning *what will never be needed* — here, **we never reconstruct the source** — licenses
aggressive flattening, dropping, and canonicalization: drop XML scaffolding / calcChain /
theme indirection, but *keep* the named-style link because it is header signal. State the
"never needed" set at the top of a representation; it is the license for every simplification
below. And establish the invariants downstream depends on *once*, at ingestion, so every
consumer inherits the guarantee instead of re-deriving or mistrusting it.

---

## 8. Directions (bullish, not yet detailed)

The declarative/relational bias (see the System doc, §10) points at a family of forward moves,
all threaded by the artifact **SHA-256** so the many tangled paths a file-based artifact takes
are unified by one key:

- **Domain-specific relational perspectives over the artifacts** (via DuckLake or another
  relational catalog): present "nice", human-facing schemas as VIEWs over the content-addressed
  box tables — e.g. a `spreadsheets` table that *pivots derived evidence into columns*
  `⟨spreadsheet_path, number_of_formulas, number_of_sheets, has_vba, …⟩`, and a long-form
  `spreadsheet_domains ⟨spreadsheet, domain, evidence⟩` presenting the various estimates/probes
  run against each artifact. Many such perspectives/VIEWs, each a different lens, all joined by
  the SHA — the relational surface the bias wants, sitting on the flexible storage the chassis
  provides.
- **Structure/content lineage.** Because structure is queryable (§2) and fingerprintable (§5),
  find which spreadsheets *resemble* each other in structure and content, and make educated
  bets on a **SHA-256 timeline** — which artifacts were minted from which ancestors — purely
  from structure. This is *[NEUTRAL-FRAMING-TERM — candidates: stemmatics / phylomemetics /
  homology]* applied to spreadsheets: reconstruct the copy-tree from shared features, framed as
  **heredity, not policing**. (Borrow the *techniques* of similarity/plagiarism detection; drop
  the judgemental framing — the goal is ancestry, not accusation.)
