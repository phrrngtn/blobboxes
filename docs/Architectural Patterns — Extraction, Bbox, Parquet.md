# Architectural patterns — extraction / bbox / Parquet work

The patterns that recurred designing the document-extraction → interned-bbox → Parquet
pipeline have been split into two documents along the seam that emerged as the work matured
— the reusable **software/system architecture** vs the domain-specific **application
architecture**:

- **[System Architecture — the blob\* chassis](System%20Architecture%20—%20The%20blob%20Chassis.md)**
  — domain-agnostic. How to take a fat, good-at-its-job native library and make it a
  first-class, database-friendly citizen: defang it behind a small **two-shape** C surface
  (JSON blob + cursor), make every host a **client**, keep all logic in the C wrapper. Plus
  the craft patterns (input polymorphism, content-addressed identity, values-by-construction,
  safe-handle reuse, physical-home-by-access-pattern, dumb-hot/smart-cold, coarse-grain
  parallelism, verify-the-substrate) and the relational/DuckDB substrate bias. This is the
  chassis shared across the blob\* family (`blobboxes`, `blobtemplates`, `blobfilters`,
  `blobodbc`).

- **[Application Architecture — documents as a table of boxes](Application%20Architecture%20—%20Documents%20as%20a%20Table%20of%20Boxes.md)**
  — domain-specific. The product bet: reduce every document to a **long, narrow table of
  boxes** — the Universal Address `(doc_id, page, region, payload)` — and recover structure,
  meaning, and lineage by *query* rather than baking it into per-format schemas. Interning,
  the reference graph, aboutness (Roaring fingerprints + catalog-anchored distant supervision),
  the discriminated-union value model, and the forward directions (relational-catalog
  perspectives threaded by SHA-256; structure-based lineage).

The through-line under both is **the spine**: never weld data to (a) its identity/name,
(b) its location/source, or (c) the mechanism used to extract, transport, or act on it —
keep those separable and late-bound. The System doc is that principle applied to the
plumbing; the Application doc is it applied to the domain model.
