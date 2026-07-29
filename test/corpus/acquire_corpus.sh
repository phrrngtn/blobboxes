#!/usr/bin/env bash
# Reconstitute the legacy-.xls test corpus (git-ignored binaries). See PROVENANCE.md.
# Idempotent-ish: skips a source dir if it already exists.
set -euo pipefail
cd "$(dirname "$0")"

echo "== read-excel (MIT) =="
[ -d read-excel ] || git clone --depth 1 -q https://github.com/igormironchik/read-excel.git read-excel

echo "== unxls (MIT) =="
[ -d unxls ] || git clone --depth 1 -q https://github.com/kinkou/unxls.git unxls

echo "== apache/poi test-data/spreadsheet/*.xls (Apache-2.0, sparse) =="
if [ ! -d poi ]; then
  git clone --depth 1 --filter=blob:none --sparse -q https://github.com/apache/poi.git poi
  ( cd poi && git sparse-checkout set --no-cone '/test-data/spreadsheet/*.xls' )
fi

echo "== enron sample (EDRM terms; 15-file spread of 20,872) =="
if [ ! -d enron ]; then
  mkdir -p enron
  curl -s "https://api.github.com/repos/SheetJS/enron_xls/git/trees/HEAD?recursive=1" \
    | grep -oE '"path": "[^"]+\.xls"' | sed 's/"path": "//;s/"$//' > /tmp/enron_all.txt
  total=$(wc -l < /tmp/enron_all.txt)
  awk -v t="$total" 'NR % int(t/15) == 1' /tmp/enron_all.txt | head -15 > /tmp/enron_sample.txt
  while read -r f; do
    enc=$(echo "$f" | sed 's/%2F/%252F/g')            # %2F is literal in the filename → double-encode
    short=$(echo "$f" | sed 's|edrm/native_000%2F||; s|\.xls$||' | cut -d. -f1-2)
    curl -sL -o "enron/enron_${short}.xls" \
      "https://raw.githubusercontent.com/SheetJS/enron_xls/HEAD/$enc" || true
  done < /tmp/enron_sample.txt
  find enron -size 0 -delete
fi

# M7 TODO: add VBA-bearing Enron files (see PROVENANCE.md — O'Beirne EuSpRIG 2015).
# ENRON_VBA_FILES=( "edrm/native_000%2F3.XXXXXX.<HASH>.1.xls" )

echo "== inventory =="
for d in read-excel unxls poi enron; do
  printf '  %-12s %s .xls\n' "$d" "$(find "$d" -iname '*.xls' 2>/dev/null | wc -l | tr -d ' ')"
done
