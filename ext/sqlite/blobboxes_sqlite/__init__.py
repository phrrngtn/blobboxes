"""Packaging wrapper for the bboxes SQLite extension."""

import pathlib

_HERE = pathlib.Path(__file__).parent


def extension_path() -> str:
    """Return the absolute path to the bboxes SQLite extension (without suffix).

    SQLite's .load command does not want the file extension:
        .load <path>
    """
    base = _HERE / "bboxes"
    for suffix in (".so", ".dylib", ".dll"):
        if (base.parent / f"bboxes{suffix}").exists():
            return str(base)
    raise FileNotFoundError(f"Extension not found at {base}.*")


def load(con) -> None:
    """Load the bboxes extension into a SQLite connection."""
    con.load_extension(extension_path())
