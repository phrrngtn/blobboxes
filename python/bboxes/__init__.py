"""bboxes — extract text bounding boxes, fonts, and styles from documents."""

from .bboxes_ext import (
    _pdf_init,
    BBoxesCursor,
    doc_json,
    pages_json,
    fonts_json,
    styles_json,
    bboxes_json,
)

_pdf_init()

open_pdf = BBoxesCursor
