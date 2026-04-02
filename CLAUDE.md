# CLAUDE.md - Project conventions for blobboxes

## Package management
- Use `uv` for all pip/venv operations (not pip or python -m venv directly)
  - Create venv: `uv venv --python $(brew --prefix python3)/bin/python3`
  - Install: `uv pip install <package>`
  - Run scripts: `uv run python script.py`
  - The venv is at `.venv/` — system python3 is NOT the project python
- Run Python via `.venv/bin/python3` or `uv run python`
- The dc1 Forgejo package registry is configured as the default PyPI index
  in `~/.config/uv/uv.toml`. Pre-built Python wheels for blob* extensions
  (blobboxes, blobfilters, etc.) are published there. Install with:
  `uv pip install blobboxes` (pulls from dc1 first, falls back to pypi.org)

## Build system
- Uses scikit-build-core with nanobind (C++ extensions)
- CMake configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_XLSX_BACKEND=ON -DBUILD_PYTHON_BINDINGS=ON -DBUILD_DUCKDB_EXTENSION=ON`
- CMake build: `cmake --build build`
- Python dependencies declared in `pyproject.toml` under `[project] dependencies`

## DuckDB extension
- Load with: `duckdb -unsigned` then `LOAD 'build/duckdb/bboxes.duckdb_extension';`
- **Always use `-unsigned`** — the extension is locally built and not signed
- When using DuckDB from Python: `duckdb.connect(config={"allow_unsigned_extensions": "true"})`
- **CTE materialization**: DuckDB materializes CTEs by default. A CTE that calls `bb()` will extract once, not per-reference. For multi-query interactive sessions, use temp tables to avoid re-extraction:
  ```sql
  CREATE TEMP TABLE bboxes AS SELECT * FROM bb('file.pdf');
  ```
  Within a single query, `AS MATERIALIZED` / `AS NOT MATERIALIZED` hints are available but rarely needed — the optimizer handles it.
- Function naming: `bb_*` for auto-detect, `bb_pdf_*`, `bb_xlsx_*`, `bb_text_*`, `bb_docx_*` for explicit format
- Table functions (return rows):
  - `bb(path)` / `bb_pdf(path)` / `bb_xlsx(path)` / ... — bounding boxes: `(page_id, style_id, x, y, w, h, text, formula)`
  - `bb_doc(path)` — document metadata: `(document_id, source_type, filename, checksum, page_count)`
  - `bb_pages(path)` — page dimensions: `(page_id, document_id, page_number, width, height)`
  - `bb_fonts(path)` — font table: `(font_id, name)`
  - `bb_styles(path)` — style table: `(style_id, font_id, font_size, color, weight, italic, underline)`
- JSON scalar functions: `bb_json(path)`, `bb_doc_json(path)`, etc. — return JSON strings
- Coordinate semantics vary by format (see `docs/bbox-as-universal-ir.md`)

## Project structure
- C++ source in `src/`, headers in `include/`
- Python package in `python/blobboxes/`
- Browser JS bundle in `browser/` (esbuild, roaring-wasm)
- DuckDB extension in `duckdb_ext/`
- SQLite extension in `sqlite_ext/`
- Build artifacts in `build/` (CMake, including fetched deps like xlnt)
- Design docs in `docs/`
- Tests: `test.py` (Python API), `test_cross_check.sh` (30 integration tests across Python/DuckDB/SQLite for all formats)

## Browser extraction subsystem
- `python/blobboxes/browser.py` — async BrowserPool + Playwright CDP controller
- `python/blobboxes/proxy_addon.py` — mitmproxy addon dispatching on `X-BLOBTASTIC-EXTRA-INFO` header
- `python/blobboxes/run_proxy.py` — CLI launcher for mitmdump with blobboxes addon
- `python/blobboxes/http_controller.py` — Jina-like HTTP server (`/read`, `/health`) over BrowserPool
- `browser/src/` — JS bundle: DOM bbox extraction, FNV-1a token hashing, roaring-wasm classification, MutationObserver
- Two build variants: lite (snapshot only, ~12 KB) and full (includes roaring-wasm WASM inlined)

## Geometry
- Using **shapely** (>=2.0) for spatial operations (bbox intersection, point-in-polygon, etc.)
- Original plan was togo (Python bindings for tidwall/tg) but it doesn't build on macOS
- Future: may add tg as a native DuckDB/SQLite extension (see Alex Garcia's sqlite-tg)
