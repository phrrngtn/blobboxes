# System Architecture — the blob\* chassis

**Domain-agnostic.** This is the reusable substrate shared across the blob\* family
(`blobboxes`, `blobtemplates`, `blobfilters`, `blobodbc`): how to take a fat,
good-at-its-job native library and make it a first-class, *database-friendly* citizen.
It knows nothing about documents, boxes, or spreadsheets — swap in a geo library or a
codec and the shape is unchanged. The *what-it-models* lives in the companion
[Application Architecture](Application%20Architecture%20—%20Documents%20as%20a%20Table%20of%20Boxes.md);
this document is the *how-it-is-built*.

The organizing principle is the spine: **never weld data to (a) its identity/name,
(b) its location/source, or (c) the mechanism used to extract, transport, or act on it.**
Every pattern below is that principle applied at the plumbing layer.

---

## 1. Defang the third-party library behind a small, two-shape C surface; make every host a client

Use the fat library for what it does well, but expose only a *small*, stable, C-ABI
surface with exactly **two shapes**:

- a **JSON blob** returned by a plain function — the *scalar / value* idiom. Scalars
  compose (nest, filter, join) where table functions do not, and JSON is the interchange
  every host already parses. Used for the low-N structured results: doc, pages, fonts,
  styles, metadata, headers, decode tables.
- a **cursor** (`open → next_* → close`) — the *streaming / table* idiom, surfaced as a
  table / table-valued function. Used for the high-cardinality bulk (the cells), where
  routing millions of rows through a JSON string is the pathological case.

Every host is then a **client** of those two shapes. DuckDB and SQLite map
blob → scalar-function and cursor → table-function. **Python is a *fancier* client** —
not obliged to shoehorn anything into a relational shape, it consumes the cursor as a
native object with methods returning lists/dicts, and blobs as parsed structures. Because
the surface is plain C-ABI, *any* other client (C, C++, another scripting language) binds
the identical interface. **All transformation lives behind the two shapes, in the C
wrapper; a binding is a mechanism-adapter, never a logic site.** Change the endpoint once
and every client inherits it — the `xlsx_header` envelope fix touched one C function and
SQLite matched it with zero SQLite edits. The two-shape endpoint set *is* the contract;
keep it small and stable so the third party's surface area never escapes it.

## 2. Path / Blob / Value polymorphism (input independence)

An operation accepts *a path, a blob of bytes, or an already-parsed value* and behaves
identically; data is not tied to how it was located or delivered. The readers are all
byte-based, so the wrapper only chooses whether to read a file or take the bytes in hand;
a spidered artifact that never hits disk and a file on disk are the same call. Toward
formalization: a `Source = Path | Bytes | Handle` at every stage boundary — a stage never
re-fetches or re-parses what a caller can hand it. (The `Handle` leg is procedural-only:
a relational engine's value-semantics reject a live parsed handle, so there it degrades
to a content-name registry — see §5.)

## 3. Content-addressed identity (a naming mechanism)

Identity from the bytes (`SHA-256`), not from location or an assigned id. One name gives
free exact-duplicate dedup, a cache key, and a scope boundary; path is demoted to a mere
*source*, never an identity. It is the universal join key across the corpus, and (see the
Application doc) the thread that ties every derived perspective back to one artifact.

## 4. Values by construction, not codegen

Build typed values in memory and pass them by value/reference; the *only* thing templated
into a query is the *locator* (the filename in a `COPY`). Data crosses the SQL boundary
via binding / Arrow / registration — never string-built `MAP {…}` literals — which is also
the correct injection-safety posture, for free.

## 5. Reuse via a safe handle (content-name registry), not a raw pointer

To reuse an expensive intermediate across a batch, reference it by a stable *name*
resolved at a controlled boundary — a table function keyed by SHA into a host-side parsed
cache — not by recomputing it and not by smuggling a bare address through a parallelizing
engine (which value-semantics will copy, spill, and dangle). Handles are content names
into a registry with an explicit lifetime/eviction contract.

## 6. Physical home by access pattern, not by ontology

Where a piece of data lives is decided by *how it is accessed*, not by what it "is": bulk
cells → compressed, sorted, prunable columns; small decode dictionaries → an opaque,
self-contained, read-once footer bag *or* lazily-queryable nested typed columns. The rule:
*sparse / low-N ⇒ metadata; scales-with-N ⇒ a column.* The single-schema-per-file
constraint is the forcing function that makes the choice explicit.

## 7. Dumb hot path, smart cold path

Separate the O(N) bulk traversal (a pure, single-pass, allocation-light byte walk) from
the low-N "gnarly" work (styles, merges, names, inheritance). Optimize the first for
throughput and safety, the second for correctness. Count your passes over the O(N)
substrate; unbounded passes over O(1) data are not a smell, but a *second* pass over the
bulk to fetch metadata is a regression (the exact bug where a metadata call re-inflated
every worksheet).

## 8. Parallelism at the coarse grain; purity at the unit

Put concurrency at the coarsest independent grain (per-file scan scheduling) and keep the
unit of work pure (no shared mutable state), because purity is the *enabling condition*
for safe fan-out. The engine fans per-file scans across cores with zero parser threading;
the parser stays "concurrency-safe by construction." Watch for hidden process-global state
(a library mutex, a locale, a lazily-built registry) that breaks unit purity; the skew
caveat is that tail latency is bounded by the largest single unit.

## 9. Verify the substrate before you design on it

Every load-bearing assumption about the tools gets checked against reality rather than
recalled — footer KV is stored uncompressed, sort is not automatic, a table-function
beats JSON in DuckDB but not in SQLite, a table function cannot be overloaded by parameter
type, `parquet_kv_metadata` is literal-only. Cheap probes repeatedly overturn
plausible-but-wrong priors. The checklist is *per substrate*, not per concept, because the
same concept behaves differently across two engines that look interchangeable.

---

## 10. Why relational / DuckDB (the substrate bias)

The house bias is toward **relational / declarative / data-oriented** solutions, and DuckDB
is loved for a specific reason: it cleanly separates the **SQL engine** — a mature,
declarative query surface — from **where and how the data is stored**. You get one query
language over enormous storage flexibility: files, in-memory blobs, Arrow tables, Parquet
globs, and relational catalogs, all addressable declaratively. The two-shape library plugs
straight into that seam — cursors become table functions, blobs become scalars, and the
storage underneath can be anything the engine reads. The declarative surface is the stable
thing; the physical layout is free to change (§6) without disturbing the queries above it.

This is also the bridge to the Application layer's forward direction: a relational catalog
(e.g. DuckLake) can present *domain-specific, "nice" relational schemas and views* over the
content-addressed artifacts, all threaded by the artifact SHA-256 — the query surface the
declarative bias wants, sitting on top of the flexible storage the chassis provides. The
details of those schemas are an application concern; see the Application doc.
