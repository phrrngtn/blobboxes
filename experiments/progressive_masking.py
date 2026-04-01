"""Progressive masking sieve with label-scope propagation.

Each round classifies bboxes and removes them from the working set.
Labels propagate scope rightward and downward — a bold section header
like "Expenses > Losses & LAE" qualifies every measure cell beneath it.

Page titles and page numbers are the outermost scope layer: they apply
to every bbox on their page (or across pages if repeated).

Scope hierarchy (outermost to innermost):
  - Page title / page number  (detected by font size or position)
  - Level-0 label             (leftmost bold text, e.g. "Revenue")
  - Level-1 label             (indented bold, e.g. "Losses & LAE")
  - Level-2 label             (further indented bold)
  - Column header             (from the header row)

A label at level N is "in force" for all rows below it until the next
label at level <= N. A higher-level label resets all deeper scopes.
"""

import duckdb
from pathlib import Path

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"

PIPELINE_SQL = """
WITH
-- ═══ Stage 0: Extract raw bboxes with styles ════════════════════════
RAW AS (
    SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
           s.font_size, s.color, s.italic,
           -- Derive bold from font name since PDFium doesn't set weight
           CASE WHEN f.name ILIKE '%bold%' OR s.weight = 'bold'
                THEN 'bold' ELSE 'normal' END AS weight
    FROM bb('{file}') AS b
    JOIN bb_styles('{file}') AS s USING (style_id)
    JOIN bb_fonts('{file}') AS f USING (font_id)
),

-- ═══ Stage 1: Row clustering ════════════════════════════════════════
-- Detect XLSX grid mode (all w=1) vs PDF point mode.
IS_GRID AS (
    SELECT CASE WHEN COUNT(DISTINCT w) = 1 AND MIN(w) = 1.0
                THEN true ELSE false END AS grid_mode
    FROM RAW
),

-- Row clustering: gap-based.
-- Sort all bboxes by (page_id, y). A new row starts when the y-gap
-- from the previous bbox exceeds the body font height.
-- This correctly groups comma/period glyphs (small h, offset y) with
-- their digit bboxes because the y-gap is small (< font height).
BODY_FONT_H AS (
    SELECT GREATEST(
        PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY h) FILTER (WHERE h > 2),
        4.0
    ) AS body_h
    FROM RAW
),
Y_SORTED AS (
    SELECT RAW.*,
           grid_mode,
           ROW_NUMBER() OVER (ORDER BY page_id, y, x) AS sort_ord,
           y - LAG(y) OVER (ORDER BY page_id, y, x) AS y_gap,
           LAG(page_id) OVER (ORDER BY page_id, y, x) AS prev_page
    FROM RAW, IS_GRID
),
ROWS AS (
    SELECT Y_SORTED.* EXCLUDE (sort_ord, y_gap, prev_page),
           SUM(CASE
               WHEN y_gap IS NULL THEN 1                    -- first bbox
               WHEN prev_page != page_id THEN 1             -- new page
               WHEN y_gap > (SELECT body_h FROM BODY_FONT_H) THEN 1  -- new row
               ELSE 0
           END) OVER (ORDER BY sort_ord) AS row_cluster
    FROM Y_SORTED
),

-- ═══ Stage 2: Cell merging (PDF word fragments → logical cells) ════
-- Within each row, group adjacent bboxes into cells.
-- A new cell starts when the horizontal gap exceeds a threshold.
-- The threshold is generous: up to 1x font height catches commas,
-- decimals, and other fragments that PDF extractors split.
CELL_GAPS AS (
    SELECT ROWS.*,
           x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
           LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h
    FROM ROWS
),
CELLS AS (
    SELECT *,
           CASE WHEN grid_mode
                THEN ROW_NUMBER() OVER (PARTITION BY page_id, row_cluster ORDER BY x)
                ELSE SUM(CASE WHEN gap IS NULL OR gap > GREATEST(prev_h * 1.2, 5.0)
                              THEN 1 ELSE 0 END)
                         OVER (PARTITION BY page_id, row_cluster ORDER BY x)
           END AS cell_id
    FROM CELL_GAPS
),
MERGED AS (
    SELECT page_id, row_cluster, cell_id,
           MIN(x) AS x, MIN(y) AS y,
           MAX(x + w) - MIN(x) AS w, MAX(h) AS h,
           -- Concatenate text within cell: punctuation-only fragments
           -- (commas, periods) get glued without space to the prior word.
           STRING_AGG(
               CASE WHEN regexp_matches(text, '^[,.:;]+$') THEN text
                    ELSE ' ' || text END,
               '' ORDER BY x
           ) AS text_raw,
           MODE(weight) AS weight,
           MODE(italic) AS italic,
           MODE(font_size) AS font_size,
           MODE(color) AS color,
           COUNT(*) AS n_fragments
    FROM CELLS
    GROUP BY page_id, row_cluster, cell_id
),
-- Clean up merged text: trim leading space, collapse whitespace
CLEAN AS (
    SELECT *,
           TRIM(REGEXP_REPLACE(text_raw, '\\s+', ' ', 'g')) AS text
    FROM MERGED
),

-- ═══ Stage 3: Page-level scope detection ════════════════════════════
-- Page title: largest font size on page, typically first row.
-- Page number: bbox at extreme y (bottom) with short numeric text.
-- These form the outermost scope layer.
PAGE_FONT_STATS AS (
    SELECT page_id,
           MAX(font_size) AS max_fs,
           MODE(font_size) AS body_fs
    FROM CLEAN
    GROUP BY page_id
),
PAGE_SCOPE AS (
    SELECT m.page_id, m.row_cluster, m.cell_id, m.x, m.y, m.text,
           m.font_size, m.weight,
           CASE
               -- Title: font size > body font AND in top 15% of page
               WHEN m.font_size > pfs.body_fs * 1.3
                    AND m.y < (SELECT MAX(y) FROM CLEAN AS m2 WHERE m2.page_id = m.page_id) * 0.15
               THEN 'page_title'
               -- Page number: short text at bottom of page, numeric
               WHEN m.y > (SELECT MAX(y) FROM CLEAN AS m2 WHERE m2.page_id = m.page_id) * 0.90
                    AND LENGTH(m.text) <= 5
                    AND TRY_CAST(TRIM(m.text) AS INTEGER) IS NOT NULL
               THEN 'page_number'
               ELSE NULL
           END AS page_role
    FROM CLEAN AS m
    JOIN PAGE_FONT_STATS AS pfs USING (page_id)
),

-- ═══ Stage 4: Round 1 — Mask numeric/date/regex cells ═══════════════
-- These are unambiguous: TRY_CAST and regex give certain classification.
ROUND1 AS (
    SELECT m.page_id, m.row_cluster, m.cell_id, m.x, m.y, m.w, m.h,
           m.text, m.weight, m.italic, m.font_size, m.color, m.n_fragments,
           ps.page_role,
           CASE
               WHEN ps.page_role IS NOT NULL THEN ps.page_role
               WHEN TRY_CAST(REPLACE(REPLACE(m.text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL
               THEN 'measure'
               WHEN TRY_CAST(m.text AS DATE) IS NOT NULL
                 OR TRY_CAST(m.text AS TIMESTAMP) IS NOT NULL
               THEN 'date'
               WHEN regexp_matches(m.text, '^\\$[\\d,.]+$') THEN 'currency'
               WHEN regexp_matches(m.text, '^[\\d,.]+\\s*%$') THEN 'percentage'
               WHEN regexp_matches(m.text, '^\\d{5}(-\\d{4})?$') THEN 'zip_code'
               WHEN regexp_matches(m.text, '^\\d{4}-\\d{2}-\\d{2}') THEN 'iso_date'
               ELSE NULL
           END AS round1_role
    FROM CLEAN AS m
    LEFT JOIN PAGE_SCOPE AS ps
        ON m.page_id = ps.page_id
       AND m.row_cluster = ps.row_cluster
       AND m.cell_id = ps.cell_id
),

-- ═══ Stage 5: Detect indent levels for label scope ══════════════════
-- Cluster the x-positions of leftmost cells per row into indent levels.
-- Only consider cells that survived round 1 (not yet masked).
ROW_LEFT_EDGES AS (
    SELECT row_cluster, MIN(x) AS left_x
    FROM ROUND1
    WHERE round1_role IS NULL  -- not yet masked
    GROUP BY row_cluster
),
-- Cluster left edges into indent levels using gap-based clustering.
-- Sort distinct left_x values; a gap > 5pt starts a new indent level.
-- This adapts to the document's actual indentation rather than fixed bins.
SORTED_EDGES AS (
    SELECT left_x,
           left_x - LAG(left_x) OVER (ORDER BY left_x) AS gap
    FROM (SELECT DISTINCT left_x FROM ROW_LEFT_EDGES) AS t
),
INDENT_BINS AS (
    SELECT left_x,
           SUM(CASE WHEN gap IS NULL OR gap > 5 THEN 1 ELSE 0 END)
               OVER (ORDER BY left_x) - 1 AS indent_level
    FROM SORTED_EDGES
),

-- ═══ Stage 6: Identify labels ═══════════════════════════════════════
-- A label is a bold or larger-font cell that is the leftmost in its row,
-- and the row has few or no numeric cells.
ROW_NUMERIC_COUNT AS (
    SELECT row_cluster, COUNT(*) AS n_cells,
           SUM(CASE WHEN round1_role IN ('measure', 'currency', 'percentage') THEN 1 ELSE 0 END) AS n_measures
    FROM ROUND1
    GROUP BY row_cluster
),
LABELS AS (
    SELECT r1.page_id, r1.row_cluster, r1.cell_id,
           r1.x, r1.y, r1.text, r1.weight, r1.font_size,
           ib.indent_level,
           -- A row is a "pure label" if it has no measure cells
           -- A row is a "label + measures" if the leftmost cell is bold
           CASE
               WHEN rnc.n_measures = 0 AND (r1.weight = 'bold' OR r1.font_size > (SELECT body_fs FROM PAGE_FONT_STATS LIMIT 1))
               THEN 'section_label'
               WHEN rnc.n_measures > 0 AND r1.weight = 'bold'
               THEN 'total_label'
               WHEN rnc.n_measures = 0
               THEN 'row_label'
               ELSE NULL
           END AS label_type
    FROM ROUND1 AS r1
    JOIN ROW_LEFT_EDGES AS rle ON r1.row_cluster = rle.row_cluster AND r1.x = rle.left_x
    JOIN INDENT_BINS AS ib ON rle.left_x = ib.left_x
    JOIN ROW_NUMERIC_COUNT AS rnc ON r1.row_cluster = rnc.row_cluster
    WHERE r1.round1_role IS NULL
      AND r1.cell_id = 1  -- leftmost cell in the row
),

-- ═══ Stage 7: Label scope propagation ═══════════════════════════════
-- Each label is "in force" downward until the next label at the same
-- or lesser indent level.
--
-- For each row, the in-force labels at each indent level are the most
-- recent label at that level, scanning upward — BUT a label at level N
-- resets all scopes at level > N.
--
-- We handle this with a series of LAST_VALUE IGNORE NULLS window
-- functions, one per indent level.  A level-0 label appearing at row R
-- means the level-1 and level-2 scopes from before row R are stale.
-- We detect this by checking whether the level-0 label's row >= the
-- level-1 label's row.

-- First, attach label info to every row
ROW_LABELS AS (
    SELECT rc.row_cluster,
           l.indent_level,
           l.label_type,
           l.text AS label_text
    FROM ROW_NUMERIC_COUNT AS rc
    LEFT JOIN LABELS AS l ON rc.row_cluster = l.row_cluster
),
-- Propagate: for each row, carry forward the most recent label at each level
SCOPE_PROPAGATED AS (
    SELECT row_cluster,
           -- Level 0 scope
           LAST_VALUE(CASE WHEN indent_level = 0 AND label_text IS NOT NULL
                           THEN label_text END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l0,
           LAST_VALUE(CASE WHEN indent_level = 0 AND label_text IS NOT NULL
                           THEN row_cluster END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l0_row,
           -- Level 1 scope
           LAST_VALUE(CASE WHEN indent_level = 1 AND label_text IS NOT NULL
                           THEN label_text END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l1_raw,
           LAST_VALUE(CASE WHEN indent_level = 1 AND label_text IS NOT NULL
                           THEN row_cluster END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l1_row,
           -- Level 2 scope
           LAST_VALUE(CASE WHEN indent_level = 2 AND label_text IS NOT NULL
                           THEN label_text END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l2_raw,
           LAST_VALUE(CASE WHEN indent_level = 2 AND label_text IS NOT NULL
                           THEN row_cluster END IGNORE NULLS)
               OVER (ORDER BY row_cluster
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l2_row
    FROM ROW_LABELS
),
-- Apply reset rule: a label at level N invalidates deeper scopes
-- that were set BEFORE the level-N label
SCOPED AS (
    SELECT row_cluster,
           scope_l0,
           -- Level 1 is valid only if it was set AFTER the current level-0 label
           CASE WHEN scope_l1_row >= scope_l0_row THEN scope_l1_raw ELSE NULL END AS scope_l1,
           -- Level 2 is valid only if it was set after both level-0 and level-1
           CASE WHEN scope_l2_row >= scope_l0_row
                 AND scope_l2_row >= COALESCE(
                     CASE WHEN scope_l1_row >= scope_l0_row THEN scope_l1_row END,
                     0)
                THEN scope_l2_raw ELSE NULL END AS scope_l2
    FROM SCOPE_PROPAGATED
),

-- ═══ Stage 8: Column header detection ═══════════════════════════════
-- The header row is the last all-text row before the first data row.
BODY_START AS (
    SELECT COALESCE(MIN(row_cluster), 1) AS first_data_row
    FROM ROW_NUMERIC_COUNT
    WHERE n_measures > 0 AND n_cells >= 2
),
HEADER_ROW AS (
    SELECT MAX(r1.row_cluster) AS header_row_id
    FROM ROUND1 AS r1, BODY_START AS bs
    WHERE r1.row_cluster < bs.first_data_row
      AND r1.round1_role IS NULL
),
-- Extract column headers: map cell x-position to header text
COL_HEADERS AS (
    SELECT r1.x, r1.w, r1.text AS col_header
    FROM ROUND1 AS r1, HEADER_ROW AS hr
    WHERE r1.row_cluster = hr.header_row_id
      AND r1.round1_role IS NULL
),

-- ═══ Stage 9: Final assembly ════════════════════════════════════════
-- Attach scope labels and column headers to every cell.
FINAL AS (
    SELECT r1.page_id, r1.row_cluster, r1.cell_id,
           r1.x, r1.y, r1.w, r1.h,
           r1.text,
           r1.weight, r1.font_size,
           -- Classification
           COALESCE(r1.round1_role,
                    l.label_type,
                    'unresolved') AS role,
           -- Page scope
           (SELECT text FROM ROUND1
            WHERE round1_role = 'page_title' AND page_id = r1.page_id
            LIMIT 1) AS page_title,
           -- Label scope (hierarchical)
           s.scope_l0,
           s.scope_l1,
           s.scope_l2,
           -- Column header (nearest header cell by x overlap)
           (SELECT ch.col_header FROM COL_HEADERS AS ch
            WHERE r1.x + r1.w/2 BETWEEN ch.x - 5 AND ch.x + ch.w + 5
            ORDER BY ABS(r1.x + r1.w/2 - ch.x - ch.w/2)
            LIMIT 1) AS col_header,
           -- Treemap path: page_title / scope_l0 / scope_l1 / scope_l2 / col_header
           -- Each non-null scope level is a segment in the path.
           -- Structural tiles (labels) contribute path segments;
           -- leaf tiles (measures/dimensions) are addressed BY the path.
           CONCAT_WS(' / ',
               (SELECT text FROM ROUND1
                WHERE round1_role = 'page_title' AND page_id = r1.page_id
                LIMIT 1),
               s.scope_l0,
               s.scope_l1,
               s.scope_l2,
               (SELECT ch.col_header FROM COL_HEADERS AS ch
                WHERE r1.x + r1.w/2 BETWEEN ch.x - 5 AND ch.x + ch.w + 5
                ORDER BY ABS(r1.x + r1.w/2 - ch.x - ch.w/2)
                LIMIT 1)
           ) AS treemap_path
    FROM ROUND1 AS r1
    LEFT JOIN LABELS AS l ON r1.row_cluster = l.row_cluster AND r1.cell_id = l.cell_id
    LEFT JOIN SCOPED AS s ON r1.row_cluster = s.row_cluster
)

SELECT * FROM FINAL
ORDER BY page_id, row_cluster, x
"""


def run_pipeline(filepath: str):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")

    sql = PIPELINE_SQL.replace('{file}', filepath)
    name = Path(filepath).name
    print(f"\n{'='*90}")
    print(f"  {name}")
    print(f"{'='*90}")

    df = con.execute(sql).df()

    import pandas as pd

    def _s(val):
        """Convert NaN/None to empty string for display."""
        if val is None or (isinstance(val, float) and pd.isna(val)):
            return ''
        return str(val)

    # Print with scope context
    prev_l0 = prev_l1 = prev_l2 = ''
    for _, r in df.iterrows():
        l0, l1, l2 = _s(r.get('scope_l0')), _s(r.get('scope_l1')), _s(r.get('scope_l2'))

        # Show scope changes
        if l0 != prev_l0 and l0:
            print(f"\n  ┌─ {l0}")
            prev_l0 = l0
            prev_l1 = prev_l2 = ''
        if l1 != prev_l1 and l1:
            print(f"  │ ┌─ {l1}")
            prev_l1 = l1
            prev_l2 = ''
        if l2 != prev_l2 and l2:
            print(f"  │ │ ┌─ {l2}")
            prev_l2 = l2

        role = r['role']
        col = _s(r.get('col_header'))
        text = str(r['text'])[:30]

        # Indentation based on role
        if role in ('page_title', 'page_number'):
            prefix = "◉ "
        elif role == 'section_label':
            prefix = "▸ "
        elif role in ('total_label', 'row_label'):
            prefix = "  ▹ "
        else:
            prefix = "    "

        col_str = f"  col={col}" if col else ""
        print(f"  {prefix}{text:<30s}  [{role:<15s}]{col_str}")

    # Summary
    print(f"\n  {'─'*60}")
    role_counts = df.groupby('role').size().sort_values(ascending=False)
    total = len(df)
    resolved = total - role_counts.get('unresolved', 0)
    print(f"  Resolved: {resolved}/{total} ({100*resolved/total:.0f}%)")
    for role, n in role_counts.items():
        print(f"    {role:<20s} {n:4d} ({100*n/total:5.1f}%)")

    # Show fully-qualified facts as treemap paths
    measures = df[df['role'].isin(['measure', 'currency', 'percentage'])]
    if len(measures) > 0:
        print(f"\n  Treemap-addressed facts (first 15):")
        for _, m in measures.head(15).iterrows():
            path = m.get('treemap_path', '') or '(no path)'
            print(f"    {path} = {m['text']}")

    con.close()


def main():
    files = [
        "test_data/synthetic/financial_statement.pdf",
        "test_data/synthetic/01_sales_simple.pdf",
        "test_data/synthetic/04_multi_table.pdf",
        "test_data/synthetic/06_sales_simple.xlsx",
    ]
    for f in files:
        if Path(f).exists():
            try:
                run_pipeline(f)
            except Exception as e:
                print(f"  ERROR: {e}")
                import traceback
                traceback.print_exc()


if __name__ == "__main__":
    main()
