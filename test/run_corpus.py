#!/usr/bin/env python3
"""Corpus runner for the legacy-.xls extraction lane.

Two jobs in one pass, per file, each file in its OWN duckdb subprocess so a
segfault/hang on a nasty workbook is reported (CRASH/HANG) instead of killing
the run:

  1. ROBUSTNESS / smoke  — does our tool parse every corpus file without
     crashing? Classify: OK | BIFF5/7-warn | FILEPASS-skip | FAIL | CRASH | HANG.
     Report per-file formula/name/sheet counts.
  2. GOLDEN diff         — for files that have a golden under test/golden/
     (mirrored tree, "<sheet>!<A1>" -> A1 formula), diff our A1 formula output
     against the LibreOffice oracle. R1C1 is trusted once A1 matches.

Usage:
  uv run python test/run_corpus.py                 # all corpus files
  uv run python test/run_corpus.py --golden-only    # only files with a golden
  uv run python test/run_corpus.py --filter shared  # substring filter on path
  uv run python test/run_corpus.py --timeout 30
No third-party deps (stdlib only); shells out to the `duckdb` CLI.
"""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXT = REPO / "build" / "duckdb" / "bboxes.duckdb_extension"
CORPUS = REPO / "test" / "corpus"
GOLDEN = REPO / "test" / "golden"
FIXTURES = REPO.parent / "xls_biff" / "test"   # our authored xlwt fixture lives here

DUCKDB = shutil.which("duckdb")

SQL = """LOAD '{ext}';
SELECT json_object(
  'formulas', xls_formulas('{p}')::JSON,
  'names',    xls_names('{p}')::JSON,
  'meta',     xls_metadata('{p}')::JSON
) AS r;"""


def probe(path: Path, timeout: int):
    """Run one file in its own duckdb process. Returns (status, data|err)."""
    sql = SQL.format(ext=str(EXT), p=str(path).replace("'", "''"))
    try:
        cp = subprocess.run([DUCKDB, "-unsigned", "-json", "-c", sql],
                            capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "HANG", None
    if cp.returncode < 0:
        return "CRASH", f"signal {-cp.returncode}"
    if cp.returncode != 0:
        return "FAIL", (cp.stderr or cp.stdout).strip()[:300]
    try:
        rows = json.loads(cp.stdout)
        return "OK", json.loads(rows[0]["r"]) if isinstance(rows[0]["r"], str) else rows[0]["r"]
    except Exception as e:
        return "FAIL", f"parse: {e}: {cp.stdout[:200]}"


def classify(data):
    f, n, m = data.get("formulas", {}), data.get("names", {}), data.get("meta", {})
    ferr = (f or {}).get("error", "")
    if str(ferr).startswith("FILEPASS"):
        return "FILEPASS", 0, 0, (m.get("sheet_count") or 0), None
    warn = (f or {}).get("warning")
    fc = len((f or {}).get("formulas") or [])
    nc = len((n or {}).get("names") or [])
    sc = m.get("sheet_count") or 0
    return ("BIFF5/7" if warn else "OK"), fc, nc, sc, warn


def our_a1_map(data):
    """{ 'SheetName!A1': formula_a1 } from our output, mapping sheet index -> name."""
    f, m = data.get("formulas", {}), data.get("meta", {})
    idx2name = {}
    for s in (m.get("sheets") or []):
        idx2name[s.get("index")] = s.get("name")
    out = {}
    for row in (f.get("formulas") or []):
        name = idx2name.get(row.get("sheet"), str(row.get("sheet")))
        out[f"{name}!{row.get('address_a1')}"] = row.get("formula_a1")
    return out


def golden_key(path: Path):
    """Corpus-relative path (or fixtures/<name>) flattened to a single filename component.
    This same key names the flattened .xls handed to LibreOffice and the golden JSON it yields."""
    try:
        rel = str(path.relative_to(CORPUS))
    except ValueError:
        rel = "fixtures/" + path.name
    if rel.endswith(".xls"):
        rel = rel[:-4]
    return rel.replace("/", "__")   # only slashes; existing underscores untouched


def golden_path_for(path: Path):
    return GOLDEN / (golden_key(path) + ".formulas.json")


def norm(s):
    if s is None:
        return None
    s = s.strip()
    return s[1:] if s.startswith("=") else s   # LibreOffice oracle keeps the leading '='


def diff_golden(data, gpath: Path):
    golden = json.loads(gpath.read_text())
    ours = our_a1_map(data)
    gk, ok = set(golden), set(ours)
    matched = mism = 0
    samples = []
    for k in gk & ok:
        if norm(golden[k]) == norm(ours[k]):
            matched += 1
        else:
            mism += 1
            if len(samples) < 4:
                samples.append(f"{k}: oracle={norm(golden[k])!r} ours={norm(ours[k])!r}")
    missing = gk - ok          # oracle has a formula we didn't emit
    extra = ok - gk            # we emitted a formula the oracle doesn't call a formula
    return matched, mism, sorted(missing)[:6], sorted(extra)[:6], samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--golden-only", action="store_true")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--baseline", default=str(GOLDEN / "known_gaps.txt"),
                    help="golden keys expected to FAIL (documented parser gaps); such failures don't fail the run")
    args = ap.parse_args()

    baseline = set()
    bp = Path(args.baseline)
    if bp.exists():
        for ln in bp.read_text().splitlines():
            ln = ln.split("#", 1)[0].strip()
            if ln:
                baseline.add(ln)

    if not DUCKDB:
        sys.exit("duckdb CLI not on PATH")
    if not EXT.exists():
        sys.exit(f"extension not built: {EXT}\n  cmake --build build --target bboxes_duckdb")

    files = sorted(p for p in CORPUS.rglob("*.xls") if ".git" not in p.parts)
    files += sorted(FIXTURES.glob("*.xls")) if FIXTURES.exists() else []
    if args.filter:
        files = [p for p in files if args.filter.lower() in str(p).lower()]

    tally = {k: 0 for k in ("OK", "BIFF5/7", "FILEPASS", "FAIL", "CRASH", "HANG")}
    crashes, fails = [], []
    g_files = g_match = g_mism = 0
    g_fail_files = []          # unexpected golden failures (regressions)
    g_known = []               # documented gaps that still fail (expected)
    g_fixed = []               # baseline entries that now PASS -> remove from known_gaps.txt

    for p in files:
        gpath = golden_path_for(p)
        has_golden = gpath.exists()
        if args.golden_only and not has_golden:
            continue
        status, data = probe(p, args.timeout)
        rel = p.relative_to(REPO) if REPO in p.parents else p
        if status in ("CRASH", "HANG", "FAIL"):
            tally[status] += 1
            (crashes if status != "FAIL" else fails).append((str(rel), data))
            print(f"  [{status:8}] {rel}  :: {data}")
            continue
        cat, fc, nc, sc, warn = classify(data)
        tally[cat] += 1
        line = f"  [{cat:8}] {rel}  sheets={sc} formulas={fc} names={nc}"
        gsuffix = ""
        if has_golden:
            try:
                m, mm, missing, extra, samples = diff_golden(data, gpath)
                g_files += 1; g_match += m; g_mism += mm
                ok = (mm == 0 and not missing)
                key = golden_key(p)
                known = key in baseline
                tag = "PASS" if ok else ("KNOWN-GAP" if known else "FAIL")
                gsuffix = f"  GOLDEN {tag} ({m} match, {mm} mismatch, {len(missing)} missing)"
                if not ok and known:
                    g_known.append(str(rel))
                elif not ok:
                    g_fail_files.append((str(rel), samples, missing, extra))
                elif known:
                    g_fixed.append((str(rel), key))
            except Exception as e:
                gsuffix = f"  GOLDEN ERR {e}"
        if args.verbose or has_golden or cat in ("BIFF5/7",):
            print(line + gsuffix)

    print("\n" + "=" * 72)
    print("ROBUSTNESS:", "  ".join(f"{k}={v}" for k, v in tally.items()), f"  (of {len(files)} files)")
    if fails:
        print(f"\nFAIL ({len(fails)}):")
        for f, e in fails[:20]:
            print(f"    {f}\n        {e}")
    if crashes:
        print(f"\nCRASH/HANG ({len(crashes)}):")
        for f, e in crashes:
            print(f"    {f}  {e}")
    print(f"\nGOLDEN: {g_files} diffed, {g_match} formulas matched, {g_mism} mismatched  |  "
          f"{len(g_known)} known-gap (expected), {len(g_fail_files)} UNEXPECTED-FAIL")
    if g_known:
        print(f"  known gaps (see golden/KNOWN_GAPS.md): {', '.join(Path(f).name for f in g_known)}")
    for f, samples, missing, extra in g_fail_files[:15]:
        print(f"    UNEXPECTED-FAIL {f}")
        for s in samples:
            print(f"        MISMATCH {s}")
        if missing:
            print(f"        MISSING(oracle-only): {missing}")
        if extra:
            print(f"        EXTRA(ours-only): {extra}")
    if g_fixed:
        print("  BASELINE NOW PASSES (remove from known_gaps.txt):")
        for f, key in g_fixed:
            print(f"        {key}")

    # regression gate: fail only on crashes/hangs or NON-baseline golden failures
    sys.exit(1 if (tally['CRASH'] or tally['HANG'] or g_fail_files) else 0)


if __name__ == "__main__":
    main()
