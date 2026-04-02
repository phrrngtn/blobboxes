"""Scatter pipeline: the full cheap-to-expensive classification flow.

Pass 1: Regex/TRY_CAST → candidate measure cells (SQL)
Pass 2: Geometry → putative columns from measure scatter (SQL)
Pass 3: Residual classification by elimination (SQL)
  - Column headers: row above first data row, vertically subtends columns
  - Section labels: bold/rare style, leftmost, no measures in row
  - Prose: contiguous unresolved cells not in any table region

Python orchestrates; SQL does the arithmetic.
"""

import duckdb
from pathlib import Path

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"


def run_pipeline(filepath: str, con: duckdb.DuckDBPyConnection):
    """Run the full scatter pipeline on a single file."""
    fp = filepath
    name = Path(fp).name

    # ── Extract into temp tables (once) ──────────────────────────────
    con.execute(f"CREATE OR REPLACE TEMP TABLE raw_bboxes AS SELECT * FROM bb('{fp}')")
    con.execute(f"CREATE OR REPLACE TEMP TABLE raw_styles AS SELECT * FROM bb_styles('{fp}')")
    con.execute(f"CREATE OR REPLACE TEMP TABLE raw_fonts AS SELECT * FROM bb_fonts('{fp}')")
    con.execute(f"CREATE OR REPLACE TEMP TABLE raw_pages AS SELECT * FROM bb_pages('{fp}')")

    # ── Pass 0 + 1: Style histogram, row clustering, cell merge, pattern classify ─
    con.execute("""
    CREATE OR REPLACE TEMP TABLE cells AS
    WITH
    RAW AS (
        SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
               s.font_size, s.color,
               CASE WHEN f.name ILIKE '%bold%' OR s.weight = 'bold'
                    THEN 'bold' ELSE 'normal' END AS weight,
               f.name AS font_name
        FROM raw_bboxes AS b
        JOIN raw_styles AS s USING (style_id)
        JOIN raw_fonts AS f USING (font_id)
    ),

    -- Style histogram
    STYLE_HIST AS (
        SELECT style_id,
               COUNT(*) AS n_bboxes,
               RANK() OVER (ORDER BY COUNT(*) DESC) AS style_rank
        FROM RAW
        GROUP BY style_id
    ),

    -- Row clustering (gap-based on y)
    BODY_H AS (
        SELECT GREATEST(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY h)
                        FILTER (WHERE h > 2), 4.0) AS bh FROM RAW
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

    -- Cell merging with punctuation-aware concatenation
    CELL_GAPS AS (
        SELECT ROWS.*,
               x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
               LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h
        FROM ROWS
    ),
    CELL_IDS AS (
        SELECT *,
               SUM(CASE WHEN gap IS NULL OR gap > GREATEST(COALESCE(prev_h, h) * 1.2, 5.0)
                        THEN 1 ELSE 0 END)
                   OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS cell_id
        FROM CELL_GAPS
    ),
    FRAG_TAGS AS (
        SELECT CELL_IDS.*,
               regexp_matches(text, '^[,.:;]+$') AS is_punct,
               COALESCE(regexp_matches(
                   LAG(text) OVER (PARTITION BY page_id, row_cluster, cell_id ORDER BY x),
                   '^[,.:;]+$'), false) AS after_punct
        FROM CELL_IDS
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
               MODE(weight) AS weight,
               MODE(font_size) AS font_size,
               MODE(color) AS color,
               MODE(style_id) AS style_id,
               COUNT(*) AS n_fragments
        FROM FRAG_TAGS
        GROUP BY page_id, row_cluster, cell_id
    )

    -- Pass 1: pattern classification
    SELECT m.*,
           sh.style_rank,
           sh.n_bboxes AS style_count,
           CASE
               WHEN TRY_CAST(REPLACE(REPLACE(REPLACE(m.text, ',', ''), '$', ''), ' ', '') AS DOUBLE) IS NOT NULL
               THEN 'measure'
               WHEN TRY_CAST(m.text AS DATE) IS NOT NULL THEN 'date'
               WHEN regexp_matches(m.text, '^\\$[\\d,.]+$') THEN 'currency'
               WHEN regexp_matches(m.text, '^[\\d,.]+\\s*%$') THEN 'percentage'
               WHEN regexp_matches(m.text, '^\\d{5}(-\\d{4})?$') THEN 'zip_code'
               WHEN regexp_matches(m.text, '^\\d{4}-\\d{2}-\\d{2}') THEN 'iso_date'
               ELSE NULL
           END AS pass1_role
    FROM MERGED AS m
    LEFT JOIN STYLE_HIST AS sh USING (style_id)
    """)

    # ── Pass 2: Assemble table candidates from measure scatter ────────
    con.execute("""
    CREATE OR REPLACE TEMP TABLE table_regions AS
    WITH
    ROW_STATS AS (
        SELECT page_id, row_cluster,
               COUNT(*) AS n_cells,
               SUM(CASE WHEN pass1_role IN ('measure', 'currency', 'percentage', 'date', 'iso_date')
                        THEN 1 ELSE 0 END) AS n_typed,
               MIN(x) AS row_left, MAX(x + w) AS row_right,
               MIN(y) AS row_top, MAX(y + h) AS row_bottom
        FROM cells
        GROUP BY page_id, row_cluster
    ),
    DATA_ROWS AS (
        SELECT *, (n_typed >= 2) AS is_data_row
        FROM ROW_STATS
    ),
    -- Contiguous runs of data rows
    RUNS AS (
        SELECT *,
               row_cluster - ROW_NUMBER() OVER (PARTITION BY page_id ORDER BY row_cluster) AS grp
        FROM DATA_ROWS
        WHERE is_data_row
    )
    SELECT page_id, grp AS table_id,
           MIN(row_cluster) AS start_row,
           MAX(row_cluster) AS end_row,
           COUNT(*) AS n_data_rows,
           MODE(n_cells) AS modal_cols,
           MIN(row_left) AS table_left,
           MAX(row_right) AS table_right,
           MIN(row_top) AS table_top,
           MAX(row_bottom) AS table_bottom
    FROM RUNS
    GROUP BY page_id, grp
    HAVING COUNT(*) >= 2
    """)

    # ── Pass 2b: Putative columns from x-alignment of measure cells ───
    con.execute("""
    CREATE OR REPLACE TEMP TABLE putative_columns AS
    WITH
    -- Measure cells within table regions
    MEASURE_CELLS AS (
        SELECT c.page_id, c.row_cluster, c.cell_id,
               c.x, c.y, c.w, c.h, c.text,
               t.table_id,
               c.x + c.w / 2 AS x_center
        FROM cells AS c
        JOIN table_regions AS t
          ON c.page_id = t.page_id
         AND c.row_cluster BETWEEN t.start_row AND t.end_row
        WHERE c.pass1_role IN ('measure', 'currency', 'percentage', 'date', 'iso_date')
    ),
    -- Cluster x_center values into columns using gap-based approach
    SORTED_X AS (
        SELECT DISTINCT page_id, table_id, x_center,
               x_center - LAG(x_center) OVER (PARTITION BY page_id, table_id ORDER BY x_center) AS x_gap
        FROM MEASURE_CELLS
    ),
    COL_BINS AS (
        SELECT page_id, table_id, x_center,
               SUM(CASE WHEN x_gap IS NULL OR x_gap > 20 THEN 1 ELSE 0 END)
                   OVER (PARTITION BY page_id, table_id ORDER BY x_center) AS col_id
        FROM SORTED_X
    ),
    COL_RANGES AS (
        SELECT cb.page_id, cb.table_id, cb.col_id,
               MIN(mc.x) AS col_left,
               MAX(mc.x + mc.w) AS col_right,
               AVG(mc.x + mc.w / 2) AS col_center,
               COUNT(*) AS n_measure_cells
        FROM COL_BINS AS cb
        JOIN MEASURE_CELLS AS mc
          ON cb.page_id = mc.page_id
         AND cb.table_id = mc.table_id
         AND ABS(mc.x_center - cb.x_center) < 20
        GROUP BY cb.page_id, cb.table_id, cb.col_id
    )
    SELECT * FROM COL_RANGES
    """)

    # ── Pass 3: Residual classification ───────────────────────────────
    con.execute("""
    CREATE OR REPLACE TEMP TABLE classified AS
    WITH
    -- Extend table regions upward to include potential header rows
    -- Look for rows just above start_row that have similar cell count
    EXTENDED_REGIONS AS (
        SELECT t.*,
               -- Include 1-2 rows above start_row as header candidates
               GREATEST(t.start_row - 2, 1) AS search_start
        FROM table_regions AS t
    ),

    -- Find column headers via vertical subtension
    -- A cell in a row above the data region is a header if its
    -- horizontal extent overlaps a putative column
    HEADER_CANDIDATES AS (
        SELECT c.page_id, c.row_cluster, c.cell_id,
               c.x, c.y, c.w, c.h, c.text,
               c.weight, c.font_size, c.style_rank,
               pc.table_id, pc.col_id, pc.col_center,
               -- Overlap: header cell's x-range intersects column's x-range
               CASE WHEN c.x < pc.col_right AND c.x + c.w > pc.col_left
                    THEN true ELSE false END AS subtends_column
        FROM cells AS c
        JOIN EXTENDED_REGIONS AS er
          ON c.page_id = er.page_id
         AND c.row_cluster >= er.search_start
         AND c.row_cluster < er.start_row
        JOIN putative_columns AS pc
          ON c.page_id = pc.page_id
         AND er.table_id = pc.table_id
        WHERE c.pass1_role IS NULL  -- not already classified
    ),

    -- Best header per column: the subtending cell closest to the data
    BEST_HEADERS AS (
        SELECT DISTINCT ON (page_id, table_id, col_id)
               page_id, table_id, col_id,
               row_cluster AS header_row, cell_id AS header_cell,
               text AS header_text
        FROM HEADER_CANDIDATES
        WHERE subtends_column
        ORDER BY page_id, table_id, col_id, row_cluster DESC
    ),

    -- Section labels: bold or rare-style cells that are leftmost in
    -- their row and the row has no measure cells
    ROW_MEASURE_COUNT AS (
        SELECT page_id, row_cluster,
               COUNT(*) AS n_total,
               SUM(CASE WHEN pass1_role IN ('measure', 'currency', 'percentage', 'date', 'iso_date')
                        THEN 1 ELSE 0 END) AS n_measures,
               MIN(x) AS leftmost_x
        FROM cells
        GROUP BY page_id, row_cluster
    ),

    -- ══ Bayesian scoring: compute log-likelihood for each role ══════
    -- Each feature contributes a log-likelihood ratio per role.
    -- Final role = argmax of summed log-likelihoods.
    -- Using LN() scores so evidence multiplies → sums in log space.
    --
    -- Roles: measure, col_header, section_label, row_label, dimension, prose
    -- (measure/currency/percentage/date already resolved in pass1;
    --  we score the remaining 4 structural roles)
    --
    -- Convention: positive = evidence FOR this role, negative = AGAINST.

    FEATURES AS (
        SELECT c.*,
               t.table_id,
               t.start_row,
               t.modal_cols,
               pc.col_id,
               pc.col_center,
               bh.header_text AS col_header,
               rmc.n_measures AS row_n_measures,
               rmc.leftmost_x,
               rmc.n_total AS row_n_cells,
               -- Boolean features for scoring
               (c.pass1_role IS NOT NULL)                         AS f_pattern_match,
               (t.table_id IS NOT NULL)                           AS f_in_table,
               COALESCE(hc.subtends_column, false)                AS f_subtends_col,
               (c.row_cluster = t.start_row - 1)                  AS f_row_above_data,
               (c.row_cluster BETWEEN t.start_row AND t.end_row)  AS f_in_data_region,
               (c.weight = 'bold')                                AS f_bold,
               (c.style_rank >= 3)                                AS f_rare_style,
               (c.x <= rmc.leftmost_x + 5)                        AS f_leftmost,
               (rmc.n_measures = 0)                                AS f_no_measures_in_row,
               -- Continuous features
               COALESCE(ABS(rmc.n_total - t.modal_cols), 99)       AS f_cell_count_delta,
               c.font_size / NULLIF((SELECT PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY font_size) FROM cells), 0) AS f_font_size_ratio
        FROM cells AS c
        LEFT JOIN table_regions AS t
          ON c.page_id = t.page_id
         AND c.row_cluster BETWEEN GREATEST(t.start_row - 2, 1) AND t.end_row
        LEFT JOIN putative_columns AS pc
          ON c.page_id = pc.page_id
         AND t.table_id = pc.table_id
         AND c.x < pc.col_right AND c.x + c.w > pc.col_left
        LEFT JOIN BEST_HEADERS AS bh
          ON pc.page_id = bh.page_id
         AND pc.table_id = bh.table_id
         AND pc.col_id = bh.col_id
        LEFT JOIN HEADER_CANDIDATES AS hc
          ON c.page_id = hc.page_id
         AND c.row_cluster = hc.row_cluster
         AND c.cell_id = hc.cell_id
         AND hc.subtends_column
        LEFT JOIN ROW_MEASURE_COUNT AS rmc
          ON c.page_id = rmc.page_id
         AND c.row_cluster = rmc.row_cluster
    ),

    SCORED AS (
        SELECT *,
               -- ── col_header score ──
               -- Strong evidence: subtends a column AND row directly above data
               (CASE WHEN f_subtends_col AND f_row_above_data THEN 3.0
                     WHEN f_subtends_col THEN 1.5
                     WHEN f_row_above_data THEN 1.0
                     ELSE -2.0 END)
               -- Cell count similar to data rows → plausible header
             + (CASE WHEN f_cell_count_delta <= 1 AND f_in_table THEN 1.0
                     WHEN f_cell_count_delta <= 2 AND f_in_table THEN 0.5
                     ELSE 0.0 END)
               -- Not a pattern match (headers are text, not numbers)
             + (CASE WHEN f_pattern_match THEN -5.0 ELSE 0.5 END)
               -- Bold is slight evidence for header
             + (CASE WHEN f_bold THEN 0.3 ELSE 0.0 END)
               AS score_col_header,

               -- ── section_label score ──
               (CASE WHEN f_bold THEN 1.5 ELSE -0.5 END)
             + (CASE WHEN f_rare_style THEN 1.0 ELSE 0.0 END)
             + (CASE WHEN f_leftmost THEN 1.0 ELSE -1.0 END)
             + (CASE WHEN f_no_measures_in_row THEN 1.5 ELSE -2.0 END)
             + (CASE WHEN f_pattern_match THEN -5.0 ELSE 0.0 END)
               -- Larger font → more likely a label
             + (CASE WHEN f_font_size_ratio > 1.3 THEN 1.5
                     WHEN f_font_size_ratio > 1.1 THEN 0.5
                     ELSE 0.0 END)
               -- Subtends column → less likely a section label
             + (CASE WHEN f_subtends_col AND f_row_above_data THEN -2.0 ELSE 0.0 END)
               AS score_section_label,

               -- ── row_label score ──
               (CASE WHEN f_leftmost THEN 2.0 ELSE -2.0 END)
             + (CASE WHEN f_in_data_region THEN 1.5 ELSE -1.0 END)
             + (CASE WHEN f_pattern_match THEN -5.0 ELSE 0.0 END)
             + (CASE WHEN f_no_measures_in_row THEN -0.5 ELSE 0.5 END)
             + (CASE WHEN f_bold THEN 0.3 ELSE 0.0 END)
               AS score_row_label,

               -- ── dimension score (categorical value in a table) ──
               (CASE WHEN f_in_data_region THEN 1.5 ELSE -2.0 END)
             + (CASE WHEN NOT f_leftmost THEN 0.5 ELSE -0.5 END)
             + (CASE WHEN f_pattern_match THEN -5.0 ELSE 0.5 END)
             + (CASE WHEN f_in_table THEN 1.0 ELSE -1.0 END)
               AS score_dimension,

               -- ── prose score (default / background) ──
               (CASE WHEN NOT f_in_table THEN 2.0 ELSE -1.0 END)
             + (CASE WHEN f_pattern_match THEN -3.0 ELSE 0.5 END)
             + (CASE WHEN f_no_measures_in_row THEN 0.5 ELSE -0.5 END)
             + (CASE WHEN NOT f_bold AND NOT f_rare_style THEN 0.5 ELSE -0.5 END)
               AS score_prose
        FROM FEATURES
    ),

    -- Argmax: pick role with highest score
    TAGGED AS (
        SELECT S.*,
               CASE
                   WHEN f_pattern_match THEN pass1_role
                   WHEN GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
                        = score_col_header     THEN 'col_header'
                   WHEN GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
                        = score_section_label  THEN 'section_label'
                   WHEN GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
                        = score_row_label      THEN 'row_label'
                   WHEN GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
                        = score_dimension      THEN 'dimension'
                   ELSE 'prose'
               END AS role,
               -- Confidence: gap between best and second-best score
               GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
               - (
                   SELECT MAX(s) FROM (VALUES
                       (score_col_header), (score_section_label), (score_row_label),
                       (score_dimension), (score_prose)
                   ) AS t(s)
                   WHERE s < GREATEST(score_col_header, score_section_label, score_row_label, score_dimension, score_prose)
               ) AS confidence
        FROM SCORED AS S
    ),

    -- Page title: topmost section label on each page
    PAGE_TITLES AS (
        SELECT DISTINCT ON (page_id) page_id, text AS page_title
        FROM TAGGED
        WHERE role = 'section_label'
        ORDER BY page_id, row_cluster
    ),

    -- Section label indent levels
    LABEL_EDGES AS (
        SELECT DISTINCT x FROM TAGGED WHERE role = 'section_label'
    ),
    SORTED_LABEL_X AS (
        SELECT x, x - LAG(x) OVER (ORDER BY x) AS x_gap FROM LABEL_EDGES
    ),
    INDENT_BINS AS (
        SELECT x,
               SUM(CASE WHEN x_gap IS NULL OR x_gap > 5 THEN 1 ELSE 0 END)
                   OVER (ORDER BY x) - 1 AS indent_level
        FROM SORTED_LABEL_X
    ),

    -- Propagate section labels downward with level reset
    ROW_LABELS AS (
        SELECT t.row_cluster,
               CASE WHEN t.role = 'section_label' THEN ib.indent_level END AS indent_level,
               CASE WHEN t.role = 'section_label' THEN t.text END AS label_text
        FROM TAGGED AS t
        LEFT JOIN INDENT_BINS AS ib ON ABS(t.x - ib.x) < 3
        WHERE t.cell_id = 1  -- leftmost cell per row
    ),
    SCOPE_RAW AS (
        SELECT row_cluster,
               LAST_VALUE(CASE WHEN indent_level = 0 THEN label_text END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l0,
               LAST_VALUE(CASE WHEN indent_level = 0 THEN row_cluster END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l0_row,
               LAST_VALUE(CASE WHEN indent_level = 1 THEN label_text END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l1,
               LAST_VALUE(CASE WHEN indent_level = 1 THEN row_cluster END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l1_row,
               LAST_VALUE(CASE WHEN indent_level = 2 THEN label_text END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l2,
               LAST_VALUE(CASE WHEN indent_level = 2 THEN row_cluster END IGNORE NULLS)
                   OVER (ORDER BY row_cluster ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS scope_l2_row
        FROM ROW_LABELS
    ),
    SCOPED AS (
        SELECT row_cluster, scope_l0,
               CASE WHEN scope_l1_row >= scope_l0_row THEN scope_l1 END AS scope_l1,
               CASE WHEN scope_l2_row >= scope_l0_row
                     AND scope_l2_row >= COALESCE(
                         CASE WHEN scope_l1_row >= scope_l0_row THEN scope_l1_row END, 0)
                    THEN scope_l2 END AS scope_l2
        FROM SCOPE_RAW
    ),

    -- Assemble treemap path and final output
    FINAL AS (
        SELECT t.*,
               pt.page_title,
               s.scope_l0, s.scope_l1, s.scope_l2,
               -- Row label: leftmost cell text for row_label/section_label rows
               CASE WHEN t.role IN ('row_label', 'section_label') THEN NULL
                    ELSE (SELECT t2.text FROM TAGGED AS t2
                          WHERE t2.page_id = t.page_id
                            AND t2.row_cluster = t.row_cluster
                            AND t2.role = 'row_label'
                          LIMIT 1)
               END AS row_label_text,
               -- Treemap path
               CONCAT_WS(' / ',
                   NULLIF(pt.page_title, ''),
                   NULLIF(s.scope_l0, ''),
                   NULLIF(s.scope_l1, ''),
                   NULLIF(s.scope_l2, ''),
                   CASE WHEN t.role IN ('row_label', 'section_label') THEN NULL
                        ELSE (SELECT t2.text FROM TAGGED AS t2
                              WHERE t2.page_id = t.page_id
                                AND t2.row_cluster = t.row_cluster
                                AND t2.role = 'row_label'
                              LIMIT 1)
                   END,
                   NULLIF(t.col_header, '')
               ) AS treemap_path
        FROM TAGGED AS t
        LEFT JOIN PAGE_TITLES AS pt USING (page_id)
        LEFT JOIN SCOPED AS s USING (row_cluster)
    )
    SELECT * FROM FINAL
    """)

    # ── Report ────────────────────────────────────────────────────────
    df = con.execute("SELECT * FROM classified ORDER BY page_id, row_cluster, x").df()

    total = len(df)
    if total == 0:
        print(f"  {name:40s}  (no bboxes)")
        return

    role_counts = df.groupby('role').size().sort_values(ascending=False)
    resolved = total - role_counts.get('prose', 0) - role_counts.get('dimension', 0)

    n_tables = con.execute("SELECT COUNT(*) FROM table_regions").fetchone()[0]
    n_cols = con.execute("SELECT COUNT(*) FROM putative_columns").fetchone()[0]
    n_headers = len(df[df['role'] == 'col_header'])

    print(f"\n{'='*80}")
    print(f"  {name}")
    print(f"{'='*80}")

    # Role summary
    print(f"\n  {total} cells → {n_tables} tables, {n_cols} columns, {n_headers} headers detected")
    print(f"\n  {'Role':<20s} {'Count':>5s} {'Pct':>6s}")
    print(f"  {'─'*35}")
    for role, n in role_counts.items():
        print(f"  {role:<20s} {n:5d} {100*n/total:5.1f}%")

    # Show table structure
    tables = con.execute("""
        SELECT t.table_id, t.n_data_rows, t.modal_cols,
               LIST(bh.header_text) AS headers
        FROM table_regions AS t
        LEFT JOIN (
            SELECT DISTINCT ON (page_id, table_id, col_id)
                   page_id, table_id, col_id, text AS header_text
            FROM classified
            WHERE role = 'col_header'
            ORDER BY page_id, table_id, col_id, row_cluster DESC
        ) AS bh
          ON t.page_id = bh.page_id AND t.table_id = bh.table_id
        GROUP BY t.table_id, t.n_data_rows, t.modal_cols
    """).df()

    if len(tables) > 0:
        print(f"\n  Tables:")
        for _, t in tables.iterrows():
            headers = list(t['headers']) if t['headers'] is not None else []
            print(f"    Table {int(t['table_id'])}: {int(t['n_data_rows'])}r × {int(t['modal_cols'])}c  headers={headers[:6]}")

    # Show a sample of classified cells grouped by role
    for role in ['col_header', 'section_label', 'row_label']:
        subset = df[df['role'] == role]
        if len(subset) > 0:
            texts = subset['text'].head(8).tolist()
            print(f"\n  {role}: {texts}")

    # Treemap-addressed facts
    measures = df[df['role'].isin(['measure', 'currency', 'percentage'])]
    if len(measures) > 0:
        print(f"\n  Treemap-addressed facts:")
        shown = set()
        for _, m in measures.iterrows():
            path = m.get('treemap_path', '') or ''
            if not path or path in shown:
                continue
            shown.add(path)
            print(f"    {path} = {m['text']}")
            if len(shown) >= 12:
                break


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")

    files = [
        "test_data/synthetic/financial_statement.pdf",
        "test_data/synthetic/01_sales_simple.pdf",
        "test_data/synthetic/04_multi_table.pdf",
        "test_data/synthetic/two_column.pdf",
        "test_data/sample.pdf",
    ]

    # Add real-world PDFs from Downloads if they exist
    downloads = [
        "/Users/paulharrington/Downloads/wealthfront_1099_2025.pdf",
        "/Users/paulharrington/Downloads/EPD document_EPD-IES-0021562_002_en.pdf",
        "/Users/paulharrington/Downloads/estimate.pdf",
    ]

    for f in files + downloads:
        if Path(f).exists():
            try:
                run_pipeline(f, con)
            except Exception as e:
                print(f"\n  {Path(f).name}: ERROR: {e}")
                import traceback
                traceback.print_exc()

    con.close()


if __name__ == "__main__":
    main()
