"""Pipeline: detect tables → sieve table cells → LLM classify remainder.

1. Geometry-based table detection (segment rows by cell-count consistency)
2. Sieve on table cells only (TRY_CAST, regex, blobfilters)
3. Extract unresolved table-cell tokens → ask LLM what domains they are
"""
import json
import time
import duckdb
from pathlib import Path

BBOXES_EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
BLOBFILTERS_EXT = "/Users/paulharrington/checkouts/blobfilters/build/duckdb/blobfilters.duckdb_extension"
SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")

# The full pipeline as one SQL query
PIPELINE_SQL = """
WITH
-- ═══ Stage 1: Extract + merge bboxes into cells ═══════════════════
RAW AS (
    SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
           s.font_size, s.weight, s.color
    FROM bb('{file}') AS b
    JOIN bb_styles('{file}') AS s USING (style_id)
),
ROWS AS (
    SELECT *,
           DENSE_RANK() OVER (ORDER BY page_id, round((y + h/2) / GREATEST(h, 1.0))) AS row_cluster
    FROM RAW
),
IS_GRID AS (
    SELECT CASE WHEN COUNT(DISTINCT w) = 1 AND MIN(w) = 1.0
                THEN true ELSE false END AS grid_mode
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
           CASE WHEN grid_mode
                THEN ROW_NUMBER() OVER (PARTITION BY page_id, row_cluster ORDER BY x)
                ELSE SUM(CASE WHEN gap IS NULL OR gap > GREATEST(prev_h * 0.8, 3.0)
                              THEN 1 ELSE 0 END)
                         OVER (PARTITION BY page_id, row_cluster ORDER BY x)
           END AS cell_id
    FROM CELL_GAPS
),
MERGED AS (
    SELECT page_id, row_cluster, cell_id,
           MIN(x) AS x, MIN(y) AS y,
           MAX(x + w) - MIN(x) AS w, MAX(h) AS h,
           STRING_AGG(text, ' ' ORDER BY x) AS text,
           MODE(weight) AS weight,
           MODE(font_size) AS font_size,
           COUNT(*) AS n_fragments
    FROM CELLS
    GROUP BY page_id, row_cluster, cell_id
),

-- ═══ Stage 2: Table detection via cell-count consistency ═══════════
ROW_CELL_COUNTS AS (
    SELECT row_cluster, COUNT(*) AS n_cells
    FROM MERGED
    GROUP BY row_cluster
),
-- Find runs of rows with consistent cell counts (table regions)
-- A row belongs to a table if it has >= 2 cells and its count is
-- within ±1 of the previous table row's count
TABLE_ROWS AS (
    SELECT row_cluster, n_cells,
           SUM(CASE WHEN n_cells >= 2 AND
                         (LAG(n_cells) OVER (ORDER BY row_cluster) IS NULL
                          OR ABS(n_cells - LAG(n_cells) OVER (ORDER BY row_cluster)) <= 1
                          OR LAG(n_cells) OVER (ORDER BY row_cluster) < 2)
                    THEN 0 ELSE 1 END)
               OVER (ORDER BY row_cluster) AS table_break
    FROM ROW_CELL_COUNTS
),
TABLE_SEGMENTS AS (
    SELECT row_cluster, n_cells,
           SUM(CASE WHEN n_cells < 2 THEN 1 ELSE 0 END)
               OVER (ORDER BY row_cluster) AS seg_id
    FROM ROW_CELL_COUNTS
),
TABLE_REGIONS AS (
    SELECT seg_id,
           MIN(row_cluster) AS start_row,
           MAX(row_cluster) AS end_row,
           COUNT(*) AS n_rows,
           MODE(n_cells) AS modal_cells
    FROM TABLE_SEGMENTS
    WHERE n_cells >= 2
    GROUP BY seg_id
    HAVING COUNT(*) >= 3  -- at least 3 rows to be a table
),

-- ═══ Stage 3: Classify cells in table regions only ════════════════
TABLE_CELLS AS (
    SELECT m.*
    FROM MERGED AS m
    JOIN TABLE_REGIONS AS tr
      ON m.row_cluster BETWEEN tr.start_row AND tr.end_row
),

-- Header detection: first all-text row in each table region
ROW_TYPE_STATS AS (
    SELECT tc.row_cluster, COUNT(*) AS n_cells,
           SUM(CASE WHEN TRY_CAST(REPLACE(REPLACE(tc.text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL
                    THEN 1 ELSE 0 END) AS n_numeric
    FROM TABLE_CELLS AS tc
    GROUP BY tc.row_cluster
),
REGION_HEADERS AS (
    SELECT tr.seg_id, tr.start_row,
           MIN(CASE WHEN rts.n_numeric = 0 AND rts.n_cells >= 2
                    THEN rts.row_cluster END) AS header_row
    FROM TABLE_REGIONS AS tr
    JOIN ROW_TYPE_STATS AS rts
      ON rts.row_cluster BETWEEN tr.start_row AND tr.end_row
    GROUP BY tr.seg_id, tr.start_row
),

-- ═══ Stage 4: Sieve on table cells ═══════════════════════════════
TYPED AS (
    SELECT tc.*,
           tc.row_cluster <= COALESCE(rh.header_row, tc.row_cluster - 1) AS is_header,
           tc.weight = 'bold' AND tc.row_cluster = rh.header_row AS is_header_bold,
           TRY_CAST(REPLACE(REPLACE(tc.text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL AS is_numeric,
           TRY_CAST(tc.text AS DATE) IS NOT NULL AS is_date,
           CASE
               WHEN regexp_matches(tc.text, '^\$[\d,.]+$') THEN 'currency'
               WHEN regexp_matches(tc.text, '^[\d,.]+\s*%$') THEN 'percentage'
               WHEN regexp_matches(tc.text, '^\d{5}(-\d{4})?$') THEN 'zip_code'
               WHEN regexp_matches(tc.text, '^\d{4}-\d{2}-\d{2}') THEN 'iso_date'
               WHEN regexp_matches(tc.text, '^[\d,.]+$') THEN 'number'
               ELSE NULL
           END AS regex_domain
    FROM TABLE_CELLS AS tc
    LEFT JOIN TABLE_REGIONS AS tr
      ON tc.row_cluster BETWEEN tr.start_row AND tr.end_row
    LEFT JOIN REGION_HEADERS AS rh ON tr.seg_id = rh.seg_id
),

-- ═══ Stage 5: Blobfilter probing on unresolved body cells ═════════
UNRESOLVED AS (
    SELECT * FROM TYPED
    WHERE NOT is_header
      AND NOT is_numeric
      AND NOT is_date
      AND regex_domain IS NULL
      AND LENGTH(text) > 3
),
FILTER_PROBES AS (
    SELECT u.page_id, u.row_cluster, u.cell_id, u.text,
           df.domain_name,
           bf_containment_json_normalized(u.text, df.filter_bitmap) AS containment
    FROM UNRESOLVED AS u
    CROSS JOIN domain_filters AS df
),
BEST_MATCH AS (
    SELECT page_id, row_cluster, cell_id, text,
           FIRST(domain_name ORDER BY containment DESC) AS matched_domain,
           MAX(containment) AS best_containment
    FROM FILTER_PROBES
    WHERE containment > 0
    GROUP BY page_id, row_cluster, cell_id, text
),

-- ═══ Stage 6: Final classification ════════════════════════════════
CLASSIFIED AS (
    SELECT t.page_id, t.row_cluster, t.cell_id,
           t.x, t.y, t.w, t.h, t.text,
           t.is_header, t.is_numeric, t.is_date, t.regex_domain,
           bm.matched_domain, bm.best_containment,
           CASE
               WHEN t.is_header THEN 'header'
               WHEN t.regex_domain IS NOT NULL THEN t.regex_domain
               WHEN t.is_numeric AND NOT t.is_date THEN 'numeric'
               WHEN t.is_date THEN 'date'
               WHEN bm.best_containment = 1.0 THEN 'domain:' || bm.matched_domain
               WHEN t.is_header_bold THEN 'label'
               WHEN LENGTH(t.text) <= 3 THEN 'code'
               ELSE 'text'
           END AS role
    FROM TYPED AS t
    LEFT JOIN BEST_MATCH AS bm
        ON t.page_id = bm.page_id
       AND t.row_cluster = bm.row_cluster
       AND t.cell_id = bm.cell_id
)

SELECT * FROM CLASSIFIED
"""


def run_pipeline(filepath: str):
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BBOXES_EXT}'")
    con.execute(f"LOAD '{BLOBFILTERS_EXT}'")
    con.execute("INSTALL postgres; LOAD postgres;")
    con.execute("ATTACH 'host=/tmp dbname=rule4_test' AS pg (TYPE POSTGRES)")

    # Load pre-built normalized filters
    con.execute("""
        CREATE TEMPORARY TABLE domain_filters AS
        SELECT domain_name,
               bf_from_base64(filter_b64) AS filter_bitmap,
               member_count AS cardinality
        FROM pg.domain.enumeration
        WHERE filter_b64 IS NOT NULL
    """)

    name = Path(filepath).name
    sql = PIPELINE_SQL.replace('{file}', filepath)

    print(f"\n{'='*70}")
    print(f"  {name}")
    print(f"{'='*70}")

    t0 = time.perf_counter()
    df = con.execute(sql).df()
    elapsed = time.perf_counter() - t0

    if len(df) == 0:
        print(f"  No table cells detected ({elapsed:.2f}s)")
        con.close()
        return None

    # Summary
    summary = df.groupby("role").size().reset_index(name="n")
    summary["pct"] = (100.0 * summary["n"] / summary["n"].sum()).round(1)
    summary = summary.sort_values("n", ascending=False)

    total = summary["n"].sum()
    resolved = summary[summary["role"] != "text"]["n"].sum()

    print(f"\n  Table cells: {total} (pipeline: {elapsed:.2f}s)")
    print(f"  Resolved: {resolved}/{total} ({100*resolved/total:.0f}%)")
    print(f"  Unresolved: {total - resolved}/{total} ({100*(total-resolved)/total:.0f}%)")

    print(f"\n  {'Role':<30} {'Count':>5} {'Pct':>6}")
    print(f"  {'-'*45}")
    for _, r in summary.iterrows():
        print(f"  {r['role']:<30} {int(r['n']):>5} {r['pct']:>5.1f}%")

    # Extract unresolved text cells for LLM
    unresolved = df[df["role"] == "text"]["text"].unique()
    # Filter out very common English words and fragments
    unresolved = [t for t in unresolved
                  if len(t) > 2
                  and not t.startswith("(")
                  and not t.startswith("[")]

    print(f"\n  Unique unresolved tokens: {len(unresolved)}")
    if unresolved:
        sample = sorted(set(unresolved))[:50]
        print(f"  Sample: {', '.join(sample[:20])}")

    con.close()
    return unresolved


def main():
    files = [
        str(SYNTH / "05_countries_footnotes.pdf"),
        str(SYNTH / "pmc_clinical.pdf"),
        str(SYNTH / "pmc_crips.pdf"),
        str(SYNTH / "two_col_paper.pdf"),
        str(SYNTH / "census_pop.xlsx"),
    ]

    all_unresolved = {}
    for f in files:
        if Path(f).exists():
            try:
                tokens = run_pipeline(f)
                if tokens:
                    all_unresolved[Path(f).name] = tokens
            except Exception as e:
                print(f"  ERROR: {e}")
                import traceback
                traceback.print_exc()

    # Aggregate all unresolved tokens across documents
    all_tokens = set()
    for tokens in all_unresolved.values():
        all_tokens.update(tokens)

    print(f"\n{'='*70}")
    print(f"  AGGREGATE: {len(all_tokens)} unique unresolved tokens across all docs")
    print(f"{'='*70}")

    # Save for LLM classification
    token_list = sorted(all_tokens)
    out_path = SYNTH / "unresolved_tokens.json"
    with open(out_path, "w") as fh:
        json.dump(token_list, fh, indent=2)
    print(f"  Saved to {out_path}")
    print(f"  First 30: {token_list[:30]}")


if __name__ == "__main__":
    main()
