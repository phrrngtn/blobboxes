"""Smoke test: load the blobboxes SQLite extension and verify functions exist."""
import sqlite3
from blobboxes_sqlite import extension_path

EXPECTED_FUNCTIONS = [
    "bb_pdf_json",
    "bb_pdf_doc_json",
    "bb_pdf_pages_json",
    "bb_pdf_fonts_json",
    "bb_pdf_styles_json",
    "bb_info",
]

conn = sqlite3.connect(":memory:")
conn.enable_load_extension(True)
conn.load_extension(extension_path())

# SQLite doesn't have a function catalog, so test by calling a function
# bb_info with no args should return extension info
try:
    result = conn.execute("SELECT bb_info()").fetchone()[0]
    print(f"bb_info() = {result[:100]}...")
except Exception as e:
    raise AssertionError(f"bb_info() failed: {e}")

# Test that JSON functions are callable (they'll error on bad input, but
# the function should be found)
for func in EXPECTED_FUNCTIONS:
    if func == "bb_info":
        continue
    try:
        conn.execute(f"SELECT {func}(x'00')")
    except sqlite3.OperationalError as e:
        if "no such function" in str(e).lower():
            raise AssertionError(f"Function {func} not registered") from e
        # Other errors (bad input) are fine — function exists
    except Exception:
        pass  # Function exists but errored on bad input — that's OK

print(f"OK: all {len(EXPECTED_FUNCTIONS)} expected functions present")
