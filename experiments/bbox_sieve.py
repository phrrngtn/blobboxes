"""Bbox sieve: cheap SQL-based classification of bboxes before embeddings.

Reads bboxes through the DuckDB extension, then applies sieve layers
entirely in SQL to tag each bbox with type/role signals. The goal is
to eliminate most bboxes from needing expensive embedding lookups.

Layers (cheapest first):
  1. Style signals  — bold/italic/color → header candidate
  2. TRY_CAST       — numeric, date, boolean detection
  3. Regex probes   — currency, percentage, phone, email, zip, UUID, ISO date
  4. Spatial role    — position relative to detected row/column structure
  5. (future) blobfilter membership — exact domain probing via roaring bitmaps

After these layers, each bbox has a "role" tag:
  header, numeric, currency, date, percentage, identifier, text, noise
Only "text" bboxes (unresolved) need embedding.
"""

import duckdb
from pathlib import Path

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")

SIEVE_SQL = """
WITH
-- Layer 0: raw bboxes with styles joined
RAW AS (
    SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
           s.font_size, s.weight, s.italic, s.color
    FROM bb('{file}') AS b
    JOIN bb_styles('{file}') AS s USING (style_id)
),

-- Layer 1: row clustering via y-midpoint bucketing
-- Assign each bbox to a row by rounding its vertical midpoint
ROWS AS (
    SELECT *,
           DENSE_RANK() OVER (ORDER BY round((y + h/2) / GREATEST(h, 1.0)) ) AS row_cluster
    FROM RAW
),

-- Layer 2: merge adjacent bboxes on same row into cells
-- For XLSX (w=1, h=1, integer coords) each bbox IS a cell already.
-- For PDF (point coords, word-per-bbox) we merge by x-gap.
-- Detect XLSX by checking if all w values are exactly 1.0.
IS_GRID AS (
    SELECT CASE WHEN COUNT(DISTINCT w) = 1 AND MIN(w) = 1.0 THEN true ELSE false END AS grid_mode
    FROM ROWS
),
CELL_GAPS AS (
    SELECT ROWS.*,
           x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
           LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h,
           grid_mode
    FROM ROWS, IS_GRID
),
CELLS AS (
    SELECT *,
           CASE
               -- XLSX: every bbox is its own cell
               WHEN grid_mode THEN ROW_NUMBER() OVER (PARTITION BY page_id, row_cluster ORDER BY x)
               -- PDF: merge by gap threshold
               ELSE SUM(CASE WHEN gap IS NULL OR gap > GREATEST(prev_h * 0.8, 3.0) THEN 1 ELSE 0 END)
                        OVER (PARTITION BY page_id, row_cluster ORDER BY x)
           END AS cell_id
    FROM CELL_GAPS
),
-- Aggregate text within each cell, inserting spaces for word gaps
MERGED AS (
    SELECT page_id, row_cluster, cell_id,
           MIN(x) AS x,
           MIN(y) AS y,
           MAX(x + w) - MIN(x) AS w,
           MAX(h) AS h,
           STRING_AGG(text, ' ' ORDER BY x) AS text,
           -- Style signals: take the dominant style in the cell
           MODE(weight) AS weight,
           MODE(italic) AS italic,
           MODE(font_size) AS font_size,
           MODE(color) AS color,
           COUNT(*) AS n_fragments
    FROM CELLS
    GROUP BY page_id, row_cluster, cell_id
),

-- Layer 2b: spatial role — detect header/body boundary
-- Header rows: bold OR all-text rows before the first row with numeric content.
-- "Body starts after the first row that has numeric cells" is a cheap
-- geometric heuristic: the transition from text-only to mixed content
-- marks the header/body boundary.
ROW_STATS AS (
    SELECT row_cluster,
           COUNT(*) AS n_cells,
           SUM(CASE WHEN TRY_CAST(REPLACE(REPLACE(text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL
                    THEN 1 ELSE 0 END) AS n_numeric,
           BOOL_OR(weight = 'bold') AS has_bold
    FROM MERGED
    GROUP BY row_cluster
),
BODY_START AS (
    SELECT COALESCE(MIN(row_cluster), 1) AS first_data_row
    FROM ROW_STATS
    WHERE n_numeric > 0 AND n_cells >= 2
),

-- Layer 3: TRY_CAST type detection + regex probes
TYPED AS (
    SELECT m.*,
           -- Spatial role: is this row above the first data row?
           CASE WHEN m.row_cluster < bs.first_data_row THEN true ELSE false END AS is_pre_data,
           -- Style-based header signal
           CASE WHEN m.weight = 'bold' THEN true ELSE false END AS is_bold,

           -- TRY_CAST probes: can the text be parsed as a number or date?
           CASE WHEN TRY_CAST(REPLACE(REPLACE(text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL
                THEN true ELSE false END AS is_numeric,

           CASE WHEN TRY_CAST(text AS DATE) IS NOT NULL
                  OR TRY_CAST(text AS TIMESTAMP) IS NOT NULL
                THEN true ELSE false END AS is_date,

           -- Regex probes (cheap pattern matching)
           CASE WHEN regexp_matches(text, '^\$[\d,.]+$')
                  OR regexp_matches(text, '^[\d,.]+\s*(?:USD|EUR|GBP|JPY)$')
                THEN 'currency'
                WHEN regexp_matches(text, '^[\d,.]+\s*%$')
                THEN 'percentage'
                WHEN regexp_matches(text, '^\(?\d{3}\)?[\s.-]?\d{3}[\s.-]?\d{4}$')
                THEN 'phone'
                WHEN regexp_matches(text, '^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$')
                THEN 'email'
                WHEN regexp_matches(text, '^\d{5}(-\d{4})?$')
                THEN 'zip_code'
                WHEN regexp_matches(text, '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$')
                THEN 'uuid'
                WHEN regexp_matches(text, '^\d{4}-\d{2}-\d{2}')
                THEN 'iso_date'
                WHEN regexp_matches(text, '^\d+$')
                THEN 'integer'
                WHEN regexp_matches(text, '^[\d,.]+$')
                THEN 'number'
                ELSE NULL
           END AS regex_domain,

           -- Length and character-class signals
           LENGTH(text) AS text_len,
           LENGTH(regexp_replace(text, '[^0-9]', '', 'g')) AS digit_count,
           LENGTH(regexp_replace(text, '[^A-Za-z]', '', 'g')) AS alpha_count

    FROM MERGED AS m, BODY_START AS bs
),

-- Layer 4: classify each cell's role
CLASSIFIED AS (
    SELECT *,
           CASE
               -- Rows before the first data row → header region
               -- (title, subtitle, column headers — no embedding needed)
               WHEN is_pre_data
               THEN 'header'
               -- Bold text in the body with no numeric content → section label
               WHEN is_bold AND NOT is_numeric AND regex_domain IS NULL
               THEN 'label'
               -- Regex matched a specific domain
               WHEN regex_domain IS NOT NULL
               THEN regex_domain
               -- Pure numeric
               WHEN is_numeric AND NOT is_date
               THEN 'numeric'
               -- Date
               WHEN is_date
               THEN 'date'
               -- Short text that's all alpha → could be a code or label
               WHEN text_len <= 3 AND alpha_count = text_len
               THEN 'code'
               -- Everything else: unresolved, needs embedding
               ELSE 'text'
           END AS role
    FROM TYPED
)

SELECT page_id, row_cluster, cell_id,
       round(x, 1) AS x, round(y, 1) AS y,
       round(w, 1) AS w, round(h, 1) AS h,
       text, role, regex_domain,
       is_bold, is_numeric, is_date
FROM CLASSIFIED
ORDER BY page_id, row_cluster, x
"""

SUMMARY_SQL = """
WITH SIEVE AS ({inner_sql})
SELECT role,
       COUNT(*) AS n_cells,
       ROUND(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 1) AS pct
FROM SIEVE
GROUP BY role
ORDER BY n_cells DESC
"""


def run_sieve(filepath: str):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")

    sql = SIEVE_SQL.replace('{file}', filepath)
    print(f"\n{'='*70}")
    print(f"  {Path(filepath).name}")
    print(f"{'='*70}")

    df = con.execute(sql).df()

    # Print first rows grouped by row_cluster
    seen_rows = 0
    for rc in sorted(df["row_cluster"].unique()):
        if seen_rows >= 5:
            print(f"  ... ({len(df['row_cluster'].unique()) - 5} more rows)")
            break
        row = df[df["row_cluster"] == rc].sort_values("x")
        cells = []
        for _, c in row.iterrows():
            tag = c["role"]
            if c["is_bold"]:
                tag = f"BOLD:{tag}"
            cells.append(f"{c['text'][:20]:20s} [{tag}]")
        print(f"  row {rc:2d}: {' | '.join(cells)}")
        seen_rows += 1

    # Summary: how many cells per role?
    summary_sql = SUMMARY_SQL.replace('{inner_sql}', sql)
    summary = con.execute(summary_sql).df()
    print(f"\n  Role distribution:")
    for _, r in summary.iterrows():
        bar = "#" * int(r["pct"] / 2)
        print(f"    {r['role']:12s} {int(r['n_cells']):4d} ({r['pct']:5.1f}%) {bar}")

    # Key metric: what fraction DOESN'T need embedding?
    resolved = summary[summary["role"] != "text"]["n_cells"].sum()
    total = summary["n_cells"].sum()
    print(f"\n  Resolved without embedding: {resolved}/{total} "
          f"({100*resolved/total:.0f}%)")
    print(f"  Need embedding:             {total - resolved}/{total} "
          f"({100*(total-resolved)/total:.0f}%)")

    con.close()


def main():
    files = [
        str(SYNTH / "01_sales_simple.pdf"),
        str(SYNTH / "02_employee_simple.pdf"),
        str(SYNTH / "05_countries_footnotes.pdf"),
        str(SYNTH / "06_sales_simple.xlsx"),
        str(SYNTH / "07_inventory_titled.xlsx"),
        str(SYNTH / "census_pop.xlsx"),
        str(SYNTH.parent / "sample.pdf"),
    ]
    for f in files:
        if Path(f).exists():
            try:
                run_sieve(f)
            except Exception as e:
                print(f"  ERROR: {e}")
                import traceback
                traceback.print_exc()


if __name__ == "__main__":
    main()
