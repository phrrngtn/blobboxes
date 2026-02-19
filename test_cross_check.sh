#!/usr/bin/env bash
# Cross-check struct vs JSON interfaces across all consumers.
# Requires: built project, .venv with nanobind module, duckdb CLI, test files.
#
# Usage:
#   ./test_cross_check.sh [file.pdf]
#
# Defaults to test_data/sample.pdf if no argument given.
# Exit code 0 = all checks pass, non-zero = failure.

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
PDF="${1:-$DIR/test_data/sample.pdf}"
PDF="$(cd "$(dirname "$PDF")" && pwd)/$(basename "$PDF")"

XLSX="$DIR/test_data/sample.xlsx"
TXT="$DIR/test_data/sample.txt"
DOCX="$DIR/test_data/sample.docx"

PASS=0
FAIL=0

check() {
    local name="$1"; shift
    if "$@"; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

PYTHON="${PYTHON:-$DIR/.venv/bin/python}"

# ─── PDF: Python struct vs JSON ───────────────────────────────────

echo "=== Python PDF: struct vs JSON ==="

check "pdf/doc" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
struct = cur.doc()
cur.close()
j = json.loads(bboxes.doc_json(data))
for k in ['document_id','source_type','checksum','page_count']:
    assert struct[k] == j[k], f'{k}: {struct[k]!r} vs {j[k]!r}'
print(f'    doc matches (pages={struct[\"page_count\"]})')
"

check "pdf/pages" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
structs = cur.pages()
cur.close()
jsons = json.loads(bboxes.pages_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['page_id','document_id','page_number']:
        assert s[k] == j[k], f'page {i} {k}: {s[k]!r} vs {j[k]!r}'
    for k in ['width','height']:
        assert abs(s[k]-j[k]) < 1e-10, f'page {i} {k}: {s[k]} vs {j[k]}'
print(f'    {len(structs)} page rows match')
"

check "pdf/fonts" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
structs = cur.fonts()
cur.close()
jsons = json.loads(bboxes.fonts_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['font_id','name']:
        assert s[k] == j[k], f'font {i} {k}: {s[k]!r} vs {j[k]!r}'
print(f'    {len(structs)} font rows match')
"

check "pdf/styles" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
structs = cur.styles()
cur.close()
jsons = json.loads(bboxes.styles_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['style_id','font_id','weight','color','italic','underline']:
        assert s[k] == j[k], f'style {i} {k}: {s[k]!r} vs {j[k]!r}'
    for k in ['font_size']:
        assert abs(s[k]-j[k]) < 1e-10, f'style {i} {k}: {s[k]} vs {j[k]}'
print(f'    {len(structs)} style rows match')
"

check "pdf/bboxes" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
structs = cur.bboxes()
cur.close()
jsons = json.loads(bboxes.bboxes_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['page_id','style_id','text']:
        assert s[k] == j[k], f'bbox {i} {k}: {s[k]!r} vs {j[k]!r}'
    assert 'formula' not in s, f'bbox {i}: formula should not appear for PDF'
    for k in ['x','y','w','h']:
        assert abs(s[k]-j[k]) < 1e-10, f'bbox {i} {k}: {s[k]} vs {j[k]}'
print(f'    {len(structs)} bbox rows match')
"

# ─── XLSX: Python smoke test ──────────────────────────────────────

echo ""
echo "=== Python XLSX: smoke test ==="

check "xlsx/smoke" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$XLSX','rb').read()
cur = bboxes.open_xlsx(data)
d = cur.doc()
assert d['source_type'] == 'xlsx', f'source_type={d[\"source_type\"]!r}'
assert d['page_count'] >= 1, f'page_count={d[\"page_count\"]}'
pages = cur.pages()
assert len(pages) >= 1
fonts = cur.fonts()
assert len(fonts) >= 1
styles = cur.styles()
assert len(styles) >= 1
boxes = cur.bboxes()
assert len(boxes) >= 1
cur.close()
print(f'    xlsx: {d[\"page_count\"]} pages, {len(fonts)} fonts, {len(styles)} styles, {len(boxes)} bboxes')
"

# ─── Text: Python smoke test ─────────────────────────────────────

echo ""
echo "=== Python Text: smoke test ==="

check "text/smoke" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$TXT','rb').read()
cur = bboxes.open_text(data)
d = cur.doc()
assert d['source_type'] == 'text', f'source_type={d[\"source_type\"]!r}'
assert d['page_count'] == 1
pages = cur.pages()
assert len(pages) == 1
boxes = cur.bboxes()
assert len(boxes) >= 1
cur.close()
print(f'    text: {len(boxes)} bboxes (lines)')
"

# ─── DOCX: Python smoke test ─────────────────────────────────────

echo ""
echo "=== Python DOCX: smoke test ==="

check "docx/smoke" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
data = open('$DOCX','rb').read()
cur = bboxes.open_docx(data)
d = cur.doc()
assert d['source_type'] == 'docx', f'source_type={d[\"source_type\"]!r}'
assert d['page_count'] >= 1
pages = cur.pages()
boxes = cur.bboxes()
assert len(boxes) >= 1
cur.close()
print(f'    docx: {d[\"page_count\"]} tables, {len(boxes)} bboxes (cells)')
"

# ─── DuckDB: PDF table function EXCEPT scalar JSON ───────────────

echo ""
echo "=== DuckDB PDF: table function EXCEPT scalar JSON ==="

DUCKDB_EXT="$DIR/build/duckdb/bboxes.duckdb_extension"
DUCKDB="${DUCKDB:-duckdb}"

check "duckdb/pdf/doc" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT document_id, source_type, checksum, page_count FROM bboxes_doc('$PDF')
    EXCEPT
    SELECT CAST(e->>'document_id' AS INTEGER), e->>'source_type',
           e->>'checksum', CAST(e->>'page_count' AS INTEGER)
    FROM (SELECT bboxes_doc_json('$PDF') as e)
);
"

check "duckdb/pdf/pages" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM bboxes_pages('$PDF')
    EXCEPT
    SELECT CAST(e->>'page_id' AS INTEGER), CAST(e->>'document_id' AS INTEGER),
           CAST(e->>'page_number' AS INTEGER),
           CAST(e->>'width' AS DOUBLE), CAST(e->>'height' AS DOUBLE)
    FROM (SELECT unnest(bboxes_pages_json('$PDF')::JSON[]) as e)
);
"

check "duckdb/pdf/fonts" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM bboxes_fonts('$PDF')
    EXCEPT
    SELECT CAST(e->>'font_id' AS INTEGER), e->>'name'
    FROM (SELECT unnest(bboxes_fonts_json('$PDF')::JSON[]) as e)
);
"

check "duckdb/pdf/styles" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM bboxes_styles('$PDF')
    EXCEPT
    SELECT CAST(e->>'style_id' AS INTEGER), CAST(e->>'font_id' AS INTEGER),
           CAST(e->>'font_size' AS DOUBLE), e->>'color', e->>'weight',
           CAST(e->>'italic' AS INTEGER), CAST(e->>'underline' AS INTEGER)
    FROM (SELECT unnest(bboxes_styles_json('$PDF')::JSON[]) as e)
);
"

check "duckdb/pdf/bboxes" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT page_id, style_id, x, y, w, h, text FROM bboxes('$PDF')
    EXCEPT
    SELECT CAST(e->>'page_id' AS INTEGER),
           CAST(e->>'style_id' AS INTEGER),
           CAST(e->>'x' AS DOUBLE), CAST(e->>'y' AS DOUBLE),
           CAST(e->>'w' AS DOUBLE), CAST(e->>'h' AS DOUBLE),
           e->>'text'
    FROM (SELECT unnest(bboxes_json('$PDF')::JSON[]) as e)
);
"

# ─── DuckDB: XLSX smoke test ─────────────────────────────────────

echo ""
echo "=== DuckDB XLSX: smoke test ==="

check "duckdb/xlsx/bboxes" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) > 0 THEN 'ok (' || count(*) || ' rows)' ELSE 'EMPTY' END
FROM bboxes_xlsx('$XLSX');
"

# ─── DuckDB: Text smoke test ─────────────────────────────────────

echo ""
echo "=== DuckDB Text: smoke test ==="

check "duckdb/text/bboxes" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) > 0 THEN 'ok (' || count(*) || ' rows)' ELSE 'EMPTY' END
FROM bboxes_text('$TXT');
"

# ─── DuckDB: DOCX smoke test ─────────────────────────────────────

echo ""
echo "=== DuckDB DOCX: smoke test ==="

check "duckdb/docx/bboxes" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) > 0 THEN 'ok (' || count(*) || ' rows)' ELSE 'EMPTY' END
FROM bboxes_docx('$DOCX');
"

# ─── SQLite: check if extension loading is available ─────────────

SQLITE_EXT="$DIR/build/sqlite/bboxes"
CAN_LOAD_EXT=$("$PYTHON" -c "
import sqlite3
try:
    db = sqlite3.connect(':memory:')
    db.enable_load_extension(True)
    db.load_extension('$SQLITE_EXT')
    print('yes')
except (AttributeError, Exception):
    print('no')
" 2>/dev/null)

if [ "$CAN_LOAD_EXT" != "yes" ]; then
    echo ""
    echo "=== SQLite: skipping (extension loading not available) ==="
else

# ─── SQLite: PDF virtual table EXCEPT scalar JSON ────────────────

echo ""
echo "=== SQLite PDF: virtual table EXCEPT scalar JSON ==="

check "sqlite/pdf/doc" "$PYTHON" -c "
import sqlite3, json
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT document_id, source_type, checksum, page_count FROM bboxes_doc('$PDF')
    EXCEPT
    SELECT json_extract(bboxes_doc_json('$PDF'),'\$.document_id'),
           json_extract(bboxes_doc_json('$PDF'),'\$.source_type'),
           json_extract(bboxes_doc_json('$PDF'),'\$.checksum'),
           json_extract(bboxes_doc_json('$PDF'),'\$.page_count')
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "sqlite/pdf/pages" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM bboxes_pages('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.page_id'), json_extract(value,'\$.document_id'),
           json_extract(value,'\$.page_number'),
           json_extract(value,'\$.width'), json_extract(value,'\$.height')
    FROM json_each(bboxes_pages_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "sqlite/pdf/fonts" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM bboxes_fonts('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.font_id'), json_extract(value,'\$.name')
    FROM json_each(bboxes_fonts_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "sqlite/pdf/styles" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM bboxes_styles('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.style_id'), json_extract(value,'\$.font_id'),
           json_extract(value,'\$.font_size'), json_extract(value,'\$.color'),
           json_extract(value,'\$.weight'), json_extract(value,'\$.italic'),
           json_extract(value,'\$.underline')
    FROM json_each(bboxes_styles_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "sqlite/pdf/bboxes" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT page_id, style_id, x, y, w, h, text FROM bboxes('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.page_id'),
           json_extract(value,'\$.style_id'),
           json_extract(value,'\$.x'), json_extract(value,'\$.y'),
           json_extract(value,'\$.w'), json_extract(value,'\$.h'),
           json_extract(value,'\$.text')
    FROM json_each(bboxes_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

# ─── SQLite: XLSX smoke test ─────────────────────────────────────

echo ""
echo "=== SQLite XLSX: smoke test ==="

check "sqlite/xlsx/bboxes" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute(\"SELECT count(*) FROM bboxes_xlsx('$XLSX')\").fetchone()
assert rows[0] > 0, f'expected rows, got {rows[0]}'
print(f'    {rows[0]} xlsx bbox rows')
"

# ─── SQLite: Text smoke test ─────────────────────────────────────

echo ""
echo "=== SQLite Text: smoke test ==="

check "sqlite/text/bboxes" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute(\"SELECT count(*) FROM bboxes_text('$TXT')\").fetchone()
assert rows[0] > 0, f'expected rows, got {rows[0]}'
print(f'    {rows[0]} text bbox rows')
"

# ─── SQLite: DOCX smoke test ─────────────────────────────────────

echo ""
echo "=== SQLite DOCX: smoke test ==="

check "sqlite/docx/bboxes" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute(\"SELECT count(*) FROM bboxes_docx('$DOCX')\").fetchone()
assert rows[0] > 0, f'expected rows, got {rows[0]}'
print(f'    {rows[0]} docx bbox rows')
"

fi  # CAN_LOAD_EXT (end of SQLite vtab/scalar tests)

# ─── Auto-detect: Python ────────────────────────────────────────

echo ""
echo "=== Auto-detect: Python ==="

check "python/auto/pdf" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
with open('$PDF', 'rb') as f: data = f.read()
cur = bboxes.open(data)
doc = cur.doc()
assert doc['source_type'] == 'pdf', f'expected pdf, got {doc[\"source_type\"]}'
assert doc['checksum'], 'missing checksum'
assert len(cur.bboxes()) > 0
cur.close()
print(f'    PDF auto-detect OK, {doc[\"page_count\"]} pages')
"

check "python/auto/xlsx" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
with open('$XLSX', 'rb') as f: data = f.read()
cur = bboxes.open(data)
doc = cur.doc()
assert doc['source_type'] == 'xlsx', f'expected xlsx, got {doc[\"source_type\"]}'
cur.close()
print(f'    XLSX auto-detect OK')
"

check "python/detect" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
with open('$PDF', 'rb') as f: pdf_data = f.read()
with open('$XLSX', 'rb') as f: xlsx_data = f.read()
with open('$TXT', 'rb') as f: txt_data = f.read()
assert bboxes.detect(pdf_data) == 'pdf'
assert bboxes.detect(xlsx_data) == 'xlsx'
assert bboxes.detect(txt_data) == 'text'
print('    detect() OK')
"

check "python/info" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import blobboxes as bboxes
with open('$PDF', 'rb') as f: data = f.read()
i = bboxes.info(data)
assert i['source_type'] == 'pdf'
assert i['checksum']
assert i['page_count'] >= 1
print(f'    info() OK: {i[\"source_type\"]}, {i[\"page_count\"]} pages, checksum={i[\"checksum\"][:8]}...')
"

# ─── Auto-detect: DuckDB bboxes_info ────────────────────────────

echo ""
echo "=== Auto-detect: DuckDB ==="

check "duckdb/bboxes_info" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE
    WHEN bboxes_info('$PDF')::JSON ->> 'source_type' = 'pdf'
    THEN 'ok'
    ELSE 'FAIL'
END;
"

# ─── Auto-detect: SQLite bboxes_info ────────────────────────────

if [ "$CAN_LOAD_EXT" = "yes" ]; then

echo ""
echo "=== Auto-detect: SQLite ==="

check "sqlite/bboxes_info" "$PYTHON" -c "
import sys; sys.path.insert(0, '$DIR/python')
import sqlite3, json
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
result = db.execute(\"SELECT bboxes_info('$PDF')\").fetchone()[0]
info = json.loads(result)
assert info['source_type'] == 'pdf', f'expected pdf, got {info[\"source_type\"]}'
assert info['checksum'], 'missing checksum'
print(f'    bboxes_info OK: {info[\"source_type\"]}, {info[\"page_count\"]} pages')
"

fi  # CAN_LOAD_EXT

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
