# Google Sheets JSON → blobboxes Schema Mapping

## Architecture

No C++ code needed. The pipeline is:

```
blobhttp (HttpAdapter)                    SQL (bt_jmespath / DuckDB)
─────────────────────                    ─────────────────────────────
Sheets API v4 GET                        Reshape JSON → 5 bboxes tables
spreadsheets/{id}?includeGridData=true   (doc, pages, fonts, styles, bboxes)
         │                                        │
         ▼                                        ▼
    raw JSON response                    Same schema as bb_xlsx()
    (stored in PG or                     (queryable, joinable, comparable
     temp DuckDB table)                   with PDF/XLSX/DOCX bboxes)
```

**blobhttp** fetches the JSON via the Google Sheets API (OAuth token from vault).
**SQL** reshapes it using `bt_jmespath` and DuckDB JSON functions — no C++ backend.

## Google Sheets API Response Structure

```
Spreadsheet
├── spreadsheetId
├── properties.title
└── sheets[]
    ├── properties
    │   ├── sheetId          → page_id
    │   ├── title            → page name
    │   ├── index            → page_number (1-based)
    │   └── gridProperties
    │       ├── rowCount
    │       └── columnCount
    └── data[]  (GridData — one per requested range, or one for all if includeGridData=true)
        ├── startRow, startColumn
        ├── rowMetadata[]
        │   └── pixelSize    → row height in pixels
        ├── columnMetadata[]
        │   └── pixelSize    → column width in pixels
        └── rowData[]
            └── values[]  (CellData)
                ├── formattedValue           → text
                ├── effectiveValue           → typed value (number, string, bool, formula result)
                ├── effectiveFormat
                │   ├── textFormat
                │   │   ├── fontFamily       → font name
                │   │   ├── fontSize         → font size (pt)
                │   │   ├── bold             → weight
                │   │   ├── italic           → italic
                │   │   ├── underline        → underline
                │   │   └── foregroundColorStyle → color
                │   ├── backgroundColor[Style]
                │   ├── borders.{top,bottom,left,right}
                │   ├── horizontalAlignment
                │   └── verticalAlignment
                ├── textFormatRuns[]          → intra-cell rich text (multiple styles per cell)
                ├── hyperlink
                └── note
```

## Mapping to blobboxes 5-Table Schema

### 1. doc

```sql
SELECT
    1                                           AS document_id,
    'gsheet'                                    AS source_type,
    bt_jmespath(response, 'spreadsheetId')      AS filename,
    NULL                                        AS checksum,
    bt_jmespath(response, 'length(sheets)')     AS page_count
```

### 2. pages

One row per sheet tab. Width/height from grid dimensions × default pixel sizes
(or sum of column/row metadata pixel sizes).

```sql
-- From sheets[].properties + sheets[].data[0].columnMetadata/rowMetadata
SELECT
    s.sheetId                                   AS page_id,
    1                                           AS document_id,
    s.index + 1                                 AS page_number,
    -- Total width = sum of all columnMetadata[].pixelSize
    col_total_width                             AS width,
    -- Total height = sum of all rowMetadata[].pixelSize
    row_total_height                            AS height
```

### 3. fonts

Deduplicated font families across all cells.

```sql
-- Collect distinct fontFamily values from all cells' effectiveFormat.textFormat
WITH ALL_FONTS AS (
    SELECT DISTINCT
        effectiveFormat.textFormat.fontFamily AS name
    FROM all_cells
    WHERE effectiveFormat IS NOT NULL
)
SELECT
    ROW_NUMBER() OVER (ORDER BY name)           AS font_id,
    name
FROM ALL_FONTS
```

### 4. styles

Deduplicated (font_id, fontSize, color, weight, italic, underline) tuples.

```sql
-- Each unique combination of formatting attributes gets a style_id
WITH ALL_STYLES AS (
    SELECT DISTINCT
        effectiveFormat.textFormat.fontFamily,
        effectiveFormat.textFormat.fontSize,
        effectiveFormat.textFormat.foregroundColorStyle,
        effectiveFormat.textFormat.bold,
        effectiveFormat.textFormat.italic,
        effectiveFormat.textFormat.underline
    FROM all_cells
)
SELECT
    ROW_NUMBER() OVER ()                        AS style_id,
    font_id,                                    -- FK to fonts via fontFamily lookup
    fontSize                                    AS font_size,
    -- color as rgba(r,g,b,a) string
    'rgba(' || COALESCE(color.red,0)*255 || ',' ||
               COALESCE(color.green,0)*255 || ',' ||
               COALESCE(color.blue,0)*255 || ',' ||
               COALESCE(color.alpha,1) || ')'   AS color,
    CASE WHEN bold THEN 'bold' ELSE 'normal' END AS weight,
    CASE WHEN italic THEN 1 ELSE 0 END         AS italic,
    CASE WHEN underline THEN 1 ELSE 0 END      AS underline
FROM ALL_STYLES
```

### 5. bboxes

One row per non-empty cell. Coordinates use the same convention as `bb_xlsx`:
- `x` = column number (1-based)
- `y` = row number (1-based)
- `w` = column span (1 for unmerged)
- `h` = row span (1 for unmerged)

```sql
-- For each cell in sheets[].data[].rowData[].values[]
SELECT
    sheet.sheetId                               AS page_id,
    style_id,                                   -- FK to styles via format lookup
    col_index + 1                               AS x,
    row_index + 1                               AS y,
    COALESCE(merge_col_span, 1)                 AS w,
    COALESCE(merge_row_span, 1)                 AS h,
    cell.formattedValue                         AS text,
    NULL                                        AS formula
    -- (could populate formula from cell.userEnteredValue.formulaValue)
```

### Merged cells

Google Sheets reports merges separately in `sheets[].merges[]`:

```json
{
    "startRowIndex": 2,
    "endRowIndex": 4,
    "startColumnIndex": 1,
    "endColumnIndex": 3
}
```

This maps to: the cell at (startRow, startCol) gets `w = endCol - startCol`,
`h = endRow - startRow`. All other cells covered by the merge are skipped
(same as bb_xlsx behavior).

## JMESPath Expressions for the Transform

### Extract all cells as flat rows

```jmespath
sheets[].{
    sheetId: properties.sheetId,
    sheetTitle: properties.title,
    sheetIndex: properties.index,
    rows: data[0].rowData[].values[]
}
```

### Extract row/column dimensions

```jmespath
sheets[].{
    sheetId: properties.sheetId,
    rowHeights: data[0].rowMetadata[].pixelSize,
    colWidths: data[0].columnMetadata[].pixelSize
}
```

### Extract merged cell ranges

```jmespath
sheets[].{
    sheetId: properties.sheetId,
    merges: merges[].{
        startRow: startRowIndex,
        endRow: endRowIndex,
        startCol: startColumnIndex,
        endCol: endColumnIndex
    }
}
```

## SQL Sketch (DuckDB)

The full transform would be a series of CTEs that:

1. `UNNEST` the sheets array
2. For each sheet, `UNNEST` the rowData array (with ordinality for row index)
3. For each row, `UNNEST` the values array (with ordinality for column index)
4. Extract formatting fields from each cell's `effectiveFormat`
5. Deduplicate fonts and styles
6. Handle merges by joining against the `merges[]` array
7. Produce the 5 output tables

```sql
-- Pseudocode sketch
WITH SHEETS AS (
    SELECT UNNEST(json_extract(response, '$.sheets')) AS sheet
),
SHEET_ROWS AS (
    SELECT
        sheet.properties.sheetId AS page_id,
        UNNEST(sheet.data[0].rowData) WITH ORDINALITY AS (row_data, row_idx)
    FROM SHEETS
),
CELLS AS (
    SELECT
        page_id,
        row_idx AS y,
        UNNEST(row_data.values) WITH ORDINALITY AS (cell, col_idx)
    FROM SHEET_ROWS
),
FORMATTED_CELLS AS (
    SELECT
        page_id,
        y,
        col_idx AS x,
        cell.formattedValue AS text,
        cell.effectiveFormat.textFormat.fontFamily AS font_family,
        cell.effectiveFormat.textFormat.fontSize AS font_size,
        cell.effectiveFormat.textFormat.bold AS is_bold,
        cell.effectiveFormat.textFormat.italic AS is_italic,
        cell.effectiveFormat.textFormat.underline AS is_underline,
        cell.effectiveFormat.textFormat.foregroundColorStyle AS fg_color
    FROM CELLS
    WHERE cell.formattedValue IS NOT NULL
)
-- ... then deduplicate into fonts, styles, and emit bboxes
SELECT * FROM FORMATTED_CELLS
```

## Comparison: XLSX Export Path vs JSON Path

| Aspect | XLSX Export | JSON (Sheets API) |
|--------|------------|-------------------|
| Fetch | Drive API export endpoint | Sheets API `includeGridData=true` |
| Processing | C++ (xlnt in blobboxes) | SQL (DuckDB JSON + bt_jmespath) |
| New C++ code | None | None |
| Cell values | Cached formula results | Live values (effectiveValue) |
| Formatting | Full (survives export) | Full (effectiveFormat) |
| Merged cells | xlnt handles natively | Need to join against merges[] |
| In-cell images (IMAGE formula) | Lost | Can detect (effectiveValue has formulaValue) |
| Sparklines | Lost | Can detect |
| Cell notes | Converted to Excel comments | Available as `note` field |
| Pivot tables | Static data only | Full structure available |
| Row/col dimensions | Preserved in XLSX | pixelSize in row/columnMetadata |
| Dependencies | xlnt (C++, already built) | DuckDB JSON functions (already available) |
| Auth | OAuth token for Drive export | OAuth token for Sheets API |

**Bottom line:** Both paths produce equivalent bboxes for tabular data. The JSON path
is more expressive (preserves IMAGE/SPARKLINE detection, live pivot structure, notes)
but requires more SQL. The XLSX path is simpler and reuses existing C++ code.

For meplex's equipment data use case, XLSX export is sufficient. The JSON path is
worth building as a general capability for the blob* family when richer Google Sheets
integration is needed.
