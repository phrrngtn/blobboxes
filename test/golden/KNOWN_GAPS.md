# `.xls` formula-parser: status & known gaps (golden baseline)

Corpus golden diff (`run_corpus.py --golden-only`, oracle = LibreOffice A1).
**17 of 21 golden files exact; 2339 / 2723 formulas match (86%).** The 4 files
still in `known_gaps.txt` are treated as expected so the harness only fails on
new regressions.

## Done (were gaps, now correct)
- **PtgName** → resolved defined name (was `Name#`).
- **Shared formulas** — `PtgExp` (0x01) → `ShrFmla` (0x04BC), keyed by the
  **anchor** Formula cell (not the ref corner); relative refs re-homed per member.
- **Relative column offset** — BIFF8 stores it as a signed **8-bit** low byte
  (`0xff` = −1); the old 14-bit sign-extension mislabeled every relative-col ref.
- **3D refs** — multi-sheet ranges `Sheet2:Sheet5!` and external-workbook
  `[n]Sheet!` (ExternSheet XTIs + SupBook parse); cross-sheet single ref → area form.
- **Ftab** — replaced with the canonical [MS-XLS] table (fixed a silent **off-by-3**
  that rendered ABS→EXP, ROUND→ABS, INT→LN, MID/LEN/LOOKUP/INDEX wrong).
- **PtgFunc arg counts** (`ftab_argc`) — 0-arg (TODAY/PI/NA/NOW/…) no longer
  underflow; common fixed 2/3-arg (ROUNDUP, LARGE, SMALL, MIRR, …) no longer truncate.
- **PtgBool** rendered `TRUE()`/`FALSE()` to match the oracle.

## Remaining gaps (baselined)
1. **`FormulaEvalTestData`** (119 mismatch) — the exhaustive all-functions file.
   Residual is the **Analysis-ToolPak / future-function** tail (`DEC2HEX`,
   `HEX2DEC`, `OCT2DEC`, `BIN2DEC`, `QUOTIENT`, `FACTDOUBLE`, `WEEKNUM`, …),
   stored via the `_xlfn`/add-in mechanism our Ptg walker doesn't decode
   (renders empty). Low value — these don't occur in normal workbooks.
2. **`enron_3.264848`** — a **BIFF5/7** workbook; the 3D-ref record layout differs
   from BIFF8 (we emit the documented `biff_version` warning rather than misparse).
3. **`FormulaRefs`** (1 mismatch) — sheet + cell correct; only the external-book
   `[n]` index differs from LibreOffice's own external-link numbering (cosmetic).
4. **`unxls/.../cells`** (6 "missing") — LibreOffice-conversion artifacts in the
   oracle (`"="` empty formulas; `TRUE()`/`FALSE()` stored as literals), not real.

Regenerate the oracle after any fix: `bash test/gen_golden_dc1.sh` (LibreOffice on
dc1), then `run_corpus.py --golden-only`. A baselined file that starts passing is
reported as "BASELINE NOW PASSES" — remove it from `known_gaps.txt`.
