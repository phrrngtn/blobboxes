# Evidence Pipeline Architecture

> Classify every bounding box in a document by accumulating evidence
> from cheap independent passes, generating competing hypotheses,
> and resolving them via constraint satisfaction.

## The Problem

Given a PDF or XLSX rendered as a flat set of bounding boxes
`(page, x, y, w, h, text, style)`, determine each cell's structural
role: is it a measure, a column header, a section label, a row label,
a key-value pair, page chrome, or prose?

The answer determines the cell's **treemap path** — the fully qualified
address like `Revenue / Net Premium Written / Q1 2025 = 1,234` that
would be its compound key in a normalized table.

## Design Principles

### Progressive masking via evidence accumulation

The original insight from [[Progressive Masking and Treemap Layout]]
still holds: classify the easy things first, and use what you learn
to classify the harder things. But instead of literally masking
(blanking out) cells between passes, we **accumulate evidence** in
a shared database. Each pass deposits observations. Later passes
and hypothesis generators can read all prior evidence.

The masking is implicit: once a cell has strong evidence for a role
(e.g., `pattern:numeric` + `spatial:in_table` + `column_align:subtends_col`),
the hypothesis generator claims it as a measure with high support.
Other generators see that evidence and don't bother competing. The
effect is the same as masking — progressive resolution — but the
mechanism is additive (evidence in) rather than subtractive (cells out).

### Evidence is facts, not conclusions

The evidence table stores **observations**, not roles:

- "This cell is bold" (style evidence)
- "This cell's text parses as a number" (pattern evidence)
- "This cell appears at the same y-position on 13 out of 16 pages" (cross-page evidence)
- "This cell's text matches 3 out of 4 tokens against `domain:us_states`" (domain evidence)

These are facts. The interpretation ("therefore it's a column header")
happens in the hypothesis layer, not the evidence layer. This separation
means you can add new evidence passes without touching interpretation
logic, and vice versa.

### SQL does the arithmetic, Python orchestrates

Each evidence pass is a focused SQL query that reads from `cells`
(and possibly earlier evidence) and INSERTs into the `evidence`,
`row_evidence`, `page_evidence`, or `doc_evidence` tables. Python
decides which passes to run and in what order, but the computation
is set-based SQL executed by DuckDB.

The hypothesis generators are also SQL queries — they read evidence
and INSERT into the `hypotheses` table. The constraint checker and
resolver are SQL queries over hypotheses. Even the ILP solver is
called via a DuckDB scalar function (`bs_solve`).

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Document (PDF/XLSX)                    │
└───────────────────────┬─────────────────────────────────┘
                        │ bb(), bb_styles(), bb_fonts()
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Ingest: row clustering, cell merging, fragment joining  │
│  → cells table (doc_id, page_id, row_cluster, cell_id)   │
└───────────────────────┬─────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   ┌─────────┐   ┌───────────┐   ┌──────────┐
   │  Style   │   │  Pattern  │   │ Position │  ... 16 passes
   │histogram │   │  match    │   │  (page)  │
   └────┬─────┘   └────┬──────┘   └────┬─────┘
        │               │               │
        └───────────────┼───────────────┘
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Evidence tables: cell, row, page, doc level             │
│  + cell_probes: roaring bitmap of boolean probe results  │
│  + probe_registry: ordinal → URI mapping                 │
└───────────────────────┬─────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   ┌──────────┐  ┌───────────┐  ┌──────────┐
   │  Chrome  │  │  Measure  │  │  K-V pair│  ... 9 generators
   │ hypoths. │  │  hypoths. │  │  hypoths.│
   └────┬─────┘  └────┬──────┘  └────┬─────┘
        │              │              │
        └──────────────┼──────────────┘
                       ▼
┌─────────────────────────────────────────────────────────┐
│  Hypotheses table: (cell, role, support, against)        │
│  + Constraints table: structural rules                   │
│  + Roles table: priorities, descriptions, colors         │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Resolver: ILP solver (blobsolver / HiGHS) or greedy     │
│  Maximize Σ(support) subject to constraints              │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Treemap: (cell, role, path)                             │
│  path = title / scope_l0 / row_label / col_header        │
└─────────────────────────────────────────────────────────┘
```

## Tables

All tables carry `doc_id` — multiple documents coexist in one database.

### cells

The working set. Produced by `ingest()` from raw bboxes after row
clustering and cell merging.

```sql
cells(doc_id, page_id, row_cluster, cell_id,
      x, y, w, h, text,
      weight, font_size, font_name, color, style_id,
      style_rank, style_count, n_fragments)
```

### evidence

Cell-level observations from independent passes.

```sql
evidence(doc_id, page_id, row_cluster, cell_id,
         source,   -- which pass: 'style', 'pattern', 'position', ...
         key,      -- what was observed: 'style_rank', 'type', 'y_frac', ...
         value)    -- the observation: '1', 'measure', '0.35', ...
```

Also: `row_evidence`, `page_evidence`, `doc_evidence` at coarser
granularity.

### probe_registry

Maps ordinals (bit positions in roaring bitmaps) to human-readable
identifiers. Ordinals are local surrogates — the URI is the stable
external identity.

```sql
probe_registry(ordinal, uri, name, kind, definition, description, tags)
```

Ordinal ranges by convention:
- 0–99: text pattern probes (regex, TRY_CAST)
- 100–199: style/visual probes
- 200–299: spatial/geometric probes
- 300–399: cross-page probes
- 1000+: domain filter probes (from ATTACHed databases)

### cell_probes

One roaring bitmap per cell recording which boolean probes matched.
Fast screening index. Complements the valued evidence table.

```sql
cell_probes(doc_id, page_id, row_cluster, cell_id, probes BLOB)
```

Query: `bf_contains(probes, 100::UINTEGER)` → is this cell bold?
Expand: `UNNEST(bf_to_array(probes)) JOIN probe_registry` → all matches.

### hypotheses

Candidate role assignments with evidence counts.

```sql
hypotheses(doc_id, page_id, row_cluster, cell_id,
           hypothesis,   -- 'measure', 'col_header', 'section_label', ...
           detail,       -- col_id, indent_level, label text, ...
           support,      -- evidence FOR this role
           against)      -- evidence AGAINST
```

### constraints

Structural rules declared as data.

```sql
constraints(name, scope, type, description)
```

Types: `uniqueness` (at most N per scope), `exclusion` (can't coexist),
`implication` (if A then B).

### roles

Role priority, description, and visualization color.

```sql
roles(role, priority, description, color)
```

### treemap

Final classification with assembled treemap path.

```sql
treemap(doc_id, page_id, row_cluster, cell_id, path, role)
```

## Evidence Passes

Each pass is a Python function that executes one or more SQL queries.
Passes are order-independent unless noted.

### Pass 0: Style histogram (`pass_style_histogram`)

Histogram bboxes by `(font_name, font_size, weight, color)`. The
dominant style (rank 1) is body/data. Rare styles (rank ≥ 3) are
structural: titles, headers, footnotes.

Evidence: `style_rank`, `style_count`, `is_bold`, `is_rare`, `is_dominant`

### Pass 1: Pattern match (`pass_pattern_match`)

Fast, content-based classification. Each cell tested independently:
- TRY_CAST as DOUBLE → numeric
- TRY_CAST as DATE → date
- Regex: `$12,400` → currency, `65.8%` → percentage
- Standalone 4-digit year (1900–2099) → year

Evidence: `(source='pattern', key='type', value='measure'|'year'|...)`

### Pass 2: Document profile (`pass_doc_profile`)

Cheap coarse classification from style and pattern statistics.
Profiles: financial, tabular, form, product, narrative. Writes
recommended domain groups to `doc_evidence` for downstream filtering.

The profile→domain_group mapping is stored in PostgreSQL
(`domain.profile_domain_groups`) as reference data.

### Pass 3: Page position (`pass_page_position`)

Cell position relative to page content bounds: `y_frac`, `x_frac`,
`w_frac`, `is_top_10pct`, `is_bottom_10pct`.

### Pass 4: Color outlier (`pass_color_outlier`)

Cells whose color differs from the dominant style. Strong structural
signal in color-coded documents (e.g., AIG's financial statement uses
3 colors to distinguish data, headers, and annotations).

### Pass 5: Cross-page (`pass_cross_page`)

Text repeated at the same y-position across multiple pages → chrome
(headers, footers, page numbers). Only fires for multi-page documents.

Evidence: `(source='cross_page', key='repeated_text_pages', value='13')`

### Pass 6: Row geometry (`pass_row_geometry`)

Row-level aggregates: cell count, typed cell count, leftmost x.
Determines which rows are "data rows" (≥2 typed cells).

### Pass 7: Column alignment (`pass_column_alignment`)

X-alignment of typed cells → putative columns. Gap-based clustering
of x-center positions. Any cell whose horizontal extent overlaps a
putative column gets column membership evidence.

### Pass 8: Table regions (`pass_table_regions`)

Contiguous runs of data rows → table regions. Uses row ordinals
(not cluster IDs) for contiguity, handling sparse numbering from
the max-height row split. Single data rows now qualify as table
candidates.

### Pass 9: Blocks (`pass_blocks`)

Spatial proximity blocks: contiguous groups of rows separated by
large vertical gaps. Coarser than table regions — blocks exist before
you know if they contain tables.

### Pass 10: Indent levels (`pass_indent_levels`)

Cluster left-edge x-positions of structural cells into indent levels.
Level 0 = outermost scope, Level 1 = nested, etc.

### Pass 11: Alignment (`pass_alignment`)

Cell alignment within columns: left, right, or center. Right-alignment
is a strong signal for numeric data. Center may indicate headers.

### Pass 12: Section scope (`pass_section_scope`)

Identify section labels (bold/rare + leftmost + non-data) and propagate
their scope downward via `LAST_VALUE IGNORE NULLS` windows. Labels at
indent level N are "in force" for all rows below until the next label
at level ≤ N.

### Pass 13: Token domain probing (`pass_token_domain_probing`)

Tokenize cell text, hash each token with `bf_hash_normalized`, probe
against domain bitmaps from ATTACHed `domains.duckdb`. Produces
containment scores per cell per domain.

Optimization: build doc-level hash bitmap → intersect against all
domains → only probe matching domains individually. 91 domains in <1ms.

### Pass 14: Column domain containment (`pass_column_domain_containment`)

Aggregate token-domain hits per putative column. "90% of cells in
column 3 match `domain:fips_state_codes`" is a high-confidence
classification that individual cell probes can't achieve.

This is the **set-oriented probing** from [[Hash Collision Analysis for Domain Filters#Set-Oriented Probing vs Single-Element Probing]] —
the false positive rate drops exponentially with column size.

### Pass 15: Cell probes (`pass_build_cell_probes`)

Build per-cell roaring bitmaps from all boolean probe results.
Registry-driven: regex probes execute via `JOIN probe_registry`,
style/spatial probes from evidence lookups, domain probes via
`bf_contains`.

## Hypothesis Generators

Each generator queries evidence and proposes candidate roles.

| Generator | Role | Key evidence |
|---|---|---|
| `hypothesize_chrome` | chrome | cross_page:repeated + style:rare + position:edges |
| `hypothesize_column_headers` | col_header | spatial:subtends_col + spatial:above_data + !numeric |
| `hypothesize_section_labels` | section_label | style:bold + spatial:leftmost + !in_data_rows |
| `hypothesize_total_rows` | total_label | style:bold + spatial:leftmost + in_data_rows |
| `hypothesize_row_labels` | row_label | spatial:leftmost + in_data_rows + !numeric |
| `hypothesize_measures` | measure | pattern:numeric (+ spatial:in_table for extra support) |
| `hypothesize_dimensions` | dimension | in_table + subtends_col + !numeric + !leftmost |
| `hypothesize_key_value_pairs` | field_label / field_value | 2-cell row + !in_table |
| `hypothesize_prose` | prose | catch-all default (support=1 for every cell) |

## Constraints

Structural rules declared in the `constraints` table:

- **one_header_per_column**: each putative column has at most one header
- **measure_not_header**: a cell can't be both a measure and a header
- **section_not_in_data**: section labels don't appear in data rows
- **header_implies_column**: headers must subtend a putative column
- **one_role_per_cell**: each cell gets exactly one role (enforced by
  the `exactly_one` ILP constraint)

Constraints are applied by finding violations (recorded in the
`violations` table for audit) and excluding violated hypotheses
from resolution.

## Resolution

Two resolvers available:

### ILP solver (default)

Builds a binary integer linear program from the hypotheses table:
- Variable per (cell, role) hypothesis with positive net support
- Maximize Σ (support − against) × x_{cell,role}
- Hard constraints: `exactly_one` per cell, `at_most_one` per column header group

Solved by HiGHS (via `bs_solve` in blobsolver). Sub-millisecond for
typical documents (100–2000 cells). Finds the globally optimal
assignment satisfying all constraints.

### Greedy (fallback)

Sequential constraint application + priority-based selection.
Used when the solver extension is not loaded.

## Domain Probing

Domain filters from [[blobfilters]] are ATTACHed as a read-only
DuckDB database (`domains.duckdb`). Each domain is a roaring bitmap
of FNV-1a hashed member strings.

```sql
ATTACH 'domains.duckdb' AS domaindb (READ_ONLY);
```

97 domains, 375K members across geographic, financial, corporate,
medical, catastrophe, MEP equipment, materials, and sustainability
categories.

Probing flow:
1. `bf_hash_normalized(text)` → uint32 hash per token
2. `bf_contains(domain_bitmap, hash)` → membership test
3. Bitmap intersection pre-filter: build doc-level hash bitmap,
   intersect against each domain to find candidates
4. Only probe matching domains per cell (typically 5–10 of 97)

Bitmap caching: `bf_contains` caches the deserialized bitmap when
the first argument is constant across rows (946ms → 22ms).

## Performance

86 documents, 300 pages, 52K cells: **7.5 seconds** (7,000 cells/sec).
Single document: ~50ms including all 16 passes + ILP solve.

## Visualization

Two HTML renderers:

- `render_interactive.py`: text-based with toggle buttons per role
- `render_pdf_overlay.py`: PDF.js renders actual pages with translucent
  colored annotation rectangles. Toggle buttons to show/hide roles.

Role colors defined in the `roles` table, not hardcoded.

## Evaluation

FinTabNet benchmark (50 S&P 500 annual report pages):

| Metric | Score |
|---|---|
| Header precision | 87% |
| Header recall | 16% |
| Row label recall | 31% |
| Numeric recall | 69% |

Header recall is the main gap — limited by table detection coverage
on pages with fragmented text extraction.

## Relationship to Other Components

- **[[blobboxes]]**: bbox extraction (`bb()`, `bb_styles()`, `bb_fonts()`)
- **[[blobfilters]]**: domain membership probes (`bf_contains()`,
  `bf_hash_normalized()`), probe registry ordinals map to domain URIs
- **[[blobsolver]]**: ILP constraint solver (`bs_solve()`)
- **[[blobhttp]]**: browser extraction for web pages (produces same
  bbox format)
- **[[blobembed]]**: embedding-based classification for genuinely
  ambiguous cells that survive all evidence passes

## What's Next

1. Column-level containment as a hypothesis input (not just page evidence)
2. Multi-level scope propagation (L0, L1, L2 with level reset)
3. Multi-page table continuation detection
4. Domain tag improvement for profile-based filtering
5. SAT solver for richer constraint types (implications, conditionals)
6. Embedding fallback for remaining ambiguous cells
