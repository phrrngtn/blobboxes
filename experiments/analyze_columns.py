"""Geometry-based column detection from bboxes.

Reads documents through the DuckDB bboxes extension, then uses pure
spatial analysis to detect columns and headers — no domain knowledge.

Strategy:
1. Load bboxes from DuckDB into a DataFrame
2. Cluster bboxes into rows by y-coordinate proximity
3. Cluster bboxes into columns by x-coordinate alignment
4. Detect header row by style differentiation
5. Compare detected structure against ground truth
"""

import json
from pathlib import Path

import duckdb
import pandas as pd

EXT_PATH = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
SYNTH_DIR = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")
GT_PATH = SYNTH_DIR / "ground_truth.json"


def load_bboxes(filepath: str) -> pd.DataFrame:
    """Load bboxes from a file through the DuckDB extension."""
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")

    suffix = Path(filepath).suffix.lower()
    if suffix == ".xlsx":
        func = "bb_xlsx"
    else:
        func = "bb"

    df = con.execute(f"""
        SELECT page_id, style_id, x, y, w, h, text
        FROM {func}('{filepath}')
    """).df()
    con.close()
    return df


def load_styles(filepath: str) -> pd.DataFrame:
    """Load styles from a file through the DuckDB extension."""
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")

    suffix = Path(filepath).suffix.lower()
    if suffix == ".xlsx":
        func = "bb_xlsx_styles"
    else:
        func = "bb_styles"

    df = con.execute(f"""
        SELECT style_id, font_id, font_size, color, weight, italic, underline
        FROM {func}('{filepath}')
    """).df()
    con.close()
    return df


# ── Row clustering ───────────────────────────────────────────────────

def cluster_rows(df: pd.DataFrame, y_tolerance: float = None) -> pd.DataFrame:
    """Assign bboxes to row clusters based on y-coordinate proximity.

    For XLSX, y values are exact integers (row numbers), so tolerance=0.
    For PDF, we need to cluster y values that are close together.

    Uses the bbox vertical midpoint (y + h/2) for clustering, which is
    more robust than top-edge when glyphs have varied baselines (e.g.
    decimal points, commas positioned lower than digits).
    """
    df = df.copy()
    df["y_mid"] = df["y"] + df["h"] / 2.0

    if y_tolerance is None:
        median_h = df["h"].median()
        y_tolerance = max(median_h * 0.6, 3.0)  # at least 3 points

    sorted_df = df.sort_values("y_mid").copy()
    row_id = 0
    row_ids = []
    cluster_y_sum = 0.0
    cluster_count = 0
    last_cluster_y = None

    for _, bbox in sorted_df.iterrows():
        if last_cluster_y is None or abs(bbox["y_mid"] - last_cluster_y) > y_tolerance:
            row_id += 1
            cluster_y_sum = bbox["y_mid"]
            cluster_count = 1
        else:
            cluster_y_sum += bbox["y_mid"]
            cluster_count += 1
        last_cluster_y = cluster_y_sum / cluster_count  # running mean
        row_ids.append(row_id)

    sorted_df["row_cluster"] = row_ids
    return sorted_df


# ── Column clustering ────────────────────────────────────────────────

def cluster_columns(df: pd.DataFrame, x_tolerance: float = None) -> pd.DataFrame:
    """Assign bboxes to column clusters based on x-coordinate alignment.

    We look at the x-coordinate of bbox left edges. Bboxes whose left
    edges are within x_tolerance of each other belong to the same column.

    For XLSX this is trivial (x = column number).
    For PDF we need to cluster nearby x values.
    """
    if x_tolerance is None:
        x_vals = sorted(df["x"].unique())
        if len(x_vals) > 1:
            gaps = [x_vals[i+1] - x_vals[i] for i in range(len(x_vals)-1)]
            # Use a fraction of the median gap as tolerance
            median_gap = sorted(gaps)[len(gaps)//2]
            x_tolerance = median_gap * 0.3
        else:
            x_tolerance = 1.0

    # Cluster x values
    x_vals = sorted(df["x"].unique())
    x_clusters = {}  # x_value -> cluster_id
    cluster_id = 0
    cluster_center = None

    for x in x_vals:
        if cluster_center is None or abs(x - cluster_center) > x_tolerance:
            cluster_id += 1
            cluster_center = x
        x_clusters[x] = cluster_id

    df = df.copy()
    df["col_cluster"] = df["x"].map(x_clusters)
    return df


# ── Merge fragmented bboxes within same row ──────────────────────────

def merge_row_fragments(df: pd.DataFrame) -> pd.DataFrame:
    """Within each row cluster, merge adjacent bboxes into logical cells.

    PDF extraction often fragments text: "$12" and ",400" are separate
    bboxes. We merge bboxes that are horizontally adjacent (gap < threshold).
    """
    rows = []
    for row_id in df["row_cluster"].unique():
        row_bboxes = df[df["row_cluster"] == row_id].sort_values("x")

        if len(row_bboxes) == 0:
            continue

        # Merge adjacent bboxes with small horizontal gaps
        merged = []
        current = row_bboxes.iloc[0].to_dict()

        for i in range(1, len(row_bboxes)):
            bbox = row_bboxes.iloc[i]
            gap = bbox["x"] - (current["x"] + current["w"])

            # If gap is small relative to font size, merge
            # Use a generous threshold: up to 1 character width
            merge_threshold = max(current["h"] * 0.8, 3.0)
            word_gap_threshold = current["h"] * 0.3
            if gap < merge_threshold:
                # Extend current bbox
                current["w"] = (bbox["x"] + bbox["w"]) - current["x"]
                # Insert space for word-boundary gaps
                if gap > word_gap_threshold:
                    current["text"] = current["text"] + " " + bbox["text"]
                else:
                    current["text"] = current["text"] + bbox["text"]
            else:
                merged.append(current)
                current = bbox.to_dict()

        merged.append(current)
        rows.extend(merged)

    return pd.DataFrame(rows)


# ── Detect header row ────────────────────────────────────────────────

def detect_header_candidates(df: pd.DataFrame, styles_df: pd.DataFrame) -> list:
    """Find row clusters that look like headers based on style differences.

    Headers typically have:
    - Different font weight (bold)
    - Different font size
    - Different color
    - All text (no pure numeric values)
    """
    # Merge style info
    if len(styles_df) > 0:
        df = df.merge(styles_df, on="style_id", how="left")

    row_styles = []
    for row_id in sorted(df["row_cluster"].unique()):
        row = df[df["row_cluster"] == row_id]
        info = {
            "row_cluster": row_id,
            "n_cells": len(row),
            "y_mean": row["y"].mean(),
        }
        if "weight" in row.columns:
            info["has_bold"] = (row["weight"].str.lower().str.contains("bold", na=False)).any()
        if "font_size" in row.columns:
            info["mean_font_size"] = row["font_size"].mean()

        # Check if all values are non-numeric (header-like)
        texts = row["text"].tolist()
        numeric_count = sum(1 for t in texts
                           if t.replace(",", "").replace(".", "").replace("$", "")
                           .replace("%", "").replace("-", "").strip().isdigit())
        info["frac_numeric"] = numeric_count / len(texts) if texts else 0
        info["texts"] = texts

        row_styles.append(info)

    return pd.DataFrame(row_styles)


# ── Table segmentation ───────────────────────────────────────────────

def segment_tables(df: pd.DataFrame, min_rows: int = 2) -> list:
    """Segment rows into table regions based on cell-count consistency.

    A table region is a contiguous run of rows where the cell count is
    stable (within ±1 of the mode for that run). Rows with very different
    cell counts, or isolated single-cell rows, mark table boundaries.

    Returns a list of (start_row_cluster, end_row_cluster) tuples.
    """
    row_ids = sorted(df["row_cluster"].unique())
    cell_counts = df.groupby("row_cluster").size()

    segments = []
    current_start = None
    current_mode = None
    current_rows = []

    for rid in row_ids:
        n_cells = cell_counts[rid]

        if current_start is None:
            # Start a new segment
            if n_cells >= 2:
                current_start = rid
                current_rows = [rid]
                current_mode = n_cells
        else:
            # Check if this row fits the current segment
            if n_cells >= 2 and abs(n_cells - current_mode) <= 1:
                current_rows.append(rid)
            elif n_cells >= 2 and abs(n_cells - current_mode) <= 2 and len(current_rows) < 3:
                # Early in a segment, allow the mode to shift
                current_rows.append(rid)
                # Recompute mode
                counts = [cell_counts[r] for r in current_rows]
                current_mode = max(set(counts), key=counts.count)
            else:
                # End current segment if it's long enough
                if len(current_rows) >= min_rows:
                    segments.append((current_rows[0], current_rows[-1]))
                # Start a new segment if this row qualifies
                if n_cells >= 2:
                    current_start = rid
                    current_rows = [rid]
                    current_mode = n_cells
                else:
                    current_start = None
                    current_rows = []
                    current_mode = None

    # Don't forget the last segment
    if current_rows and len(current_rows) >= min_rows:
        segments.append((current_rows[0], current_rows[-1]))

    return segments


# ── Detect one table from a segment ──────────────────────────────────

def detect_single_table(df: pd.DataFrame, styles_df: pd.DataFrame,
                         start_row: int, end_row: int, table_idx: int) -> dict:
    """Detect header and columns for a single table segment."""
    seg = df[(df["row_cluster"] >= start_row) & (df["row_cluster"] <= end_row)]

    header_info = detect_header_candidates(seg, styles_df)

    # Find header: bold + low numeric, or first all-text row
    header_candidates = header_info[
        (header_info.get("has_bold", pd.Series(dtype=bool)) == True) &
        (header_info["frac_numeric"] < 0.3)
    ] if "has_bold" in header_info.columns else pd.DataFrame()

    if len(header_candidates) == 0:
        header_candidates = header_info[
            (header_info["frac_numeric"] == 0) &
            (header_info["n_cells"] >= 2)
        ]

    detected_header_row = None
    detected_columns = []

    if len(header_candidates) > 0:
        best = header_candidates.loc[header_candidates["n_cells"].idxmax()]
        detected_header_row = int(best["row_cluster"])
    else:
        # Fallback: first row in segment
        detected_header_row = start_row

    header_bboxes = seg[seg["row_cluster"] == detected_header_row].sort_values("x")
    detected_columns = header_bboxes["text"].tolist()

    # Data rows: after header, within segment, similar cell count
    header_col_count = len(detected_columns)
    data = seg[seg["row_cluster"] > detected_header_row]
    data_counts = data.groupby("row_cluster").size()
    matching = data_counts[abs(data_counts - header_col_count) <= 1]
    n_data_rows = len(matching)

    # Print
    print(f"\n  Table {table_idx}: rows {start_row}-{end_row}")
    print(f"    Header (row {detected_header_row}): {detected_columns}")
    print(f"    Data rows: {n_data_rows}")

    # Grid preview
    if header_col_count > 0:
        col_clusters = sorted(
            seg[seg["row_cluster"] == detected_header_row]["col_cluster"].unique()
        )
        header_texts = []
        for cc in col_clusters:
            cell = seg[(seg["row_cluster"] == detected_header_row) & (seg["col_cluster"] == cc)]
            header_texts.append(cell["text"].iloc[0] if len(cell) > 0 else "?")
        col_widths = [max(12, len(t) + 2) for t in header_texts]
        print(f"    {' | '.join(t.ljust(w) for t, w in zip(header_texts, col_widths))}")
        print(f"    {'-+-'.join('-' * w for w in col_widths)}")

        data_row_ids = sorted(data["row_cluster"].unique())[:3]
        for rid in data_row_ids:
            row_data = seg[seg["row_cluster"] == rid].sort_values("x")
            cells = []
            for cc in col_clusters:
                cell = row_data[row_data["col_cluster"] == cc]
                cells.append(cell["text"].iloc[0] if len(cell) > 0 else "")
            print(f"    {' | '.join(t.ljust(w) for t, w in zip(cells, col_widths))}")
        if len(data_row_ids) < n_data_rows:
            print(f"    ... ({n_data_rows - len(data_row_ids)} more rows)")

    return {
        "header_row": detected_header_row,
        "columns": detected_columns,
        "n_data_rows": n_data_rows,
    }


# ── Main column detection pipeline ───────────────────────────────────

def detect_table_structure(filepath: str) -> dict:
    """Full pipeline: load bboxes, segment into tables, detect structure."""
    print(f"\n{'='*60}")
    print(f"Analyzing: {Path(filepath).name}")
    print(f"{'='*60}")

    df = load_bboxes(filepath)
    styles_df = load_styles(filepath)

    print(f"\nRaw bboxes: {len(df)}")

    is_xlsx = filepath.endswith(".xlsx")

    # Step 1: Cluster into rows
    if is_xlsx:
        df["row_cluster"] = df["y"].astype(int)
    else:
        df = cluster_rows(df)

    n_rows = df["row_cluster"].nunique()
    print(f"Row clusters: {n_rows}")

    # Step 2: Merge fragmented bboxes (PDF only)
    if not is_xlsx:
        df = merge_row_fragments(df)
        print(f"After merging fragments: {len(df)} cells")

    # Step 3: Cluster into columns
    if is_xlsx:
        df["col_cluster"] = df["x"].astype(int)
    else:
        df = cluster_columns(df)

    n_cols = df["col_cluster"].nunique()
    print(f"Column clusters: {n_cols}")

    # Step 4: Row analysis overview
    header_info = detect_header_candidates(df, styles_df)
    print(f"\nRow analysis:")
    for _, row in header_info.iterrows():
        marker = ""
        if row.get("has_bold", False) and row["frac_numeric"] < 0.3:
            marker = " <-- HEADER CANDIDATE"
        elif row["frac_numeric"] == 0 and row["n_cells"] >= 2:
            marker = " <-- possible header (all text)"
        print(f"  Row {int(row['row_cluster']):2d}: "
              f"{int(row['n_cells'])} cells, "
              f"{row['frac_numeric']:.0%} numeric"
              f"{', BOLD' if row.get('has_bold', False) else ''}"
              f"{marker}")

    # Step 5: Segment into table regions
    segments = segment_tables(df)
    print(f"\nTable segments found: {len(segments)}")

    tables_detected = []
    for idx, (start, end) in enumerate(segments):
        tbl = detect_single_table(df, styles_df, start, end, idx + 1)
        tables_detected.append(tbl)

    # Return info about all detected tables (use first for backward compat)
    first = tables_detected[0] if tables_detected else {
        "header_row": None, "columns": [], "n_data_rows": 0
    }

    return {
        "file": Path(filepath).name,
        "n_raw_bboxes": len(load_bboxes(filepath)),
        "n_row_clusters": n_rows,
        "n_col_clusters": n_cols,
        "detected_header_row": first["header_row"],
        "detected_columns": first["columns"],
        "n_data_rows": first["n_data_rows"],
        "all_tables": tables_detected,
    }


# ── Evaluate against ground truth ────────────────────────────────────

def _match_columns(gt_cols, det_cols):
    """Match detected columns against ground truth (case-insensitive, partial)."""
    matched = 0
    for gc in gt_cols:
        for dc in det_cols:
            if gc.lower() in dc.lower() or dc.lower() in gc.lower():
                matched += 1
                break
    recall = matched / len(gt_cols) if gt_cols else 0
    precision = matched / len(det_cols) if det_cols else 0
    return matched, recall, precision


def evaluate(detected: dict, ground_truth: list) -> dict:
    """Compare detected structure against ground truth.

    For multi-table documents, match each GT table to its best-matching
    detected table by column overlap.
    """
    results = {"file": detected["file"], "tables": []}
    all_tables = detected.get("all_tables", [])

    for gt in ground_truth:
        gt_cols = [c["name"] for c in gt["columns"]]

        # Find the detected table that best matches this GT table
        best_match = None
        best_recall = -1

        for tbl in all_tables:
            det_cols = tbl["columns"]
            _, recall, _ = _match_columns(gt_cols, det_cols)
            if recall > best_recall:
                best_recall = recall
                best_match = tbl

        if best_match is None:
            # No tables detected at all
            best_match = {"columns": [], "n_data_rows": 0}

        det_cols = best_match["columns"]
        matched, col_recall, col_precision = _match_columns(gt_cols, det_cols)
        row_match = best_match["n_data_rows"] == gt["num_data_rows"]

        result = {
            "table_id": gt["table_id"],
            "difficulty": gt["difficulty"],
            "gt_cols": gt_cols,
            "det_cols": det_cols,
            "col_recall": col_recall,
            "col_precision": col_precision,
            "gt_data_rows": gt["num_data_rows"],
            "det_data_rows": best_match["n_data_rows"],
            "row_count_match": row_match,
        }
        results["tables"].append(result)

    return results


def main():
    with open(GT_PATH) as f:
        ground_truth = json.load(f)

    # Also test files without ground truth (real-world, exploratory)
    extra_files = [
        str(SYNTH_DIR / "census_pop.xlsx"),
        str(SYNTH_DIR.parent / "sample.pdf"),
        str(SYNTH_DIR.parent / "sample.xlsx"),
    ]
    for ef in extra_files:
        if Path(ef).exists():
            try:
                detect_table_structure(ef)
            except Exception as e:
                print(f"\nERROR processing {ef}: {e}")

    all_results = []

    for filename, gt_tables in ground_truth.items():
        filepath = str(SYNTH_DIR / filename)
        try:
            detected = detect_table_structure(filepath)
            result = evaluate(detected, gt_tables)
            all_results.append(result)
        except Exception as e:
            print(f"\nERROR processing {filename}: {e}")
            import traceback
            traceback.print_exc()

    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")

    for result in all_results:
        for t in result["tables"]:
            status = ""
            if t["col_recall"] == 1.0 and t["row_count_match"]:
                status = "PASS"
            elif t["col_recall"] >= 0.8:
                status = "PARTIAL"
            else:
                status = "FAIL"

            print(f"  [{status:7s}] {result['file']:30s} "
                  f"{t['table_id']:25s} ({t['difficulty']:6s}) "
                  f"cols={t['col_recall']:.0%} "
                  f"rows={t['det_data_rows']}/{t['gt_data_rows']}")

            if status != "PASS":
                print(f"           GT:  {t['gt_cols']}")
                print(f"           Det: {t['det_cols']}")


if __name__ == "__main__":
    main()
