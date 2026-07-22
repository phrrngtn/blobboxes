#ifndef BBOXES_H
#define BBOXES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── backend lifecycle ───────────────────────────────────────────── */

void bboxes_pdf_init(void);
void bboxes_pdf_destroy(void);

void bboxes_xlsx_init(void);     /* no-op (xlnt needs no global init) */
void bboxes_xlsx_destroy(void);  /* no-op */

/* ── struct types ────────────────────────────────────────────────── */

typedef struct {
    uint32_t    document_id;
    const char* source_type;   /* "pdf", "xlsx", "text", "docx", ... */
    const char* filename;      /* NULL for buffer-based open  */
    const char* checksum;      /* MD5 hex of source bytes */
    int         page_count;
} bboxes_doc;

typedef struct {
    uint32_t    page_id;
    uint32_t    document_id;
    int         page_number;   /* 1-based */
    double      width, height;
} bboxes_page;

typedef struct {
    uint32_t    font_id;
    const char* name;
} bboxes_font;

typedef struct {
    uint32_t    style_id;
    uint32_t    font_id;
    double      font_size;
    const char* color;         /* "rgba(r,g,b,a)" */
    const char* weight;        /* "normal" or "bold" */
    int         italic;        /* 0 or 1 */
    int         underline;     /* 0 or 1 */
} bboxes_style;

typedef struct {
    uint32_t    page_id;
    uint32_t    style_id;
    double      x, y, w, h;
    const char* text;
    const char* formula;    /* raw formula string, NULL if none */
} bboxes_bbox;

/* ── cursor ──────────────────────────────────────────────────────── */
/*
 * Single cursor opened once per document.
 * Separate iterators for each object type.
 *
 * bboxes_open_pdf: start_page/end_page are 1-based inclusive.
 *                  Pass 0,0 for all pages. Returns NULL on bad PDF.
 */

/* ── format detection ────────────────────────────────────────────── */

/* Returns "pdf", "xlsx", "docx", or "text" based on magic bytes. */
const char* bboxes_detect(const void* buf, size_t len);

/* ── document metadata (full-take JSON clob; not the bboxes stream) ──
 *
 * Dialect-specific structural / provenance metadata → one JSON string.
 * Distinct from recognition (what a blob is) and from the positioned
 * content stream (cells/text/geometry). Returned pointer is owned by a
 * thread-local buffer, valid until the next call on the same thread.
 *
 * The *_file entry points are stream-oriented: miniz seeks to the zip's
 * central directory (tail) and inflates ONLY the metadata parts, never
 * the whole workbook body.
 */
const char* bboxes_xlsx_metadata_json(const void* buf, size_t len);
const char* bboxes_xlsx_metadata_json_file(const char* path);

/* Manifest: central-directory-only structural view (the tail tier) — part
   list + booleans (has_vba, worksheet_parts, ...), no part bodies read. */
const char* bboxes_xlsx_manifest_json(const void* buf, size_t len);
const char* bboxes_xlsx_manifest_json_file(const char* path);

/* Biconditional style decode: styles.xml + theme1.xml resolved to a workbook-
   global decode keyed to the fast reader's raw cellXfs `s`. Emits {dialect,
   date1904, theme_palette[], style_decode:[{id,numfmt,font,fill,border,
   named_style}], s_to_id:[canonical id per raw s]} — the metadata that lets a
   Parquet artifact un-intern style_id. */
const char* bboxes_xlsx_style_decode_json(const void* buf, size_t len);
const char* bboxes_xlsx_style_decode_json_file(const char* path);

/* Lean global metadata (docProps/workbook/names/tables) — skips the per-worksheet
   loop that re-inflates the bulk; the artifact pipeline is single-pass. */
const char* bboxes_xlsx_artifact_meta_json(const void* buf, size_t len);
const char* bboxes_xlsx_artifact_meta_json_file(const char* path);

/* Path/blob wrappers for the sheet-meta side-channel (open fast cursor, pull, close). */
const char* bboxes_xlsx_sheet_meta_json(const void* buf, size_t len);
const char* bboxes_xlsx_sheet_meta_json_file(const char* path);

/* Combined artifact HEADER (the footer bag): sha256 workbook id + style/theme
   decode + lean global metadata, one zip open, small parts only (never the
   worksheets — disjoint from the bb_xlsx body pass). */
const char* bboxes_xlsx_header_json(const void* buf, size_t len);
const char* bboxes_xlsx_header_json_file(const char* path);

/* One-parse PDF artifact header: sha + page_count + fonts + styles + page dims
   from a SINGLE PDFium cursor (fonts/styles are byproducts of the content parse,
   so this replaces 3 separate re-parses). Blob and file variants. */
const char* bboxes_pdf_header_json(const void* buf, size_t len);
const char* bboxes_pdf_header_json_file(const char* path);

/* Recursive container walk: a blob is a tree of typed blobs. Sniffs each node
   by magic bytes, dispatches to the matching extractor, recurses into nested
   containers (zip-in-zip). Returns a nested JSON tree. */
const char* bboxes_container_walk_json(const void* buf, size_t len);
const char* bboxes_container_walk_json_file(const char* path);

/* Raw xl/vbaProject.bin as base64 ("" when absent). blobboxes does not parse
   the OLE2/CFB container — this hands the bytes to a downstream library. */
const char* bboxes_xlsx_vba_base64(const void* buf, size_t len);
const char* bboxes_xlsx_vba_base64_file(const char* path);

/* XFDF overlay emitter (the inverse of extraction): classification annotations
   as JSON → an XFDF document a PDF viewer renders as overlays without mutating
   the file. Input is {href?, annots:[{page,x,y,w,h,page_height,type,color,
   subject,contents,...}]} (or a bare annots array); top-left-origin rects are
   flipped to PDF bottom-left space per annot via page_height. */
const char* bboxes_xfdf_from_json(const char* annots_json);

/* PDF metadata: Info dict + structural (version, pages, encryption, tagged).
   The *_file variant streams via FPDF_LoadCustomDocument — PDFium reads only
   the trailer/xref/referenced objects, not the whole file. Assumes
   bboxes_pdf_init() has been called; serialized on the PDFium mutex. */
const char* bboxes_pdf_metadata_json(const void* buf, size_t len);
const char* bboxes_pdf_metadata_json_file(const char* path);

/* ── cursor ──────────────────────────────────────────────────────── */

typedef struct bboxes_cursor bboxes_cursor;

/* Auto-detecting open — inspects magic bytes and dispatches. */
bboxes_cursor* bboxes_open(const void* buf, size_t len);

bboxes_cursor* bboxes_open_pdf(const void* buf, size_t len,
                                const char* password,
                                int start_page, int end_page);

/* Object-level PDF: one bbox per text object (word/phrase).
   Clean text without downstream merging — for interactive exploration. */
bboxes_cursor* bboxes_open_pdf_objects(const void* buf, size_t len,
                                        const char* password,
                                        int start_page, int end_page);

bboxes_cursor* bboxes_open_xlsx(const void* buf, size_t len,
                                 const char* password,
                                 int start_page, int end_page);

bboxes_cursor* bboxes_open_xlsx_fast(const void* buf, size_t len,
                                     const char* password,
                                     int start_page, int end_page);

bboxes_cursor* bboxes_open_text(const void* buf, size_t len);

bboxes_cursor* bboxes_open_docx(const void* buf, size_t len);

/* doc (single row, not an iterator) */
const bboxes_doc*   bboxes_get_doc(bboxes_cursor* cursor);
const char*         bboxes_get_doc_json(bboxes_cursor* cursor);

/* page iterator */
const bboxes_page*  bboxes_next_page(bboxes_cursor* cursor);
const char*         bboxes_next_page_json(bboxes_cursor* cursor);

/* font iterator */
const bboxes_font*  bboxes_next_font(bboxes_cursor* cursor);
const char*         bboxes_next_font_json(bboxes_cursor* cursor);

/* style iterator */
const bboxes_style* bboxes_next_style(bboxes_cursor* cursor);
const char*         bboxes_next_style_json(bboxes_cursor* cursor);

/* bbox iterator */
const bboxes_bbox*  bboxes_next_bbox(bboxes_cursor* cursor);
const char*         bboxes_next_bbox_json(bboxes_cursor* cursor);

/* array-level JSON (returns entire array as a single string) */
const char* bboxes_get_pages_json(bboxes_cursor* cursor);
const char* bboxes_get_fonts_json(bboxes_cursor* cursor);
const char* bboxes_get_styles_json(bboxes_cursor* cursor);
const char* bboxes_get_bboxes_json(bboxes_cursor* cursor);
/* Per-sheet side-channel (merges + dimension) captured during the cell scan —
   pull from the SAME cursor as the bboxes for a single worksheet inflation. */
const char* bboxes_get_sheet_meta_json(bboxes_cursor* cursor);

void bboxes_close(bboxes_cursor* cursor);

/* ── format codes for bboxes_open_format() ──────────────────────── */
#define BBOXES_FORMAT_AUTO         0
#define BBOXES_FORMAT_PDF          1
#define BBOXES_FORMAT_XLSX         2
#define BBOXES_FORMAT_TEXT         3
#define BBOXES_FORMAT_DOCX         4
#define BBOXES_FORMAT_PDF_OBJECTS  5  /* object-level PDF: one bbox per text object */
#define BBOXES_FORMAT_XLSX_FAST    6  /* fast byte-scan xlsx reader (parallel to XLSX) */

bboxes_cursor* bboxes_open_format(int fmt, const void* buf, size_t len);

/* Coordinate model (single source of truth — hosts must not re-encode this).
   Returns 1 for cell-grid formats (xlsx/text/docx) whose bbox x/y/w/h are
   integer row/col positions, 0 for rendered formats (pdf) with float coords. */
int bboxes_format_int_coords(int fmt);

const char *bboxes_errmsg(void);

#ifdef __cplusplus
}
#endif

#endif
