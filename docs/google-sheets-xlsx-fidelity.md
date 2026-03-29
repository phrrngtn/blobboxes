# Google Sheets → XLSX Export Fidelity

## Summary

For blobboxes' use case (extracting cell positions, text, and formatting as bounding
boxes), exporting Google Sheets to .xlsx is sufficient. No native Google Sheets path
is needed.

## What Survives Export

Everything blobboxes needs:

- **Cell content** (text, numbers, dates, booleans) — yes
- **Cell formatting** (fonts, colors, borders, number formats) — yes
- **Merged cells** — yes
- **Cell dimensions** (row heights, column widths) — yes
- **Multiple sheets/tabs** — yes
- **Named ranges** — yes
- **Data validation** (dropdowns, rules) — mostly yes
- **Basic charts** (bar, line, pie, scatter, combo) — yes
- **Hyperlinks** (external URLs, HYPERLINK formula) — yes
- **Conditional formatting** (simple rules) — mostly yes
- **Floating images** — yes

## What Is Lost

Mostly irrelevant to bbox extraction:

| Feature | Lost? | Impact on blobboxes |
|---------|-------|---------------------|
| `GOOGLEFINANCE`, `IMPORTDATA`, `IMPORTRANGE`, `QUERY` | Yes — become cached values | None — blobboxes sees the displayed value |
| `ARRAYFORMULA` (complex cases) | Flattened to cached values | None |
| `REGEXMATCH/EXTRACT/REPLACE` | Yes — Google-only | None |
| `IMAGE()` formula (in-cell images) | Yes — becomes URL text | **Minor risk** if sheets contain in-cell images |
| `SPARKLINE()` formula | Yes | **Minor risk** — visual element lost |
| Pivot tables | Exported as static data, not live PivotTable | None — visual layout preserved |
| Apps Script / macros | Completely lost | None |
| Filter views (saved per-user) | Lost | None |
| Smart chips (@mentions, linked people/events) | Lost | None |
| Threaded comments | Flattened to single comment | None |
| Alternating row colors | Converted to static fills | None — colors preserved |
| Google-specific chart types (Scorecard, Org, Timeline) | Lost | Minor |
| Protected ranges | Permission model lost (account-based → password-based) | None |

## Size Limits

| Limit | Google Sheets | Excel (.xlsx) |
|-------|---------------|---------------|
| Max cells per spreadsheet | 10,000,000 (across all tabs) | ~17 billion per sheet |
| Max rows per sheet | Varies (limited by cell count) | 1,048,576 |
| Max columns per sheet | 18,278 (col ZZZ) | 16,384 (col XFD) |
| Max characters per cell | 50,000 | 32,767 |

**Risk**: Cells with >32,767 characters may be **silently truncated** on export.
Unlikely for equipment/tabular data but worth noting.

## Recommendation

**Export to .xlsx, feed to blobboxes via xlnt.** The export path is:

```
Google Sheet → Drive API export (mimeType=application/vnd.openxmlformats-officedocument.spreadsheetml.sheet)
            → .xlsx file
            → blobboxes bb_xlsx() / bb_objs()
            → bounding boxes
```

This reuses all existing blobboxes infrastructure with no new code. The only scenario
requiring a native Google Sheets → bboxes path would be heavy use of `IMAGE()` or
`SPARKLINE()` formulas, which is not expected for MEP equipment data.

## Formula Compatibility Quick Reference

| Category | Examples | Survives? |
|----------|----------|-----------|
| Standard math/stat/text | SUM, VLOOKUP, IF, INDEX, MATCH | Yes |
| Dynamic arrays (Excel 365) | XLOOKUP, FILTER, SORT, UNIQUE, LAMBDA | Yes |
| Google-connected services | GOOGLEFINANCE, IMPORTDATA, IMPORTRANGE | No → cached value |
| Google query language | QUERY | No → cached value |
| Google array expansion | ARRAYFORMULA (complex) | Partial → may flatten |
| Google regex | REGEXMATCH, REGEXEXTRACT, REGEXREPLACE | No → cached value |
| Google visual | IMAGE, SPARKLINE | No → lost/URL string |
| Google i18n | DETECTLANGUAGE, GOOGLETRANSLATE | No → cached value |
