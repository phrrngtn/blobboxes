# blobboxes

A normalized relational view of document content — PDF, Excel, Word, and plain text — exposed through Python, DuckDB, SQLite, and a C API.

Every source produces the same five tables: **doc**, **pages**, **fonts**, **styles**, and **bboxes**. This uniform schema lets you query and join across formats as if they were database tables.

## Why

Extracting structured data from documents usually means reaching for a different library per format, each with its own API and output shape. blobboxes normalizes all of them into one relational schema so you can:

- Query a PDF invoice and an Excel spreadsheet with the same SQL
- Join a DOCX table with an XLSX sheet on shared column values
- Pipe any document through the same downstream pipeline

The core is a C library with a cursor-based API. Each consumer (Python, DuckDB extension, SQLite extension) compiles it in directly — no intermediate shared library, no IPC.

## Supported formats

| Format | Backend | Notes |
|--------|---------|-------|
| PDF | [PDFium](https://pdfium.googlesource.com/pdfium/) | Text runs with position, font, style, color |
| XLSX | [xlnt](https://github.com/tfussell/xlnt) | One bbox per cell; includes formulas |
| DOCX | [pugixml](https://github.com/zeux/pugixml) + [miniz](https://github.com/richgel999/miniz) | Table cells extracted from Word documents |
| Text | Built-in | One bbox per line |

Format is auto-detected from magic bytes, or you can specify it explicitly.

## Schema

All formats produce the same five tables:

**doc** — one row per document

| Field | Type | Description |
|-------|------|-------------|
| document_id | int | Unique ID |
| source_type | string | `"pdf"`, `"xlsx"`, `"text"`, `"docx"` |
| filename | string | NULL for buffer-based opens |
| checksum | string | MD5 hex of source bytes |
| page_count | int | Number of pages (sheets for XLSX) |

**pages** — one row per page/sheet

| Field | Type | Description |
|-------|------|-------------|
| page_id | int | Unique ID |
| document_id | int | FK to doc |
| page_number | int | 1-based |
| width, height | float | Dimensions in points (PDF) or 0 (other formats) |

**fonts** — distinct fonts used

| Field | Type | Description |
|-------|------|-------------|
| font_id | int | Unique ID |
| name | string | Font family name |

**styles** — distinct (font, size, color, weight) combinations

| Field | Type | Description |
|-------|------|-------------|
| style_id | int | Unique ID |
| font_id | int | FK to fonts |
| font_size | float | Size in points |
| color | string | `"rgba(r,g,b,a)"` |
| weight | string | `"normal"` or `"bold"` |
| italic | int | 0 or 1 |
| underline | int | 0 or 1 |

**bboxes** — one row per text run / cell / line

| Field | Type | Description |
|-------|------|-------------|
| page_id | int | FK to pages |
| style_id | int | FK to styles |
| x, y, w, h | float | Bounding box (points for PDF, 0 for others) |
| text | string | Text content |
| formula | string | Raw formula (XLSX only, NULL otherwise) |

## Usage

### Python

```bash
pip install blobboxes
```

```python
import blobboxes

data = open("invoice.pdf", "rb").read()

# Auto-detect format, open a cursor
cur = blobboxes.open(data)
doc = cur.doc()          # dict
pages = cur.pages()      # list of dicts
fonts = cur.fonts()
styles = cur.styles()
bboxes = cur.bboxes()
cur.close()

# Or use format-specific openers
cur = blobboxes.open_pdf(data)
cur = blobboxes.open_xlsx(data)
cur = blobboxes.open_docx(data)
cur = blobboxes.open_text(data)

# Quick metadata without a cursor
info = blobboxes.info(data)   # {"source_type": "pdf", "page_count": 3, ...}
fmt  = blobboxes.detect(data) # "pdf"

# JSON interface (returns JSON strings, no cursor needed)
import json
doc_obj = json.loads(blobboxes.doc_json(data))
all_bboxes = json.loads(blobboxes.bboxes_json(data))
```

Pydantic schemas are available for validation:

```python
from blobboxes.schema import Doc, Page, Font, Style, BBox, XlsxBBox
```

### DuckDB

```sql
LOAD 'bboxes.duckdb_extension';

-- Auto-detecting (inspects magic bytes)
SELECT * FROM bboxes('invoice.pdf');
SELECT * FROM bboxes_doc('report.xlsx');
SELECT * FROM bboxes_pages('document.docx');

-- Format-specific
SELECT * FROM bboxes_xlsx('report.xlsx');
SELECT * FROM bboxes_xlsx_fonts('report.xlsx');

-- JSON scalars
SELECT bboxes_json('invoice.pdf');
SELECT bboxes_xlsx_doc_json('report.xlsx');

-- Quick info
SELECT bboxes_info('invoice.pdf');
```

Each format registers five table functions and five JSON scalars:

| Auto-detect | PDF | XLSX | Text | DOCX |
|-------------|-----|------|------|------|
| `bboxes_doc` | `bboxes_pdf_doc` | `bboxes_xlsx_doc` | `bboxes_text_doc` | `bboxes_docx_doc` |
| `bboxes_pages` | `bboxes_pdf_pages` | `bboxes_xlsx_pages` | `bboxes_text_pages` | `bboxes_docx_pages` |
| `bboxes_fonts` | `bboxes_pdf_fonts` | `bboxes_xlsx_fonts` | `bboxes_text_fonts` | `bboxes_docx_fonts` |
| `bboxes_styles` | `bboxes_pdf_styles` | `bboxes_xlsx_styles` | `bboxes_text_styles` | `bboxes_docx_styles` |
| `bboxes` | `bboxes_pdf` | `bboxes_xlsx` | `bboxes_text` | `bboxes_docx` |

The same names with `_json` appended are JSON scalar variants (e.g. `bboxes_pdf_json`).

### SQLite

```sql
.load bboxes

-- Virtual tables (file_path is a hidden required column)
SELECT * FROM bboxes WHERE file_path = 'invoice.pdf';
SELECT * FROM bboxes_pages WHERE file_path = 'report.xlsx';

-- Format-specific
SELECT * FROM bboxes_xlsx WHERE file_path = 'report.xlsx';

-- JSON scalars
SELECT bboxes_json('invoice.pdf');
SELECT bboxes_xlsx_doc_json('report.xlsx');
SELECT bboxes_info('invoice.pdf');
```

The function naming follows the same pattern as DuckDB.

### C/C++

```c
#include "bboxes.h"

bboxes_pdf_init();

bboxes_cursor* cur = bboxes_open(buf, len);    // auto-detect
// or: bboxes_open_pdf(buf, len, password, start_page, end_page);
// or: bboxes_open_xlsx(buf, len, password, start_page, end_page);
// or: bboxes_open_text(buf, len);
// or: bboxes_open_docx(buf, len);

const bboxes_doc*   doc = bboxes_get_doc(cur);
const bboxes_page*  pg;  while ((pg = bboxes_next_page(cur)))  { /* ... */ }
const bboxes_font*  f;   while ((f  = bboxes_next_font(cur)))  { /* ... */ }
const bboxes_style* s;   while ((s  = bboxes_next_style(cur))) { /* ... */ }
const bboxes_bbox*  b;   while ((b  = bboxes_next_bbox(cur)))  { /* ... */ }

bboxes_close(cur);
bboxes_pdf_destroy();
```

Every struct iterator has a JSON variant (`bboxes_next_page_json`, etc.) that returns a `const char*` JSON string.

## Building

Requires CMake 3.20+ and a C++17 compiler. Dependencies (PDFium, nlohmann/json, hash-library, xlnt, pugixml, miniz) are fetched automatically via CMake FetchContent.

```bash
# Quick: build everything and run tests
./build_all.sh

# Manual: configure with desired backends
cmake -B build \
  -DBUILD_PYTHON_BINDINGS=ON \
  -DBUILD_DUCKDB_EXTENSION=ON \
  -DBUILD_SQLITE_EXTENSION=ON \
  -DBUILD_XLSX_BACKEND=ON \
  -DBUILD_TEXT_BACKEND=ON \
  -DBUILD_DOCX_BACKEND=ON
cmake --build build
```

For Python bindings, the build needs a virtual environment with nanobind:

```bash
uv venv .venv
uv pip install nanobind scikit-build-core
cmake -B build \
  -DBUILD_PYTHON_BINDINGS=ON \
  -DPython_FIND_VIRTUALENV=ONLY \
  -DPython_ROOT_DIR=.venv
cmake --build build
```

## Testing

```bash
./test_cross_check.sh
```

Runs 30 tests verifying that struct and JSON outputs match across all consumers (Python, DuckDB, SQLite) for all formats (PDF, XLSX, DOCX, text). Uses `EXCEPT` queries to ensure exact equivalence.

## License

[MIT](LICENSE)
