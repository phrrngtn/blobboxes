"""Isolate *why* bb_html's grid diverges from rendered layout.

The companion script measures agreement. This one attributes the disagreement,
because the headline numbers turn out to be dominated by one fixable defect
rather than by many small ones.

Run: uv run python experiments/html_grid_diagnose.py
"""

from __future__ import annotations

import pathlib
import sys
from collections import defaultdict
from itertools import combinations

import duckdb
from playwright.sync_api import sync_playwright

from html_grid_vs_rendered import JS, PAGE, EXT, norm, spearman


def rendered():
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page(viewport={"width": 1200, "height": 900})
        pg.goto(PAGE.as_uri())
        pg.wait_for_load_state("networkidle")
        rows = pg.evaluate(JS)
        b.close()
    return [{**r, "text": norm(r["text"])} for r in rows if norm(r["text"])]


def grid():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    return [
        {"x": x, "y": y, "text": norm(t)}
        for x, y, t in con.execute(
            "SELECT x, y, text FROM bb_html(?)", [str(PAGE)]
        ).fetchall()
        if norm(t)
    ]


def inversions(pairs, key_g, key_r):
    g = sorted(range(len(pairs)), key=lambda i: key_g(pairs[i][0], i))
    r = sorted(range(len(pairs)), key=lambda i: key_r(pairs[i][1]))
    gr = {v: i for i, v in enumerate(g)}
    rr = {v: i for i, v in enumerate(r)}
    inv = sum(1 for i, j in combinations(range(len(pairs)), 2)
              if (gr[i] < gr[j]) != (rr[i] < rr[j]))
    tot = len(pairs) * (len(pairs) - 1) // 2
    rho = spearman([gr[i] for i in range(len(pairs))],
                   [rr[i] for i in range(len(pairs))])
    return inv, tot, rho


def main() -> int:
    g_rows, r_rows = grid(), rendered()

    by_r = defaultdict(list)
    for r in sorted(r_rows, key=lambda r: (round(r["y"], 1), r["x"])):
        by_r[r["text"]].append(r)
    by_g = defaultdict(list)
    for g in g_rows:                       # keep emission order — that is the signal
        by_g[g["text"]].append(g)

    pairs = []
    for text, gs in by_g.items():
        for gg, rr in zip(gs, by_r.get(text, [])):
            pairs.append((gg, rr))

    # ── the defect: y restarts per structural block ──────────────────
    print("1. IS THE ROW COUNTER DOCUMENT-GLOBAL?")
    ys = [g["y"] for g in g_rows]
    resets = [i for i in range(1, len(ys)) if ys[i] < ys[i - 1]]
    print(f"   grid y sequence  : {ys}")
    print(f"   backward jumps   : {len(resets)} at emission index {resets}")
    if resets:
        print("   FAIL -> a block restarts the row counter, so a heading after a")
        print("           table can share a y with a row inside it, and ordering")
        print("           the document by (y, x) interleaves the two.")
    else:
        print("   PASS -> monotonic; tables continue the document numbering.")

    # ── what does that cost, and what would fixing it buy? ───────────
    # "Fixed" grid = emission order made monotonic by numbering rows globally,
    # which is what a document-global counter would have produced.
    seq, last, off = [], None, 0
    for g in g_rows:
        if last is not None and g["y"] < last:
            off = max(seq) + 1 if seq else 0
        seq.append(g["y"] + off)
        last = g["y"]
    fixed = {id(g): s for g, s in zip(g_rows, seq)}

    inv0, tot, rho0 = inversions(pairs, lambda g, i: (g["y"], g["x"]),
                                 lambda r: (round(r["y"], 1), r["x"]))
    inv1, _, rho1 = inversions(pairs, lambda g, i: (fixed[id(g)], g["x"]),
                               lambda r: (round(r["y"], 1), r["x"]))
    print("\n2. COST OF ANY COLLIDING COUNTERS")
    print(f"   as emitted       : rho={rho0:.4f}  inversions={inv0}/{tot} "
          f"({100*inv0/tot:.1f}%)")
    print(f"   with a global y  : rho={rho1:.4f}  inversions={inv1}/{tot} "
          f"({100*inv1/tot:.1f}%)")
    if inv0 == inv1:
        print(f"   -> no gain available: numbering is already global")
    else:
        print(f"   -> restarting counters accounts for {inv0-inv1} of {inv0} inversions")
    print(f"   (the residual {inv1} is the flex side-by-side case in section 3,")
    print(f"    which no DOM-order model can capture)")

    # ── the inherent limit: side-by-side layout ──────────────────────
    print("\n3. WHAT A GRID CANNOT CAPTURE (inherent, not a bug)")
    left = next((r for r in r_rows if r["text"].startswith("Left column")), None)
    right = next((r for r in r_rows if r["text"].startswith("Right column")), None)
    gl = next((g for g in g_rows if g["text"].startswith("Left column")), None)
    gr_ = next((g for g in g_rows if g["text"].startswith("Right column")), None)
    if left and right and gl and gr_:
        print(f"   'Left column'    grid y={gl['y']}  rendered y={left['y']:.0f} x={left['x']:.0f}")
        print(f"   'Right column'   grid y={gr_['y']}  rendered y={right['y']:.0f} x={right['x']:.0f}")
        same_visual_row = abs(left["y"] - right["y"]) < 5
        print(f"   rendered side by side on one row : {same_visual_row}")
        print(f"   grid puts them on separate rows  : {gl['y'] != gr_['y']}")
        print("   -> flex/float layout is resolved by the layout engine, not by")
        print("      the DOM. No static walk can know it without doing layout.")
    # Regression gate: the row counter must stay document-global.
    return 1 if resets else 0


if __name__ == "__main__":
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    raise SystemExit(main())
