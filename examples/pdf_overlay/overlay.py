#!/usr/bin/env python3
"""Context-sensitive annotation overlay for a PDF, driven by blobboxes bb_pdf
extraction + SQL domain imputation, emitted as a standalone XFDF *sidecar*.

The source PDF is NEVER modified: annotations live in a separate .xfdf that
references the PDF via <f href>, so the document's SHA256 is invariant. (Baking
annotations into a copy would mint a new SHA — deliberately avoided.)

Coordinate note: bb_pdf returns TOP-LEFT-origin boxes (x,y,w,h, y down).
XFDF rects are PDF user space (bottom-left), so we flip against page height:
  llx=x, urx=x+w, lly=H-(y+h), ury=H-y.
"""
import argparse, json, hashlib, html, os, subprocess, sys
from collections import Counter
import pypdfium2 as pdfium
from PIL import ImageDraw

COLOR = {"year":"#2563eb","ein":"#dc2626","currency":"#16a34a","number":"#0d9488",
         "lineref":"#ea580c","integer":"#7c3aed","text":"#94a3b8"}

# bb_pdf extract + SQL domain imputation (a stand-in for catalog/authority
# matching via blobfilters; swap the CASE for a join against domain fingerprints).
SQL = r"""
LOAD '__EXT__';
COPY (
  SELECT page_id, x, y, w, h, text,
    CASE
      WHEN regexp_matches(text, '^\d{4}$')              THEN 'year'
      WHEN regexp_matches(text, '^\d{2}-?\d{7}$')       THEN 'ein'
      WHEN regexp_matches(text, '^\$[\d,]+(\.\d{2})?$') THEN 'currency'
      WHEN regexp_matches(text, '^[\d,]+\.\d+$')        THEN 'number'
      WHEN regexp_matches(text, '^\d{1,2}[a-z]?$')      THEN 'lineref'
      WHEN regexp_matches(text, '^\d+$')                THEN 'integer'
      ELSE 'text'
    END AS domain
  FROM bb_pdf('__PDF__')
  WHERE text IS NOT NULL AND trim(text) <> ''
) TO '__OUT__' (FORMAT json, ARRAY true);
"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("--ext", default=os.environ.get("BBOXES_DUCKDB_EXT"),
                    help="path to bboxes.duckdb_extension (or set BBOXES_DUCKDB_EXT)")
    ap.add_argument("--out", default=None, help="output .xfdf path")
    a = ap.parse_args()
    if not a.ext:
        sys.exit("need --ext or BBOXES_DUCKDB_EXT (path to bboxes.duckdb_extension)")
    out = a.out or os.path.splitext(a.pdf)[0] + ".xfdf"
    boxes_json = os.path.splitext(a.pdf)[0] + ".boxes.json"

    sha_before = hashlib.sha256(open(a.pdf, "rb").read()).hexdigest()

    sql = SQL.replace("__EXT__", a.ext).replace("__PDF__", a.pdf).replace("__OUT__", boxes_json)
    subprocess.run(["duckdb", "-unsigned", "-c", sql], check=True)
    boxes = json.load(open(boxes_json))

    pdf = pdfium.PdfDocument(a.pdf)
    H = [pdf[i].get_size()[1] for i in range(len(pdf))]
    ann = []
    for b in boxes:
        p, x, y, w, h, dom = b["page_id"], b["x"], b["y"], b["w"], b["h"], b["domain"]
        c = COLOR.get(dom, "#94a3b8")
        llx, urx, lly, ury = x, x + w, H[p] - (y + h), H[p] - y
        ann.append(f'<square page="{p}" rect="{llx:.2f},{lly:.2f},{urx:.2f},{ury:.2f}" '
                   f'color="{c}" interior-color="{c}" opacity="0.30" width="0.75" title="{dom}">'
                   f'<contents>{html.escape(dom)}: {html.escape(b["text"][:40])}</contents></square>')
    open(out, "w").write('<?xml version="1.0" encoding="UTF-8"?>\n'
        '<xfdf xmlns="http://ns.adobe.com/xfdf/" xml:space="preserve">\n'
        f'<f href="{os.path.basename(a.pdf)}"/>\n<annots>\n' + "\n".join(ann) + "\n</annots>\n</xfdf>\n")

    sha_after = hashlib.sha256(open(a.pdf, "rb").read()).hexdigest()
    assert sha_before == sha_after, "source PDF changed!"
    print(f"{len(boxes)} annotations -> {out}")
    print("domains:", dict(Counter(b["domain"] for b in boxes)))
    print(f"source SHA256 {sha_before[:16]}… PRESERVED (byte-identical)")

if __name__ == "__main__":
    main()
