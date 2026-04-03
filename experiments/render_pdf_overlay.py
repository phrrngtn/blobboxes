"""Render PDF with translucent role annotations overlaid via PDF.js."""
import sys, json, base64
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

pdf_path = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/paulharrington/Downloads/EPD document_EPD-IES-0021562_002_en.pdf"
name = Path(pdf_path).stem

ingest(con, pdf_path, doc_id=1)
for _, fn in ALL_PASSES:
    fn(con, doc_id=1)
for _, fn in ALL_HYPOTHESES:
    fn(con, doc_id=1)
resolve_hypotheses(con, doc_id=1)
assemble_treemap_paths(con, doc_id=1)

# Get page dimensions
pages = con.execute("""
    SELECT page_id, width, height
    FROM bb_pages(?)
    ORDER BY page_id
""", [pdf_path]).fetchdf()

# Get cells with roles and coordinates
cells = con.execute("""
    SELECT c.page_id, c.x, c.y, c.w, c.h, c.text, t.role
    FROM cells AS c
    JOIN treemap AS t USING (doc_id, page_id, row_cluster, cell_id)
    WHERE c.doc_id = 1
    ORDER BY c.page_id, c.y, c.x
""").fetchdf()

# Get role colors
role_colors = dict(con.execute("SELECT role, color FROM roles").fetchall())

# Map rich color names to CSS rgba
CSS_COLORS = {
    'dim red':         'rgba(200, 80, 80, 0.3)',
    'bold cyan':       'rgba(0, 220, 220, 0.3)',
    'bold yellow':     'rgba(220, 220, 0, 0.3)',
    'magenta':         'rgba(200, 100, 200, 0.3)',
    'bright_magenta':  'rgba(240, 130, 240, 0.3)',
    'green':           'rgba(60, 200, 60, 0.3)',
    'blue':            'rgba(100, 130, 220, 0.3)',
    'white':           'rgba(200, 200, 200, 0.15)',
    'dim white':       'rgba(100, 100, 100, 0.2)',
}

BORDER_COLORS = {
    'dim red':         'rgba(200, 80, 80, 0.6)',
    'bold cyan':       'rgba(0, 220, 220, 0.6)',
    'bold yellow':     'rgba(220, 220, 0, 0.6)',
    'magenta':         'rgba(200, 100, 200, 0.6)',
    'bright_magenta':  'rgba(240, 130, 240, 0.6)',
    'green':           'rgba(60, 200, 60, 0.6)',
    'blue':            'rgba(100, 130, 220, 0.6)',
    'white':           'rgba(200, 200, 200, 0.3)',
    'dim white':       'rgba(100, 100, 100, 0.4)',
}

# Build annotations JSON per page
annotations = {}
for _, c in cells.iterrows():
    pid = int(c['page_id'])
    if pid not in annotations:
        annotations[pid] = []
    rich_color = role_colors.get(c['role'], 'dim white')
    annotations[pid].append({
        'x': float(c['x']),
        'y': float(c['y']),
        'w': float(c['w']),
        'h': float(c['h']),
        'text': str(c['text'])[:80] if c['text'] else '',
        'role': c['role'],
        'fill': CSS_COLORS.get(rich_color, 'rgba(100,100,100,0.2)'),
        'border': BORDER_COLORS.get(rich_color, 'rgba(100,100,100,0.4)'),
    })

# Encode PDF as base64 data URI
with open(pdf_path, 'rb') as f:
    pdf_b64 = base64.b64encode(f.read()).decode('ascii')

# Role counts for legend
role_counts = cells['role'].value_counts().to_dict()

# Get page dimensions as JSON
page_dims = {}
for _, p in pages.iterrows():
    page_dims[int(p['page_id'])] = {
        'width': float(p['width']),
        'height': float(p['height'])
    }

html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>{name} — Role Annotations</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/pdf.js/4.0.379/pdf.min.mjs" type="module"></script>
<style>
body {{
    font-family: -apple-system, 'Helvetica Neue', sans-serif;
    background: #2a2a3e;
    color: #eee;
    margin: 0;
    padding: 20px;
}}
h1 {{ font-size: 16px; margin: 0 0 10px; }}
.stats {{ color: #888; font-size: 12px; margin-bottom: 10px; }}
.legend {{
    position: sticky; top: 0; z-index: 200;
    background: #2a2a3e; padding: 8px 0;
    border-bottom: 1px solid #444;
    margin-bottom: 15px;
}}
.legend-btn {{
    display: inline-block;
    padding: 4px 12px;
    margin: 2px 4px;
    border-radius: 4px;
    cursor: pointer;
    font-size: 12px;
    border: 2px solid;
    font-family: inherit;
    transition: opacity 0.2s;
    color: #eee;
}}
.legend-btn.hidden {{
    opacity: 0.25;
    text-decoration: line-through;
}}
.page-container {{
    position: relative;
    display: inline-block;
    margin: 10px 0;
    border: 1px solid #444;
}}
canvas {{
    display: block;
}}
.annotation-layer {{
    position: absolute;
    top: 0; left: 0;
    pointer-events: none;
}}
.anno {{
    position: absolute;
    pointer-events: auto;
    cursor: default;
    box-sizing: border-box;
    transition: opacity 0.2s;
}}
.anno:hover {{
    outline: 2px solid #fff;
    z-index: 100;
}}
.anno-hidden {{
    display: none;
}}
.page-label {{
    color: #888;
    font-size: 12px;
    margin: 15px 0 5px;
}}
</style>
</head>
<body>

<h1>{name}</h1>
<div class="stats">{len(cells)} cells across {cells['page_id'].nunique()} pages</div>

<div class="legend" id="legend"></div>
<div id="pages"></div>

<script type="module">
import * as pdfjsLib from 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/4.0.379/pdf.min.mjs';

pdfjsLib.GlobalWorkerOptions.workerSrc =
    'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/4.0.379/pdf.worker.min.mjs';

const PDF_DATA = atob("{pdf_b64}");
const ANNOTATIONS = {json.dumps(annotations)};
const PAGE_DIMS = {json.dumps(page_dims)};
const ROLE_COUNTS = {json.dumps(role_counts)};

const ROLE_COLORS = {json.dumps({
    role: CSS_COLORS.get(role_colors.get(role, 'dim white'), 'rgba(100,100,100,0.2)')
    for role in role_counts
})};
const ROLE_BORDERS = {json.dumps({
    role: BORDER_COLORS.get(role_colors.get(role, 'dim white'), 'rgba(100,100,100,0.4)')
    for role in role_counts
})};

const ROLE_ORDER = ['chrome','col_header','section_label','field_label',
                    'field_value','measure','row_label','prose','unresolved'];

// Hidden roles state
const hiddenRoles = new Set();

// Build legend
const legend = document.getElementById('legend');
for (const role of ROLE_ORDER) {{
    const count = ROLE_COUNTS[role] || 0;
    if (count === 0) continue;
    const btn = document.createElement('span');
    btn.className = 'legend-btn';
    btn.id = 'btn-' + role;
    btn.style.borderColor = ROLE_BORDERS[role] || '#666';
    btn.style.background = ROLE_COLORS[role] || 'rgba(100,100,100,0.2)';
    btn.textContent = role + ' (' + count + ')';
    btn.onclick = () => toggleRole(role);
    legend.appendChild(btn);
}}

function toggleRole(role) {{
    const btn = document.getElementById('btn-' + role);
    if (hiddenRoles.has(role)) {{
        hiddenRoles.delete(role);
        btn.classList.remove('hidden');
    }} else {{
        hiddenRoles.add(role);
        btn.classList.add('hidden');
    }}
    document.querySelectorAll('.anno').forEach(el => {{
        el.classList.toggle('anno-hidden', hiddenRoles.has(el.dataset.role));
    }});
}}

// Load and render PDF
const pdfData = new Uint8Array(PDF_DATA.length);
for (let i = 0; i < PDF_DATA.length; i++) pdfData[i] = PDF_DATA.charCodeAt(i);

const pdf = await pdfjsLib.getDocument({{data: pdfData}}).promise;
const pagesDiv = document.getElementById('pages');
const SCALE = 1.5;

for (let pageNum = 1; pageNum <= pdf.numPages; pageNum++) {{
    const page = await pdf.getPage(pageNum);
    const viewport = page.getViewport({{scale: SCALE}});
    const pageIdx = pageNum - 1;

    const label = document.createElement('div');
    label.className = 'page-label';
    label.textContent = 'Page ' + pageIdx;
    pagesDiv.appendChild(label);

    const container = document.createElement('div');
    container.className = 'page-container';
    container.style.width = viewport.width + 'px';
    container.style.height = viewport.height + 'px';

    const canvas = document.createElement('canvas');
    canvas.width = viewport.width;
    canvas.height = viewport.height;
    container.appendChild(canvas);

    // Render PDF page
    const ctx = canvas.getContext('2d');
    await page.render({{canvasContext: ctx, viewport: viewport}}).promise;

    // Overlay annotations
    const annoLayer = document.createElement('div');
    annoLayer.className = 'annotation-layer';
    annoLayer.style.width = viewport.width + 'px';
    annoLayer.style.height = viewport.height + 'px';

    const annos = ANNOTATIONS[pageIdx] || [];
    const dims = PAGE_DIMS[pageIdx] || {{width: 612, height: 792}};

    for (const a of annos) {{
        const el = document.createElement('div');
        el.className = 'anno';
        el.dataset.role = a.role;

        // Convert PDF coordinates to screen coordinates
        // PDF origin is bottom-left; blobboxes may use top-left
        // PDF.js viewport handles the transform
        const sx = (a.x / dims.width) * viewport.width;
        const sy = (a.y / dims.height) * viewport.height;
        const sw = (a.w / dims.width) * viewport.width;
        const sh = (a.h / dims.height) * viewport.height;

        el.style.left = sx + 'px';
        el.style.top = sy + 'px';
        el.style.width = Math.max(sw, 2) + 'px';
        el.style.height = Math.max(sh, 2) + 'px';
        el.style.background = a.fill;
        el.style.border = '1px solid ' + a.border;
        el.title = a.role + ': ' + a.text;

        if (hiddenRoles.has(a.role)) el.classList.add('anno-hidden');
        annoLayer.appendChild(el);
    }}

    container.appendChild(annoLayer);
    pagesDiv.appendChild(container);
}}
</script>
</body>
</html>"""

out = f"/tmp/{name}_overlay.html"
with open(out, 'w') as f:
    f.write(html)

import subprocess
subprocess.run(["open", "-a", "Safari", out])
print(f"Opened {out}")
