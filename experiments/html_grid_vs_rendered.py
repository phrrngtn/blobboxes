"""How good a proxy are bb_html's grid ordinates for real rendered geometry?

`bb_html` walks the DOM and assigns each text run a *logical* (x, y) — column
and row indices, not pixels. A browser assigns real coordinates by running
layout. Comparing the two absolutely is meaningless; the useful question is
whether the grid is a faithful **order- and structure-preserving** proxy:

1. **Coverage** — does it find the same text runs at all?
2. **Reading order** — does sorting by (y, x) reproduce sorting by (top, left)?
3. **Row grouping** — do runs sharing a grid row actually share a rendered row?
4. **Column alignment** — do runs sharing a grid column actually line up?

(3) and (4) are the ones that matter for downstream table reasoning, which is
what the grid exists to support.

Run:  uv run python experiments/html_grid_vs_rendered.py
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
from itertools import combinations

import duckdb
from playwright.sync_api import sync_playwright

HERE = pathlib.Path(__file__).resolve().parent
PAGE = HERE / "html_grid_vs_rendered.html"
EXT = HERE.parent / "zig-out" / "lib" / "bboxes.duckdb_extension"

# Mirrors browser/src/bbox.js: TreeWalker over text nodes, Range.getClientRects()
# per node. Rects rather than the element box, so a wrapped paragraph yields one
# rect per visual line — the same granularity bb_html aims at.
JS = """
() => {
  const out = [];
  const walk = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  let n;
  while ((n = walk.nextNode())) {
    const text = n.nodeValue.replace(/\\s+/g, ' ').trim();
    if (!text) continue;
    // Which structural region the run belongs to. A grid model is *supposed*
    // to be exact inside a table and is a poor fit for flow content, so the
    // aggregate is misleading unless these are reported separately.
    const cell = n.parentElement.closest('td, th');
    const region = cell ? 'table' : 'flow';
    const range = document.createRange();
    range.selectNodeContents(n);
    const rects = [...range.getClientRects()].filter(r => r.width && r.height);
    rects.forEach((r, i) => out.push({
      text, x: r.x, y: r.y, w: r.width, h: r.height,
      region, wrapped: rects.length > 1, line: i,
    }));
  }
  return out;
}
"""


def norm(s: str) -> str:
    return " ".join(s.split()).strip()


def grid_rows() -> list[dict]:
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    rows = con.execute(
        "SELECT x, y, w, h, text FROM bb_html(?)", [str(PAGE)]
    ).fetchall()
    return [
        {"x": r[0], "y": r[1], "w": r[2], "h": r[3], "text": norm(r[4])}
        for r in rows
        if norm(r[4])
    ]


def rendered_rows() -> list[dict]:
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1200, "height": 900})
        page.goto(PAGE.as_uri())
        page.wait_for_load_state("networkidle")
        rows = page.evaluate(JS)
        browser.close()
    return [{**r, "text": norm(r["text"])} for r in rows if norm(r["text"])]


def spearman(a: list[float], b: list[float]) -> float:
    """Rank correlation, without pulling in scipy for one number."""
    n = len(a)
    if n < 2:
        return float("nan")

    def ranks(v):
        order = sorted(range(n), key=lambda i: v[i])
        r = [0.0] * n
        i = 0
        while i < n:                       # average ties, or ties skew rho
            j = i
            while j + 1 < n and v[order[j + 1]] == v[order[i]]:
                j += 1
            avg = (i + j) / 2 + 1
            for k in range(i, j + 1):
                r[order[k]] = avg
            i = j + 1
        return r

    ra, rb = ranks(a), ranks(b)
    ma, mb = sum(ra) / n, sum(rb) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    da = sum((x - ma) ** 2 for x in ra) ** 0.5
    db = sum((y - mb) ** 2 for y in rb) ** 0.5
    return num / (da * db) if da and db else float("nan")


def main() -> int:
    if not EXT.exists():
        sys.exit(f"extension not built: {EXT}\n  zig build")

    grid = grid_rows()
    rend = rendered_rows()

    print(f"bb_html runs : {len(grid)}")
    print(f"rendered runs: {len(rend)}")

    # ── 1. coverage ──────────────────────────────────────────────────
    # Matched on text. Duplicated strings are matched positionally in
    # reading order, which is the best available without ids.
    from collections import defaultdict

    by_text_r = defaultdict(list)
    for r in sorted(rend, key=lambda r: (round(r["y"], 1), r["x"])):
        by_text_r[r["text"]].append(r)
    by_text_g = defaultdict(list)
    for g in sorted(grid, key=lambda g: (g["y"], g["x"])):
        by_text_g[g["text"]].append(g)

    pairs = []
    for text, gs in by_text_g.items():
        rs = by_text_r.get(text, [])
        for g, r in zip(gs, rs):
            pairs.append((g, r))

    only_grid = sum(len(v) for k, v in by_text_g.items()) - len(pairs)
    only_rend = sum(len(v) for k, v in by_text_r.items()) - len(pairs)
    print(f"\n1. COVERAGE")
    print(f"   matched            : {len(pairs)}")
    print(f"   only in bb_html    : {only_grid}")
    print(f"   only in rendered   : {only_rend}")
    wrapped = sum(1 for r in rend if r.get("wrapped"))
    print(f"   (rendered runs that are wrap fragments: {wrapped} — a browser "
          f"emits one rect per visual line,\n    bb_html one run per text node, "
          f"so a wrapped paragraph is 1 grid run vs N rendered)")
    if len(pairs) == 0:
        return 1

    # ── 2. reading order ─────────────────────────────────────────────
    g_order = sorted(range(len(pairs)), key=lambda i: (pairs[i][0]["y"], pairs[i][0]["x"]))
    r_order = sorted(range(len(pairs)), key=lambda i: (round(pairs[i][1]["y"], 1), pairs[i][1]["x"]))
    g_rank = {v: i for i, v in enumerate(g_order)}
    r_rank = {v: i for i, v in enumerate(r_order)}
    rho = spearman([g_rank[i] for i in range(len(pairs))],
                   [r_rank[i] for i in range(len(pairs))])
    inversions = sum(
        1 for i, j in combinations(range(len(pairs)), 2)
        if (g_rank[i] < g_rank[j]) != (r_rank[i] < r_rank[j])
    )
    total_pairs = len(pairs) * (len(pairs) - 1) // 2
    print(f"\n2. READING ORDER")
    print(f"   Spearman rho       : {rho:.4f}")
    print(f"   pair inversions    : {inversions}/{total_pairs} "
          f"({100*inversions/total_pairs:.1f}% out of order)")

    # ── 3. row grouping ──────────────────────────────────────────────
    # Same grid y => should overlap vertically on screen.
    def row_score(sel):
        ok = bad = 0
        for (g1, r1), (g2, r2) in combinations([p for p in pairs if sel(p)], 2):
            if g1["y"] != g2["y"]:
                continue
            ov = min(r1["y"] + r1["h"], r2["y"] + r2["h"]) - max(r1["y"], r2["y"])
            ok, bad = (ok + 1, bad) if ov > 0 else (ok, bad + 1)
        return ok, ok + bad

    print(f"\n3. ROW GROUPING  (same grid y -> vertically overlapping?)")
    for label, sel in (("table cells", lambda p: p[1]["region"] == "table"),
                       ("flow text  ", lambda p: p[1]["region"] == "flow"),
                       ("all        ", lambda p: True)):
        ok, tot = row_score(sel)
        pct = f"  ({100*ok/tot:.1f}%)" if tot else ""
        print(f"   {label}        : {ok}/{tot}{pct}")

    # ── 4. column alignment ──────────────────────────────────────────
    def col_score(sel):
        ok = bad = 0
        for (g1, r1), (g2, r2) in combinations([p for p in pairs if sel(p)], 2):
            if g1["x"] != g2["x"] or g1["y"] == g2["y"]:
                continue
            # Same column => left edges close. 12px covers right-aligned
            # numerals of differing width inside one table column.
            ok, bad = (ok + 1, bad) if abs(r1["x"] - r2["x"]) <= 12 else (ok, bad + 1)
        return ok, ok + bad

    print(f"\n4. COLUMN ALIGNMENT  (same grid x -> same rendered left, +/-12px?)")
    for label, sel in (("table cells", lambda p: p[1]["region"] == "table"),
                       ("flow text  ", lambda p: p[1]["region"] == "flow"),
                       ("all        ", lambda p: True)):
        ok, tot = col_score(sel)
        pct = f"  ({100*ok/tot:.1f}%)" if tot else ""
        print(f"   {label}        : {ok}/{tot}{pct}")

    # ── where they disagree ──────────────────────────────────────────
    print(f"\n5. WORST DISAGREEMENTS (by reading-order rank delta)")
    deltas = sorted(range(len(pairs)), key=lambda i: -abs(g_rank[i] - r_rank[i]))
    for i in deltas[:6]:
        g, r = pairs[i]
        print(f"   d={abs(g_rank[i]-r_rank[i]):3d}  grid=({g['x']},{g['y']})"
              f"  rendered=({r['x']:.0f},{r['y']:.0f})  {g['text'][:44]!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
