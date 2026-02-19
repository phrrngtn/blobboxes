#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "pdf_bboxes.h"
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

/* ── pdf_extract virtual table ───────────────────────────────────── */

struct ExtractVtab : sqlite3_vtab {
};

struct ExtractCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    pdf_bboxes_cursor* cur = nullptr;
    const pdf_bboxes_run* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int extractConnect(sqlite3* db, void*, int, const char* const*,
                          sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(font_id INTEGER, page INTEGER, x REAL, y REAL, "
        "w REAL, h REAL, text TEXT, color TEXT, font_size REAL, style TEXT, "
        "file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new ExtractVtab{};
    return SQLITE_OK;
}

static int extractDisconnect(sqlite3_vtab* pVtab) {
    delete static_cast<ExtractVtab*>(pVtab);
    return SQLITE_OK;
}

static int extractBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 10 &&
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

static int extractOpen(sqlite3_vtab*, sqlite3_vtab_cursor** ppCursor) {
    *ppCursor = new ExtractCursor{};
    return SQLITE_OK;
}

static int extractClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<ExtractCursor*>(pCursor);
    if (c->cur) pdf_bboxes_extract_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int extractFilter(sqlite3_vtab_cursor* pCursor, int, const char*,
                         int argc, sqlite3_value** argv) {
    auto* c = static_cast<ExtractCursor*>(pCursor);
    if (c->cur) { pdf_bboxes_extract_close(c->cur); c->cur = nullptr; }

    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }

    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }

    c->cur = pdf_bboxes_extract_open(c->buf.data(), c->buf.size(), nullptr, 0, 0);
    if (!c->cur) { c->eof = true; return SQLITE_OK; }

    c->rowid = 0;
    c->current = pdf_bboxes_extract_next(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int extractNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<ExtractCursor*>(pCursor);
    c->rowid++;
    c->current = pdf_bboxes_extract_next(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int extractEof(sqlite3_vtab_cursor* pCursor) {
    return static_cast<ExtractCursor*>(pCursor)->eof ? 1 : 0;
}

static int extractColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* r = static_cast<ExtractCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, r->font_id); break;
        case 1: sqlite3_result_int(ctx, r->page); break;
        case 2: sqlite3_result_double(ctx, r->x); break;
        case 3: sqlite3_result_double(ctx, r->y); break;
        case 4: sqlite3_result_double(ctx, r->w); break;
        case 5: sqlite3_result_double(ctx, r->h); break;
        case 6: sqlite3_result_text(ctx, r->text, -1, SQLITE_TRANSIENT); break;
        case 7: sqlite3_result_text(ctx, r->color, -1, SQLITE_TRANSIENT); break;
        case 8: sqlite3_result_double(ctx, r->font_size); break;
        case 9: sqlite3_result_text(ctx, r->style, -1, SQLITE_TRANSIENT); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

static int extractRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<ExtractCursor*>(pCursor)->rowid;
    return SQLITE_OK;
}

static sqlite3_module extractModule = {
    0, nullptr, extractConnect, extractBestIndex, extractDisconnect,
    extractDisconnect, extractOpen, extractClose, extractFilter,
    extractNext, extractEof, extractColumn, extractRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ── pdf_fonts virtual table ─────────────────────────────────────── */

struct FontsVtab : sqlite3_vtab {
};

struct FontsCursor : sqlite3_vtab_cursor {
    std::vector<char> buf;
    pdf_bboxes_font_cursor* cur = nullptr;
    const pdf_bboxes_font* current = nullptr;
    bool eof = true;
    int64_t rowid = 0;
};

static int fontsConnect(sqlite3* db, void*, int, const char* const*,
                        sqlite3_vtab** ppVtab, char**) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(font_id INTEGER, name TEXT, flags INTEGER, style TEXT, "
        "file_path TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    *ppVtab = new FontsVtab{};
    return SQLITE_OK;
}

static int fontsBestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
    for (int i = 0; i < info->nConstraint; i++) {
        if (info->aConstraint[i].iColumn == 4 &&
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

static int fontsDisconnect(sqlite3_vtab* pVtab) {
    delete static_cast<FontsVtab*>(pVtab);
    return SQLITE_OK;
}

static int fontsOpen(sqlite3_vtab*, sqlite3_vtab_cursor** ppCursor) {
    *ppCursor = new FontsCursor{};
    return SQLITE_OK;
}

static int fontsClose(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    if (c->cur) pdf_bboxes_fonts_close(c->cur);
    delete c;
    return SQLITE_OK;
}

static int fontsFilter(sqlite3_vtab_cursor* pCursor, int, const char*,
                       int argc, sqlite3_value** argv) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    if (c->cur) { pdf_bboxes_fonts_close(c->cur); c->cur = nullptr; }

    if (argc < 1) { c->eof = true; return SQLITE_OK; }
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    if (!path) { c->eof = true; return SQLITE_OK; }

    c->buf = read_file(path);
    if (c->buf.empty()) { c->eof = true; return SQLITE_OK; }

    c->cur = pdf_bboxes_fonts_open(c->buf.data(), c->buf.size(), nullptr);
    if (!c->cur) { c->eof = true; return SQLITE_OK; }

    c->rowid = 0;
    c->current = pdf_bboxes_fonts_next(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int fontsNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    c->rowid++;
    c->current = pdf_bboxes_fonts_next(c->cur);
    c->eof = (c->current == nullptr);
    return SQLITE_OK;
}

static int fontsEof(sqlite3_vtab_cursor* pCursor) {
    return static_cast<FontsCursor*>(pCursor)->eof ? 1 : 0;
}

static int fontsColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* f = static_cast<FontsCursor*>(pCursor)->current;
    switch (col) {
        case 0: sqlite3_result_int(ctx, f->font_id); break;
        case 1: sqlite3_result_text(ctx, f->name, -1, SQLITE_TRANSIENT); break;
        case 2: sqlite3_result_int(ctx, f->flags); break;
        case 3: sqlite3_result_text(ctx, f->style, -1, SQLITE_TRANSIENT); break;
        default: sqlite3_result_null(ctx); break;
    }
    return SQLITE_OK;
}

static int fontsRowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    *pRowid = static_cast<FontsCursor*>(pCursor)->rowid;
    return SQLITE_OK;
}

static sqlite3_module fontsModule = {
    0, nullptr, fontsConnect, fontsBestIndex, fontsDisconnect,
    fontsDisconnect, fontsOpen, fontsClose, fontsFilter,
    fontsNext, fontsEof, fontsColumn, fontsRowid,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

/* ── scalar JSON functions ───────────────────────────────────────── */

static void extract_json_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    int sp = (argc > 1) ? sqlite3_value_int(argv[1]) : 0;
    int ep = (argc > 2) ? sqlite3_value_int(argv[2]) : 0;

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = pdf_bboxes_extract_open(buf.data(), buf.size(), nullptr, sp, ep);
    if (!cur) { sqlite3_result_null(ctx); return; }

    std::string result = "[";
    bool first = true;
    while (const char* json = pdf_bboxes_extract_next_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    pdf_bboxes_extract_close(cur);
    result += ']';
    sqlite3_result_text(ctx, result.c_str(), result.size(), SQLITE_TRANSIENT);
}

static void fonts_json_func(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));

    auto buf = read_file(path);
    if (buf.empty()) { sqlite3_result_null(ctx); return; }

    auto* cur = pdf_bboxes_fonts_open(buf.data(), buf.size(), nullptr);
    if (!cur) { sqlite3_result_null(ctx); return; }

    std::string result = "[";
    bool first = true;
    while (const char* json = pdf_bboxes_fonts_next_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    pdf_bboxes_fonts_close(cur);
    result += ']';
    sqlite3_result_text(ctx, result.c_str(), result.size(), SQLITE_TRANSIENT);
}

/* ── extension entry point ───────────────────────────────────────── */

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_pdfbboxes_init(sqlite3* db, char** pzErrMsg,
                           const sqlite3_api_routines* pApi) {
    SQLITE_EXTENSION_INIT2(pApi);
    pdf_bboxes_init();

    int rc = sqlite3_create_module(db, "pdf_extract", &extractModule, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_module(db, "pdf_fonts", &fontsModule, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "pdf_extract_json", -1, SQLITE_UTF8, nullptr,
                                 extract_json_func, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function(db, "pdf_fonts_json", 1, SQLITE_UTF8, nullptr,
                                 fonts_json_func, nullptr, nullptr);
    return rc;
}
}
