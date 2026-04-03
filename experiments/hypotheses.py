"""Hypothesis generation and testing over the evidence tables.

Given accumulated evidence from evidence_pipeline.py, generate
structural hypotheses about cells and test them against the evidence.

Hypotheses are rows in a hypothesis table:
  hypotheses(doc_id, page_id, row_cluster, cell_id, hypothesis, support, against)

Each hypothesis is a proposed interpretation. Support and against
count the evidence rows that are consistent or inconsistent with it.
The treemap path is assembled from the surviving hypotheses.
"""

import duckdb


def create_hypothesis_tables(con):
    """Create tables for hypothesis results."""
    con.execute("""
    CREATE TABLE IF NOT EXISTS hypotheses (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        hypothesis  VARCHAR NOT NULL,
        detail      VARCHAR,             -- e.g. col_id, indent_level
        support     INTEGER DEFAULT 0,   -- evidence FOR
        against     INTEGER DEFAULT 0    -- evidence AGAINST
    );

    CREATE TABLE IF NOT EXISTS treemap (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        path        VARCHAR,
        role        VARCHAR              -- winning hypothesis
    );
    """)


def hypothesize_chrome(con, doc_id=1):
    """Hypothesize page chrome: headers, footers, page numbers.

    Evidence FOR: cross_page:repeated, style:rare, spatial:top/bottom_10pct
    Evidence AGAINST: pattern:numeric (for titles), spatial:in_table
    """
    con.execute(f"""
    INSERT INTO hypotheses
    WITH
    -- Cells with cross-page repetition are strong chrome candidates
    CHROME_CANDIDATES AS (
        SELECT DISTINCT c.page_id, c.row_cluster, c.cell_id, c.text
        FROM cells AS c
        JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
        WHERE c.doc_id = {doc_id}
          AND (
              -- Repeated across pages
              bf_contains(cp.probes, 300::UINTEGER)
              -- OR rare style at page edges
              OR (bf_contains(cp.probes, 103::UINTEGER)
                  AND (bf_contains(cp.probes, 200::UINTEGER)
                       OR bf_contains(cp.probes, 201::UINTEGER)))
          )
    ),
    -- Count supporting and contradicting evidence
    SCORED AS (
        SELECT cc.*,
               -- Support: each of these counts as +1
               (SELECT COUNT(*) FROM cell_probes AS cp
                WHERE cp.doc_id = {doc_id}
                  AND cp.page_id = cc.page_id
                  AND cp.row_cluster = cc.row_cluster
                  AND cp.cell_id = cc.cell_id
                  AND (bf_contains(cp.probes, 300::UINTEGER)   -- cross-page repeated
                       OR bf_contains(cp.probes, 103::UINTEGER) -- rare style
                       OR bf_contains(cp.probes, 200::UINTEGER) -- top 10%
                       OR bf_contains(cp.probes, 201::UINTEGER) -- bottom 10%
                  )) AS support,
               -- Against: in a table region
               (SELECT COUNT(*) FROM cell_probes AS cp
                WHERE cp.doc_id = {doc_id}
                  AND cp.page_id = cc.page_id
                  AND cp.row_cluster = cc.row_cluster
                  AND cp.cell_id = cc.cell_id
                  AND bf_contains(cp.probes, 203::UINTEGER)  -- in_table
               ) AS against
        FROM CHROME_CANDIDATES AS cc
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'chrome' AS hypothesis,
           text AS detail,
           support, against
    FROM SCORED
    WHERE support > against
    """)


def hypothesize_column_headers(con, doc_id=1):
    """Hypothesize column headers: cells above data that subtend columns.

    A cell is hypothesized as a column header if:
    - It subtends a putative column (spatial:subtends_col)
    - It is above data rows (spatial:above_data)
    - It is NOT a measure/numeric (contradicts header role)
    """
    con.execute(f"""
    INSERT INTO hypotheses
    WITH
    HEADER_CANDIDATES AS (
        SELECT c.page_id, c.row_cluster, c.cell_id, c.text,
               e_col.value AS col_id
        FROM cells AS c
        JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
        LEFT JOIN evidence AS e_col
          ON c.doc_id = e_col.doc_id AND c.page_id = e_col.page_id
         AND c.row_cluster = e_col.row_cluster AND c.cell_id = e_col.cell_id
         AND e_col.source = 'column_align' AND e_col.key = 'col_id'
        WHERE c.doc_id = {doc_id}
          -- Subtends a column AND above data
          AND bf_contains(cp.probes, 205::UINTEGER)
          AND bf_contains(cp.probes, 204::UINTEGER)
    )
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           'col_header' AS hypothesis,
           col_id AS detail,
           -- Support: subtends + above_data + not_numeric
           2 + CASE WHEN NOT EXISTS (
               SELECT 1 FROM cell_probes AS cp2
               WHERE cp2.doc_id = {doc_id}
                 AND cp2.page_id = hc.page_id
                 AND cp2.row_cluster = hc.row_cluster
                 AND cp2.cell_id = hc.cell_id
                 AND bf_contains(cp2.probes, 0::UINTEGER)  -- numeric
           ) THEN 1 ELSE 0 END AS support,
           -- Against: is numeric
           CASE WHEN EXISTS (
               SELECT 1 FROM cell_probes AS cp2
               WHERE cp2.doc_id = {doc_id}
                 AND cp2.page_id = hc.page_id
                 AND cp2.row_cluster = hc.row_cluster
                 AND cp2.cell_id = hc.cell_id
                 AND bf_contains(cp2.probes, 0::UINTEGER)
           ) THEN 1 ELSE 0 END AS against
    FROM HEADER_CANDIDATES AS hc
    """)


def hypothesize_section_labels(con, doc_id=1):
    """Hypothesize section labels: bold/rare leftmost cells with no measures.

    Support: bold, rare, leftmost, no measures in row
    Against: in data rows, numeric
    """
    con.execute(f"""
    INSERT INTO hypotheses
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'section_label' AS hypothesis,
           e_ind.value AS detail,  -- indent level
           -- Support
           (CASE WHEN bf_contains(cp.probes, 100::UINTEGER) THEN 1 ELSE 0 END  -- bold
            + CASE WHEN bf_contains(cp.probes, 103::UINTEGER) THEN 1 ELSE 0 END -- rare
            + CASE WHEN bf_contains(cp.probes, 202::UINTEGER) THEN 1 ELSE 0 END -- leftmost
           ) AS support,
           -- Against
           (CASE WHEN bf_contains(cp.probes, 0::UINTEGER) THEN 1 ELSE 0 END    -- numeric
            + CASE WHEN bf_contains(cp.probes, 206::UINTEGER) THEN 1 ELSE 0 END -- in data rows
           ) AS against
    FROM cells AS c
    JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
    LEFT JOIN evidence AS e_ind
      ON c.doc_id = e_ind.doc_id AND c.page_id = e_ind.page_id
     AND c.row_cluster = e_ind.row_cluster AND c.cell_id = e_ind.cell_id
     AND e_ind.source = 'indent' AND e_ind.key = 'indent_level'
    WHERE c.doc_id = {doc_id}
      -- Bold or rare
      AND (bf_contains(cp.probes, 100::UINTEGER)
           OR bf_contains(cp.probes, 103::UINTEGER))
      -- Leftmost
      AND bf_contains(cp.probes, 202::UINTEGER)
      -- Not numeric
      AND NOT bf_contains(cp.probes, 0::UINTEGER)
    """)


def hypothesize_row_labels(con, doc_id=1):
    """Hypothesize row labels: leftmost non-numeric cell in a data row.

    Support: leftmost, in_data_rows, not numeric
    Against: numeric, bold (more likely section label)
    """
    con.execute(f"""
    INSERT INTO hypotheses
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'row_label' AS hypothesis,
           NULL AS detail,
           -- Support: leftmost + in data rows
           (CASE WHEN bf_contains(cp.probes, 202::UINTEGER) THEN 1 ELSE 0 END
            + CASE WHEN bf_contains(cp.probes, 206::UINTEGER) THEN 1 ELSE 0 END
           ) AS support,
           -- Against: numeric, or already hypothesized as section_label
           (CASE WHEN bf_contains(cp.probes, 0::UINTEGER) THEN 1 ELSE 0 END
           ) AS against
    FROM cells AS c
    JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
    WHERE c.doc_id = {doc_id}
      AND bf_contains(cp.probes, 202::UINTEGER)    -- leftmost
      AND bf_contains(cp.probes, 206::UINTEGER)    -- in data rows
      AND NOT bf_contains(cp.probes, 0::UINTEGER)  -- not numeric
    """)


def hypothesize_measures(con, doc_id=1):
    """Hypothesize measures: numeric cells in table regions.

    This is high confidence — pattern match + table context.
    """
    con.execute(f"""
    INSERT INTO hypotheses
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'measure' AS hypothesis,
           e_pat.value AS detail,  -- 'measure', 'currency', 'percentage', etc.
           -- Support: numeric + in table
           (CASE WHEN bf_contains(cp.probes, 0::UINTEGER) THEN 1 ELSE 0 END
            + CASE WHEN bf_contains(cp.probes, 203::UINTEGER) THEN 1 ELSE 0 END
            + CASE WHEN bf_contains(cp.probes, 205::UINTEGER) THEN 1 ELSE 0 END
           ) AS support,
           0 AS against
    FROM cells AS c
    JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
    LEFT JOIN evidence AS e_pat
      ON c.doc_id = e_pat.doc_id AND c.page_id = e_pat.page_id
     AND c.row_cluster = e_pat.row_cluster AND c.cell_id = e_pat.cell_id
     AND e_pat.source = 'pattern' AND e_pat.key = 'type'
    WHERE c.doc_id = {doc_id}
      AND bf_contains(cp.probes, 0::UINTEGER)  -- numeric
    """)


def resolve_hypotheses(con, doc_id=1):
    """Pick the winning hypothesis per cell.

    Priority: chrome > col_header > section_label > row_label > measure > unresolved
    Within same hypothesis type: highest (support - against) wins.
    """
    con.execute(f"""
    INSERT INTO treemap
    WITH
    PRIORITY AS (
        SELECT *, support - against AS net,
               CASE hypothesis
                   WHEN 'chrome'        THEN 1
                   WHEN 'col_header'    THEN 2
                   WHEN 'section_label' THEN 3
                   WHEN 'measure'       THEN 4
                   WHEN 'row_label'     THEN 5
                   ELSE 6
               END AS priority
        FROM hypotheses
        WHERE doc_id = {doc_id} AND support > against
    ),
    BEST AS (
        SELECT DISTINCT ON (page_id, row_cluster, cell_id)
               page_id, row_cluster, cell_id,
               hypothesis, detail, net
        FROM PRIORITY
        ORDER BY page_id, row_cluster, cell_id, priority, net DESC
    )
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           NULL AS path,  -- assembled later
           COALESCE(b.hypothesis, 'unresolved') AS role
    FROM cells AS c
    LEFT JOIN BEST AS b USING (page_id, row_cluster, cell_id)
    WHERE c.doc_id = {doc_id}
    """)


def assemble_treemap_paths(con, doc_id=1):
    """Assemble treemap paths from resolved hypotheses.

    path = page_title / scope_l0 / scope_l1 / row_label / col_header
    """
    con.execute(f"""
    UPDATE treemap SET path = sub.path
    FROM (
        WITH
        -- Page title: first chrome cell on each page
        PAGE_TITLES AS (
            SELECT DISTINCT ON (page_id) page_id, c.text AS title
            FROM treemap AS t
            JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
            WHERE t.doc_id = {doc_id} AND t.role = 'chrome'
            ORDER BY page_id, c.y
        ),
        -- Section scope from evidence
        SCOPES AS (
            SELECT page_id, row_cluster, cell_id, value AS scope_l0
            FROM evidence
            WHERE doc_id = {doc_id}
              AND source = 'section' AND key = 'scope_l0'
        ),
        -- Row label: leftmost row_label per row
        ROW_LABELS AS (
            SELECT DISTINCT ON (t.page_id, t.row_cluster)
                   t.page_id, t.row_cluster, c.text AS row_label
            FROM treemap AS t
            JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
            WHERE t.doc_id = {doc_id} AND t.role = 'row_label'
            ORDER BY t.page_id, t.row_cluster, c.x
        ),
        -- Column header per cell (from hypothesis detail = col_id)
        COL_HEADERS AS (
            SELECT h.detail AS col_id,
                   c.text AS col_header, c.page_id
            FROM hypotheses AS h
            JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
            WHERE h.doc_id = {doc_id} AND h.hypothesis = 'col_header'
              AND h.support > h.against
        ),
        PATHS AS (
            SELECT t.page_id, t.row_cluster, t.cell_id,
                   CONCAT_WS(' / ',
                       NULLIF(pt.title, ''),
                       NULLIF(sc.scope_l0, ''),
                       NULLIF(rl.row_label, ''),
                       NULLIF(ch.col_header, '')
                   ) AS path
            FROM treemap AS t
            LEFT JOIN PAGE_TITLES AS pt USING (page_id)
            LEFT JOIN SCOPES AS sc
              ON t.page_id = sc.page_id
             AND t.row_cluster = sc.row_cluster
             AND t.cell_id = sc.cell_id
            LEFT JOIN ROW_LABELS AS rl
              ON t.page_id = rl.page_id
             AND t.row_cluster = rl.row_cluster
            LEFT JOIN evidence AS e_col
              ON t.doc_id = e_col.doc_id AND t.page_id = e_col.page_id
             AND t.row_cluster = e_col.row_cluster
             AND t.cell_id = e_col.cell_id
             AND e_col.source = 'column_align' AND e_col.key = 'col_id'
            LEFT JOIN COL_HEADERS AS ch
              ON t.page_id = ch.page_id AND e_col.value = ch.col_id
            WHERE t.doc_id = {doc_id}
        )
        SELECT page_id, row_cluster, cell_id, path
        FROM PATHS
    ) AS sub
    WHERE treemap.doc_id = {doc_id}
      AND treemap.page_id = sub.page_id
      AND treemap.row_cluster = sub.row_cluster
      AND treemap.cell_id = sub.cell_id
    """)


ALL_HYPOTHESES = [
    ("chrome",         hypothesize_chrome),
    ("column_headers", hypothesize_column_headers),
    ("section_labels", hypothesize_section_labels),
    ("row_labels",     hypothesize_row_labels),
    ("measures",       hypothesize_measures),
]


def run_hypotheses(con, doc_id=1):
    """Generate all hypotheses, resolve, and assemble treemap paths."""
    for name, fn in ALL_HYPOTHESES:
        fn(con, doc_id)

    resolve_hypotheses(con, doc_id)
    assemble_treemap_paths(con, doc_id)


def report_hypotheses(con, doc_id=1):
    """Report hypothesis results."""
    # Role distribution
    roles = con.execute(f"""
        SELECT role, COUNT(*) AS n
        FROM treemap WHERE doc_id = {doc_id}
        GROUP BY role ORDER BY n DESC
    """).fetchdf()
    print(f"\n  Roles:")
    for _, r in roles.iterrows():
        print(f"    {r['role']:<20s} {int(r['n']):5d}")

    # Hypothesis support distribution
    hyp = con.execute(f"""
        SELECT hypothesis, COUNT(*) AS n,
               ROUND(AVG(support - against), 1) AS avg_net
        FROM hypotheses WHERE doc_id = {doc_id}
        GROUP BY hypothesis ORDER BY n DESC
    """).fetchdf()
    print(f"\n  Hypotheses generated:")
    for _, r in hyp.iterrows():
        print(f"    {r['hypothesis']:<20s} {int(r['n']):5d}  avg_net={r['avg_net']}")

    # Sample treemap paths for measures
    paths = con.execute(f"""
        SELECT t.path, c.text
        FROM treemap AS t
        JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
        WHERE t.doc_id = {doc_id}
          AND t.role = 'measure'
          AND t.path IS NOT NULL AND t.path != ''
        LIMIT 8
    """).fetchdf()
    if len(paths) > 0:
        print(f"\n  Treemap paths (measures):")
        for _, r in paths.iterrows():
            print(f"    {r['path']} = {r['text']}")

    # Chrome
    chrome = con.execute(f"""
        SELECT c.text, c.page_id
        FROM treemap AS t
        JOIN cells AS c USING (doc_id, page_id, row_cluster, cell_id)
        WHERE t.doc_id = {doc_id} AND t.role = 'chrome'
        ORDER BY c.page_id, c.y
        LIMIT 8
    """).fetchdf()
    if len(chrome) > 0:
        print(f"\n  Chrome:")
        for _, r in chrome.iterrows():
            print(f"    p{int(r['page_id'])} {repr(r['text'][:50])}")


if __name__ == "__main__":
    import sys
    from pathlib import Path
    sys.path.insert(0, "experiments")
    from evidence_pipeline import init_connection, ingest, ALL_PASSES

    con = init_connection()
    create_hypothesis_tables(con)

    cases = [
        ("CFG_2014_page_161", "test_data/fintabnet/pdf/CFG_2014_page_161.pdf"),
        ("ETR_2004_page_253", "test_data/fintabnet/pdf/ETR_2004_page_253.pdf"),
        ("DVA_2003_page_40",  "test_data/fintabnet/pdf/DVA_2003_page_40.pdf"),
    ]

    for i, (name, path) in enumerate(cases, 1):
        print(f"\n{'='*70}")
        print(f"  {name} (doc_id={i})")
        print(f"{'='*70}")

        ingest(con, path, doc_id=i)
        for _, fn in ALL_PASSES:
            fn(con, doc_id=i)
        run_hypotheses(con, doc_id=i)
        report_hypotheses(con, doc_id=i)

    con.close()
