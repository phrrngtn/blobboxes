"""blobboxes — extract text bounding boxes, fonts, and styles from documents."""

__version__ = "0.4.5"

from .blobboxes_ext import (
    _pdf_init,
    _xlsx_init,
    BBoxesCursor,
    BBoxesXlsxCursor,
    BBoxesXlsxSlowCursor,
    BBoxesTextCursor,
    BBoxesDocxCursor,
    BBoxesAutoCursor,
    detect,
    info,
    doc_json,
    pages_json,
    fonts_json,
    styles_json,
    bboxes_json,
)

_pdf_init()
_xlsx_init()

open_pdf       = BBoxesCursor
open_xlsx      = BBoxesXlsxCursor       # DEFAULT: fast byte-scan reader
open_xlsx_slow = BBoxesXlsxSlowCursor   # legacy xlnt path (kept for A/B)
open_text      = BBoxesTextCursor
open_docx = BBoxesDocxCursor
open      = BBoxesAutoCursor
