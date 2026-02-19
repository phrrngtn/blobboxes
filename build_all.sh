#!/usr/bin/env bash
# Local CI equivalent — builds all targets and runs cross-check tests.
# Usage: ./build_all.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$DIR/build"
VENV="$DIR/.venv"

echo "=== 1/5  Virtual environment ==="
if [ ! -d "$VENV" ]; then
    uv venv "$VENV"
fi
uv pip install -q --python "$VENV/bin/python" nanobind scikit-build-core

echo ""
echo "=== 2/5  CMake configure ==="
cmake -B "$BUILD" \
    -DBUILD_PYTHON_BINDINGS=ON \
    -DBUILD_DUCKDB_EXTENSION=ON \
    -DBUILD_SQLITE_EXTENSION=ON \
    -DPython_FIND_VIRTUALENV=ONLY \
    -DPython_ROOT_DIR="$VENV"

echo ""
echo "=== 3/5  CMake build ==="
cmake --build "$BUILD"

echo ""
echo "=== 4/5  Append DuckDB metadata ==="
"$VENV/bin/python" "$DIR/duckdb_ext/append_metadata.py" \
    "$BUILD/duckdb/pdf_bboxes.so" \
    "$BUILD/duckdb/pdf_bboxes.duckdb_extension"

echo ""
echo "=== 5/5  Cross-check tests ==="
"$DIR/test_cross_check.sh"

echo ""
echo "=== All done — build and tests passed ==="
