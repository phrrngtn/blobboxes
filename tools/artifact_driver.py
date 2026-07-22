"""Per-file Parquet artifact driver for blobboxes.

Each input workbook -> <sha256>.parquet: cells sorted on write (page_id, y, x) +
the artifact header (sha id + biconditional style/theme decode + lean global meta)
in the Parquet footer key-value metadata. The sha256 filename makes each artifact
content-addressed: free exact-dup dedup, idempotent reprocessing, tamper-evident
integrity (name == content hash), and a per-object governance/ACL unit. Query a
whole (permitted) corpus as one relation with read_parquet('dir/*.parquet').

The expensive xlsx parse is paid ONCE here; downstream FAST-checks read the
columnar cells (~80x faster than re-parsing) via read_parquet.

Usage:
    EXT=/path/to/bboxes.duckdb_extension \
    OUTDIR=/path/to/artifacts \
    uv run --no-project --with duckdb python artifact_driver.py <label> file1.xlsx file2.xlsx ...

Requires the duckdb Python module and the built (unsigned) bboxes DuckDB extension.
Runs one process per core; each worker gets its own DuckDB connection.
"""
import duckdb, hashlib, os, sys, time
import multiprocessing as mp

EXT    = os.environ["EXT"]
OUTDIR = os.environ["OUTDIR"]

# per-format: the cells table function + the footer header (KV metadata). The
# header carries whatever the format interns and can decode:
#   xlsx — one disjoint call (styles.xml/theme, never the worksheets)
#   pdf  — fonts + styles + doc info; NB these come from the CONTENT parse, so
#          each call re-runs PDFium (serialized by its global mutex) — several
#          parses per artifact until a cursor side-channel is added
#   docx — only a single default style to decode (degenerate); header ~= sha
def _spec(p, sha, ext):
    if ext == ".xlsx":
        return f"bb_xlsx('{p}')", f"{{ bbox_header: xlsx_header('{p}') }}"
    if ext == ".pdf":
        # one-parse header (fonts+styles+dims from a single PDFium cursor), not
        # three separate re-parses; artifact = bb_pdf (cells) + pdf_header = 2 parses
        return f"bb_pdf('{p}')", f"{{ bbox_header: pdf_header('{p}') }}"
    if ext == ".docx":
        return f"bb_docx('{p}')", f"{{ sha256: '{sha}', styles: bb_docx_styles_json('{p}') }}"
    raise ValueError(f"unsupported: {ext}")

def make_artifact(path):
    ext = os.path.splitext(path)[1].lower()
    sha = hashlib.sha256(open(path, "rb").read()).hexdigest()   # content address
    out = os.path.join(OUTDIR, sha + ".parquet")
    if os.path.exists(out):                                     # idempotent / dedup
        return (sha, os.path.getsize(out), "dedup")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    p   = path.replace("'", "''")
    cells, kv = _spec(p, sha, ext)
    tmp = f"{out}.{os.getpid()}.tmp"                            # atomic write
    # COPY returns the row count. The extension is resilient (bad input -> 0 rows,
    # not an abort). But 0 rows has TWO very different meanings, so classify:
    #   - a VALID pdf with no text layer (image-only / scanned) is a legitimate
    #     METADATA-ONLY artifact (an OCR candidate), NOT a failure — it keeps its
    #     header and is a first-class citizen.
    #   - an input that isn't the claimed format at all (e.g. an HTML error page
    #     saved as .pdf) is REJECTED — the only thing worth flagging.
    rows = con.execute(f"""
        COPY (SELECT * FROM {cells} ORDER BY page_id, y, x)
        TO '{tmp}' (FORMAT PARQUET, COMPRESSION 'zstd', KV_METADATA {kv})""").fetchone()[0]
    status = "write"
    if rows == 0:
        status = "metadata-only"
        if ext == ".pdf":   # integrity distinguishes image-only (clean) from not-a-pdf (failed)
            integ = con.execute(
                f"SELECT json_extract_string(pdf_metadata('{p}'), '$.integrity.status')").fetchone()[0]
            if integ == "failed":
                status = "rejected"
    con.close()
    os.replace(tmp, out)
    return (sha, os.path.getsize(out), status)

if __name__ == "__main__":
    label, files = sys.argv[1], sys.argv[2:]
    os.makedirs(OUTDIR, exist_ok=True)
    t0 = time.perf_counter()
    with mp.Pool(mp.cpu_count()) as pool:
        res = pool.map(make_artifact, files)
    dt = time.perf_counter() - t0
    written  = [r for r in res if r[2] == "write"]
    metaonly = [r for r in res if r[2] == "metadata-only"]
    rejected = [r for r in res if r[2] == "rejected"]
    deduped  = [r for r in res if r[2] == "dedup"]
    uniq     = len(set(r[0] for r in res))
    src_bytes = sum(os.path.getsize(f) for f in files)
    art_bytes = sum(os.path.getsize(os.path.join(OUTDIR, r[0] + ".parquet")) for r in res)
    print(f"[{label}] {len(files)} inputs -> {uniq} unique artifact(s) | {dt:.2f}s")
    print(f"  {len(written)} with-text, {len(metaonly)} metadata-only (valid, no text), "
          f"{len(rejected)} REJECTED (not the claimed format), {len(deduped)} deduped")
    print(f"  {len(files)/dt:5.1f} inputs/s   {len(written)/dt:5.1f} artifacts/s written   "
          f"cores={mp.cpu_count()}")
    print(f"  source {src_bytes/1e6:.1f} MB -> artifacts {art_bytes/1e6:.1f} MB "
          f"({src_bytes/art_bytes:.1f}x smaller)")
