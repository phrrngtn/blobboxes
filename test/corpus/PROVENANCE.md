# Legacy `.xls` (BIFF) test corpus — provenance & terms

**TEST-ONLY. DO NOT REDISTRIBUTE IN THE blobboxes PACKAGE.** These files are
third-party spreadsheets pulled in solely to verify the `.xls` extraction lane
(libxls values/formats · our BIFF walker formulas/names · compoundfilereader
CFB · MS-OVBA VBA). The corpus binaries are git-ignored (see `.gitignore`); only
the acquisition script, this file, the golden oracle, and the runner are tracked.
Reconstitute the corpus with `./acquire_corpus.sh`.

The `.xls` VBA storage (`_VBA_PROJECT_CUR`) has the same MS-OVBA layout across
all sources, so any macro-bearing file exercises the (future, M7) decompressor.

## Sources

| Dir           | Source                                             | License / terms                         | Why it's here |
|---------------|----------------------------------------------------|-----------------------------------------|---------------|
| `read-excel/` | https://github.com/igormironchik/read-excel        | MIT                                     | Minimal BIFF8 fixtures for the earliest milestones; `MiscOperatorTests.xls`, `stringformula.xls` (operator/formula coverage). |
| `unxls/`      | https://github.com/kinkou/unxls (`spec/files/`)    | MIT                                     | Spec-driven feature fixtures: BIFF2/3/4/5/7/8 version variants (exercise our BIFF5/7 warning), per-record biff8 files (sst, xf, palette, font, format, hyperlinks, style), and 10 `filepass/` **encrypted** files (exercise FILEPASS skip-don't-fail). |
| `poi/`        | https://github.com/apache/poi (`test-data/spreadsheet/*.xls`, sparse) | Apache-2.0 (test data — see POI NOTICE/LICENSE) | Highest-signal feature suite; each file names a feature: `3dFormulas`, `shared_formulas`/`SharedFormulaTest`/`overlapSharedFormula`, `ContinueRecordProblem`, `external_name`/`multibookFormula*` (external-workbook refs), `named-cell-in-formula`, `MatrixFormulaEvalTestData` (array formulas), `IfFormulaTest`. |
| `enron/`      | https://github.com/SheetJS/enron_xls (mirror: https://sheetjs.github.io/enron_xls/) | EDRM Enron Data Set terms (public, research use). Original: Hermans & Murphy-Hill, https://figshare.com/articles/dataset/Enron_Spreadsheets_and_Emails/1221767 | Real-world "nasty" workbooks (heavy formulas, cross-sheet refs, defined names, scale — one sample is ~10.7 MB). A 15-file spread sampled from the 20,872-file set (2 in `native_001/002/` subdirs failed the sampler; 13 kept). |

## Pending: VBA-bearing Enron selection (blocked on M7)

Only a minority of Enron workbooks contain VBA. The specific macro-bearing files
are enumerated by **Patrick O'Beirne, "VBA in the spreadsheets from the Enron
email corpus", EuSpRIG 2015** —
https://eusprig.org/wp-content/uploads/VBA-in-spreadsheets-from-EnronPOBeirne-2015.pdf

Targeted download of a file with **VBA + defined names + formulas** is deferred
to milestone **M7 (MS-OVBA decompression)**, when a VBA golden can actually be
diffed. Until then the current Enron sample already stresses the value/formula/
name lanes on real workbooks. The `acquire_corpus.sh` script has a commented
`ENRON_VBA_FILES=(...)` slot to fill from the paper at that point.

## Golden oracle

`../golden/` holds expected output. For third-party files the oracle is
**LibreOffice headless** (`soffice --convert-to xlsx`) + openpyxl A1 formula
extraction — run on **dc1** (Ubuntu, `soffice` installed there, not on the
laptop) via `gen_golden_dc1.sh`. For our own authored fixture
(`../../xls_biff/test/formulas.xls`, xlwt) the golden is hand-authored since we
know the formulas exactly. R1C1 is trusted once A1 matches (same expression
tree). See `run_corpus.py` for the pass/fail diff.
