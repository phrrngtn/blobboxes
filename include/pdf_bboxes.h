#ifndef PDF_BBOXES_H
#define PDF_BBOXES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback receives a JSON string per entry. Return 0 to continue, non-zero to abort. */
typedef int (*pdf_bbox_callback)(const char* json, void* user_data);

/* Initialize / destroy the underlying PDF library. Call once per process. */
void pdf_bboxes_init(void);
void pdf_bboxes_destroy(void);

/*
 * Extract text bounding boxes from a PDF loaded from memory.
 *   buf / len   — raw PDF bytes
 *   password    — document password, or NULL
 *   cb          — called once per text run with a JSON string
 *   user_data   — forwarded to cb
 *
 * Returns 0 on success, -1 on error (bad PDF, etc.).
 * If the callback returns non-zero, extraction stops and that value is returned.
 */
int pdf_bboxes_extract(const void* buf, size_t len, const char* password,
                       pdf_bbox_callback cb, void* user_data);

/*
 * Extract the font table from a PDF loaded from memory.
 * Callback receives one JSON object per unique font.
 * Same return semantics as pdf_bboxes_extract.
 */
int pdf_bboxes_fonts(const void* buf, size_t len, const char* password,
                     pdf_bbox_callback cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* PDF_BBOXES_H */
