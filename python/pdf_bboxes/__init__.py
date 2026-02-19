"""pdf_bboxes — extract text bounding boxes and fonts from PDFs."""

import atexit as _atexit

from .pdf_bboxes_ext import _init, _destroy, _extract, _fonts

_init()
_atexit.register(_destroy)


def extract(data: bytes, *, password: str | None = None) -> list[dict]:
    """Extract text bounding boxes from PDF bytes.

    Args:
        data: Raw PDF file contents.
        password: Optional document password.

    Returns:
        List of dicts, one per text run, with keys:
        font_id, page, x, y, w, h, text, color, font_size, style.
    """
    return _extract(data, password)


def fonts(data: bytes, *, password: str | None = None) -> list[dict]:
    """Extract the font table from PDF bytes.

    Args:
        data: Raw PDF file contents.
        password: Optional document password.

    Returns:
        List of dicts, one per unique font, with keys:
        font_id, name, flags, style.
    """
    return _fonts(data, password)
