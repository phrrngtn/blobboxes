"""Layout detection from bbox coordinate histograms."""

import duckdb
from pathlib import Path

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"


def analyze(filepath: str):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    fp = filepath  # for SQL substitution

    name = Path(filepath).name
    print(f"\n{'='*70}")
    print(f"  {name}")
    print(f"{'='*70}")

    # Basic stats
    stats = con.execute(f"""
        SELECT b.page_id,
               COUNT(*) AS n_bboxes,
               p.width, p.height
        FROM bb('{fp}') AS b
        JOIN bb_pages('{fp}') AS p USING (page_id)
        WHERE b.page_id < 3
        GROUP BY b.page_id, p.width, p.height
        ORDER BY b.page_id
    """).df()

    for _, pg in stats.iterrows():
        pid = int(pg['page_id'])
        pw = pg['width']

        print(f"\n  Page {pid}: {int(pg['n_bboxes'])} bboxes, "
              f"{pg['width']:.0f} x {pg['height']:.0f} pt")

        # X-histogram: find text-segment start positions.
        # Within each y-row, detect gaps that split left/right columns:
        # sort bboxes by x, look for gaps > 10% of page width.
        # Each segment's leftmost x is a "line start" — histogram those.
        # In a two-column layout this produces two peaks at the column margins.
        bin_w = pw * 0.05
        gap_thresh = pw * 0.10
        xhist = con.execute(f"""
            WITH SORTED AS (
                SELECT x, y, w,
                       ROUND(y, 0) AS y_r,
                       x - LAG(x + w) OVER (PARTITION BY ROUND(y, 0) ORDER BY x) AS gap
                FROM bb('{fp}')
                WHERE page_id = {pid}
            ),
            SEGMENTS AS (
                SELECT y_r, x,
                       SUM(CASE WHEN gap IS NULL OR gap > {gap_thresh} THEN 1 ELSE 0 END)
                           OVER (PARTITION BY y_r ORDER BY x) AS seg_id
                FROM SORTED
            ),
            SEG_STARTS AS (
                SELECT y_r, seg_id, MIN(x) AS seg_x
                FROM SEGMENTS
                GROUP BY y_r, seg_id
            )
            SELECT FLOOR(seg_x / {bin_w}) AS x_bin,
                   ROUND(FLOOR(seg_x / {bin_w}) * {bin_w}, 1) AS x_left,
                   COUNT(*) AS n
            FROM SEG_STARTS
            GROUP BY x_bin, x_left
            ORDER BY x_bin
        """).df()

        max_n = xhist['n'].max()
        print(f"\n    X-histogram (left edges, bin={bin_w:.0f}pt):")
        for _, r in xhist.iterrows():
            bar_len = int(r['n'] * 40 / max_n)
            bar = '█' * bar_len
            marker = ''
            if r['n'] > xhist['n'].median() * 2 and r['n'] >= 3:
                marker = ' ← PEAK'
            print(f"      x={r['x_left']:6.1f}  n={int(r['n']):4d}  {bar}{marker}")

        # Detect peaks: bins with count > 2× median and >= 5
        median_n = xhist['n'].median()
        peaks = xhist[
            (xhist['n'] > max(median_n * 2, 3))
        ].sort_values('n', ascending=False)

        if len(peaks) >= 4:
            layout = "table/grid"
        elif len(peaks) == 3:
            layout = "three-column"
        elif len(peaks) == 2:
            # Check if peaks are roughly half-page apart
            peak_xs = sorted(peaks['x_left'].tolist())
            gap = peak_xs[1] - peak_xs[0]
            if gap > pw * 0.3:
                layout = "two-column"
            else:
                layout = "offset (two clusters, close together)"
        elif len(peaks) == 1:
            layout = "single-column"
        else:
            layout = "uniform/mixed"

        print(f"\n    → Layout: {layout} ({len(peaks)} peaks)")

        # H-histogram: font size distribution
        hhist = con.execute(f"""
            SELECT ROUND(h, 0) AS h_bin, COUNT(*) AS n
            FROM bb('{fp}')
            WHERE page_id = {pid}
            GROUP BY h_bin
            HAVING COUNT(*) >= 2
            ORDER BY h_bin
        """).df()

        if len(hhist) > 0:
            hmax = hhist['n'].max()
            print(f"\n    H-histogram (bbox heights = font size proxy):")
            for _, r in hhist.iterrows():
                bar_len = int(r['n'] * 30 / hmax)
                print(f"      h={r['h_bin']:5.0f}  n={int(r['n']):4d}  {'█' * bar_len}")

        # Y-regularity: how uniform is row spacing?
        yreg = con.execute(f"""
            WITH Y_VALS AS (
                SELECT DISTINCT ROUND(y, 0) AS y_r
                FROM bb('{fp}') WHERE page_id = {pid}
                ORDER BY y_r
            ),
            GAPS AS (
                SELECT y_r - LAG(y_r) OVER (ORDER BY y_r) AS gap
                FROM Y_VALS
            )
            SELECT ROUND(AVG(gap), 1) AS mean_gap,
                   ROUND(STDDEV(gap), 1) AS std_gap,
                   ROUND(CASE WHEN AVG(gap) > 0 THEN STDDEV(gap)/AVG(gap) ELSE 0 END, 2) AS cv
            FROM GAPS
            WHERE gap > 0
        """).df()

        if len(yreg) > 0:
            r = yreg.iloc[0]
            regularity = "regular" if r['cv'] < 0.3 else "irregular" if r['cv'] > 0.7 else "moderate"
            print(f"\n    Y-regularity: CV={r['cv']:.2f} ({regularity}), "
                  f"mean row gap={r['mean_gap']:.1f}pt")

    con.close()


def main():
    files = [
        "/Users/paulharrington/checkouts/blobboxes/test_data/synthetic/two_column.pdf",
        "/Users/paulharrington/checkouts/blobboxes/test_data/synthetic/two_col_paper.pdf",
        "/Users/paulharrington/checkouts/blobboxes/test_data/synthetic/01_sales_simple.pdf",
        "/Users/paulharrington/checkouts/blobboxes/test_data/synthetic/06_sales_simple.xlsx",
        "/Users/paulharrington/checkouts/blobboxes/test_data/sample.pdf",
    ]
    for f in files:
        if Path(f).exists():
            try:
                analyze(f)
            except Exception as e:
                print(f"  ERROR: {e}")
                import traceback
                traceback.print_exc()


if __name__ == "__main__":
    main()
