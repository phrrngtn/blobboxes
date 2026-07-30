/*
 * pdfium_dyn.h — resolve PDFium at runtime instead of linking against it.
 *
 * Why: PDFium ships shared-only. Every bblanchon release carries
 * lib/libpdfium.{dylib,so} and no static variant, so a link-time dependency
 * means the extension is not a single file — libpdfium must sit beside it and
 * be found through an rpath, and if it is missing the *whole extension* fails
 * to load. A DuckDB user then sees "failed to load extension" for a file they
 * only wanted to read a spreadsheet with.
 *
 * With dlopen the extension is one self-contained file, loads regardless, and
 * the PDF backend reports a clear error when libpdfium is absent while every
 * other format keeps working.
 *
 * How: PDFium's public headers are pure C, so the whole surface we use is 35
 * ordinary functions. The X-macro below names them once; from it we declare a
 * function pointer per entry and then `#define` each PDFium name onto its
 * pointer, so **no call site changes**. bboxes_pdf.cpp still reads as if it
 * were linking normally.
 *
 * Only functions are redirected. PDFium's typedefs (FPDF_DOCUMENT, FPDF_PAGE,
 * FPDF_TEXTPAGE, ...) are untouched, which is why the macro list must stay
 * exactly the set of *functions* — adding a type name here would rewrite a
 * declaration into nonsense.
 *
 * The same arrangement blobhttp uses for GSS-API, and the shape blobsolver had
 * to build by hand for HiGHS. See build.zig.
 */

#ifndef BBOXES_PDFIUM_DYN_H
#define BBOXES_PDFIUM_DYN_H

/* Real headers first: the pointer declarations below use decltype on the
 * genuine prototypes, so these must be seen before anything is redefined. */
#include <fpdfview.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <fpdf_doc.h>
#include <fpdf_catalog.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every PDFium function blobboxes calls. Keep sorted by header for review. */
#define BBOXES_PDFIUM_FUNCS(X)                                                 \
    /* fpdfview.h — library, document, page lifecycle */                       \
    X(FPDF_InitLibrary)                                                        \
    X(FPDF_DestroyLibrary)                                                     \
    X(FPDF_LoadMemDocument)                                                    \
    X(FPDF_LoadCustomDocument)                                                 \
    X(FPDF_CloseDocument)                                                      \
    X(FPDF_LoadPage)                                                           \
    X(FPDF_ClosePage)                                                          \
    X(FPDF_GetPageCount)                                                       \
    X(FPDF_GetPageWidth)                                                       \
    X(FPDF_GetPageHeight)                                                      \
    X(FPDF_GetLastError)                                                       \
    X(FPDF_GetFileVersion)                                                     \
    X(FPDF_GetDocPermissions)                                                  \
    X(FPDF_GetSecurityHandlerRevision)                                         \
    /* fpdf_doc.h — metadata */                                                \
    X(FPDF_GetMetaText)                                                        \
    /* fpdf_catalog.h */                                                       \
    X(FPDFCatalog_IsTagged)                                                    \
    /* fpdf_text.h — character-level extraction */                             \
    X(FPDFText_LoadPage)                                                       \
    X(FPDFText_ClosePage)                                                      \
    X(FPDFText_CountChars)                                                     \
    X(FPDFText_GetUnicode)                                                     \
    X(FPDFText_GetCharBox)                                                     \
    X(FPDFText_GetFontSize)                                                    \
    X(FPDFText_GetFontInfo)                                                    \
    X(FPDFText_GetFillColor)                                                   \
    /* fpdf_edit.h — object-level extraction */                                \
    X(FPDFPage_CountObjects)                                                   \
    X(FPDFPage_GetObject)                                                      \
    X(FPDFPageObj_GetType)                                                     \
    X(FPDFPageObj_GetBounds)                                                   \
    X(FPDFPageObj_GetFillColor)                                                \
    X(FPDFTextObj_GetText)                                                     \
    X(FPDFTextObj_GetFont)                                                     \
    X(FPDFTextObj_GetFontSize)                                                 \
    X(FPDFFont_GetFamilyName)                                                  \
    X(FPDFFont_GetFlags)                                                       \
    X(FPDFFont_GetWeight)

/* One pointer per function, holding the real prototype's type. */
#define BBOXES_PDFIUM_DECL(name) extern decltype(&::name) bb_dyn_##name;
BBOXES_PDFIUM_FUNCS(BBOXES_PDFIUM_DECL)
#undef BBOXES_PDFIUM_DECL

/*
 * Load libpdfium and resolve every pointer above.
 *
 * Idempotent and safe to call from multiple threads. Returns 1 on success, 0 if
 * the library could not be found or a symbol was missing — in which case
 * bb_pdfium_error() explains, and no pointer is left half-resolved.
 *
 * Search order: $BBOXES_PDFIUM_PATH if set, then beside this extension
 * (@loader_path / $ORIGIN, found via dladdr), then the plain soname so the
 * system loader's own paths apply.
 */
int bb_pdfium_load(void);

/* Human-readable reason the last bb_pdfium_load() failed, or NULL. */
const char *bb_pdfium_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/*
 * Redirect the call sites. Must come after the declarations above, and after
 * the real headers, so that only *uses* are rewritten and never declarations.
 *
 * pdfium_dyn.cpp defines BBOXES_PDFIUM_NO_REDIRECT before including this,
 * because it has to name the real functions to take their addresses — with the
 * redirect active, `&::FPDF_InitLibrary` would rewrite to
 * `&::bb_dyn_FPDF_InitLibrary`, i.e. the pointer's own address.
 *
 * Expanded by hand rather than through the X-macro: a macro cannot emit
 * `#define`, and doing it another way would hide which names are shadowed.
 */
#ifndef BBOXES_PDFIUM_NO_REDIRECT
#define FPDF_InitLibrary              bb_dyn_FPDF_InitLibrary
#define FPDF_DestroyLibrary           bb_dyn_FPDF_DestroyLibrary
#define FPDF_LoadMemDocument          bb_dyn_FPDF_LoadMemDocument
#define FPDF_LoadCustomDocument       bb_dyn_FPDF_LoadCustomDocument
#define FPDF_CloseDocument            bb_dyn_FPDF_CloseDocument
#define FPDF_LoadPage                 bb_dyn_FPDF_LoadPage
#define FPDF_ClosePage                bb_dyn_FPDF_ClosePage
#define FPDF_GetPageCount             bb_dyn_FPDF_GetPageCount
#define FPDF_GetPageWidth             bb_dyn_FPDF_GetPageWidth
#define FPDF_GetPageHeight            bb_dyn_FPDF_GetPageHeight
#define FPDF_GetLastError             bb_dyn_FPDF_GetLastError
#define FPDF_GetFileVersion           bb_dyn_FPDF_GetFileVersion
#define FPDF_GetDocPermissions        bb_dyn_FPDF_GetDocPermissions
#define FPDF_GetSecurityHandlerRevision bb_dyn_FPDF_GetSecurityHandlerRevision
#define FPDF_GetMetaText              bb_dyn_FPDF_GetMetaText
#define FPDFCatalog_IsTagged          bb_dyn_FPDFCatalog_IsTagged
#define FPDFText_LoadPage             bb_dyn_FPDFText_LoadPage
#define FPDFText_ClosePage            bb_dyn_FPDFText_ClosePage
#define FPDFText_CountChars           bb_dyn_FPDFText_CountChars
#define FPDFText_GetUnicode           bb_dyn_FPDFText_GetUnicode
#define FPDFText_GetCharBox           bb_dyn_FPDFText_GetCharBox
#define FPDFText_GetFontSize          bb_dyn_FPDFText_GetFontSize
#define FPDFText_GetFontInfo          bb_dyn_FPDFText_GetFontInfo
#define FPDFText_GetFillColor         bb_dyn_FPDFText_GetFillColor
#define FPDFPage_CountObjects         bb_dyn_FPDFPage_CountObjects
#define FPDFPage_GetObject            bb_dyn_FPDFPage_GetObject
#define FPDFPageObj_GetType           bb_dyn_FPDFPageObj_GetType
#define FPDFPageObj_GetBounds         bb_dyn_FPDFPageObj_GetBounds
#define FPDFPageObj_GetFillColor      bb_dyn_FPDFPageObj_GetFillColor
#define FPDFTextObj_GetText           bb_dyn_FPDFTextObj_GetText
#define FPDFTextObj_GetFont           bb_dyn_FPDFTextObj_GetFont
#define FPDFTextObj_GetFontSize       bb_dyn_FPDFTextObj_GetFontSize
#define FPDFFont_GetFamilyName        bb_dyn_FPDFFont_GetFamilyName
#define FPDFFont_GetFlags             bb_dyn_FPDFFont_GetFlags
#define FPDFFont_GetWeight            bb_dyn_FPDFFont_GetWeight

#endif /* BBOXES_PDFIUM_NO_REDIRECT */

#endif /* BBOXES_PDFIUM_DYN_H */
