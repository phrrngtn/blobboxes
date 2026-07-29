/* Legacy .xls (BIFF5/BIFF8, OLE2/CFB) backend via libxls.
 *
 * Parallel to bboxes_xlsx.cpp (xlnt / OOXML). libxls does the OLE2/CFB + BIFF parsing blobboxes keeps
 * out of its own tree. Surfaced like the xlsx path:
 *   - bb() cells (extract_xls): values, types, merges, and per-cell STYLES (font name/size/weight/italic/
 *     underline/COLOR) decoded from the BIFF XF table.
 *   - xls_metadata()      -> bboxes_xls_metadata_json  (doc props via xls_summaryInfo + sheet names)
 *   - xls_style_decode()  -> bboxes_xls_style_decode_json  (per-XF font/numfmt/fill/border, mirrors xlsx)
 * Fidelity note: no formula source (BIFF exposes evaluated results) and no named ranges (libxls has no
 * accessor); colour uses the BIFF default 56-colour palette.
 */
#include "bboxes.h"
#include "bboxes_types.h"

#include <xls.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>

using namespace xls;   // libxls wraps its C API in `namespace xls` (see xls.h)

namespace {

/* BIFF8 default 56-colour palette, colour indices 8..63 (icv). */
static const uint32_t kPalette[56] = {
    0x000000,0xFFFFFF,0xFF0000,0x00FF00,0x0000FF,0xFFFF00,0xFF00FF,0x00FFFF,
    0x800000,0x008000,0x000080,0x808000,0x800080,0x008080,0xC0C0C0,0x808080,
    0x9999FF,0x993366,0xFFFFCC,0xCCFFFF,0x660066,0xFF8080,0x0066CC,0xCCCCFF,
    0x000080,0xFF00FF,0xFFFF00,0x00FFFF,0x800080,0x800000,0x008080,0x0000FF,
    0x00CCFF,0xCCFFFF,0xCCFFCC,0xFFFF99,0x99CCFF,0xFF99CC,0xCC99FF,0xFFCC99,
    0x3366FF,0x33CCCC,0x99CC00,0xFFCC00,0xFF9900,0xFF6600,0x666699,0x969696,
    0x003366,0x339966,0x003300,0x333300,0x993300,0x993366,0x333399,0x333333 };

std::string xls_color(uint16_t idx) {
    uint32_t rgb = (idx >= 8 && idx <= 63) ? kPalette[idx - 8]
                 : (idx == 0x41)           ? 0xFFFFFF          /* default background */
                 :                            0x000000;         /* auto / default fg / 0x7FFF / 0x40 / 0 */
    char b[40];
    std::snprintf(b, sizeof b, "rgba(%u,%u,%u,255)", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return b;
}

struct FontDec {
    std::string name = "default", weight = BBOXES_DEFAULT_WEIGHT, color = BBOXES_DEFAULT_COLOR;
    double size = BBOXES_DEFAULT_FONT_SIZE;
    bool italic = false, underline = false;
};

FontDec decode_font(xlsWorkBook* wb, uint16_t xfidx) {
    FontDec d;
    if (wb->xfs.xf && xfidx < wb->xfs.count) {
        uint16_t f = wb->xfs.xf[xfidx].font;
        uint32_t fpos = (f > 4) ? static_cast<uint32_t>(f) - 1 : f;   /* BIFF skips font index 4 */
        if (wb->fonts.font && fpos < wb->fonts.count) {
            const auto& fo = wb->fonts.font[fpos];
            if (fo.name && fo.name[0]) d.name = reinterpret_cast<const char*>(fo.name);
            if (fo.height) d.size = fo.height / 20.0;                 /* twips -> points */
            if (fo.bold >= 700 || (fo.flag & 0x0001)) d.weight = "bold";
            d.italic    = (fo.flag & 0x0002) != 0;
            d.underline = (fo.underline != 0);
            if (fo.color) d.color = xls_color(fo.color);
        }
    }
    return d;
}

std::string slurp(const char* path) {
    std::FILE* f = std::fopen(path, "rb"); if (!f) return {};
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::string s(sz > 0 ? static_cast<size_t>(sz) : 0, '\0');
    if (sz > 0 && std::fread(&s[0], 1, s.size(), f) != s.size()) s.clear();
    std::fclose(f); return s;
}

}  // namespace

/* ── bb() cells ──────────────────────────────────────────────────────── */

BBoxResult extract_xls(const void* buf, size_t len, const char* /*password*/,
                       int start_page, int end_page) {
    BBoxResult res;
    res.source_type = "xls";
    res.page_count  = 0;

    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(static_cast<const unsigned char*>(buf), len, "UTF-8", &err);
    if (!wb) { res.page_count = -1; return res; }   /* wrap_result() turns <0 into a NULL cursor */

    std::unordered_map<uint16_t, uint32_t> xf_to_style;   /* XF index -> bbox style_id, decoded once */
    auto style_for = [&](uint16_t xfidx) -> uint32_t {
        auto it = xf_to_style.find(xfidx);
        if (it != xf_to_style.end()) return it->second;
        FontDec d = decode_font(wb, xfidx);
        uint32_t fid = res.fonts.intern(d.name.c_str());
        uint32_t sid = res.styles.intern(fid, d.size, d.color, d.weight, d.italic, d.underline);
        xf_to_style[xfidx] = sid;
        return sid;
    };

    const int nsheets = static_cast<int>(wb->sheets.count);
    for (int s = 0; s < nsheets; s++) {
        if (start_page > 0 && (s + 1) < start_page) continue;
        if (end_page   > 0 && (s + 1) > end_page)   continue;
        xlsWorkSheet* ws = xls_getWorkSheet(wb, s);
        if (!ws || xls_parseWorkSheet(ws) != LIBXLS_OK) continue;

        Page page;
        page.page_id     = static_cast<uint32_t>(s);
        page.document_id = 0;
        page.page_number = s + 1;
        page.width       = static_cast<double>(ws->rows.lastcol + 1);
        page.height      = static_cast<double>(ws->rows.lastrow + 1);

        for (WORD r = 0; r <= ws->rows.lastrow; r++) {
            for (WORD c = 0; c <= ws->rows.lastcol; c++) {
                xlsCell* cell = xls_cell(ws, r, c);
                if (!cell || cell->isHidden) continue;
                const bool has_str = cell->str && cell->str[0] != '\0';
                const WORD id = cell->id;
                const bool numeric = (id == XLS_RECORD_NUMBER || id == XLS_RECORD_RK || id == XLS_RECORD_MULRK);
                const bool boolerr = (id == XLS_RECORD_BOOLERR);
                const bool formula = (id == XLS_RECORD_FORMULA);
                if (!has_str && !numeric && !boolerr && !formula) continue;

                BBox b;
                b.page_id  = page.page_id;
                b.style_id = style_for(cell->xf);
                b.x = static_cast<double>(cell->col + 1);   /* 1-based col, matching the xlsx reader */
                b.y = static_cast<double>(cell->row + 1);   /* 1-based row */
                b.w = static_cast<double>(cell->colspan ? cell->colspan : 1);
                b.h = static_cast<double>(cell->rowspan ? cell->rowspan : 1);
                b.text = has_str ? std::string(cell->str) : std::string();

                if (numeric) {
                    b.cell_type = BBOX_NUMBER; b.vnum = cell->d;
                    if (b.text.empty()) { char t[32]; std::snprintf(t, sizeof t, "%.15g", cell->d); b.text = t; }
                } else if (boolerr) {
                    if (has_str && cell->str[0] == '#') { b.cell_type = BBOX_ERROR; }
                    else { b.cell_type = BBOX_BOOL; b.vbool = (cell->d != 0.0);
                           if (b.text.empty()) b.text = b.vbool ? "TRUE" : "FALSE"; }
                } else if (formula) {
                    if (has_str) { b.cell_type = BBOX_STRING; }
                    else { b.cell_type = BBOX_NUMBER; b.vnum = cell->d;
                           char t[32]; std::snprintf(t, sizeof t, "%.15g", cell->d); b.text = t; }
                } else {
                    b.cell_type = BBOX_STRING;   /* LABEL / LABELSST */
                }

                if (cell->colspan > 1 || cell->rowspan > 1) {
                    page.merges.push_back({ cell->row + 1, cell->col + 1,
                        cell->row + (cell->rowspan ? cell->rowspan : 1),
                        cell->col + (cell->colspan ? cell->colspan : 1) - 1 });
                }
                page.bboxes.push_back(std::move(b));
            }
        }
        res.pages.push_back(std::move(page));
        res.page_count++;
    }
    xls_close(wb);
    return res;
}

/* ── xls_metadata() — document properties + sheet names (mirrors xlsx_metadata) ──────────────────────── */

const char* bboxes_xls_metadata_json(const void* buf, size_t len) {
    using json = nlohmann::json;
    static thread_local std::string out;
    json o; o["dialect"] = "xls";
    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(static_cast<const unsigned char*>(buf), len, "UTF-8", &err);
    if (!wb) { o["integrity"] = {{"status", "failed"}, {"error", "not an .xls / unparseable"}};
               out = o.dump(-1, ' ', false, json::error_handler_t::replace); return out.c_str(); }
    o["integrity"] = {{"status", "clean"}};
    if (xlsSummaryInfo* si = xls_summaryInfo(wb)) {
        auto S = [&](const char* k, BYTE* v) { o[k] = (v && v[0]) ? json(reinterpret_cast<const char*>(v)) : json(nullptr); };
        S("title", si->title);   S("subject", si->subject);   S("author", si->author);
        S("keywords", si->keywords); S("comments", si->comment); S("last_author", si->lastAuthor);
        S("app_name", si->appName); S("category", si->category); S("manager", si->manager);
        S("company", si->company);
        xls_close_summaryInfo(si);
    }
    o["sheet_count"] = static_cast<int>(wb->sheets.count);
    json sheets = json::array();
    for (DWORD i = 0; i < wb->sheets.count; i++) {
        const char* nm = wb->sheets.sheet ? reinterpret_cast<const char*>(wb->sheets.sheet[i].name) : nullptr;
        sheets.push_back({{"index", (int)i}, {"name", nm ? json(nm) : json(nullptr)},
                          {"hidden", wb->sheets.sheet ? (wb->sheets.sheet[i].visibility != 0) : false}});
    }
    o["sheets"] = std::move(sheets);
    xls_close(wb);
    out = o.dump(-1, ' ', false, json::error_handler_t::replace);
    return out.c_str();
}
const char* bboxes_xls_metadata_json_file(const char* path) {
    std::string s = slurp(path);
    return bboxes_xls_metadata_json(s.data(), s.size());
}

/* ── xls_style_decode() — per-XF font/numfmt/fill/border (mirrors xlsx_style_decode) ─────────────────── */

const char* bboxes_xls_style_decode_json(const void* buf, size_t len) {
    using json = nlohmann::json;
    static thread_local std::string out;
    json o; o["dialect"] = "xls";
    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(static_cast<const unsigned char*>(buf), len, "UTF-8", &err);
    if (!wb) { o["style_decode"] = json::array();
               out = o.dump(-1, ' ', false, json::error_handler_t::replace); return out.c_str(); }
    json dec = json::array();
    for (DWORD i = 0; i < wb->xfs.count; i++) {
        const auto& xf = wb->xfs.xf[i];
        FontDec d = decode_font(wb, static_cast<uint16_t>(i));
        json numfmt = {{"id", xf.format}, {"code", nullptr}};   /* custom formats live in wb->formats */
        for (DWORD k = 0; k < wb->formats.count; k++)
            if (wb->formats.format[k].index == xf.format && wb->formats.format[k].value) {
                numfmt["code"] = reinterpret_cast<const char*>(wb->formats.format[k].value); break; }
        json rs = {
            {"id", (int)i},
            {"font", {{"name", d.name}, {"size", d.size}, {"weight", d.weight},
                      {"italic", d.italic}, {"underline", d.underline}, {"color", d.color}}},
            {"numfmt", numfmt},
            {"fill", {{"fg", xls_color(xf.groundcolor & 0x7F)}, {"bg", xls_color((xf.groundcolor >> 7) & 0x7F)}}},
            {"border", {{"left",   (int)(xf.linestyle & 0xF)},        {"right",  (int)((xf.linestyle >> 4) & 0xF)},
                        {"top",    (int)((xf.linestyle >> 8) & 0xF)}, {"bottom", (int)((xf.linestyle >> 12) & 0xF)}}}
        };
        dec.push_back(std::move(rs));
    }
    o["style_decode"] = std::move(dec);
    xls_close(wb);
    out = o.dump(-1, ' ', false, json::error_handler_t::replace);
    return out.c_str();
}
const char* bboxes_xls_style_decode_json_file(const char* path) {
    std::string s = slurp(path);
    return bboxes_xls_style_decode_json(s.data(), s.size());
}
