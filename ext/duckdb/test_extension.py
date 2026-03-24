"""Smoke test: load the blobboxes DuckDB extension and verify functions exist."""
import duckdb
from blobboxes_duckdb import extension_path

EXPECTED_FUNCTIONS = [
    "bb_pdf",
    "bb_pdf_doc",
    "bb_pdf_pages",
    "bb_pdf_fonts",
    "bb_pdf_styles",
    "bb_pdf_json",
    "bb_info",
]

conn = duckdb.connect()
conn.execute("SET allow_unsigned_extensions = true")
conn.execute(f"LOAD '{extension_path()}'")

# Query duckdb_functions() to verify our functions are registered
registered = set(
    row[0]
    for row in conn.execute(
        "SELECT DISTINCT function_name FROM duckdb_functions() WHERE function_name LIKE 'bb_%'"
    ).fetchall()
)

missing = [f for f in EXPECTED_FUNCTIONS if f not in registered]
if missing:
    raise AssertionError(f"Missing functions: {missing}")

print(f"OK: {len(registered)} bb_* functions registered, all {len(EXPECTED_FUNCTIONS)} expected functions present")
