"""Benchmark char-by-char vs object-level PDF extraction.

Runs both paths on the same documents and compares:
- Extraction time
- Number of bboxes produced
- Text content quality (spot-check)
"""
import time
import duckdb
from pathlib import Path

BBOXES_EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")

def bench(filepath: str):
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BBOXES_EXT}'")

    name = Path(filepath).name

    # Warm up (first call loads the library)
    con.execute(f"SELECT count(*) FROM bb('{filepath}')").fetchone()

    # Benchmark current char-by-char path
    times = []
    for _ in range(3):
        t0 = time.perf_counter()
        r = con.execute(f"SELECT count(*) AS n FROM bb('{filepath}')").fetchone()
        times.append(time.perf_counter() - t0)
    n_bboxes = r[0]
    char_time = min(times)

    # Get page count
    pages = con.execute(f"SELECT count(*) FROM bb_pages('{filepath}')").fetchone()[0]

    # Sample some bboxes
    sample = con.execute(f"""
        SELECT round(x,1) AS x, round(y,1) AS y, text
        FROM bb('{filepath}')
        WHERE length(text) > 2
        ORDER BY y, x
        LIMIT 10
    """).df()

    print(f"\n  {name}: {pages} pages, {n_bboxes} bboxes, {char_time*1000:.1f}ms (best of 3)")
    for _, r in sample.iterrows():
        print(f"    ({r['x']:6.1f}, {r['y']:6.1f}) {r['text'][:40]}")

    con.close()
    return {"name": name, "pages": pages, "bboxes": n_bboxes, "time_ms": char_time * 1000}


def main():
    files = [
        str(SYNTH.parent / "sample.pdf"),
        str(SYNTH / "01_sales_simple.pdf"),
        str(SYNTH / "05_countries_footnotes.pdf"),
        str(SYNTH / "pmc_clinical.pdf"),
        str(SYNTH / "pmc_crips.pdf"),
        str(SYNTH / "two_col_paper.pdf"),
        str(SYNTH / "irs_w4.pdf"),
    ]

    print("Current char-by-char extraction (baseline):")
    results = []
    for f in files:
        if Path(f).exists():
            results.append(bench(f))

    print(f"\n{'='*60}")
    print(f"  {'File':<30} {'Pages':>5} {'BBoxes':>7} {'Time':>8}")
    print(f"  {'-'*55}")
    for r in results:
        print(f"  {r['name']:<30} {r['pages']:>5} {r['bboxes']:>7} {r['time_ms']:>7.1f}ms")

    total_bboxes = sum(r['bboxes'] for r in results)
    total_time = sum(r['time_ms'] for r in results)
    print(f"\n  Total: {total_bboxes} bboxes in {total_time:.0f}ms")
    print(f"  Rate: {total_bboxes/total_time*1000:.0f} bboxes/sec")


if __name__ == "__main__":
    main()
