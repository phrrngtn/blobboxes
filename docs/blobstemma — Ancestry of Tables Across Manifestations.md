# blobstemma — the ancestry of tables across manifestations

**Candidate sibling in the blob\* family** (`blobboxes`, `blobtemplates`, `blobfilters`,
`blobodbc` → **`blobstemma`**). Where `blobboxes` *reduces* every document to a
[table of boxes](Application%20Architecture%20—%20Documents%20as%20a%20Table%20of%20Boxes.md),
`blobstemma` *reads that relation back* to answer a lineage question: **which artifacts —
and which tables inside them — descend from which?** It owns no extraction of its own; it is
pure analysis over the content-addressed box tables the chassis already produces.

The framing is **stemmatics**, borrowed from textual criticism: a *stemma codicum* is the
family tree of manuscript copies, reconstructed from shared features. A spreadsheet — or a
table — has a stemma too. This is **heredity, not policing**: we borrow the *techniques* of
similarity / plagiarism detection and drop the judgemental framing. The valuable question is
not "are these two files identical" (byte-SHA already answers that) but **"is this the same
data, and where did it come from"** — provenance, the way information actually propagates:
out of a database, into a spreadsheet, into a report, onto a page.

---

## 1. The reframe: the unit of heredity is the *latent table*, not the document

A file's byte-`SHA-256` is a perfect identity for an *exact copy* and nothing else. But the
thing that actually travels through an organization is a **table** — a customer list, a rate
card, a trial balance — and it re-manifests constantly with a completely different byte-SHA:
queried live, snapshotted to Excel, flattened into a PDF, scraped back off a web page. Byte
identity cannot track that. So `blobstemma` moves the unit of heredity down from the document
to the **latent table** — a maximal box-region that behaves relationally (header, typed
columns, tuples) — recovered from the box relation by structure analysis.

## 2. Two-level identity

| level | key | invariant to | answers |
|---|---|---|---|
| **hard** | byte `SHA-256` | nothing | exact-artifact dedup; the universal join thread |
| **soft** | **latent-table fingerprint** — Roaring domain fingerprint over the interned/canonicalized value space + structural signature | styling, coordinates, format scaffolding, transpose, column reorder | *manifestation* — "same table, different clothing" |

The soft key is **manifestation-invariant by construction**: it is computed over
*canonicalized values and box shape*, never over bytes or format. That is the SIFT move — a
descriptor built to be blind to exactly the transform you have chosen not to care about.

## 3. Why one engine covers DB / Excel / PDF / Web

All four sources reduce to the same box relation, so **latent-table detection is a single,
format-blind structure pass** — "find the relationally-coherent tabular regions" — not four
bespoke table extractors. That is the box bet paying off on a new job. The canonical
provenance chain is a walk down a **structure-loss gradient**:

```
DB view / query result  ──►  exported .xlsx range  ──►  pasted into PDF report  ──►  scraped web table
      (live)                   (snapshot; formulas)         (flattened; ruled)          (re-parsed HTML)
```

Four byte-SHAs, **one** latent-table identity, a four-node stemma. Crucially, that gradient
is *monotone loss* — each hop strips something (live-ness, then formulas, then cell
boundaries), and monotone loss is a **direction cue**, the arrow heredity needs and bare
similarity lacks (§6).

## 4. The founding job is a subroutine, not an analogy

`blobstemma` inherits this repository's original task directly. Dimension/measure column
classification + catalog-anchored domain detection **types a latent table's columns** — which
are keys, which are measures — and that does two jobs at once:

- gives the table a **schema** that can be matched column-for-column across manifestations;
- tells the fingerprint **which columns' domains are the discriminating bits** (a key-column's
  value set discriminates; a boolean flag does not).

So aboutness (see the Application doc §5) is not adjacent to lineage — it *is* the identity
function lineage runs on.

## 5. Pipeline

Each stage is a query (or small operator) over the box relation; nothing here needs a new
extractor.

1. **Detect latent tables.** Structure analysis over boxes → candidate tabular regions
   (header band, contiguous typed columns, tuple rows). One pass, all formats.
2. **Type the columns.** Dimension/measure + catalog-anchored domains → per-column semantic
   type and key/measure role (§4).
3. **Fingerprint.** Roaring domain fingerprint over canonicalized values + structural
   signature (`[[structural-skeleton-graph]]`) → the **soft key** (§2). Nests up/down the
   address hierarchy by OR-ing, exactly as aboutness does.
4. **Block (the sieve).** Cheap fingerprint overlap → candidate *pairs* of tables that might
   be kin. This is a filter, not a verdict — it must over-generate.
5. **Align + confirm.** Heavier column-to-column structural alignment on survivors — the
   within-format invariance machinery (`[[cross-sheet-sift-similarity]]`: R1C1 descriptors,
   Hough offset-voting for translation/transpose skew) generalized to cross-format matching.
   This kills the false merges the sieve lets through.
6. **Infer direction.** Turn confirmed kinship (undirected) into ancestry (directed) using
   the structure-loss gradient (§3), containment/monotone-growth (a later copy usually *has*
   more rows/sheets or *less* structure), and OOXML/PDF core-props timestamps where
   trustworthy — as tie-breakers, never as the sole signal.
7. **Build the stemma.** A **maximum-weight spanning arborescence** over the directed,
   weighted kinship graph → the family tree. Nodes are latent tables (or artifacts); edges
   cross documents *and* formats.

## 6. Two invariance axes

`blobstemma` needs robustness on two independent axes; the domain fingerprint is the single
descriptor invariant on both.

- **Within-format** — translation / transpose / reorder. Already have it:
  `[[cross-sheet-sift-similarity]]`, R1C1 fingerprints, Hough offset-voting.
- **Across-format** — manifestation. Strip styling, coordinates, and format scaffolding; keep
  canonicalized domain content + relational shape.

## 7. Discipline and failure modes

- **Sieve, not classifier.** Fingerprint bits are *presence* and hashing *aliases*; two
  unrelated tables both keyed on `US-states` will collide. Blocking (step 4) is allowed to be
  loud precisely because alignment (step 5) is the arbiter. Never let a fingerprint match
  alone assert kinship.
- **Cross-format multiplies fuzz.** Every manifestation hop degrades the signal, so confidence
  must decay along a chain and the stemma must carry per-edge weights, not booleans.
- **Direction is the weakest link.** Similarity is symmetric; ancestry is not. Treat every
  direction cue (§6) as probabilistic and let them vote; be willing to emit an *undirected*
  cluster when no cue is decisive rather than invent an arrow.
- **Clean-IP, hard constraint.** Cross-manifestation corpora for building and testing this
  must be **public prior art** (`[[spreadsheet-test-corpora]]` — Enron, EUSES, SpreadsheetBench)
  or original synthetic chains, **never day-job files or a private system-of-record**
  (`[[project-timeline-and-clean-ip]]`).

## 8. Build posture

Consistent with the house bias (System doc §10): **relational-first**. `blobstemma` should
*start life as a `queries/` folder over the Parquet box tables* — latent-table detection,
fingerprinting, blocking, and stemma assembly are all expressible as SQL / small operators
over the existing artifacts, joined by `SHA-256`. It graduates to a C library in the two-shape
mould (System doc §1) only if a step proves too heavy for the query engine (the arborescence
solver, or MinHash-LSH blocking at corpus scale are the likely first escapees). The output is
itself relational — a `table_stemma ⟨child_sha, child_table_id, parent_sha, parent_table_id,
edge_weight, direction_evidence⟩` edge set — which slots straight into the DuckLake
relational-catalog perspectives sketched in the Application doc §8.

## 9. Open questions

- **What earns a fingerprint bit** for a *table* (vs a whole document)? Key-column domain
  values, header-token set, column-type vector, arity — needs the same "what earns a bit"
  design pass aboutness needs.
- **Latent-table detection quality** is the floor on everything downstream; how much can be
  borrowed from published table-detection work vs. what the box/skeleton structure already
  gives for free.
- **Value canonicalization across formats** is the hardest sub-problem (a PDF renders `1000`
  as `1,000.00`, the web table as `$1,000`) — the entity-resolution crux from aboutness
  (`[[universal-address-and-aboutness]]`) resurfaces here as the thing that makes or breaks the
  soft key.
