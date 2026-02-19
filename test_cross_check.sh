#!/usr/bin/env bash
# Cross-check struct vs JSON interfaces across all consumers.
# Requires: built project, .venv with nanobind module, duckdb CLI, a test PDF.
#
# Usage:
#   ./test_cross_check.sh [file.pdf]
#
# Defaults to glavel_receipt.pdf if no argument given.
# Exit code 0 = all checks pass, non-zero = failure.

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
PDF="${1:-$DIR/glavel_receipt.pdf}"
PDF="$(cd "$(dirname "$PDF")" && pwd)/$(basename "$PDF")"

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

echo "=== Python: struct vs JSON ==="

check "doc" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR')
import bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
struct = cur.doc()
cur.close()
j = json.loads(bboxes.doc_json(data))
for k in ['document_id','source_type','page_count']:
    assert struct[k] == j[k], f'{k}: {struct[k]!r} vs {j[k]!r}'
print(f'    doc matches (pages={struct[\"page_count\"]})')
"

check "pages" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR')
import bboxes
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

check "fonts" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR')
import bboxes
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

check "styles" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR')
import bboxes
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

check "bboxes" "$PYTHON" -c "
import json, sys; sys.path.insert(0, '$DIR')
import bboxes
data = open('$PDF','rb').read()
cur = bboxes.open_pdf(data)
structs = cur.bboxes()
cur.close()
jsons = json.loads(bboxes.bboxes_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['bbox_id','page_id','style_id','text']:
        assert s[k] == j[k], f'bbox {i} {k}: {s[k]!r} vs {j[k]!r}'
    for k in ['x','y','w','h']:
        assert abs(s[k]-j[k]) < 1e-10, f'bbox {i} {k}: {s[k]} vs {j[k]}'
print(f'    {len(structs)} bbox rows match')
"

echo ""
echo "=== DuckDB: table function EXCEPT scalar JSON ==="

DUCKDB_EXT="$DIR/build/duckdb/bboxes.duckdb_extension"
DUCKDB="${DUCKDB:-duckdb}"

check "doc" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT document_id, source_type, page_count FROM bboxes_doc('$PDF')
    EXCEPT
    SELECT CAST(e->>'document_id' AS INTEGER), e->>'source_type',
           CAST(e->>'page_count' AS INTEGER)
    FROM (SELECT bboxes_doc_json('$PDF') as e)
);
"

check "pages" "$DUCKDB" -unsigned -c "
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

check "fonts" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM bboxes_fonts('$PDF')
    EXCEPT
    SELECT CAST(e->>'font_id' AS INTEGER), e->>'name'
    FROM (SELECT unnest(bboxes_fonts_json('$PDF')::JSON[]) as e)
);
"

check "styles" "$DUCKDB" -unsigned -c "
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

check "bboxes" "$DUCKDB" -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM bboxes('$PDF')
    EXCEPT
    SELECT CAST(e->>'bbox_id' AS INTEGER), CAST(e->>'page_id' AS INTEGER),
           CAST(e->>'style_id' AS INTEGER),
           CAST(e->>'x' AS DOUBLE), CAST(e->>'y' AS DOUBLE),
           CAST(e->>'w' AS DOUBLE), CAST(e->>'h' AS DOUBLE),
           e->>'text'
    FROM (SELECT unnest(bboxes_json('$PDF')::JSON[]) as e)
);
"

echo ""
echo "=== SQLite: virtual table EXCEPT scalar JSON ==="

SQLITE_EXT="$DIR/build/sqlite/bboxes"

check "doc" "$PYTHON" -c "
import sqlite3, json
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT document_id, source_type, page_count FROM bboxes_doc('$PDF')
    EXCEPT
    SELECT json_extract(bboxes_doc_json('$PDF'),'\$.document_id'),
           json_extract(bboxes_doc_json('$PDF'),'\$.source_type'),
           json_extract(bboxes_doc_json('$PDF'),'\$.page_count')
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "pages" "$PYTHON" -c "
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

check "fonts" "$PYTHON" -c "
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

check "styles" "$PYTHON" -c "
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

check "bboxes" "$PYTHON" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM bboxes('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.bbox_id'), json_extract(value,'\$.page_id'),
           json_extract(value,'\$.style_id'),
           json_extract(value,'\$.x'), json_extract(value,'\$.y'),
           json_extract(value,'\$.w'), json_extract(value,'\$.h'),
           json_extract(value,'\$.text')
    FROM json_each(bboxes_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
