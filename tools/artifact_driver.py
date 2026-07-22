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

def make_artifact(path):
    sha = hashlib.sha256(open(path, "rb").read()).hexdigest()   # content address
    out = os.path.join(OUTDIR, sha + ".parquet")
    if os.path.exists(out):                                     # idempotent / dedup
        return (sha, os.path.getsize(out), "dedup")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    p   = path.replace("'", "''")
    tmp = f"{out}.{os.getpid()}.tmp"                            # atomic write
    con.execute(f"""
        COPY (SELECT * FROM bb_xlsx('{p}') ORDER BY page_id, y, x)
        TO '{tmp}' (FORMAT PARQUET, COMPRESSION 'zstd',
                    KV_METADATA {{ bbox_header: xlsx_header('{p}') }})""")
    con.close()
    os.replace(tmp, out)
    return (sha, os.path.getsize(out), "write")

if __name__ == "__main__":
    label, files = sys.argv[1], sys.argv[2:]
    os.makedirs(OUTDIR, exist_ok=True)
    t0 = time.perf_counter()
    with mp.Pool(mp.cpu_count()) as pool:
        res = pool.map(make_artifact, files)
    dt = time.perf_counter() - t0
    written = [r for r in res if r[2] == "write"]
    deduped = [r for r in res if r[2] == "dedup"]
    uniq    = len(set(r[0] for r in res))
    src_bytes = sum(os.path.getsize(f) for f in files)
    art_bytes = sum(os.path.getsize(os.path.join(OUTDIR, r[0] + ".parquet")) for r in res)
    print(f"[{label}] {len(files)} inputs -> {uniq} unique artifact(s) "
          f"| {len(written)} written, {len(deduped)} deduped-away | {dt:.2f}s")
    print(f"  {len(files)/dt:5.1f} inputs/s   {len(written)/dt:5.1f} artifacts/s written   "
          f"cores={mp.cpu_count()}")
    print(f"  source {src_bytes/1e6:.1f} MB -> artifacts {art_bytes/1e6:.1f} MB "
          f"({src_bytes/art_bytes:.1f}x smaller)")
