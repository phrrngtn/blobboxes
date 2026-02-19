#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "bboxes.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ══════════════════════════════════════════════════════════════════════
 * Format enum and dispatch
 * ══════════════════════════════════════════════════════════════════════ */

enum Format { FMT_PDF, FMT_XLSX, FMT_TEXT, FMT_DOCX, FMT_AUTO };

static bboxes_cursor* open_by_format(Format fmt, const void* buf, size_t len) {
    switch (fmt) {
        case FMT_PDF:   return bboxes_open_pdf(buf, len, nullptr, 0, 0);
        case FMT_XLSX:  return bboxes_open_xlsx(buf, len, nullptr, 0, 0);
        case FMT_TEXT:  return bboxes_open_text(buf, len);
        case FMT_DOCX:  return bboxes_open_docx(buf, len);
        case FMT_AUTO:  return bboxes_open(buf, len);
    }
    return nullptr;
}

/* ── File I/O ────────────────────────────────────────────────────── */

static std::vector<char> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<char> buf(sz);
    if (fread(buf.data(), 1, sz, f) != static_cast<size_t>(sz))
        buf.clear();
    fclose(f);
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════
 * Format-aware pAux helper
 * ══════════════════════════════════════════════════════════════════════ */

static Format* make_fmt(Format f) {
    static Format fmts[5] = { FMT_PDF, FMT_XLSX, FMT_TEXT, FMT_DOCX, FMT_AUTO };
    return &fmts[f];
}

/* ── Generic vtab struct that carries the Format ─────────────────── */

struct FmtVtab : sqlite3_vtab {
    Format fmt;
};

/* ══════════════════════════════════════════════════════════════════════
 * DEFINE_VTAB macro — generates Connect, Open, Close, Disconnect,
 * Filter, Next, Eof, Rowid, BestIndex, and the sqlite3_module struct.
 * Only the Column function is written manually per type.
 * ══════════════════════════════════════════════════════════════════════ */

#define DEFINE_VTAB(Name, DDL, CursorType, current_type, next_fn, hidden_col, est_cost) \
                                                                                        \
struct Name##Cursor : sqlite3_vtab_cursor {                                             \
    std::vector<char> buf;                                                              \
    bboxes_cursor* cur = nullptr;                                                       \
    current_type const* current = nullptr;                                              \
    bool eof = true;                                                                    \
    int64_t rowid = 0;                                                                  \
};                                                                                      \
                                                                                        \
static int Name##Connect(sqlite3* db, void* pAux, int, const char* const*,              \
                         sqlite3_vtab** ppVtab, char**) {                               \
    int rc = sqlite3_declare_vtab(db, DDL);                                             \
    if (rc != SQLITE_OK) return rc;                                                     \
    auto* v = new FmtVtab{};                                                            \
    v->fmt = *static_cast<Format*>(pAux);                                               \
    *ppVtab = v;                                                                        \
    return SQLITE_OK;                                                                   \
}                                                                                       \
                                                                                        \
static int Name##BestIndex(sqlite3_vtab*, sqlite3_index_info* info) {                   \
    for (int i = 0; i < info->nConstraint; i++) {                                       \
        if (info->aConstraint[i].iColumn == hidden_col &&                               \
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&                    \
            info->aConstraint[i].usable) {                                              \
            info->aConstraintUsage[i].argvIndex = 1;                                    \
            info->aConstraintUsage[i].omit = 1;                                         \
            info->estimatedCost = est_cost;                                             \
            return SQLITE_OK;                                                           \
        }                                                                               \
    }                                                                                   \
    return SQLITE_CONSTRAINT;                                                           \
}                                                                                       \
                                                                                        \
static int Name##Disconnect(sqlite3_vtab* pVtab) {                                     \
    delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK;                             \
}                                                                                       \
static int Name##Open(sqlite3_vtab*, sqlite3_vtab_cursor** pp) {                        \
    *pp = new Name##Cursor{}; return SQLITE_OK;                                         \
}                                                                                       \
                                                                                        \
static int Name##Close(sqlite3_vtab_cursor* pCursor) {                                  \
    auto* c = static_cast<Name##Cursor*>(pCursor);                                      \
    if (c->cur) bboxes_close(c->cur);                                                   \
    delete c;                                                                           \
    return SQLITE_OK;                                                                   \
}                                                                                       \
                                                                                        \
static int Name##Filter(sqlite3_vtab_cursor* pCursor, int, const char*,                 \
                        int argc, sqlite3_value** argv) {                               \
    auto* c = static_cast<Name##Cursor*>(pCursor);                                      \
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;                            \
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }                             \
    if (argc < 1) { c->eof = true; return SQLITE_OK; }                                  \
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));       \
    if (!path) { c->eof = true; return SQLITE_OK; }                                     \
    c->buf = read_file(path);                                                           \
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }                            \
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());                         \
    if (!c->cur) { c->eof = true; return SQLITE_OK; }                                   \
    c->current = next_fn(c->cur);                                                       \
    c->eof = (c->current == nullptr);                                                   \
    c->rowid = 0;                                                                       \
    return SQLITE_OK;                                                                   \
}                                                                                       \
                                                                                        \
static int Name##Next(sqlite3_vtab_cursor* pCursor) {                                   \
    auto* c = static_cast<Name##Cursor*>(pCursor);                                      \
    c->rowid++;                                                                         \
    c->current = next_fn(c->cur);                                                       \
    c->eof = (c->current == nullptr);                                                   \
    return SQLITE_OK;                                                                   \
}                                                                                       \
                                                                                        \
static int Name##Eof(sqlite3_vtab_cursor* pCursor) {                                    \
    return static_cast<Name##Cursor*>(pCursor)->eof ? 1 : 0;                            \
}                                                                                       \
                                                                                        \
static int Name##Rowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {           \
    *pRowid = static_cast<Name##Cursor*>(pCursor)->rowid; return SQLITE_OK;             \
}                                                                                       \
                                                                                        \
static int Name##Column(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col);   \
                                                                                        \
static sqlite3_module Name##Module = {                                                  \
    0, nullptr, Name##Connect, Name##BestIndex, Name##Disconnect,                       \
    Name##Disconnect, Name##Open, Name##Close, Name##Filter,                            \
    Name##Next, Name##Eof, Name##Column, Name##Rowid,                                   \
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,                      \
    nullptr, nullptr, nullptr, nullptr, nullptr                                         \
};

/* ══════════════════════════════════════════════════════════════════════
 * Doc virtual table — special: single row, Next just sets eof
 * ══════════════════════════════════════════════════════════════════════ */

/* Doc uses bboxes_get_doc (not an iterator), so we define it with a
   small wrapper that matches the next_fn signature for Filter,
   and override Next to just set eof. */

static const bboxes_doc* doc_get_wrapper(bboxes_cursor* cur) {
    return bboxes_get_doc(cur);
}

DEFINE_VTAB(Doc,
    "CREATE TABLE x(document_id INTEGER, source_type TEXT, "
    "filename TEXT, checksum TEXT, page_count INTEGER, file_path TEXT HIDDEN)",
    DocCursor, bboxes_doc, doc_get_wrapper, 5, 10.0)

/* Doc is single-row: override the macro-generated Next */
static int DocNextSingleRow(sqlite3_vtab_cursor* pCursor) {
    static_cast<DocCursor*>(pCursor)->eof = true;
    return SQLITE_OK;
}
static struct DocModuleFixup {
    DocModuleFixup() { DocModule.xNext = DocNextSingleRow; }
} s_doc_module_fixup;

static int DocColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* d = static_cast<DocCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, d->document_id); break;
        case 1: sqlite3_result_text(ctx, d->source_type, -1, SQLITE_TRANSIENT); break;
        case 2: if (d->filename) sqlite3_result_text(ctx, d->filename, -1, SQLITE_TRANSIENT);
                else sqlite3_result_null(ctx); break;
        case 3: sqlite3_result_text(ctx, d->checksum, -1, SQLITE_TRANSIENT); break;
        case 4: sqlite3_result_int(ctx, d->page_count); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Pages virtual table
 * ══════════════════════════════════════════════════════════════════════ */

DEFINE_VTAB(Pages,
    "CREATE TABLE x(page_id INTEGER, document_id INTEGER, page_number INTEGER, "
    "width REAL, height REAL, file_path TEXT HIDDEN)",
    PagesCursor, bboxes_page, bboxes_next_page, 5, 100.0)

static int PagesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* p = static_cast<PagesCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, p->page_id); break;
        case 1: sqlite3_result_int(ctx, p->document_id); break;
        case 2: sqlite3_result_int(ctx, p->page_number); break;
        case 3: sqlite3_result_double(ctx, p->width); break;
        case 4: sqlite3_result_double(ctx, p->height); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Fonts virtual table
 * ══════════════════════════════════════════════════════════════════════ */

DEFINE_VTAB(Fonts,
    "CREATE TABLE x(font_id INTEGER, name TEXT, file_path TEXT HIDDEN)",
    FontsCursor, bboxes_font, bboxes_next_font, 2, 100.0)

static int FontsColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* f = static_cast<FontsCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, f->font_id); break;
        case 1: sqlite3_result_text(ctx, f->name, -1, SQLITE_TRANSIENT); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Styles virtual table
 * ══════════════════════════════════════════════════════════════════════ */

DEFINE_VTAB(Styles,
    "CREATE TABLE x(style_id INTEGER, font_id INTEGER, font_size REAL, "
    "color TEXT, weight TEXT, italic INTEGER, underline INTEGER, "
    "file_path TEXT HIDDEN)",
    StylesCursor, bboxes_style, bboxes_next_style, 7, 100.0)

static int StylesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* s = static_cast<StylesCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, s->style_id); break;
        case 1: sqlite3_result_int(ctx, s->font_id); break;
        case 2: sqlite3_result_double(ctx, s->font_size); break;
        case 3: sqlite3_result_text(ctx, s->color, -1, SQLITE_TRANSIENT); break;
        case 4: sqlite3_result_text(ctx, s->weight, -1, SQLITE_TRANSIENT); break;
        case 5: sqlite3_result_int(ctx, s->italic); break;
        case 6: sqlite3_result_int(ctx, s->underline); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Bboxes virtual table
 * ══════════════════════════════════════════════════════════════════════ */

DEFINE_VTAB(Bboxes,
    "CREATE TABLE x(page_id INTEGER, style_id INTEGER, "
    "x REAL, y REAL, w REAL, h REAL, text TEXT, formula TEXT, file_path TEXT HIDDEN)",
    BboxesCursor, bboxes_bbox, bboxes_next_bbox, 8, 1000.0)

static int BboxesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* b = static_cast<BboxesCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, b->page_id); break;
        case 1: sqlite3_result_int(ctx, b->style_id); break;
        case 2: sqlite3_result_double(ctx, b->x); break;
        case 3: sqlite3_result_double(ctx, b->y); break;
        case 4: sqlite3_result_double(ctx, b->w); break;
        case 5: sqlite3_result_double(ctx, b->h); break;
        case 6: sqlite3_result_text(ctx, b->text, -1, SQLITE_TRANSIENT); break;
        case 7: if (b->formula) sqlite3_result_text(ctx, b->formula, -1, SQLITE_TRANSIENT);
                else sqlite3_result_null(ctx); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Generic scalar JSON functions (dispatch via sqlite3_user_data)
 * ══════════════════════════════════════════════════════════════════════ */

typedef const char* (*json_iter_fn)(bboxes_cursor*);

struct ScalarDesc {
    Format fmt;
    json_iter_fn iter_fn;
};

static void generic_doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto* desc = static_cast<ScalarDesc*>(sqlite3_user_data(ctx));
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { sqlite3_result_null(ctx); return; }

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = open_by_format(desc->fmt, buf.data(), buf.size());
    if (!cur) { sqlite3_result_null(ctx); return; }

    const char* json = bboxes_get_doc_json(cur);
    if (json)
        sqlite3_result_text(ctx, json, -1, SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);
    bboxes_close(cur);
}

static void generic_json_array_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto* desc = static_cast<ScalarDesc*>(sqlite3_user_data(ctx));
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { sqlite3_result_null(ctx); return; }

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = open_by_format(desc->fmt, buf.data(), buf.size());
    if (!cur) { sqlite3_result_null(ctx); return; }

    std::string result = "[";
    bool first = true;
    while (const char* json = desc->iter_fn(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    sqlite3_result_text(ctx, result.c_str(), result.size(), SQLITE_TRANSIENT);
}

/* ══════════════════════════════════════════════════════════════════════
 * Static scalar descriptors — one per (format × function) combination
 * ══════════════════════════════════════════════════════════════════════ */

/* 5 formats × 4 array iterators + 5 formats × 1 doc = 25 descriptors */
static ScalarDesc s_doc_desc[]    = { {FMT_PDF, nullptr}, {FMT_XLSX, nullptr}, {FMT_TEXT, nullptr}, {FMT_DOCX, nullptr}, {FMT_AUTO, nullptr} };
static ScalarDesc s_pages_desc[]  = { {FMT_PDF, bboxes_next_page_json},  {FMT_XLSX, bboxes_next_page_json},  {FMT_TEXT, bboxes_next_page_json},  {FMT_DOCX, bboxes_next_page_json},  {FMT_AUTO, bboxes_next_page_json} };
static ScalarDesc s_fonts_desc[]  = { {FMT_PDF, bboxes_next_font_json},  {FMT_XLSX, bboxes_next_font_json},  {FMT_TEXT, bboxes_next_font_json},  {FMT_DOCX, bboxes_next_font_json},  {FMT_AUTO, bboxes_next_font_json} };
static ScalarDesc s_styles_desc[] = { {FMT_PDF, bboxes_next_style_json}, {FMT_XLSX, bboxes_next_style_json}, {FMT_TEXT, bboxes_next_style_json}, {FMT_DOCX, bboxes_next_style_json}, {FMT_AUTO, bboxes_next_style_json} };
static ScalarDesc s_bboxes_desc[] = { {FMT_PDF, bboxes_next_bbox_json},  {FMT_XLSX, bboxes_next_bbox_json},  {FMT_TEXT, bboxes_next_bbox_json},  {FMT_DOCX, bboxes_next_bbox_json},  {FMT_AUTO, bboxes_next_bbox_json} };

/* ══════════════════════════════════════════════════════════════════════
 * Table-driven registration
 * ══════════════════════════════════════════════════════════════════════ */

struct FormatInfo {
    const char* prefix;
    Format      fmt;
    int         desc_idx;   /* index into s_*_desc arrays */
};

static const FormatInfo s_formats[] = {
    { "bboxes_pdf",  FMT_PDF,  0 },
    { "bboxes_xlsx", FMT_XLSX, 1 },
    { "bboxes_text", FMT_TEXT, 2 },
    { "bboxes_docx", FMT_DOCX, 3 },
    { "bboxes",      FMT_AUTO, 4 },   /* auto-detect last so it gets the unqualified names */
};

static int register_format(sqlite3* db, const FormatInfo& fi) {
    Format* pFmt = make_fmt(fi.fmt);
    int rc;
    std::string name;
    int di = fi.desc_idx;

    /* Virtual tables */
    name = std::string(fi.prefix) + "_doc";
    rc = sqlite3_create_module(db, name.c_str(), &DocModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(fi.prefix) + "_pages";
    rc = sqlite3_create_module(db, name.c_str(), &PagesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(fi.prefix) + "_fonts";
    rc = sqlite3_create_module(db, name.c_str(), &FontsModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(fi.prefix) + "_styles";
    rc = sqlite3_create_module(db, name.c_str(), &StylesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(fi.prefix);
    rc = sqlite3_create_module(db, name.c_str(), &BboxesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    /* Scalar JSON functions */
    struct { const char* suffix; void(*fn)(sqlite3_context*, int, sqlite3_value**); ScalarDesc* desc; } scalars[] = {
        { "_doc_json",    generic_doc_json_func,    &s_doc_desc[di] },
        { "_pages_json",  generic_json_array_func,  &s_pages_desc[di] },
        { "_fonts_json",  generic_json_array_func,  &s_fonts_desc[di] },
        { "_styles_json", generic_json_array_func,  &s_styles_desc[di] },
        { "_json",        generic_json_array_func,  &s_bboxes_desc[di] },
    };

    for (auto& s : scalars) {
        name = std::string(fi.prefix) + s.suffix;
        rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, s.desc, s.fn, nullptr, nullptr);
        if (rc != SQLITE_OK) return rc;
    }

    return SQLITE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Extension entry point
 * ══════════════════════════════════════════════════════════════════════ */

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_bboxes_init(sqlite3* db, char** pzErrMsg,
                        const sqlite3_api_routines* pApi) {
    SQLITE_EXTENSION_INIT2(pApi);
    bboxes_pdf_init();
    bboxes_xlsx_init();

    int rc;
    for (const auto& fi : s_formats) {
        rc = register_format(db, fi);
        if (rc != SQLITE_OK) return rc;
    }

    /* bboxes_info — auto-detecting doc info scalar */
    rc = sqlite3_create_function(db, "bboxes_info", 1, SQLITE_UTF8,
                                  &s_doc_desc[4], generic_doc_json_func, nullptr, nullptr);
    return rc;
}
}
