#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "bboxes.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
 * bboxes_doc virtual table (single row per file)
 * ══════════════════════════════════════════════════════════════════════ */

struct DocVtab : sqlite3_vtab {};

struct DocCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_doc* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int docConnect(sqlite3* db, void*, int, const char* const*,
                      sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(document_id INTEGER, source_type TEXT, "
        "filename TEXT, page_count INTEGER, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new DocVtab{};
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

static int docDisconnect(sqlite3_vtab* pVtab) { delete static_cast<DocVtab*>(pVtab); return SQLITE_OK; }
static int docOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new DocCursor{}; return SQLITE_OK; }

static int docClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<DocCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int docFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<DocCursor*>(pCursor);
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = bboxes_open_pdf(c->buf.data(), c->buf.size(), nullptr, 0, 0);
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
 * bboxes_pages virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct PagesVtab : sqlite3_vtab {};

struct PagesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_page* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int pagesConnect(sqlite3* db, void*, int, const char* const*,
                        sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(page_id INTEGER, document_id INTEGER, page_number INTEGER, "
        "width REAL, height REAL, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new PagesVtab{};
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

static int pagesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<PagesVtab*>(pVtab); return SQLITE_OK; }
static int pagesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new PagesCursor{}; return SQLITE_OK; }

static int pagesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<PagesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int pagesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<PagesCursor*>(pCursor);
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = bboxes_open_pdf(c->buf.data(), c->buf.size(), nullptr, 0, 0);
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
 * bboxes_fonts virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct FontsVtab : sqlite3_vtab {};

struct FontsCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_font* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int fontsConnect(sqlite3* db, void*, int, const char* const*,
                        sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(font_id INTEGER, name TEXT, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new FontsVtab{};
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

static int fontsDisconnect(sqlite3_vtab* pVtab) { delete static_cast<FontsVtab*>(pVtab); return SQLITE_OK; }
static int fontsOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new FontsCursor{}; return SQLITE_OK; }

static int fontsClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int fontsFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = bboxes_open_pdf(c->buf.data(), c->buf.size(), nullptr, 0, 0);
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
 * bboxes_styles virtual table
 * ══════════════════════════════════════════════════════════════════════ */

struct StylesVtab : sqlite3_vtab {};

struct StylesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_style* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int stylesConnect(sqlite3* db, void*, int, const char* const*,
                         sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(style_id INTEGER, font_id INTEGER, font_size REAL, "
        "color TEXT, weight TEXT, italic INTEGER, underline INTEGER, "
        "file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new StylesVtab{};
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

static int stylesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<StylesVtab*>(pVtab); return SQLITE_OK; }
static int stylesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new StylesCursor{}; return SQLITE_OK; }

static int stylesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<StylesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int stylesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<StylesCursor*>(pCursor);
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = bboxes_open_pdf(c->buf.data(), c->buf.size(), nullptr, 0, 0);
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
 * bboxes virtual table (the bbox rows)
 * ══════════════════════════════════════════════════════════════════════ */

struct BboxesVtab : sqlite3_vtab {};

struct BboxesCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    bboxes_cursor* cur = nullptr;
    const bboxes_bbox* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int bboxesConnect(sqlite3* db, void*, int, const char* const*,
                         sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(bbox_id INTEGER, page_id INTEGER, style_id INTEGER, "
        "x REAL, y REAL, w REAL, h REAL, text TEXT, file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new BboxesVtab{};
    return SQLITE_OK;
}

static int bboxesBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 8 &&
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

static int bboxesDisconnect(sqlite3_vtab* pVtab) { delete static_cast<BboxesVtab*>(pVtab); return SQLITE_OK; }
static int bboxesOpen(sqlite3_vtab*, sqlite3_vtab_cursor** pp) { *pp = new BboxesCursor{}; return SQLITE_OK; }

static int bboxesClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<BboxesCursor*>(pCursor);
    if (c->cur) bboxes_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int bboxesFilter(sqlite3_vtab_cursor* pCursor, int, const char*, int argc, sqlite3_value** argv) {
    auto* c = static_cast<BboxesCursor*>(pCursor);
    if (c->cur) { bboxes_close(c->cur); c->cur = nullptr; }
    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }
    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }
    c->cur = bboxes_open_pdf(c->buf.data(), c->buf.size(), nullptr, 0, 0);
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
 * Scalar JSON functions
 * ══════════════════════════════════════════════════════════════════════ */

typedef const char* (*json_iter_fn)(bboxes_cursor*);

static void json_array_func(sqlite3_context* ctx, int argc, sqlite3_value** argv,
                             json_iter_fn iter_fn) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    int sp = (argc > 1) ? sqlite3_value_int(argv[1]) : 0;
    int ep = (argc > 2) ? sqlite3_value_int(argv[2]) : 0;

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = bboxes_open_pdf(buf.data(), buf.size(), nullptr, sp, ep);
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

static void doc_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    int sp = (argc > 1) ? sqlite3_value_int(argv[1]) : 0;
    int ep = (argc > 2) ? sqlite3_value_int(argv[2]) : 0;

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = bboxes_open_pdf(buf.data(), buf.size(), nullptr, sp, ep);
    if (!cur) { sqlite3_result_null(ctx); return; }

    const char* json = bboxes_get_doc_json(cur);
    if (json)
        sqlite3_result_text(ctx, json, -1, SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);
    bboxes_close(cur);
}

static void pages_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func(ctx, argc, argv, bboxes_next_page_json);
}
static void fonts_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func(ctx, argc, argv, bboxes_next_font_json);
}
static void styles_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func(ctx, argc, argv, bboxes_next_style_json);
}
static void bboxes_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    json_array_func(ctx, argc, argv, bboxes_next_bbox_json);
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

    int rc;
    rc = sqlite3_create_module(db, "bboxes_doc",    &docModule,    nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_pages",  &pagesModule,  nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_fonts",  &fontsModule,  nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes_styles", &stylesModule, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "bboxes",        &bboxesModule, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "bboxes_doc_json",    -1, SQLITE_UTF8, nullptr, doc_json_func,    nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_pages_json",  -1, SQLITE_UTF8, nullptr, pages_json_func,  nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_fonts_json",  -1, SQLITE_UTF8, nullptr, fonts_json_func,  nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_styles_json", -1, SQLITE_UTF8, nullptr, styles_json_func, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "bboxes_json",        -1, SQLITE_UTF8, nullptr, bboxes_json_func, nullptr, nullptr);
    return rc;
}
}
