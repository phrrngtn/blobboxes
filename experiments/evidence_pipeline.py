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

EXT_BBOXES = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
EXT_FILTERS = "/Users/paulharrington/checkouts/blobfilters/build/duckdb/blobfilters.duckdb_extension"
DOMAIN_DB = "/Users/paulharrington/checkouts/blobfilters/data/domains.duckdb"


def create_schema(con):
    """Create the evidence accumulation schema."""
    con.execute("""

    -- ── Probe registry ─────────────────────────────────────────────
    -- Each row is a named check that can be applied to a cell.
    -- The ordinal is the bit position in per-cell roaring bitmaps.
    -- URIs are stable identifiers; ordinals are local to this database.
    CREATE TABLE IF NOT EXISTS probe_registry (
        ordinal     INTEGER PRIMARY KEY,
        uri         VARCHAR UNIQUE NOT NULL,  -- e.g. 'pattern:year', 'domain:country_code'
        name        VARCHAR NOT NULL,         -- short display name
        kind        VARCHAR NOT NULL,         -- 'regex', 'try_cast', 'domain', 'style', 'spatial'
        definition  VARCHAR,                  -- the regex, SQL expression, domain filter path, etc.
        description VARCHAR,                  -- human-readable explanation
        tags        VARCHAR                   -- JSON array of taxonomy tags, e.g. '["numeric","temporal"]'
    );

    -- ── Cell probe results ─────────────────────────────────────────
    -- One roaring bitmap per cell: which probes matched.
    -- Join to probe_registry via bf_contains(probes, ordinal) or
    -- bf_to_array(probes) → UNNEST → JOIN probe_registry.
    CREATE TABLE IF NOT EXISTS cell_probes (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        probes      BLOB NOT NULL,            -- roaring bitmap of matching ordinals
        PRIMARY KEY (doc_id, page_id, row_cluster, cell_id)
    );

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

    # Extract to temp tables first — avoids a bug where calling bb(),
    # bb_styles(), bb_fonts() as table functions inside the same CTE
    # fails after many prior file extractions in the same connection.
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ingest_bb AS SELECT * FROM bb('{fp}')")
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ingest_st AS SELECT * FROM bb_styles('{fp}')")
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ingest_fn AS SELECT * FROM bb_fonts('{fp}')")

    con.execute(f"""
    INSERT INTO cells
    WITH
    RAW AS (
        SELECT b.page_id, b.style_id, b.x, b.y, b.w, b.h, b.text,
               s.font_size, s.color, f.name AS font_name,
               CASE WHEN f.name ILIKE '%bold%' OR s.weight = 'bold'
                    THEN 'bold' ELSE 'normal' END AS eff_weight
        FROM _ingest_bb AS b
        JOIN _ingest_st AS s USING (style_id)
        JOIN _ingest_fn AS f USING (font_id)
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

    con.execute("DROP TABLE IF EXISTS _ingest_bb")
    con.execute("DROP TABLE IF EXISTS _ingest_st")
    con.execute("DROP TABLE IF EXISTS _ingest_fn")

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


def seed_probe_registry(con):
    """Populate the probe registry with built-in probes.

    Ordinal ranges by convention:
      0-99:    text pattern probes (regex, TRY_CAST)
      100-199: style/visual probes
      200-299: spatial/geometric probes
      300-399: cross-page probes
      1000+:   domain filter probes (loaded from ATTACHed databases)
    """
    # Use parameterized inserts to avoid quoting hell
    probes = [
        # ── Text pattern probes (0-99) ───────────────────────────
        # Regex probes: definition is a DuckDB-compatible regexp
        (0,  'pattern:numeric',    'Numeric',     'try_cast', None,
         'Cell text castable to number (after stripping $, comma, space)',
         '["numeric"]'),
        (1,  'pattern:year',       'Year',        'regex',    r'^\d{4}$',
         'Standalone 4-digit year (1900-2099)',
         '["numeric","temporal"]'),
        (2,  'pattern:date',       'Date',        'try_cast', None,
         'Cell text parses as a date',
         '["temporal"]'),
        (3,  'pattern:currency',   'Currency',    'regex',    r'^\$[\d,.]+$',
         'Dollar-prefixed number',
         '["numeric","financial"]'),
        (4,  'pattern:percentage', 'Percentage',  'regex',    r'^[\d,.]+\s*%$',
         'Number followed by percent sign',
         '["numeric"]'),
        (5,  'pattern:zip_code',   'ZIP Code',    'regex',    r'^\d{5}(-\d{4})?$',
         'US ZIP code (5 or 5+4 digit)',
         '["geographic","code"]'),
        (6,  'pattern:iso_date',   'ISO Date',    'regex',    r'^\d{4}-\d{2}-\d{2}',
         'ISO 8601 date prefix',
         '["temporal"]'),
        (7,  'pattern:integer',    'Integer',     'try_cast', None,
         'Cell text is a pure integer (after stripping commas)',
         '["numeric"]'),
        (8,  'pattern:negative',   'Negative',    'regex',    r'^\([\d,.]+\)$',
         'Parenthesized number (accounting negative)',
         '["numeric","financial"]'),
        (9,  'pattern:em_dash',    'Em Dash',     'regex',    r'^[—–\-]$',
         'Single dash/em-dash (null marker in financial tables)',
         '["financial","null_marker"]'),

        # ── Style/visual probes (100-199) ────────────────────────
        (100, 'style:bold',         'Bold',           'style', None,
         'Cell uses bold font weight', '["style"]'),
        (101, 'style:italic',       'Italic',         'style', None,
         'Cell uses italic font', '["style"]'),
        (102, 'style:dominant',     'Dominant Style', 'style', None,
         'Most common style on the page', '["style"]'),
        (103, 'style:rare',         'Rare Style',     'style', None,
         'Style rank >= 3 (uncommon)', '["style","structural"]'),
        (104, 'style:color_outlier','Color Outlier',  'style', None,
         'Color differs from dominant style', '["style","structural"]'),

        # ── Spatial/geometric probes (200-299) ───────────────────
        (200, 'spatial:top_10pct',   'Top 10%',      'spatial', None,
         'In the top 10% of page content area', '["position","chrome"]'),
        (201, 'spatial:bottom_10pct','Bottom 10%',   'spatial', None,
         'In the bottom 10% of page content area', '["position","chrome"]'),
        (202, 'spatial:leftmost',    'Leftmost',     'spatial', None,
         'At or near the left edge of its row', '["position","label"]'),
        (203, 'spatial:in_table',    'In Table',     'spatial', None,
         'Within a detected table region', '["table"]'),
        (204, 'spatial:above_data',  'Above Data',   'spatial', None,
         'In header position (1-5 rows above data)', '["table","header"]'),
        (205, 'spatial:subtends_col','Subtends Col', 'spatial', None,
         'Horizontally overlaps a putative column', '["table","column"]'),
        (206, 'spatial:in_data_rows','In Data Rows', 'spatial', None,
         'In a row with >= 2 typed cells', '["table"]'),

        # ── Cross-page probes (300-399) ──────────────────────────
        (300, 'cross_page:repeated','Repeated Text', 'cross_page', None,
         'Same text at same y on 2+ pages', '["chrome","header","footer"]'),
    ]
    con.executemany(
        "INSERT OR IGNORE INTO probe_registry VALUES (?,?,?,?,?,?,?)",
        probes
    )


def seed_domain_probes(con, domain_db_path=DOMAIN_DB):
    """Seed probe_registry with domain probes from an ATTACHed database.

    Each domain gets an ordinal starting at 1000. The URI from the
    domains table becomes the probe URI. The bitmap stays in the
    attached database — probed at query time via bf_contains.
    """
    db_path = Path(domain_db_path)
    if not db_path.exists():
        return 0

    con.execute(f"ATTACH '{db_path}' AS domaindb (READ_ONLY)")

    # Assign ordinals starting at 1000, ordered by URI for stability
    n = con.execute("""
    INSERT OR IGNORE INTO probe_registry
    SELECT 1000 + ROW_NUMBER() OVER (ORDER BY uri) - 1 AS ordinal,
           uri,
           name,
           'domain' AS kind,
           uri AS definition,   -- self-referencing: used to look up bitmap
           source AS description,
           tags
    FROM domaindb.domains
    ORDER BY uri
    """).fetchone()

    count = con.execute(
        "SELECT COUNT(*) FROM probe_registry WHERE kind = 'domain'"
    ).fetchone()[0]
    return count


def pass_build_cell_probes(con, doc_id=1):
    """Build per-cell roaring bitmaps from probe results.

    Drives off the probe_registry table:
    - kind='regex':    JOIN cells × registry, filter by regexp_matches
    - kind='try_cast': individual SQL expressions per probe URI
    - kind='style':    cell metadata checks
    - kind='spatial':  queries against accumulated evidence
    - kind='domain':   bf_contains against ATTACHed domain bitmaps
    - kind='cross_page': queries against cross_page evidence

    Each kind produces (page_id, row_cluster, cell_id, ordinal) rows.
    These are UNIONed, grouped per cell, and packed into a roaring bitmap.
    """
    # Collect all matching (cell, ordinal) pairs into a temp table
    con.execute(f"CREATE OR REPLACE TEMP TABLE _probe_hits (page_id INTEGER, row_cluster INTEGER, cell_id INTEGER, ordinal INTEGER)")

    # ── Regex probes: driven entirely by registry ────────────────
    con.execute(f"""
    INSERT INTO _probe_hits
    SELECT c.page_id, c.row_cluster, c.cell_id, pr.ordinal
    FROM cells AS c
    CROSS JOIN probe_registry AS pr
    WHERE c.doc_id = {doc_id}
      AND pr.kind = 'regex'
      AND pr.definition IS NOT NULL
      AND regexp_matches(TRIM(c.text), pr.definition)
    """)

    # ── Regex year needs extra range check ───────────────────────
    con.execute(f"""
    DELETE FROM _probe_hits
    WHERE ordinal = (SELECT ordinal FROM probe_registry WHERE uri = 'pattern:year')
      AND (page_id, row_cluster, cell_id) IN (
          SELECT page_id, row_cluster, cell_id FROM cells
          WHERE doc_id = {doc_id}
            AND (TRY_CAST(TRIM(text) AS INTEGER) < 1900
                 OR TRY_CAST(TRIM(text) AS INTEGER) > 2099)
      )
    """)

    # ── TRY_CAST probes: each has its own SQL ────────────────────
    # Read ordinals from registry so they're not hardcoded
    try_cast_probes = con.execute(
        "SELECT ordinal, uri FROM probe_registry WHERE kind = 'try_cast'"
    ).fetchall()

    for ordinal, uri in try_cast_probes:
        if uri == 'pattern:numeric':
            expr = "TRY_CAST(REPLACE(REPLACE(REPLACE(text, ',', ''), '$', ''), ' ', '') AS DOUBLE) IS NOT NULL"
        elif uri == 'pattern:date':
            expr = "TRY_CAST(text AS DATE) IS NOT NULL"
        elif uri == 'pattern:integer':
            expr = "TRY_CAST(REPLACE(text, ',', '') AS BIGINT) IS NOT NULL"
        else:
            continue
        con.execute(f"""
        INSERT INTO _probe_hits
        SELECT page_id, row_cluster, cell_id, {ordinal}
        FROM cells WHERE doc_id = {doc_id} AND {expr}
        """)

    # ── Style probes: cell metadata ──────────────────────────────
    style_checks = {
        'style:bold':          "weight = 'bold'",
        'style:italic':        "font_name ILIKE '%italic%'",
        'style:dominant':      "style_rank = 1",
        'style:rare':          "style_rank >= 3",
    }
    for uri, expr in style_checks.items():
        row = con.execute(
            "SELECT ordinal FROM probe_registry WHERE uri = ?", [uri]
        ).fetchone()
        if row:
            con.execute(f"""
            INSERT INTO _probe_hits
            SELECT page_id, row_cluster, cell_id, {row[0]}
            FROM cells WHERE doc_id = {doc_id} AND {expr}
            """)

    # style:color_outlier — needs dominant color subquery
    row = con.execute(
        "SELECT ordinal FROM probe_registry WHERE uri = 'style:color_outlier'"
    ).fetchone()
    if row:
        con.execute(f"""
        INSERT INTO _probe_hits
        SELECT page_id, row_cluster, cell_id, {row[0]}
        FROM cells
        WHERE doc_id = {doc_id}
          AND color != (SELECT color FROM cells
                        WHERE doc_id = {doc_id} AND style_rank = 1
                        GROUP BY color ORDER BY COUNT(*) DESC LIMIT 1)
        """)

    # ── Spatial probes: from accumulated evidence ────────────────
    spatial_evidence_checks = {
        'spatial:top_10pct':    ("position", "is_top_10pct", "true"),
        'spatial:bottom_10pct': ("position", "is_bottom_10pct", "true"),
        'spatial:in_table':     ("table_region", "in_table", "true"),
        'spatial:above_data':   ("table_region", "above_data", "true"),
        'spatial:in_data_rows': ("table_region", "in_data_rows", "true"),
        'spatial:subtends_col': ("column_align", "subtends_column", "true"),
    }
    for uri, (source, key, val) in spatial_evidence_checks.items():
        row = con.execute(
            "SELECT ordinal FROM probe_registry WHERE uri = ?", [uri]
        ).fetchone()
        if row:
            con.execute(f"""
            INSERT INTO _probe_hits
            SELECT DISTINCT page_id, row_cluster, cell_id, {row[0]}
            FROM evidence
            WHERE doc_id = {doc_id}
              AND source = '{source}' AND key = '{key}' AND value = '{val}'
            """)

    # spatial:leftmost — from row_geom evidence
    row = con.execute(
        "SELECT ordinal FROM probe_registry WHERE uri = 'spatial:leftmost'"
    ).fetchone()
    if row:
        con.execute(f"""
        INSERT INTO _probe_hits
        SELECT DISTINCT page_id, row_cluster, cell_id, {row[0]}
        FROM evidence
        WHERE doc_id = {doc_id}
          AND source = 'row_geom' AND key = 'is_leftmost' AND value = 'true'
        """)

    # ── Cross-page probes ────────────────────────────────────────
    row = con.execute(
        "SELECT ordinal FROM probe_registry WHERE uri = 'cross_page:repeated'"
    ).fetchone()
    if row:
        con.execute(f"""
        INSERT INTO _probe_hits
        SELECT DISTINCT page_id, row_cluster, cell_id, {row[0]}
        FROM evidence
        WHERE doc_id = {doc_id}
          AND source = 'cross_page' AND key = 'repeated_text_pages'
        """)

    # ── Domain probes (from ATTACHed databases) ──────────────────
    # Strategy: compute hash once per cell into a temp table, then
    # check each domain bitmap against the set of hashes. This avoids
    # repeated bf_hash_normalized calls in the cross join.
    n_domain_probes = con.execute(
        "SELECT COUNT(*) FROM probe_registry WHERE kind = 'domain'"
    ).fetchone()[0]

    if n_domain_probes > 0:
        # Step 1: hash each cell's text once
        con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _cell_hashes AS
        SELECT page_id, row_cluster, cell_id,
               bf_hash_normalized(text)::UINTEGER AS text_hash
        FROM cells
        WHERE doc_id = {doc_id} AND LENGTH(TRIM(text)) > 1
        """)

        # Step 2: build one bitmap of all doc hashes, then find which
        # domains have ANY intersection. This is O(n_domains) bitmap
        # intersections instead of O(n_cells × n_domains) bf_contains calls.
        con.execute("""
        CREATE OR REPLACE TEMP TABLE _doc_hash_bitmap AS
        SELECT bf_from_array(LIST(DISTINCT text_hash)::UINTEGER[]) AS bm
        FROM _cell_hashes
        """)

        # Step 3: find domains with non-zero intersection (cheap)
        con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _matching_domains AS
        SELECT pr.ordinal, d.bitmap
        FROM probe_registry AS pr
        JOIN domaindb.domains AS d ON d.uri = pr.uri
        WHERE pr.kind = 'domain'
          AND bf_intersection_card(d.bitmap,
              (SELECT bm FROM _doc_hash_bitmap)) > 0
        """)

        # Step 4: only for matching domains, check individual cells
        con.execute(f"""
        INSERT INTO _probe_hits
        SELECT ch.page_id, ch.row_cluster, ch.cell_id, md.ordinal
        FROM _cell_hashes AS ch
        JOIN _matching_domains AS md
          ON bf_contains(md.bitmap, ch.text_hash)
        """)

        con.execute("DROP TABLE IF EXISTS _cell_hashes")
        con.execute("DROP TABLE IF EXISTS _doc_hash_bitmap")
        con.execute("DROP TABLE IF EXISTS _matching_domains")

    # ── Pack hits into roaring bitmaps ───────────────────────────
    con.execute(f"""
    INSERT INTO cell_probes
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           bf_from_array(LIST(DISTINCT ordinal ORDER BY ordinal)::UINTEGER[])
    FROM _probe_hits
    GROUP BY page_id, row_cluster, cell_id
    """)

    con.execute("DROP TABLE IF EXISTS _probe_hits")


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


def pass_blocks(con, doc_id=1):
    """Spatial proximity blocks: rectangular regions separated by whitespace.

    Group rows into blocks by detecting large vertical gaps between them.
    A block is a contiguous run of rows without a gap > 2×body_height.
    This is coarser than table_regions — blocks exist before we know
    if they contain tables.
    """
    con.execute(f"""
    INSERT INTO row_evidence
    WITH
    ROW_POSITIONS AS (
        SELECT page_id, row_cluster,
               MIN(y) AS row_y, MAX(y + h) AS row_bottom,
               MIN(x) AS row_left, MAX(x + w) AS row_right,
               COUNT(*) AS n_cells
        FROM cells WHERE doc_id = {doc_id}
        GROUP BY page_id, row_cluster
    ),
    BODY_H AS (
        SELECT GREATEST(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY h)
                        FILTER (WHERE h > 2), 4.0) AS bh
        FROM cells WHERE doc_id = {doc_id}
    ),
    GAPS AS (
        SELECT *,
               row_y - LAG(row_bottom)
                   OVER (PARTITION BY page_id ORDER BY row_y) AS row_gap,
               LAG(page_id)
                   OVER (PARTITION BY page_id ORDER BY row_y) AS prev_page
        FROM ROW_POSITIONS
    ),
    BLOCK_IDS AS (
        SELECT page_id, row_cluster, row_y, row_left, row_right, n_cells,
               SUM(CASE
                   WHEN row_gap IS NULL THEN 1
                   WHEN row_gap > (SELECT bh FROM BODY_H) * 2 THEN 1
                   ELSE 0
               END) OVER (PARTITION BY page_id ORDER BY row_y) AS block_id
        FROM GAPS
    ),
    BLOCK_STATS AS (
        SELECT page_id, block_id,
               COUNT(*) AS n_rows,
               MIN(row_left) AS block_left,
               MAX(row_right) AS block_right,
               MODE(n_cells) AS modal_cells
        FROM BLOCK_IDS
        GROUP BY page_id, block_id
    )
    SELECT {doc_id}, bi.page_id, bi.row_cluster,
           'block' AS source, key, value
    FROM BLOCK_IDS AS bi
    JOIN BLOCK_STATS AS bs
      ON bi.page_id = bs.page_id AND bi.block_id = bs.block_id
    CROSS JOIN LATERAL (VALUES
        ('block_id',     CAST(bi.block_id AS VARCHAR)),
        ('block_n_rows', CAST(bs.n_rows AS VARCHAR)),
        ('block_left',   CAST(bs.block_left AS VARCHAR)),
        ('block_right',  CAST(bs.block_right AS VARCHAR)),
        ('block_modal_cells', CAST(bs.modal_cells AS VARCHAR))
    ) AS kv(key, value)
    """)


def pass_indent_levels(con, doc_id=1):
    """Left-edge clustering → indent level evidence.

    Cluster the distinct left-edge x-positions of non-dominant-style
    or leftmost cells into indent levels using gap-based binning.
    Indent level encodes hierarchy: level 0 = outermost scope.
    """
    con.execute(f"""
    INSERT INTO evidence
    WITH
    -- Collect distinct left-edge x positions for structural cells
    -- (bold, rare, or leftmost in their row)
    LEFT_EDGES AS (
        SELECT DISTINCT ROUND(c.x, 0) AS x_rounded
        FROM cells AS c
        WHERE c.doc_id = {doc_id}
          AND (c.weight = 'bold'
               OR c.style_rank >= 2
               OR c.x <= (SELECT MIN(x) + 5 FROM cells
                          WHERE doc_id = {doc_id}
                            AND page_id = c.page_id
                            AND row_cluster = c.row_cluster))
    ),
    SORTED_EDGES AS (
        SELECT x_rounded,
               x_rounded - LAG(x_rounded) OVER (ORDER BY x_rounded) AS x_gap
        FROM LEFT_EDGES
    ),
    INDENT_BINS AS (
        SELECT x_rounded,
               SUM(CASE WHEN x_gap IS NULL OR x_gap > 10
                        THEN 1 ELSE 0 END)
                   OVER (ORDER BY x_rounded) - 1 AS indent_level
        FROM SORTED_EDGES
    )
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'indent' AS source,
           'indent_level' AS key,
           CAST(ib.indent_level AS VARCHAR) AS value
    FROM cells AS c
    JOIN INDENT_BINS AS ib
      ON ROUND(c.x, 0) = ib.x_rounded
    WHERE c.doc_id = {doc_id}
    """)


def pass_alignment(con, doc_id=1):
    """Cell alignment within columns: left, right, center, or decimal.

    Right-alignment is a strong signal for numeric data.
    Left-alignment signals labels. Center-alignment may indicate headers.
    Determined by comparing cell edges to putative column boundaries.
    """
    con.execute(f"""
    INSERT INTO evidence
    WITH
    -- Get column boundaries from column_align evidence
    COL_BOUNDS AS (
        SELECT DISTINCT e_id.page_id,
               CAST(e_id.value AS INTEGER) AS col_id,
               CAST(e_ctr.value AS DOUBLE) AS col_center
        FROM evidence AS e_id
        JOIN evidence AS e_ctr
          ON e_id.doc_id = e_ctr.doc_id AND e_id.page_id = e_ctr.page_id
         AND e_id.row_cluster = e_ctr.row_cluster
         AND e_id.cell_id = e_ctr.cell_id
         AND e_ctr.source = 'column_align' AND e_ctr.key = 'col_center'
        WHERE e_id.doc_id = {doc_id}
          AND e_id.source = 'column_align' AND e_id.key = 'col_id'
          AND e_id.value IS NOT NULL
    ),
    -- For cells that subtend a column, compute alignment
    ALIGNED AS (
        SELECT c.page_id, c.row_cluster, c.cell_id,
               c.x AS cell_left,
               c.x + c.w AS cell_right,
               c.x + c.w / 2 AS cell_center,
               cb.col_center,
               -- Alignment: compare cell center vs column center
               -- If cell right-edge aligns across rows → right-aligned
               -- If cell left-edge aligns → left-aligned
               ABS(cell_center - cb.col_center) AS center_offset,
               c.w AS cell_width
        FROM cells AS c
        JOIN evidence AS e_col
          ON c.doc_id = e_col.doc_id AND c.page_id = e_col.page_id
         AND c.row_cluster = e_col.row_cluster
         AND c.cell_id = e_col.cell_id
         AND e_col.source = 'column_align' AND e_col.key = 'col_id'
         AND e_col.value IS NOT NULL
        JOIN COL_BOUNDS AS cb
          ON c.page_id = cb.page_id
         AND CAST(e_col.value AS INTEGER) = cb.col_id
        WHERE c.doc_id = {doc_id}
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'alignment' AS source,
           'type' AS key,
           CASE
               -- Narrow cell near column center → centered (likely header)
               WHEN cell_width < 30 AND center_offset < 5 THEN 'center'
               -- Cell right edge near column center → right-aligned (numeric)
               WHEN ABS(cell_right - col_center) < ABS(cell_left - col_center)
               THEN 'right'
               -- Otherwise → left-aligned (label)
               ELSE 'left'
           END AS value
    FROM ALIGNED
    """)


def pass_section_scope(con, doc_id=1):
    """Section scope propagation: bold/rare leftmost cells define scopes.

    A section label is a cell that is:
    - Bold or rare style
    - Leftmost (or near-leftmost) in its row
    - Row has no or few typed cells (not a data row)

    Section labels propagate "down and to the right" until the next
    label at the same or lesser indent level.
    """
    # Identify section label candidates
    con.execute(f"""
    INSERT INTO evidence
    WITH
    LABEL_CANDIDATES AS (
        SELECT c.page_id, c.row_cluster, c.cell_id, c.text,
               c.x, c.y, c.weight, c.style_rank
        FROM cells AS c
        LEFT JOIN row_evidence AS re
          ON c.doc_id = re.doc_id AND c.page_id = re.page_id
         AND c.row_cluster = re.row_cluster
         AND re.source = 'row_geom' AND re.key = 'n_typed'
        WHERE c.doc_id = {doc_id}
          AND (c.weight = 'bold' OR c.style_rank >= 2)
          AND c.x <= (SELECT MIN(x) + 10 FROM cells AS c2
                      WHERE c2.doc_id = {doc_id}
                        AND c2.page_id = c.page_id
                        AND c2.row_cluster = c.row_cluster)
          AND COALESCE(CAST(re.value AS INTEGER), 0) <= 1
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'section' AS source,
           'is_section_label' AS key,
           'true' AS value
    FROM LABEL_CANDIDATES
    """)

    # Propagate scope via indent levels
    # For each cell, find the nearest section label above it at each
    # indent level, using the evidence already accumulated
    con.execute(f"""
    INSERT INTO evidence
    WITH
    LABELS AS (
        SELECT c.page_id, c.row_cluster, c.cell_id, c.text, c.y,
               COALESCE(CAST(ind.value AS INTEGER), 0) AS indent_level
        FROM cells AS c
        JOIN evidence AS sec
          ON c.doc_id = sec.doc_id AND c.page_id = sec.page_id
         AND c.row_cluster = sec.row_cluster AND c.cell_id = sec.cell_id
         AND sec.source = 'section' AND sec.key = 'is_section_label'
         AND sec.value = 'true'
        LEFT JOIN evidence AS ind
          ON c.doc_id = ind.doc_id AND c.page_id = ind.page_id
         AND c.row_cluster = ind.row_cluster AND c.cell_id = ind.cell_id
         AND ind.source = 'indent' AND ind.key = 'indent_level'
        WHERE c.doc_id = {doc_id}
    ),
    -- For level 0: last label at indent 0 above each row
    SCOPE_L0 AS (
        SELECT c.page_id, c.row_cluster, c.cell_id,
               LAST_VALUE(l.text IGNORE NULLS) OVER (
                   PARTITION BY c.page_id
                   ORDER BY c.row_cluster
                   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
               ) AS scope_text
        FROM cells AS c
        LEFT JOIN LABELS AS l
          ON c.page_id = l.page_id
         AND c.row_cluster = l.row_cluster
         AND l.indent_level = 0
        WHERE c.doc_id = {doc_id}
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'section' AS source,
           'scope_l0' AS key,
           scope_text AS value
    FROM SCOPE_L0
    WHERE scope_text IS NOT NULL
    """)


def pass_token_domain_probing(con, doc_id=1):
    """Tokenize cell text, probe tokens against domain filters.

    Unlike the boolean cell-level probe (which hashes the whole cell),
    this splits cell text into tokens and checks each token individually.
    Produces valued evidence:
      - domain_token_hits: how many tokens matched this domain
      - domain_containment: fraction of tokens that matched

    This catches multi-token cells like "Sunnyvale, California 94086 USA"
    where individual tokens match different domains.
    """
    n_domain_probes = con.execute(
        "SELECT COUNT(*) FROM probe_registry WHERE kind = 'domain'"
    ).fetchone()[0]
    if n_domain_probes == 0:
        return

    # Step 1: tokenize cells — split on whitespace and common punctuation
    # Keep only tokens with length > 1 (skip single chars)
    con.execute(f"""
    CREATE OR REPLACE TEMP TABLE _cell_tokens AS
    SELECT c.page_id, c.row_cluster, c.cell_id,
           t.token,
           bf_hash_normalized(t.token)::UINTEGER AS token_hash
    FROM cells AS c,
         LATERAL UNNEST(
             string_split_regex(TRIM(c.text), '[\\s,;:()\\[\\]/]+')
         ) AS t(token)
    WHERE c.doc_id = {doc_id}
      AND LENGTH(TRIM(c.text)) > 1
      AND LENGTH(t.token) > 1
    """)

    n_tokens = con.execute("SELECT COUNT(*) FROM _cell_tokens").fetchone()[0]
    if n_tokens == 0:
        con.execute("DROP TABLE IF EXISTS _cell_tokens")
        return

    # Step 2: build doc-level token bitmap for fast domain screening
    con.execute("""
    CREATE OR REPLACE TEMP TABLE _token_bitmap AS
    SELECT bf_from_array(LIST(DISTINCT token_hash)::UINTEGER[]) AS bm
    FROM _cell_tokens
    """)

    # Step 3: find domains with any intersection
    con.execute("""
    CREATE OR REPLACE TEMP TABLE _token_matching_domains AS
    SELECT pr.ordinal, pr.uri, d.bitmap
    FROM probe_registry AS pr
    JOIN domaindb.domains AS d ON d.uri = pr.uri
    WHERE pr.kind = 'domain'
      AND bf_intersection_card(d.bitmap,
          (SELECT bm FROM _token_bitmap)) > 0
    """)

    n_matching = con.execute(
        "SELECT COUNT(*) FROM _token_matching_domains"
    ).fetchone()[0]

    if n_matching > 0:
        # Step 4: for each cell, count how many tokens hit each domain
        # and compute containment (hits / total tokens in cell)
        con.execute(f"""
        INSERT INTO evidence
        WITH
        TOKEN_HITS AS (
            SELECT ct.page_id, ct.row_cluster, ct.cell_id,
                   md.uri AS domain_uri, md.ordinal,
                   COUNT(*) AS n_hits
            FROM _cell_tokens AS ct
            JOIN _token_matching_domains AS md
              ON bf_contains(md.bitmap, ct.token_hash)
            GROUP BY ct.page_id, ct.row_cluster, ct.cell_id,
                     md.uri, md.ordinal
        ),
        CELL_TOKEN_COUNTS AS (
            SELECT page_id, row_cluster, cell_id,
                   COUNT(*) AS n_tokens
            FROM _cell_tokens
            GROUP BY page_id, row_cluster, cell_id
        )
        SELECT {doc_id}, th.page_id, th.row_cluster, th.cell_id,
               'token_domain' AS source,
               th.domain_uri AS key,
               CAST(ROUND(th.n_hits::DOUBLE / tc.n_tokens, 3) AS VARCHAR) AS value
        FROM TOKEN_HITS AS th
        JOIN CELL_TOKEN_COUNTS AS tc USING (page_id, row_cluster, cell_id)
        WHERE th.n_hits >= 1
        """)

    con.execute("DROP TABLE IF EXISTS _cell_tokens")
    con.execute("DROP TABLE IF EXISTS _token_bitmap")
    con.execute("DROP TABLE IF EXISTS _token_matching_domains")


# ── All passes in order ────────────────────────────────────────────

ALL_PASSES = [
    # No dependencies:
    ("style_histogram",   pass_style_histogram),
    ("pattern_match",     pass_pattern_match),
    ("page_position",     pass_page_position),
    ("color_outlier",     pass_color_outlier),
    ("cross_page",        pass_cross_page),
    # Depends on pattern:
    ("row_geometry",      pass_row_geometry),
    ("column_alignment",  pass_column_alignment),
    ("table_regions",     pass_table_regions),
    # Depends on row_geometry / column_alignment:
    ("blocks",            pass_blocks),
    ("indent_levels",     pass_indent_levels),
    ("alignment",         pass_alignment),
    ("section_scope",     pass_section_scope),
    # Token-level domain probing (valued evidence, not just boolean):
    ("token_domains",     pass_token_domain_probing),
    # Probe bitmap (runs after all other evidence is in place):
    ("cell_probes",       pass_build_cell_probes),
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

    # Blocks
    blocks = con.execute(f"""
        SELECT DISTINCT value FROM row_evidence
        WHERE doc_id = {doc_id} AND source = 'block' AND key = 'block_id'
    """).fetchdf()
    print(f"  Blocks: {len(blocks)}")

    # Alignment distribution
    align = con.execute(f"""
        SELECT value, COUNT(*) AS n FROM evidence
        WHERE doc_id = {doc_id} AND source = 'alignment' AND key = 'type'
        GROUP BY value ORDER BY n DESC
    """).fetchdf()
    if len(align) > 0:
        print(f"  Alignment: {dict(zip(align['value'], align['n']))}")

    # Section labels
    sections = con.execute(f"""
        SELECT c.text, c.page_id, c.row_cluster,
               ind.value AS indent
        FROM evidence AS e
        JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
        LEFT JOIN evidence AS ind
          ON c.doc_id = ind.doc_id AND c.page_id = ind.page_id
         AND c.row_cluster = ind.row_cluster AND c.cell_id = ind.cell_id
         AND ind.source = 'indent' AND ind.key = 'indent_level'
        WHERE e.doc_id = {doc_id} AND e.source = 'section'
          AND e.key = 'is_section_label' AND e.value = 'true'
        ORDER BY c.page_id, c.row_cluster
        LIMIT 15
    """).fetchdf()
    if len(sections) > 0:
        print(f"\n  Section labels:")
        for _, s in sections.iterrows():
            ind = f"L{s['indent']}" if s['indent'] else '?'
            print(f"    p{int(s['page_id'])} {ind} {repr(s['text'][:60])}")

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

        # Show probe bitmap for same cell
        probes = con.execute(f"""
            SELECT pr.ordinal, pr.uri, pr.name
            FROM cell_probes AS cp,
                 LATERAL UNNEST(bf_to_array(cp.probes)) AS t(ordinal)
            JOIN probe_registry AS pr ON pr.ordinal = t.ordinal::INTEGER
            WHERE cp.doc_id = {doc_id}
              AND cp.page_id = {int(s['page_id'])}
              AND cp.row_cluster = {int(s['row_cluster'])}
              AND cp.cell_id = {int(s['cell_id'])}
            ORDER BY pr.ordinal
        """).fetchdf()
        if len(probes) > 0:
            print(f"\n  Probe bitmap ({len(probes)} probes matched):")
            for _, p in probes.iterrows():
                print(f"    [{int(p['ordinal']):3d}] {p['uri']:<30s} {p['name']}")

    # Probe coverage summary
    probe_stats = con.execute(f"""
        SELECT pr.uri, pr.name, COUNT(*) AS n_cells
        FROM cell_probes AS cp,
             LATERAL UNNEST(bf_to_array(cp.probes)) AS t(ordinal)
        JOIN probe_registry AS pr ON pr.ordinal = t.ordinal::INTEGER
        WHERE cp.doc_id = {doc_id}
        GROUP BY pr.uri, pr.name
        ORDER BY n_cells DESC
    """).fetchdf()
    if len(probe_stats) > 0:
        print(f"\n  Probe coverage (cells matching each probe):")
        for _, p in probe_stats.iterrows():
            print(f"    {p['uri']:<30s} {int(p['n_cells']):5d}")


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
    """Create a DuckDB connection with extensions loaded, schema and registry ready."""
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_BBOXES}'")
    con.execute(f"LOAD '{EXT_FILTERS}'")
    create_schema(con)
    seed_probe_registry(con)
    n_domains = seed_domain_probes(con)
    if n_domains:
        print(f"  Loaded {n_domains} domain probes from {DOMAIN_DB}")
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
