# Python Cursor as an Escape Hatch for SQL Awkwardness

blobboxes has two front ends over the same C readers:

- **SQL** (DuckDB / SQLite) — set-based, value-semantics. The right tool for
  corpus-scale work: scan a glob, join, aggregate, run the FAST-checks, write
  artifacts with `COPY`.
- **Python** (a thin nanobind binding) — a stateful **cursor**: open a document
  once, then pull whatever you need off that single parse.

The Python surface is deliberately *not* a peer of the SQL surface — it is thin
over the C library. But precisely because it is a **procedural handle**, it walks
straight past several things that are awkward or impossible in SQL. When you hit
one of those, drop to the cursor.

```python
import blobboxes
cur = blobboxes.open_pdf(data)      # data: bytes (a file, a spider result, a blob store row)
cells  = cur.bboxes()               # payload  — one row per box (dicts)
doc    = cur.doc()                  # \
pages  = cur.pages()                #  \  metadata, all off the SAME single parse
fonts  = cur.fonts()                #  /
styles = cur.styles()               # /
# open_xlsx / open_docx / open_text / open (auto-detect by magic bytes) are the same shape
```

## 1. One parse, many outputs (the single-pass problem)

For PDF (and, to a lesser degree, every format) the *metadata is a byproduct of
the content parse* — fonts and styles are discovered while walking the page
content, not stored in a separate part. In SQL a **table function** returns the
cells and **scalars** return the metadata, and the two cannot share a cursor:

```sql
-- SQL: 3 separate calls => 3 PDFium parses of the same document (PDFium is mutex-serialized)
SELECT * FROM bb_pdf('doc.pdf');            -- parse #1 (cells)
SELECT bb_pdf_fonts_json('doc.pdf');        -- parse #2 (fonts)
SELECT bb_pdf_styles_json('doc.pdf');       -- parse #3 (styles)
```

`pdf_header` folds fonts+styles+page-dims into one scalar, cutting the artifact
to two parses (cells + header) — but **two is the SQL floor**, because a
table-function and a scalar still can't share one parse.

```python
# Python: ONE parse yields all of it
cur = blobboxes.open_pdf(data)
cells, fonts, styles = cur.bboxes(), cur.fonts(), cur.styles()
```

If you find yourself re-opening the same document to get a *second* thing out of
it, that's the signal to use the cursor.

## 2. Header + body from one call (the cardinality mismatch)

A document is one aggregate: a cardinality-1 **header** (doc info, fonts, styles,
sheet merges) and a cardinality-N **body** (the boxes). SQL has no clean way to
return both from a single call — a table function yields rows, a scalar yields one
value, and every trick to bridge them (smuggling an object pointer through a
`BIGINT`, an `OUT`/`INOUT` parameter, a nested `STRUCT(meta, cells)` that must be
`UNNEST`ed) is rejected by the engine's value-semantics or costs an allocation
pass. The relational answer is to keep them as two relations joined by a key, or
to let the *file format* hold the split (Parquet's footer + row groups).

The cursor simply *is* that shared handle — header and body come off one parse,
and the handle is freed by GC / a `with` block. No pointer, no re-parse, no LRU.

## 3. Per-item iteration over a byte store (no `LATERAL`)

DuckDB will not take a subquery or a `LATERAL` column as a table-function
argument — a blob must be a **bound parameter**, one item at a time:

```sql
-- NOT supported: read_blob('*.pdf') AS b, LATERAL bb_pdf_blob(b.content)
```

So a store of spidered bytes (`sha -> bytes`) is processed one bound param per
item — which in Python is just a loop:

```python
for sha, data in blob_store.items():        # bytes never touch the filesystem
    cur = blobboxes.open(data)              # auto-detect format from magic bytes
    write_artifact(sha, cur.bboxes(), cur.doc(), cur.fonts(), cur.styles())
```

(The SQLite vtab accepts a path *or* a blob in one function; DuckDB needs the
`bb_<fmt>` / `bb_<fmt>_blob` split. Either way, many-blobs-in-one-query is not a
thing — it is a bound param, or a loop, per item.)

## When to use which

| Use SQL when… | Use the Python cursor when… |
|---|---|
| analysing across a corpus (globs, joins, `GROUP BY`) | working one document at a time |
| running the FAST-checks / region graph over `read_parquet` | you need cells **and** fonts/styles/doc from **one** parse |
| generating artifacts with `COPY` | integrating a spider / blob store (bytes, not files) |
| the analysis is genuinely set-based | SQL's value-semantics are fighting you |

**Rule of thumb:** if you are re-parsing a document to extract a second output, or
trying to carry a handle/pointer/out-parameter through SQL, stop — open the
document once with the cursor and pull everything off it. That is the procedural
escape hatch the Python binding exists to provide.
