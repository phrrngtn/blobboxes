"""ctypes binding to libbboxes — the C ABI declared in include/bboxes.h.

This replaces the nanobind extension module. The C ABI already exists and both
other hosts bind to it, so the Python layer has nothing to gain from a compiled
shim: ctypes over the same symbols removes nanobind, scikit-build-core, the
Python development headers, and the wheel-per-CPython matrix.

Two ownership rules the C header states, both enforced here rather than left to
callers:

* **Struct strings are borrowed** and point into the cursor's storage, valid
  until the next call on that cursor and freed by `bboxes_close`. Every row is
  copied into plain Python data as it is read, so nothing handed out can dangle.
* **The document buffer must outlive the cursor.** The C API takes a pointer
  without copying; the nanobind layer kept its own `std::vector<char>`. Here the
  `bytes` object is held on the cursor for the same reason — dropping it would
  leave the cursor reading freed memory.
"""

from __future__ import annotations

import ctypes
import pathlib
from ctypes import POINTER, c_char_p, c_double, c_int, c_size_t, c_uint32, c_void_p

import blobzig

__all__ = [
    "lib", "Error", "library_path", "duckdb_extension_path",
    "sqlite_extension_path", "Doc", "Page", "Font", "Style", "BBox",
    "FORMAT_AUTO", "FORMAT_PDF", "FORMAT_XLSX", "FORMAT_TEXT", "FORMAT_DOCX",
    "FORMAT_PDF_OBJECTS", "FORMAT_XLSX_FAST", "FORMAT_HTML", "FORMAT_XLS",
]

_PKG = pathlib.Path(__file__).resolve().parent
_artifacts = blobzig.Artifacts("bboxes", package_dir=_PKG, repo_root=_PKG.parents[1])

# nanobind raised nb::value_error, which surfaced as ValueError; blobzig.Error
# is a ValueError, so existing callers and pytest.raises keep working.
Error = blobzig.Error


def library_path() -> str:
    """Path to libbboxes, the shared library behind this module."""
    return _artifacts.library()


def duckdb_extension_path() -> str:
    return _artifacts.duckdb_extension()


def sqlite_extension_path() -> str:
    return _artifacts.sqlite_extension()


lib = _artifacts.load()

# Format codes, mirroring the BBOXES_FORMAT_* macros in include/bboxes.h.
FORMAT_AUTO = 0
FORMAT_PDF = 1
FORMAT_XLSX = 2
FORMAT_TEXT = 3
FORMAT_DOCX = 4
FORMAT_PDF_OBJECTS = 5
FORMAT_XLSX_FAST = 6
FORMAT_HTML = 7
FORMAT_XLS = 8


# ── struct layouts, mirroring include/bboxes.h ───────────────────────
#
# Field order and types are load-bearing: ctypes lays these out by declaration
# order, so a field added to the C header and not added here reads adjacent
# memory rather than failing. Keep them in step.


class Doc(ctypes.Structure):
    _fields_ = [
        ("document_id", c_uint32),
        ("source_type", c_char_p),
        ("filename", c_char_p),
        ("checksum", c_char_p),
        ("page_count", c_int),
    ]


class Page(ctypes.Structure):
    _fields_ = [
        ("page_id", c_uint32),
        ("document_id", c_uint32),
        ("page_number", c_int),
        ("width", c_double),
        ("height", c_double),
    ]


class Font(ctypes.Structure):
    _fields_ = [("font_id", c_uint32), ("name", c_char_p)]


class Style(ctypes.Structure):
    _fields_ = [
        ("style_id", c_uint32),
        ("font_id", c_uint32),
        ("font_size", c_double),
        ("color", c_char_p),
        ("weight", c_char_p),
        ("italic", c_int),
        ("underline", c_int),
    ]


class BBox(ctypes.Structure):
    _fields_ = [
        ("page_id", c_uint32),
        ("style_id", c_uint32),
        ("x", c_double),
        ("y", c_double),
        ("w", c_double),
        ("h", c_double),
        ("cell_type", c_char_p),
        ("has_vnum", c_int),
        ("vnum", c_double),
        ("has_vbool", c_int),
        ("vbool", c_int),
        ("text", c_char_p),
        ("formula", c_char_p),
    ]


# ── prototypes ───────────────────────────────────────────────────────

_P = c_void_p   # bboxes_cursor*
_B = c_char_p   # const void* buffer (bytes)
_S = c_char_p


def _proto(name, argtypes, restype):
    """Declare a symbol if the build has it.

    Optional backends mean some openers are genuinely absent — a build without
    -Dxls has no bboxes_open_xls. Missing symbols are left unset so the cursor
    layer can raise something intelligible rather than ctypes raising
    AttributeError deep inside a call.
    """
    fn = getattr(lib, name, None)
    if fn is not None:
        fn.argtypes = argtypes
        fn.restype = restype
    return fn


# Global init/teardown for the two backends that need it.
for _n in ("bboxes_pdf_init", "bboxes_pdf_destroy",
           "bboxes_xlsx_init", "bboxes_xlsx_destroy"):
    _proto(_n, [], None)

# Openers. PDF and the spreadsheet readers take a password and a 1-based
# inclusive page range; the flow formats take neither.
for _n in ("bboxes_open_pdf", "bboxes_open_pdf_objects",
           "bboxes_open_xlsx", "bboxes_open_xlsx_fast", "bboxes_open_xls"):
    _proto(_n, [_B, c_size_t, _S, c_int, c_int], _P)

for _n in ("bboxes_open", "bboxes_open_text", "bboxes_open_docx", "bboxes_open_html"):
    _proto(_n, [_B, c_size_t], _P)

_proto("bboxes_open_format", [c_int, _B, c_size_t], _P)
_proto("bboxes_close", [_P], None)
_proto("bboxes_detect", [_B, c_size_t], _S)          # borrowed static string
_proto("bboxes_errmsg", [_P], _S)                    # borrowed
_proto("bboxes_format_int_coords", [c_int], c_int)

# Iterators return a borrowed pointer, NULL at end of stream.
_proto("bboxes_get_doc", [_P], POINTER(Doc))
_proto("bboxes_next_page", [_P], POINTER(Page))
_proto("bboxes_next_font", [_P], POINTER(Font))
_proto("bboxes_next_style", [_P], POINTER(Style))
_proto("bboxes_next_bbox", [_P], POINTER(BBox))

# JSON accessors returning into a thread-local buffer, valid only until the next
# call on the same thread (see the header). Copied immediately by _str.
for _n in ("bboxes_get_doc_json", "bboxes_get_pages_json", "bboxes_get_fonts_json",
           "bboxes_get_styles_json", "bboxes_get_bboxes_json",
           "bboxes_get_sheet_meta_json"):
    _proto(_n, [_P], _S)

_proto("bboxes_xfdf_from_json", [_S], _S)

# Buffer-and-path metadata pairs. Each JSON extractor has a _file twin.
for _base in ("bboxes_pdf_metadata_json", "bboxes_pdf_header_json",
              "bboxes_xlsx_metadata_json", "bboxes_xlsx_header_json",
              "bboxes_xlsx_manifest_json", "bboxes_xlsx_sheet_meta_json",
              "bboxes_xlsx_style_decode_json", "bboxes_xlsx_artifact_meta_json",
              "bboxes_xlsx_vba_base64", "bboxes_xls_metadata_json",
              "bboxes_xls_formulas_json", "bboxes_xls_names_json",
              "bboxes_xls_style_decode_json", "bboxes_container_walk_json"):
    _proto(_base, [_B, c_size_t], _S)
    _proto(_base + "_file", [_S], _S)


def _str(raw) -> str | None:
    """Copy a borrowed C string into Python, or None."""
    if not raw:
        return None
    return raw.decode("utf-8", "replace") if isinstance(raw, bytes) else str(raw)
