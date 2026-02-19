#include "bboxes_types.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <string>
#include <vector>
#include <cstring>

/* ── helpers ─────────────────────────────────────────────────────── */

/* extract a file from a zip archive in memory */
static std::vector<char> zip_extract(const void* buf, size_t len, const char* name) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, buf, len, 0))
        return {};

    int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return {};
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, idx, &stat)) {
        mz_zip_reader_end(&zip);
        return {};
    }

    std::vector<char> out(static_cast<size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, idx, out.data(), out.size(), 0))
        out.clear();

    mz_zip_reader_end(&zip);
    return out;
}

/* get text content from a <w:r> run or <w:t> element tree */
static std::string get_run_text(const pugi::xml_node& run) {
    std::string text;
    for (auto& child : run.children()) {
        if (strcmp(child.name(), "w:t") == 0) {
            text += child.child_value();
        }
    }
    return text;
}

/* get concatenated text from a paragraph */
static std::string get_para_text(const pugi::xml_node& para) {
    std::string text;
    for (auto& child : para.children()) {
        if (strcmp(child.name(), "w:r") == 0) {
            text += get_run_text(child);
        }
    }
    return text;
}

/* get text from a table cell (may contain multiple paragraphs) */
static std::string get_cell_text(const pugi::xml_node& tc) {
    std::string text;
    bool first = true;
    for (auto& child : tc.children()) {
        if (strcmp(child.name(), "w:p") == 0) {
            if (!first) text += '\n';
            text += get_para_text(child);
            first = false;
        }
    }
    return text;
}

/* get integer attribute from <w:gridSpan w:val="N"> etc. */
static int get_span(const pugi::xml_node& tcPr, const char* elem, int def) {
    auto node = tcPr.child(elem);
    if (!node) return def;
    int v = node.attribute("w:val").as_int(def);
    return v > 0 ? v : def;
}

/* ── extract ─────────────────────────────────────────────────────── */

BBoxResult extract_docx(const void* buf, size_t len) {
    BBoxResult result;
    result.source_type = "docx";

    /* extract word/document.xml from the zip */
    auto xml_data = zip_extract(buf, len, "word/document.xml");
    if (xml_data.empty()) {
        result.page_count = -1;
        return result;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result pr = doc.load_buffer(xml_data.data(), xml_data.size());
    if (!pr) {
        result.page_count = -1;
        return result;
    }

    /* default font + style */
    uint32_t font_id = result.fonts.intern("default");
    uint32_t style_id = result.styles.intern(
        font_id, 12.0, "rgba(0,0,0,255)", "normal", false, false);

    /* find all <w:tbl> elements in the document body */
    auto body = doc.child("w:document").child("w:body");
    if (!body) {
        result.page_count = -1;
        return result;
    }

    std::vector<pugi::xml_node> tables;
    for (auto& child : body.children()) {
        if (strcmp(child.name(), "w:tbl") == 0)
            tables.push_back(child);
    }

    result.page_count = static_cast<int>(tables.size());

    for (size_t ti = 0; ti < tables.size(); ti++) {
        Page page;
        page.page_id     = static_cast<uint32_t>(ti);
        page.document_id = 0;
        page.page_number = static_cast<int>(ti + 1);

        uint32_t row_num = 0;
        uint32_t max_cols = 0;

        for (auto& tr : tables[ti].children()) {
            if (strcmp(tr.name(), "w:tr") != 0) continue;
            row_num++;
            uint32_t col_num = 0;

            for (auto& tc : tr.children()) {
                if (strcmp(tc.name(), "w:tc") != 0) continue;
                col_num++;

                /* check for gridSpan (colspan equivalent) */
                auto tcPr = tc.child("w:tcPr");
                int colspan = get_span(tcPr, "w:gridSpan", 1);

                /* vMerge: skip cells that continue a vertical merge */
                auto vMerge = tcPr.child("w:vMerge");
                if (vMerge && !vMerge.attribute("w:val")) {
                    col_num += (colspan - 1);
                    continue;  /* continuation cell, skip */
                }

                std::string text = get_cell_text(tc);

                BBox bb;
                bb.page_id  = page.page_id;
                bb.style_id = style_id;
                bb.x = static_cast<double>(col_num);
                bb.y = static_cast<double>(row_num);
                bb.w = static_cast<double>(colspan);
                bb.h = 1.0;
                bb.text = text;
                page.bboxes.push_back(std::move(bb));

                col_num += (colspan - 1);
            }
            if (col_num > max_cols) max_cols = col_num;
        }

        page.width  = static_cast<double>(max_cols);
        page.height = static_cast<double>(row_num);
        result.pages.push_back(std::move(page));
    }

    return result;
}
