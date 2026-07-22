"""Fetch a clean-IP PDF conformance / edge-case test corpus for stressing the
readers. Sources are public, openly-licensed suites built to break PDF parsers:

  - mozilla/pdf.js test/pdfs  (Apache-2.0) — real-world edge cases: encrypted,
    linearized, malformed, unusual versions, forms, annotations, CJK, images.
  - veraPDF-corpus (staging)  (CC BY 4.0 / BSD) — PDF/A + PDF/UA conformance
    pass/fail files, one per spec clause.

The PDFs are downloaded to a target dir (kept OUT of git — commit this fetcher,
not the binaries). Usage:  python fetch_pdf_testsuite.py <out_dir> [count]
"""
import json, os, sys, urllib.request

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "blobboxes-testsuite"})
    return urllib.request.urlopen(req, timeout=30).read()

def gh_dir(repo, path, ref="master"):
    return json.loads(get(f"https://api.github.com/repos/{repo}/contents/{path}?ref={ref}"))

def main(out, count):
    os.makedirs(out, exist_ok=True)
    got = 0
    # 1) pdf.js — even sample across the alphabetized set (diverse edge cases)
    listing = gh_dir("mozilla/pdf.js", "test/pdfs")
    pdfs = sorted([x for x in listing
                   if x["name"].endswith(".pdf") and x.get("download_url")
                   and 512 <= x["size"] <= 4_000_000], key=lambda x: x["name"])
    step = max(1, len(pdfs) // max(1, count))
    for x in pdfs[::step][:count]:
        try:
            open(os.path.join(out, "pdfjs__" + x["name"]), "wb").write(get(x["download_url"]))
            got += 1
        except Exception as e:
            print("  skip", x["name"], e)
    # 2) veraPDF PDF/A-1b — a few conformance pass/fail files per clause dir
    try:
        clauses = gh_dir("veraPDF/veraPDF-corpus", "PDF_A-1b", ref="staging")
        for cl in [c for c in clauses if c["type"] == "dir"][:6]:
            for sub in [s for s in gh_dir("veraPDF/veraPDF-corpus", cl["path"], ref="staging")
                        if s["type"] == "dir"][:1]:
                for f in [g for g in gh_dir("veraPDF/veraPDF-corpus", sub["path"], ref="staging")
                          if g["name"].endswith(".pdf") and g.get("download_url")][:2]:
                    open(os.path.join(out, "vera__" + f["name"].replace(" ", "_")), "wb").write(
                        get(f["download_url"]))
                    got += 1
    except Exception as e:
        print("  veraPDF skipped:", e)
    print(f"fetched {got} PDFs into {out}")

if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 40)
