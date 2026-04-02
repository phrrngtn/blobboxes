"""Test the hypothesis: classified scatter from cheap single-element
probes IS the table detector. No explicit table detection step needed.

For each PDF/XLSX:
1. Extract bboxes + styles
2. Run single-element pattern classification (TRY_CAST, regex)
3. Find rows where multiple cells classified as measures
4. Report whether those rows form rectangular table candidates
"""
import duckdb
from pathlib import Path
import sys

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"

CLASSIFY_SQL = """
WITH
RAW AS (
    SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
           s.font_size, s.weight, s.color, f.name AS font_name,
           CASE WHEN f.name ILIKE '%bold%' OR s.weight = 'bold'
                THEN 'bold' ELSE 'normal' END AS eff_weight
    FROM bb('{file}') AS b
    JOIN bb_styles('{file}') AS s USING (style_id)
    JOIN bb_fonts('{file}') AS f USING (font_id)
),

-- Style histogram: count bboxes per style
STYLE_HIST AS (
    SELECT style_id, font_size, eff_weight, color, font_name,
           COUNT(*) AS n_bboxes,
           RANK() OVER (ORDER BY COUNT(*) DESC) AS style_rank
    FROM RAW
    GROUP BY style_id, font_size, eff_weight, color, font_name
),

-- Row clustering (gap-based)
BODY_H AS (
    SELECT GREATEST(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY h)
                    FILTER (WHERE h > 2), 4.0) AS bh
    FROM RAW
),
Y_SORTED AS (
    SELECT RAW.*,
           ROW_NUMBER() OVER (ORDER BY page_id, y, x) AS sort_ord,
           y - LAG(y) OVER (ORDER BY page_id, y, x) AS y_gap,
           LAG(page_id) OVER (ORDER BY page_id, y, x) AS prev_page
    FROM RAW
),
ROWS AS (
    SELECT * EXCLUDE (sort_ord, y_gap, prev_page),
           SUM(CASE
               WHEN y_gap IS NULL THEN 1
               WHEN prev_page != page_id THEN 1
               WHEN y_gap > (SELECT bh FROM BODY_H) THEN 1
               ELSE 0
           END) OVER (ORDER BY sort_ord) AS row_cluster
    FROM Y_SORTED
),

-- Cell merging
CELL_GAPS AS (
    SELECT ROWS.*,
           x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
           LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h
    FROM ROWS
),
CELLS AS (
    SELECT *,
           SUM(CASE WHEN gap IS NULL OR gap > GREATEST(COALESCE(prev_h, h) * 1.2, 5.0)
                    THEN 1 ELSE 0 END)
               OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS cell_id
    FROM CELL_GAPS
),
FRAG_TAGS AS (
    SELECT CELLS.*,
           regexp_matches(text, '^[,.:;]+$') AS is_punct,
           COALESCE(regexp_matches(
               LAG(text) OVER (PARTITION BY page_id, row_cluster, cell_id ORDER BY x),
               '^[,.:;]+$'), false) AS after_punct
    FROM CELLS
),
MERGED AS (
    SELECT page_id, row_cluster, cell_id,
           MIN(x) AS x, MIN(y) AS y,
           MAX(x + w) - MIN(x) AS w, MAX(h) AS h,
           TRIM(REGEXP_REPLACE(
               STRING_AGG(
                   CASE WHEN is_punct THEN text
                        WHEN after_punct THEN text
                        ELSE ' ' || text END,
                   '' ORDER BY x),
               '\\s+', ' ', 'g')) AS text,
           MODE(eff_weight) AS weight,
           MODE(font_size) AS font_size,
           MODE(style_id) AS style_id
    FROM FRAG_TAGS
    GROUP BY page_id, row_cluster, cell_id
),

-- Pass 1: single-element pattern classification
CLASSIFIED AS (
    SELECT m.*,
           sh.style_rank,
           CASE
               WHEN TRY_CAST(REPLACE(REPLACE(REPLACE(m.text, ',', ''), '$', ''), ' ', '') AS DOUBLE) IS NOT NULL
               THEN 'measure'
               WHEN TRY_CAST(m.text AS DATE) IS NOT NULL
               THEN 'date'
               WHEN regexp_matches(m.text, '^\\$[\\d,.]+$') THEN 'currency'
               WHEN regexp_matches(m.text, '^[\\d,.]+\\s*%$') THEN 'percentage'
               WHEN regexp_matches(m.text, '^\\d{5}(-\\d{4})?$') THEN 'zip_code'
               WHEN regexp_matches(m.text, '^\\d{4}-\\d{2}-\\d{2}') THEN 'iso_date'
               WHEN m.weight = 'bold' AND sh.style_rank > 1 THEN 'structural_bold'
               WHEN sh.style_rank >= 3 THEN 'structural_rare'
               ELSE NULL
           END AS pass1_role
    FROM MERGED AS m
    LEFT JOIN STYLE_HIST AS sh USING (style_id)
),

-- Table candidate detection from classified scatter
-- A "data row" has >= 2 measure/currency/percentage/date cells
ROW_MEASURE_COUNTS AS (
    SELECT page_id, row_cluster,
           COUNT(*) AS n_cells,
           SUM(CASE WHEN pass1_role IN ('measure', 'currency', 'percentage', 'date', 'iso_date', 'zip_code')
                    THEN 1 ELSE 0 END) AS n_typed,
           SUM(CASE WHEN pass1_role IS NULL THEN 1 ELSE 0 END) AS n_unresolved,
           MIN(x) AS row_left, MAX(x + w) AS row_right
    FROM CLASSIFIED
    GROUP BY page_id, row_cluster
),
DATA_ROWS AS (
    SELECT *, (n_typed >= 2) AS is_data_row
    FROM ROW_MEASURE_COUNTS
),
-- Find contiguous runs of data rows = table candidates
TABLE_CANDIDATES AS (
    SELECT page_id,
           MIN(row_cluster) AS start_row,
           MAX(row_cluster) AS end_row,
           COUNT(*) AS n_data_rows,
           MODE(n_cells) AS modal_cols,
           MIN(row_left) AS table_left,
           MAX(row_right) AS table_right
    FROM (
        SELECT *,
               row_cluster - ROW_NUMBER() OVER (PARTITION BY page_id ORDER BY row_cluster) AS grp
        FROM DATA_ROWS
        WHERE is_data_row
    ) AS t
    GROUP BY page_id, grp
    HAVING COUNT(*) >= 2
)

SELECT 'style_hist' AS section, style_rank, font_name, font_size, eff_weight, n_bboxes, NULL AS n_typed, NULL AS modal_cols, NULL AS table_left, NULL AS table_right
FROM STYLE_HIST
WHERE style_rank <= 5

UNION ALL

SELECT 'summary', NULL, NULL, NULL, NULL,
       (SELECT COUNT(*) FROM CLASSIFIED),
       (SELECT SUM(CASE WHEN pass1_role IS NOT NULL THEN 1 ELSE 0 END) FROM CLASSIFIED),
       NULL, NULL, NULL

UNION ALL

SELECT 'table_candidate', NULL, NULL, NULL, NULL,
       n_data_rows, modal_cols, NULL, table_left, table_right
FROM TABLE_CANDIDATES
"""


def run(filepath: str):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")

    name = Path(filepath).name
    sql = CLASSIFY_SQL.replace('{file}', filepath)

    try:
        df = con.execute(sql).df()
    except Exception as e:
        print(f"  {name:40s}  ERROR: {e}")
        con.close()
        return

    styles = df[df['section'] == 'style_hist'].sort_values('style_rank')
    summary = df[df['section'] == 'summary'].iloc[0]
    tables = df[df['section'] == 'table_candidate']

    total = int(summary['n_bboxes'])
    classified = int(summary['n_typed'])
    pct = 100 * classified / total if total > 0 else 0
    n_tables = len(tables)

    # Compact output
    table_desc = ""
    if n_tables > 0:
        parts = []
        for _, t in tables.iterrows():
            parts.append(f"{int(t['n_bboxes'])}r×{int(t['n_typed'])}c")
        table_desc = " tables=" + ",".join(parts)

    style_desc = ""
    for _, s in styles.head(3).iterrows():
        fn = str(s['font_name'])[:15] if s['font_name'] else '?'
        style_desc += f" [{int(s['style_rank'])}:{fn}/{s['font_size']:.0f}/{s['eff_weight'][:1]}={int(s['n_bboxes'])}]"

    print(f"  {name:40s}  {total:4d} bboxes  {classified:4d} classified ({pct:4.0f}%)  {n_tables} table candidates{table_desc}{style_desc}")

    con.close()


def main():
    test_dir = Path("/Users/paulharrington/checkouts/blobboxes/test_data")
    files = sorted(test_dir.rglob("*.pdf")) + sorted(test_dir.rglob("*.xlsx"))

    print(f"{'  File':<42s}  {'Bbox':>4s} {'boxes':>5s}  {'Class':>4s} {'ified':>5s}  {'Tables':>6s}")
    print(f"  {'─' * 100}")

    for f in files:
        run(str(f))


if __name__ == "__main__":
    main()
