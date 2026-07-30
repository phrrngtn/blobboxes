"""blobboxes — extract text bounding boxes, fonts, and styles from documents."""

from __future__ import annotations

__version__ = "0.4.5"

from ._cursors import (
    BBoxesAutoCursor,
    BBoxesDocxCursor,
    BBoxesHtmlCursor,
    BBoxesPdfCursor,
    BBoxesPdfObjCursor,
    BBoxesTextCursor,
    BBoxesXlsCursor,
    BBoxesXlsxCursor,
    BBoxesXlsxSlowCursor,
)
from ._native import (
    Error,
    duckdb_extension_path,
    lib,
    library_path,
    sqlite_extension_path,
)
from ._native import _str as _decode

__all__ = [
    "open", "open_pdf", "open_pdf_objects", "open_xlsx", "open_xlsx_slow",
    "open_xls", "open_text", "open_docx", "open_html",
    "detect", "info",
    "doc_json", "pages_json", "fonts_json", "styles_json", "bboxes_json", "xfdf",
    "Error", "library_path", "duckdb_extension_path", "sqlite_extension_path",
    "BBoxesAutoCursor", "BBoxesPdfCursor", "BBoxesPdfObjCursor",
    "BBoxesXlsxCursor", "BBoxesXlsxSlowCursor", "BBoxesXlsCursor",
    "BBoxesTextCursor", "BBoxesDocxCursor", "BBoxesHtmlCursor",
]


def _init() -> None:
    """One-time backend init.

    The nanobind package called _pdf_init()/_xlsx_init() at import. PDFium in
    particular has process-global state and must be initialised before any
    document is opened. Guarded, because a build with those backends disabled
    has no such symbol.
    """
    for name in ("bboxes_pdf_init", "bboxes_xlsx_init"):
        fn = getattr(lib, name, None)
        if fn is not None:
            fn()


_init()


# Format-qualified aliases, as the nanobind package exposed them.
open_pdf = BBoxesPdfCursor
open_pdf_objects = BBoxesPdfObjCursor  # object-level PDF (one bbox per text object)
open_xlsx = BBoxesXlsxCursor           # DEFAULT: fast byte-scan reader
open_xlsx_slow = BBoxesXlsxSlowCursor  # legacy xlnt path (kept for A/B)
open_xls = BBoxesXlsCursor
open_text = BBoxesTextCursor
open_docx = BBoxesDocxCursor
open_html = BBoxesHtmlCursor
open = BBoxesAutoCursor  # noqa: A001 — shadows the builtin deliberately, as before


def detect(data: bytes) -> str | None:
    """Identify the format from magic bytes: 'pdf', 'xlsx', 'docx', 'text', ..."""
    buf = bytes(data)
    return _decode(lib.bboxes_detect(buf, len(buf)))


def info(data: bytes) -> dict:
    """Open, read the document record, and close. Raises Error on a bad document."""
    with BBoxesAutoCursor(data) as cur:
        return cur.doc()


def _cursor_for(data, password, start_page, end_page):
    """Pick the reader a JSON accessor should run against.

    The auto cursor takes neither a password nor a page range, so when either is
    supplied the caller wants a format-specific reader — which is what the
    nanobind helper did by dispatching on the detected format.
    """
    if not (password or start_page or end_page):
        return BBoxesAutoCursor(data)
    fmt = detect(data)
    if fmt == "pdf":
        return BBoxesPdfCursor(data, password, start_page, end_page)
    if fmt == "xlsx":
        return BBoxesXlsxCursor(data, password, start_page, end_page)
    if fmt == "xls":
        return BBoxesXlsCursor(data, password, start_page, end_page)
    return BBoxesAutoCursor(data)


def _json_via_cursor(data, password, start_page, end_page, getter) -> str:
    """Open, pull one JSON clob, close.

    The accessor returns a borrowed pointer into a thread-local buffer, so it is
    copied before the cursor is closed.
    """
    cur = _cursor_for(data, password, start_page, end_page)
    try:
        return _decode(getter(cur._cur)) or "null"
    finally:
        cur.close()


def doc_json(data, password=None, start_page=0, end_page=0) -> str:
    return _json_via_cursor(data, password, start_page, end_page, lib.bboxes_get_doc_json)


def pages_json(data, password=None, start_page=0, end_page=0) -> str:
    return _json_via_cursor(data, password, start_page, end_page, lib.bboxes_get_pages_json)


def fonts_json(data, password=None, start_page=0, end_page=0) -> str:
    return _json_via_cursor(data, password, start_page, end_page, lib.bboxes_get_fonts_json)


def styles_json(data, password=None, start_page=0, end_page=0) -> str:
    return _json_via_cursor(data, password, start_page, end_page, lib.bboxes_get_styles_json)


def bboxes_json(data, password=None, start_page=0, end_page=0) -> str:
    return _json_via_cursor(data, password, start_page, end_page, lib.bboxes_get_bboxes_json)


def xfdf(annots_json: str) -> str:
    """Render an annotation list as XFDF."""
    return _decode(lib.bboxes_xfdf_from_json(annots_json.encode())) or ""
