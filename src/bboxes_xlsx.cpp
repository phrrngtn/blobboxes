#include "bboxes.h"
#include "bboxes_types.h"

#include <xlnt/xlnt.hpp>
#include <sstream>
#include <string>

/* ── shared/array-formula regions ─────────────────────────────────────────
   xlnt exposes no shared-formula API (only per-cell cell.formula(), which it
   expands — incompletely). To keep drag-fill / array formulas FIRST-CLASS and
   UNEXPANDED, read the group `ref` extents straight from the sheet XML with
   pugixml (already a blobboxes dependency, used by bboxes_meta.cpp). Each group
   becomes ONE box at its master, w/h spanning the ref; children are skipped.
   Reuses the existing w/h fields — no schema change (same shape as merges). */
#include <miniz.h>
#include <pugixml.hpp>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Region { double w = 1.0, h = 1.0; std::string formula; };
struct SheetRegions {
    std::unordered_map<uint64_t, Region> masters;  // master cell -> extent + formula
    std::unordered_set<uint64_t> covered;          // drag-fill/array children to skip
};

inline uint64_t cell_key(uint32_t row, uint32_t col) { return (uint64_t(row) << 24) | col; }

const char* local_name(const char* n) {
    const char* c = std::strchr(n, ':');
    return c ? c + 1 : n;
}

void all_by(const pugi::xml_node& n, const char* name, std::vector<pugi::xml_node>& out) {
    for (auto c : n.children()) {
        if (c.type() == pugi::node_element && std::strcmp(local_name(c.name()), name) == 0)
            out.push_back(c);
        all_by(c, name, out);
    }
}

bool zip_read(mz_zip_archive& z, const char* name, std::string& out) {
    int idx = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    if (idx < 0) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
    if (!p) return false;
    out.assign(static_cast<const char*>(p), sz);
    mz_free(p);
    return true;
}

bool parse_a1(const char* s, uint32_t& row, uint32_t& col) {
    uint32_t c = 0;
    while (*s == '$') s++;
    while (*s && std::isalpha((unsigned char)*s)) {
        c = c * 26 + (std::toupper((unsigned char)*s) - 'A' + 1);
        s++;
    }
    while (*s == '$') s++;
    if (c == 0 || !std::isdigit((unsigned char)*s)) return false;
    uint32_t r = 0;
    while (*s && std::isdigit((unsigned char)*s)) { r = r * 10 + (*s - '0'); s++; }
    if (r == 0) return false;
    row = r; col = c; return true;
}

bool parse_ref(const std::string& ref, uint32_t& r1, uint32_t& c1, uint32_t& r2, uint32_t& c2) {
    auto pos = ref.find(':');
    if (pos == std::string::npos) {
        if (!parse_a1(ref.c_str(), r1, c1)) return false;
        r2 = r1; c2 = c1; return true;
    }
    return parse_a1(ref.substr(0, pos).c_str(), r1, c1) &&
           parse_a1(ref.substr(pos + 1).c_str(), r2, c2);
}

void parse_sheet_regions(const std::string& xml, SheetRegions& sr) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return;
    std::vector<pugi::xml_node> cells;
    all_by(doc, "c", cells);
    for (auto& c : cells) {
        pugi::xml_node f;
        for (auto ch : c.children())
            if (ch.type() == pugi::node_element && std::strcmp(local_name(ch.name()), "f") == 0) {
                f = ch;
                break;
            }
        if (!f) continue;
        uint32_t cr, cc;
        if (!parse_a1(c.attribute("r").value(), cr, cc)) continue;
        const char* t = f.attribute("t").value();
        const char* refv = f.attribute("ref").value();
        uint64_t key = cell_key(cr, cc);
        if (std::strcmp(t, "shared") == 0) {
            if (refv && *refv) {  // master carries ref + formula text
                uint32_t r1, c1, r2, c2;
                if (parse_ref(refv, r1, c1, r2, c2)) {
                    Region rg;
                    rg.w = double(c2 - c1 + 1);
                    rg.h = double(r2 - r1 + 1);
                    rg.formula = std::string("=") + f.text().get();
                    sr.masters[key] = std::move(rg);
                }
            } else {  // child: empty <f t="shared" si=.../>
                sr.covered.insert(key);
            }
        } else if (std::strcmp(t, "array") == 0 && refv && *refv) {
            uint32_t r1, c1, r2, c2;
            if (parse_ref(refv, r1, c1, r2, c2)) {
                Region rg;
                rg.w = double(c2 - c1 + 1);
                rg.h = double(r2 - r1 + 1);
                rg.formula = std::string("=") + f.text().get();
                sr.masters[key] = std::move(rg);
                for (uint32_t rr = r1; rr <= r2; rr++)  // array children have no <f>: geometric fill
                    for (uint32_t cx = c1; cx <= c2; cx++)
                        if (!(rr == cr && cx == cc)) sr.covered.insert(cell_key(rr, cx));
            }
        }
    }
}

std::unordered_map<std::string, SheetRegions> build_sheet_regions(const void* buf, size_t len) {
    std::unordered_map<std::string, SheetRegions> out;
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, buf, len, 0)) return out;
    std::string wbxml, relsxml;
    if (zip_read(z, "xl/workbook.xml", wbxml) &&
        zip_read(z, "xl/_rels/workbook.xml.rels", relsxml)) {
        pugi::xml_document wbdoc, reldoc;
        wbdoc.load_buffer(wbxml.data(), wbxml.size());
        reldoc.load_buffer(relsxml.data(), relsxml.size());
        std::vector<pugi::xml_node> sheets, rels;
        all_by(wbdoc, "sheet", sheets);       // workbook order matches xlnt sheet index
        all_by(reldoc, "Relationship", rels);
        std::unordered_map<std::string, std::string> rid2t;
        for (auto& r : rels) rid2t[r.attribute("Id").value()] = r.attribute("Target").value();
        for (auto& s : sheets) {
            auto it = rid2t.find(s.attribute("r:id").value());
            if (it == rid2t.end()) continue;
            const std::string& t = it->second;
            std::string part = (!t.empty() && t[0] == '/') ? t.substr(1) : ("xl/" + t);
            std::string sxml;
            if (zip_read(z, part.c_str(), sxml)) {
                SheetRegions sr;
                parse_sheet_regions(sxml, sr);
                out[s.attribute("name").value()] = std::move(sr);  // key by sheet name
            }
        }
    }
    mz_zip_reader_end(&z);
    return out;
}

}  // namespace

/* ── lifecycle (no-ops for xlnt) ─────────────────────────────────── */

void bboxes_xlsx_init(void) {}
void bboxes_xlsx_destroy(void) {}

/* ── extract ─────────────────────────────────────────────────────── */

BBoxResult extract_xlsx(const void* buf, size_t len, const char* password,
                         int start_page, int end_page) {
    BBoxResult result;
    result.source_type = "xlsx";

    try {
        xlnt::workbook wb;
        std::string data(static_cast<const char*>(buf), len);
        std::istringstream stream(data);
        wb.load(stream);

        /* shared/array-formula group extents, read straight from the XML */
        auto regions = build_sheet_regions(buf, len);

        int sheet_count = static_cast<int>(wb.sheet_count());
        result.page_count = sheet_count;

        /* resolve page range (1-based inclusive, 0,0 = all) */
        int sp = (start_page > 0) ? start_page : 1;
        int ep = (end_page > 0) ? end_page : sheet_count;
        if (sp > sheet_count) sp = sheet_count;
        if (ep > sheet_count) ep = sheet_count;
        if (sp > ep) { result.page_count = -1; return result; }

        for (int si = sp - 1; si < ep; si++) {
            auto ws = wb.sheet_by_index(si);

            const SheetRegions* sr = nullptr;
            if (auto it = regions.find(ws.title()); it != regions.end()) sr = &it->second;

            auto highest_row = ws.highest_row();
            auto highest_col = ws.highest_column();

            Page page;
            page.page_id     = static_cast<uint32_t>(si);
            page.document_id = 0;
            page.page_number = si + 1;
            page.width       = static_cast<double>(highest_col.index);
            page.height      = static_cast<double>(highest_row);

            /* collect merged cell ranges to skip covered cells */
            auto merged = ws.merged_ranges();

            for (auto row = 1u; row <= highest_row; row++) {
                for (auto col = xlnt::column_t(1); col <= highest_col; col++) {
                    /* skip cells covered by a merge (not the top-left origin) */
                    bool is_covered = false;
                    for (const auto& mr : merged) {
                        if (mr.contains(xlnt::cell_reference(col, row))) {
                            auto tl = mr.top_left();
                            if (tl.column() != col || tl.row() != row) {
                                is_covered = true;
                                break;
                            }
                        }
                    }
                    if (is_covered) continue;

                    /* skip drag-fill/array children — the group is emitted once at its master */
                    uint64_t key = cell_key(row, col.index);
                    if (sr && sr->covered.count(key)) continue;

                    if (!ws.has_cell(xlnt::cell_reference(col, row))) continue;
                    auto cell = ws.cell(xlnt::cell_reference(col, row));
                    bool is_master = sr && sr->masters.count(key);
                    /* Emit truly-empty cells only when they carry nothing: a formula IS
                       content even with no cached value (fullCalcOnLoad / exporters that
                       omit <v>, e.g. openpyxl), so keep it — the emit path below reads
                       cell.formula() directly. Masters emit even if uncalculated. */
                    if (!cell.has_value() && !cell.has_formula() && !is_master) continue;

                    /* font / style */
                    std::string font_name = "default";
                    double font_size = 11.0;
                    std::string color = BBOXES_DEFAULT_COLOR;
                    std::string weight = "normal";
                    bool italic = false;
                    bool underline = false;

                    if (cell.has_format()) {
                        auto fmt = cell.format();
                        if (fmt.font_applied()) {
                            auto f = fmt.font();
                            font_name = f.name();
                            font_size = f.size();
                            if (f.bold()) weight = "bold";
                            italic = f.italic();
                            if (f.underlined()) underline = true;
                            if (f.has_color()) {
                                auto c = f.color();
                                if (c.type() == xlnt::color_type::rgb) {
                                    auto rgb = c.rgb();
                                    font_name = f.name();
                                    /* xlnt rgb is ARGB hex string */
                                    auto hex = rgb.hex_string();
                                    /* parse AARRGGBB */
                                    if (hex.size() == 8) {
                                        unsigned a = std::stoul(hex.substr(0, 2), nullptr, 16);
                                        unsigned r = std::stoul(hex.substr(2, 2), nullptr, 16);
                                        unsigned g = std::stoul(hex.substr(4, 2), nullptr, 16);
                                        unsigned b = std::stoul(hex.substr(6, 2), nullptr, 16);
                                        color = color_string(r, g, b, a);
                                    }
                                }
                            }
                        }
                    }

                    uint32_t font_id = result.fonts.intern(font_name.c_str());
                    uint32_t style_id = result.styles.intern(
                        font_id, font_size, color, weight, italic, underline);

                    /* For merged cells, set w/h to the span of the merge range
                       so that downstream spatial clustering sees the true extent.
                       NB: this seems right for table detection but is unproven —
                       may need revisiting if merged headers confuse grid alignment. */
                    double cell_w = 1.0;
                    double cell_h = 1.0;
                    for (const auto& mr : merged) {
                        auto tl = mr.top_left();
                        if (tl.column() == col && tl.row() == row) {
                            auto br = mr.bottom_right();
                            cell_w = static_cast<double>(br.column().index - col.index + 1);
                            cell_h = static_cast<double>(br.row() - row + 1);
                            break;
                        }
                    }

                    /* a drag-fill/array master spans its whole group (unexpanded region) */
                    if (is_master) {
                        const Region& rg = sr->masters.at(key);
                        cell_w = rg.w;
                        cell_h = rg.h;
                    }

                    BBox bb;
                    bb.page_id  = page.page_id;
                    bb.style_id = style_id;
                    bb.x = static_cast<double>(col.index);
                    bb.y = static_cast<double>(row);
                    bb.w = cell_w;
                    bb.h = cell_h;
                    bb.text = cell.to_string();  /* master's displayed value represents the region */
                    if (is_master)
                        bb.formula = sr->masters.at(key).formula;  /* unexpanded master formula (from XML) */
                    else if (cell.has_formula())
                        bb.formula = "=" + cell.formula();
                    page.bboxes.push_back(std::move(bb));
                }
            }

            result.pages.push_back(std::move(page));
        }
    } catch (...) {
        result.page_count = -1;
    }

    return result;
}

/* ── fast path: single byte-scan pass (hybrid). Emits the same BBox grain as
   extract_xlsx ~7-9x faster (bench_reader): our scanner walks the O(cells) grid;
   shared/array-formula masters + merges are detected inline (low-N gnarly bits).
   Differences vs extract_xlsx (by design): text is the RAW <v> value (not xlnt's
   number-format display), and style_id is the cellXfs `s` index (not an interned id). */

static std::string x_attr(const char* p, const char* tag_end, const char* name) {
    size_t nl = std::strlen(name);
    const char* a = static_cast<const char*>(memmem(p, tag_end - p, name, nl));
    if (!a) return {};
    a += nl;
    const char* q = static_cast<const char*>(std::memchr(a, '"', tag_end - a));
    return q ? std::string(a, q - a) : std::string();
}

static std::string x_inner(const char* p, const char* end, const char* open, size_t ol,
                           const char* close, size_t cl) {
    const char* t = static_cast<const char*>(memmem(p, end - p, open, ol));
    if (!t) return {};
    const char* gt = static_cast<const char*>(std::memchr(t, '>', end - t));
    if (!gt || *(gt - 1) == '/') return {};
    const char* c = static_cast<const char*>(memmem(gt, end - gt, close, cl));
    return c ? std::string(gt + 1, c - (gt + 1)) : std::string();
}

static void xml_unescape(std::string& s) {
    if (s.find('&') == std::string::npos) return;   // fast path: nothing to decode
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') { out += s[i++]; continue; }
        if      (s.compare(i, 5, "&amp;")  == 0) { out += '&';  i += 5; }
        else if (s.compare(i, 4, "&lt;")   == 0) { out += '<';  i += 4; }
        else if (s.compare(i, 4, "&gt;")   == 0) { out += '>';  i += 4; }
        else if (s.compare(i, 6, "&quot;") == 0) { out += '"';  i += 6; }
        else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; }
        else if (s.compare(i, 2, "&#")     == 0) {
            size_t semi = s.find(';', i);
            if (semi == std::string::npos) { out += s[i++]; continue; }
            long code = (s[i + 2] == 'x' || s[i + 2] == 'X')
                        ? std::strtol(s.c_str() + i + 3, nullptr, 16)
                        : std::strtol(s.c_str() + i + 2, nullptr, 10);
            if (code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6));
                                     out += static_cast<char>(0x80 | (code & 0x3F)); }
            else { out += static_cast<char>(0xE0 | (code >> 12));
                   out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                   out += static_cast<char>(0x80 | (code & 0x3F)); }
            i = semi + 1;
        }
        else out += s[i++];
    }
    s = std::move(out);
}

BBoxResult extract_xlsx_fast(const void* buf, size_t len, const char* /*password*/,
                             int start_page, int end_page) {
    BBoxResult result;
    result.source_type = "xlsx";
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, buf, len, 0)) { result.page_count = -1; return result; }

    try {
        // ordered sheets [(part, name)] from workbook.xml + rels (workbook order = page_id)
        std::string wbxml, relsxml;
        std::vector<std::pair<std::string, std::string>> sheets;
        if (zip_read(z, "xl/workbook.xml", wbxml) &&
            zip_read(z, "xl/_rels/workbook.xml.rels", relsxml)) {
            pugi::xml_document wbdoc, reldoc;
            wbdoc.load_buffer(wbxml.data(), wbxml.size());
            reldoc.load_buffer(relsxml.data(), relsxml.size());
            std::vector<pugi::xml_node> ss, rels;
            all_by(wbdoc, "sheet", ss);
            all_by(reldoc, "Relationship", rels);
            std::unordered_map<std::string, std::string> rid2t;
            for (auto& r : rels) rid2t[r.attribute("Id").value()] = r.attribute("Target").value();
            for (auto& s : ss) {
                auto it = rid2t.find(s.attribute("r:id").value());
                if (it == rid2t.end()) continue;
                const std::string& t = it->second;
                std::string part = (!t.empty() && t[0] == '/') ? t.substr(1) : ("xl/" + t);
                sheets.emplace_back(part, s.attribute("name").value());
            }
        }

        // shared strings, once
        std::string sstxml;
        std::vector<std::string> sst;
        if (zip_read(z, "xl/sharedStrings.xml", sstxml)) {
            const char* p = sstxml.data(); const char* end = p + sstxml.size();
            while ((p = static_cast<const char*>(memmem(p, end - p, "<si>", 4)))) {
                p += 4;
                const char* se = static_cast<const char*>(memmem(p, end - p, "</si>", 5));
                if (!se) break;
                std::string val; const char* q = p;
                while (q < se) {
                    const char* t = static_cast<const char*>(memmem(q, se - q, "<t", 2));
                    if (!t) break;
                    const char* gt = static_cast<const char*>(std::memchr(t, '>', se - t));
                    if (!gt) break;
                    if (*(gt - 1) == '/') { q = gt + 1; continue; }
                    const char* tc = static_cast<const char*>(memmem(gt, se - gt, "</t>", 4));
                    if (!tc) break;
                    val.append(gt + 1, tc - (gt + 1)); q = tc + 4;
                }
                sst.push_back(std::move(val)); p = se + 5;
            }
        }

        int sheet_count = static_cast<int>(sheets.size());
        result.page_count = sheet_count;
        int sp = (start_page > 0) ? start_page : 1;
        int ep = (end_page > 0) ? end_page : sheet_count;
        if (sp > sheet_count) sp = sheet_count;
        if (ep > sheet_count) ep = sheet_count;
        if (sp > ep) { result.page_count = -1; mz_zip_reader_end(&z); return result; }

        for (int si = sp - 1; si < ep; si++) {
            std::string xml;
            if (!zip_read(z, sheets[si].first.c_str(), xml)) continue;
            const char* base = xml.data(); const char* end = base + xml.size();

            Page page;
            page.page_id = static_cast<uint32_t>(si);
            page.document_id = 0;
            page.page_number = si + 1;

            std::unordered_set<uint64_t> covered;
            std::unordered_map<uint64_t, std::pair<double, double>> extent;  // top-left -> (w,h)

            // merges (small block, usually after sheetData) -> extents + covered non-origins
            const char* m = base;
            while ((m = static_cast<const char*>(memmem(m, end - m, "<mergeCell ", 11)))) {
                const char* me = static_cast<const char*>(std::memchr(m, '>', end - m));
                if (!me) break;
                uint32_t r1, c1, r2, c2;
                if (parse_ref(x_attr(m, me, " ref=\"").c_str(), r1, c1, r2, c2)) {
                    extent[cell_key(r1, c1)] = { double(c2 - c1 + 1), double(r2 - r1 + 1) };
                    page.merges.push_back({int(r1), int(c1), int(r2), int(c2)});  // side-channel
                    for (uint32_t rr = r1; rr <= r2; rr++)
                        for (uint32_t cc = c1; cc <= c2; cc++)
                            if (!(rr == r1 && cc == c1)) covered.insert(cell_key(rr, cc));
                }
                m = me + 1;
            }

            double pw = 0, ph = 0;
            const char* p = base;
            while ((p = static_cast<const char*>(memmem(p, end - p, "<c ", 3)))) {
                const char* tag_end = static_cast<const char*>(std::memchr(p, '>', end - p));
                if (!tag_end) break;
                bool self_closing = (*(tag_end - 1) == '/');
                uint32_t row = 0, col = 0;
                parse_a1(x_attr(p, tag_end, " r=\"").c_str(), row, col);
                if (col > pw) pw = col;
                if (row > ph) ph = row;

                const char* next; const char* cell_end;
                if (self_closing) { next = tag_end + 1; cell_end = tag_end; }
                else {
                    const char* ce = static_cast<const char*>(memmem(tag_end, end - tag_end, "</c>", 4));
                    if (!ce) break;
                    cell_end = ce; next = ce + 4;
                }
                uint64_t key = cell_key(row, col);
                if (self_closing || covered.count(key)) { p = next; continue; }

                std::string sref = x_attr(p, tag_end, " s=\"");
                std::string tref = x_attr(p, tag_end, " t=\"");

                // formula + shared/array master extent
                std::string formula;
                double w = 1, h = 1;
                const char* f = static_cast<const char*>(memmem(tag_end, cell_end - tag_end, "<f", 2));
                if (f) {
                    const char* fgt = static_cast<const char*>(std::memchr(f, '>', cell_end - f));
                    if (fgt && *(fgt - 1) != '/') {
                        std::string ft = x_attr(f, fgt, " t=\"");
                        std::string fref = x_attr(f, fgt, " ref=\"");
                        const char* fc = static_cast<const char*>(memmem(fgt, cell_end - fgt, "</f>", 4));
                        if (fc && fc > fgt + 1) formula = "=" + std::string(fgt + 1, fc - (fgt + 1));
                        if ((ft == "shared" || ft == "array") && !fref.empty()) {
                            uint32_t r1, c1, r2, c2;
                            if (parse_ref(fref.c_str(), r1, c1, r2, c2)) {
                                w = double(c2 - c1 + 1); h = double(r2 - r1 + 1);
                                for (uint32_t rr = r1; rr <= r2; rr++)
                                    for (uint32_t cc = c1; cc <= c2; cc++)
                                        if (!(rr == row && cc == col)) covered.insert(cell_key(rr, cc));
                            }
                        }
                    }
                }
                if (auto e = extent.find(key); e != extent.end()) { w = e->second.first; h = e->second.second; }

                // value-element PRESENCE (a cell with an empty <v> still emits, like xlnt)
                const char* vpos = (tref == "inlineStr")
                    ? static_cast<const char*>(memmem(tag_end, cell_end - tag_end, "<is", 3))
                    : static_cast<const char*>(memmem(tag_end, cell_end - tag_end, "<v", 2));
                std::string v = (tref == "inlineStr") ? x_inner(tag_end, cell_end, "<t", 2, "</t>", 4)
                                                       : x_inner(tag_end, cell_end, "<v", 2, "</v>", 4);
                std::string text;
                if (tref == "s") { long i = std::strtol(v.c_str(), nullptr, 10);
                                   if (i >= 0 && static_cast<size_t>(i) < sst.size()) text = sst[i]; }
                else text = std::move(v);

                if (!vpos && formula.empty()) { p = next; continue; }   // truly-empty cell
                xml_unescape(text);
                xml_unescape(formula);

                BBox bb;
                bb.page_id = static_cast<uint32_t>(si);
                bb.style_id = sref.empty() ? 0 : static_cast<uint32_t>(std::strtoul(sref.c_str(), nullptr, 10));
                bb.x = col; bb.y = row; bb.w = w; bb.h = h;
                bb.text = std::move(text);
                bb.formula = std::move(formula);
                page.bboxes.push_back(std::move(bb));
                p = next;
            }
            page.width = pw; page.height = ph;
            result.pages.push_back(std::move(page));
        }
    } catch (...) {
        result.page_count = -1;
    }
    mz_zip_reader_end(&z);
    return result;
}
