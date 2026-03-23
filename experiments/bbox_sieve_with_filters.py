"""Bbox sieve with blobfilter domain probing.

Full pipeline: bboxes → merge → type detection → blobfilter probing.
Runs entirely in DuckDB SQL, loading bboxes + blobfilters extensions
and connecting to PG for domain filters.

The key question: what fraction of "text" cells (those that survived
TRY_CAST and regex probing) can be resolved by blobfilter membership
testing, without embeddings?
"""

import duckdb
import time
from pathlib import Path

BBOXES_EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
BLOBFILTERS_EXT = "/Users/paulharrington/checkouts/blobfilters/build/duckdb/blobfilters.duckdb_extension"
SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")


def run_sieve(filepath: str):
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BBOXES_EXT}'")
    con.execute(f"LOAD '{BLOBFILTERS_EXT}'")

    # Connect to PG and stage domain filters locally
    con.execute("INSTALL postgres; LOAD postgres;")
    con.execute("ATTACH 'host=/tmp dbname=rule4_test' AS pg (TYPE POSTGRES)")

    # Load pre-built normalized filters from PG (built by build_normalized_filters.py)
    t0 = time.perf_counter()
    con.execute("""
        CREATE TEMPORARY TABLE domain_filters AS
        SELECT domain_name,
               bf_from_base64(filter_b64) AS filter_bitmap,
               member_count AS cardinality
        FROM pg.domain.enumeration
        WHERE filter_b64 IS NOT NULL
    """)
    t_filters = time.perf_counter() - t0

    n_domains = con.execute("SELECT COUNT(*) FROM domain_filters").fetchone()[0]
    total_members = con.execute("SELECT SUM(cardinality) FROM domain_filters").fetchone()[0]

    fp = filepath
    name = Path(filepath).name
    is_xlsx = fp.endswith(".xlsx")

    print(f"\n{'='*70}")
    print(f"  {name}")
    print(f"  {n_domains} domains, {total_members} members loaded in {t_filters:.2f}s")
    print(f"{'='*70}")

    # The full sieve as one query
    t0 = time.perf_counter()
    results = con.execute(f"""
    WITH
    -- Layer 0: raw bboxes with styles
    RAW AS (
        SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
               s.font_size, s.weight, s.color
        FROM bb('{fp}') AS b
        JOIN bb_styles('{fp}') AS s USING (style_id)
    ),

    -- Layer 1: row clustering
    ROWS AS (
        SELECT *,
               DENSE_RANK() OVER (ORDER BY round((y + h/2) / GREATEST(h, 1.0))) AS row_cluster
        FROM RAW
    ),

    -- Layer 2: cell merging (grid mode for XLSX, gap-based for PDF)
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
               MODE(font_size) AS font_size
        FROM CELLS
        GROUP BY page_id, row_cluster, cell_id
    ),

    -- Layer 3: header/body split
    ROW_STATS AS (
        SELECT row_cluster, COUNT(*) AS n_cells,
               SUM(CASE WHEN TRY_CAST(REPLACE(REPLACE(text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL
                        THEN 1 ELSE 0 END) AS n_numeric
        FROM MERGED GROUP BY row_cluster
    ),
    BODY_START AS (
        SELECT COALESCE(MIN(row_cluster), 1) AS first_data_row
        FROM ROW_STATS WHERE n_numeric > 0 AND n_cells >= 2
    ),

    -- Layer 4: type detection + regex
    TYPED AS (
        SELECT m.*,
               m.row_cluster < bs.first_data_row AS is_pre_data,
               m.weight = 'bold' AS is_bold,
               TRY_CAST(REPLACE(REPLACE(m.text, ',', ''), '$', '') AS DOUBLE) IS NOT NULL AS is_numeric,
               TRY_CAST(m.text AS DATE) IS NOT NULL AS is_date,
               CASE
                   WHEN regexp_matches(m.text, '^\$[\d,.]+$') THEN 'currency'
                   WHEN regexp_matches(m.text, '^[\d,.]+\s*%$') THEN 'percentage'
                   WHEN regexp_matches(m.text, '^\d{{5}}(-\d{{4}})?$') THEN 'zip_code'
                   WHEN regexp_matches(m.text, '^\d{{4}}-\d{{2}}-\d{{2}}') THEN 'iso_date'
                   WHEN regexp_matches(m.text, '^[\d,.]+$') THEN 'number'
                   ELSE NULL
               END AS regex_domain
        FROM MERGED AS m, BODY_START AS bs
    ),

    -- Layer 5: blobfilter probing on unresolved text cells
    -- For each cell that isn't already resolved, probe against all domain filters.
    -- bf_containment_json returns the fraction of probe values found in the filter.
    -- We probe with a single-element JSON array: ["cell_text"]
    -- A containment of 1.0 means exact match.
    UNRESOLVED AS (
        SELECT * FROM TYPED
        WHERE NOT is_pre_data
          AND NOT is_numeric
          AND NOT is_date
          AND regex_domain IS NULL
          AND LENGTH(text) > 3  -- skip tiny codes
    ),
    FILTER_PROBES AS (
        SELECT u.page_id, u.row_cluster, u.cell_id, u.text,
               df.domain_name,
               bf_containment_json_normalized(
                   u.text,
                   df.filter_bitmap
               ) AS containment
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

    -- Layer 6: final classification
    CLASSIFIED AS (
        SELECT t.page_id, t.row_cluster, t.cell_id,
               t.x, t.y, t.w, t.h, t.text,
               t.is_pre_data, t.is_bold, t.is_numeric, t.is_date,
               t.regex_domain,
               bm.matched_domain,
               bm.best_containment,
               CASE
                   WHEN t.is_pre_data THEN 'header'
                   WHEN t.regex_domain IS NOT NULL THEN t.regex_domain
                   WHEN t.is_numeric AND NOT t.is_date THEN 'numeric'
                   WHEN t.is_date THEN 'date'
                   WHEN bm.best_containment = 1.0 THEN 'domain:' || bm.matched_domain
                   WHEN t.is_bold AND NOT t.is_numeric THEN 'label'
                   WHEN LENGTH(t.text) <= 3 THEN 'code'
                   ELSE 'text'
               END AS role
        FROM TYPED AS t
        LEFT JOIN BEST_MATCH AS bm
            ON t.page_id = bm.page_id
           AND t.row_cluster = bm.row_cluster
           AND t.cell_id = bm.cell_id
    )

    SELECT role,
           COUNT(*) AS n,
           ROUND(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 1) AS pct
    FROM CLASSIFIED
    GROUP BY role
    ORDER BY n DESC
    """).df()

    t_sieve = time.perf_counter() - t0

    print(f"\n  Sieve completed in {t_sieve:.2f}s\n")
    print(f"  {'Role':<25} {'Count':>5} {'Pct':>6}")
    print(f"  {'-'*40}")
    for _, r in results.iterrows():
        bar = '█' * int(r['pct'] / 2)
        print(f"  {r['role']:<25} {int(r['n']):>5} {r['pct']:>5.1f}% {bar}")

    # Key metric
    resolved = results[results['role'] != 'text']['n'].sum()
    total = results['n'].sum()
    print(f"\n  Resolved: {resolved}/{total} ({100*resolved/total:.0f}%)")
    print(f"  Need embedding: {total - resolved}/{total} ({100*(total-resolved)/total:.0f}%)")

    # Show the domain matches
    matches = con.execute(f"""
    WITH
    RAW AS (
        SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
               s.weight
        FROM bb('{fp}') AS b
        JOIN bb_styles('{fp}') AS s USING (style_id)
    ),
    ROWS AS (
        SELECT *, DENSE_RANK() OVER (ORDER BY round((y + h/2) / GREATEST(h, 1.0))) AS row_cluster
        FROM RAW
    ),
    IS_GRID AS (
        SELECT CASE WHEN COUNT(DISTINCT w) = 1 AND MIN(w) = 1.0
                    THEN true ELSE false END AS grid_mode
        FROM ROWS
    ),
    CELL_GAPS AS (
        SELECT ROWS.*, x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
               LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h, grid_mode
        FROM ROWS, IS_GRID
    ),
    CELLS AS (
        SELECT *, CASE WHEN grid_mode
                       THEN ROW_NUMBER() OVER (PARTITION BY page_id, row_cluster ORDER BY x)
                       ELSE SUM(CASE WHEN gap IS NULL OR gap > GREATEST(prev_h * 0.8, 3.0)
                                     THEN 1 ELSE 0 END)
                                OVER (PARTITION BY page_id, row_cluster ORDER BY x)
                  END AS cell_id
        FROM CELL_GAPS
    ),
    MERGED AS (
        SELECT page_id, row_cluster, cell_id,
               STRING_AGG(text, ' ' ORDER BY x) AS text
        FROM CELLS GROUP BY page_id, row_cluster, cell_id
    ),
    PROBES AS (
        SELECT m.text, df.domain_name,
               bf_containment_json_normalized(json_array(UPPER(m.text)), df.filter_bitmap) AS containment
        FROM MERGED AS m
        CROSS JOIN domain_filters AS df
        WHERE LENGTH(m.text) > 3
    )
    SELECT text, domain_name, ROUND(containment, 2) AS score
    FROM PROBES
    WHERE containment = 1.0
    ORDER BY domain_name, text
    LIMIT 30
    """).df()

    if len(matches) > 0:
        print(f"\n  Domain matches (exact, top 30):")
        for domain in matches['domain_name'].unique():
            dm = matches[matches['domain_name'] == domain]
            vals = dm['text'].tolist()
            print(f"    {domain}: {', '.join(vals[:8])}"
                  + (f" ... (+{len(vals)-8})" if len(vals) > 8 else ""))

    con.close()


def main():
    files = [
        str(SYNTH / "05_countries_footnotes.pdf"),
        str(SYNTH / "06_sales_simple.xlsx"),
        str(SYNTH / "census_pop.xlsx"),
        str(SYNTH / "pmc_clinical.pdf"),
        str(SYNTH / "pmc_jmir.pdf"),
        str(SYNTH / "pmc_crips.pdf"),
        str(SYNTH / "two_col_paper.pdf"),
    ]
    for f in files:
        if Path(f).exists():
            try:
                run_sieve(f)
            except Exception as e:
                print(f"\n  ERROR on {Path(f).name}: {e}")
                import traceback
                traceback.print_exc()


if __name__ == "__main__":
    main()
