# pdf_overlay — context-sensitive annotations from profiling, SHA-preserving

Closes the extract → profile → annotate loop on a real PDF: `bb_pdf` extracts the
box table, DuckDB SQL imputes a domain per box, and the result is emitted as a
standalone **XFDF sidecar** — the source PDF's bytes are never touched, so its
**SHA256 is invariant** (baking annotations into a copy would mint a new SHA; we
don't). Acrobat/Reader render the `.xfdf` over the PDF via its `<f href>`; viewers
without XFDF support get the same findings as a draw-on-render overlay layer.

```bash
./fetch_sample.sh                                   # public-domain IRS Schedule C
BBOXES_DUCKDB_EXT=/path/to/bboxes.duckdb_extension \
  python overlay.py f1040sc.pdf                     # -> f1040sc.xfdf (source untouched)
```

Notes:
- **Coordinates:** `bb_pdf` is top-left-origin (y down); XFDF is PDF bottom-left,
  so rects flip against page height (`lly=H-(y+h)`, `ury=H-y`). Validated visually.
- **Imputation:** the SQL `CASE` is a stand-in — replace it with a join against
  blobfilters domain fingerprints (catalog/authority) for real domain matching,
  and colour out-of-domain values (residue) as violators.
- Deps: `pypdfium2`, `pillow`, and the `duckdb` CLI with the `bboxes` extension.
