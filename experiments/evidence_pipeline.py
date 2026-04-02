"""Evidence-accumulation pipeline for bbox classification.

Architecture:
- In-memory DuckDB database with persistent schema
- All tables carry doc_id (no FK constraint — just a grouping key)
- Evidence table accumulates observations from independent passes
- Each pass is a small, focused query that INSERTs into evidence
- Final classification queries evidence with arbitrary logic
- Multi-page from the start: cross-page signals (repeated text, etc.)
- Multi-doc: ingest several documents into the same database, compare

Tables:
  cells(doc_id, page_id, row_cluster, cell_id, x, y, w, h, text, ...)
    — merged bboxes, the working set

  evidence(doc_id, page_id, row_cluster, cell_id, source, key, value)
    — cell-level observations. source identifies the pass.

  row_evidence(doc_id, page_id, row_cluster, source, key, value)
    — row-level observations (aggregates across cells in a row)

  page_evidence(doc_id, page_id, source, key, value)
    — page-level observations

  doc_evidence(doc_id, source, key, value)
    — document-level observations
"""

import duckdb
from pathlib import Path

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"


def create_schema(con):
    """Create the evidence accumulation schema."""
    con.execute("""
    CREATE TABLE IF NOT EXISTS cells (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        x           DOUBLE,
        y           DOUBLE,
        w           DOUBLE,
        h           DOUBLE,
        text        VARCHAR,
        weight      VARCHAR,
        font_size   DOUBLE,
        font_name   VARCHAR,
        color       VARCHAR,
        style_id    INTEGER,
        style_rank  INTEGER,
        style_count INTEGER,
        n_fragments INTEGER,
        PRIMARY KEY (doc_id, page_id, row_cluster, cell_id)
    );

    CREATE TABLE IF NOT EXISTS evidence (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        source      VARCHAR NOT NULL,
        key         VARCHAR NOT NULL,
        value       VARCHAR
    );

    CREATE TABLE IF NOT EXISTS row_evidence (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        source      VARCHAR NOT NULL,
        key         VARCHAR NOT NULL,
        value       VARCHAR
    );

    CREATE TABLE IF NOT EXISTS page_evidence (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        source      VARCHAR NOT NULL,
        key         VARCHAR NOT NULL,
        value       VARCHAR
    );

    CREATE TABLE IF NOT EXISTS doc_evidence (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        source      VARCHAR NOT NULL,
        key         VARCHAR NOT NULL,
        value       VARCHAR
    );
    """)


def ingest(con, filepath, doc_id=1):
    """Extract bboxes, merge into cells, populate cells table."""
    fp = str(filepath)

    con.execute(f"""
    INSERT INTO cells
    WITH
    RAW AS (
        SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
               s.font_size, s.color, f.name AS font_name,
               CASE WHEN f.name ILIKE '%bold%' OR s.weight = 'bold'
                    THEN 'bold' ELSE 'normal' END AS eff_weight
        FROM bb('{fp}') AS b
        JOIN bb_styles('{fp}') AS s USING (style_id)
        JOIN bb_fonts('{fp}') AS f USING (font_id)
    ),

    STYLE_HIST AS (
        SELECT style_id,
               COUNT(*) AS n_bboxes,
               RANK() OVER (ORDER BY COUNT(*) DESC) AS style_rank
        FROM RAW
        GROUP BY style_id
    ),

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
    ROWS_INITIAL AS (
        SELECT * EXCLUDE (sort_ord, y_gap, prev_page),
               sort_ord, y_gap,
               SUM(CASE
                   WHEN y_gap IS NULL THEN 1
                   WHEN prev_page != page_id THEN 1
                   WHEN y_gap > (SELECT bh FROM BODY_H) THEN 1
                   ELSE 0
               END) OVER (ORDER BY sort_ord) AS row_cluster_init
        FROM Y_SORTED
    ),
    ROW_HEIGHTS AS (
        SELECT row_cluster_init,
               MAX(y) - MIN(y) AS row_y_range,
               MIN(y) AS row_min_y
        FROM ROWS_INITIAL GROUP BY row_cluster_init
    ),
    ROWS AS (
        SELECT r.* EXCLUDE (sort_ord, y_gap, row_cluster_init),
               CASE
                   WHEN rh.row_y_range <= (SELECT bh FROM BODY_H) * 2
                   THEN r.row_cluster_init
                   ELSE r.row_cluster_init * 1000
                        + FLOOR((r.y - rh.row_min_y) / (SELECT bh FROM BODY_H))
               END AS row_cluster
        FROM ROWS_INITIAL AS r
        JOIN ROW_HEIGHTS AS rh ON r.row_cluster_init = rh.row_cluster_init
    ),

    CELL_GAPS AS (
        SELECT ROWS.*,
               x - LAG(x + w) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS gap,
               LAG(h) OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS prev_h
        FROM ROWS
    ),
    GAP_THRESHOLD AS (
        SELECT GREATEST(
                   COALESCE(
                       PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY gap)
                           FILTER (WHERE gap > 0.5
                                   AND gap < (SELECT bh FROM BODY_H) * 3),
                       (SELECT bh FROM BODY_H)
                   ) * 2.0,
                   (SELECT bh FROM BODY_H)
               ) AS cell_gap_thresh
        FROM CELL_GAPS
    ),
    CELL_IDS AS (
        SELECT *,
               SUM(CASE WHEN gap IS NULL
                        OR gap > (SELECT cell_gap_thresh FROM GAP_THRESHOLD)
                        THEN 1 ELSE 0 END)
                   OVER (PARTITION BY page_id, row_cluster ORDER BY x) AS cell_id
        FROM CELL_GAPS
    ),
    FRAG_TAGS AS (
        SELECT CELL_IDS.*,
               regexp_matches(text, '^[,.:;]+$') AS is_punct,
               COALESCE(regexp_matches(
                   LAG(text) OVER (PARTITION BY page_id, row_cluster,
                                   cell_id ORDER BY x),
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
               MODE(eff_weight) AS weight,
               MODE(font_size) AS font_size,
               MODE(font_name) AS font_name,
               MODE(color) AS color,
               MODE(style_id) AS style_id,
               COUNT(*) AS n_fragments
        FROM FRAG_TAGS
        GROUP BY page_id, row_cluster, cell_id
    )

    SELECT {doc_id}, m.page_id, m.row_cluster, m.cell_id,
           m.x, m.y, m.w, m.h, m.text,
           m.weight, m.font_size, m.font_name, m.color, m.style_id,
           sh.style_rank, sh.n_bboxes, m.n_fragments
    FROM MERGED AS m
    LEFT JOIN STYLE_HIST AS sh USING (style_id)
    """)

    n = con.execute(
        f"SELECT COUNT(*) FROM cells WHERE doc_id = {doc_id}"
    ).fetchone()[0]
    n_pages = con.execute(
        f"SELECT COUNT(DISTINCT page_id) FROM cells WHERE doc_id = {doc_id}"
    ).fetchone()[0]
    return n, n_pages


# ── Evidence passes ─────────────────────────────────────────────────
# Each pass reads cells (and possibly earlier evidence) and appends to
# the appropriate evidence table.  Passes are order-independent unless
# noted otherwise.


def pass_style_histogram(con, doc_id=1):
    """Style rarity, bold, dominant/rare flags."""
    con.execute(f"""
    INSERT INTO evidence
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'style' AS source, key, value
    FROM cells
    CROSS JOIN LATERAL (VALUES
        ('style_rank',  CAST(style_rank AS VARCHAR)),
        ('style_count', CAST(style_count AS VARCHAR)),
        ('is_bold',     CAST(weight = 'bold' AS VARCHAR)),
        ('is_rare',     CAST(style_rank >= 3 AS VARCHAR)),
        ('is_dominant', CAST(style_rank = 1 AS VARCHAR))
    ) AS kv(key, value)
    WHERE doc_id = {doc_id}
    """)


def pass_pattern_match(con, doc_id=1):
    """Regex / TRY_CAST text probes."""
    con.execute(f"""
    INSERT INTO evidence
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'pattern' AS source, 'type' AS key,
           CASE
               WHEN regexp_matches(TRIM(text), '^\\d{{4}}$')
                    AND TRY_CAST(TRIM(text) AS INTEGER) BETWEEN 1900 AND 2099
               THEN 'year'
               WHEN TRY_CAST(
                    REPLACE(REPLACE(REPLACE(text, ',', ''), '$', ''), ' ', '')
                    AS DOUBLE) IS NOT NULL
               THEN 'measure'
               WHEN TRY_CAST(text AS DATE) IS NOT NULL
               THEN 'date'
               WHEN regexp_matches(text, '^\\$[\\d,.]+$')
               THEN 'currency'
               WHEN regexp_matches(text, '^[\\d,.]+\\s*%$')
               THEN 'percentage'
               WHEN regexp_matches(text, '^\\d{{5}}(-\\d{{4}})?$')
               THEN 'zip_code'
               WHEN regexp_matches(text, '^\\d{{4}}-\\d{{2}}-\\d{{2}}')
               THEN 'iso_date'
               ELSE NULL
           END AS value
    FROM cells WHERE doc_id = {doc_id}
    """)


def pass_page_position(con, doc_id=1):
    """Cell position relative to page content bounds."""
    con.execute(f"""
    INSERT INTO evidence
    WITH PAGE_DIM AS (
        SELECT page_id,
               MIN(y) AS y_top, MAX(y + h) AS y_bot,
               MIN(x) AS x_left, MAX(x + w) AS x_right
        FROM cells WHERE doc_id = {doc_id}
        GROUP BY page_id
    )
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'position' AS source, key, value
    FROM cells AS c
    JOIN PAGE_DIM AS p USING (page_id)
    CROSS JOIN LATERAL (VALUES
        ('y_frac', CAST(
            (c.y - p.y_top) / NULLIF(p.y_bot - p.y_top, 0) AS VARCHAR)),
        ('x_frac', CAST(
            (c.x - p.x_left) / NULLIF(p.x_right - p.x_left, 0) AS VARCHAR)),
        ('w_frac', CAST(
            c.w / NULLIF(p.x_right - p.x_left, 0) AS VARCHAR)),
        ('is_top_10pct', CAST(
            c.y < p.y_top + (p.y_bot - p.y_top) * 0.1 AS VARCHAR)),
        ('is_bottom_10pct', CAST(
            c.y + c.h > p.y_bot - (p.y_bot - p.y_top) * 0.1 AS VARCHAR))
    ) AS kv(key, value)
    WHERE c.doc_id = {doc_id}
    """)


def pass_row_geometry(con, doc_id=1):
    """Row-level geometry: cell count, typed count, leftmost cell, extent."""
    # Row-level evidence
    con.execute(f"""
    INSERT INTO row_evidence
    WITH ROW_STATS AS (
        SELECT c.page_id, c.row_cluster,
               COUNT(*) AS n_cells,
               SUM(CASE WHEN e.value IN
                   ('measure','currency','percentage','date','iso_date','year')
                   THEN 1 ELSE 0 END) AS n_typed,
               MIN(c.x) AS row_left,
               MAX(c.x + c.w) AS row_right,
               MIN(c.y) AS row_top,
               MAX(c.y + c.h) AS row_bottom
        FROM cells AS c
        LEFT JOIN evidence AS e
          ON c.doc_id = e.doc_id AND c.page_id = e.page_id
         AND c.row_cluster = e.row_cluster AND c.cell_id = e.cell_id
         AND e.source = 'pattern' AND e.key = 'type'
        WHERE c.doc_id = {doc_id}
        GROUP BY c.page_id, c.row_cluster
    )
    SELECT {doc_id}, page_id, row_cluster,
           'row_geom' AS source, key, value
    FROM ROW_STATS
    CROSS JOIN LATERAL (VALUES
        ('n_cells',    CAST(n_cells AS VARCHAR)),
        ('n_typed',    CAST(n_typed AS VARCHAR)),
        ('is_data_row', CAST(n_typed >= 2 AS VARCHAR)),
        ('row_left',   CAST(row_left AS VARCHAR)),
        ('row_right',  CAST(row_right AS VARCHAR)),
        ('row_width',  CAST(row_right - row_left AS VARCHAR))
    ) AS kv(key, value)
    """)

    # Cell-level: is this cell the leftmost in its row?
    con.execute(f"""
    INSERT INTO evidence
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'row_geom' AS source, 'is_leftmost' AS key,
           CAST(c.x <= rm.min_x + 5 AS VARCHAR) AS value
    FROM cells AS c
    JOIN (SELECT page_id, row_cluster, MIN(x) AS min_x
          FROM cells WHERE doc_id = {doc_id}
          GROUP BY page_id, row_cluster) AS rm
      ON c.page_id = rm.page_id AND c.row_cluster = rm.row_cluster
    WHERE c.doc_id = {doc_id}
    """)


def pass_column_alignment(con, doc_id=1):
    """X-alignment of typed cells → putative column evidence.

    Typed cells (measures, years, etc.) that share x-positions form
    putative columns. Any cell whose horizontal extent overlaps a
    putative column gets evidence of that alignment.
    """
    con.execute(f"""
    INSERT INTO evidence
    WITH
    -- Typed cells with their x-center
    TYPED AS (
        SELECT c.page_id, c.row_cluster, c.cell_id,
               c.x, c.w, c.x + c.w / 2 AS x_center
        FROM cells AS c
        JOIN evidence AS e
          ON c.doc_id = e.doc_id AND c.page_id = e.page_id
         AND c.row_cluster = e.row_cluster AND c.cell_id = e.cell_id
         AND e.source = 'pattern' AND e.key = 'type'
         AND e.value IS NOT NULL
        WHERE c.doc_id = {doc_id}
    ),
    -- Cluster x_centers into columns (gap-based)
    DISTINCT_X AS (
        SELECT DISTINCT page_id, x_center,
               x_center - LAG(x_center)
                   OVER (PARTITION BY page_id ORDER BY x_center) AS x_gap
        FROM TYPED
    ),
    COL_BINS AS (
        SELECT page_id, x_center,
               SUM(CASE WHEN x_gap IS NULL OR x_gap > 20
                        THEN 1 ELSE 0 END)
                   OVER (PARTITION BY page_id ORDER BY x_center) AS col_id
        FROM DISTINCT_X
    ),
    COLUMNS AS (
        SELECT cb.page_id, cb.col_id,
               MIN(t.x) AS col_left,
               MAX(t.x + t.w) AS col_right,
               AVG(t.x + t.w / 2) AS col_center,
               COUNT(*) AS n_cells
        FROM COL_BINS AS cb
        JOIN TYPED AS t
          ON cb.page_id = t.page_id
         AND ABS(t.x_center - cb.x_center) < 20
        GROUP BY cb.page_id, cb.col_id
    )
    -- For every cell, check if it overlaps any putative column
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'column_align' AS source, key, value
    FROM cells AS c
    LEFT JOIN COLUMNS AS col
      ON c.page_id = col.page_id
     AND c.x < col.col_right AND c.x + c.w > col.col_left
    CROSS JOIN LATERAL (VALUES
        ('subtends_column', CAST(col.col_id IS NOT NULL AS VARCHAR)),
        ('col_id',          CAST(col.col_id AS VARCHAR)),
        ('col_center',      CAST(col.col_center AS VARCHAR))
    ) AS kv(key, value)
    WHERE c.doc_id = {doc_id}
    """)

    # Page-level: how many putative columns?
    con.execute(f"""
    INSERT INTO page_evidence
    SELECT DISTINCT {doc_id}, c.page_id,
           'column_align' AS source,
           'n_columns' AS key,
           CAST(COUNT(DISTINCT e.value) AS VARCHAR) AS value
    FROM cells AS c
    JOIN evidence AS e
      ON c.doc_id = e.doc_id AND c.page_id = e.page_id
     AND c.row_cluster = e.row_cluster AND c.cell_id = e.cell_id
     AND e.source = 'column_align' AND e.key = 'col_id'
     AND e.value IS NOT NULL
    WHERE c.doc_id = {doc_id}
    GROUP BY c.page_id
    """)


def pass_cross_page(con, doc_id=1):
    """Repeated text at same y-position across pages → chrome."""
    n_pages = con.execute(
        f"SELECT COUNT(DISTINCT page_id) FROM cells WHERE doc_id = {doc_id}"
    ).fetchone()[0]

    # Doc-level: page count
    con.execute(f"""
    INSERT INTO doc_evidence VALUES
        ({doc_id}, 'doc_info', 'n_pages', '{n_pages}')
    """)

    if n_pages < 2:
        return  # nothing to compare

    # Cell-level: text repeated on multiple pages at similar y
    con.execute(f"""
    INSERT INTO evidence
    WITH REPEATS AS (
        SELECT text, FLOOR(y / 5) AS y_band,
               COUNT(DISTINCT page_id) AS n_pages
        FROM cells
        WHERE doc_id = {doc_id} AND LENGTH(TRIM(text)) > 0
        GROUP BY text, FLOOR(y / 5)
        HAVING COUNT(DISTINCT page_id) > 1
    )
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'cross_page' AS source,
           'repeated_text_pages' AS key,
           CAST(r.n_pages AS VARCHAR) AS value
    FROM cells AS c
    JOIN REPEATS AS r
      ON c.text = r.text AND FLOOR(c.y / 5) = r.y_band
    WHERE c.doc_id = {doc_id}
    """)


def pass_color_outlier(con, doc_id=1):
    """Color differs from the dominant style → structural signal."""
    con.execute(f"""
    INSERT INTO evidence
    WITH DOMINANT_COLOR AS (
        SELECT color FROM cells
        WHERE doc_id = {doc_id} AND style_rank = 1
        GROUP BY color ORDER BY COUNT(*) DESC LIMIT 1
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'color' AS source,
           'is_color_outlier' AS key,
           CAST(color != (SELECT color FROM DOMINANT_COLOR) AS VARCHAR)
    FROM cells WHERE doc_id = {doc_id}
    """)


def pass_table_regions(con, doc_id=1):
    """Contiguous runs of data rows → table region evidence.

    A data row has >= 2 typed cells. Contiguous runs of >= 2 data rows
    form a table region. Cells in/near a region get tagged.
    """
    # Identify table regions as row-level evidence
    con.execute(f"""
    INSERT INTO row_evidence
    WITH DATA_ROWS AS (
        SELECT page_id, row_cluster, CAST(value AS INTEGER) AS n_typed
        FROM row_evidence
        WHERE doc_id = {doc_id} AND source = 'row_geom'
          AND key = 'is_data_row' AND value = 'true'
    ),
    NUMBERED AS (
        SELECT *,
               row_cluster - ROW_NUMBER()
                   OVER (PARTITION BY page_id ORDER BY row_cluster) AS grp
        FROM DATA_ROWS
    ),
    REGIONS AS (
        SELECT page_id, grp AS table_id,
               MIN(row_cluster) AS start_row,
               MAX(row_cluster) AS end_row,
               COUNT(*) AS n_rows
        FROM NUMBERED
        GROUP BY page_id, grp
        HAVING COUNT(*) >= 2
    )
    -- Tag rows in/near table regions
    SELECT {doc_id}, c.page_id, c.row_cluster,
           'table_region' AS source, key, value
    FROM (SELECT DISTINCT page_id, row_cluster
          FROM cells WHERE doc_id = {doc_id}) AS c
    LEFT JOIN REGIONS AS r
      ON c.page_id = r.page_id
     AND c.row_cluster BETWEEN r.start_row - 5 AND r.end_row
    CROSS JOIN LATERAL (VALUES
        ('in_table',     CAST(r.table_id IS NOT NULL AS VARCHAR)),
        ('table_id',     CAST(r.table_id AS VARCHAR)),
        ('in_data_rows', CAST(c.row_cluster BETWEEN
                              r.start_row AND r.end_row AS VARCHAR)),
        ('above_data',   CAST(c.row_cluster >= r.start_row - 5
                              AND c.row_cluster < r.start_row AS VARCHAR))
    ) AS kv(key, value)
    """)

    # Propagate to cell level
    con.execute(f"""
    INSERT INTO evidence
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           re.source, re.key, re.value
    FROM cells AS c
    JOIN row_evidence AS re
      ON c.doc_id = re.doc_id AND c.page_id = re.page_id
     AND c.row_cluster = re.row_cluster
    WHERE c.doc_id = {doc_id}
      AND re.source = 'table_region'
    """)


# ── All passes in order ────────────────────────────────────────────

ALL_PASSES = [
    ("style_histogram",   pass_style_histogram),
    ("pattern_match",     pass_pattern_match),
    ("page_position",     pass_page_position),
    ("color_outlier",     pass_color_outlier),
    ("cross_page",        pass_cross_page),
    # These depend on pattern evidence existing:
    ("row_geometry",      pass_row_geometry),
    ("column_alignment",  pass_column_alignment),
    ("table_regions",     pass_table_regions),
]


# ── Reporting ───────────────────────────────────────────────────────

def report(con, doc_id=1):
    n_cells = con.execute(
        f"SELECT COUNT(*) FROM cells WHERE doc_id = {doc_id}").fetchone()[0]
    n_ev = con.execute(
        f"SELECT COUNT(*) FROM evidence WHERE doc_id = {doc_id}").fetchone()[0]
    n_row_ev = con.execute(
        f"SELECT COUNT(*) FROM row_evidence WHERE doc_id = {doc_id}").fetchone()[0]
    n_pages = con.execute(
        f"SELECT COUNT(DISTINCT page_id) FROM cells WHERE doc_id = {doc_id}"
    ).fetchone()[0]

    print(f"\n  {n_cells} cells, {n_pages} pages, "
          f"{n_ev} cell-evidence, {n_row_ev} row-evidence")

    # Evidence sources
    sources = con.execute(f"""
        SELECT source, COUNT(DISTINCT key) AS n_keys, COUNT(*) AS n
        FROM evidence WHERE doc_id = {doc_id}
        GROUP BY source ORDER BY source
    """).fetchdf()
    print(f"\n  {'Source':<20s} {'Keys':>5s} {'Rows':>7s}")
    print(f"  {'─'*35}")
    for _, s in sources.iterrows():
        print(f"  {s['source']:<20s} {int(s['n_keys']):5d} {int(s['n']):7d}")

    # Pattern types
    patterns = con.execute(f"""
        SELECT value, COUNT(*) AS n FROM evidence
        WHERE doc_id = {doc_id} AND source = 'pattern'
          AND key = 'type' AND value IS NOT NULL
        GROUP BY value ORDER BY n DESC
    """).fetchdf()
    if len(patterns) > 0:
        print(f"\n  Patterns: {dict(zip(patterns['value'], patterns['n']))}")

    # Table regions
    tables = con.execute(f"""
        SELECT DISTINCT value FROM row_evidence
        WHERE doc_id = {doc_id} AND source = 'table_region'
          AND key = 'table_id' AND value IS NOT NULL
    """).fetchdf()
    print(f"  Table regions: {len(tables)}")

    # Columns per page
    cols = con.execute(f"""
        SELECT page_id, value FROM page_evidence
        WHERE doc_id = {doc_id} AND source = 'column_align'
          AND key = 'n_columns'
    """).fetchdf()
    if len(cols) > 0:
        for _, r in cols.iterrows():
            print(f"  Page {int(r['page_id'])}: {r['value']} putative columns")

    # Cross-page repeats
    repeats = con.execute(f"""
        SELECT c.text, e.value AS n_pages, COUNT(*) AS n
        FROM evidence AS e
        JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
        WHERE e.doc_id = {doc_id} AND e.source = 'cross_page'
        GROUP BY c.text, e.value
        ORDER BY CAST(e.value AS INTEGER) DESC, n DESC
        LIMIT 8
    """).fetchdf()
    if len(repeats) > 0:
        print(f"\n  Cross-page repeats:")
        for _, r in repeats.iterrows():
            print(f"    {r['n_pages']}pg × {int(r['n'])} cells: "
                  f"{repr(r['text'][:50])}")

    # Chrome candidates: rare style + position
    chrome = con.execute(f"""
        SELECT c.text, c.style_rank, c.page_id,
               pos.value AS y_frac
        FROM cells AS c
        JOIN evidence AS style_ev
          ON c.doc_id = style_ev.doc_id AND c.page_id = style_ev.page_id
         AND c.row_cluster = style_ev.row_cluster
         AND c.cell_id = style_ev.cell_id
         AND style_ev.source = 'style' AND style_ev.key = 'is_rare'
         AND style_ev.value = 'true'
        LEFT JOIN evidence AS pos
          ON c.doc_id = pos.doc_id AND c.page_id = pos.page_id
         AND c.row_cluster = pos.row_cluster AND c.cell_id = pos.cell_id
         AND pos.source = 'position' AND pos.key = 'y_frac'
        WHERE c.doc_id = {doc_id}
        ORDER BY c.style_rank, CAST(pos.value AS DOUBLE)
        LIMIT 10
    """).fetchdf()
    if len(chrome) > 0:
        print(f"\n  Rare-style cells:")
        for _, r in chrome.iterrows():
            yf = f"{float(r['y_frac']):.2f}" if r['y_frac'] else '?'
            print(f"    rank={int(r['style_rank'])} p{int(r['page_id'])} "
                  f"y={yf} {repr(r['text'][:50])}")

    # Evidence summary for a sample cell: show all evidence for
    # the first typed cell
    sample = con.execute(f"""
        SELECT c.page_id, c.row_cluster, c.cell_id, c.text
        FROM cells AS c
        JOIN evidence AS e USING (doc_id, page_id, row_cluster, cell_id)
        WHERE c.doc_id = {doc_id} AND e.source = 'pattern'
          AND e.key = 'type' AND e.value = 'measure'
        LIMIT 1
    """).fetchdf()
    if len(sample) > 0:
        s = sample.iloc[0]
        print(f"\n  Sample cell evidence "
              f"(p{int(s['page_id'])} r{int(s['row_cluster'])} "
              f"c{int(s['cell_id'])} {repr(s['text'][:30])}):")
        cell_ev = con.execute(f"""
            SELECT source, key, value FROM evidence
            WHERE doc_id = {doc_id} AND page_id = {int(s['page_id'])}
              AND row_cluster = {int(s['row_cluster'])}
              AND cell_id = {int(s['cell_id'])}
            ORDER BY source, key
        """).fetchdf()
        for _, e in cell_ev.iterrows():
            print(f"    {e['source']:<16s} {e['key']:<25s} {e['value']}")


# ── Main ────────────────────────────────────────────────────────────

def run(con, filepath, doc_id=1):
    name = Path(filepath).stem
    print(f"\n{'='*80}")
    print(f"  {name} (doc_id={doc_id})")
    print(f"{'='*80}")

    n_cells, n_pages = ingest(con, filepath, doc_id)
    print(f"  Ingested {n_cells} cells from {n_pages} pages")

    for pass_name, pass_fn in ALL_PASSES:
        pass_fn(con, doc_id)

    report(con, doc_id)


def init_connection():
    """Create a DuckDB connection with extension loaded and schema ready."""
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    create_schema(con)
    return con


if __name__ == "__main__":
    DATA = Path("/Users/paulharrington/checkouts/blobboxes/test_data/fintabnet/pdf")

    con = init_connection()

    cases = [
        "CFG_2014_page_161",
        "SBUX_2019_page_82",
        "ETR_2004_page_253",
        "DVA_2003_page_40",
        "AIG_2013_page_143",
    ]

    for i, case in enumerate(cases, start=1):
        pdf = DATA / f"{case}.pdf"
        if pdf.exists():
            run(con, str(pdf), doc_id=i)

    # Cross-document query: compare pattern distributions
    print(f"\n{'='*80}")
    print(f"  Cross-document comparison")
    print(f"{'='*80}")
    xdoc = con.execute("""
        SELECT doc_id, value AS pattern, COUNT(*) AS n
        FROM evidence
        WHERE source = 'pattern' AND key = 'type' AND value IS NOT NULL
        GROUP BY doc_id, value
        ORDER BY doc_id, n DESC
    """).fetchdf()
    print(f"\n  Pattern counts by document:")
    for doc_id in sorted(xdoc['doc_id'].unique()):
        subset = xdoc[xdoc['doc_id'] == doc_id]
        counts = dict(zip(subset['pattern'], subset['n']))
        print(f"    doc {doc_id}: {counts}")

    con.close()
