"""blobboxes — extract text bounding boxes, fonts, and styles from documents."""

from .blobboxes_ext import (
    _pdf_init,
    _xlsx_init,
    BBoxesCursor,
    BBoxesXlsxCursor,
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

open_pdf  = BBoxesCursor
open_xlsx = BBoxesXlsxCursor
open_text = BBoxesTextCursor
open_docx = BBoxesDocxCursor
open      = BBoxesAutoCursor
