"""Cursor classes over the bboxes C ABI.

A faithful reimplementation of what `python/bindings.cpp` exposed through
nanobind. Method names, return shapes and constructor signatures are unchanged,
so existing callers do not move: `doc()` returns a dict, `pages()`/`fonts()`/
`styles()`/`bboxes()` return lists of dicts, and `close()` is idempotent.

The one thing worth reading before editing: **the buffer is held on the
cursor**. `bboxes_open_*` takes a pointer into the caller's bytes and does not
copy, so the `bytes` object must outlive the cursor. The C++ layer kept a
`std::vector<char>`; here `self._buf` does the same job, and dropping that
attribute would leave the cursor reading freed memory.
"""

from __future__ import annotations

import json as _json

from . import _native as _n
from ._native import Error, lib

__all__ = [
    "BBoxesPdfCursor", "BBoxesPdfObjCursor", "BBoxesXlsxCursor",
    "BBoxesXlsxSlowCursor", "BBoxesXlsCursor", "BBoxesTextCursor",
    "BBoxesDocxCursor", "BBoxesHtmlCursor", "BBoxesAutoCursor",
]


def _require(name: str):
    fn = getattr(lib, name, None)
    if fn is None:
        raise Error(
            f"{name} is not in this build of libbboxes — the corresponding "
            f"backend was disabled at build time (see -D options in build.zig)"
        )
    return fn


def _rows(step, build):
    """Drain a `bboxes_next_*` iterator, copying each row out as it is read."""
    out = []
    while True:
        p = step()
        if not p:
            return out
        out.append(build(p.contents))


class _CursorBase:
    """Shared behaviour for every cursor type."""

    # Set by subclasses before opening.
    _include_formula = False
    _int_coords = False

    def __init__(self):
        self._cur = None
        self._buf = None  # keeps the document bytes alive; see module docstring

    # ── document-level ───────────────────────────────────────────────

    def doc(self) -> dict:
        p = lib.bboxes_get_doc(self._cur)
        if not p:
            return {}
        d = p.contents
        return {
            "document_id": d.document_id,
            "source_type": _n._str(d.source_type),
            "filename": _n._str(d.filename),
            "checksum": _n._str(d.checksum),
            "page_count": d.page_count,
        }

    def pages(self) -> list:
        return _rows(lambda: lib.bboxes_next_page(self._cur), lambda p: {
            "page_id": p.page_id,
            "document_id": p.document_id,
            "page_number": p.page_number,
            "width": p.width,
            "height": p.height,
        })

    def fonts(self) -> list:
        return _rows(lambda: lib.bboxes_next_font(self._cur), lambda f: {
            "font_id": f.font_id,
            "name": _n._str(f.name),
        })

    def styles(self) -> list:
        return _rows(lambda: lib.bboxes_next_style(self._cur), lambda s: {
            "style_id": s.style_id,
            "font_id": s.font_id,
            "font_size": s.font_size,
            "color": _n._str(s.color),
            "weight": _n._str(s.weight),
            "italic": s.italic,
            "underline": s.underline,
        })

    def bboxes(self) -> list:
        int_coords = self._int_coords
        want_formula = self._include_formula

        def build(b):
            # Cell-grid formats (xlsx/xls/text/docx/html) carry integer
            # coordinates; bboxes_format_int_coords is the single source of
            # truth for which, and hosts must not re-decide it.
            if int_coords:
                geom = {"x": int(b.x), "y": int(b.y), "w": int(b.w), "h": int(b.h)}
            else:
                geom = {"x": b.x, "y": b.y, "w": b.w, "h": b.h}
            row = {"page_id": b.page_id, "style_id": b.style_id, **geom}
            row["cell_type"] = _n._str(b.cell_type)
            row["vnum"] = b.vnum if b.has_vnum else None
            row["vbool"] = bool(b.vbool) if b.has_vbool else None
            row["text"] = _n._str(b.text)
            if want_formula:
                row["formula"] = _n._str(b.formula)
            return row

        return _rows(lambda: lib.bboxes_next_bbox(self._cur), build)

    # ── xlsx extras, off the same parse as bboxes() ──────────────────

    def sheet_meta(self):
        """Per-sheet merges and dimension, captured during the cell scan."""
        return _json.loads(_n._str(lib.bboxes_get_sheet_meta_json(self._cur)) or "null")

    def style_decode(self):
        """styles.xml + theme decode, from the cursor's held bytes."""
        fn = _require("bboxes_xlsx_style_decode_json")
        return _json.loads(_n._str(fn(self._buf, len(self._buf))) or "null")

    # ── lifecycle ────────────────────────────────────────────────────

    def close(self) -> None:
        if self._cur:
            lib.bboxes_close(self._cur)
            self._cur = None
        # Only now is it safe to drop the buffer the cursor was reading.
        self._buf = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass  # interpreter teardown; nothing useful to do


def _open(self, data: bytes, opener: str, *args, what: str):
    self._buf = bytes(data)
    self._cur = _require(opener)(self._buf, len(self._buf), *args)
    if not self._cur:
        raise Error(f"bad {what}")


class BBoxesPdfCursor(_CursorBase):
    def __init__(self, data: bytes, password=None, start_page: int = 0, end_page: int = 0):
        super().__init__()
        pw = password.encode() if isinstance(password, str) else password
        _open(self, data, "bboxes_open_pdf", pw, start_page, end_page, what="PDF")


class BBoxesPdfObjCursor(_CursorBase):
    """Object-level PDF reader: one bbox per text object."""

    def __init__(self, data: bytes, password=None, start_page: int = 0, end_page: int = 0):
        super().__init__()
        pw = password.encode() if isinstance(password, str) else password
        _open(self, data, "bboxes_open_pdf_objects", pw, start_page, end_page, what="PDF")


class _SpreadsheetCursor(_CursorBase):
    _include_formula = True
    _opener = ""
    _format = 0
    _what = ""

    def __init__(self, data: bytes, password=None, start_page: int = 0, end_page: int = 0):
        super().__init__()
        self._int_coords = bool(lib.bboxes_format_int_coords(self._format))
        pw = password.encode() if isinstance(password, str) else password
        _open(self, data, self._opener, pw, start_page, end_page, what=self._what)


class BBoxesXlsxCursor(_SpreadsheetCursor):
    """DEFAULT xlsx reader: the fast byte-scan path."""

    _opener, _format, _what = "bboxes_open_xlsx_fast", _n.FORMAT_XLSX_FAST, "XLSX"


class BBoxesXlsxSlowCursor(_SpreadsheetCursor):
    """Legacy xlnt path, kept for A/B — the only one that yields fonts/styles."""

    _opener, _format, _what = "bboxes_open_xlsx", _n.FORMAT_XLSX, "XLSX"


class BBoxesXlsCursor(_SpreadsheetCursor):
    """Legacy .xls (BIFF/OLE2)."""

    _opener, _format, _what = "bboxes_open_xls", _n.FORMAT_XLS, "XLS"


class _FlowCursor(_CursorBase):
    _opener = ""
    _format = 0
    _what = ""

    def __init__(self, data: bytes):
        super().__init__()
        self._int_coords = bool(lib.bboxes_format_int_coords(self._format))
        _open(self, data, self._opener, what=self._what)


class BBoxesTextCursor(_FlowCursor):
    _opener, _format, _what = "bboxes_open_text", _n.FORMAT_TEXT, "text"


class BBoxesDocxCursor(_FlowCursor):
    _opener, _format, _what = "bboxes_open_docx", _n.FORMAT_DOCX, "DOCX"


class BBoxesHtmlCursor(_FlowCursor):
    _opener, _format, _what = "bboxes_open_html", _n.FORMAT_HTML, "HTML"


class BBoxesAutoCursor(_CursorBase):
    """Detects the format from magic bytes and opens the right reader."""

    def __init__(self, data: bytes):
        super().__init__()
        self._buf = bytes(data)
        fmt = _n._str(lib.bboxes_detect(self._buf, len(self._buf)))
        # Formulas exist only for the spreadsheet formats; asking for them
        # elsewhere would add a permanently-None column.
        self._include_formula = fmt in ("xlsx", "xls")
        self._cur = lib.bboxes_open(self._buf, len(self._buf))
        if not self._cur:
            raise Error("failed to parse document")
        code = {
            "pdf": _n.FORMAT_PDF, "xlsx": _n.FORMAT_XLSX, "xls": _n.FORMAT_XLS,
            "text": _n.FORMAT_TEXT, "docx": _n.FORMAT_DOCX, "html": _n.FORMAT_HTML,
        }.get(fmt or "", _n.FORMAT_AUTO)
        self._int_coords = bool(lib.bboxes_format_int_coords(code))
