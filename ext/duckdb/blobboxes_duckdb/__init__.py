"""Packaging wrapper for the blobboxes DuckDB extension."""

import pathlib

_HERE = pathlib.Path(__file__).parent


def extension_path() -> str:
    """Return the absolute path to the blobboxes DuckDB extension.

    Usage:
        LOAD '<path>';  -- in DuckDB with allow_unsigned_extensions
    """
    ext = _HERE / "bboxes.duckdb_extension"
    if not ext.exists():
        raise FileNotFoundError(f"Extension not found at {ext}")
    return str(ext)
