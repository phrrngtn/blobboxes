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

enum Format { FMT_PDF, FMT_XLSX, FMT_TEXT, FMT_DOCX };

static bboxes_cursor* open_by_format(Format fmt, const void* buf, size_t len) {
    switch (fmt) {
        case FMT_PDF:   return bboxes_open_pdf(buf, len, nullptr, 0, 0);
        case FMT_XLSX:  return bboxes_open_xlsx(buf, len, nullptr, 0, 0);
        case FMT_TEXT:  return bboxes_open_text(buf, len);
        case FMT_DOCX:  return bboxes_open_docx(buf, len);
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
 *
 * Each sqlite3_module instance stores a Format* in pAux.  The Connect,
 * BestIndex, Column callbacks are shared; only the Filter dispatches
 * to the right bboxes_open_* via open_by_format().
 * ══════════════════════════════════════════════════════════════════════ */

static Format* make_fmt(Format f) {
    /* Allocated once, freed when the module is unregistered via
       sqlite3_create_module_v2's xDestroy, or lives for the
       lifetime of the connection.  We use static storage per-format. */
    static Format fmts[4] = { FMT_PDF, FMT_XLSX, FMT_TEXT, FMT_DOCX };
    return &fmts[f];
}

static Format get_fmt(sqlite3_vtab* pVtab) {
    /* The pAux pointer was stashed by sqlite3_create_module; but SQLite
       does not expose it on the vtab.  We use a custom vtab struct. */
    (void)pVtab;
    return FMT_PDF; /* fallback -- not used; see FmtVtab */
}

/* ── Generic vtab struct that carries the Format ─────────────────── */

struct FmtVtab : sqlite3_vtab {
    Format fmt;
};

/* ══════════════════════════════════════════════════════════════════════
 * bboxes_{fmt}_doc  virtual table  (single row per file)
 * ══════════════════════════════════════════════════════════════════════ */

struct DocCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_doc* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int docConnect(sqlite3* db, void* pAux, int, const char* const*,
                      sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(document_id INTEGER, source_type TEXT, "
        "filename TEXT, page_count INTEGER, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    auto* v = new FmtVtab{};
    v->fmt = *static_cast<Format*>(pAux);
    *ppVtab = v;
    return SQLITE_OK;
}

static int docBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 4 &&
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&
            info->aConstraint[i].usable) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->estimatedCost = 10.0;
            return SQLITE_OK;
        }
    }
    return SQLITE_CONSTRAINT;
}

static int docDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK; }
static int docOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new DocCursor{}; return SQLITE_OK; }

static int docClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<DocCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int docFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<DocCursor*>(pCursor);
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());
    if (!c->cur) { c->eof = true; return SQLITE_OK; }
    c->current = bboxes_get_doc(c->cur);
    c->eof = (c->current == nullptr);
    c->rowid = 0;
    return SQLITE_OK;
}

static int docNext(sqlite3_vtab_cursor* pCursor) {
    static_cast<DocCursor*>(pCursor)->eof = true;
    return SQLITE_OK;
}

static int docEof(sqlite3_vtab_cursor* pCursor) { return static_cast<DocCursor*>(pCursor)->eof ? 1 : 0; }

static int docColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* d = static_cast<DocCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, d->document_id); break;
        case 1: sqlite3_result_text(ctx, d->source_type, -1, SQLITE_TRANSIENT); break;
        case 2: if (d->filename) sqlite3_result_text(ctx, d->filename, -1, SQLITE_TRANSIENT);
                else sqlite3_result_null(ctx); break;
        case 3: sqlite3_result_int(ctx, d->page_count); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

static int docRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<DocCursor*>(pCursor)->rowid; return SQLITE_OK;
}

static sqlite3_module docModule = {
    0, nullptr, docConnect, docBestIndex, docDisconnect,
    docDisconnect, docOpen, docClose, docFilter,
    docNext, docEof, docColumn, docRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ══════════════════════════════════════════════════════════════════════
 * bboxes_{fmt}_pages  virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct PagesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_page* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int pagesConnect(sqlite3* db, void* pAux, int, const char* const*,
                        sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(page_id INTEGER, document_id INTEGER, page_number INTEGER, "
        "width REAL, height REAL, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    auto* v = new FmtVtab{};
    v->fmt = *static_cast<Format*>(pAux);
    *ppVtab = v;
    return SQLITE_OK;
}

static int pagesBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 5 &&
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&
            info->aConstraint[i].usable) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->estimatedCost = 100.0;
            return SQLITE_OK;
        }
    }
    return SQLITE_CONSTRAINT;
}

static int pagesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK; }
static int pagesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new PagesCursor{}; return SQLITE_OK; }

static int pagesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<PagesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int pagesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<PagesCursor*>(pCursor);
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());
    if (!c->cur) { c->eof = true; return SQLITE_OK; }
    c->current = bboxes_next_page(c->cur);
    c->eof = (c->current == nullptr);
    c->rowid = 0;
    return SQLITE_OK;
}

static int pagesNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<PagesCursor*>(pCursor);
    c->rowid++;
    c->current = bboxes_next_page(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int pagesEof(sqlite3_vtab_cursor* pCursor) { return static_cast<PagesCursor*>(pCursor)->eof ? 1 : 0; }

static int pagesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
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

static int pagesRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<PagesCursor*>(pCursor)->rowid; return SQLITE_OK;
}

static sqlite3_module pagesModule = {
    0, nullptr, pagesConnect, pagesBestIndex, pagesDisconnect,
    pagesDisconnect, pagesOpen, pagesClose, pagesFilter,
    pagesNext, pagesEof, pagesColumn, pagesRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ══════════════════════════════════════════════════════════════════════
 * bboxes_{fmt}_fonts  virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct FontsCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_font* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int fontsConnect(sqlite3* db, void* pAux, int, const char* const*,
                        sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(font_id INTEGER, name TEXT, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    auto* v = new FmtVtab{};
    v->fmt = *static_cast<Format*>(pAux);
    *ppVtab = v;
    return SQLITE_OK;
}

static int fontsBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 2 &&
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&
            info->aConstraint[i].usable) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->estimatedCost = 100.0;
            return SQLITE_OK;
        }
    }
    return SQLITE_CONSTRAINT;
}

static int fontsDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK; }
static int fontsOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new FontsCursor{}; return SQLITE_OK; }

static int fontsClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int fontsFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());
    if (!c->cur) { c->eof = true; return SQLITE_OK; }
    c->current = bboxes_next_font(c->cur);
    c->eof = (c->current == nullptr);
    c->rowid = 0;
    return SQLITE_OK;
}

static int fontsNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    c->rowid++;
    c->current = bboxes_next_font(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int fontsEof(sqlite3_vtab_cursor* pCursor) { return static_cast<FontsCursor*>(pCursor)->eof ? 1 : 0; }

static int fontsColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* f = static_cast<FontsCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, f->font_id); break;
        case 1: sqlite3_result_text(ctx, f->name, -1, SQLITE_TRANSIENT); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

static int fontsRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<FontsCursor*>(pCursor)->rowid; return SQLITE_OK;
}

static sqlite3_module fontsModule = {
    0, nullptr, fontsConnect, fontsBestIndex, fontsDisconnect,
    fontsDisconnect, fontsOpen, fontsClose, fontsFilter,
    fontsNext, fontsEof, fontsColumn, fontsRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ══════════════════════════════════════════════════════════════════════
 * bboxes_{fmt}_styles  virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct StylesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_style* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int stylesConnect(sqlite3* db, void* pAux, int, const char* const*,
                         sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(style_id INTEGER, font_id INTEGER, font_size REAL, "
        "color TEXT, weight TEXT, italic INTEGER, underline INTEGER, "
        "file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    auto* v = new FmtVtab{};
    v->fmt = *static_cast<Format*>(pAux);
    *ppVtab = v;
    return SQLITE_OK;
}

static int stylesBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 7 &&
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&
            info->aConstraint[i].usable) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->estimatedCost = 100.0;
            return SQLITE_OK;
        }
    }
    return SQLITE_CONSTRAINT;
}

static int stylesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK; }
static int stylesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new StylesCursor{}; return SQLITE_OK; }

static int stylesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<StylesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int stylesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<StylesCursor*>(pCursor);
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());
    if (!c->cur) { c->eof = true; return SQLITE_OK; }
    c->current = bboxes_next_style(c->cur);
    c->eof = (c->current == nullptr);
    c->rowid = 0;
    return SQLITE_OK;
}

static int stylesNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<StylesCursor*>(pCursor);
    c->rowid++;
    c->current = bboxes_next_style(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int stylesEof(sqlite3_vtab_cursor* pCursor) { return static_cast<StylesCursor*>(pCursor)->eof ? 1 : 0; }

static int stylesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
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

static int stylesRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<StylesCursor*>(pCursor)->rowid; return SQLITE_OK;
}

static sqlite3_module stylesModule = {
    0, nullptr, stylesConnect, stylesBestIndex, stylesDisconnect,
    stylesDisconnect, stylesOpen, stylesClose, stylesFilter,
    stylesNext, stylesEof, stylesColumn, stylesRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ══════════════════════════════════════════════════════════════════════
 * bboxes_{fmt}  virtual table  (the bbox rows)
 * ══════════════════════════════════════════════════════════════════════ */

struct BboxesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_bbox* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int bboxesConnect(sqlite3* db, void* pAux, int, const char* const*,
                         sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(bbox_id INTEGER, page_id INTEGER, style_id INTEGER, "
        "x REAL, y REAL, w REAL, h REAL, text TEXT, formula TEXT, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    auto* v = new FmtVtab{};
    v->fmt = *static_cast<Format*>(pAux);
    *ppVtab = v;
    return SQLITE_OK;
}

static int bboxesBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 9 &&
            info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ &&
            info->aConstraint[i].usable) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->estimatedCost = 1000.0;
            return SQLITE_OK;
        }
    }
    return SQLITE_CONSTRAINT;
}

static int bboxesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FmtVtab*>(pVtab); return SQLITE_OK; }
static int bboxesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new BboxesCursor{}; return SQLITE_OK; }

static int bboxesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<BboxesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int bboxesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<BboxesCursor*>(pCursor);
    Format fmt = static_cast<FmtVtab*>(pCursor->pVtab)->fmt;
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = open_by_format(fmt, c->buf.data(), c->buf.size());
    if (!c->cur) { c->eof = true; return SQLITE_OK; }
    c->current = bboxes_next_bbox(c->cur);
    c->eof = (c->current == nullptr);
    c->rowid = 0;
    return SQLITE_OK;
}

static int bboxesNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<BboxesCursor*>(pCursor);
    c->rowid++;
    c->current = bboxes_next_bbox(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int bboxesEof(sqlite3_vtab_cursor* pCursor) { return static_cast<BboxesCursor*>(pCursor)->eof ? 1 : 0; }

static int bboxesColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* b = static_cast<BboxesCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, b->bbox_id); break;
        case 1: sqlite3_result_int(ctx, b->page_id); break;
        case 2: sqlite3_result_int(ctx, b->style_id); break;
        case 3: sqlite3_result_double(ctx, b->x); break;
        case 4: sqlite3_result_double(ctx, b->y); break;
        case 5: sqlite3_result_double(ctx, b->w); break;
        case 6: sqlite3_result_double(ctx, b->h); break;
        case 7: sqlite3_result_text(ctx, b->text, -1, SQLITE_TRANSIENT); break;
        case 8: if (b->formula) sqlite3_result_text(ctx, b->formula, -1, SQLITE_TRANSIENT);
                else sqlite3_result_null(ctx); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

static int bboxesRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<BboxesCursor*>(pCursor)->rowid; return SQLITE_OK;
}

static sqlite3_module bboxesModule = {
    0, nullptr, bboxesConnect, bboxesBestIndex, bboxesDisconnect,
    bboxesDisconnect, bboxesOpen, bboxesClose, bboxesFilter,
    bboxesNext, bboxesEof, bboxesColumn, bboxesRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ══════════════════════════════════════════════════════════════════════
 * Scalar JSON functions  (format-aware)
 * ══════════════════════════════════════════════════════════════════════ */

typedef const char* (*json_iter_fn)(bboxes_cursor*);

static void json_array_func_fmt(sqlite3_context* ctx, int argc, sqlite3_value** argv,
                                 json_iter_fn iter_fn, Format fmt) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { sqlite3_result_null(ctx); return; }

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = open_by_format(fmt, buf.data(), buf.size());
    if (!cur) { sqlite3_result_null(ctx); return; }

    std::string result = "[";
    bool first = true;
    while (const char* json = iter_fn(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    sqlite3_result_text(ctx, result.c_str(), result.size(), SQLITE_TRANSIENT);
}

static void doc_json_func_fmt(sqlite3_context* ctx, int argc, sqlite3_value** argv,
                               Format fmt) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { sqlite3_result_null(ctx); return; }

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = open_by_format(fmt, buf.data(), buf.size());
    if (!cur) { sqlite3_result_null(ctx); return; }

    const char* json = bboxes_get_doc_json(cur);
    if (json)
        sqlite3_result_text(ctx, json, -1, SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);
    bboxes_close(cur);
}

/* ── Per-format scalar wrappers (PDF) ─────────────────────────────── */

static void pdf_doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    doc_json_func_fmt(ctx, argc, argv, FMT_PDF);
}
static void pdf_pages_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_page_json, FMT_PDF);
}
static void pdf_fonts_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_font_json, FMT_PDF);
}
static void pdf_styles_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_style_json, FMT_PDF);
}
static void pdf_bboxes_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_bbox_json, FMT_PDF);
}

/* ── Per-format scalar wrappers (XLSX) ────────────────────────────── */

static void xlsx_doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    doc_json_func_fmt(ctx, argc, argv, FMT_XLSX);
}
static void xlsx_pages_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_page_json, FMT_XLSX);
}
static void xlsx_fonts_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_font_json, FMT_XLSX);
}
static void xlsx_styles_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_style_json, FMT_XLSX);
}
static void xlsx_bboxes_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_bbox_json, FMT_XLSX);
}

/* ── Per-format scalar wrappers (TEXT) ────────────────────────────── */

static void text_doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    doc_json_func_fmt(ctx, argc, argv, FMT_TEXT);
}
static void text_pages_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_page_json, FMT_TEXT);
}
static void text_fonts_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_font_json, FMT_TEXT);
}
static void text_styles_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_style_json, FMT_TEXT);
}
static void text_bboxes_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_bbox_json, FMT_TEXT);
}

/* ── Per-format scalar wrappers (DOCX) ────────────────────────────── */

static void docx_doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    doc_json_func_fmt(ctx, argc, argv, FMT_DOCX);
}
static void docx_pages_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_page_json, FMT_DOCX);
}
static void docx_fonts_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_font_json, FMT_DOCX);
}
static void docx_styles_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_style_json, FMT_DOCX);
}
static void docx_bboxes_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func_fmt(ctx, argc, argv, bboxes_next_bbox_json, FMT_DOCX);
}

/* ══════════════════════════════════════════════════════════════════════
 * Helper: register all 5 virtual tables + 5 JSON functions for a format
 * ══════════════════════════════════════════════════════════════════════ */

struct JsonFuncs {
    void (*doc_json)(sqlite3_context*, int, sqlite3_value**);
    void (*pages_json)(sqlite3_context*, int, sqlite3_value**);
    void (*fonts_json)(sqlite3_context*, int, sqlite3_value**);
    void (*styles_json)(sqlite3_context*, int, sqlite3_value**);
    void (*bboxes_json)(sqlite3_context*, int, sqlite3_value**);
};

static int register_format(sqlite3* db, const char* prefix, Format fmt,
                            const JsonFuncs& jf) {
    Format* pFmt = make_fmt(fmt);
    int rc;
    std::string name;

    /* Virtual tables */
    name = std::string(prefix) + "_doc";
    rc = sqlite3_create_module(db, name.c_str(), &docModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_pages";
    rc = sqlite3_create_module(db, name.c_str(), &pagesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_fonts";
    rc = sqlite3_create_module(db, name.c_str(), &fontsModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_styles";
    rc = sqlite3_create_module(db, name.c_str(), &stylesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    /* The bare bboxes table for this format */
    name = std::string(prefix);
    rc = sqlite3_create_module(db, name.c_str(), &bboxesModule, pFmt);
    if (rc != SQLITE_OK) return rc;

    /* Scalar JSON functions */
    name = std::string(prefix) + "_doc_json";
    rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, nullptr, jf.doc_json, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_pages_json";
    rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, nullptr, jf.pages_json, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_fonts_json";
    rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, nullptr, jf.fonts_json, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_styles_json";
    rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, nullptr, jf.styles_json, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;

    name = std::string(prefix) + "_json";
    rc = sqlite3_create_function(db, name.c_str(), -1, SQLITE_UTF8, nullptr, jf.bboxes_json, nullptr, nullptr);
    return rc;
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

    /* ── PDF (bboxes_pdf_doc, bboxes_pdf_pages, ... bboxes_pdf) ──── */
    rc = register_format(db, "bboxes_pdf", FMT_PDF, {
        pdf_doc_json_func, pdf_pages_json_func, pdf_fonts_json_func,
        pdf_styles_json_func, pdf_bboxes_json_func
    });
    if (rc != SQLITE_OK) return rc;

    /* ── XLSX (bboxes_xlsx_doc, bboxes_xlsx_pages, ... bboxes_xlsx) ─ */
    rc = register_format(db, "bboxes_xlsx", FMT_XLSX, {
        xlsx_doc_json_func, xlsx_pages_json_func, xlsx_fonts_json_func,
        xlsx_styles_json_func, xlsx_bboxes_json_func
    });
    if (rc != SQLITE_OK) return rc;

    /* ── TEXT (bboxes_text_doc, bboxes_text_pages, ... bboxes_text) ─ */
    rc = register_format(db, "bboxes_text", FMT_TEXT, {
        text_doc_json_func, text_pages_json_func, text_fonts_json_func,
        text_styles_json_func, text_bboxes_json_func
    });
    if (rc != SQLITE_OK) return rc;

    /* ── DOCX (bboxes_docx_doc, bboxes_docx_pages, ... bboxes_docx) ─ */
    rc = register_format(db, "bboxes_docx", FMT_DOCX, {
        docx_doc_json_func, docx_pages_json_func, docx_fonts_json_func,
        docx_styles_json_func, docx_bboxes_json_func
    });
    if (rc != SQLITE_OK) return rc;

    /* ── Legacy aliases (bboxes_doc, bboxes_pages, ... bboxes) ───── */
    /* These keep backward compatibility -- they use the PDF backend. */
    Format* pPdf = make_fmt(FMT_PDF);
    rc = sqlite3_create_module(db, "bboxes_doc",    &docModule,    pPdf);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_pages",  &pagesModule,  pPdf);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_fonts",  &fontsModule,  pPdf);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_styles", &stylesModule, pPdf);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes",        &bboxesModule, pPdf);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "bboxes_doc_json",    -1, SQLITE_UTF8, nullptr, pdf_doc_json_func,    nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_pages_json",  -1, SQLITE_UTF8, nullptr, pdf_pages_json_func,  nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_fonts_json",  -1, SQLITE_UTF8, nullptr, pdf_fonts_json_func,  nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_styles_json", -1, SQLITE_UTF8, nullptr, pdf_styles_json_func, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_json",        -1, SQLITE_UTF8, nullptr, pdf_bboxes_json_func, nullptr, nullptr);
    return rc;
}
}
