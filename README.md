# pdf_bboxes

Extract text bounding boxes and fonts from PDFs using [PDFium](https://pdfium.googlesource.com/pdfium/).

Each text run includes its position, dimensions, font, size, color, and style — everything you need to understand the visual layout of a PDF page.

## Interfaces

The core library (`src/pdf_bboxes.cpp`) wraps PDFium and is compiled directly into each consumer:

- **C/C++ shared library** — `libpdf_bboxes.so` / `.dylib`
- **Python module** — via [nanobind](https://github.com/wjakob/nanobind), installed with pip/uv
- **DuckDB extension** — loadable `.duckdb_extension` with table and scalar functions
- **SQLite extension** — loadable module with virtual tables and scalar functions

Each interface provides two access patterns:

| Pattern | Python | DuckDB | SQLite |
|---|---|---|---|
| **Structured** (rows/dicts) | `extract()` iterator | `pdf_extract()` table function | `pdf_extract()` virtual table |
| **JSON** (single array string) | `extract_json()` | `pdf_extract_json()` scalar | `pdf_extract_json()` scalar |

## Building

Requires CMake 3.20+ and a C++17 compiler. PDFium and nlohmann/json are fetched automatically.

```bash
cmake -B build
cmake --build build
```

Enable optional targets:

```bash
cmake -B build \
  -DBUILD_PYTHON_BINDINGS=ON \
  -DBUILD_DUCKDB_EXTENSION=ON \
  -DBUILD_SQLITE_EXTENSION=ON
cmake --build build
```

### Python

```bash
pip install .
# or for development:
pip install nanobind scikit-build-core
pip install -e .
```

### DuckDB extension

After building, append the required metadata footer:

```bash
python duckdb_ext/append_metadata.py build/duckdb/pdf_bboxes.so build/duckdb/pdf_bboxes.duckdb_extension
```

## Usage

### Python

```python
import pdf_bboxes

data = open("document.pdf", "rb").read()

# Iterate over text runs as dicts
for run in pdf_bboxes.extract(data):
    print(run["text"], run["x"], run["y"], run["font_size"])

# Get all fonts
for font in pdf_bboxes.fonts(data):
    print(font["name"], font["style"])

# Or get everything as a JSON string
import json
runs = json.loads(pdf_bboxes.extract_json(data))
fonts = json.loads(pdf_bboxes.fonts_json(data))
```

### DuckDB

```sql
LOAD 'pdf_bboxes.duckdb_extension';

-- Table function (row-at-a-time)
SELECT text, x, y, w, h, font_size, style
FROM pdf_extract('document.pdf')
WHERE page = 1;

-- Scalar JSON (returns full array)
SELECT pdf_extract_json('document.pdf');

-- Fonts
SELECT * FROM pdf_fonts('document.pdf');
SELECT pdf_fonts_json('document.pdf');
```

### SQLite

```sql
.load pdf_bboxes

SELECT text, x, y, w, h, font_size, style
FROM pdf_extract('document.pdf')
WHERE page = 1;

SELECT pdf_extract_json('document.pdf');
SELECT * FROM pdf_fonts('document.pdf');
SELECT pdf_fonts_json('document.pdf');
```

### C++

```bash
./build/pdf_bboxes_example document.pdf
```

## Output fields

### Text runs (`pdf_extract` / `extract`)

| Field | Type | Description |
|---|---|---|
| `font_id` | int | Index into the font table |
| `page` | int | 1-based page number |
| `x`, `y` | float | Top-left corner position (points) |
| `w`, `h` | float | Bounding box dimensions (points) |
| `text` | string | The text content |
| `color` | string | Fill color as `rgba(r,g,b,a)` |
| `font_size` | float | Font size in points |
| `style` | string | `normal`, `bold`, `italic`, or `bold-italic` |

### Fonts (`pdf_fonts` / `fonts`)

| Field | Type | Description |
|---|---|---|
| `font_id` | int | Matches `font_id` in text runs |
| `name` | string | Font name |
| `flags` | int | PDFium font descriptor flags |
| `style` | string | `normal`, `bold`, `italic`, or `bold-italic` |

## Testing

```bash
./test_cross_check.sh [file.pdf]
```

Verifies that structured and JSON outputs match across all three consumers (Python, DuckDB, SQLite) using `EXCEPT` queries.

## License

[MIT](LICENSE)
