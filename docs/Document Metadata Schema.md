# Document Metadata Schema (blobboxes)

Dialect-specific **metadata** extraction — distinct from *recognition* (what dialect
a blob is; that's a separate, later extension, likely libmagic) and from the
**bboxes stream** (positioned content: cells/text, fonts, page/cell geometry). This
doc specs the *document / structural / provenance* metadata for **PDF** and **Excel**.

## Architecture

- **One scalar extractor per dialect** — `pdf_metadata(path) → JSON`,
  `xlsx_metadata(path) → JSON`. A **full-take, single open/unzip/parse pass**: IO +
  decompression + parsing dominate cost, so pay them once and emit one fat blob.
  Full-take is also what *enables triangulation* — every source co-present in one blob.
- **SQL views/macros reify + triangulate** over the blob (DuckDB table macros;
  SQLite `json_each`; Python `json.loads`). *Scalars produce blobs; SQL reifies.*
  The C surface is *only* the scalar extractors — no C table functions.
- **Dialect is explicit** (you call the named function); recognition is separate.
- **Two exclusions from the blob:**
  - **Do not replicate the bboxes stream** — no text/cells, fonts, or per-page/cell
    geometry (those are `bboxes` / `fonts` / `styles` / `pages`). Metadata only.
  - **Drop truly-voluminous payloads** — external-link *cached cell values*
    (`<sheetDataSet>`): keep link *structure* (target, sheets, referenced addresses),
    drop the value cache. Otherwise full-take.

---

## PDF — `pdf_metadata` (PDFium + pugixml)

PDF metadata is redundant *by design* across several partly-independent sources;
gathering all of them is what makes triangulation possible.

### Sources
| source | via | fields |
|---|---|---|
| **Info dict** | PDFium `FPDF_GetMetaText` | Title, Author, Subject, Keywords, Creator, Producer, CreationDate, ModDate |
| **XMP** | `/Metadata` stream → pugixml | `dc:*`, `xmp:CreatorTool`, `pdf:Producer`, `xmp:CreateDate`/`ModifyDate`/`MetadataDate`, `xmpMM:DocumentID`/`InstanceID`/`OriginalDocumentID`/`DerivedFrom`/`History`, `pdfaid:part`/`conformance` |
| **structural** | PDFium | version, page_count, encrypted + permissions, tagged?, linearized?, has AcroForm/JavaScript/attachments |
| **integrity** | open result | `clean` \| `recovered` \| `failed` (some real files won't open — record it) |

*(Per-page geometry, fonts, and text stay in the bboxes stream — not here.)*

### JSON shape (sketch)
```jsonc
{ "info": {"title":…,"author":…,"creator":…,"producer":…,"created":…,"modified":…},
  "xmp":  {"creator_tool":…,"producer":…,"created":…,"modified":…,
           "document_id":…,"instance_id":…,"derived_from":…,"history":[…],"pdfa":…},
  "structural": {"version":"1.4","page_count":12,"encrypted":false,"permissions":…,
                 "tagged":false,"linearized":true,"has_forms":false,"attachments":0},
  "integrity": {"status":"clean"} }
```

### Triangulation (macros over the blob)
Grounded in real test files (`pmc_clinical` = InDesign→Adobe PDF Library, edited,
+05:30, full XMP lineage; `financial_statement` = ReportLab, Create==Mod, no XMP):
- **`pdf_provenance(p)`** — `creator` + `producer` → pipeline class: *ReportLab* =
  programmatic/born-machine; *InDesign → Adobe PDF Library* = desktop-publishing;
  → human-vs-machine, tool-by-tool corpus clustering.
- **`pdf_corroboration(p)`** — Info vs XMP agreement on title/producer/dates →
  `corroborated` \| `divergent` (edited by a different tool than made it) \| `xmp_absent`.
- **`pdf_temporal(p)`** — Create vs Mod (edited after creation?), **timezone offset**
  (geography), internal dates vs filesystem `mtime`.
- **`pdf_lineage(p)`** — `xmpMM:DocumentID`/`InstanceID` (dedup) + `DerivedFrom`/
  `OriginalDocumentID`/`History` (edit chain).
- **`pdf_integrity(p)`** — the open verdict.

---

## Excel — `xlsx_metadata` (unzip + pugixml; **no xlnt**)

Framed by the **VBA object model**: whatever a `Workbook` / `Worksheet` / `VBProject`
exposes as a property is metadata, and each has an at-rest home in an OOXML part.
None of this needs the (optional, off-by-default) xlnt cell backend.

### Workbook-level (`Workbook` object → blob top-level)
| VBA object-model | at-rest OOXML part |
|---|---|
| `BuiltinDocumentProperties` | `docProps/core.xml` (title, creator, lastModifiedBy, created, modified, revision, …) |
| `Application` / version / `Company` / `Manager` | `docProps/app.xml` (**Application, AppVersion**, Company, DocSecurity, TitlesOfParts) |
| `CustomDocumentProperties` | `docProps/custom.xml` |
| `Names` (defined names) | `xl/workbook.xml` `<definedNames>` (name, refersTo, scope, visible) |
| `Sheets` / `Worksheets` | `xl/workbook.xml` `<sheets>` |
| `ActiveSheet` / `Windows` | `xl/workbook.xml` `<bookViews>` (`activeTab`) |
| `Date1904` (**date system**) | `workbook.xml` `<workbookPr date1904=>` |
| `Calculation` mode / `CalculationVersion` | `workbook.xml` `<calcPr>` |
| `ProtectStructure` / `ProtectWindows` | `workbook.xml` `<workbookProtection>` |
| `LinkSources(xlExcelLinks)` | `xl/externalLinks/*` (+ `_rels`) — structure only |
| `Connections` / `QueryTables` | `xl/connections.xml` |
| `XmlMaps` | `xl/xmlMaps.xml` |
| `Styles` (named cell styles) | `xl/styles.xml` `<cellStyles>` |
| `HasVBProject` / `VBProject` | `xl/vbaProject.bin` present? (modules/refs → below) |
| `FileFormat` | extension / `[Content_Types].xml` (xlsx/xlsm/xlsb) |
| encryption / `ReadOnly` | encrypted-OLE wrapper (record "encrypted") |

### Worksheet-level (`Worksheet` object → blob `sheets[]`)
| VBA | at-rest |
|---|---|
| `Name` | `sheet@name` |
| **`CodeName`** (VBA name ≠ display name) | `sheetPr@codeName` |
| `Visible` (Visible/Hidden/**VeryHidden**) | `sheet@state` |
| `Type` (Worksheet/Chart/Macro) | worksheet vs chartsheet part |
| `Tab.Color` | `sheetPr <tabColor>` |
| `UsedRange` | `<dimension ref=>` |
| `ProtectContents` | `<sheetProtection>` |
| `AutoFilter` | `<autoFilter ref=>` |
| `ListObjects` (Tables) | `xl/tables/*` |
| `PivotTables` | `xl/pivotTables/*` |
| `Hyperlinks` | `<hyperlinks>` |
| `Comments` / notes | `xl/comments*`, `threadedComments` |
| `Validation` | `<dataValidations>` |
| conditional formats | `<conditionalFormatting>` |
| `PageSetup` (PrintArea, orientation, header/footer) | `<pageSetup>`, `_xlnm.Print_Area` name |
| freeze/split panes | `<sheetView><pane>` |
| outline (grouping) levels | `<outlinePr>`, row/col `outlineLevel` |

### VBA project — surface, don't parse
`vbaProject.bin` is an OLE2/CFB container. blobboxes deliberately does **not** parse it
(no in-house CFB/MS-OVBA code); that is left to a mature downstream library
(`olefile` / `oletools`). blobboxes provides two things:
| field / fn | what | why |
|---|---|---|
| `vba.present` | `vbaProject.bin` present | trivial flag |
| `vba.size` | byte size of the bin | near-match signal |
| **`vba.sha1`** | SHA-1 of the bin | **JOIN/cluster identical VBA across a corpus** without shipping bytes |
| **`xlsx_vba_base64(p)`** | the raw bin, base64 | the "get it out" hook → `from_base64()` → BLOB → feed `olefile` |

Downstream (Python `oletools`) does modules / references (GUIDs) / source — the symbol
table and porting analysis live there, over the bytes blobboxes hands out.

### JSON shape (sketch)
```jsonc
{ "core": {"title":…,"creator":…,"last_modified_by":…,"created":…,"modified":…,"revision":…},
  "app":  {"application":"Microsoft Excel","app_version":"16.0300","company":…},
  "custom": {…},
  "workbook": {"date1904":false,"calc_mode":"auto","protection":…,"active_tab":0,"sheet_count":3},
  "sheets": [{"name":"Jan","code_name":"Sheet1","state":"visible","type":"worksheet",
              "tab_color":…,"dimension":"A1:H120","has_autofilter":true,"protected":false}],
  "names":  [{"name":"SalesTotal","refers_to":"Jan!$H$120","scope":"workbook","visible":true}],
  "external_links": [{"target":"[Budget.xlsx]","sheets":["Q1"],"names":[…],"cells":["A1"]}],
  "connections": [...], "tables": [...],
  "vba": {"present":true,"size":32256,"sha1":"b5db7a6b…"} }   // parse bytes downstream
```

### Triangulation (macros over the blob)
- **`xlsx_provenance(p)`** — `app.Application`+`AppVersion` (which Excel), `core.creator`
  vs `lastModifiedBy` (author vs last editor), `created` vs `modified` (edit history).
- **`xlsx_consistency(p)`** — `app.xml` sheet-count/`TitlesOfParts` **vs** actual
  `workbook.xml` sheets (cross-check); `code_name` vs `name` drift (renamed in UI,
  code still refers to CodeName).
- **`xlsx_date_system(p)`** — surface `date1904` (it silently reinterprets *every*
  date; a validation input for any date logic).
- **`xlsx_dependencies(p)`** — `has_vba` + `vba.references` + `external_links` +
  `connections` → the workbook's full external/code dependency surface (porting &
  risk). "Has code (deps X), external data (links Y)."
- reifiers: **`xlsx_worksheets(p)` / `xlsx_names(p)` / `xlsx_links(p)` / `xlsx_tables(p)`
  / `xlsx_vba_refs(p)`** — unnest the tabular arrays.

### Reification (DuckDB table macro, example)
```sql
CREATE MACRO xlsx_worksheets(p) AS TABLE
  SELECT s.name, s.code_name, s.state, s.type, s.dimension
  FROM (SELECT unnest(from_json(xlsx_metadata(p) -> '$.sheets', '["json"]')) AS s);
```

---

## Phasing

1. **Phase 1 (cheap, no new deps):** PDF Info-dict + structural + integrity; Excel
   core/app/custom + workbook props + the tabular arrays (worksheets/names/links/
   tables). All via PDFium / unzip+pugixml.
2. **Phase 2:** PDF **XMP** parse (`/Metadata` → pugixml); Excel **VBA module list +
   references** (OLE `dir` stream). The provenance/dependency richness.
3. **Phase 3 (separate concern):** VBA **source** extraction (OLE + VBA
   decompression) — the porting payload, not metadata.
