"""Render document with interactive role toggle buttons."""
import sys
from pathlib import Path
sys.path.insert(0, "experiments")
from evidence_pipeline import init_connection, ingest, ALL_PASSES
from hypotheses import (create_hypothesis_tables, seed_constraints,
                         seed_roles, ALL_HYPOTHESES, resolve_hypotheses,
                         assemble_treemap_paths)

con = init_connection()
create_hypothesis_tables(con)
seed_roles(con)
seed_constraints(con)

pdf = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/paulharrington/Downloads/EPD document_EPD-IES-0021562_002_en.pdf"
name = Path(pdf).stem

ingest(con, pdf, doc_id=1)
for _, fn in ALL_PASSES:
    fn(con, doc_id=1)
for _, fn in ALL_HYPOTHESES:
    fn(con, doc_id=1)
resolve_hypotheses(con, doc_id=1)
assemble_treemap_paths(con, doc_id=1)

# Get roles with colors
roles_df = con.execute("SELECT role, priority, description, color FROM roles ORDER BY priority").fetchdf()

ROLE_CSS = {
    'chrome':        '#cc6666',
    'col_header':    '#00cccc',
    'section_label': '#cccc00',
    'field_label':   '#cc66cc',
    'field_value':   '#ee88ee',
    'measure':       '#44cc44',
    'row_label':     '#6688cc',
    'prose':         '#cccccc',
    'unresolved':    '#666666',
}

# Get all cells with roles
cells = con.execute("""
    SELECT c.page_id, c.row_cluster, c.cell_id,
           c.x, c.y, c.w, c.h, c.text,
           t.role, t.path
    FROM cells AS c
    JOIN treemap AS t USING (doc_id, page_id, row_cluster, cell_id)
    WHERE c.doc_id = 1
    ORDER BY c.page_id, c.y, c.x
""").fetchdf()

n_pages = cells['page_id'].nunique()
role_counts = cells['role'].value_counts().to_dict()

# Build HTML
html_parts = []
html_parts.append(f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>{name}</title>
<style>
body {{
    font-family: 'SF Mono', 'Menlo', 'Monaco', monospace;
    font-size: 11px;
    background: #1a1a2e;
    color: #eee;
    margin: 20px;
    line-height: 1.6;
}}
h1 {{ color: #eee; font-size: 16px; }}
.stats {{ color: #888; margin-bottom: 10px; }}
.legend {{
    position: sticky; top: 0; z-index: 100;
    background: #1a1a2e; padding: 10px 0;
    border-bottom: 1px solid #333;
    margin-bottom: 10px;
}}
.legend-btn {{
    display: inline-block;
    padding: 4px 12px;
    margin: 2px 4px;
    border-radius: 4px;
    cursor: pointer;
    font-family: inherit;
    font-size: 11px;
    border: 2px solid;
    transition: opacity 0.2s;
}}
.legend-btn.hidden {{
    opacity: 0.3;
    text-decoration: line-through;
}}
.page-header {{
    color: #888;
    border-top: 1px solid #333;
    margin-top: 15px;
    padding-top: 5px;
    font-weight: bold;
}}
.row {{ white-space: nowrap; }}
.cell {{
    display: inline;
    padding: 0 2px;
    border-radius: 2px;
    cursor: default;
}}
.cell:hover {{
    outline: 1px solid #fff;
}}
""")

# CSS classes per role
for role, color in ROLE_CSS.items():
    html_parts.append(f"""
.role-{role} {{ color: {color}; }}
.role-{role}.bg {{ background: {color}22; }}
""")

html_parts.append("""
.cell-hidden { display: none; }
</style>
</head>
<body>
""")

html_parts.append(f"<h1>{name}</h1>")
html_parts.append(f'<div class="stats">{len(cells)} cells, {n_pages} pages</div>')

# Legend with toggle buttons
html_parts.append('<div class="legend">')
for _, r in roles_df.iterrows():
    role = r['role']
    color = ROLE_CSS.get(role, '#888')
    count = role_counts.get(role, 0)
    html_parts.append(
        f'<span class="legend-btn" id="btn-{role}" '
        f'style="color:{color}; border-color:{color};" '
        f'onclick="toggleRole(\'{role}\')">'
        f'{role} ({count})</span>'
    )
html_parts.append('</div>')

# Pages
for page_id in sorted(cells['page_id'].unique()):
    page_cells = cells[cells['page_id'] == page_id]
    html_parts.append(
        f'<div class="page-header">Page {page_id} '
        f'({len(page_cells)} cells)</div>'
    )

    for rc in sorted(page_cells['row_cluster'].unique()):
        row = page_cells[page_cells['row_cluster'] == rc].sort_values('x')
        html_parts.append('<div class="row">')
        for _, c in row.iterrows():
            role = c['role']
            text = str(c['text']) if c['text'] is not None else ''
            text = text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
            title = f"{role}"
            path = c['path'] if isinstance(c['path'], str) else ''
            if path:
                title += f"\n{path}"
            title = title.replace('"', '&quot;')
            html_parts.append(
                f'<span class="cell bg role-{role}" '
                f'data-role="{role}" title="{title}">'
                f'{text}</span> '
            )
        html_parts.append('</div>')

# JavaScript for toggle
html_parts.append("""
<script>
const hiddenRoles = new Set();

function toggleRole(role) {
    const btn = document.getElementById('btn-' + role);
    if (hiddenRoles.has(role)) {
        hiddenRoles.delete(role);
        btn.classList.remove('hidden');
    } else {
        hiddenRoles.add(role);
        btn.classList.add('hidden');
    }
    document.querySelectorAll('.cell').forEach(el => {
        const r = el.getAttribute('data-role');
        el.classList.toggle('cell-hidden', hiddenRoles.has(r));
    });
}
</script>
</body>
</html>
""")

html = ''.join(html_parts)
out = f"/tmp/{name}_roles.html"
with open(out, 'w') as f:
    f.write(html)

import subprocess
subprocess.run(["open", "-a", "Safari", out])
print(f"Opened {out}")
