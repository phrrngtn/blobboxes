#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "pdf_bboxes.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using json = nlohmann::json;

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
    std::string current_json;
    json current_obj;
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
        if (info->aConstraint[i].iColumn == 10 && // file_path
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
    const char* s = pdf_bboxes_extract_next(c->cur);
    if (!s) { c->eof = true; return SQLITE_OK; }
    c->current_json = s;
    c->current_obj = json::parse(c->current_json, nullptr, false);
    c->eof = false;
    return SQLITE_OK;
}

static int extractNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<ExtractCursor*>(pCursor);
    c->rowid++;
    const char* s = pdf_bboxes_extract_next(c->cur);
    if (!s) { c->eof = true; return SQLITE_OK; }
    c->current_json = s;
    c->current_obj = json::parse(c->current_json, nullptr, false);
    return SQLITE_OK;
}

static int extractEof(sqlite3_vtab_cursor* pCursor) {
    return static_cast<ExtractCursor*>(pCursor)->eof ? 1 : 0;
}

static int extractColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* c = static_cast<ExtractCursor*>(pCursor);
    auto& o = c->current_obj;
    switch (col) {
        case 0: sqlite3_result_int(ctx, o["font_id"].get<int>()); break;
        case 1: sqlite3_result_int(ctx, o["page"].get<int>()); break;
        case 2: sqlite3_result_double(ctx, o["x"].get<double>()); break;
        case 3: sqlite3_result_double(ctx, o["y"].get<double>()); break;
        case 4: sqlite3_result_double(ctx, o["w"].get<double>()); break;
        case 5: sqlite3_result_double(ctx, o["h"].get<double>()); break;
        case 6: { auto s = o["text"].get<std::string>();
                  sqlite3_result_text(ctx, s.c_str(), s.size(), SQLITE_TRANSIENT); break; }
        case 7: { auto s = o["color"].get<std::string>();
                  sqlite3_result_text(ctx, s.c_str(), s.size(), SQLITE_TRANSIENT); break; }
        case 8: sqlite3_result_double(ctx, o["font_size"].get<double>()); break;
        case 9: { auto s = o["style"].get<std::string>();
                  sqlite3_result_text(ctx, s.c_str(), s.size(), SQLITE_TRANSIENT); break; }
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
    std::string current_json;
    json current_obj;
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
        if (info->aConstraint[i].iColumn == 4 && // file_path
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
    const char* s = pdf_bboxes_fonts_next(c->cur);
    if (!s) { c->eof = true; return SQLITE_OK; }
    c->current_json = s;
    c->current_obj = json::parse(c->current_json, nullptr, false);
    c->eof = false;
    return SQLITE_OK;
}

static int fontsNext(sqlite3_vtab_cursor* pCursor) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    c->rowid++;
    const char* s = pdf_bboxes_fonts_next(c->cur);
    if (!s) { c->eof = true; return SQLITE_OK; }
    c->current_json = s;
    c->current_obj = json::parse(c->current_json, nullptr, false);
    return SQLITE_OK;
}

static int fontsEof(sqlite3_vtab_cursor* pCursor) {
    return static_cast<FontsCursor*>(pCursor)->eof ? 1 : 0;
}

static int fontsColumn(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    auto* c = static_cast<FontsCursor*>(pCursor);
    auto& o = c->current_obj;
    switch (col) {
        case 0: sqlite3_result_int(ctx, o["font_id"].get<int>()); break;
        case 1: { auto s = o["name"].get<std::string>();
                  sqlite3_result_text(ctx, s.c_str(), s.size(), SQLITE_TRANSIENT); break; }
        case 2: sqlite3_result_int(ctx, o["flags"].get<int>()); break;
        case 3: { auto s = o["style"].get<std::string>();
                  sqlite3_result_text(ctx, s.c_str(), s.size(), SQLITE_TRANSIENT); break; }
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
    return rc;
}
}
