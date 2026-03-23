"""End-to-end benchmark: extraction + merging to usable cells.

Path A: char-by-char extraction (fast) + SQL cell merging (extra work)
Path B: object-level extraction (slower) + no merging needed

Both should produce comparable output: clean text cells with coordinates.
"""
import time
import duckdb
from pathlib import Path

BBOXES_EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"

MERGE_SQL = """
WITH ROWS AS (
    SELECT *,
           DENSE_RANK() OVER (ORDER BY page_id, round((y + h/2) / GREATEST(h, 1.0))) AS row_cluster
    FROM bb('{file}')
),
CELL_GAPS AS (
    SELECT *,
           x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
           LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h
    FROM ROWS
),
CELLS AS (
    SELECT *,
           SUM(CASE WHEN gap IS NULL OR gap > GREATEST(prev_h * 0.8, 3.0)
                    THEN 1 ELSE 0 END)
               OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS cell_id
    FROM CELL_GAPS
)
SELECT page_id, row_cluster, cell_id,
       MIN(x) AS x, MIN(y) AS y,
       MAX(x + w) - MIN(x) AS w, MAX(h) AS h,
       STRING_AGG(text, ' ' ORDER BY x) AS text
FROM CELLS
GROUP BY page_id, row_cluster, cell_id
"""


def bench(filepath: str):
    name = Path(filepath).name

    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BBOXES_EXT}'")

    # Warm up
    con.execute(f"SELECT count(*) FROM bb('{filepath}')").fetchone()

    # Path A: extraction only (raw bboxes)
    times_raw = []
    for _ in range(3):
        t0 = time.perf_counter()
        r = con.execute(f"SELECT count(*) FROM bb('{filepath}')").fetchone()
        times_raw.append(time.perf_counter() - t0)
    n_raw = r[0]
    t_raw = min(times_raw)

    # Path A: extraction + SQL merge (end-to-end to usable cells)
    sql = MERGE_SQL.replace('{file}', filepath)
    times_merged = []
    for _ in range(3):
        t0 = time.perf_counter()
        r = con.execute(f"SELECT count(*) FROM ({sql})").fetchone()
        times_merged.append(time.perf_counter() - t0)
    n_merged = r[0]
    t_merged = min(times_merged)

    # Sample merged output
    sample = con.execute(f"""
        SELECT round(x,1) AS x, round(y,1) AS y, text
        FROM ({sql})
        WHERE length(text) > 2
        ORDER BY y, x LIMIT 5
    """).df()

    con.close()

    print(f"\n  {name}:")
    print(f"    Raw bboxes:    {n_raw:>6}  in {t_raw*1000:>7.1f}ms")
    print(f"    After merge:   {n_merged:>6}  in {t_merged*1000:>7.1f}ms  (extract + SQL merge)")
    print(f"    Merge overhead:         {(t_merged - t_raw)*1000:>7.1f}ms")
    for _, r in sample.iterrows():
        print(f"      ({r['x']:6.1f}, {r['y']:6.1f}) {r['text'][:50]}")

    return {
        "name": name, "n_raw": n_raw, "n_merged": n_merged,
        "t_raw_ms": t_raw * 1000, "t_merged_ms": t_merged * 1000,
    }


def main():
    SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")
    files = [
        str(SYNTH.parent / "sample.pdf"),
        str(SYNTH / "01_sales_simple.pdf"),
        str(SYNTH / "05_countries_footnotes.pdf"),
        str(SYNTH / "pmc_clinical.pdf"),
        str(SYNTH / "pmc_crips.pdf"),
        str(SYNTH / "two_col_paper.pdf"),
        str(SYNTH / "irs_w4.pdf"),
    ]

    print("Path A: char-by-char + SQL merge (current)")
    results = []
    for f in files:
        if Path(f).exists():
            results.append(bench(f))

    print(f"\n{'='*65}")
    print(f"  {'File':<28} {'Raw':>6} {'Merged':>6} {'Extract':>8} {'E2E':>8} {'Merge%':>6}")
    print(f"  {'-'*60}")
    for r in results:
        merge_pct = (r['t_merged_ms'] - r['t_raw_ms']) / r['t_merged_ms'] * 100
        print(f"  {r['name']:<28} {r['n_raw']:>6} {r['n_merged']:>6} "
              f"{r['t_raw_ms']:>7.1f}ms {r['t_merged_ms']:>7.1f}ms {merge_pct:>5.0f}%")


if __name__ == "__main__":
    main()
