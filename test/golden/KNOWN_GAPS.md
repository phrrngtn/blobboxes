# Known `.xls` formula-parser gaps (golden baseline)

The corpus golden diff (`run_corpus.py --golden-only`, oracle = LibreOffice A1)
currently confirms **9 files exact** and flags **13 files** with gaps. The 13 are
listed in `known_gaps.txt` so the harness treats them as expected and only fails
on *new* regressions. Each is one of the following, ranked by effort. This is the
prioritized worklist for the next `.xls` formula milestones.

## 1. Shared formulas — `PtgExp` (0x01) → `ShrFmla` (0x04BC)  *(biggest)*
Files: `SharedFormulaTest`, `ex47747-sharedFormula`, `overlapSharedFormula`,
`shared_formulas`, `enron_3.264848`, `enron_3.1131604` (dominates their mismatch
counts).

Excel stores a formula once (a `ShrFmla` record with a base rgce over a cell
range); member cells hold a `Formula` record whose rgce is just `PtgExp` pointing
at the shared group's top-left. We currently emit empty for those cells. Fix:
first pass to collect `ShrFmla` (range + base rgce) and `Array` (0x0221) records
per sheet, then on a `Formula` whose rgce is a lone `PtgExp`, render the shared
base rgce with the member cell as the home cell (relative refs re-home per cell).
`PtgExp` also anchors array formulas, hence the 6 "missing" in `biff8/cells`.

## 2. Multi-sheet 3D range — `Sheet2:Sheet5!A1`
File: `FormulaSheetRange` (`SUM(Sheet2:Sheet5!A12:C12)` vs our `SUM(Sheet2!A12:C12)`).

`ExternSheet` XTI carries `itabFirst` *and* `itabLast`; we keep only `itabFirst`
(`Globals::xti_first`). Fix: also store `xti_last`, and in `sheet_qual` render
`first:last!` (quoting the pair) when they differ. Also collapse `A11:A11` → `A11`
for single-cell areas to match the oracle.

## 3. External-workbook references — `[1]Sheet1!A1`
Files: `multibookFormulaA`, `multibookFormulaB`, part of `external_name`.

When a 3D ref's `SupBook` is an external workbook (not the self-referencing
`<same>` SupBook), Excel prefixes `[n]` (a 1-based external-workbook index).
We currently resolve every XTI to a *local* sheet name. Fix: parse `SupBook`
(0x01AE) records, mark which are external vs self, thread the SupBook index
through `xti_first`, and emit the `[n]` prefix + the external sheet name.

## 4. Ftab completeness
File: `FormulaEvalTestData` (990 mismatches — a deliberate all-functions file),
some of `FormulaRefs`.

Unknown function indices render as `FUNC<n>`. Extend `ftab()` from
`[MS-XLS] 2.5.198.17`. (Already added: `SUBSTITUTE`=120, `FIND`=124.) Low risk
per entry but must be exact — a wrong mapping is caught by this same golden diff.

---
Regenerate the oracle after any fix: `bash test/gen_golden_dc1.sh` (LibreOffice on
dc1), then `run_corpus.py --golden-only`. When a listed file starts passing, the
runner prints "BASELINE NOW PASSES" — remove it from `known_gaps.txt`.
