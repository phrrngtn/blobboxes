#!/usr/bin/env bash
# Generate golden oracle for the formula/name-relevant corpus subset, using
# LibreOffice headless on dc1 (soffice is installed there, not on the laptop).
#
# Pipeline: flatten each selected .xls to its golden key -> scp to dc1 ->
# soffice --convert-to xlsx -> openpyxl extract (gen_golden.py) -> scp the
# small JSON goldens back to test/golden/. The .xlsx never returns to the laptop.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"          # .../blobboxes/test
CORPUS="$HERE/corpus"
GOLDEN="$HERE/golden"
DC1=phrrngtn@dc1
REMOTE=/tmp/xls_golden
mkdir -p "$GOLDEN"

# Curated selection: files that actually carry formulas and/or defined names —
# the only files for which a golden diff is meaningful. Globs are corpus-relative.
SELECT=(
  poi/test-data/spreadsheet/3dFormulas.xls
  poi/test-data/spreadsheet/shared_formulas.xls
  poi/test-data/spreadsheet/SharedFormulaTest.xls
  poi/test-data/spreadsheet/overlapSharedFormula.xls
  poi/test-data/spreadsheet/ex47747-sharedFormula.xls
  poi/test-data/spreadsheet/external_name.xls
  poi/test-data/spreadsheet/multibookFormulaA.xls
  poi/test-data/spreadsheet/multibookFormulaB.xls
  poi/test-data/spreadsheet/named-cell-in-formula-test.xls
  poi/test-data/spreadsheet/named-cell-test.xls
  poi/test-data/spreadsheet/namedinput.xls
  poi/test-data/spreadsheet/IfFormulaTest.xls
  poi/test-data/spreadsheet/FormulaRefs.xls
  poi/test-data/spreadsheet/FormulaSheetRange.xls
  poi/test-data/spreadsheet/FormulaEvalTestData.xls
  read-excel/test/data/MiscOperatorTests.xls
  read-excel/test/data/stringformula.xls
  unxls/spec/files/biff8/cells.xls
  enron/enron_3.264848.xls
  enron/enron_3.1131604.xls
)
FIXTURE="$HERE/../../xls_biff/test/formulas.xls"   # our authored xlwt fixture

echo "== staging + flattening selection =="
STAGE=$(mktemp -d)
push_one() { # <abs-src> <flatkey>
  cp "$1" "$STAGE/$2.xls"
}
for rel in "${SELECT[@]}"; do
  src="$CORPUS/$rel"
  [ -f "$src" ] || { echo "  MISSING $rel"; continue; }
  push_one "$src" "$(echo "$rel" | sed 's/\.xls$//; s|/|__|g')"   # canonical key: strip .xls, slash->__
done
[ -f "$FIXTURE" ] && push_one "$FIXTURE" "fixtures__formulas"
echo "  staged $(ls "$STAGE" | wc -l | tr -d ' ') files"

echo "== ship to dc1 =="
ssh "$DC1" "rm -rf $REMOTE && mkdir -p $REMOTE/in $REMOTE/xlsx $REMOTE/golden"
scp -q "$STAGE"/*.xls "$DC1:$REMOTE/in/"
scp -q "$HERE/gen_golden.py" "$DC1:$REMOTE/gen_golden.py"

echo "== soffice convert + openpyxl extract on dc1 =="
ssh "$DC1" bash -s <<REMOTE_EOF
set -e
cd $REMOTE
soffice --headless -env:UserInstallation=file:///tmp/lo_profile \
        --convert-to xlsx --outdir xlsx in/*.xls >/dev/null 2>&1 || true
echo "  converted \$(ls xlsx/*.xlsx 2>/dev/null | wc -l) xlsx"
[ -d venv ] || python3 -m venv venv
./venv/bin/pip -q install --disable-pip-version-check openpyxl >/dev/null 2>&1
./venv/bin/python gen_golden.py xlsx golden
REMOTE_EOF

echo "== pull goldens back =="
scp -q "$DC1:$REMOTE/golden/*.json" "$GOLDEN/"
rm -rf "$STAGE"
echo "  golden files: $(ls "$GOLDEN"/*.formulas.json 2>/dev/null | wc -l | tr -d ' ')"
