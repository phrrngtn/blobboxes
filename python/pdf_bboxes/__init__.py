"""pdf_bboxes — extract text bounding boxes and fonts from PDFs."""

from .pdf_bboxes_ext import (
    _init,
    ExtractCursor,
    FontCursor,
    ExtractJsonCursor,
    FontJsonCursor,
)

_init()

extract = ExtractCursor
fonts = FontCursor
extract_json = ExtractJsonCursor
fonts_json = FontJsonCursor
