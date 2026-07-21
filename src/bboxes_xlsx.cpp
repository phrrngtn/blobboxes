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
