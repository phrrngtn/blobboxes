# Google Drive/Docs/Sheets API — Inja Template Reference

## Overview

This document captures URL patterns for the Google Drive, Docs, and Sheets APIs,
formatted as Inja templates compatible with `bt_template_render()` from blobtemplates.
These can be registered as `HttpAdapter` rows in the blobapi `domain.http_adapter` table.

All endpoints require `Authorization: Bearer {{access_token}}` header.

---

## Google Drive API (v3)

### Download file (binary content)

```
GET https://www.googleapis.com/drive/v3/files/{{fileId}}?alt=media
```
Only works for non-Google-native files (PDFs, images, uploads). For Google Docs/Sheets,
use Export.

### Export Google Doc → PDF

```
GET https://www.googleapis.com/drive/v3/files/{{fileId}}/export?mimeType=application/pdf
```

### Export Google Sheet → XLSX

```
GET https://www.googleapis.com/drive/v3/files/{{fileId}}/export?mimeType=application/vnd.openxmlformats-officedocument.spreadsheetml.sheet
```
Exports entire spreadsheet (all sheets) as one Excel workbook.

### Export Google Sheet → CSV (first sheet only)

```
GET https://www.googleapis.com/drive/v3/files/{{fileId}}/export?mimeType=text/csv
```
Only exports the **first sheet**. For a specific sheet by gid, use the web export URL:
```
GET https://docs.google.com/spreadsheets/d/{{spreadsheetId}}/export?format=csv&gid={{sheetId}}
```
(Not an official API endpoint but widely used; works with OAuth tokens.)

### List files in folder

```
GET https://www.googleapis.com/drive/v3/files?q='{{folderId}}'+in+parents&fields=nextPageToken,files(id,name,mimeType,modifiedTime)&pageSize={{pageSize}}&pageToken={{pageToken}}
```

### Get file metadata

```
GET https://www.googleapis.com/drive/v3/files/{{fileId}}?fields=id,name,mimeType,size,modifiedTime,parents
```
Drive v3 returns minimal fields by default — `fields` param is required.

### Upload file (multipart — metadata + content)

```
POST https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart
Content-Type: multipart/related; boundary={{boundary}}
```
Body: Part 1 = `application/json` metadata (`name`, `parents`, `mimeType`);
Part 2 = raw file bytes.

### Upload file (resumable — for large files)

```
POST https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable
Content-Type: application/json
X-Upload-Content-Type: {{fileMimeType}}
X-Upload-Content-Length: {{fileSize}}
```
Returns `Location` header with session URI. Then PUT bytes in chunks.

### Copy file

```
POST https://www.googleapis.com/drive/v3/files/{{fileId}}/copy
Content-Type: application/json
```
Body: `{"name": "{{newName}}", "parents": ["{{folderId}}"]}`

---

## Google Docs API (v1)

### Get document

```
GET https://docs.googleapis.com/v1/documents/{{documentId}}
```
Returns full structured JSON (paragraphs, tables, inline objects).

### Create document

```
POST https://docs.googleapis.com/v1/documents
Content-Type: application/json
```
Body: `{"title": "{{title}}"}`

### Batch update document

```
POST https://docs.googleapis.com/v1/documents/{{documentId}}:batchUpdate
Content-Type: application/json
```
Body: `{"requests": [...]}`
Request types: `insertText`, `deleteContentRange`, `insertTable`,
`updateTextStyle`, `replaceAllText`, `insertInlineImage`.

---

## Google Sheets API (v4)

### Get spreadsheet metadata

```
GET https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}?fields=spreadsheetId,properties.title,sheets.properties
```

### Get sheet names and IDs

```
GET https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}?fields=sheets.properties.title,sheets.properties.sheetId
```
Each element in `sheets[]` has `properties.title` (tab name) and `properties.sheetId` (gid).

### Read cell values

```
GET https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}/values/{{range}}?valueRenderOption={{renderOption}}&dateTimeRenderOption={{dateTimeOption}}
```
- `range`: A1 notation, URL-encoded — e.g. `Sheet1!A1:Z100`
- `valueRenderOption`: `FORMATTED_VALUE` (default), `UNFORMATTED_VALUE`, `FORMULA`
- `dateTimeRenderOption`: `SERIAL_NUMBER` or `FORMATTED_STRING`

### Batch read (multiple ranges)

```
GET https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}/values:batchGet?ranges={{range1}}&ranges={{range2}}
```

### Write cell values

```
PUT https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}/values/{{range}}?valueInputOption={{inputOption}}
Content-Type: application/json
```
- `valueInputOption`: `RAW` (literal) or `USER_ENTERED` (parsed as if typed)

### Append rows

```
POST https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}/values/{{range}}:append?valueInputOption={{inputOption}}&insertDataOption=INSERT_ROWS
Content-Type: application/json
```

### Create spreadsheet

```
POST https://sheets.googleapis.com/v4/spreadsheets
Content-Type: application/json
```
Body: `{"properties": {"title": "{{title}}"}, "sheets": [{"properties": {"title": "{{sheetName}}"}}]}`

### Batch update (formatting, structure)

```
POST https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}:batchUpdate
Content-Type: application/json
```
Body: `{"requests": [...]}`
Request types: `addSheet`, `deleteSheet`, `mergeCells`, `repeatCell`,
`updateSheetProperties`, `autoResizeDimensions`, `addConditionalFormatRule`,
`setBasicFilter`, `sortRange`, `updateBorders`.

**Note**: This is different from `values:batchUpdate` — this handles structure/formatting;
the values endpoint handles cell data.

---

## OAuth Scopes

| API | Full Access | Read-Only |
|-----|-------------|-----------|
| Drive | `https://www.googleapis.com/auth/drive` | `drive.readonly` |
| Docs | `https://www.googleapis.com/auth/documents` | `documents.readonly` |
| Sheets | `https://www.googleapis.com/auth/spreadsheets` | `spreadsheets.readonly` |

For public/published files, `?key={{apiKey}}` works for read operations without OAuth.

## Rate Limits

- Sheets read: 300 requests/min/project
- Sheets write: 60 requests/min/project
- Drive: higher limits, varies by operation

---

## Inja Template Examples for HttpAdapter

### Export a Google Sheet to XLSX (for blobboxes ingestion)

```sql
INSERT INTO domain.http_adapter (name, method, url_template, default_headers, response_notes, source)
VALUES (
    'google_sheet_to_xlsx',
    'get',
    'https://www.googleapis.com/drive/v3/files/{{fileId}}/export?mimeType=application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    '{"Authorization": "Bearer {{access_token}}"}',
    'Returns binary XLSX. Feed to blobboxes bb_xlsx() for bbox extraction.',
    'google:drive:v3'
);
```

### Read sheet values as JSON

```sql
INSERT INTO domain.http_adapter (name, method, url_template, default_headers, response_jmespath, source)
VALUES (
    'google_sheet_values',
    'get',
    'https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}/values/{{range}}?valueRenderOption=FORMATTED_VALUE',
    '{"Authorization": "Bearer {{access_token}}"}',
    'values[]',
    'google:sheets:v4'
);
```

### Get spreadsheet tab list

```sql
INSERT INTO domain.http_adapter (name, method, url_template, default_headers, response_jmespath, source)
VALUES (
    'google_sheet_tabs',
    'get',
    'https://sheets.googleapis.com/v4/spreadsheets/{{spreadsheetId}}?fields=sheets.properties',
    '{"Authorization": "Bearer {{access_token}}"}',
    'sheets[].properties.{title: title, sheetId: sheetId, rowCount: gridProperties.rowCount, columnCount: gridProperties.columnCount}',
    'google:sheets:v4'
);
```
