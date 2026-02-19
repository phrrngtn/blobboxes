#ifndef PDF_BBOXES_H
#define PDF_BBOXES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pdf_bboxes_init(void);
void pdf_bboxes_destroy(void);

/* ── struct types ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t font_id;
    int      page;
    double   x, y, w, h;
    const char* text;
    const char* color;
    double   font_size;
    const char* style;
} pdf_bboxes_run;

typedef struct {
    uint32_t font_id;
    const char* name;
    int      flags;
    const char* style;
} pdf_bboxes_font;

/* ── extract cursor ───────────────────────────────────────────────── */
/*
 * start_page / end_page are 1-based inclusive. Pass 0,0 for all pages.
 * Returns NULL on bad PDF.
 */
typedef struct pdf_bboxes_cursor pdf_bboxes_cursor;

pdf_bboxes_cursor*      pdf_bboxes_extract_open(const void* buf, size_t len,
                                                 const char* password,
                                                 int start_page, int end_page);
const pdf_bboxes_run*   pdf_bboxes_extract_next(pdf_bboxes_cursor* cursor);
const char*             pdf_bboxes_extract_next_json(pdf_bboxes_cursor* cursor);
void                    pdf_bboxes_extract_close(pdf_bboxes_cursor* cursor);

/* ── font cursor ──────────────────────────────────────────────────── */

typedef struct pdf_bboxes_font_cursor pdf_bboxes_font_cursor;

pdf_bboxes_font_cursor* pdf_bboxes_fonts_open(const void* buf, size_t len,
                                               const char* password);
const pdf_bboxes_font*  pdf_bboxes_fonts_next(pdf_bboxes_font_cursor* cursor);
const char*             pdf_bboxes_fonts_next_json(pdf_bboxes_font_cursor* cursor);
void                    pdf_bboxes_fonts_close(pdf_bboxes_font_cursor* cursor);

#ifdef __cplusplus
}
#endif

#endif
