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

/* ── cursor ──────────────────────────────────────────────────────── */

typedef struct bboxes_cursor bboxes_cursor;

/* Auto-detecting open — inspects magic bytes and dispatches. */
bboxes_cursor* bboxes_open(const void* buf, size_t len);

bboxes_cursor* bboxes_open_pdf(const void* buf, size_t len,
                                const char* password,
                                int start_page, int end_page);

bboxes_cursor* bboxes_open_xlsx(const void* buf, size_t len,
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

void bboxes_close(bboxes_cursor* cursor);

#ifdef __cplusplus
}
#endif

#endif
