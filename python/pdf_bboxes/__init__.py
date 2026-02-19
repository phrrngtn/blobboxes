"""pdf_bboxes — extract text bounding boxes and fonts from PDFs."""

from .pdf_bboxes_ext import _init, ExtractCursor, FontCursor, extract_json, fonts_json

_init()

extract = ExtractCursor
fonts = FontCursor
