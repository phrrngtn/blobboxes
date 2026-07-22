# blobboxes — a grand unified index for the world's paginated data

Every document format is a walled garden. A spreadsheet is a zip of XML with its own
notion of cells, styles, and formulas; a PDF is a content stream of glyphs at
coordinates; a Word file is a flow of runs and tables; a text file is lines. To ask a
simple cross-cutting question — *which of these ten thousand files mention this
customer, contain a currency column, or share a table structure* — you today write ten
thousand bespoke parses, throw the results away, and do it again tomorrow. The data is
trapped inside the mechanism that read it.

The insight is that all of it reduces to the same shape. Every atom of paginated
material is **a value-or-formula, at a geometry, on a page, in a document** — a
*Universal Address*. The address hierarchy is identical across formats; only the
geometry is native to each (an integer grid cell in a spreadsheet, a float rectangle in
a PDF, a character span in a document). One shared reader melts every format down to
that shape, and — crucially — it never welds the data to how it was found: a file on
disk, a blob from a spider, an in-memory buffer are all the same call, and every
document is named by the SHA-256 of its bytes, so identity comes from content, not
location.

Inside that address space, two moves make it fast. First, **intern, don't interpret**:
styles, strings, and eventually *values* are carried as cheap integer surrogates, so
"are these two cells the same?" is an integer compare, and you only decode to a font or
a currency format when a human actually needs to read it. Second, **parse once,
forever**: each document is written to a self-contained, content-addressed Parquet
artifact — the cells as compressed, sorted, pushdown-ready columns, plus a footer that
carries the decode dictionaries needed to un-intern it. The expensive parse is paid
exactly once, at ingestion; from then on an entire corpus is ordinary set-based SQL,
and re-analysis is measured in *milliseconds per file* — roughly **80× cheaper** than
reopening the originals — with each artifact a discrete, access-controlled object, so
"what can this user see" is a set of files, not a row-level-security headache.

The same library speaks three dialects — DuckDB, SQLite, and Python — off one C core,
so the analyst queries a glob of artifacts as a single relation, the embedded case runs
the identical logic, and the procedural caller gets a cursor that hands back a
document's cells, fonts, styles, and merges from a single parse. Nothing is
re-implemented per surface; the readers, the coordinate model, and the interning all
live once, in the core, and every dialect inherits them.

That substrate is the launch pad, not the destination. Because a payload can be a
*reference* — a formula, a hyperlink, a cross-reference — every reference is an edge
over the address space, so the corpus is not a bag of boxes but a **graph**: you can
see a spreadsheet's inputs and outputs, a report's totals, a document's structure. And
because every value interns to a shared dictionary, a region's contents become a
**fingerprint** — a compact bitmap — that you can match, with nothing but set
intersections, against known reference domains (a catalog of customer IDs, product
codes, ISO dates). Ask *what is this region, this page, this whole collection about?*
and the answer is an argmax of cheap popcounts, computed at any level of the address
hierarchy by unioning the pieces. It is a coarse, blazing sieve that shrinks a mountain
of documents to a handful of candidates, which heavier methods then refine.

At which point the whole thing closes a loop: automatically telling a spreadsheet's
*dimensions* from its *measures* — the task this repository started from — turns out to
be the small, local case of asking what a corpus of documents is about. blobboxes is
the machine that makes that question cheap, uniform, and answerable at scale.
