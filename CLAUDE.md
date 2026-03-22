# CLAUDE.md - Project conventions for blobboxes

## Package management
- Use `uv` for all pip operations (not pip directly)
  - Install: `uv pip install <package>`
  - The venv is at `.venv/` — system python3 is NOT the project python
- Run Python via `.venv/bin/python3` or activate the venv first

## Build system
- Uses scikit-build-core with nanobind (C++ extensions)
- CMake build: `cmake --build build`
- Python dependencies declared in `pyproject.toml` under `[project] dependencies`

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
