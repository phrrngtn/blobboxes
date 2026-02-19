#ifndef PDF_BBOXES_H
#define PDF_BBOXES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void pdf_bboxes_init(void);
void pdf_bboxes_destroy(void);

/*
 * Cursor-based extraction — open, pull JSON strings one at a time, close.
 *
 *   cursor = pdf_bboxes_extract_open(buf, len, NULL, 0, 0);
 *   while ((json = pdf_bboxes_extract_next(cursor)))
 *       process(json);
 *   pdf_bboxes_extract_close(cursor);
 *
 * start_page / end_page are 1-based inclusive. Pass 0,0 for all pages.
 * Returns NULL on bad PDF.
 */
typedef struct pdf_bboxes_cursor pdf_bboxes_cursor;

pdf_bboxes_cursor* pdf_bboxes_extract_open(const void* buf, size_t len,
                                            const char* password,
                                            int start_page, int end_page);
const char* pdf_bboxes_extract_next(pdf_bboxes_cursor* cursor);
void        pdf_bboxes_extract_close(pdf_bboxes_cursor* cursor);

/*
 * Font table — same cursor pattern but no page range (scans whole doc).
 *
 *   cursor = pdf_bboxes_fonts_open(buf, len, NULL);
 *   while ((json = pdf_bboxes_fonts_next(cursor)))
 *       process(json);
 *   pdf_bboxes_fonts_close(cursor);
 */
typedef struct pdf_bboxes_font_cursor pdf_bboxes_font_cursor;

pdf_bboxes_font_cursor* pdf_bboxes_fonts_open(const void* buf, size_t len,
                                               const char* password);
const char* pdf_bboxes_fonts_next(pdf_bboxes_font_cursor* cursor);
void        pdf_bboxes_fonts_close(pdf_bboxes_font_cursor* cursor);

#ifdef __cplusplus
}
#endif

#endif
