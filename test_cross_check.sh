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

echo "=== Python: struct vs JSON ==="

check "extract" "$DIR/.venv/bin/python" -c "
import json, sys; sys.path.insert(0, '$DIR')
import pdf_bboxes
data = open('$PDF','rb').read()
structs = list(pdf_bboxes.extract(data))
jsons   = json.loads(pdf_bboxes.extract_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['font_id','page','text','color','style']:
        assert s[k] == j[k], f'row {i} {k}: {s[k]!r} vs {j[k]!r}'
    for k in ['x','y','w','h','font_size']:
        assert abs(s[k]-j[k]) < 1e-10, f'row {i} {k}: {s[k]} vs {j[k]}'
print(f'    {len(structs)} extract rows match')
"

check "fonts" "$DIR/.venv/bin/python" -c "
import json, sys; sys.path.insert(0, '$DIR')
import pdf_bboxes
data = open('$PDF','rb').read()
structs = list(pdf_bboxes.fonts(data))
jsons   = json.loads(pdf_bboxes.fonts_json(data))
assert len(structs) == len(jsons), f'{len(structs)} vs {len(jsons)}'
for i,(s,j) in enumerate(zip(structs, jsons)):
    for k in ['font_id','name','flags','style']:
        assert s[k] == j[k], f'font {i} {k}: {s[k]!r} vs {j[k]!r}'
print(f'    {len(structs)} font rows match')
"

echo ""
echo "=== DuckDB: table function EXCEPT scalar JSON ==="

DUCKDB_EXT="$DIR/build/duckdb/pdf_bboxes.duckdb_extension"

check "extract" duckdb -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM pdf_extract('$PDF')
    EXCEPT
    SELECT CAST(e->>'font_id' AS INTEGER), CAST(e->>'page' AS INTEGER),
           CAST(e->>'x' AS DOUBLE), CAST(e->>'y' AS DOUBLE),
           CAST(e->>'w' AS DOUBLE), CAST(e->>'h' AS DOUBLE),
           e->>'text', e->>'color',
           CAST(e->>'font_size' AS DOUBLE), e->>'style'
    FROM (SELECT unnest(pdf_extract_json('$PDF')::JSON[]) as e)
);
"

check "fonts" duckdb -unsigned -c "
LOAD '$DUCKDB_EXT';
SELECT CASE WHEN count(*) = 0 THEN 'ok' ELSE 'MISMATCH' END FROM (
    SELECT * FROM pdf_fonts('$PDF')
    EXCEPT
    SELECT CAST(e->>'font_id' AS INTEGER), e->>'name',
           CAST(e->>'flags' AS INTEGER), e->>'style'
    FROM (SELECT unnest(pdf_fonts_json('$PDF')::JSON[]) as e)
);
"

echo ""
echo "=== SQLite: virtual table EXCEPT scalar JSON ==="

SQLITE_EXT="$DIR/build/sqlite/pdf_bboxes"

check "extract" "$DIR/.venv/bin/python" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM pdf_extract('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.font_id'), json_extract(value,'\$.page'),
           json_extract(value,'\$.x'), json_extract(value,'\$.y'),
           json_extract(value,'\$.w'), json_extract(value,'\$.h'),
           json_extract(value,'\$.text'), json_extract(value,'\$.color'),
           json_extract(value,'\$.font_size'), json_extract(value,'\$.style')
    FROM json_each(pdf_extract_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

check "fonts" "$DIR/.venv/bin/python" -c "
import sqlite3
db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension('$SQLITE_EXT')
rows = db.execute('''
    SELECT * FROM pdf_fonts('$PDF')
    EXCEPT
    SELECT json_extract(value,'\$.font_id'), json_extract(value,'\$.name'),
           json_extract(value,'\$.flags'), json_extract(value,'\$.style')
    FROM json_each(pdf_fonts_json('$PDF'))
''').fetchall()
assert len(rows) == 0, f'{len(rows)} mismatched rows'
"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
