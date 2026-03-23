# The BBox as Universal Intermediate Representation

> Every document format is a different encoding of the same thing:
> regions of text with spatial coordinates. Normalize to bboxes first,
> then classify. The classifier never needs to know where the data came from.

## The Principle

A bounding box is `(page, x, y, w, h, text)` plus style metadata. This
tuple is the **format-agnostic intermediate representation** for all
downstream operations: domain classification, table detection, schema
matching, and the resolution sieve.

The source backends produce bboxes with format-specific coordinate
semantics, but the **consumer doesn't care**:

| Source       | page_number       | x               | y              | w             | h     |
|-------------|-------------------|-----------------|----------------|---------------|-------|
| PDF         | PDF page (1-based)| points from left| points from top| glyph width   | line height |
| XLSX        | sheet index       | column number   | row number     | column span   | row span    |
| Plain text  | 1 (always)        | 1 (always)      | line number    | character count| 1     |
| Browser DOM | 1 (always)        | CSS pixels left | CSS pixels top | element width | element height |
| DOCX        | page (estimated)  | points from left| points from top| run width     | line height |

The coordinates differ in units, but the **topology is the same**:
bboxes tile the document surface, text is associated with regions, and
spatial proximity encodes structure (rows, columns, paragraphs, headers).

## Why This Matters for Classification

The resolution sieve (see `blobembed/doc/resolution-sieve.md`) classifies
columns as dimension/measure/key/content using a six-layer cascade:

1. Normalization + tokenization
2. blobfilter domain membership probing
3. Domain-aware re-tokenization
4. Embedding-based semantic similarity
5. Schema matching against known table profiles
6. LLM fallback

**Every layer of this sieve operates on text values, not on document
structure.** The sieve doesn't need to know whether "San Francisco" came
from cell B7 of an Excel sheet, page 3 of a PDF, or line 42 of a CSV.
It needs the text, the spatial context (what's nearby), and optionally
the style (bold headers vs normal data).

BBoxes provide exactly this. The sieve's input is a set of bboxes; its
output is classification labels attached back to those bboxes. The
format-specific backends are upstream; the sieve is downstream; the bbox
is the interface between them.

## Spatial Context Without Parsing

The bbox representation preserves spatial relationships that pure text
extraction destroys:

- **Column alignment**: bboxes with similar x-coordinates and consistent
  y-spacing are likely a column. No need to parse HTML `<td>` or Excel
  cell references — the geometry tells you.
- **Header detection**: a bbox with a distinct style (larger font, bold
  weight) at the top of a column cluster is probably a header. Works
  identically for PDF headers, Excel frozen rows, and HTML `<th>`.
- **Table boundaries**: a rectangular cluster of bboxes with grid-like
  alignment is a table candidate. The table detection algorithm works on
  geometry, not on format-specific table markup.

This is the key insight: **structure is spatial, not syntactic.** A PDF
table, an HTML `<div>` grid, and an Excel range all look the same in
bbox space — a rectangular grid of text regions with aligned coordinates.

## The Coordinate Contract

Each backend must satisfy a minimal contract:

1. **Text is UTF-8.** No format-specific encoding leaks through.
2. **Coordinates increase top-to-bottom, left-to-right.** The origin is
   the top-left corner of the page.
3. **page_number is 1-based.** Single-page formats (text, browser) use
   page 1.
4. **Style is interned.** Fonts and styles are deduplicated into lookup
   tables; bboxes reference them by ID.
5. **Empty regions produce no bboxes.** Blank lines, empty cells, and
   whitespace-only nodes are skipped.

Beyond this, backends are free to use whatever units make sense for
their format. The sieve and table detector normalize as needed.

## Plain Text: Line Numbers as Geometry

The text backend (`bboxes_text.cpp`) demonstrates the principle in its
simplest form. A plain text file has no explicit geometry, but it has
implicit geometry:

- **y = line number** — each line occupies a row
- **x = 1** — all lines start at the same column
- **w = character count** — width is the line length
- **h = 1** — each line is one unit tall

This means a CSV file, when processed through the text backend,
produces bboxes where each line is a row. The table detector sees
a tall, narrow stack of bboxes — recognizable as a single-column
table or, after splitting on delimiters, a multi-column table.

A log file produces bboxes where timestamps cluster at consistent
x-offsets, log levels appear in a second cluster, and message text
fills the remainder. The spatial structure emerges from the text
without any format-specific parsing.

## Excel: Cells Are Already BBoxes

The XLSX backend maps directly:

- **page_number = sheet index** — each sheet is a page
- **x = column number, y = row number** — the grid is the geometry
- **w = column span, h = row span** — merged cells have w > 1 or h > 1
- **formula** — preserved as an extra field on XlsxBBox

Excel is the most natural fit because spreadsheet cells are literally
bounding boxes. The bbox representation doesn't add an abstraction
layer — it just standardizes the interface.

## Browser DOM: Computed Layout as Ground Truth

The browser bundle (`browser/src/`) uses `getClientRects()` to extract
the **rendered** geometry, not the DOM structure. A CSS grid, a flexbox
layout, and an HTML table all produce the same bbox representation:
rectangles with text, positioned in pixel space.

This is why blobboxes works where `<table>` scraping fails: Shadow DOM
components, React-rendered layouts, and CSS Grid tables have no
`<table>` elements, but they have rendered rectangles. The geometry is
the ground truth; the DOM is just one way to produce it.

## The Sieve on BBoxes

Applying the resolution sieve to bboxes (rather than to database
columns) extends classification to unstructured documents:

| Sieve layer | On database columns | On bboxes |
|---|---|---|
| **blobfilters** | Probe column values against domain bitmaps | Probe bbox text tokens against the same bitmaps |
| **Re-tokenize** | Domain-aware normalization of column values | Same normalization on bbox text |
| **Embeddings** | Embed column values with hierarchical context | Embed bbox text with spatial context (nearby bboxes) |
| **Schema match** | Match table column profile against known schemas | Match bbox column cluster against known schemas |
| **LLM** | Send unresolved columns with all context | Send unresolved bboxes with spatial + style context |

The critical difference is context construction. For database columns,
context comes from the table schema (column names, types, co-occurring
columns). For bboxes, context comes from **spatial proximity**: the
bboxes above (likely headers), beside (likely same-row values), and
in the same cluster (likely same-table values).

Both feed into the same sieve. The sieve doesn't care whether context
was derived from `sys.columns` or from geometric clustering.
