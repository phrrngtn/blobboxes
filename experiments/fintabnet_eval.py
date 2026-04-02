"""Evaluate scatter_pipeline against FinTabNet ground truth.

For each PDF + annotation pair:
1. Run our pipeline to get classified cells
2. Compare against FinTabNet ground truth:
   - Cell text matching (do we find the same text?)
   - Role classification (header vs data)
   - Table detection (do we find tables where they exist?)

Metrics:
- Text recall: fraction of GT cell texts found in our output
- Header precision/recall: agreement on is_column_header
- Numeric recall: fraction of GT numeric cells we classify as measure-like
- Table detection: did we find a table overlapping the GT bbox?
"""

import duckdb
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, "/Users/paulharrington/checkouts/blobboxes/experiments")

EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
DATA_DIR = Path("/Users/paulharrington/checkouts/blobboxes/test_data/fintabnet")
PDF_DIR = DATA_DIR / "pdf"
ANN_DIR = DATA_DIR / "annotations"

# Numeric pattern: matches things like $1,171 / 2,105 / (372) / 18.5% / —
NUMERIC_RE = re.compile(r'^[\$\(\)]?[\d,]+\.?\d*[\)\%]?$|^—$|^\([\d,]+\.?\d*\)$')


def normalize_text(t):
    if not t:
        return ""
    return " ".join(t.split()).strip().lower()


def strip_numeric(t):
    """Strip $, commas, parens, %, spaces for numeric comparison."""
    return re.sub(r'[\$,\s\(\)%]', '', t).strip()


def is_numeric_text(t):
    """Is this GT cell text a numeric value?"""
    t = t.strip()
    if not t or t == '—':
        return True  # em-dash = null/zero in financial tables
    return bool(NUMERIC_RE.match(t))


def texts_match(gt_norm, our_norm):
    """Fuzzy text match between GT and our cell."""
    if not gt_norm or not our_norm:
        return False
    # Exact substring
    if gt_norm == our_norm:
        return True
    if gt_norm in our_norm or our_norm in gt_norm:
        # Guard against short strings matching inside longer ones
        # e.g., "25" matching "2,251" — require the match to be substantial
        shorter = min(len(gt_norm), len(our_norm))
        longer = max(len(gt_norm), len(our_norm))
        if shorter >= 3 or shorter / longer > 0.5:
            return True
    # Numeric normalization: strip $, commas, etc.
    gt_num = strip_numeric(gt_norm)
    our_num = strip_numeric(our_norm)
    if gt_num and our_num and gt_num == our_num:
        return True
    return False


def evaluate_one(pdf_path, ann_path, con):
    name = pdf_path.stem

    with open(ann_path) as f:
        tables_gt = json.load(f)

    if not tables_gt:
        return {"name": name, "status": "no_gt_tables"}

    gt_cells = []
    for ti, table in enumerate(tables_gt):
        for cell in table.get("cells", []):
            text = cell.get("pdf_text_content", "").strip()
            bbox = cell.get("pdf_bbox", [])
            if len(bbox) != 4:
                continue
            gt_cells.append({
                "table_idx": ti,
                "text": text,
                "text_norm": normalize_text(text),
                "bbox": bbox,
                "is_header": cell.get("is_column_header", False),
                "is_row_header": cell.get("is_projected_row_header", False),
                "is_numeric": is_numeric_text(text),
                "row_nums": cell.get("row_nums", []),
                "col_nums": cell.get("column_nums", []),
            })

    if not gt_cells:
        return {"name": name, "status": "no_gt_cells"}

    # Run pipeline
    try:
        con.execute(f"CREATE OR REPLACE TEMP TABLE raw_bboxes AS SELECT * FROM bb('{pdf_path}')")
        con.execute(f"CREATE OR REPLACE TEMP TABLE raw_styles AS SELECT * FROM bb_styles('{pdf_path}')")
        con.execute(f"CREATE OR REPLACE TEMP TABLE raw_fonts AS SELECT * FROM bb_fonts('{pdf_path}')")
        con.execute(f"CREATE OR REPLACE TEMP TABLE raw_pages AS SELECT * FROM bb_pages('{pdf_path}')")
    except Exception as e:
        return {"name": name, "status": f"extract_error: {e}"}

    n_raw = con.execute("SELECT COUNT(*) FROM raw_bboxes").fetchone()[0]
    if n_raw == 0:
        return {"name": name, "status": "no_bboxes"}

    from scatter_pipeline import run_pipeline
    try:
        run_pipeline(str(pdf_path), con)
    except Exception as e:
        return {"name": name, "status": f"pipeline_error: {e}"}

    try:
        our_cells = con.execute("""
            SELECT page_id, row_cluster, cell_id, x, y, w, h, text, role,
                   pass1_role, winning_tag
            FROM classified
            ORDER BY page_id, row_cluster, x
        """).fetchdf()
    except Exception as e:
        return {"name": name, "status": f"query_error: {e}"}

    if len(our_cells) == 0:
        return {"name": name, "status": "no_classified"}

    our_bboxes = []
    for _, c in our_cells.iterrows():
        our_bboxes.append({
            "text": c['text'],
            "text_norm": normalize_text(c['text']),
            "role": c['role'],
        })

    # ── Metrics ──────────────────────────────────────────────────────

    # 1. Text recall: fraction of GT cell texts found in our output
    gt_with_content = [g for g in gt_cells if g["text_norm"]]
    text_found = 0
    for gt in gt_with_content:
        for ours in our_bboxes:
            if texts_match(gt["text_norm"], ours["text_norm"]):
                text_found += 1
                break
    text_recall = text_found / len(gt_with_content) if gt_with_content else 0

    # 2. Header classification
    gt_headers = [g for g in gt_cells if g["is_header"] and g["text_norm"]]
    gt_non_headers = [g for g in gt_cells if not g["is_header"] and g["text_norm"]]

    header_tp = header_fn = header_fp = 0
    for gt in gt_headers:
        matched = False
        for ours in our_bboxes:
            if texts_match(gt["text_norm"], ours["text_norm"]):
                if ours["role"] == "col_header":
                    matched = True
                break
        if matched:
            header_tp += 1
        else:
            header_fn += 1

    for gt in gt_non_headers:
        for ours in our_bboxes:
            if texts_match(gt["text_norm"], ours["text_norm"]):
                if ours["role"] == "col_header":
                    header_fp += 1
                break

    header_precision = header_tp / (header_tp + header_fp) if (header_tp + header_fp) > 0 else 0
    header_recall = header_tp / (header_tp + header_fn) if (header_tp + header_fn) > 0 else 0

    # 3. Table detection
    n_tables_found = con.execute("SELECT COUNT(*) FROM table_regions").fetchone()[0]
    n_gt_tables = len(tables_gt)

    # 4. Numeric recall: of GT cells that ARE numeric, how many did we classify as measures?
    gt_numeric = [g for g in gt_cells
                  if not g["is_header"] and not g["is_row_header"]
                  and g["text_norm"] and g["is_numeric"]]
    numeric_found = 0
    numeric_missed = []
    for gt in gt_numeric:
        matched_as_measure = False
        matched_role = None
        for ours in our_bboxes:
            if texts_match(gt["text_norm"], ours["text_norm"]):
                matched_role = ours["role"]
                if ours["role"] in ("measure", "currency", "percentage", "date", "iso_date"):
                    matched_as_measure = True
                break
        if matched_as_measure:
            numeric_found += 1
        else:
            numeric_missed.append((gt["text"], matched_role))

    numeric_recall = numeric_found / len(gt_numeric) if gt_numeric else 0

    # 5. Row label recall: of GT row headers, how many did we find as row_label/section_label?
    gt_row_headers = [g for g in gt_cells if g["is_row_header"] and g["text_norm"]]
    rl_found = 0
    for gt in gt_row_headers:
        for ours in our_bboxes:
            if texts_match(gt["text_norm"], ours["text_norm"]):
                if ours["role"] in ("row_label", "section_label"):
                    rl_found += 1
                break
    rl_recall = rl_found / len(gt_row_headers) if gt_row_headers else 0

    winning_tag = our_cells['winning_tag'].iloc[0] if 'winning_tag' in our_cells.columns else 'unknown'

    return {
        "name": name,
        "status": "ok",
        "n_gt_cells": len(gt_cells),
        "n_gt_tables": n_gt_tables,
        "n_our_cells": len(our_cells),
        "n_tables_found": n_tables_found,
        "n_raw_bboxes": n_raw,
        "text_recall": round(text_recall, 3),
        "header_precision": round(header_precision, 3),
        "header_recall": round(header_recall, 3),
        "numeric_recall": round(numeric_recall, 3),
        "rl_recall": round(rl_recall, 3),
        "winning_tag": winning_tag,
        "n_gt_headers": len(gt_headers),
        "n_gt_numeric": len(gt_numeric),
        "n_gt_row_headers": len(gt_row_headers),
        "numeric_missed": numeric_missed[:5],  # sample
    }


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")

    with open(DATA_DIR / "manifest.json") as f:
        manifest = json.load(f)

    results = []
    errors = []

    print(f"\n{'Name':<35s} {'GT':>4s} {'Ours':>5s} {'Tbls':>4s} "
          f"{'TxtR':>5s} {'HdrP':>5s} {'HdrR':>5s} {'NumR':>5s} {'RLR':>5s} {'Tag':<20s}")
    print("─" * 120)

    for key, entry in manifest.items():
        pdf_path = PDF_DIR / entry["pdf"]
        ann_path = ANN_DIR / entry["annotation"]
        if not pdf_path.exists() or not ann_path.exists():
            continue

        r = evaluate_one(pdf_path, ann_path, con)
        results.append(r)

        if r["status"] != "ok":
            print(f"  {r['name']:<33s}  {r['status']}")
            errors.append(r)
            continue

        gt_found = f"{r['n_tables_found']}/{r['n_gt_tables']}"
        print(f"  {r['name']:<33s} {r['n_gt_cells']:4d} {r['n_our_cells']:5d} {gt_found:>4s} "
              f"{r['text_recall']:5.1%} {r['header_precision']:5.1%} {r['header_recall']:5.1%} "
              f"{r['numeric_recall']:5.1%} {r['rl_recall']:5.1%} {r['winning_tag']:<20s}")

    # Summary
    ok_results = [r for r in results if r["status"] == "ok"]
    if ok_results:
        def avg(key):
            return sum(r[key] for r in ok_results) / len(ok_results)

        print(f"\n{'─' * 120}")
        print(f"  {'AVERAGE':<33s} {'':4s} {'':5s} {'':4s} "
              f"{avg('text_recall'):5.1%} {avg('header_precision'):5.1%} {avg('header_recall'):5.1%} "
              f"{avg('numeric_recall'):5.1%} {avg('rl_recall'):5.1%}")
        print(f"\n  {len(ok_results)} evaluated, {len(errors)} errors")

        from collections import Counter
        tags = Counter(r["winning_tag"] for r in ok_results)
        print(f"  Tags: {dict(tags)}")

        # Show most common numeric misses
        all_missed = []
        for r in ok_results:
            all_missed.extend(r.get("numeric_missed", []))
        if all_missed:
            role_dist = Counter(m[1] for m in all_missed)
            print(f"\n  Numeric cells misclassified as: {dict(role_dist)}")
            print(f"  Sample misses: {all_missed[:10]}")

    con.close()


if __name__ == "__main__":
    main()
