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
- C++ source in `src/`
- Python package in `python/blobboxes/`
- Build artifacts in `build/` (CMake, including fetched deps like xlnt)
- Tests: TODO

## Geometry
- Using **shapely** (>=2.0) for spatial operations (bbox intersection, point-in-polygon, etc.)
- Original plan was togo (Python bindings for tidwall/tg) but it doesn't build on macOS
- Future: may add tg as a native DuckDB/SQLite extension (see Alex Garcia's sqlite-tg)
