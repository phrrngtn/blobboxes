# blobboxes — Zig port

Done on macOS arm64 and Linux x86_64. CMake is gone (317 lines), and with it
every `brew --prefix` call in the repo.

## What replaced what

| was | now |
| --- | --- |
| PDFium linked, `libpdfium` co-located and rpath'd | **dlopen'd** — the extension is one self-contained file |
| hash-library (3-file static lib + a synthesised `<endian.h>` on macOS) | `std.crypto.hash.sha2` in `src/bboxes_zig.zig` |
| lexbor and libxls from `brew --prefix` | fetched and Zig-built from source |
| xlnt built by its own CMake | Zig-built, 67 C++ + 2 C sources + a 9-file libstudxml |
| nanobind + scikit-build-core | ctypes over the same C ABI, `py3-none-<platform>` wheels |
| `FetchContent` git tags | content-addressed hashes in `build.zig.zon` |

## PDFium needs no shim, unlike HiGHS in blobsolver

Worth stating because it looks like the same problem and is not. PDFium is far
more C++ inside than HiGHS, yet caused none of the trouble:

- its public headers are **pure C**, so nothing about its C++ ABI reaches our
  link line;
- it ships **shared only**, so its C++ runtime is sealed inside the dylib by
  construction.

That is exactly the arrangement blobsolver had to *build* for HiGHS with a g++
shim and a version script. A fat C++ dependency is safe when someone has already
put a C boundary around it.

Since the surface is only 35 functions, `include/pdfium_dyn.h` now resolves them
through `dlopen` instead of linking: an X-macro names each one, `decltype` on
the real prototype gives the pointer type, and a `#define` per name redirects
the call sites — so **`bboxes_pdf.cpp` did not change**. Only functions are
redirected; PDFium's typedefs must stay out of that list.

The payoff: the extension loads and reads spreadsheets with **no libpdfium
present at all** (verified), instead of failing to load entirely.

## Traps hit, so they are not rediscovered

- **PDFium's macOS install name is `./libpdfium.dylib`** — a *relative* path, so
  every rpath entry is ignored. CMake fixed this with `install_name_tool -id`
  mutating the download in place; `zig-pkg` is content-addressed, so we produce
  a corrected copy as a build output instead.
- **miniz 3.x is four translation units**, not one. Linking `miniz.c` alone
  compiles and then fails with fourteen undefined `mz_zip_*`/`tdefl_*`.
- **xlnt's source tarball is unbuildable** — five git submodules, none of which
  a GitHub source archive carries. Upstream publishes
  `xlnt-1.6.1_with_submodules.tar.gz`.
- **Grepping `configure_file` misses `generate_export_header`.** That is how
  "xlnt has no generated headers" got recorded and was wrong, at a cost of 72
  compile errors. Both `miniz_export.h` and `xlnt_cmake_export.h` are now
  hand-authored in `third_party/`.
- **"Enumerate, do not glob" fails if the enumeration is a glob** with the wrong
  pattern: a `*.cpp` sweep of xlnt dropped `sha1.c` and `sha512.c` and failed at
  link rather than compile.
- **libxls needs five config macros, not three.** Grepping `HAVE_` found three
  and missed `ICONV_CONST` and `PACKAGE_VERSION`. The complete sweep is recorded
  in `third_party/libxls/config.h`.
- **lexbor's `ports` module is per-platform.** posix/ and windows_nt/ define the
  same three functions; compiling both fails on `windows.h`.
- **glibc hides POSIX under `-std=c11`.** libxls compiled on macOS and produced
  32 errors on the first native Linux build — `strdup`, `locale_t`,
  `newlocale`, `LC_CTYPE_MASK`. Fixed with `-D_GNU_SOURCE` on Linux.
- **dlopen without guards is a segfault, not a graceful failure.** The first
  version skipped `FPDF_InitLibrary` when the library was absent but nothing
  stopped `extract_pdf` from calling a null pointer. Every PDF entry point now
  checks `bb_pdfium_load()`. Found by reading the exit code — the output was
  empty either way, and 139 is easy to miss.

## Deliberately unchanged

`bb()` yields **zero rows** for a file it cannot parse rather than raising. The
DuckDB shim documents why: glob and batch scans over a corpus must skip a bad
file, not abort the query. `bb_pdfium_error()` carries the diagnosis for anyone
asking why their PDFs came back empty.

## Verified

Both platforms, identical results:

| | macOS arm64 | Linux x86_64 (dc1) |
| --- | --- | --- |
| `zig build` | silent | silent |
| pdf / xlsx / xls / docx via DuckDB | 107 / 54 / 68 / 35 | 107 / 54 / 68 / 35 |
| SQLite | 107 | 107 |
| pdfium as a link dependency | 0 | 0 |

Plus, on macOS: `bb_fonts` and `bb_styles` from the xlnt path; the Python ctypes
layer agreeing with the extension on every count; the SHA-256 replacement
matching `shasum -a 256` on a real file; and `test/run_corpus.py` over 470 `.xls`
files with **0 unexpected failures** and 2,339 formulas matched against the
golden oracle.

## Not done

- **xlsx/xls parser rationalization** — deferred by decision, not blocked. Nine
  parsing engines are linked; the pugixml fast path could replace xlnt and the
  BIFF walker could replace libxls. See the memory note and the discussion of
  2026-07-30.
- **Two corpus files OOM** (`enron_3.319803.xls`, a clusterfuzz testcase), both
  small, so an unchecked allocation from a malformed record length. Left
  unbaselined on purpose: adding them to `known_gaps.txt` would hide a real bug.
