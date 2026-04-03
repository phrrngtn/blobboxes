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

    CREATE TABLE IF NOT EXISTS constraints (
        name        VARCHAR PRIMARY KEY,
        scope       VARCHAR NOT NULL,    -- 'cell', 'column', 'row', 'page'
        type        VARCHAR NOT NULL,    -- 'uniqueness', 'exclusion', 'implication'
        description VARCHAR
    );

    CREATE TABLE IF NOT EXISTS violations (
        doc_id      INTEGER NOT NULL DEFAULT 1,
        page_id     INTEGER NOT NULL,
        row_cluster INTEGER NOT NULL,
        cell_id     INTEGER NOT NULL,
        hypothesis  VARCHAR NOT NULL,
        constraint_name VARCHAR NOT NULL,
        detail      VARCHAR
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


def seed_constraints(con):
    """Declare structural constraints.

    Each constraint eliminates impossible hypothesis assignments.
    Types:
      uniqueness  — at most N of this hypothesis per scope
      exclusion   — two hypotheses can't coexist on the same cell
      implication — if H1 then H2 must also hold (or not hold)
    """
    constraints = [
        # ── Uniqueness constraints ───────────────────────────────
        ('one_header_per_column',  'column', 'uniqueness',
         'Each putative column has at most one header cell. '
         'Keep the one closest to (immediately above) the data rows.'),

        ('one_role_per_cell',      'cell',   'uniqueness',
         'Each cell has exactly one role. '
         'If multiple hypotheses survive, pick highest net support.'),

        ('one_page_title',         'page',   'uniqueness',
         'Each page has at most one title. '
         'Keep the topmost chrome cell.'),

        # ── Exclusion constraints ────────────────────────────────
        ('measure_not_header',     'cell',   'exclusion',
         'A cell cannot be both a measure and a column header. '
         'Numeric cells subtending columns above data are ambiguous — '
         'prefer measure if support is higher.'),

        ('chrome_not_data',        'cell',   'exclusion',
         'Chrome cells (headers/footers) cannot also be data cells '
         '(measure, row_label, dimension).'),

        ('section_not_in_data',    'row',    'exclusion',
         'Section labels do not appear in data rows. '
         'If a bold leftmost cell is in a row with >= 2 measures, '
         'it is a row label (e.g., "Total Revenue"), not a section label.'),

        # ── Implication constraints ──────────────────────────────
        ('header_implies_column',  'cell',   'implication',
         'A col_header must subtend at least one putative column. '
         'Reject col_header hypothesis if no column alignment evidence.'),

        ('data_rows_contiguous',   'row',    'implication',
         'Data rows within a table should be contiguous. '
         'A non-data row between two data rows is likely a sub-header '
         'or total row, not a break in the table.'),

        ('scope_resets_on_peer',   'page',   'implication',
         'A section label at indent level N resets all deeper scopes. '
         '"Expenses" at L0 clears any L1/L2 scope from the "Revenue" section.'),
    ]
    con.executemany(
        "INSERT OR IGNORE INTO constraints VALUES (?,?,?,?)",
        constraints
    )


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


def apply_constraints(con, doc_id=1):
    """Apply constraints to eliminate invalid hypotheses.

    Each constraint check finds violating hypotheses and records them
    in the violations table. Violated hypotheses are then excluded
    from resolution.
    """

    # ── one_header_per_column: keep best header per column ───────
    # For each (page, col_id), keep the header closest to (just above)
    # the data rows. Eliminate others.
    con.execute(f"""
    INSERT INTO violations
    WITH
    RANKED_HEADERS AS (
        SELECT h.doc_id, h.page_id, h.row_cluster, h.cell_id,
               h.hypothesis, h.detail AS col_id,
               h.support - h.against AS net,
               ROW_NUMBER() OVER (
                   PARTITION BY h.page_id, h.detail
                   ORDER BY h.support - h.against DESC, h.row_cluster DESC
               ) AS rn
        FROM hypotheses AS h
        WHERE h.doc_id = {doc_id} AND h.hypothesis = 'col_header'
          AND h.detail IS NOT NULL
    )
    SELECT doc_id, page_id, row_cluster, cell_id, hypothesis,
           'one_header_per_column' AS constraint_name,
           'displaced by better header for col ' || col_id AS detail
    FROM RANKED_HEADERS
    WHERE rn > 1
    """)

    # ── measure_not_header: if both exist, keep higher net ───────
    con.execute(f"""
    INSERT INTO violations
    WITH
    CONFLICTS AS (
        SELECT m.page_id, m.row_cluster, m.cell_id,
               m.support - m.against AS measure_net,
               h.support - h.against AS header_net
        FROM hypotheses AS m
        JOIN hypotheses AS h
          ON m.doc_id = h.doc_id AND m.page_id = h.page_id
         AND m.row_cluster = h.row_cluster AND m.cell_id = h.cell_id
        WHERE m.doc_id = {doc_id}
          AND m.hypothesis = 'measure' AND h.hypothesis = 'col_header'
    )
    -- Eliminate the loser
    SELECT {doc_id}, page_id, row_cluster, cell_id,
           CASE WHEN measure_net >= header_net
                THEN 'col_header' ELSE 'measure' END AS hypothesis,
           'measure_not_header' AS constraint_name,
           'conflicting measure/header on same cell' AS detail
    FROM CONFLICTS
    """)

    # ── section_not_in_data: bold leftmost in data row = row_label ─
    con.execute(f"""
    INSERT INTO violations
    SELECT {doc_id}, h.page_id, h.row_cluster, h.cell_id,
           'section_label' AS hypothesis,
           'section_not_in_data' AS constraint_name,
           'section_label in a data row — should be row_label' AS detail
    FROM hypotheses AS h
    JOIN cell_probes AS cp
      ON h.doc_id = cp.doc_id AND h.page_id = cp.page_id
     AND h.row_cluster = cp.row_cluster AND h.cell_id = cp.cell_id
    WHERE h.doc_id = {doc_id}
      AND h.hypothesis = 'section_label'
      AND bf_contains(cp.probes, 206::UINTEGER)  -- in_data_rows
    """)

    # ── header_implies_column: reject headers without column evidence ─
    con.execute(f"""
    INSERT INTO violations
    SELECT {doc_id}, h.page_id, h.row_cluster, h.cell_id,
           'col_header' AS hypothesis,
           'header_implies_column' AS constraint_name,
           'col_header without column alignment evidence' AS detail
    FROM hypotheses AS h
    WHERE h.doc_id = {doc_id}
      AND h.hypothesis = 'col_header'
      AND h.detail IS NULL
    """)

    n_violations = con.execute(
        f"SELECT COUNT(*) FROM violations WHERE doc_id = {doc_id}"
    ).fetchone()[0]
    return n_violations


def resolve_hypotheses(con, doc_id=1):
    """Pick the winning hypothesis per cell after constraint elimination.

    1. Apply constraints to find violations
    2. Remove violated hypotheses from consideration
    3. Among remaining: pick by (support - against), break ties by type priority
    """
    n_violations = apply_constraints(con, doc_id)

    con.execute(f"""
    INSERT INTO treemap
    WITH
    SURVIVING AS (
        SELECT h.*, h.support - h.against AS net,
               CASE h.hypothesis
                   WHEN 'chrome'        THEN 1
                   WHEN 'col_header'    THEN 2
                   WHEN 'section_label' THEN 3
                   WHEN 'field_label'   THEN 4
                   WHEN 'field_value'   THEN 5
                   WHEN 'measure'       THEN 6
                   WHEN 'row_label'     THEN 7
                   WHEN 'prose'         THEN 8
                   ELSE 9
               END AS priority
        FROM hypotheses AS h
        WHERE h.doc_id = {doc_id}
          AND h.support > h.against
          -- Exclude violated hypotheses
          AND NOT EXISTS (
              SELECT 1 FROM violations AS v
              WHERE v.doc_id = h.doc_id AND v.page_id = h.page_id
                AND v.row_cluster = h.row_cluster AND v.cell_id = h.cell_id
                AND v.hypothesis = h.hypothesis
          )
    ),
    BEST AS (
        SELECT DISTINCT ON (page_id, row_cluster, cell_id)
               page_id, row_cluster, cell_id,
               hypothesis, detail, net
        FROM SURVIVING
        ORDER BY page_id, row_cluster, cell_id, net DESC, priority
    )
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           NULL AS path,
           COALESCE(b.hypothesis, 'unresolved') AS role
    FROM cells AS c
    LEFT JOIN BEST AS b USING (page_id, row_cluster, cell_id)
    WHERE c.doc_id = {doc_id}
    """)

    return n_violations


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


def hypothesize_key_value_pairs(con, doc_id=1):
    """Hypothesize key-value pairs: 2-cell rows outside table regions.

    Pattern: a row with exactly 2 cells where the left cell is a label
    and the right cell is a value. Common in forms, metadata sections,
    product specifications, header blocks.

    The left cell becomes 'field_label', the right becomes 'field_value'.

    Support: 2-cell row, not in table, leftmost (for label)
    Against: in data rows (that's a table, not a KV pair)
    """
    # First: find 2-cell rows outside table regions
    con.execute(f"""
    INSERT INTO hypotheses
    WITH
    ROW_CELL_COUNTS AS (
        SELECT page_id, row_cluster, COUNT(*) AS n_cells
        FROM cells WHERE doc_id = {doc_id}
        GROUP BY page_id, row_cluster
    ),
    -- Rows with exactly 2 cells
    KV_ROWS AS (
        SELECT rcc.page_id, rcc.row_cluster
        FROM ROW_CELL_COUNTS AS rcc
        WHERE rcc.n_cells = 2
    ),
    -- Tag each cell in a KV row as label (leftmost) or value (other)
    KV_CELLS AS (
        SELECT c.page_id, c.row_cluster, c.cell_id, c.x, c.text,
               ROW_NUMBER() OVER (
                   PARTITION BY c.page_id, c.row_cluster ORDER BY c.x
               ) AS pos
        FROM cells AS c
        JOIN KV_ROWS AS kv USING (page_id, row_cluster)
        WHERE c.doc_id = {doc_id}
    )
    SELECT {doc_id}, kvc.page_id, kvc.row_cluster, kvc.cell_id,
           CASE WHEN kvc.pos = 1 THEN 'field_label'
                ELSE 'field_value' END AS hypothesis,
           -- Detail: for field_value, store the label text as context
           CASE WHEN kvc.pos = 2
                THEN (SELECT kv2.text FROM KV_CELLS AS kv2
                      WHERE kv2.page_id = kvc.page_id
                        AND kv2.row_cluster = kvc.row_cluster
                        AND kv2.pos = 1)
                ELSE NULL END AS detail,
           -- Support: 2-cell row + not in table
           2 + CASE WHEN NOT EXISTS (
               SELECT 1 FROM cell_probes AS cp
               WHERE cp.doc_id = {doc_id}
                 AND cp.page_id = kvc.page_id
                 AND cp.row_cluster = kvc.row_cluster
                 AND cp.cell_id = kvc.cell_id
                 AND bf_contains(cp.probes, 203::UINTEGER)  -- in_table
           ) THEN 1 ELSE 0 END AS support,
           -- Against: in data rows (this is a table row, not KV)
           CASE WHEN EXISTS (
               SELECT 1 FROM cell_probes AS cp
               WHERE cp.doc_id = {doc_id}
                 AND cp.page_id = kvc.page_id
                 AND cp.row_cluster = kvc.row_cluster
                 AND cp.cell_id = kvc.cell_id
                 AND bf_contains(cp.probes, 206::UINTEGER)  -- in_data_rows
           ) THEN 2 ELSE 0 END AS against
    FROM KV_CELLS AS kvc
    """)

    # Also handle rows with 3-4 cells that look like "Label: value unit"
    # or multi-column KV grids — but start simple with 2-cell rows.


def hypothesize_prose(con, doc_id=1):
    """Hypothesize prose: cells not in table regions, not structural.

    Prose is the default for cells that are:
    - Not in a table region
    - Not bold or rare style (those are structural)
    - Not numeric (those are measures)
    - Dominant style (body text)

    This is a low-confidence catch-all — anything unresolved that
    looks like body text.
    """
    con.execute(f"""
    INSERT INTO hypotheses
    SELECT {doc_id}, c.page_id, c.row_cluster, c.cell_id,
           'prose' AS hypothesis,
           NULL AS detail,
           -- Support: dominant style + not in table + not numeric
           (CASE WHEN bf_contains(cp.probes, 102::UINTEGER) THEN 1 ELSE 0 END  -- dominant
            + CASE WHEN NOT bf_contains(cp.probes, 203::UINTEGER) THEN 1 ELSE 0 END  -- not in table
            + CASE WHEN NOT bf_contains(cp.probes, 0::UINTEGER) THEN 1 ELSE 0 END    -- not numeric
           ) AS support,
           -- Against: bold, rare, or numeric
           (CASE WHEN bf_contains(cp.probes, 100::UINTEGER) THEN 1 ELSE 0 END  -- bold
            + CASE WHEN bf_contains(cp.probes, 103::UINTEGER) THEN 1 ELSE 0 END -- rare
            + CASE WHEN bf_contains(cp.probes, 0::UINTEGER) THEN 1 ELSE 0 END   -- numeric
           ) AS against
    FROM cells AS c
    JOIN cell_probes AS cp USING (doc_id, page_id, row_cluster, cell_id)
    WHERE c.doc_id = {doc_id}
      -- Dominant style, not numeric, not bold
      AND bf_contains(cp.probes, 102::UINTEGER)
      AND NOT bf_contains(cp.probes, 0::UINTEGER)
      AND NOT bf_contains(cp.probes, 100::UINTEGER)
      AND LENGTH(TRIM(c.text)) > 1
    """)


ALL_HYPOTHESES = [
    ("chrome",         hypothesize_chrome),
    ("column_headers", hypothesize_column_headers),
    ("section_labels", hypothesize_section_labels),
    ("row_labels",     hypothesize_row_labels),
    ("measures",       hypothesize_measures),
    ("key_value_pairs", hypothesize_key_value_pairs),
    ("prose",           hypothesize_prose),
]


def run_hypotheses(con, doc_id=1):
    """Generate all hypotheses, apply constraints, resolve, assemble paths."""
    for name, fn in ALL_HYPOTHESES:
        fn(con, doc_id)

    n_violations = resolve_hypotheses(con, doc_id)
    assemble_treemap_paths(con, doc_id)
    return n_violations


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

    # Constraint violations
    violations = con.execute(f"""
        SELECT constraint_name, COUNT(*) AS n
        FROM violations WHERE doc_id = {doc_id}
        GROUP BY constraint_name ORDER BY n DESC
    """).fetchdf()
    if len(violations) > 0:
        print(f"\n  Constraint violations:")
        for _, r in violations.iterrows():
            print(f"    {r['constraint_name']:<30s} {int(r['n']):4d} eliminated")

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
    seed_constraints(con)

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
