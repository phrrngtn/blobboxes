#!/usr/bin/env python3
"""Golden oracle extractor — the LibreOffice/openpyxl side.

Given a directory of .xlsx files (produced by `soffice --headless --convert-to
xlsx` from the corpus .xls), emit, per file, the expected A1 formula strings
and defined names as flat JSON goldens. The .xlsx stem is the golden key
(corpus-relative path flattened with '__'), so run_corpus.py finds it directly.

Runs wherever openpyxl + the converted xlsx live (we do this on dc1, next to
soffice, then scp the small JSON back). Usage:
    python3 gen_golden.py <xlsx_dir> <golden_out_dir>
"""
import json, sys
from pathlib import Path
import openpyxl


def a1_formulas(wb):
    """{ 'SheetName!A1': '=FORMULA' } for every cell openpyxl reports as a formula."""
    out = {}
    for ws in wb.worksheets:
        for row in ws.iter_rows():
            for cell in row:
                v = cell.value
                if isinstance(v, str) and v.startswith("="):
                    out[f"{ws.title}!{cell.coordinate}"] = v
    return out


def defined_names(wb):
    """{ name: refers_to }. openpyxl 3.x exposes wb.defined_names as a dict-like."""
    out = {}
    try:
        dn = wb.defined_names
        items = dn.items() if hasattr(dn, "items") else [(d.name, d) for d in dn]
        for name, d in items:
            out[name] = getattr(d, "value", getattr(d, "attr_text", str(d)))
    except Exception as e:
        out["__error__"] = str(e)
    return out


def main():
    xlsx_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = err = 0
    for xlsx in sorted(xlsx_dir.glob("*.xlsx")):
        stem = xlsx.stem  # == golden key
        try:
            wb = openpyxl.load_workbook(xlsx, data_only=False, read_only=True, keep_links=True)
            (out_dir / f"{stem}.formulas.json").write_text(json.dumps(a1_formulas(wb), indent=0))
            (out_dir / f"{stem}.names.json").write_text(json.dumps(defined_names(wb), indent=0))
            ok += 1
        except Exception as e:
            print(f"  ERR {stem}: {e}", file=sys.stderr)
            err += 1
    print(f"gen_golden: {ok} ok, {err} err")


if __name__ == "__main__":
    main()
