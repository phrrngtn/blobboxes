#include "bboxes.h"
#include "bboxes_types.h"

#include <xlnt/xlnt.hpp>
#include <sstream>
#include <string>

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

                    if (!ws.has_cell(xlnt::cell_reference(col, row))) continue;
                    auto cell = ws.cell(xlnt::cell_reference(col, row));
                    if (!cell.has_value()) continue;

                    /* font / style */
                    std::string font_name = "default";
                    double font_size = 11.0;
                    std::string color = "rgba(0,0,0,255)";
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
                                        color = "rgba(" + std::to_string(r) + "," +
                                                std::to_string(g) + "," +
                                                std::to_string(b) + "," +
                                                std::to_string(a) + ")";
                                    }
                                }
                            }
                        }
                    }

                    uint32_t font_id = result.fonts.intern(font_name.c_str());
                    uint32_t style_id = result.styles.intern(
                        font_id, font_size, color, weight, italic, underline);

                    BBox bb;
                    bb.page_id  = page.page_id;
                    bb.style_id = style_id;
                    bb.x = static_cast<double>(col.index);
                    bb.y = static_cast<double>(row);
                    bb.w = 1.0;
                    bb.h = 1.0;
                    bb.text = cell.to_string();
                    if (cell.has_formula())
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
